# Gran Turismo 2 PC & VR — 0.2.0

A native C++ port of Gran Turismo 2 for **Windows PC** and **Quest 3 standalone VR**, with Vulkan rendering. Quest runs the game on the headset without a streaming PC. The game uses ported simulation code; the PS1 interpreter is a separate development/reference tool.

See [CHANGELOG.md](CHANGELOG.md) for the 0.2.0 changes. Supply your own supported disc images. Game data, BIOS, saves and copyrighted game artwork are not included.

## Features

- Arcade and Simulation discs, with a disc picker at Quest startup and shared VR preferences and separate saves for each mode.
- Arcade races, rally/time trials, opponent AI, ghost/replay sessions; Simulation career, licences, dealerships, garage, tuning and events.
- Original fixed-step vehicle simulation, animated steering/suspension/wheels, car reflections, track scenery, particles and night lighting.
- Native audio: engine, tyres, road effects, music and movies. Race pause stops timers and the entire race audio mixer. VR/system suspension does not advance the race.
- Disc publisher/warning screens and Arcade intro, course previews and ending movies; button skipping. Optional playback of a locally prepared PlayStation startup; BIOS and captured startup media are **not bundled or required to play**.
- Saved graphics and control settings, graphics overlay, configurable draw distance including the entire course and distant scenery, MSAA and texture filtering.
- Original-resolution assets with higher-resolution rendering. PC supports fixed **720p, 1080p, 1440p and 4K**, or 50–200% of window size; configurable FPS cap/VSync and interpolated presentation. Physics stays at 30 Hz.
- Keyboard, XInput controllers and **native DualSense / DualSense Edge support on PC**, over USB or Bluetooth: buttons, sticks, trigger pedals, adjustable vibration and adaptive accelerator/brake resistance. Adaptive effects do not require Steam Input. Touch controllers have vibration, not adaptive triggers.
- Quest theatre-screen menus/movies and head-tracked stereo driving/replays. The HUD and mirror share a properly projected stereo plane.
- Three Quest driving modes: **Stick, Virtual wheel and Motion**. One/two-handed wheel grabbing with animated hands; all fingers stay closed while holding the wheel. Motion uses wrist rotation around the forearm, with grip-held steering and independent trigger pedals.
- Quest Controls submenu: driving bindings, selected steering stick/motion hand, wheel position/size and height-only adjustment of the original starting flythrough. Automatic brake-to-reverse is available.
- Quest Graphics submenu: 50–200% eye resolution, available headset refresh rates, MSAA, distance, textures, foveation and vibration. Resolution changes require a restart; other supported settings apply immediately.
- Saved km/h / mph selection in the VR HUD menu, with km/h as the default on both discs. Saved HUD visibility controls for map, lap/times, records, gauges, turbo, tyres, mirror, countdown, warnings, messages and replay caption.
- FPS profiler: application FPS, frame time, GPU time, 1% low, peak frame interval, texture-cache coverage and frustum-culling counts.
- Arcade cheats: unlock all course selections and the car roster as reversible overrides. Simulation VR cheats: gold licences, 99,999,999 credits, event unlocks and a car catalogue for adding cars to the garage. The original 100-car garage limit remains. Career cheat writes are checked and backed up before replacing a save.
- VR performance features: multiview stereo, both-eye frustum culling, decoded texture caching, staged uploads, direct MSAA rendering to compatible XR images, fixed peripheral foveation and CPU/GPU performance requests.
- Trees use position-based cylindrical billboards in VR: head rotation alone does not rotate them.
- Developer tools for car/track export, JSON modifications, captures and reference comparisons. Arbitrary new-track geometry compilation is not implemented.

## Install the player release

Extract **GT2-VR-0.2.0.zip** into a normal writable folder. Run **INSTALL.bat** for Quest or **INSTALL-PC.bat** for Windows, then select one or both of your disc images. The package contains precompiled installation tools; no Visual Studio or C++ compilation is needed.

On **Linux / Steam Deck**, extract the same ZIP, open a terminal in its folder and run `bash INSTALL-LINUX.sh`. It prepares discs and installs the standalone Quest game without Wine or modifying the SteamOS system partition. See the [Steam Deck step-by-step guide](docs/LINUX-INSTALL.md).

Both installers offer an **optional PlayStation startup**. Skip the BIOS prompt to play normally without it, or select your own matching 512 KiB BIOS dump to prepare the original white and dark screens with sound. Firmware is never downloaded or sent to Quest. Existing prepared intros can be disabled in the VR menu.

For Quest, enable developer mode, connect USB and accept USB debugging in the headset. The installer locates ADB or downloads a pinned Google Platform Tools archive, prepares the disc data, updates the APK, copies the assets and verifies the disc hashes and application read access. It does not launch the game. Open **GT2 VR** under **Unknown Sources**. See [player installation](docs/PLAYER-INSTALL.md).

## Supported discs

| Disc | Executable | Executable SHA-1 |
|---|---|---|
| US Arcade v1.1 | SCUS_944.55 | `231f9dba7191b9ef915621662afdc40a7c66df95` |
| US Simulation v1.2 | SCUS_944.88 | `3030aa271c0a4022fc69ce09d76a6bc75e69a32a` |
| Europe Arcade (En,Fr,De,Es,It) | SCES_023.80 | `2b59ad844a4dbb934fc1d8ef955c63038f54e932` |
| Europe Simulation (En,Fr,De,Es,It) | SCES_123.80 | `5172a19c1d0fe07a2a61966653b755afe26d9e69` |

