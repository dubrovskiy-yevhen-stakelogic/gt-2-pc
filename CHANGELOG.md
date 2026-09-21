# Changelog

## 0.3.0 - 2026-09-21

### One release for PC and VR

- One download, **GT2-0.3.0.zip**, includes Windows PC play on a normal monitor, Windows PCVR and Quest 3 standalone VR.
- Added Windows PCVR through OpenXR: theatre-screen menus and movies, stereo races, head tracking, virtual hands and three driving modes: Stick, Virtual wheel and Motion.
- Added dedicated launchers for SteamVR / Steam Link, Meta Quest Link / Air Link and Virtual Desktop (VDXR). Each selects its runtime for the game without changing the Windows OpenXR default.

### Menus and controls

- Added a shared startup disc picker for Arcade and Simulation on PC and Quest, with the last choice remembered.
- Added **Change game (Arcade / Simulation)** to the settings menu. Return to disc selection without restarting the application or replaying the PlayStation startup. Save progress before confirming a disc change.
- Added **L3 + R3** as an alternative VR menu shortcut. **Both grips + Menu** remains available; **Y** changes the camera even while both hands hold the virtual wheel.
- Shared graphics, HUD and control preferences between discs while keeping memory cards and progression separate.
- Expanded desktop settings with graphics, HD media and intro options, HUD visibility, profiler, gamepad bindings, DualSense pedal resistance and Arcade/Simulation cheats.
- Added PCVR controller vibration and pause/resume handling when the headset runtime loses focus.

### Fixes

- Fixed crackling audio and slow startup movies through Steam Link by increasing the Windows audio queue to cover delayed streaming-device callbacks.
- Fixed the runtime-selection conflict when SteamVR remained running while using Meta Link or Virtual Desktop.
- Removed the conflict between the old grips + Y menu shortcut and camera switching while steering.

### Saves and installation

- Added USB save-transfer helpers in both directions between PC and Quest. Transfers validate memory cards, create backups and verify the copied data.
- Added save-folder locking to prevent the game and transfer tool from writing the same card at once. Transfers copy complete cards; they do not convert saves between regions.
- Packaged the Windows OpenXR loader and precompiled installation tools. No SDK or C++ build is needed to install the player release.
- Existing saves and settings are retained. The standalone APK is version **0.3.0**, Android version code **15**, signed with the existing public release identity.

SteamVR / Steam Link, Meta Link and Virtual Desktop were tested by the project author. The packaged VR bindings target Touch controllers; other controller layouts are not yet validated. The planned wheel/shifter/force-feedback, cockpit-view and PSVR2 Sense adaptive-trigger work is listed in the [README roadmap](README.md#committed-roadmap).

## 0.2.0

### Graphics and media

- Added optional offline HD preparation for game pictures, menu/HUD atlases and full-screen movies. Prepared media works on PC and Quest.
- Added a saved HD textures and media switch in the VR menu; original resources remain available.
- Added mipmaps and bounded anisotropic filtering to reduce distant texture shimmer while preserving road signs.
- Added optional playback and HD preparation of the original PlayStation startup, plus a VR menu switch to disable it. Capturing it requires a local BIOS dump; no BIOS is needed to install or play the game. BIOS and startup recordings are not bundled.
- Added a saved HUD option to hide the movie-skip hint.

### Fixes

- Fixed the rally crash reported in 0.1.0.
- Corrected oversized car shadows and their placement on banked road surfaces.
- Fixed overlapping distant road/hillside geometry, the blocked Midfield tunnel entrance and grass covering direction arrows.
- Corrected transparent texture fringes at joined course surfaces that exposed the sky through thin seams.
- Fixed scenery disappearing too close to the viewer. Entire-course mode now uses detailed road geometry and the authored near scenery representations, including empty entries for distant-only copies.
- Fixed menu stalls during car/course selection after enabling enhanced media.
- Improved race-music streaming to address crackling and nearly inaudible playback reported on European Arcade SCES-02380.
- Corrected HUD map-edge artifacts.

### Settings and release

- Added a native Python installer for Linux and Steam Deck that prepares supported discs and installs the standalone Quest game without Wine or system-partition changes.
- Added an explicit optional BIOS choice in the Windows installation wizard and Linux installer. Skipping it leaves the game fully playable without console startup.

- VR graphics, controls and HUD preferences are shared between Arcade and Simulation; progression remains separate.
- New Quest profiles default to **72 Hz**, **150% resolution**, **MSAA 2x**, **medium foveation** and the **entire detailed course**. Existing saved preferences are preserved.
- Updated source documentation, dependency credits and MIT licensing notices. Player packages retain third-party notices.
- Android version code: **14**. Release APKs use the same signing identity as 0.1.0.

### Known limits

- Some menu/HUD lettering still shows pixelation or smoothing artifacts; HD movies can show artifacts in a few game-footage sequences. The HD switch restores original media.
- Full-course rendering can increase GPU load. A selected 72 Hz mode does not guarantee sustained 72 FPS on every course; 175% at 90 FPS is not a supported performance claim.
- European discs currently use the port's English UI.

## 0.1.0

- Initial public alpha for Windows PC and Quest 3 standalone VR, including Arcade and Simulation, three VR driving modes and native DualSense support on PC.
