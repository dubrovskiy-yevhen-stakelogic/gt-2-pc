# Quest 3 standalone — 0.2.0

The Android ARM64 build runs locally on Quest 3. Menus, startup artwork and movies use a theatre screen; single-player driving and replays use head-tracked stereo. Two-player split-screen stays on the theatre screen. No streaming PC is required.

For the precompiled release, follow [PLAYER-INSTALL.md](PLAYER-INSTALL.md). Every launch offers a disc picker. Only installed modes appear; the previous disc is highlighted. VR preferences are shared between both discs; each disc retains its own memory card.

## Touch controls

| Control | Default action |
|---|---|
| Left stick | Steering; menu navigation |
| Right trigger | Accelerator |
| Left trigger | Brake; reverse after stopping with automatic transmission |
| A / B in menus | Confirm / back |
| X / A while driving | Shift down / up |
| Y | Change camera |
| Right stick click | Handbrake |
| B / left stick click while driving | Reverse / look back |
| A / B during a movie | Skip |
| Left Menu | Original race pause / Continue |
| Both grips + left Menu | Open VR settings; Menu closes |
| Left stick click + Menu | Alternative VR settings shortcut |
| Headset system recenter | Recenter the driving viewpoint |

In VR settings, stick up/down or X/Y selects rows. Left/right triggers decrease/increase values; release a trigger before the next change. A opens a submenu or confirms an explicit action, B goes back. Horizontal stick motion and grips cannot activate menu entries. Opening settings pauses gameplay/audio; closing settings over an already-paused race keeps that race paused.

## Driving modes and bindings

Controls > Steering and wheel selects **Stick**, **Virtual wheel** or **Motion**. Accelerator and brake remain independent trigger controls. Controls also exposes the driving button bindings, selected steering stick, motion hand and automatic brake-to-reverse.

Virtual wheel supports one or both hands. Grab near a side of the wheel with that hand's grip. Its continuous angle, grip hysteresis, hand transitions and tracking-loss handling follow the MiamiVR implementation. Wheel lock is 80 degrees with a 3% dead zone. Height, distance and radius are adjustable. While a hand holds the wheel, all its fingers remain closed regardless of trigger pressure; releasing it restores tracked finger poses.

Motion: hold the selected controller's grip to establish neutral, then rotate your fist clockwise/counter-clockwise like a wheel. Relative wrist twist around the initial forearm axis controls steering. Releasing the accelerator does not release steering. Release/regrip to establish a new neutral; pausing, tracking loss and recentering clear the reference.

Intro camera lower adjusts only the height of the original starting flythrough: 0–2000 cm in 50 cm steps, default 200 cm. Zero restores original height. The horizontal path, orientation and timing retain the original grid traversal; a floor protects the transition to the driving view.

## Graphics, performance and HUD

VR settings save automatically and are shared between both discs. Eye resolution is 50–200% of the runtime's recommended width and height and requires a restart. Available OpenXR refresh rates are listed dynamically; accepted changes apply immediately. Initial defaults are 72 Hz, 150% resolution, MSAA 2x and the entire detailed course. Existing saved preferences are retained.

MSAA, texture filtering, draw distance, foveation and vibration can be adjusted. Entire course extends all road and scenery to full distance and detail, including large mountains. It selects the authored nearest scenery representation, including empty near entries for distant-only course copies, so simplified asphalt and hills do not overlap the detailed course. When detailed course geometry is present, matching coarse scenery surfaces are replaced by it; independent scenery keeps the selected distance. Stereo frustum culling still skips objects outside both eyes. Foveation Off/Low/Balanced/High changes peripheral fragment shading; the centre, menus, HUD and mirror remain full rate. It is fixed foveation, not eye tracking, and falls back to full-rate shading where unsupported.

The profiler shows APP FPS, average/MAX frame interval, preceding GPU render time, texture-cache coverage, slowest 1% of the last 256 frame intervals, peak interval and culling counts. It counts fresh stereo application frames; it does not count repeated compositor images or physics ticks. Physics remains 30 Hz with interpolated presentation. GPU/CPU performance requests are hints subject to the headset's thermal management.

175% resolution renders about 3.06 times the pixels of 100%. Sustained 175%/90 FPS is not achieved across races in this release. Reduce resolution, MSAA or distance, or increase peripheral foveation while checking image quality and APP FPS. Loading/start hitches can still occur. See [VALIDATION.md](VALIDATION.md) for the checked build and limits.

HUD elements has two pages: map, lap/times, records, gauges, turbo, tyres, mirror, countdown, warnings, messages and replay caption. Each visibility choice persists. HUD, pause panels and mirror share a head-relative plane 2 m ahead with a 4:3 layout, projected through each eye's actual frustum. Independently movable HUD panels are not implemented.

Trees remain flat billboards. Their horizontal axis is computed from the common viewer position, so turning your head in place does not swivel the trees. Walking/driving around a tree still changes its facing direction.

In **HUD elements > Speed units**, use either trigger to switch between **km/h** and **mph**. The choice applies on resume, is saved for both discs and also controls speed records. New Arcade and Simulation profiles default to km/h; existing saved unit choices are preserved.

## First-launch defaults

