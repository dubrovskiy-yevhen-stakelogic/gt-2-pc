# GT2 on Quest 3 (standalone Android): staged VR port plan

Implementation status, 2026-09-20: the first standalone APK now builds and installs with theatre menus, stereo single-player driving, Touch controls and AAudio. See ../QUEST.md and ../VALIDATION.md for the current scope and verified evidence. The roadmap below records earlier design and simulator work; estimates and proposed milestones are not completion claims. The first Android build still samples PS1 VRAM/CLUT textures and uses extracted assets plus raw sectors, not the proposed fully predecoded pack. Headset rendering/performance acceptance remains pending.

Plan of 2026-09-19 (design survey of C:\Dev\harry-potter-vr = HP, C:\Dev\miamivr-quest\overlay = VC, and this repo).
User decisions: VR after the flat game is finished; target = standalone Android build on Meta Quest 3, modelled on HP
(C++20 reimplementation on Vulkan + OpenXR + AAudio); HP / VC / Gothic / GTA SA ports are the user's own code and can be
reused directly. Automated checks on the PC: our OpenXR simulator runtime tools\xrsim (docs\research\xrsim.md).
Related: docs\research\quest_ports.md, docs\formats\modern_graphics.md (display-rate interpolation, VR section).

## 0. Installer and PC-side preparation (user decision 2026-09-19)

Nothing is decoded or prepared on the headset. As in HP VR (whose on-device level preparation made loads very long) and
the user's other mods, a PC installer does everything: installs its own dependencies (adb / platform-tools from the
user's toolchain, the APK), reads the user's disc image(s) on the PC, converts every asset into a ready-to-use pack
(decoded textures in the GPU format the headset samples directly - RGBA8 or ASTC pages instead of PS1 VRAM + CLUT decoding
in scene.frag; meshes, course data, audio banks decoded to PCM, movies, menus / fonts, the career save), packs it, and
installs APK + pack on the headset (adb install, adb push into the app's external files dir, created as the app's UID
first). The headset only loads prepared data. The pack is generated on the user's machine from their disc and never
shipped (no game data in the repo or the APK). Consequences for the plan: the "Adreno cost of scene.frag" risk goes away
(the Quest shader samples prepared textures); section 4 (data) becomes "the installer pushes the pack"; a new milestone
M6a = the installer (tools/installer: dependency setup, pack builder sharing gt2formats parsers, headset detection and
install, progress UI) with the check "fresh headset -> one run of the installer -> the game starts with no on-device
preparation".

## 1. Architecture

Findings: every screen (title, menus, race, panels, movies) runs its own loop on GameWindow::BeginFrame / EndFrame / Present,
so the platform seam belongs inside GameWindow. Every DrawItem has the view-projection pre-multiplied (race_view.cpp
Multiply(vp, model, ...)) - must change for stereo. VkSceneRenderer(HINSTANCE, HWND) owns instance / Win32 surface / device,
one frame in flight. Windows-only today: game_window.* (message pump, DWM timing, waitable timer), audio_device.cpp (waveOut),
input_system.* (XInput / DirectInput, windows.h in the header), title_mode.cpp ExecutableDirectory(), VK_* key codes in ten
gt2game .cpp files, root CMakeLists (gt2vk / gt2game under WIN32, winmm / dinput8 / dwmapi). Portable already: sim, camera,
pad model (ps1_pad.*), mixer, formats, VFS, all 2D builders (they take windowAspect).

| Layer | New / changed files | Reuse |
|---|---|---|
| OS services | src/platform/os/paths.h + paths_win32.cpp / paths_android.cpp; src/platform/os/keys.h (numeric VK-compatible constants) | - |
| Vulkan context | src/gt2view/vk_context.{h,cpp}: own instance + Win32 surface, or instance / device via xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR; VkSceneRenderer(VkContext&) renders into a RenderTarget (window swapchain, XR stereo array image colour + depth, offscreen 2D image) | VC xr_vulkan_session.cpp device creation |
| XR session | src/platform/xr/xr_session.{h,cpp}: instance, system, vulkan_enable2, state machine, LOCAL / STAGE / VIEW, swapchains (stereo colour + depth array, cinema quad, HUD quad, mirror quad), FB display refresh rate, EXT performance settings, META performance metrics, time conversion | VC xr_vulkan_session.{h,cpp} (theater anchor, refresh / perf retry, render-scale fallback); HP android_main.cpp InitializeOpenXr; HP xr_vulkan_smoke.cpp (refresh enumeration, metrics) |
| XR input | src/platform/xr/xr_actions.{h,cpp} (Touch profile -> ControllerInput, haptics) | VC ControllerInput / getInput / triggerHaptic |
| VR rig | src/platform/xr/vr_rig.{h,cpp}: camera o comfort o seat o head, recenter, horizon lock | HP quest_recenter.h; VC VrVehicleSeatRecenter.h |
| Window backends | game_window.h = portable front (input latching, key script, field clock, shots) over WindowBackend: game_window_win32.cpp (today), game_window_xr.cpp (Windows + Android) | - |
| Audio | audio_device_win32.cpp + audio_device_aaudio.cpp behind AudioDevice | HP quest_audio.cpp |
| Input | input_system.h without windows.h; input_system_win32.cpp; src/platform/input/xr_pad_device.{h,cpp} (gt2::input::Device) | VC QuestDrivingVR.cpp steering math |
| Entry | main()'s body -> tools/gt2game/game_main.{h,cpp} (GameMain(args, PlatformHost&)); main.cpp = Windows entry; android/app/src/main/cpp/android_main.cpp | HP android_main.cpp, quest_lifecycle.* |

### 1.1 M0 done (2026-09-20): the portability split, Windows only, no behaviour change

What moved where (everything else is untouched; no game logic was renamed):

