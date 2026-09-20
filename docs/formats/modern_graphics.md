# Modern graphics options (ours) and the display frame rate

Not a format of the original: the PC presentation options of gt2game. They change HOW a frame is drawn, never WHAT the
original would draw or any simulation state; the simulation, replays, the career and every gt2verify row are unaffected.
Code: `src\game\shell\title_options.* GraphicsSettings` (settings.txt keys, PC SETTINGS page rows), `tools\gt2game\
graphics_options.*` (command line, settings -> renderer), `src\gt2view\vk_scene_renderer.* RenderOptions` (scene target,
MSAA, shader options), `src\gt2view\shaders\scene.{vert,frag}`, `tools\gt2game\frame_interp.*` (interpolation between two
30 Hz steps), `tools\gt2game\race_view.cpp` (the presentation loop), `tools\gt2game\game_window.*` (field clock, vblank timing).

## 1. Settings

| settings.txt key | Values (default first) | Flag | Meaning |
|---|---|---|---|
| `frame_rate` | `display`, `original` | `--frame-rate` | display: frames at the display's rate, interpolated between the last two 30 Hz steps; original: each step shown as it is (two fields per step) |
| `frame_cap` | `0` (none), N | `--frame-cap N` | frames per second with `display` |
| `vsync` | `1`, `0` | `--vsync 0\|1` | FIFO presentation; off: MAILBOX (or IMMEDIATE) |
| `render_scale` | `100` (50..200) | `--render-scale P` | the 3D scene's internal resolution in percent of the window (the HUD / 2D layers stay at the window's) |
| `msaa` | `1`, 2, 4, 8 | `--msaa N` | multisampling of the 3D scene (clamped to the GPU's limit) |
| `texture_filter` | `nearest`, `smooth` | `--texture-filter` | smooth: texels decoded through their CLUT first, then bilinear inside the polygon's UV rectangle, alpha-masked texels left out, STP class kept |
| `texture_mapping` | `perspective`, `affine` | `--texture-mapping` | affine: the PS1 GPU's screen-linear texture / colour interpolation (warp bounded to 4 texels, since our course polygons are not subdivided) |
| `scenery_detail` | `original`, `max` | `--max-detail` | max: every scenery instance at LOD 0 without the distance cut-off |
| `draw_distance` | `original`, N metres, `all` | `--draw-distance` | original: the camera chunk's render list (+0xA0) only; N: plus every chunk within N m of the eye; all: every chunk |

Presets: `--vanilla` = `GraphicsSettings::Vanilla()` (frame rate original, everything else default: the frame is the
same image as before the options existed - 0 differing pixels against the pre-options build, race Seattle frame 1600);
`--modern` = MSAA 4x, smooth textures, max scenery, draw distance 1000 m (frame rate display). A preset replaces the
settings and then re-applies the single-setting flags of the command line, so the order of flags does not matter
(`--vsync 0 --modern` keeps vsync off). Any graphics flag pins the session: the PC SETTINGS page still edits and saves
settings.txt, but the running process keeps the command line's values (reproducible automated runs).

Scripted, `--shot`, `--fast` and `--frames-compare` runs always present frame-locked (the original's two fields per step),
whatever the settings: their frames must be reproducible. `--interp-alpha A` draws a fixed in-between state in such runs.

## 2. The display frame rate

The simulation keeps the original's 30 Hz step (bit-exact, replays frame-locked). With `frame_rate=display`:

- **Time base.** The window's paced 60 Hz field clock (`GameWindow::NextField`) runs the input, scripts and logic; a step
  runs on every second field (the same rule as the deterministic runs), and its state belongs to that field's SCHEDULED
  time (`stepAnchor`), not to the time the step happened to execute. A field that starts late (a blocking present, a
  hitch) keeps its scheduled time while it is at most 4 fields behind (`SkipFrame(frame, maxLag)`): the following fields
  catch up and no game time is lost.
- **Interpolation.** `RenderSnapshot` copies what the renderer reads after a step (car render transforms, the camera
  object's output, the smoke pool, the HUD gauges, the race clock); a frame shown at time t draws
  alpha = (t - stepAnchor) / step (clamped to 0..1) between the snapshot before the last step and the last step:
  car matrices lerped + Gram-Schmidt, camera eye / axes / H lerped (`CameraCut` / `PoseJump` show a cut as it is), smoke
  records alive in both lerped, HUD rpm / speed / clock lerped. Only copies are written; never the simulation or the camera.
- **Pacing.** t is the time the frame reaches the screen:
  - vsync (FIFO): one frame per vertical blank of the compositor (`GameWindow::VBlankTiming` = DwmGetCompositionTimingInfo:
    last vblank + refresh period on the steady clock); the frame is built just before its blank and shows that blank's time.
  - no vsync with a cap: frames at the cap's period (high-resolution waitable timer), built `buildEstimate` ahead of their slot.
  - no vsync, no cap: as fast as frames are built.
  The cadence runs across fields: a slot before the next field is presented even when its build runs a little into that
  field; a later slot is taken by the next field's frame (with the newer state). `buildEstimate` tracks the build time
  (rises at once, decays 10 %, a single measure is capped at 5.5 ms so a hitch cannot stop the in-between frames).
- **Window title.** With the display frame rate the title (SetWindowText, which waits ~20 ms for the compositor) changes only
  when the lap / position text changes.

### Measurements (2026-09-19, Seattle, 6 cars, `--ai-player`, `--frame-log <csv>`; 60 Hz monitor)

`--frame-log` writes one CSV line per presented frame: time, frame time, in-between flag, alpha, steps, build / present ms.
"alpha step error" = |alpha(frame) - alpha(previous frame) - frame spacing / step| (0 = every frame shows exactly its own time).

| Settings | fps | frame ms median / p99 / max | alpha step error median / p99 | steps/s |
|---|---|---|---|---|
| default (vsync, 60 Hz display) | 60.0 | 16.67 / 17.3 / 17.6 | 0 (every step exactly 0.5) | 30.0 |
| `--frame-cap 72 --vsync 0` | 72.0 | 13.89 / 14.75 / 30.5 | 0.0007 / 0.049 | 30.0 |
| `--frame-cap 72 --vsync 0 --modern` | 72.0 | 13.89 / - / 15.1 | - | 30.1 |
| `--frame-cap 72 --vsync 0 --modern --render-scale 200 --msaa 8` | 72.0 | 13.89 / 15.33 / 16.65 | 0.0007 / 0.060 | 29.9 |
| `--frame-cap 90 --vsync 0 --modern` | 89.9 | 11.12 / 13.88 / 26.3 | 0.0007 / 0.164 | 30.2 |
| `--frame-cap 120 --vsync 0 --modern` | 120.0 | 8.34 / 9.72 / 13.0 | 0.0000 / 0.081 | 30.0 |
| `--frame-cap 144 --vsync 0` | 142.4 | 7.02 (avg) / - / 8.54 | 0.008 / 0.018 | 30.0 |
| `--vsync 0` (no cap) | 525.7 | 1.90 (avg) / - / 3.52 | - | 30.0 |
| `--frame-cap 30` | 30.0 | 33.33 (avg) / - / 35.3 | - | 30.0 |

The frame build (CPU draw items) costs ~1.4 ms: cars with the reflection pass ~1.0 ms, course ~0.2 ms, HUD ~0.2 ms.
Fixed on 2026-09-19: the earlier loop anchored alpha to the step's execution time and the field frame's build start
(alpha clamped to 1 and repeated, then a jump: 0.61 -> 1.00 -> 0.11), dropped the slot that did not fit before the next
field (cap 144 gave 120 fps) and, with vsync, lost game time to the blocking present (25 steps/s on a 60 Hz display) and
showed two nearly equal states per step.

### VR (Quest 3: 72 / 90 / 120 Hz)

The same scheme serves an OpenXR runtime: the frame's display time is `XrFrameState::predictedDisplayTime` of xrWaitFrame
(instead of the DWM blank), converted to the steady clock (`xrConvertTimeToWin32PerformanceCounterKHR`), and the frame
shows alpha = (predictedDisplayTime - stepAnchor) / step; the 30 Hz steps and the 60 Hz field clock stay as they are.
The monitor measurements above (72 / 90 / 120 with the modern preset, 200 % scale and MSAA 8x at 72) are the flat
equivalent; head pose is not part of this (the headset's view goes on top of the interpolated camera).

## 3. Not done

- Widescreen HUD / 2D layouts beyond the edge anchoring the HUD already has; UI scaling for high resolutions.
- The menus / title / screens run at the field rate (60 Hz; nothing there moves between fields except the 3D car in menus).
- An upscaler (DLSS-like); texture replacement packs.
