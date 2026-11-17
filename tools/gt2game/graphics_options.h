#pragma once
// The graphics settings of the running game (docs/formats/modern_graphics.md): the PC SETTINGS page / settings.txt
// (shell::GraphicsSettings) with the command line on top. Presentation only - the race view reads them when a race
// starts; nothing of the simulation, the replays or the checks against the original depends on them.
//
// Command line (main.cpp): --vanilla (today's output exactly: Original 30 frame rate, window resolution, no MSAA, PS1
// nearest texels, perspective-correct mapping, the original's LOD and render lists), --low-spec (the small-VRAM
// preset; the Vulkan 1.1/1.2 render-pass path is separate), --modern, --frame-rate
// original|display, --frame-cap N, --vsync 0|1, --render-scale P (percent) , --msaa N, --texture-filter
// nearest|smooth, --texture-mapping perspective|affine, --draw-distance original|all|<metres>, --max-detail,
// --output-height 720|900|1080|0 (0 = the display; the swapchain is scaled up by the presentation engine).
// Any of them pins the session's settings: the PC SETTINGS page still edits and saves settings.txt, but the running
// session keeps the command line's values (automated runs stay reproducible).
#include <string>

#include "game/shell/title_options.h"
#include "gt2view/scene_renderer.h"

namespace gt2game {

// The settings in effect (initially GraphicsSettings{} = the defaults).
const gt2::shell::GraphicsSettings& CurrentGraphics();
// From settings.txt (the canonical store): ignored once the command line pinned the settings.
void SetGraphicsFromSettings(const gt2::shell::GraphicsSettings& settings);
// From the PC SETTINGS page: the page is authoritative for the session (it also overrides an overlay file loaded at
// startup), still ignored when the command line pinned the settings.
void SetGraphicsFromPage(const gt2::shell::GraphicsSettings& settings);
// A graphics flag of the command line at argv[i] (advances i past its value); false when it is not one. The first flag
// seen starts from `fileSettings` (settings.txt) and pins the session.
bool ParseGraphicsFlag(int argc, char** argv, int& i, const gt2::shell::GraphicsSettings& fileSettings);
bool GraphicsPinned();
void SetGraphicsFromOverlay(const gt2::shell::GraphicsSettings& settings);
// Applies the memory safety this machine needs: Memory Use Low (the decoded texture cache stays off) when the
// available video memory is below what the standard settings need. It changes no other setting, so a chosen quality
// preset is kept; the Low spec preset remains a manual choice. A machine below the game's own floor reports
// `memoryTooLow` instead, so the game can stop with a clear message rather than fail during a race.
// Command-line flags still win. `reason` is one line for the log.
struct RecommendedMemory {
    bool applied = false;
    bool memoryTooLow = false;
    std::string reason;
};
RecommendedMemory ApplyRecommendedMemory(const std::string& settingsPath);

// The renderer's side of the settings.
gt2view::RenderOptions RenderOptionsOf(const gt2::shell::GraphicsSettings& settings);
// One line for the log ("graphics: frame rate display, cap 0, vsync on, scale 100%, MSAA 1x, ...").
std::string DescribeGraphics(const gt2::shell::GraphicsSettings& settings);

} // namespace gt2game
