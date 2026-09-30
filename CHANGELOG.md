# Changelog

## 0.8.1

- Fixed European Simulation career races failing to start from the GO! button when event tables lay beyond the main UI data range.
- Fixed European Simulation memory-card screens and confirmation buttons for saving, loading and replay management.

- Added Campaign / saves to the overlay on Quest standalone and desktop. In Simulation GT Mode, start a new campaign or restore the previous one without reinstalling.
- Campaign replacement defaults to Cancel and requires a fresh three-second hold. Releasing the button, leaving the confirmation row, losing focus or suspending cancels the hold.
- Each reset or restore first verifies a separate backup of the memory card and live progress, including unsaved changes. Failed backups prevent replacement; older backups are retained.
- Controls, graphics settings, other memory-card entries and slot 2 are preserved. New campaigns reset cars, credits, licences and records, disable Simulation event unlock and return to the title.

## 0.8.0

- Added browser play through WebAssembly and WebGL2, compiled from the same C++ game as the native platforms.
- Added local BIN selection, browser audio, persistent saves and save backup import/export.
- Added automatic brake-to-reverse for keyboard controls without requiring a connected gamepad.
- Changed desktop resolution scaling to 5% steps between 50% and 200%.
- Added the ready-to-host browser site to the complete release alongside Windows, PCVR, Quest, macOS and Steam Deck installation files.
- Updated the English interface and installation guide for one complete release.

## 0.7.0

- Added a native Linux / Steam Deck SDL2 client with Vulkan rendering, gamepad input and audio.
- Added a desktop disc picker, separate Arcade and Simulation imports, application locking and Linux save-folder locks.
- Added Flatpak build/install/play scripts and a 1280 x 800 initial fullscreen size for the Deck package.
- Added a View + Menu settings shortcut, one pause action per Menu press, disc switching, disc import and Quit game in the overlay.

## 0.6.0 - 2026-09-27

- Added the macOS desktop client with SDL2 input/audio and Vulkan rendering through MoltenVK.
- Added source installation, bundled runtime dependencies, a native disc picker and a save-preserving application update.
- Added Shift+Q for settings, Option+Enter for fullscreen, Quit game and Import another game disc.
- Added PREPARE-HD.command for HD pictures, menu fonts, HUD and optional full-screen movies. Original discs and saves are retained. See [Mac setup](docs/MACOS.md) for installation instructions.

## 0.5.0 - 2026-09-24

- Added a fitted cockpit for the live single-player Driver camera: dashboard, analogue speed/RPM needles, moving steering wheel, seats and door trim inside the selected car's original body. The cockpit appears as soon as the countdown camera switches to Driver, before GO.
- Window openings follow the original glass and body geometry, including curved windscreens and separate panes. The exterior retains its paint and reflections.
- Added **Cockpit / driver view** settings: **Cockpit / Original**, seat height **-20 to +20 cm**, seat forward/back **-20 to +40 cm**, a steering-wheel visibility switch and reset. Seat adjustments use 2 cm steps and save across both discs.
- Hands and the interactive VR wheel follow suspension roll, pitch and impact movement with the body.
- Added a physical central rear-view mirror using one rear-camera image for both eyes. The mirror can be switched off or resized from **25% to 100%**. Switching it off removes its rear-view render pass.
- Enabling the FPS profiler now also records a CSV log. Logs include frame timing, GPU timing, draw counts, rear-view activity and active graphics settings; disabling the profiler flushes and closes the file.
- New VR profiles default to **130% eye resolution**. The **instrument HUD** and **profiler** start **off** on all platforms; physical cockpit gauges remain visible. Desktop rendering scale remains 100%. Updates preserve saved settings.
- Fixed an enabled but disconnected or incomplete wheel setup suppressing gamepad and keyboard driving input. The saved wheel calibration remains available when the rig reconnects.
- The cockpit uses original procedural geometry. No external game's models or textures are required. Replays, the external starting flythrough and split-screen retain their existing views; vehicle simulation and saves are unchanged.

## 0.4.0 - 2026-09-22

- Added an embedded offline catalogue of 72 USB device records for automatic wheel/pedal/shifter setup, with exact device matching and explicit choices for ambiguous rigs or interchangeable rims.
- Added live wheel input indicators, gearbox/FFB controls, guided axis calibration, an illustrated H-shifter wizard and wheel-operated menu buttons.
- Saved device bindings survive disconnects. Clutch pedals and USB shifters are optional; steering and pedals remain usable without them.
- Added a Windows PC / PCVR racing-wheel submenu under Controls, with independent USB device assignments for steering, pedals and shifters.
- Added axis detection, calibration, inversion, dead zones, saturation, response curves, live input values and saved profiles shared by both discs.
- Added sequential/paddle and H-pattern gear selection, neutral, analogue reverse throttle and a clutch pedal. Wheel steering and pedals bypass the gamepad curves.
- Added tyre-force-based DirectInput force feedback, strength, damping and force-direction settings. Effects stop on pause, focus/device loss and exit, with a finite driver-side timeout.
- Added native wheel replay/ghost frames that preserve direct gear selection and clutch travel. These frames require 0.4.0 or later; original pad replay encoding is unchanged.
- Race AT/MT selection is independent of the hardware layout. Added an optional **Ignore gear-change speed** switch, off by default, with the setting preserved in replays.
- Added **Traction control 0..5** (default 0) and **Countersteering assistance Off / Weak / Strong** (default Weak). Settings are shared by both discs and saved without resetting calibration.
- Added configurable steering geometry for tyre-force feedback and timing diagnostics for input/FFB stalls.
- Windows installation now prepares HD pictures, fonts and HUD for both Arcade and Simulation by default. Simulation US v1.2 contains 461 picture replacements. Original media and additional movie preparation remain selectable.

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

### Saves and installation

- Added USB save-transfer helpers in both directions between PC and Quest. Transfers validate memory cards, create backups and verify the copied data.
- Added save-folder locking to prevent the game and transfer tool from writing the same card at once. Transfers copy complete cards; they do not convert saves between regions.
- Packaged the Windows OpenXR loader and precompiled installation tools. No SDK or C++ build is needed to install the player release.


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
- Improved race-music streaming to address crackling and nearly inaudible playback reported on European Arcade SCES-02380.

### Settings and release

- Added a native Python installer for Linux and Steam Deck that prepares supported discs and installs the standalone Quest game without Wine or system-partition changes.
- Added an explicit optional BIOS choice in the Windows installation wizard and Linux installer. Skipping it leaves the game fully playable without console startup.

- VR graphics, controls and HUD preferences are shared between Arcade and Simulation; progression remains separate.
- New Quest profiles default to **72 Hz**, **150% resolution**, **MSAA 2x**, **medium foveation** and the **entire detailed course**. Existing saved preferences are preserved.
- Updated source documentation, dependency credits and MIT licensing notices. Player packages retain third-party notices.

### Known limits

- Some menu/HUD lettering still shows pixelation or smoothing artifacts; HD movies can show artifacts in a few game-footage sequences. The HD switch restores original media.
- Full-course rendering can increase GPU load. A selected 72 Hz mode does not guarantee sustained 72 FPS on every course; 175% at 90 FPS is not a supported performance claim.
- European discs currently use the port's English UI.

## 0.1.0

- Initial public alpha for Windows PC and Quest 3 standalone VR, including Arcade and Simulation, three VR driving modes and native DualSense support on PC.
