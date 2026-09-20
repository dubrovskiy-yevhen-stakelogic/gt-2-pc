// The graphics settings of the running game (see graphics_options.h).
#include "graphics_options.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gt2game {

namespace {
gt2::shell::GraphicsSettings g_graphics;
bool g_pinned = false;
bool g_overlay = false;
// The command line's single-setting flags so far (key of settings.txt, value): a preset flag (--vanilla / --modern)
// replaces the settings and re-applies them, so that the flags' order does not matter ("--vsync 0 --modern" keeps vsync off).
std::vector<std::pair<std::string, std::string>> g_overrides;
} // namespace

const gt2::shell::GraphicsSettings& CurrentGraphics() { return g_graphics; }

void SetGraphicsFromSettings(const gt2::shell::GraphicsSettings& settings) {
    if (!g_pinned && !g_overlay) g_graphics = settings;
}
void SetGraphicsFromOverlay(const gt2::shell::GraphicsSettings& settings) {
    if (!g_pinned) { g_graphics = settings; g_overlay = true; }
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
    if (a == "--modern") { preset(GraphicsSettings::Modern()); return true; }
    if (a == "--max-detail") {
        pin();
        g_graphics.Parse("scenery_detail", "max");
        g_overrides.emplace_back("scenery_detail", "max");
        return true;
    }
    static const char* const kKeys[][2] = {{"--frame-rate", "frame_rate"},       {"--frame-cap", "frame_cap"},
                                           {"--vsync", "vsync"},                 {"--render-scale", "render_scale"},
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

gt2view::RenderOptions RenderOptionsOf(const gt2::shell::GraphicsSettings& s) {
    gt2view::RenderOptions o;
    o.sceneScale = float(std::clamp(s.renderScale, 50, 200)) / 100.0f;
    if (s.renderHeight > 0) { o.sceneHeight = uint32_t(s.renderHeight); o.sceneWidth = o.sceneHeight * 16 / 9; }
    o.msaa = uint32_t(std::clamp(s.msaa, 1, 8));
    o.smoothTextures = s.smoothTextures;
    o.affine = s.affine;
    o.vsync = s.vsync;
    return o;
}

std::string DescribeGraphics(const gt2::shell::GraphicsSettings& s) {
    char distance[32];
    if (s.drawDistance < 0) std::snprintf(distance, sizeof(distance), "all chunks");
    else if (s.drawDistance == 0) std::snprintf(distance, sizeof(distance), "original");
    else std::snprintf(distance, sizeof(distance), "+%d m", s.drawDistance);
    char line[320];
    std::snprintf(line, sizeof(line), "frame rate %s, cap %d, vsync %s, render scale %d%%, MSAA %dx, textures %s, mapping %s, scenery %s, draw distance %s%s%s",
                  s.frameRate == gt2::shell::GraphicsSettings::kFrameRateOriginal ? "original 30" : "display", s.frameCap, s.vsync ? "on" : "off", s.renderScale,
                  s.msaa, s.smoothTextures ? "smooth" : "PS1 nearest", s.affine ? "affine" : "perspective", s.maxDetail ? "max" : "original", distance,
                  s == gt2::shell::GraphicsSettings::Vanilla() ? " (vanilla)" : "", g_pinned ? " [command line]" : "");
    return std::string(line) + (s.renderHeight ? "; fixed scene " + std::to_string(s.renderHeight * 16 / 9) + "x" + std::to_string(s.renderHeight) : "");
}

} // namespace gt2game
