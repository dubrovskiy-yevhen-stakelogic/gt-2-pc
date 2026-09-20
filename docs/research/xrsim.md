# xrsim - OpenXR simulator runtime for automated tests (development tool)

xrsim is our own OpenXR runtime for Windows that simulates a stereo HMD with two Touch controllers, so agents (and the
user) can run and check Vulkan OpenXR apps on this PC without a headset, fully scripted and headless. It is a development
tool (`tools\xrsim\`), not part of the game, and generic: any Vulkan OpenXR app (the user's other VR ports too) can run on
it.

What it gives an automated check:

- a real OpenXR runtime behind the official Khronos loader (or loaded directly), activated per process by an environment
  variable - no registry, no admin;
- frame pacing at 72 / 80 / 90 / 120 Hz with a monotonic `predictedDisplayTime`;
- a script that drives head / controller poses, buttons, triggers, thumbsticks, focus loss, exit requests and the
  refresh rate per frame index or per time;
- captures: the submitted projection-layer images of both eyes (and quad layers) read back from the swapchain images
  after `xrEndFrame` and written as PNG;
- a per-frame CSV log (timing, layers, poses) and a text log that names every error the runtime returns and why;
- an optional desktop mirror window (both eyes side by side).

Own code; the OpenXR headers in `third_party\openxr\include` are Khronos (`Apache-2.0 OR MIT`, SPDX lines kept, licence
text in `third_party\openxr\LICENSE`). The PNG compressor is our `src\gt2export\png_deflate.cpp`. The MIT
"OpenXR-Simulator" project (the user's download) was consulted as a reference for the loader negotiation and the
Vulkan interop; no code was copied from it.

## Build

Part of the main CMake project (optional targets, need the Vulkan SDK with glslc, Windows):

```
cmd /c C:\Dev\gran-turismo2-pc\build.cmd build_xrsim
```

Outputs in `build_xrsim\tools\xrsim\`:

| file | what |
|---|---|
| `xrsim_runtime.dll` | the runtime (exports only `xrNegotiateLoaderRuntimeInterface`) |
| `xrsim_runtime.json` | runtime manifest (`library_path: ./xrsim_runtime.dll`), generated next to the DLL |
| `xrsim_test.exe` | self-test / demo client |
| `openxr_loader.dll` | copy of a local Khronos loader build (see below), used by xrsim_test's loader path |

The loader is not vendored. CMake copies an existing local x64 build next to the test: the cache variable
`XRSIM_OPENXR_LOADER` (path to an `openxr_loader.dll`) or, when empty, the first one found among the user's own builds
(`C:\Dev\witcher-vr\vendor\OpenXR-SDK-Source-1.1.58\build-win64\src\loader\Release`, then
`C:\Dev\fighting-force-vr\openxr-sdk\build\...`, `C:\Dev\re3-miami-vr\vendor\openxr-1.1.58\x64\bin`). Without one the
self-test still runs, through the direct path only, and says so.

## Running an app on xrsim

PowerShell (per process; nothing global changes):

```
$env:XR_RUNTIME_JSON = 'C:\Dev\gran-turismo2-pc\build_xrsim\tools\xrsim\xrsim_runtime.json'
$env:XRSIM_SCRIPT    = 'C:\path\script.txt'      # optional
$env:XRSIM_OUT       = 'C:\path\out'             # optional: captures, CSV, log
$env:XRSIM_WINDOW    = '1'                       # optional: mirror window
& C:\path\app.exe
```

The Khronos loader honours `XR_RUNTIME_JSON` (not in elevated processes). Apps that do not use the loader can load
`xrsim_runtime.dll` themselves and call `xrNegotiateLoaderRuntimeInterface` with an `XrNegotiateLoaderInfo`
(interface version 1, API range 1.0 .. 1.x) - the returned `getInstanceProcAddr` gives every other function;
`tools\xrsim\xrsim_test\main.cpp` (`OpenDirect`) is the reference.

### Environment variables

| variable | meaning |
|---|---|
| `XR_RUNTIME_JSON` | (loader) path of `xrsim_runtime.json` |
| `XRSIM_SCRIPT` | automation script (format below); a parse error fails `xrCreateInstance` with `XR_ERROR_RUNTIME_FAILURE` and the reason in the log |
| `XRSIM_OUT` | output directory (created): `xrsim_frames.csv`, `xrsim_runtime.log`, capture PNGs. Unset = no files (captures are then skipped with a log line) |
| `XRSIM_WINDOW=1` | mirror window; closing it acts like the user quitting (runtime exit request) |
| `XRSIM_REFRESH` | initial refresh rate in Hz (overrides the script) |
| `XRSIM_RESOLUTION` | recommended per-eye size, `WxH` (overrides the script) |
| `XRSIM_GPU` | Vulkan physical device index returned by `xrGetVulkanGraphicsDevice(2)KHR` (default: first discrete GPU) |
| `XRSIM_VERBOSE=1` | log also to stderr, incl. every function the loader / app asks for that is not provided |

Everything is read at `xrCreateInstance`, so one process can run several differently configured instances in sequence
(the self-test does).

## Script format (`XRSIM_SCRIPT`)

Plain text, one command per line, `#` starts a comment, case-insensitive. A line starting with `@N` (frame index) or
`@Ts` (seconds) is a timed key; lines without a key are header directives (or keys at frame 0 for channel commands).

- Frame index N = the N-th `xrWaitFrame` of the session (0-based). Time T = seconds of `predictedDisplayTime` since
  frame 0's. Keys are evaluated at each `xrWaitFrame`, the state is valid for that frame's `predictedDisplayTime`
  (`xrLocateSpace` / `xrLocateViews` at a frame's display time see exactly that frame's state; older times see older
  frames, up to 512 frames back).
- Values hold until the next key (step). A key ending in `lerp` interpolates from the previous key of the same channel
  (position linear, orientation slerp, scalars linear). Keys of one channel must be in chronological order.

Header directives:

| directive | default | meaning |
|---|---|---|
| `refresh HZ` | 72 | initial refresh rate (72, 80, 90, 120 are offered through XR_FB_display_refresh_rate; another value is added to the list) |
| `resolution W H` | 1680 1760 | recommended per-eye image size (max = 2x, <= 8192) |
| `ipd M` | 0.063 | eye separation in meters |
| `fov L R U D` | -50 43 45 -52 | left-eye field of view in degrees (right eye mirrored) |
| `local_height M` | 1.6 | height of the LOCAL origin above the floor |
| `stage W D` | 2 2 | STAGE bounds |
| `gpu I` | - | like `XRSIM_GPU` |
| `capture F [F ...]` | - | capture at these frame indices |

Timed / channel commands:

| command | meaning |
|---|---|
| `head [pos X Y Z] [rot QX QY QZ QW \| ypr YAW PITCH ROLL] [lerp]` | head pose in STAGE space (defaults: `pos 0 1.6 0`, identity). Omitted parts keep the previous key's values |
| `left\|right [pos ...] [rot ...\|ypr ...] [lerp]` | controller grip pose in STAGE space (defaults: `pos -0.2 1.3 -0.35` / `pos 0.2 1.3 -0.35`) |
| `left\|right tracked 0\|1` | 0 = controller pose not tracked (action-space locations get flags 0) |
| `left\|right trigger V`, `squeeze V` | 0..1 (`lerp` allowed) |
| `left\|right thumbstick X Y` | -1..1 (`lerp` allowed) |
| `left\|right x\|y\|a\|b\|menu\|system V` | buttons (x, y, menu: left; a, b, system: right) |
| `left\|right trigger_touch\|thumbstick_click\|thumbstick_touch\|thumbrest_touch\|x_touch\|y_touch\|a_touch\|b_touch V` | touches; a pressed button / pulled trigger / deflected stick counts as touched automatically |
| `event focus_loss\|focus_gain` | FOCUSED <-> VISIBLE (actions go inactive, `xrSyncActions` returns `XR_SESSION_NOT_FOCUSED`) |
| `event hide\|show` | VISIBLE/FOCUSED <-> SYNCHRONIZED (`shouldRender` false) |
| `event exit` | runtime-initiated exit: ... -> SYNCHRONIZED -> STOPPING; after `xrEndSession`: IDLE -> EXITING |
| `refresh HZ` | change the refresh rate from this frame on (queues `XrEventDataDisplayRefreshRateChangedFB` when the app enabled the extension) |
| `capture` | capture this frame |

Angles: degrees; yaw about +Y (positive turns left), pitch about +X (positive looks up), roll about +Z, applied
yaw * pitch * roll. OpenXR convention: +Y up, -Z forward, meters.

Example:

```
refresh 90
resolution 1680 1760
@0    head pos 0 1.6 0
@90   head pos 0 1.6 -0.5 ypr 45 0 0 lerp      # walk forward and turn left over 1 s
@0    right pos 0.25 1.2 -0.4
@1.5s right trigger 1
@2s   right a 1
@2.1s right a 0
@3s   event focus_loss
@3.5s event focus_gain
capture 10 90 200
@5s   event exit
```

## Outputs

`xrsim_frames.csv` (one row per frame, written at `xrEndFrame`, or when a frame is discarded / never ended):

| column | meaning |
|---|---|
| `frame` | frame index |
| `wait_call_ns`, `wait_return_ns`, `wait_ms` | when the app called `xrWaitFrame`, when it returned (XrTime ns), blocked time |
| `display_ns`, `period_ns` | `predictedDisplayTime`, `predictedDisplayPeriod` |
| `skipped` | refresh intervals skipped because the app called `xrWaitFrame` after the frame's display time |
| `should_render`, `state` | `XrFrameState::shouldRender`, session state at `xrWaitFrame` |
| `begin_ns`, `end_ns` | `xrBeginFrame` / `xrEndFrame` times |
| `result` | `ok`, `discarded` (a second `xrBeginFrame`), `not_ended` |
| `layers` | submitted layers: `P2` projection (stereo), `P2d` with XR_KHR_composition_layer_depth, `Q` quad, joined by `\|` |
| `captured` | 1 when the frame was captured |
| `head_*` | simulated head pose (STAGE) |
| `v0_*`, `v1_*` | view poses the app submitted in its first projection layer (in that layer's space) |

XrTime is nanoseconds of `QueryPerformanceCounter` (`xrConvertTimeToWin32PerformanceCounterKHR` maps it back exactly
up to the counter resolution).

Captures, per captured frame, in `XRSIM_OUT`:
`f<frame6>_l<layer>_proj_v<view>.png` for each projection view, `f<frame6>_l<layer>_quad.png` for each quad layer. The
image is the layer's `imageRect` of the image the app last released before `xrEndFrame`, from the referenced array
layer. PNGs are RGBA8, display-referred: `*_SRGB` formats are copied bit-exactly, linear formats (`*_UNORM`, 16-bit
float, `A2B10G10R10`) are encoded with the sRGB transfer function (what a compositor shows); alpha is forced to 255 for
projection layers without `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`. The readback is synchronous in
`xrEndFrame` (a few ms at 1680x1760x2); conversion and PNG compression run on a writer thread, flushed when the session
ends (so the files exist once `xrDestroySession` returned). Multisampled and depth swapchains are not captured.

`xrsim_runtime.log`: configuration, session state changes, swapchains, captures, and every error return as
`<function> -> <XrResult>: <reason>` - the first place to look when an app misbehaves on xrsim.

Mirror window: the first projection layer's two views, downscaled on the GPU (blit into an sRGB image, asynchronous
readback, never blocks the frame), side by side. Quad layers are not composited into it.

## API surface

Extensions: `XR_KHR_vulkan_enable`, `XR_KHR_vulkan_enable2`, `XR_KHR_composition_layer_depth`,
`XR_KHR_win32_convert_performance_counter_time`, `XR_KHR_locate_spaces`, `XR_EXT_local_floor`,
`XR_EXT_performance_settings`, `XR_FB_display_refresh_rate`. OpenXR 1.0 and 1.1 instances (`xrLocateSpaces` and
`LOCAL_FLOOR` are core in 1.1). All core 1.0 functions are implemented; functions of extensions the app did not enable
return `XR_ERROR_FUNCTION_UNSUPPORTED` from `xrGetInstanceProcAddr`.

- System: HMD form factor only (handheld -> `XR_ERROR_FORM_FACTOR_UNSUPPORTED`), `PRIMARY_STEREO` only, blend mode
  `OPAQUE` only, max 16 layers, 8192 max image size, up to 4x MSAA swapchains.
- Vulkan: `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` pass the app's create infos through unchanged (xrsim
  needs no extra Vulkan extensions or features; the v1 extension-string queries return empty strings);
  `xrGetVulkanGraphicsDevice(2)KHR` picks `XRSIM_GPU` or the first discrete GPU; the session's physical device must be
  that one. The graphics requirements call is mandatory before `xrCreateSession`
  (`XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING`). Min Vulkan 1.0, max 1.4.