European media currently use the port's English UI. Other language selections and other revisions are not claimed as supported. Revisions are checked by executable hash, not by the image filename. Single-track MODE2/2352 BIN/CUE, raw 2352-byte ISO, ZIP and 7z inputs are accepted. A 2048-byte ISO omits required XA sectors and is rejected. The installer never downloads game images.

## Controls

On PC, **F10** or **Create/Select + Options/Start** opens settings. Arrows/D-pad navigate and change values; Escape/Triangle closes. DualSense R2 accelerates and L2 brakes in the pedal profile. Native HID effects require the physical controller, rather than a virtual Xbox controller exposed by a mapper.

On Quest, **Menu** pauses the race. **Hold both grips + Menu** opens VR settings. Stick up/down selects rows; **left/right triggers decrease/increase values**. A confirms/opens, B goes back. Grips and horizontal stick drift cannot change menu values. See [Quest controls and settings](docs/QUEST.md).

## Performance and limits

New Quest disc profiles start at **150% resolution, 72 Hz, MSAA 2x, the entire detailed course and medium foveation**, with smooth textures, virtual-wheel driving, full HUD, 100% vibration and the profiler off. All cheats and unlock overrides start off on both discs. Existing saved preferences are preserved. See [the full default settings](docs/QUEST.md#first-launch-defaults).

175% eye resolution means about **3.06 times as many pixels** as 100%. Sustained 175% at 90 FPS is not achieved across races. Select resolution, MSAA, foveation and distance to suit the scene; the profiler reports actual application frames, not the selected display refresh rate.

Two-player split-screen remains on the theatre screen in VR. Wider VR views can reveal gaps in original geometry. Billboards remain flat artwork; the fix prevents gaze-following rotation and does not turn trees into 3D models. Public release signing differs from development signing: never uninstall a development build merely to bypass a signature mismatch without exporting its saves first.

## Build from source

Run the source folder's **INSTALL.bat** to install missing CMake, MSVC C++ Build Tools and Vulkan SDK dependencies through WinGet, build, run checks and prepare your discs. Optional 7z extraction installs 7-Zip. Windows driver installation remains the GPU vendor's responsibility. Runtime requires Windows x64 and Vulkan 1.3.

```powershell
.\scripts\install.ps1 -DiscImage 'D:\Discs\Arcade.bin','D:\Discs\Simulation.bin'
cmd /c build.cmd build_local
ctest --test-dir build_local --output-on-failure
```

`-InstallDir`, `-BuildDir`, `-SkipDependencies` and `-NoBuild` support custom/offline preparations. Loose assets are stored under `runtime/{arcade,simulation}/assets`, with raw sectors retained for audio, movies and overlay data. Source installation preserves previous data and saves. Android build/signing instructions are in [QUEST.md](docs/QUEST.md).

Sources are published as a normal repository folder. Run `scripts/audit-source.ps1` before publication; retail data, private diagnostics, signing keys and build outputs are excluded. See [validation](docs/VALIDATION.md) and [third-party provenance](THIRD_PARTY.md).

## Optional offline media preparation

The installer can upscale title/GT Mode backgrounds, startup pictures and full-screen movies, and prepare 4x menu-font/button/HUD atlases with palette-aware shader smoothing on a Windows GPU, then use the prepared assets on PC or Quest. It can also capture the original two-screen PlayStation startup and sound from a local BIOS dump, before the Quest disc selector; the firmware sequence cannot be skipped. Movie preparation limits neural changes against the original frames to reduce invented detail. Smooth texture filtering now includes mipmaps for distant surfaces and fences. See [HD media setup, coverage and limitations](docs/HD-MEDIA.md). BIOS is optional and used only to prepare the console startup; normal installation and gameplay do not require it. No BIOS or generated game artwork is distributed.

HD resources can be switched off in **VR menu → Graphics and performance → HD textures and media**. Arcade and Simulation share VR graphics, controls and HUD preferences. Optional offline HD preparation also enhances the PlayStation startup while retaining its original version and audio; see [HD media](docs/HD-MEDIA.md).

HD preparation includes offline xBR contour smoothing of shared menu/HUD atlases with original glyphs and live palette colours. Opaque menu captions are included. Pictures and movies use a separate neural upscaler. The HUD submenu can hide the movie-skip reminder; its visibility is saved across both discs.

**VR menu → Graphics and performance → PlayStation intro** enables or disables the prepared console startup on the next launch. It defaults to ON and is shared by both discs. When enabled, the sequence plays completely before disc selection.

## License

Project code is available under the [MIT License](LICENSE). You may use, modify
and redistribute it, including commercially, provided that you retain the
copyright and permission notice. Credit as **Gran Turismo 2 PC & VR contributors**.
Third-party components retain their own licenses. The MIT license does not grant
rights to Gran Turismo game data, Sony firmware, trademarks or extracted assets.

Virtual-wheel and motion controls and hand integration are shared with MiamiVR
Quest, a project by the same sole author.

## Credits and third-party components

- **Khronos Group** — OpenXR headers and the Android OpenXR loader, Apache-2.0.
- **VRMADA / UltimateXR** — virtual hand meshes, poses and skin texture, MIT.
- **Sean Barrett and stb contributors** — image decoding/encoding libraries, used under MIT.
- **Hyllian** — xBR contour algorithm adapted for offline UI processing, MIT.
- **RetroArch team** — libretro API header used by the optional offline boot-capture helper, MIT.

The optional installer also downloads **Real-ESRGAN** (Xintao Wang, BSD-3-Clause)
and **Beetle PSX libretro** (GPL-2.0) for offline media preparation and local BIOS
capture respectively. They are separate tools, not linked into the game.
See [THIRD_PARTY.md](THIRD_PARTY.md) for source links, exact provenance and license
locations; player packages retain the dependency notices under `LICENSES/`.
