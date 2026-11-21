// The vertex arena: stable virtual vertex indices, GPU memory only for the ranges a scene uploads.
// Renders offscreen, so it needs a Vulkan device but no window or game data.
#include "offscreen_car_audit.h"
#include "gt2view/vk_scene_renderer.h"
#include "gt2formats/png_reader.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <vector>

using namespace gt2view;

namespace {
void Check(bool condition, const char* label) { if (!condition) throw std::runtime_error(label); }

std::vector<SceneVertex> Make(uint32_t count, float x) {
    std::vector<SceneVertex> vertices(count);
    for (uint32_t i = 0; i < count; ++i) {
        vertices[i].pos[0] = x + float(i) * 1e-4f;
        vertices[i].pos[1] = float(i % 7) * 1e-3f;
        vertices[i].color[0] = .25f; vertices[i].flags = i % 3;
    }
    return vertices;
}

std::vector<SceneVertex> Copy(VkSceneRenderer& renderer, uint32_t first, uint32_t count) {
    DrawItem item;
    item.firstVertex = first;
    item.vertexCount = count;
    return renderer.VerticesForAudit(item);
}

void Same(const std::vector<SceneVertex>& expected, const std::vector<SceneVertex>& actual, const char* label) {
    Check(expected.size() == actual.size(), label);
    for (size_t i = 0; i < expected.size(); ++i)
        Check(std::memcmp(&expected[i], &actual[i], sizeof(SceneVertex)) == 0, label);
}

// The reserved layout the arena replaced: one vertex, one UV rect and one environment code per index.
constexpr uint64_t kReservedBytes = uint64_t(SceneRenderer::kMaxVertices) *
    (sizeof(SceneVertex) + 4 * sizeof(float) + sizeof(uint32_t));
} // namespace