| New / renamed file | Contents |
|---|---|
| `src/platform/os/keys.h` | the key codes the screens use, as numbers (= the Win32 virtual-key codes, so a key message's wParam passes straight through). The ten gt2game `.cpp` files that used `VK_*` now use `keys::kUp` ... and none of them includes `<windows.h>` any more. |
| `src/platform/os/paths.h` + `paths_win32.cpp` (library `gt2os`) | `gt2::os::ExecutableDir / DataRoot / SavesDir / LogPath / TickCountMs`. Replaces `title_mode.cpp ExecutableDirectory()` (gone from `title_mode.h`) and the `GetTickCount()` seeds of `game_main.cpp`, `race_view.cpp`, `split_race.cpp`, `arcade_mode.cpp`. Android adds `paths_android.cpp` here (data root = the app's external files dir, saves = internal). |
| `src/gt2view/vk_context.{h,cpp}` | `VkContext`: instance, surface, physical device, device, queue family, queue - exactly the code that was `VkSceneRenderer::CreateDevice`, plus the window's client size for a surface that does not report its extent. `VkSceneRenderer(VkContext&)` creates none of it and destroys none of it; it still owns the window swapchain, the pipelines, the buffers and the scene targets. `vk_scene_renderer.h` no longer defines `VK_USE_PLATFORM_WIN32_KHR`, so `<windows.h>` no longer reaches everything that includes it. |
| `src/gt2view/vk_context.h RenderTarget` | colour view + format, optional depth view + format, extent, array layers. `VkSceneRenderer::WindowTarget(imageIndex)` builds it for the swapchain image and `BeginTargetRendering(target, loadOp)` (was `BeginWindowRendering`) begins the dynamic rendering from it. |
| `tools/gt2game/game_window.{h,cpp}` + `game_window_win32.cpp` | `game_window.cpp` keeps the portable front - the key script, the input latching of a field, the pad merge, the screenshots, the field clock and the pacing API. `WindowBackend` (declared in `game_window.h`) is the OS half: renderer, native handle, window caption, `Pump`, `Closed / Close / Focused`, `TakeKeyDowns`, `KeyDown`, `TakeDevicesChanged`, `SleepUntil`, `VBlankTiming`; `game_window_win32.cpp` holds today's code (window class and message pump, `timeBeginPeriod`, the waitable timer, `DwmGetCompositionTimingInfo`) behind `CreateWindowBackend`. `GameWindow::Handle()` is replaced by `SetTitle` (the five `SetWindowTextA` call sites). |
| `src/game/audio/audio_device_win32.cpp` | the waveOut implementation of the unchanged `audio_device.h` interface; `audio_device_aaudio.cpp` will sit next to it. |
| `src/platform/input/input_system.h` + `input_system_win32.cpp` | the header has no `<windows.h>`: `AttachWindow(void*)` and an opaque window handle; the XInput / DirectInput devices are the platform file. |
| `tools/gt2game/game_main.{h,cpp}` + `main.cpp` | `GameMain(argc, argv)` is the program (command line and dispatch); `main.cpp` is the five-line Windows entry point. `android_main` will call the same function. |
| `CMakeLists.txt` | the Windows-only pieces are named in variables instead of being wired in directly: `GT2_AUDIO_PLATFORM_SOURCES`, `GT2VK_PLATFORM_SOURCES`, `GT2GAME_PLATFORM_SOURCES` / `GT2GAME_PLATFORM_LIBS` (user32 gdi32 winmm dwmapi), `gt2os` and `gt2inputdev` under `if(WIN32)`. No Android branch yet. |

What the XR path plugs into (M1 / M2), with nothing to undo: `VkContext(VkContext::Existing{...})` adopts the instance /
physical device / device / queue family / queue that `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` produce and
owns none of them; such a context has no surface, so the renderer's window swapchain is the only part left to bypass -
the frame is recorded through `RenderTarget`, whose `layers` becomes the stereo array image's layer count.
`CreateWindowBackend` is the single place a `game_window_xr.cpp` replaces, and `GameMain` is the single entry point an
`android_main.cpp` calls.

Verified on Windows (build `build_m0`, warnings = errors, clean): `gt2verify` race_demo 265 ok / license_race 249 /
gtmode 72 / title 55 / arcade_race 250 / arcade_2p 246, all 0 FAIL, exit 0; `--selftest --cars 6` step 300 = 137.8 km/h,
rpm 6126, gear 3, 0 failures; `--vanilla --shot` race / menu / title PNGs byte-identical to `build_final`'s;
`--race-screen-check pause` (both states) 78 / 78 primitives 0 pixels, `--race-menu-check licence | event` with the
dumps' state 0 pixels, `--change-parts-check` (Simulation route) 10 captures 0 pixels and an equal commit,
`--title-compare` (interpreter sprite rules) 0 pixels; `--replay-check 60` identical, 0 failures; `--frame-cap 72
--vsync 0 --frame-log` 71.9 fps (13.892 ms median) and 30.00 steps/s, as `build_final` on the same machine.

Builds: (a) Windows desktop - unchanged, `--vanilla` bit-identical; (b) Windows + OpenXR - same exe with `--vr` (loader
loaded dynamically, the local build found via XRSIM_OPENXR_LOADER; without it `--vr` fails cleanly); (c) Android -
libgt2game.so from the root CMake with if(ANDROID) branches (+ openxr_loader, vulkan, aaudio, android, log).
Settings: vr_* keys in settings.txt (GraphicsSettings / graphics_options).

### 1.2 M1 done (2026-09-20): the XR session on Windows, `--vr`, every screen on the cinema quad (mono)

`gt2game <disc> --vr` runs any mode - title, GT-mode menus, race, panels, movies, race screens, the arcade disc's
menus - inside an OpenXR session and shows it on a world-locked quad 2 m ahead. Nothing of the game's own code knows
about it: every screen still draws through `GameWindow` into the same 2D / 3D path.

| New / changed file | Contents |
|---|---|
| `src/platform/xr/xr_session.{h,cpp}` (library `gt2xr`) | `gt2::xr::Session`: loads `openxr_loader.dll` at run time, instance with the extensions the runtime offers, HMD system, `XR_KHR_vulkan_enable2` (`xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` -> a Vulkan 1.3 device with `dynamicRendering` and `multiview` for M2), session, the state machine over `xrPollEvent` (`xrBeginSession` on READY, `xrEndSession` on STOPPING, quit on EXITING / instance loss), LOCAL + VIEW reference spaces (STAGE when offered, `SessionInfo::stageSpace`), the cinema quad's swapchain, `BeginFrame` / `SubmitFrame` around `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame`, `XR_FB_display_refresh_rate` (rates enumerated, rate changes followed through `XrEventDataDisplayRefreshRateChangedFB`) and `XR_EXT_performance_settings` (CPU / GPU SUSTAINED_HIGH) when present, and `XR_KHR_win32_convert_performance_counter_time` to put `predictedDisplayTime` on the steady clock. Only `XR_KHR_vulkan_enable2` is mandatory; everything else is optional and logged. Every failure throws with the reason - **without a loader or a runtime `--vr` stops with that message, it never falls back to the desktop path**. |
| `src/gt2view/vk_scene_renderer.{h,cpp}` | a second constructor `VkSceneRenderer(VkContext&, VkExtent2D, VkFormat)`: **offscreen mode**. Instead of a window swapchain the renderer owns one colour image of that size and format, which `images_[0]` / `views_[0]` point at, so the recording, the graphics options' scene targets (render scale / MSAA) and `--shot` all take exactly the window path's code. `Draw` leaves it in `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` and does not present; `Offscreen()`, `OffscreenImage()`, `Extent()` and `WaitFrame()` are what the session copies from. |
| `tools/gt2game/game_window_xr.cpp` | the `WindowBackend` of `--vr`. It owns the session, a `VkContext(VkContext::Existing{...})` on the runtime's Vulkan objects, an offscreen `VkSceneRenderer` at 1280 x 960, and a plain Win32 window (`CreateInputWindowBackend`) for the keyboard, the focus and the controllers (M5 replaces that with XR actions). `BeginRenderFrame` / `EndRenderFrame` wrap the renderer's frame in a compositor frame; `SleepUntil` keeps submitting the last image while the field's time has not come; `VBlankTiming` / `DisplayTime` / `XrPaced` report the runtime's clock. An optional desktop mirror (`GT2_XR_MIRROR=1`, off by default) blits the same image into a swapchain on that window, never with FIFO presentation. |
| `tools/gt2game/game_window.{h,cpp}` | `WindowBackend` gained `BeginRenderFrame` / `EndRenderFrame` (empty on Windows: `Draw` presents by itself), `XrPaced`, `DisplayTime`; `GameWindow` gained `BeginPresent` / `FinishPresent` (the same pair around one `Draw`) and now waits through the backend's `SleepUntil`. `SetVrMode(on, deterministic)` makes every `GameWindow` of the program an XR one, `SetWindowNoFocus` is the focus rule below, `kCinemaWidth / kCinemaHeight` = 1280 x 960. |
| `tools/gt2game/game_window_win32.cpp` | `CreateInputWindowBackend` = the same window without a renderer (`Renderer()` throws). Two fixes the checks needed: the process is made **per-monitor DPI aware** (without it Windows shrinks a window that does not fit the *scaled* desktop - a 1280 x 960 client is impossible on a 250 % 4K screen - so `--window 1280x960` could not be compared with the quad; the rendered frame is the client area either way, so no capture changes), and an automated run's window is **shown without being activated** (`SetWindowPos(..., SWP_NOACTIVATE | SWP_SHOWWINDOW)`, not `ShowWindow`, whose first call in a process obeys the launcher's `STARTUPINFO`). |
| `tools/gt2game/game_main.cpp` | `--vr` and `--xr-deterministic` (the latter implies `--vr` and the deterministic race path); in VR the window size is forced to the quad's 1280 x 960 so the 2D builders get their 4:3. It also marks a run as automated - a key script, a screenshot, `--fast`, `--headless`, a frame limit, any `*-check` / `*-compare` - which is what suppresses the window activation (or `GT2_NO_FOCUS` in the environment). |
| `tools/gt2game/race_view.cpp` | an XR branch of the display-rate loop (`window.XrPaced()`): one frame per display period, each **built after `xrWaitFrame` for that frame's `predictedDisplayTime`**, until a frame reaches past the next field's time. That is 72 / 90 compositor frames per second out of 60 fields per second, with the simulation still stepping at the original's 30 Hz. (This is most of what M3 was going to add; M3 is left with the loading gaps and the same branch in `split_race.cpp`, which still presents one frame per field.) |
| `CMakeLists.txt` | `gt2xr` (OpenXR headers from `third_party/openxr/include`, no loader linked); the search for a local Khronos `openxr_loader.dll` moved from `tools/xrsim/CMakeLists.txt` to the root (cache variable `XRSIM_OPENXR_LOADER`), so `gt2game` and `xrsim_test` use the same build; it is copied next to `gt2game.exe` and compiled in as the fallback path `GT2_OPENXR_LOADER_PATH`. |

How the frame works. The 2D / 3D builders draw the field into the offscreen 1280 x 960 image in `VK_FORMAT_*_UNORM`
(the desktop window's format, so the bytes are the desktop frame's); the quad swapchain is the `*_SRGB` twin of that
format and the frame is copied into it with `vkCmdCopyImage` (size-compatible formats, no conversion), which is why
the runtime's capture of the quad is the desktop PNG bit for bit. The quad is `XrCompositionLayerQuad` in LOCAL space,
2.56 m wide (1.92 m high), 2 m ahead, `XR_EYE_VISIBILITY_BOTH` - both eyes see the same image, M2 replaces it with the
stereo projection layer. Its pose is latched from the head's yaw and height on the first rendered frame (VC's theater
anchor) and again after `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` (`Session::RecenterQuad`). No background
layer is submitted: the blend mode is OPAQUE, so the compositor shows black around the screen.

