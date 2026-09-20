# Quest 3 standalone ports of the user (C:\Dev) - reference for the GT2 Android VR build

Survey of 2026-09-19 (read-only). Target decided by the user: a STANDALONE Android build on Quest 3 (not PC VR). All of
these ports are the user's own work (the user is their author): their code may be reused directly, no licence review needed.

## Which folders are real Quest builds

- Vice City Quest: `C:\Dev\miamivr-quest` (assembled `C:\Dev\qbuild`) - Vulkan 1.1 multiview, fullest OpenXR layer, car driving.
- Harry Potter VR: `C:\Dev\harry-potter-vr` - a C++20 reimplementation (not an engine port) on Vulkan + OpenXR + AAudio: the
  closest model of GT2's situation.
- Gothic II VR: `C:\Dev\gothic2-vr-source-kit` (+ `gothic2-vr` notes, `gothic2-vr-multiview`) - Tempest Vulkan, deferred.
- GTA SA Quest: `C:\Dev\gta-sa-vr-quest-soure-kit` (+ `gta-sa-vr-quest`, `-fable`) - GLES, injected into the retail APK.
- Not Quest: `C:\Dev\vice-city-vr` (PCVR, D3D12), `vcrd-android` (keystore only), `vcrd-gradle` (Gradle cache).

## Build

Common: NDK 27.2.12479018, CMake 3.22.1, compile/target SDK 35, JDK 21, AGP 8.7.3, arm64-v8a only; toolchain in
`C:\Dev\android-toolchain` (sdk, jdk21, gradle, platform-tools/adb). OpenXR loader: Khronos AAR
`org.khronos.openxr:openxr_loader_for_android:1.1.43` (copy `C:\Dev\openxr_loader_for_android-1.1.43.aar`); nobody links
`C:\Dev\_meta_openxr_sdk` (reference only).
- Vice City: `miamivr-quest\overlay\android\app\build.gradle.kts` (minSdk 32, c++_shared), `tools\build-and-install.ps1`
  (assembleDebug/Release + `adb install -r`), `BUILD_AND_INSTALL.bat`.
- Harry Potter: `harry-potter-vr\android\app\build.gradle` (minSdk 32, `-std=c++20`, c++_static), `docs\BUILDING.md`.
- Gothic: `config\android-toolchain.lock.json`, `tools\build-android.ps1`, `tools\verify-android.py`.
- Manifest essentials (`miamivr-quest\overlay\android\app\src\main\AndroidManifest.xml`): NativeActivity,
  `com.oculus.vr.focusaware`, categories `org.khronos.openxr.intent.category.IMMERSIVE_HMD` + `com.oculus.intent.category.VR`,
  features `vr.headtracking`, `vulkan.level 1`, `vulkan.version 0x401000`; hand tracking deliberately NOT declared (the grab
  gesture triggered Home).

## Graphics / OpenXR

- Vice City `overlay\android\app\src\main\cpp\xr_vulkan_session.{h,cpp}`: `XR_KHR_vulkan_enable2`, Vulkan 1.1, one swapchain
  arraySize 2 (R8G8B8A8_SRGB), render-pass multiview; render scale = percent of recommendedImageRect (default 125 % at 72 Hz,
  fallback 100 %); optional FB display refresh rate, META performance metrics, EXT performance settings, KHR Android thread
  settings. No dynamic rendering anywhere (GT2 uses Vulkan 1.3 dynamic rendering: `VkRenderingInfo.viewMask` for multiview).
- Gothic `engine\common\vr\questxr.{h,cpp}`: 3 swapchains (left, right, HUD), 72 Hz, probes `XR_FB_space_warp`;
  `com.oculus.trade_cpu_for_gpu_amount=1` for GPU level 5.
- GTA SA `native\src\Xr.cpp`: GLES, FB foveation, fixed 72 fps.
- Harry Potter `android\app\src\main\cpp\xr_vulkan_smoke.cpp`: Vulkan 1.1, per-eye rendering, refresh rates from
  `xrEnumerateDisplayRefreshRatesFB` in its menu, GPU boost on.
