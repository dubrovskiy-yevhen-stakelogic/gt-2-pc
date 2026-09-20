#pragma once
// Dev check of the post-race views (src/gt2view/race_result_screens.h) against gt2play captures:
//   gt2game <disc> --race-menu-check <results|bonus|postmenu> <cap.txt.vram.bin> <side.png> [ram=<ram.bin>] [sim=N]
//           [advance=N [press=K:button,...]] [ps1] [vulkan]
// The view's state is read from the RAM dump of the capture (default <cap.txt>.ram.bin = RAM after field F + 6, the
// state of the capture's last frame; during a view switch, M+0x211 > 0, the view is the current or the previous one
// and the other a wait / leave view); `advance=N` runs our view manager (PostRaceFlow) N fields on it (ram= an earlier
// dump; `press=` pads of single updates, 1-based: a gt2play script press at field P is update K = P - F - 4 of a run
// from the dump of capture F (measured on the captures); a view that is left switches to the leave view 0x8005AE30);
// `sim=N` instead sets the view up from the dump's
// race results (place, times, laps, prize, money) and runs N fields without input (N = the fields since the original's
// setup call). Compares
//   - the primitive sequence of our frame with the capture's last frame (gt2play listing <cap.txt>), leaving out the
//     primitives drawn in the 3D car's draw environment (drawing area other than the full frame): printed as excluded;
//   - our frame rasterised with the capture's rules against the capture's VRAM (352 x 480), leaving out the pixels
//     inside the car's drawing area when the capture has car primitives (the count is printed);
//   - the VRAM of the fonts (pages 6 / 7) and of arcade/setting.tim (page 0x16) with what RaceMenuAssets kSettings uploads;
//   - the 3D model of the view (0x80048754, PostRaceModel; camera objects from the dump): its floor disc primitive by
//     primitive and the silhouette of the captured model primitives against our model's triangles through our
//     projection (IoU >= 0.85; <side>_model.png); with `vulkan` also the renderer's own model pixels (gt2game's
//     Panels::FullScreenModel in a 640 x 480 window; <side>_vulkan.png). With sim= the RESULTS car camera is seeded
//     with the VSync counter that reproduces the dump's pose (printed: the check of 0x80050BC4).
// Writes ours | original | differences (excluded pixels blue) to side.png. Returns 0 when everything compared is equal.
#include <string>
#include <vector>

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

bool IsRaceResultScreen(const char* screen);
// args[0] = the screen, then the arguments above.
int RunRaceResultCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::vector<std::string>& args);