Pacing. `xrWaitFrame` is the clock. In the race the loop above presents once per display period; on the 2D screens the
field stays at the original's 60 Hz and `SleepUntil` submits the last image again for every period that ends before the
next field is due (72 fields' worth of frames from 60 fields at 72 Hz, none of the compositor's periods skipped).
`--xr-deterministic` switches all of that off: no pacing, exactly one compositor frame per field, so the runtime's
frame N is the game's field N and a run can be compared frame by frame with the desktop one.

Running it under xrsim (`docs/research/xrsim.md`; build both with `cmd /c build.cmd build_m1`):

```
$env:GT2_NO_FOCUS   = '1'                                            # automated run: do not take the keyboard
$env:XR_RUNTIME_JSON = 'C:\Dev\gran-turismo2-pc\build_m1\tools\xrsim\xrsim_runtime.json'
$env:XRSIM_SCRIPT    = 'C:\Dev\gran-turismo2-pc\tests\xr\session.xrsim'
$env:XRSIM_OUT       = 'C:\Dev\gran-turismo2-pc\work\xr\session'
& C:\Dev\gran-turismo2-pc\build_m1\gt2game.exe "<sim disc>.bin" --title --no-sound --vr
```

All of M1's checks at once (captures and logs under `work\xr`, nothing in the repository):

