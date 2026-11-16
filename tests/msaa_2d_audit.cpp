// Regression audit for issue #71: MSAA must cover the 3D scene and never the 2D HUD.
// Renders a synthetic stereo frame at 1x and 8x: the 3D triangle's edge must change,
// the 2D triangle's edge must stay byte-identical.
#include "offscreen_car_audit.h"
#include "gt2view/vk_scene_renderer.h"
#include "gt2formats/png_reader.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

using namespace gt2view;

namespace {
std::array<float, 16> Identity() {
    std::array<float, 16> m{};
    m[0] = m[5] = m[10] = m[15] = 1;
    return m;
}

void AddVertex(std::vector<SceneVertex>& v, float x, float y, float z, const float rgb[3]) {
    SceneVertex s{};
    s.pos[0] = x; s.pos[1] = y; s.pos[2] = z;
    for (int c = 0; c < 3; ++c) s.color[c] = rgb[c];
    v.push_back(s);
}

struct Frame {
    std::vector<uint8_t> eye; // the left eye, RGBA8
    size_t width = 0, height = 0;
    uint8_t At(size_t x, size_t y, int c) const { return eye[(y * width + x) * 4 + size_t(c)]; }
};

Frame Render(VkSceneRenderer& renderer, const std::vector<DrawItem>& items, size_t sceneItems,
             const VkExtent2D extent, uint32_t samples) {
    RenderOptions options;
    options.msaa = samples;
    renderer.SetOptions(options);
    renderer.CreateStereoTarget(extent, VK_FORMAT_R8G8B8A8_UNORM, true);
    const auto both = renderer.CaptureStereoForAudit(items, sceneItems);
    Frame f;
    f.width = extent.width; f.height = extent.height;
    f.eye.assign(both.begin(), both.begin() + std::ptrdiff_t(size_t(extent.width) * extent.height * 4));
    return f;
}

Frame RenderSingleTarget(VkSceneRenderer& renderer, const std::vector<DrawItem>& items, size_t sceneItems,
                         const VkExtent2D extent, uint32_t samples, const std::filesystem::path& path) {
    RenderOptions options;
    options.msaa = samples;
    renderer.SetOptions(options);
    renderer.Draw(items, path.string(), sceneItems);
    renderer.WaitFrame();
    const auto png = gt2::ReadPngFile(path.string());
    if (png.width != int(extent.width) || png.height != int(extent.height))
        throw std::runtime_error("single-target capture has an unexpected size");
    Frame f;
    f.width = size_t(png.width);
    f.height = size_t(png.height);
    f.eye = png.rgba;
    return f;
}

size_t CountDifferences(const Frame& a, const Frame& b, int x0, int y0, int x1, int y1) {
    size_t n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x)
            for (int c = 0; c < 3; ++c)
                n += a.At(size_t(x), size_t(y), c) != b.At(size_t(x), size_t(y), c) ? 1 : 0;
    return n;
}
} // namespace