- Swapchains: images are created by the runtime on the app's device (3 images, 1 with `STATIC_IMAGE`), any `arraySize`
  (multiview), mips, samples; formats offered when the device supports them as attachments:
  `R8G8B8A8_SRGB`, `B8G8R8A8_SRGB`, `R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `R16G16B16A16_SFLOAT`,
  `A2B10G10R10_UNORM_PACK32`, `D32_SFLOAT`, `D24_UNORM_S8_UINT`, `D16_UNORM`, `D32_SFLOAT_S8_UINT`. Layout of an
  acquired image: `COLOR_ATTACHMENT_OPTIMAL` (color attachment usage), else `DEPTH_STENCIL_ATTACHMENT_OPTIMAL` (depth
  usage), else `GENERAL` (storage), else `SHADER_READ_ONLY_OPTIMAL` (sampled), else `TRANSFER_DST_OPTIMAL`; the app
  must return it in that layout. `TRANSFER_SRC` usage is always added (readback). Acquire / wait / release follow the
  spec order rules (`XR_ERROR_CALL_ORDER_INVALID` otherwise); `xrWaitSwapchainImage` never blocks (the runtime's own
  GPU work is complete when `xrEndFrame` returns).
- Runtime GPU work (initial layout transitions, capture and mirror copies) is submitted on the app's queue from the
  binding (`queueFamilyIndex` / `queueIndex`) inside `xrCreateSwapchain` / `xrEndFrame`; the app must externally
  synchronize its own use of that queue with these calls, as the spec requires.
- Session: IDLE -> READY at creation; after `xrBeginSession` the first `xrEndFrame` moves to SYNCHRONIZED -> VISIBLE ->
  FOCUSED (one event per step). `xrRequestExitSession` and script `exit` walk down to STOPPING; `xrEndSession` then
  gives IDLE -> EXITING. Wrong-state calls return `XR_ERROR_SESSION_NOT_READY / _RUNNING / _NOT_RUNNING /
  _NOT_STOPPING`.
- Frame loop: frame N's `predictedDisplayTime` is frame N-1's plus the period; `xrWaitFrame` sleeps (high-resolution
  waitable timer + spin) until one period before it; if the app calls it after that display time, the frame moves to the
  next refresh (`skipped` in the CSV). A second `xrWaitFrame` on the same thread without `xrBeginFrame` returns
  `XR_ERROR_CALL_ORDER_INVALID` (it would deadlock); from another thread it blocks as the spec says. A second
  `xrBeginFrame` returns `XR_FRAME_DISCARDED`. `xrEndFrame` validates layers (spaces, swapchains released, rects,
  array indices, poses, projection view count 2, depth info) with the spec's error codes; unsupported layer types
  (cylinder, cube, equirect) -> `XR_ERROR_LAYER_INVALID`.
- Spaces: VIEW = head; LOCAL = origin at `(0, local_height, 0)` of STAGE; STAGE and LOCAL_FLOOR = floor origin;
  STAGE bounds from `stage`. Action spaces locate the controller pose (grip, aim and grip_surface all return the same
  simulated controller pose); an inactive / untracked action space gives location flags 0. Velocities are reported with
  flags 0 (not simulated).
- Input: interaction profiles `/interaction_profiles/oculus/touch_controller` (all Touch components, incl.
  `thumbrest/touch`, `grip_surface/pose`, `output/haptic`) and `/interaction_profiles/khr/simple_controller`
  (`select/click` = trigger > 0.5, `menu/click` = left menu / right system). Other profiles and components not on the
  device -> `XR_ERROR_PATH_UNSUPPORTED`. Bindings may omit the identifier (`.../trigger` -> `.../trigger/value` for a
  float action, `.../click` for a boolean one). The current profile is Touch when the app suggested Touch bindings,
  otherwise simple_controller; `XrEventDataInteractionProfileChanged` is queued at `xrAttachSessionActionSets`.
  Aggregation without a subaction path: boolean OR, float largest magnitude, vector2 longest. Boolean from a float
  source: > 0.5. Haptics are accepted (counted, logged with `XRSIM_VERBOSE`). Action set priorities are ignored.
- Refresh rate: 72 / 80 / 90 / 120 Hz; `xrRequestDisplayRefreshRateFB` with another value ->
  `XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB`, 0 = the configured default. Performance levels are validated and
  logged (no effect).

## Self-test

```
C:\Dev\gran-turismo2-pc\build_xrsim\tools\xrsim\xrsim_test.exe --selftest [--out DIR] [--window] [--no-validation] [--loader PATH]
```

Exit code 0 = all checks passed. Output directory default: `<exe dir>\xrsim_selftest\phaseA|phaseB` (generated script,
CSV, log, PNGs). The test sets `XR_RUNTIME_JSON` / `XRSIM_*` for itself. It enables `VK_LAYER_KHRONOS_validation`
when installed (it validates the runtime's Vulkan calls too, same device) and fails on any validation error.

- Phase A: official Khronos loader -> xrsim (runtime name checked), OpenXR 1.1, `XR_KHR_vulkan_enable2`
  (`xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR`), one multiview swapchain (arraySize 2,
  `R8G8B8A8_SRGB`, Vulkan 1.3 dynamic rendering with `viewMask` 3) + depth swapchain submitted with
  `XR_KHR_composition_layer_depth` + head-locked quad layer, Touch bindings. Script: head pose lerp, controller poses,
  trigger / button / thumbstick values, controller tracking loss, focus loss and gain, runtime exit at frame 200,
  captures at frames 20, 60, 120. The app requests 90 Hz at frame 160. Checks: every rendered eye image pixel-exact
  (1680x1760; clear colour depends on the frame, checkerboard colours on eye + frame) and the quad; action values,
  `changedSinceLastSync`, inactive while unfocused; located VIEW / LOCAL / STAGE / LOCAL_FLOOR / action-space poses and
  views vs the script; time conversion; the exact session state sequence; interaction profile and refresh events;
  a set of error codes (handheld form factor, bad path, unknown profile, bad binding, unsupported format,
  `xrBeginFrame` before `xrWaitFrame`, `xrEndSession` while focused, type mismatch, unsupported refresh rate, function
  of a non-enabled extension); timing at 72 Hz: mean `xrWaitFrame` interval over frames 10-159 = 13.889 ms +- 0.5,
  `predictedDisplayTime` steps exactly 13888889 ns (no skips) for frames 0-159, 11111111 ns after the 90 Hz request;
  CSV rows / display times equal to what the app saw, layers column.
- Phase B: runtime DLL loaded directly (`xrNegotiateLoaderRuntimeInterface`, no loader), OpenXR 1.0,
  `XR_KHR_vulkan_enable` (app-created VkInstance / VkDevice), two `B8G8R8A8_UNORM` swapchains (one per eye),
  simple_controller, script `refresh 90` + `resolution 800 600`, app `xrRequestExitSession` at frame 60. Checks: pixel-
  exact captures with the UNORM -> sRGB encoding, select from the trigger, aim pose, state sequence, 90 Hz timing
  (11.111 ms, steps 11111111 ns).

Result on this PC (RTX 4090, 2026-09-19): 119 checks passed, 0 failed (also with `--window`); measured mean frame
interval 13.8889 ms (max deviation 0.14 ms) at 72 Hz and 11.1111 ms at 90 Hz.

Demo client: `xrsim_test --run [--frames N] [--direct]` runs the same renderer against the environment the caller set
(e.g. with `XRSIM_WINDOW=1` and a script ending in `event exit`); `--frames N` makes it call `xrRequestExitSession`
after N frames.

## Limits (not simulated)

- Vulkan apps only (no D3D11/12 / OpenGL bindings, no headless `XR_MND_headless`); one instance and one session at a
  time (`XR_ERROR_LIMIT_REACHED`).
- Not a compositor: no reprojection, no distortion; captures are per layer (the quad is a separate PNG, not composited);
  the mirror shows the first projection layer only; depth layers are validated but not used.
- Pacing is a simulated display clock on the CPU; it does not wait for the app's GPU work, so GPU-bound frame times
  are only visible as late `xrWaitFrame` calls.
- No velocities, no hand / eye tracking, no passthrough, no visibility mask, no foveation, no `XR_EXT_debug_utils`, no
  cylinder / cube / equirect layers, no controller models; grip, aim and grip_surface poses are identical.
- Performance-settings notifications, instance loss and headset-removal (STOPPING -> READY) are not generated.
- Action set priority and `XrActiveActionSetPrioritiesEXT` are ignored.