```
powershell -NoProfile -File C:\Dev\gran-turismo2-pc\tests\xr\m1_checks.ps1 [-Build build_m1] [-Disc <sim disc>.bin]
```

`tests\xr\` holds the scripts it uses - `session.xrsim` (state sequence and runtime exit), `title_capture.xrsim`,
`menu_capture.xrsim`, `race_capture.xrsim` (one `capture` each), `race_72hz.xrsim` / `race_90hz.xrsim` (20 s of racing)
- and `compare_png.ps1`. xrsim itself needed no change for M1.

Verified (build `build_m1`, warnings = errors, clean; RTX 4090, 2026-09-20; `m1_checks.ps1` 12 of 12 PASS):

- (a) session: `IDLE READY SYNCHRONIZED VISIBLE FOCUSED VISIBLE SYNCHRONIZED STOPPING IDLE EXITING` on the runtime's
  exit request, exit code 0, 361 compositor frames for 300 title fields at 72 Hz with 0 skipped;
- (b) cinema quad = desktop, pixel for pixel at 1280 x 960: title field 200, GT-mode menu (world map) field 120, race
  field 400 (Seattle, 6 cars) - the runtime's `f000NNN_l0_quad.png` equals `--window 1280x960 --shot-at`'s PNG;
- (c) 72 Hz: 1442 compositor frames in 20 s, 0 skipped, the game's own `--frame-log` 72.0 fps (60 of them in-between
  frames per 5 s) and 29.9-30.1 simulation steps/s; 90 Hz: 1802 frames, 0 skipped, 90.0 fps, 150 in-between per 5 s,
  29.9-30.2 steps/s. Repeated runs occasionally show 1-3 skipped frames out of ~1440 - a single 40 ms hitch of the PC
  (visible in the frame log as one long frame), not of the pacing; the check allows 0.1 %.
- the Vulkan validation layer (`GT2_VK_VALIDATION=1`) reports no error and no warning over a whole VR run;
- `--vr` without a runtime: `error: OpenXR: no runtime available (...)`, exit 1, no window and no fallback;
- desktop unchanged: `gt2verify` race_demo 265 ok / license_race 249 / title 55 / arcade_race 250, 0 FAIL, exit 0;
  `--selftest --cars 6` step 300 = 137.8 km/h, rpm 6126, gear 3, `selftest: 0 failure(s)`; `--vanilla --shot` race,
  title and menu PNGs byte-identical to `C:\Dev\gt2-play\build\gt2game.exe`'s (SHA-1 F4A46A41..., 209AD2E2...,
  BD819E53...). NB: a fresh build directory has no `saves\settings.txt`, so the HUD falls back to mph and the race
  frame differs for that reason alone - copy the settings file before comparing.

What M2 plugs into. `Session` already creates the Vulkan device with `multiview` and holds LOCAL / VIEW / STAGE; it
needs a second swapchain (array size 2 colour + a depth swapchain for `XR_KHR_composition_layer_depth`),
`xrLocateViews` at `predictedDisplayTime`, and an `XrCompositionLayerProjection` next to (or instead of) the quad -
`SubmitFrame` is the one place that builds the layer list. On the renderer's side the offscreen target becomes a
`RenderTarget` with `layers = 2` over the swapchain's images, which `BeginTargetRendering` already takes.
`race_view.cpp`'s XR branch already builds each frame for its own display time, so the stereo work is the draw list
(`DrawItem::space`, the stereo shader) and not the loop. Not done in M1 and left for later: XR input (M5 - the
keyboard of the desktop window is the only input), the loading-time frame loop (M4), `split_race.cpp`'s XR pacing
branch, and a stereo or 3D view of anything (M2).

### 1.3 M2 done (2026-09-20): the race in real stereo, head-tracked

`gt2game <disc> --vr` now draws the race itself as an `XrCompositionLayerProjection`: two eyes, per-eye asymmetric
field of view from `xrLocateViews`, reversed Z with an infinite far plane and a near plane of 0.05 m, and the player's
head moving the camera inside the world the original's race camera puts it in. Everything else - title, GT-mode menus,
the race overlay's own menus, panels, movies, race screens - stays on M1's mono cinema quad, and so do the HUD and the
rear-view mirror inside the race (they are flat in both eyes until M4 gives them their own quads).

**The draw list is built once for both eyes.** `race_view.cpp buildFrame` no longer multiplies the whole world ->
clip matrix into every `DrawItem`; it builds the list in the reference space "world - refEye" and says what each item
is through the new `DrawItem::space`:

| space | what `DrawItem::mvp` holds | what the renderer does with it |
|---|---|---|
| `kSpaceWorld` | object -> reference space (`T(-refEye) . model`) | `worldVP[eye] . mvp` |
| `kSpaceSky` | the backdrop, the same way | `skyVP[eye] . mvp` - the eye translation is dropped, so the backdrop sits at infinity and is **identical in both eyes** |
| `kSpaceScreen` | the whole world -> clip matrix (today's 2D path) | used as it is - the HUD, the panels and the rear-view mirror, the same in both eyes |

On the desktop no stereo views are set and the item's matrix is used exactly as it is, so the window path keeps its
former arithmetic to the bit (`--vanilla` shots are unchanged, see the numbers below). The CPU work runs **once**, from
the rig's mid eye: the backdrop's centre, the course's render list and scenery LOD, the smoke billboards' axes, the
car's reflection axes and the projection distance (the original's `H`, unchanged).

| New / changed file | Contents |
|---|---|
| `src/platform/xr/vr_rig.{h,cpp}` (in `gt2xr`) | the rig, pure arithmetic with no OpenXR and no Vulkan types (it compiles for Android as it is). `Build(camera, eyes, settings)` returns `world_from_eye[v] = C . H . S . local_from_eye[v]` and the two matrices per eye: **C** the original's interpolated race camera (columns right, up, back - the OpenXR convention, 1 unit = 1 m, so there is no axis conversion anywhere), **H** the horizon lock (the camera slerped towards its own yaw-only frame by a percentage: pitch and roll stripped, yaw kept), **S** the seat offset in the levelled camera's axes (the driver view's 0.8 m above the car's matrix is already part of C, so the default is zero), **local_from_eye** what `xrLocateViews` reported, moved into the recentred origin. `Recenter` latches the head's yaw and its horizontal position (height, pitch and roll are kept, so leaning and crouching still move the eyes) on the first frame and after `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING`. The head pose is never interpolated. `OriginalFov` recovers the original camera's frustum from its clip matrix by solving `rows0 = A right + B back`, `rows1 = C up + D back` exactly (the camera's axes are s16 quantised to 4096 = 1 - assuming they were orthonormal costs about 1e-4 of the field of view, which is a visible tenth of a pixel; it was the whole difference in check (a) below). |
| `src/gt2view/shaders/scene_stereo.vert` | the stereo vertex shader, compiled twice from one source: with `GT2_MULTIVIEW` the eye is `gl_ViewIndex` (one pass, `VkRenderingInfo::viewMask` 0b11), without it the `eye` push constant (one pass per array layer). Both read the same `Views` uniform block (set 0, binding 2: `worldVP[2]`, `skyVP[2]`) and both produce identical images. `scene.vert` and `scene.frag` are untouched, so the desktop pipelines are the same SPIR-V as before. |
| `src/gt2view/vk_scene_renderer.{h,cpp}` | `DrawSpace` / `DrawItem::space`, `StereoViews`, `SetStereoViews`, `CreateStereoTarget(extent, format, multiview)` (a two-layer colour image, a two-layer depth image and, with MSAA, a two-layer multisampled colour resolved in the pass) and `DrawStereo(items, shot, sceneItems)`, which records the list once (multiview) or once per array layer. `CreateImage` gained an array-layer count and a per-layer view; the whole frame - scene and 2D layers - goes into one pass per eye, which is exactly what the window path does when no render scale or MSAA is asked for. `--shot` in a stereo frame writes the left eye. |
| `src/platform/xr/xr_session.{h,cpp}` | `CreateStereoSwapchains(w, h)` (one colour swapchain of `arraySize` 2 in the sRGB format whose `*_UNORM` twin the game renders into - so the compositor, and the runtime's capture, get the bytes the game drew - plus a `D32_SFLOAT` depth swapchain when the runtime offers `XR_KHR_composition_layer_depth`), `LocateViews` (the head and both eyes in LOCAL at `predictedDisplayTime`), `SubmitStereoFrame` (the two-layer images copied into the swapchain images, then `XrCompositionLayerProjection` with `XrCompositionLayerDepthInfoKHR` - reversed Z with an infinite far plane is `minDepth` 0, `maxDepth` 1, `nearZ` = infinity, `farZ` = the near plane), and `TakeRecenter`. The projection layer carries the poses and fields of view **the runtime reported**, in LOCAL space - not the rig's world poses: the rendered image is what that eye sees, and the compositor's reprojection must work on the real head. |
| `tools/gt2game/game_window.{h,cpp}` | `WindowBackend::StereoAvailable / StereoAspect / BeginStereoScene / DrawStereoScene / CancelStereoScene`, `GameWindow::BeginStereoScene` (run between `BeginPresent` and the frame's build) and `WillPresent`; `EndFrame` / `FinishPresent` record a begun stereo frame into the stereo target instead of the mono image. `VrOptions` / `SetVrOptions` hold the VR settings for the process. |
| `tools/gt2game/game_window_xr.cpp` | creates the stereo swapchains and the stereo target, runs the rig per compositor frame (locate -> recentre -> `vr::Build` -> `SetStereoViews`), submits the projection layer in `EndRenderFrame`, repeats it unchanged for an idle frame (the pose it was rendered for is what the compositor's reprojection expects), shows the left eye in the optional desktop mirror, and writes `--vr-pose-log`. `BeginRenderFrame` is idempotent, because the race view opens the compositor frame itself before it builds a stereo one. |
| `tools/gt2game/race_view.cpp` | the `space` tags, the reference-space `vp`, the mid-eye camera for the CPU work, the eye image's aspect for the 2D layers, and `beginStereo(alpha)` in both frame loops (the display-rate one and the frame-locked one of `--xr-deterministic` / `--shot`). The frame log's `build_ms` in VR now measures the CPU work alone (it used to include the `xrWaitFrame` block). |
| `src/game/shell/title_options.{h,cpp}` | `VrSettings` in `settings.txt`: `vr_stereo`, `vr_multiview`, `vr_horizon_lock`, `vr_world_scale`, `vr_render_scale`, `vr_near_mm`, `vr_seat_x/_y/_z`. |
| `tools/gt2game/game_main.cpp` | `--vr-mono`, `--vr-multiview 0|1`, `--vr-horizon 0..100`, `--vr-world-scale P`, `--vr-render-scale 50..150`, `--vr-near-mm N`, `--vr-seat X Y Z` (millimetres), and for the checks `--vr-ipd M`, `--vr-original-fov`, `--vr-eye WxH`, `--vr-pose-log <csv>`. |

Defaults: **two passes** (`vr_multiview=0`; the user's Gothic measurements had multiview 11-22 % slower on an Adreno
740 for few draws - M7 measures it on the Quest), horizon lock 60 %, world scale 1.0, near 0.05 m, no seat offset,
render scale 100 % of the runtime's recommended per-eye image. `--vr-mono` (or `vr_stereo=0`) puts the race back on
M1's cinema quad. The graphics settings' `render_scale` does not apply to a stereo frame - `vr_render_scale` sizes the
eye image instead; `msaa` does apply (the depth layer is then left out, because a multisampled depth image cannot be
handed to the compositor as it is).

Running the checks (build both with `cmd /c build.cmd build_m2`; captures and logs under `work\xr`, nothing in the
repository):

```
powershell -NoProfile -File C:\Dev\gran-turismo2-pc\tests\xr\m2_checks.ps1 [-Build build_m2] [-Disc <sim disc>.bin]
```

`tests\xr\` holds what it uses: `stereo_zero.xrsim` (IPD 0), `stereo_drive.xrsim` (a driving frame, symmetric equal
fields of view so that the only difference between the eyes is the parallax), `head_yawl / head_yawr / head_pitch /
head_lean.xrsim` (a fixed head pose from frame 100 - the rig recentres on the FIRST frame, so a pose held from frame 0
would simply become the new origin), `stereo_head.xrsim` (a continuous sweep), `stereo_72hz / stereo_90hz.xrsim`, and
the analysis scripts `stereo_image.ps1` (differing pixels, per-band horizontal disparity, rectangles, and the
left | right | difference PNG) and `rig_check.ps1` (the rig's formula against the head pose the runtime reported).
`m1_checks.ps1`'s race case now passes `--vr-mono` (that check is about the cinema quad) and still passes 12 of 12.

Verified (build `build_m2`, warnings = errors, clean; RTX 4090, 2026-09-20; `m2_checks.ps1` 26 of 26 PASS):

- **(a) the stereo path is the original camera.** With IPD 0, an identity head pose, horizon lock 0 and the original
  camera's own frustum (`--vr-ipd 0 --vr-original-fov --vr-horizon 0 --vr-eye 1280x960`), the two eye captures of race
  field 400 are **pixel-identical**, and the left eye differs from the desktop `--window 1280x960 --shot 400` frame in
  **340 of 1 228 800 pixels (0.028 %)** - single-pixel edges, the float matrix path alone (the desktop folds the whole
  world -> clip matrix into every item, VR multiplies the eye's view-projection with the item's reference-space
  matrix). The near plane makes no difference (0.05 m: 340 pixels, 0.10 m: 342).
- **(b) the eyes really differ, and nearer geometry shifts more.** Eye separation 0.064 m, symmetric +-45 degrees over
  1280 pixels (so a point at distance d shifts 640 * 0.064 / d pixels), race field 700 at Seattle with 6 cars, HUD off.
  Chase 1, horizontal disparity by row band (960 rows in 12 bands, best-matching integer shift):

  | rows | 0-79 | 80-159 | 160-239 | 240-319 | 320-399 | 400-479 | 480-559 | 560-639 | 640-719 | 720-799 | 800-879 | 880-959 |
  |---|---|---|---|---|---|---|---|---|---|---|---|---|
  | shift, px | 4 | 3 | 3 | 2 | 1 | 1 | 2 | 9 | 13 | 12 | 16 | 19 |

  4 px at the top are the overhead wires (about 10 m away: 640 * 0.064 / 10 = 4.1 px), 1-2 px around the horizon
  (20-40 m), 19 px at the bottom of the picture (the road about 2.2 m from the eye). The driver view, whose road starts
  about a metre away, reaches **39 px** in the same bottom band against the chase view's 19. The player's car itself:
  **12 px in Chase 1** (the camera 5.4 m behind the car's origin, so about 3.4 m from its rear) against **9 px in
  Chase 2** (6.8 m / 4.5 m) - 12 / 9 against the expected 4.5 / 3.4.
- **(c) the backdrop has no parallax.** A 230 x 50 patch of the backdrop's **cloud layer**, (570, 330)-(800, 380) of
  the same frame, is **byte-identical between the eyes: 0 of 11 500 pixels differ** - and still 0 when the eye
  separation is raised to 0.200 m, which nothing at a finite distance survives. The patch is textured, not a flat
  colour (the check prints its contrast: 88 of 255), so "0 differing pixels" means something there; a 440 x 130 patch
  of plain sky above it, (500, 10)-(940, 140), contrast 45, is 0 as well. The track band below (rows 700-959, full
  width) differs in **81.9 %** of its pixels at 0.064 m and **85.6 %** at 0.200 m. Over the whole frame 42.5 % of the
  pixels differ.
- **(d) head tracking.** Four scripted poses at the same race field change the picture: yaw +90 deg 78.3 % of the
  pixels, yaw -90 deg 68.8 %, pitch -30 deg 91.4 %, a 0.30 m lean 52.7 %. Over a 999-frame sweep (yaw -90 -> +90,
  pitch -35 deg, a lean with roll) the rig's logged output matches `C . H . S . head` recomputed from the head pose
  **xrsim itself logged** (STAGE minus the LOCAL height): the head the rig used 1.9e-16 m, the mid eye 7.0e-6 m, each
  eye 8.0e-6 m, each eye's orientation 7.6e-4 as the largest element of the 3x3 difference (the original camera's axes
  are s16 quantised at 1/4096 = 2.4e-4, which is where that comes from), and the two views submitted in the projection
  layer stay centred on the runtime's head to 5e-7 m. Tolerance of the check: 5e-4 m and 2e-3 for the rotation.
- **(e) multiview = two passes**, pixel for pixel, in both eyes.
- **(f) pacing.** Seattle, 6 cars, 20 s: at 72 Hz **1442 compositor frames, 0 skipped**, 72.0 fps, 29.9-30.1
  simulation steps/s; at 90 Hz **1802 frames, 0 skipped**, 90.0 fps, 29.9-30.0 steps/s. The CPU cost of building one
  frame's draw list (measured after `xrWaitFrame` returns): median **1.39 ms**, p95 1.77, max 4.6 at 72 Hz and median
  1.32 ms, p95 1.56 at 90 Hz; recording and submitting it (two passes, the copy into the swapchain and `xrEndFrame`)
  about 2.0 ms. NB: the game writes its log **unbuffered**, so the check must not read it through a PowerShell
  pipeline - one `Write-Output` per `printf` blocked the game long enough to miss 13-33 compositor frames of 1800 at
  90 Hz, which is exactly what this check measures. `m2_checks.ps1` runs the game with `Start-Process
  -RedirectStandardOutput` into a file instead.
- the Vulkan validation layer (`GT2_VK_VALIDATION=1`) reports no error and no warning over a stereo run, in both the
  two-pass and the multiview path, with `--msaa 4` (the multisampled colour resolved in the pass).
- **(g) the desktop is unchanged**: `gt2verify` race_demo 265 rows ok / license_race 249 / title 55 / arcade_race 250,
  0 mismatches, exit 0; `--selftest --cars 6` step 300 = 137.8 km/h, rpm 6126, gear 3, `selftest: 0 failure(s)`;
  `--vanilla --shot` race / title / menu PNGs byte-identical to `C:\Dev\gt2-play\build\gt2game.exe`'s (SHA-1
  F4A46A41..., 209AD2E2..., BD819E53...). NB: copy that build's `saves\settings.txt` next to the new executable first,
  or the HUD falls back to mph and the race frame differs for that reason alone.

To see the stereo by eye: `work\xr\m2_stereo_left_right_diff.png` (3840 x 960) is left eye | right eye | their
difference times 4 of a Seattle race frame - the sky is black in the difference panel (no parallax), the wires, the
trees, the road and the car are not.

What M3 / M4 plug into. `Session::SubmitStereoFrame` and `VkSceneRenderer::DrawStereo` are the one place a frame
becomes a projection layer, `vr::Build` the one place a pose becomes matrices, and `GameWindow::BeginStereoScene` the
one call a screen makes to ask for a stereo frame. Not done in M2 and left for later: the HUD and the rear-view mirror
on their own quads (M4 - today they are drawn flat into both eye images, in the aspect of the eye image, which is only
right when that image is 4:3), the loading-time frame loop (M4), `split_race.cpp`'s XR branch (M3), two frames in
flight and the queue mutex (M7), the render scale of the graphics settings inside a stereo frame, XR input (M5), the
per-car seat offset file `saves/vr_seats.txt` (M8 - M2 has one seat offset for the whole session, `vr_seat_*`), and a
3D view of the start fly-around, the trackside replay cameras and the cuts (section 2: they are stereo today, with no
fade on a `CameraCut`).

## 2. Rendering in VR

- Stereo from the original camera: its view columns are right, up, back in 16.16 m (OpenXR convention, 1 unit = 1 m).
  world_from_eye[v] = C(alpha) . H . S . local_from_eye[v]: C = interpolated original camera (InterpolatedProjection),
  H = horizon lock (pitch / roll stripped by a percentage with a low-pass, yaw kept), S = seat offset (driver view: car
  matrix + 0.8 m as the original + per-car offset in saves/vr_seats.txt; chase views as the original), local_from_eye =
  xrLocateViews relative to the recentered origin. Head pose is never interpolated. Per-eye asymmetric XrFovf, reversed-Z
  infinite far, near 0.05 m, IPD from the runtime, world-scale slider (default 1.0). Start fly-around, trackside replay
  cameras and cuts: cinema quad (mono) by default, optional 3D with a fade on CameraCut.
- Draw list once for both eyes: buildFrame takes a ViewSet; stereo passes vp = T(-refEye) (object -> reference-relative
  world); DrawItem::space: kWorld -> eyeVP[v] . mvp, kSky -> rotation-only eyeVP[v] (backdrop at infinity, no parallax),
  kScreen -> 2D. scene.vert STEREO variant (VP from a UBO by gl_ViewIndex or an eye push constant); desktop SPIR-V unchanged.
  CPU work once from the mid-eye pose (smoke billboards, reflection axes, LOD, projection distance). Render list stays on
  the original camera's chunk; VR defaults draw_distance to ~300 m.
- Multiview and two passes both (vr_multiview 0/1), dynamic rendering (viewMask 0b11 or two vkCmdBeginRendering); default
  two passes (Gothic: multiview 11-22 % slower on Adreno 740 for few draws), measured at M7. One array swapchain arraySize 2
  R8G8B8A8_SRGB + D32 depth swapchain (XR_KHR_composition_layer_depth).
- Render scale 100 % of recommendedImageRect (50..150); 4x MSAA transient, resolved in the pass, storeOp DONT_CARE; no
  foveation at first (FDM was a dead end in Gothic).
- Renderer: two frames in flight (per-frame command buffer / fence / UBO), double-buffered per-frame vertices (smoke,
  reflection UVs), a queue mutex around xrEndFrame and swapchain acquire / release.
- HUD: Hud::Build(..., 4/3) into a 1280 x 960 premultiplied quad swapchain; LOCAL-space quad at dashboard position (0.9 m
  forward, 0.35 m below the eyes, 0.6 m wide, tilted); option head-locked.
- Mirror: camera::MirrorCamera into its own 480 x 128 target, LOCAL quad at the top of the windscreen (later: planar mirror,
  HP quest_planar_mirror.cpp).
- Menus / title / race screens / movies / split race: the existing 2D builders at 4/3 into a 1280 x 960 cinema quad,
  world-locked 2 m ahead, 2.56 m wide, yaw latched on entry (VC setTheaterMode anchor); re-rendered only on a new field.
- Loading: LoadingScope RAII keeps the XR frame loop alive from a helper thread (last cinema image + spinner quad).

## 3. Input

XrPadDevice fills Ps1PadFrame so menus and the race use the original's pad path unchanged. Stick mode (type 7, as the XInput
mapping): A/B/X/Y = Cross/Circle/Square/Triangle, grips L1/R1, triggers L2/R2 + pressure (triggerPedals), left stick LX/LY
(+ D-pad in menus), right stick RX/RY, Menu = Start, stick clicks Select / R3; rumble -> XR haptics. Wheel / motion modes
present as a neGcon (type 2: twist steering, I throttle, II brake - calibrated, no DualShock dead zone): IMMERSIVE virtual
wheel with the grips (+-80 deg, 45 deg max step per frame; QuestDrivingVR.cpp), MOTION aim-pose yaw against a reference
taken at first throttle (3 deg dead zone, 30 deg fine zone, 90 deg max); math in a unit-tested src/platform/input/vr_steering.*.
Recenter: XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING + in-game chord (both stick clicks 1 s). Seated LOCAL space.
Per-car seat adjustment (right grip + stick). No hand tracking in the manifest.

## 4. Audio and data

audio_device_aaudio.cpp: SHARED, LOW_LATENCY, float, stereo, 44100 requested (resample to the stream rate in the callback),
data callback -> Mixer::Mix; stop / restart on focus loss / resume. Data: the user's disc image(s) in
/sdcard/Android/data/<pkg>/files/GT2/ (identified by the EXE SHA-1 via exe_profile), saves in internalDataPath/saves;
first launch creates files/GT2/ under the app's UID and shows the path on the cinema quad; tools/android/push-disc.ps1.
No game data in the APK.

## 5. Android build

android/ (Gradle project) with the user's toolchain (C:\Dev\android-toolchain): AGP 8.7.3, Gradle 9.3.1, JDK 21, SDK 35,
minSdk 32, NDK 27.2.12479018, CMake 3.22.1, arm64-v8a, -std=c++20, c++_static, openxr_loader_for_android 1.1.43
(C:\Dev\openxr_loader_for_android-1.1.43.aar), shaders by the NDK glslc. Native CMake adds the repo root with
GT2_PLATFORM_ANDROID=ON, gt2game SHARED, native_app_glue. Manifest: NativeActivity, lib_name gt2game,
com.oculus.vr.focusaware, IMMERSIVE_HMD + com.oculus.intent.category.VR, vr.headtracking, vulkan.level 1,
vulkan.version 0x401000, singleTask, landscape, allowBackup false, no supportedDevices, no hand tracking. Lifecycle:
the looper pumped in GameWindow::BeginFrame, block while the session is not running, ResetPacing on resume, _exit(0).
Refresh from xrEnumerateDisplayRefreshRatesFB (72 / 90 / 120, default 90), CPU SUSTAINED_HIGH, GPU level setting, thread
hints. Scripts: tools/android/build-and-install.ps1, tools/android/logcat.ps1 (tag GT2.Quest).

## 6. Verification

xrsim on Windows (XR_RUNTIME_JSON = build_xrsim\tools\xrsim\xrsim_runtime.json, scripts tests\xr\*.xrsim, captures only
under work\xr\): (1) cinema quad = desktop --shot-at PNGs of the same fields at 1280 x 960, pixel for pixel
(`--xr-deterministic`: one field per XR frame); (2) ipd 0, identity head, the original's symmetric FOV: both eyes equal and
<= 0.1 % differing pixels vs the desktop race shot; (3) backdrop pixels equal between eyes, multiview = two passes;
(4) scripted head yaw / lean / recenter vs the logged view poses; (5) 72 / 90 / 120 Hz: no skipped frames, alpha step error
~0, 30.0 steps/s, loading gaps <= 2 periods; (6) scripted Touch input drives title -> race -> finish, steering unit tests;
(7) gt2verify 0 FAIL and desktop --vanilla shots bit-identical after every milestone. PC headset via SteamVR / Link by hand.
On device: logcat lines for lifecycle, Vulkan version / features, refresh / perf levels, META performance metrics +
Vulkan timestamps every 5 s; Home / headset off-on / recenter; 20-minute thermal run at 90 Hz.

## 7. Milestones

| M | Scope | Observable check | Effort |
|---|---|---|---|
| M0 | Portability split (keys, paths, audio, input header, VkContext, window backend), Windows only | --vanilla shots bit-identical; gt2verify 0 FAIL | 4 d |
| M1 | xr_session on Windows, --vr, every screen on the cinema quad (mono) | xrsim check 1, state sequence and exit | 5 d |
| M2 | Stereo race: DrawItem::space, stereo shader, depth swapchain, sky at infinity, two passes + multiview | xrsim checks 2-4 (**done 2026-09-20**, section 1.3; two frames in flight moved to M7) | 7 d |
| M3 | Interpolation on predictedDisplayTime, XR pacing branch in race_view.cpp | xrsim check 5 at 72 / 90 / 120 | 3 d |
| M4 | HUD quad, mirror quad, LoadingScope presenter, replays / intro on the cinema quad | quad captures vs desktop HUD / mirror crops; no frame gaps during loads | 4 d |
| M5 | XrPadDevice (stick), recenter, haptics; wheel + motion as neGcon | xrsim check 6; unit tests; PC-headset drive | 6 d |
| M6 | Android project, android_main + lifecycle, AAudio, data dir + disc detection, cinema only | Quest 3: title visible / audible, menus with Touch, Home / resume | 6 d |
| M7 | Quest stereo race + performance (refresh, perf levels, MSAA / scale defaults, multiview vs two passes) | GPU <= ~75 % of the period at 90 Hz, Seattle, 6 cars; decision in modern_graphics.md | 6 d |
| M8 | Comfort / settings (horizon lock, per-car seat, world scale, HUD placement, VR rows on PC SETTINGS) | settings persist; level horizon on a banked corner with lock 100 % (xrsim capture) | 4 d |
| M9 (optional) | Cockpit frame / hands, SGSR, space warp | - | open |

## Open (not determinable from the survey)

Vulkan 1.3 on the user's Quest 3 (all ports target 1.1; fallback 1.1 + VK_KHR_dynamic_rendering, log at M6); Adreno cost
of scene.frag (PS1 VRAM decode per fragment; fallback: decoded RGBA page / CLUT cache); which vertices are rewritten every
frame (decides double buffering); whether a HUD item uses PS1 subtractive blend (not reproducible on a premultiplied quad);
whether 1 unit = 1 m looks right; how far head yaw exposes geometry outside the camera chunk's render list; AAudio at
44.1 kHz; which Windows OpenXR loader to ship; HP's real frame timings.