- Upscalers: SGSR sources `C:\Dev\snapdragon-gsr-official`, `C:\Dev\adreno-sgsr2-sample`; none shipped.

## Frame loop

- Vice City `android_main.cpp`: main thread services the looper; one game thread owns the session: xrWaitFrame -> begin ->
  render callback (steps the game) -> end; game time from predicted display time (wall-clock deltas stuttered); logic keeps
  stepping without frames; `_exit(0)` at the end (Horizon reuses processes); CPU/GPU performance levels separate.
- GTA SA: game thread records both eyes into a ring, separate XR present thread (bounded wait 1.25-6.5 ms), thread hints
  RENDERER_MAIN / RENDERER_WORKER.
- Harry Potter: single thread, dt = difference of predicted display times clamped to 0.05 s.
- For GT2: the display-rate interpolation of `docs\formats\modern_graphics.md` maps directly: frame display time =
  `predictedDisplayTime`, the 30 Hz steps stay on the field clock.

## Input, driving, UI

- All: `/interaction_profiles/oculus/touch_controller`, exposed to the game as a gamepad-like struct (Vice City
  `ControllerInput`, bindings in `xr_vulkan_session.cpp`).
- Driving: `miamivr-quest\overlay\src\vr\QuestDrivingVR.cpp` - DEFAULT (stick), IMMERSIVE (tracked virtual wheel, 80 deg max,
  3 % dead zone), MOTION (aim-pose yaw against a reference taken at first throttle); per-model seat / wheel calibration.
  GTA SA `native\src\Driving.{h,cpp}`, audit `C:\Dev\gta-sa-vr-quest\docs\VICE_CITY_DRIVING_AUDIT.md`.
- Menus: quad layers - Vice City `setTheaterMode` (world-locked cinema quad) + head-locked overlay quad; Gothic `setCinema`;
  GTA SA theater quad + HUD quads. GT2's menus / title / screens are 2D frames: a cinema quad is the natural first step.
- Comfort: snap / smooth / physical turning exist; no vignette implementation found.

## Audio and data

- AAudio callback mixers: Harry Potter `quest_audio.cpp` (closest to GT2's own mixer), Vice City
  `overlay\src\audio\sampman_android.cpp` (I16 stereo, LOW_LATENCY, SHARED). Gothic: openal-soft / OpenSL.
- Game data in app-specific external storage (no permissions), e.g. `/sdcard/Android/data/<package>/files/...`, pushed with
  adb; gotcha: create the folder as the app's UID first, or Horizon gives it to the shell UID. For GT2: the user's disc image
  (.bin) goes there; nothing of the game is shipped in the APK.

## Performance lessons

- `C:\Dev\gothic2-vr\PERFORMANCE.md`: on Adreno 740 multiview was 11-22 % SLOWER for a GPU-bound renderer with few draws
  (pays only when CPU / driver bound with many draws); fragment-density-map foveation with regular attachments is a dead end;
  ~0.8 ms spikes = compositor preemption; outdoors vertex / binning bound (render scale does not help); depth pre-pass pays.
- `C:\Dev\miamivr-quest\RENDERING_NOTES.md` / `PERFORMANCE.md`: ~8.7 ms per frame at 2524x2645 per eye (forward multiview);
  depth storeOp DONT_CARE saves writeback; 2 MiB/frame upload budget.
- `C:\Dev\gta-sa-vr-quest\docs\PERF_TODO.md`: the double scene record was CPU bound; resolution did not matter.
- GT2 expectation: a PS1 scene (thousands of polygons, few textures) is far lighter than these games; the CPU draw-list
  build (~1.4 ms on the PC today) should be built once per frame for both eyes; measure multiview vs two passes on device.

## Not found in the ports

Vulkan 1.3 / dynamic rendering, eye-tracked foveation, 90 / 120 Hz tuning results (all target 72), shipped SGSR, comfort
vignette, Oboe, a non-Touch gamepad path.