int main() {
    std::unique_ptr<Device> device;
    try {
        device = std::make_unique<Device>();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "msaa_2d_audit skipped: %s\n", e.what());
        return 0; // no Vulkan 1.3 device on this machine
    }
    try {
        VkContext context(device->existing);
        const VkExtent2D extent{320, 240};
        VkSceneRenderer renderer(context, extent, VK_FORMAT_R8G8B8A8_UNORM);
        renderer.EnableDecodedTextures(false);

        StereoViews views;
        const auto identity = Identity();
        std::copy(identity.begin(), identity.end(), views.worldVP[0]);
        std::copy(identity.begin(), identity.end(), views.worldVP[1]);
        std::copy(identity.begin(), identity.end(), views.hudVP[0]);
        std::copy(identity.begin(), identity.end(), views.hudVP[1]);
        std::copy(identity.begin(), identity.end(), views.skyVP[0]);
        std::copy(identity.begin(), identity.end(), views.skyVP[1]);
        renderer.SetStereoViews(views);

        std::vector<SceneVertex> vertices;
        const float background[3] = {.10f, .10f, .10f};
        const float scene[3] = {.80f, .10f, .10f};
        const float hud[3] = {.10f, .20f, .90f};
        // Full-screen 3D background: its edges are the screen bounds, so it is identical at any sample count.
        AddVertex(vertices, -1, -1, .4f, background);
        AddVertex(vertices, 1, -1, .4f, background);
        AddVertex(vertices, 1, 1, .4f, background);
        AddVertex(vertices, -1, -1, .4f, background);
        AddVertex(vertices, 1, 1, .4f, background);
        AddVertex(vertices, -1, 1, .4f, background);
        // A 3D triangle with a diagonal edge: MSAA must change it.
        AddVertex(vertices, -.9f, -.9f, .6f, scene);
        AddVertex(vertices, -.4f, -.9f, .6f, scene);
        AddVertex(vertices, -.4f, -.4f, .6f, scene);
        // A 2D HUD triangle with a diagonal edge: MSAA must not change it.
        AddVertex(vertices, -.4f, -.4f, .5f, hud);
        AddVertex(vertices, .6f, -.4f, .5f, hud);
        AddVertex(vertices, .6f, .6f, .5f, hud);
        renderer.SetVerticesForAudit(0, vertices);

        const auto identityMvp = Identity();
        std::vector<DrawItem> items(3);
        items[0].firstVertex = 0; items[0].vertexCount = 6;
        std::copy(identityMvp.begin(), identityMvp.end(), items[0].mvp);
        items[0].space = kSpaceWorld;
        items[1].firstVertex = 6; items[1].vertexCount = 3;
        std::copy(identityMvp.begin(), identityMvp.end(), items[1].mvp);
        items[1].space = kSpaceWorld;
        items[2].firstVertex = 9; items[2].vertexCount = 3;
        std::copy(identityMvp.begin(), identityMvp.end(), items[2].mvp);
        items[2].space = kSpaceScreen;

        const Frame one = Render(renderer, items, 2, extent, 1);
        const Frame eight = Render(renderer, items, 2, extent, 8);

        // NDC -> pixels: x = (ndc.x * .5 + .5) * w, y = (ndc.y * .5 + .5) * h (Vulkan's y-down viewport).
        const int hudX0 = int((-.4f * .5f + .5f) * extent.width), hudX1 = int((.6f * .5f + .5f) * extent.width);
        const int hudY0 = int((-.4f * .5f + .5f) * extent.height), hudY1 = int((.6f * .5f + .5f) * extent.height);
        const int sceneX0 = int((-.9f * .5f + .5f) * extent.width), sceneX1 = int((-.4f * .5f + .5f) * extent.width);
        const int sceneY0 = int((-.9f * .5f + .5f) * extent.height), sceneY1 = int((-.4f * .5f + .5f) * extent.height);
        const size_t hudDiff = CountDifferences(one, eight, hudX0, hudY0, hudX1, hudY1);
        const size_t sceneDiff = CountDifferences(one, eight, sceneX0, sceneY0, sceneX1, sceneY1);

        std::printf("msaa_2d_audit: HUD bbox changed %zu, scene bbox changed %zu\n", hudDiff, sceneDiff);
        if (hudDiff != 0) {
            std::fprintf(stderr, "FAIL: 2D HUD changed between MSAA 1x and 8x (%zu channels)\n", hudDiff);
            return 1;
        }
        if (sceneDiff == 0) {
            std::fprintf(stderr, "FAIL: 3D scene did not change between MSAA 1x and 8x\n");
            return 1;
        }
        // The window path (one target, viewMask 0) - the fallback's most common pass, which the stereo-only frame
        // above does not reach. GT2_VK_RENDERPASS=1 records it through render passes, 1.3 through dynamic rendering.
        const auto dir = std::filesystem::temp_directory_path();
        const Frame singleOne = RenderSingleTarget(renderer, items, 2, extent, 1, dir / "gt2-msaa-single-1x.png");
        const Frame singleEight = RenderSingleTarget(renderer, items, 2, extent, 8, dir / "gt2-msaa-single-8x.png");
        const size_t singleHud = CountDifferences(singleOne, singleEight, hudX0, hudY0, hudX1, hudY1);
        const size_t singleScene = CountDifferences(singleOne, singleEight, sceneX0, sceneY0, sceneX1, sceneY1);
        std::printf("msaa_2d_audit: single-target HUD bbox changed %zu, scene bbox changed %zu\n", singleHud, singleScene);
        if (singleHud != 0) {
            std::fprintf(stderr, "FAIL: 2D HUD changed between MSAA 1x and 8x on the single target (%zu channels)\n", singleHud);
            return 2;
        }
        if (singleScene == 0) {
            std::fprintf(stderr, "FAIL: 3D scene did not change between MSAA 1x and 8x on the single target\n");
            return 3;
        }
        std::puts("PASS: MSAA affects the 3D scene and leaves the 2D HUD untouched");
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