Each new disc profile uses 150% eye resolution, 72 Hz, MSAA 2x, smooth perspective-correct textures, the entire course with its authored near scenery representations and medium foveation. Multiview is on, horizon lock is 60%, world scale is 100%, the near plane is 50 mm and seat offsets are zero. The stored flat frame cap is 72; VR presentation follows the selected 72 Hz headset rate.

Driving starts in Virtual wheel mode, with the wheel 28 cm below, 38 cm forward and 18 cm in radius. The original start flythrough is lowered by 200 cm. Automatic brake-to-reverse is on; the steering stick is left and the Motion hand is right. Bindings follow the controls table. All HUD elements are on, the profiler is off, vibration is 100%, speed units are km/h, music volume is 240/255 and effects volume is 192/255.

All cheats and unlock overrides start off on both discs; no career progress, cars, money or licences are prewritten. Both profiles remain editable in the game. Startup seeds these settings only when neither settings file exists; updates preserve existing preferences. PC also defaults to km/h; its other defaults are unchanged.

## Cheats and saves

Arcade: unlock all course selections and all cars in the Arcade roster. These are reversible availability overrides, preserving earned results.

Simulation cheats are available in GT Mode menus: gold licences, 99,999,999 credits, event unlocks and the full car catalogue. Triggers page through the catalogue, stick up/down selects and A adds the chosen car free of charge. A full 100-car garage is refused without replacing owned cars. Event unlocks bypass progression checks; class, tyre and power rules remain.

Before the first career cheat write, the original save is retained as `card1.mcd.before-cheats.bak` (or a bare career snapshot when no card existed). A candidate is saved separately, reloaded and checked before replacement. Other files on an existing memory card remain intact. Cheats cannot alter the career during a race.

## Build and signing

Prepare supported discs with the source INSTALL.bat. Android uses the same extracted assets and raw sectors. Required tools: JDK 17+, Gradle 8.13, Android SDK platform 35/build tools 35.0.0, NDK 27.2.12479018, CMake 3.22.1 and Vulkan SDK glslc. Gradle resolves Android Gradle Plugin 8.7.3 and Khronos OpenXR loader 1.1.43. The first build requires network access; `-Offline` uses an existing cache.

```powershell
$env:VULKAN_SDK = 'C:\VulkanSDK\<version>'
.\scripts\build-quest.ps1 -AndroidSdk 'C:\Android\Sdk' -Gradle 'C:\Gradle\bin\gradle.bat'
.\scripts\build-quest-release.ps1 -AndroidSdk 'C:\Android\Sdk' -JavaDirectory 'C:\Java\jdk-21' -Gradle 'C:\Gradle\bin\gradle.bat' -InitializeSigningKey
```

Debug output: `android/app/build/outputs/apk/debug/app-debug.apk`. Public release: `dist/GT2-VR-0.2.0.apk`, ARM64, non-debuggable and signed. The release script stores its private key under `work/signing/release`; retain a private backup and use the same key for future updates. Credentials are protected with Windows DPAPI for the creating account. Use `-InitializeSigningKey` only for a new identity. It never replaces an existing key.

The public version is 0.2.0; Android versionCode is 14 to remain above development build codes. Development and public signing identities differ. An incompatible signature must never be handled by automatically uninstalling the installed application.

```powershell
.\scripts\install-quest.ps1 -Adb 'C:\Android\Sdk\platform-tools\adb.exe' -BothDiscs
```

Source deployment defaults to the debug APK and does not launch it. It installs with `-r`, copies changed game data, verifies raw-disc SHA-256 and tests access through the application's UID. The public player installer passes its release APK explicitly. Data lives under `/sdcard/Android/data/io.github.gt2pc.quest/files`; private debug logs can be retrieved with `adb shell run-as io.github.gt2pc.quest`.

Optional offline media preparation captures the complete, unskippable PlayStation startup and boot audio from a local BIOS dump. It also prepares HD backgrounds/movies and enlarged menu/HUD atlases; see [HD media](HD-MEDIA.md). Smooth + mipmaps filtering reduces distant texture aliasing. Disc publisher/warning artwork and existing Arcade movies remain supported. Build, signing and desktop tests do not replace headset checks of comfort, controls and sustained performance.

### HD resources and shared preferences

**Graphics and performance → HD textures and media** selects prepared HD pictures, interface atlases, movies and the enhanced PlayStation startup. OFF uses original resources; world texture filtering/mipmaps have their own switch. Missing HD resources use the original assets. Startup remains unskippable in both modes.

Arcade and Simulation use one private `files/saves/vr-settings.txt` for VR graphics, controls, HUD, speed units, vibration and HD selection. On the first launch after updating, preferences migrate from the most recently modified disc overlay. Later disc switches reuse that common file. Disc memory cards, earned progress and cheat switches remain separate. The migration does not alter old per-disc files.

On the second **HUD elements** page, **Movie skip hint** controls the A/B skip reminder. It defaults to ON, saves with the shared VR preferences, and does not change the skip buttons or the unskippable PlayStation startup.

**Graphics and performance → PlayStation intro** can disable the complete console startup for subsequent launches. This defaults to ON and is saved with the shared VR preferences. Enabled startup still plays to completion before the disc selector.
