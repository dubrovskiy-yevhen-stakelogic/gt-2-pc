#pragma once
// The graphics settings of the running game (docs/formats/modern_graphics.md): the PC SETTINGS page / settings.txt
// (shell::GraphicsSettings) with the command line on top. Presentation only - the race view reads them when a race
// starts; nothing of the simulation, the replays or the checks against the original depends on them.
//
// Command line (main.cpp): --vanilla (today's output exactly: Original 30 frame rate, window resolution, no MSAA, PS1
// nearest texels, perspective-correct mapping, the original's LOD and render lists), --modern, --frame-rate
// original|display, --frame-cap N, --vsync 0|1, --render-scale P (percent) , --msaa N, --texture-filter
// nearest|smooth, --texture-mapping perspective|affine, --draw-distance original|all|<metres>, --max-detail.
// Any of them pins the session's settings: the PC SETTINGS page still edits and saves settings.txt, but the running
// session keeps the command line's values (automated runs stay reproducible).
#include <string>

#include "game/shell/title_options.h"
#include "gt2view/vk_scene_renderer.h"

namespace gt2game {

// The settings in effect (initially GraphicsSettings{} = the defaults).
const gt2::shell::GraphicsSettings& CurrentGraphics();
// From settings.txt / the PC SETTINGS page: ignored once the command line pinned the settings.
void SetGraphicsFromSettings(const gt2::shell::GraphicsSettings& settings);
// A graphics flag of the command line at argv[i] (advances i past its value); false when it is not one. The first flag
// seen starts from `fileSettings` (settings.txt) and pins the session.
bool ParseGraphicsFlag(int argc, char** argv, int& i, const gt2::shell::GraphicsSettings& fileSettings);
bool GraphicsPinned();
void SetGraphicsFromOverlay(const gt2::shell::GraphicsSettings& settings);

// The renderer's side of the settings.
gt2view::RenderOptions RenderOptionsOf(const gt2::shell::GraphicsSettings& settings);
// One line for the log ("graphics: frame rate display, cap 0, vsync on, scale 100%, MSAA 1x, ...").
std::string DescribeGraphics(const gt2::shell::GraphicsSettings& settings);

} // namespace gt2game