int main() {
    std::unique_ptr<Device> device;
    try {
        device = std::make_unique<Device>();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vertex_arena_checks skipped: %s\n", e.what());
        return 0; // no Vulkan 1.3 device on this machine
    }
    try {
        VkContext context(device->existing);
        const VkExtent2D extent{64, 64};
        VkSceneRenderer renderer(context, extent, VK_FORMAT_R8G8B8A8_UNORM);
        renderer.EnableDecodedTextures(false);
        const uint64_t start = renderer.AllocatedBytes();

        // Scattered virtual ranges: the far ones do not reserve the space below them.
        const auto high = Make(3000, 1.f);
        const auto low = Make(2000, 2.f);
        renderer.SetVertices(700000, high);
        renderer.SetVertices(1000, low);
        Check(renderer.AllocatedBytes() - start < 8ull << 20, "the arena follows the content, not the index space");
        Same(high, Copy(renderer, 700000, 3000), "a high virtual range round-trips");
        Same(low, Copy(renderer, 1000, 2000), "a low virtual range round-trips");

        // Growth and relocation: a range that outgrows its block moves, the others keep their addresses.
        const auto grown = Make(60000, 3.f);
        renderer.SetVertices(1000, grown);
        const auto later = Make(4000, 4.f);
        renderer.SetVertices(900000, later);
        Same(grown, Copy(renderer, 1000, 60000), "a relocated range keeps its data");
        Same(high, Copy(renderer, 700000, 3000), "a range below a relocation keeps its data");
        Same(later, Copy(renderer, 900000, 4000), "a range uploaded after a relocation round-trips");
        Check(renderer.AllocatedBytes() - start < kReservedBytes / 2, "the arena is still far below the reserved layout");

        // A range written in two adjacent pieces draws as one block (chunked course uploads).
        const auto firstPart = Make(300, 5.f), secondPart = Make(500, 6.f);
        renderer.SetVertices(640000, firstPart);
        renderer.SetVertices(640300, secondPart);
        DrawItem joined;
        joined.firstVertex = 640000;
        joined.vertexCount = 800;
        Check(renderer.VerticesForAudit(joined).size() == 800, "adjacent uploads are one drawable range");
        bool threw = false;
        try { Copy(renderer, 500000, 10); } catch (const std::exception&) { threw = true; }
        Check(threw, "a range that was never uploaded is not readable");

        // Stress the allocator: rewrite scattered slots with varying sizes, so runs grow, move and their freed
        // blocks get reused and coalesced. Every slot must still read back what was last written to it.
        struct Slot { uint32_t first = 0; std::vector<SceneVertex> data; };
        std::vector<Slot> slots(12);
        uint32_t seed = 12345;
        const auto random = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        for (int step = 0; step < 96; ++step) {
            Slot& slot = slots[size_t(random() % slots.size())];
            slot.first = 1000 + uint32_t(&slot - slots.data()) * 30000;
            slot.data = Make(1 + random() % 12000, float(step));
            renderer.SetVertices(slot.first, slot.data);
            if (step % 8 == 0)
                for (const auto& check : slots)
                    if (!check.data.empty()) Same(check.data, Copy(renderer, check.first, uint32_t(check.data.size())),
                                                  "a rewritten slot keeps its data");
        }
        for (const auto& slot : slots)
            if (!slot.data.empty())
                Same(slot.data, Copy(renderer, slot.first, uint32_t(slot.data.size())), "every slot survives relocation");

        // Draws read their own physical range: two triangles at far virtual addresses, one frame.
        const float red[3] = {1, 0, 0}, green[3] = {0, 1, 0};
        std::vector<SceneVertex> triangles(6);
        const float positions[6][3] = {{-.8f, -.8f, .5f}, {0, -.8f, .5f}, {0, .8f, .5f},   // left, red
                                       {0, -.8f, .5f}, {.8f, -.8f, .5f}, {.8f, .8f, .5f}};  // right, green
        for (int i = 0; i < 6; ++i) {
            for (int k = 0; k < 3; ++k) triangles[i].pos[k] = positions[i][k];
            for (int k = 0; k < 3; ++k) triangles[i].color[k] = i < 3 ? red[k] : green[k];
        }
        const std::vector<SceneVertex> left(triangles.begin(), triangles.begin() + 3);
        const std::vector<SceneVertex> right(triangles.begin() + 3, triangles.end());
        renderer.SetVertices(700000, left);
        renderer.SetVertices(1u << 20, right);
        float identity[16]{};
        for (int i = 0; i < 4; ++i) identity[i * 5] = 1;
        std::vector<DrawItem> items(2);
        items[0].firstVertex = 700000; items[0].vertexCount = 3; items[0].space = kSpaceWorld;
        items[1].firstVertex = 1u << 20; items[1].vertexCount = 3; items[1].space = kSpaceWorld;
        std::memcpy(items[0].mvp, identity, sizeof(identity));
        std::memcpy(items[1].mvp, identity, sizeof(identity));
        const auto path = std::filesystem::temp_directory_path() / "gt2-vertex-arena.png";
        renderer.Draw(items, path.string(), items.size());
        renderer.WaitFrame();
        const auto png = gt2::ReadPngFile(path.string());
        Check(png.width == int(extent.width) && png.height == int(extent.height), "the frame was captured");
        const auto at = [&](int x, int y, int c) { return png.rgba[(size_t(y) * size_t(png.width) + size_t(x)) * 4 + size_t(c)]; };
        // NDC -> pixels: x = (x * .5 + .5) * w, y = (y * .5 + .5) * h (Vulkan's y-down viewport).
        Check(at(19, 19, 0) > 200 && at(19, 19, 1) < 60, "the far virtual range draws its own vertices");
        Check(at(44, 16, 1) > 200 && at(44, 16, 0) < 60, "the second far range draws its own vertices");
        std::filesystem::remove(path);
        std::printf("PASS: vertex arena maps sparse indices onto packed memory (%llu MiB reserved before, +%.1f MiB now)\n",
                    static_cast<unsigned long long>(kReservedBytes / (1024 * 1024)),
                    double(renderer.AllocatedBytes() - start) / (1024 * 1024));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "vertex_arena_checks FAILED: %s\n", e.what());
        return 1;
    }
    return 0;
}
