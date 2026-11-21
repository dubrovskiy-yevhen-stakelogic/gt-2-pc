// The graphics settings of the running game (see graphics_options.h).
#include "graphics_options.h"
#include "gt2formats/model_set.h"
#ifndef __EMSCRIPTEN__
#include "gt2view/vk_capabilities.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gt2game {

namespace {
gt2::shell::GraphicsSettings g_graphics;
bool g_pinned = false;
bool g_overlay = false;
// Defaults for the two content choices that spend the most video memory (HD media, GT4 models) on a small machine.
// Not a stored mode: the rows remain free choices, and a bigger GPU defaults them on.
bool g_tightMemory = false;
// The command line's single-setting flags so far (key of settings.txt, value): a preset flag (--vanilla / --modern)
// replaces the settings and re-applies them, so that the flags' order does not matter ("--vsync 0 --modern" keeps vsync off).
std::vector<std::pair<std::string, std::string>> g_overrides;
} // namespace

const gt2::shell::GraphicsSettings& CurrentGraphics() { return g_graphics; }

bool TightVideoMemory() { return g_tightMemory; }

void SetGraphicsFromSettings(const gt2::shell::GraphicsSettings& settings) {
    // settings.txt is the canonical store: a (legacy) overlay read before it must not shadow it. The command line wins.
    if (!g_pinned) { g_graphics = settings; gt2::modernCarReflections = settings.modernCarReflections; gt2::carReflections = settings.carReflections; }
}
void SetGraphicsFromPage(const gt2::shell::GraphicsSettings& settings) {
    if (g_pinned) return;
    g_graphics = settings;
    g_overlay = true;
    gt2::modernCarReflections = settings.modernCarReflections;
    gt2::carReflections = settings.carReflections;
}
void SetGraphicsFromOverlay(const gt2::shell::GraphicsSettings& settings) {
    if (!g_pinned) { g_graphics = settings; g_overlay = true; gt2::modernCarReflections=settings.modernCarReflections;gt2::carReflections=settings.carReflections;}
}

bool GraphicsPinned() { return g_pinned; }

bool ParseGraphicsFlag(int argc, char** argv, int& i, const gt2::shell::GraphicsSettings& fileSettings) {
    using gt2::shell::GraphicsSettings;
    const std::string a = argv[i];
    auto value = [&]() -> std::string {
        if (i + 1 >= argc) throw std::runtime_error(a + " needs a value");
        return argv[++i];
    };
    auto pin = [&] {
        if (!g_pinned && !g_overlay) g_graphics = fileSettings;
        g_pinned = true;
    };
    auto preset = [&](const GraphicsSettings& p) {
        pin();
        g_graphics = p;
        for (const auto& o : g_overrides) g_graphics.Parse(o.first, o.second);
    };
    if (a == "--vanilla") { preset(GraphicsSettings::Vanilla()); return true; }
    if (a == "--low-spec") { preset(GraphicsSettings::LowSpec()); return true; }
    if (a == "--modern") { preset(GraphicsSettings::Modern()); return true; }
    if (a == "--max-detail") {
        pin();
        g_graphics.Parse("scenery_detail", "max");
        g_overrides.emplace_back("scenery_detail", "max");
        return true;
    }
    static const char* const kKeys[][2] = {{"--frame-rate", "frame_rate"},       {"--frame-cap", "frame_cap"},
                                           {"--vsync", "vsync"},                 {"--output-height", "output_height"},   {"--render-scale", "render_scale"},
                                           {"--msaa", "msaa"}, {"--render-height", "render_height"},                   {"--texture-filter", "texture_filter"},
                                           {"--texture-mapping", "texture_mapping"}, {"--draw-distance", "draw_distance"}};
    for (const auto& k : kKeys)
        if (a == k[0]) {
            pin();
            const std::string v = value();
            if (!g_graphics.Parse(k[1], v)) throw std::runtime_error(a + ": bad value '" + v + "'");
            g_overrides.emplace_back(k[1], v);
            return true;
        }
    return false;
}

RecommendedMemory ApplyRecommendedMemory(const std::string& settingsPath) {
#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
    (void)settingsPath;
    return {}; // the browser and Quest builds have their own context and no Vulkan probe
#else
    RecommendedMemory result;
    if (settingsPath.empty()) return result;
    const gt2view::VulkanCapabilities caps = gt2view::ProbeVulkan();
    const gt2view::GpuCapability* gpu = caps.best();
    const uint64_t usable = gpu ? gt2view::UsableMemory(*gpu) : 0;
    const auto mib = [](uint64_t bytes) { return std::to_string(bytes / (1024 * 1024)); };
    gt2::shell::PcSettings stored = gt2::shell::PcSettings::Load(settingsPath);
    const uint32_t output = gt2view::RecommendedOutputHeight(usable, uint32_t(std::max(stored.graphics.outputHeight, 0)));
    // HD media and GT4 models are the two content choices that can spend ~100 MiB on their own. Their defaults follow
    // the machine (about the point where the decoded cache would shrink under half); the rows stay free choices.
    g_tightMemory = gt2view::RecommendedDecodedCacheLayers(usable, output) < 100; // ~62 MiB
    if (gt2view::GraphicsMemoryTooLow(caps, output)) {
        result.memoryTooLow = true;
        result.reason = mib(usable) + " MiB of video memory available, at least " + mib(gt2view::GraphicsMemoryFloor(output)) + " MiB required";
        return result;
    }
    if (g_pinned || g_overlay) return result; // the command line wins
    bool changed = false;
    std::string why;
    if (output != uint32_t(std::max(stored.graphics.outputHeight, 0))) {
        stored.graphics.outputHeight = int(output);
        changed = true;
        why = output ? std::to_string(output) + "p" : "display";
    }
    const uint32_t msaa = gt2view::RecommendedMsaa(usable, output, uint32_t(std::clamp(stored.graphics.msaa, 1, 8)));
    if (msaa != uint32_t(stored.graphics.msaa)) {
        stored.graphics.msaa = int(msaa);
        changed = true;
        why += (why.empty() ? "" : ", ") + std::string("MSAA ") + (msaa > 1 ? std::to_string(msaa) + "x" : "off");
    }
    if (!changed) return result; // this machine's settings already fit
    g_graphics = stored.graphics; // this session runs it now, not only from the next launch
    try {
        const std::filesystem::path path(settingsPath);
        if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
        stored.Save(settingsPath);
    } catch (const std::exception&) {
        // The session still runs adapted; the next change writes the file.
    }
    result.applied = true;
    result.reason = mib(usable) + " MiB of video memory available: " + why;
    return result;
#endif
}

gt2view::RenderOptions RenderOptionsOf(const gt2::shell::GraphicsSettings& s) {
    gt2view::RenderOptions o;
    o.outputHeight = uint32_t(std::clamp(s.outputHeight, 0, 4320));
    o.sceneScale = float(std::clamp(s.renderScale, 50, 200)) / 100.0f;
    if (s.renderHeight > 0) { o.sceneHeight = uint32_t(s.renderHeight); o.sceneWidth = o.sceneHeight * 16 / 9; }
    o.msaa = uint32_t(std::clamp(s.msaa, 1, 8));
    o.smoothTextures = s.smoothTextures;
    o.affine = s.affine;
    gt2::modernCarReflections=s.modernCarReflections;
    gt2::carReflections=s.carReflections;
    o.vsync = s.vsync;
    return o;
}

std::string DescribeGraphics(const gt2::shell::GraphicsSettings& s) {
    char distance[32];
    if (s.drawDistance < 0) std::snprintf(distance, sizeof(distance), "all chunks");
    else if (s.drawDistance == 0) std::snprintf(distance, sizeof(distance), "original");
    else std::snprintf(distance, sizeof(distance), "%d m", s.drawDistance);
    const std::string output = s.outputHeight ? std::to_string(s.outputHeight) + "p" : "display";
    char line[320];
    std::snprintf(line, sizeof(line), "frame rate %s, cap %d, vsync %s, output %s, scene scale %d%%, MSAA %dx, textures %s, mapping %s, scenery %s, draw distance %s%s%s%s",
                  s.frameRate == gt2::shell::GraphicsSettings::kFrameRateOriginal ? "original 30" : "display", s.frameCap, s.vsync ? "on" : "off", output.c_str(), s.renderScale,
                  s.msaa, s.smoothTextures ? "smooth" : "PS1 nearest", s.affine ? "affine" : "perspective", s.maxDetail ? "max" : "original", distance,
                  s == gt2::shell::GraphicsSettings::LowSpec() ? " (low spec)"
                  : s == gt2::shell::GraphicsSettings::Vanilla() ? " (vanilla)" : "",
                  g_pinned ? " [command line]" : "", TightVideoMemory() ? " [small video memory]" : "");
    return std::string(line) + (s.renderHeight ? "; fixed scene " + std::to_string(s.renderHeight * 16 / 9) + "x" + std::to_string(s.renderHeight) : "");
}

} // namespace gt2game
