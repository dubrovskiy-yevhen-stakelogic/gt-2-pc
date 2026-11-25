# Changelog

## 1.0.0

### Cross-platform multiplayer

- Add native online racing between Windows/PCVR, Linux/Steam Deck and Quest, entered through the new Arcade Multiplayer tile. Browse public rooms or create a named room with an optional password; no manual IP entry or invitation files are needed.
- Support 2-6 real players, with at least two required to start. All connected players must be Ready. Hosts choose the track, laps, transmission and either a shared car or an Arcade class; players select eligible cars in the garage.
- Offer optional bots to fill vacant places, with Easy, Normal and Hard difficulty. Support rally cars and tracks, including point-to-point races.
- Add track-map previews and 50 layouts, with day/night and reverse variants where the disc provides them. Preserve valid car choices when changing unrelated room settings and restore road-car choices after rally.
- Wait for every player to load before the shared camera intro and countdown. Keep Arcade menu music playing until the race begins.
- Display player nicknames, bot labels and multiplayer results. The first human finisher starts a visible 30-second countdown; finish sooner if all human players finish.
- Keep rooms open between races, clear readiness for the next race and show who is still racing or viewing results. Players can return to the room, choose cars and race again on another track.
- Let hosts remove players in the lobby or during a race. Handle host succession, reconnects, kicked players and empty-room cleanup.
- Add an emote wheel: press L3, select with the left stick, then confirm with L3 or Cross/A. Emotes appear beside player names, including for the sender. Steering and pedals remain active while using emotes or browsing the race menu.
- Include the room directory, dedicated server and native networking dependencies in the source kit; allow multiplayer networking in the Steam Deck Flatpak.
- Multiplayer has not had a public test yet; testing so far has been by one person using multiple devices. Bugs and connection problems may remain.

### HD textures, 3D cockpits and GT4 models

- Add optional drop-in HD texture packs with editable PNGs, saved category controls and original-texture fallback for missing images. Players can replace or remove individual pictures without rebuilding the game. Texture selection is available in settings.
- Use mipmapped HD texture atlases and optional compressed Quest caches; changing replacement images invalidates their cached artwork.
- Include authored 3D interiors for 12 Nissan Skyline GT-R/R32 catalogue entries, with camera anchors, animated instruments and mirrors. Attach the supported interiors during GT4 preparation and include their manifests and attribution in the source kit; other cars retain fitted cockpits.
- Replace the generated steering wheel and hands with real textured 3D models: a steering wheel adapted from Marc Ed's interior and technical gloves from VRMADA's UltimateXR. Include the source attribution and licenses.
- Improve cockpit fitting, glass rendering and steering-column placement for views inside and outside the car. Add L3 garage filtering for cars with authored interiors.
- Reduce GT4 pack size by storing paint variants as texture deltas instead of full duplicate images, and limit imported paints to the usable GT2 colour count. Existing full-PNG packs remain supported. ([#108])
- Prepare GT4 geometry, textures and authored interiors in the background during startup, prioritizing the career car, recent models and standard Arcade A/B/C/S cars. Reuse prepared data in menus and races.

### Disc mods, saves and gameplay

- Load A-Spec Simulation/Combined and GT2+ Complete NTSC-U Simulation/Arcade images directly, including US Arcade v1.1. Handle modified volume layouts, menu tables and sparse car-colour data. ([#115])
- Keep retail and mod datasets as separate picker entries with separate saves and settings; compatible standalone and Combined images of the same mod share their memory card. Read save/replay filenames from the disc executable so GT2+ saves can coexist with retail saves. ([#119])
- Install all prepared retail and mod datasets on Quest without replacing existing saves. Preserve the selected Combined dataset and its cards when switching Arcade/Simulation.
- Add a Simulation car-damage switch. Record its event-time value with replays so later preference changes do not change replay behaviour. ([#70])
- Add replay input-display front-wheel indicators and preserve their original pixel-aligned, nearest-filtered graphics. ([#72], [#92])
- Prevent P from skipping an event race's finish and result handling. ([#82])
- Add disc-aware GT2+ turbo-section and power-unit behaviour: Auto follows the loaded disc, with Turbo Auto/On/Off and Power Auto/hp/PS overrides. Retail and A-Spec keep stock defaults; physics and saves keep their native units. ([#116], [#127])

### Menus, graphics, audio and controls

- Reduce painted road-line flicker and stationary-camera shimmer, improve decoded-texture cache handling, and avoid repeated texture promotion during the start countdown. ([#95])

- Prevent flickering on zero-thickness car panels such as rear wings by drawing the correct side of coincident opposite-facing polygons. ([#126])
- Apply the saved texture filter from the first menu frame. ([#74])
- Improve MSAA coverage for stereo HUD layers, menu cars and mirrors; keep VR menu-car clipping attached to the HUD plane. ([#75])
- Cache unchanged 2D menu layers instead of rebuilding them, and refresh them when HD media or rendering settings change. ([#85])
- Add Off to car reflections and apply reflection changes to already loaded cars. ([#97], integration [#98])
- Make a positive Draw Distance a real rendering cap for road chunks, scenery, billboards and glows. ([#110])
- Reuse Vulkan pipelines and render targets when changing graphics settings. Report the actual supported MSAA count when the requested count is clamped. Use the direct OpenXR colour path on Quest at 1x as well as multisampled settings.
- Run on Vulkan 1.1/1.2 GPUs with a render-pass fallback (Vulkan 1.3 keeps dynamic rendering), report the loader, GPUs, available memory and chosen path with `gt2game --diagnose`, and start with a clear error on a machine without `vulkan-1.dll` instead of failing inside the loader. VR follows the same rule: a runtime that offers only Vulkan 1.1 or 1.2 gets multiview render passes, and foveation uses the extension's shading-rate attachment on that path. `GT2_VK_RENDERPASS=1` forces the fallback for testing.
- Renderer memory follows the content: the vertex arena and native-model storage grow on demand instead of reserving 137 MiB, so scenes and menus allocate only the geometry they upload. Allocation peaks are logged, and a new vertex-arena audit covers sparse indices, growth, holes and relocation.
- Choose a graphics preset that fits the machine: Low spec, Vanilla or Modern, with Custom derived. Low spec renders at 720p without MSAA or car reflections, Vanilla keeps the original 30 fps look, Modern uses MSAA 4, smooth textures and 1000 m draw distance. The preset is in PC SETTINGS and the port menu, also `--low-spec` / `--vanilla` / `--modern`.
- Resolution is the output setting (720p / 900p / 1080p / Display): the whole frame renders at that size and the presentation engine scales it to the window. Usable video memory caps the output and MSAA, sizes the decoded texture cache in layers (there is no low-memory mode or flag), and stops with a clear message below 128 MiB at 720p / 192 MiB at 1080p/Display. Scene scale stays advanced at 100%.
- Correct Up navigation in the disc picker. ([#79])
- Improve settings layout with consistent row spacing, automatic pagination and room for status text. Remove the duplicate pause row, update control hints and place display timing under Graphics. ([#100], [#118])
- Show Saved only after an actual setting change, and clear stale status messages. ([#102])
- Group content/media options separately from graphics and put vibration under Controls. ([#105], integration [#111])
- Add configurable keyboard driving controls, reserved-shortcut validation and consistent modifier/navigation/function-key mapping across desktop, SDL and browser builds. ([#106], integration [#111])
- Add browser gamepad rumble where the browser exposes a vibration actuator; keep it routed to the selected controller through reconnects and identical-pad setups. ([#93], integration [#98])
- Align the profiler's 1% low line with its FPS/GPU lines. ([#96])
- Use the SDL audio device's actual sample rate, including 96/192 kHz, while retaining a bounded audio queue and clean error handling. ([#84])

### Installation, packaging and build checks

- Reuse existing Windows data when moving from an older ZIP installation to MSI, including the legacy default folder. Preserve saves and prepared media without requiring another disc import.
- Prepare HD media for Combined discs. ([#90])
- Document moving an installed data folder and add the Quiver Launcher catalogue link. ([#108], [#69])
- Package Quest and PCVR installers together in a clearly named VR archive, and keep the packaging automated for later releases.
- Publish prebuilt Linux x86_64 and ARM64 ZIP packages. ([#113])
- Retry transient APT/Flathub downloads and retain the workflow's download helper when building older tags. ([#66])
- Correct macOS test targets and Windows CI Vulkan-device setup; use a Vulkan SDK release that includes the required Windows runtime. ([#114])
- Extend regression coverage for rendering/MSAA, input, audio, model imports, mod datasets and multiplayer room/race lifecycles. Validate source exports, including the new cockpit, wheel/glove assets and attribution.

Thanks to **@lucianodato** for the contributions linked above. Integration PRs
[#98] and [#111] also include the accompanying compatibility fixes.

[#66]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/66
[#69]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/69
[#70]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/70
[#72]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/72
[#74]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/74
[#75]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/75
[#79]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/79
[#82]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/82
[#84]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/84
[#85]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/85
[#90]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/90
[#92]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/92
[#93]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/93
[#95]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/95
[#96]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/96
[#97]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/97
[#98]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/98
[#100]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/100
[#102]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/102
[#105]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/105
[#106]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/106
[#108]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/108
[#110]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/110
[#111]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/111
[#113]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/113
[#114]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/114
[#115]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/115
[#116]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/116
[#118]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/118
[#119]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/119
[#126]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/126
[#127]: https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/127


## 0.9.2 - Installation, car lights and HUD fixes

### Game fixes

- Apply the configured music and sound-effect volumes to Arcade, Simulation and title menus. ([#48](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/48), @lucianodato)
- Keep short menu-music notes from being cut at their start and release completed voices correctly. ([#58](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/58), @lucianodato)
- Run GT Mode event races with the correct game mode, fixing unintended damage behaviour. ([#55](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/55), @lucianodato)
- Support Silent's combined disc with Arcade and Simulation selection, separate saves, and the correct Arcade intro, ending and menu music. ([#59](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/59), @lucianodato)
- Keep HUD, menus and licence-result panels above the cockpit on the direct Vulkan path, including with MSAA disabled. ([#61](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/61), @lucianodato)
- Restore stock car lamps over their housings, select night models/textures on night courses, and drive each split-screen player's brake lights from that player's own input. Cars without a night pair retain their day assets. ([#62](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/62), @lucianodato)

### Installers and launchers

Thanks to **@lucianodato** for the cross-platform installer and launcher improvements in [PR #65](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/65):

- Provide ready-to-install Windows MSI, macOS DMG and Linux/Steam Deck Flatpak packages, alongside the portable Windows ZIP, macOS ZIPs and Quest APK, so players do not need to build the game from source.
- Let Windows users choose the folder for discs, saves and media, including another drive, and reuse that location on updates.
- Import discs on the first Windows launcher run, then start the game. Refresh the installed game files on launcher updates without re-importing discs.
- Avoid interactive HD-media and BIOS console prompts when launched from a graphical launcher; HD media can be prepared separately.
- Use one x86_64 Flatpak for Linux and Steam Deck, with access to discs in home and mounted-media folders. Apply the Deck window preset only from the Deck launcher.
- Preserve the macOS app bundle inside the ZIP and provide clearer platform download choices. Arrange filenames and package layouts for launcher compatibility, including Quiver; its external catalogue configuration is maintained separately.
- Correct the macOS test target, Steam Deck installer/launcher checks and Windows MSI payload validation.

### Cross-platform builds and release automation

- Add automated builds for Windows, macOS Apple Silicon and Intel, Linux, Steam Deck, Quest and WebAssembly. Fix compiler, macOS dependency/source-export and Flatpak packaging failures, and derive the Deck bundle version from the project version instead of a stale hardcoded value. ([#53](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/53), @lucianodato)
- Make release uploads independent per platform, so a failed platform job does not withhold successful builds from the others. Improve audio-test timing on loaded runners, retry transient OpenXR download failures, and document signing secrets and release procedures. ([#60](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/60), @lucianodato)

### Integration fixes and version information

- Fix first-run Windows argument binding and validate standalone Arcade discs with their correct profile, preventing false installer failures.
- Fix combined-disc selection on Quest and switching between Arcade and Simulation in direct-launch sessions, keeping mode-specific save paths.
- Preserve rolling starts when entering GT Mode events with the corrected race shell.
- Show the running game version in the title of the custom settings menu on desktop, VR and browser builds.
- Build the requested release tag and target the intended publication repository in manually triggered release runs.

### Release scope

This release uses the public main branch. Network multiplayer, new HD car models
and fully authored 3D cockpits are planned for future releases and remain in development.
Existing split-screen, GT4 imports and current cockpit rendering remain available.

Thanks to **@lucianodato** for all nine merged contributions in this release.

## 0.9.1 - Cockpits, race results and platform fixes

### Cockpits and VR

- Improve GT4 cockpit fitting against original body and window geometry: roof lining, lower walls, door trim and windscreen/dashboard joins.
- Remove more leftover interior surfaces while preserving body panels, window frames, decals and other original details.
- Preserve physical body backfaces in authored interiors and keep generated trim clear of window openings and mirror sightlines.
- Add cockpit inspection from the garage and car selection. Returning preserves the selected car, paint, menu position and career progress.
- Increase imported-model vertex capacity to match the full-catalogue validation configuration.
- Remaining fit issues are tracked in [the cockpit follow-up checklist](docs/COCKPIT-NEXT-RELEASE.md): window overlap is flagged on 20 models and source-body openings on 92; the two sets overlap.

### Race results, music and HUD

- Show the original licence BONUS screen when a licence or all-gold prize car is earned, including Save/Next choices. Awards appear with automatic replay enabled or disabled; the licence plate renders without generated car wheels. ([#20](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/20), @lucianodato)
- Add optional throttle, brake, handbrake and steering indicators, including the followed car in two-player full-screen replays. ([#23](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/23), @lucianodato)
- Add a setting to skip automatic race replays and return directly to the result/menu flow. ([#19](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/19), @lucianodato)
- Restore GT Mode music transitions and volume handling for post-race views, machine tests and career menus. ([#9](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/9), @lucianodato)
- Fix menu music gradually losing notes during continuous playback. ([#36](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/36), @lucianodato)
- Show the gold cup correctly in licence test menus. ([#17](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/17), @lucianodato)

### Menus and models

- Accept GM pages without an unused picture strip, fixing affected tuning-shop/menu pages. ([#11](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/11), @lucianodato)
- Load packed mod-car models whose LOD offsets are stale. ([#24](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/24), @lucianodato)
- Preserve the 4:3 settings/menu canvas on wide and tall displays. ([#32](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/32), @g2h0)
- Restore HD event-page backgrounds after the post-race save screen replaces a shared picture texture.

### Import and installation

- Play Silent's combined disc from a single installed image: the picker offers Arcade Mode and Simulation Mode, and each uses its own existing save slot. The PC, macOS, Steam Deck and Quest installers recognize the disc. ([#52](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/issues/52), @lucianodato)
- Automatically prepare missing GT4-import dependencies, handle long extraction paths, and install the converted catalogue directly on the selected PC or Quest destination.
- Find 7-Zip through PATH when preparing HD media. ([#34](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/34), @g2h0)
- Preserve installer validation output and stderr, and show the exit code and complete log path on failure.
- Include INSTALL-QUEST.bat in standalone native packages while retaining INSTALL.bat compatibility.
- Create the APK alignment output directory when building from a fresh source checkout.

### Browser, Linux, macOS and build checks

- Match shared shader uniform layouts to restore WebGL startup.
- Recreate the Wayland swapchain when the drawable size changes. ([#28](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/28), @g2h0)
- Close the Linux disc-import dialog before starting the game. ([#29](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/29), @g2h0)
- Resolve GCC 16 warnings in warnings-as-errors builds. ([#30](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/30), @g2h0)
- Isolate SDL input tests from attached physical controllers. ([#31](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/31), @g2h0)
- Build the menu-shortcut checks required by the macOS test suite. ([#26](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/26), @g2h0)

Thanks to @lucianodato and @g2h0 for these contributions.

## 0.9.0 - GT4 cars, restored interiors and rendering fixes

### GT4 cars

- Add 722 selectable GT4 cars and 388 model replacements for existing GT2 entries. GT2 and GT4 cars can race together.
- Browse and buy added cars through **GT4 Showroom** in Simulation. Existing cars and career progress are preserved; the Simulation garage retains its original 100-car limit.
- Add the available catalogue to a separate Arcade garage through **Settings > Cheats > Arcade - add all cars to garage**. The extended Arcade garage supports up to 4,096 entries and saves independently of the career card.
- Restore body paint variants, materials, lights, wheel positions and tyre dimensions. Fix disappearing rims and incorrect wheel colours when changing body paint.
- Show the selected car's name when its original title graphic is unavailable. Fix clipped garage names and another car's logo appearing in previews.
- Move the GT4 model switch to **Graphics**. Replacements default to enabled and remember the saved preference. Added GT4 cars keep their matching models when replacements for original GT2 cars are disabled.
- Include GT4 import and installation tools in the source kit. Installation checks the prepared files, backs up replaced models and preserves saves and settings.

### Cockpits and VR controls

- Restore original interiors from **66 GT4 models across 89 catalogue entries**. Cars with restored cabins display **ORIGINAL INTERIOR** in Arcade and GT Mode previews. See [GT4 cars](README.md#gt4-cars) for the complete list and tested disc revision.
- Restore missing seats, dashboard and door trim, including the Toyota SERA interior. Preserve open racing cockpits and unusual steering controls instead of fitting them with a closed cabin.
- Correct driving-side and steering alignment. Remove the duplicate original steering rim when the interactive wheel is displayed.
- Improve generated dashboards, seats, roof lining, pillars and door panels. Fix protruding dashboard corners, gaps around windscreens and bonnets, missing roof surfaces and interior panels showing through the body.
- Correct driver positions and seat adjustment to fit the visible car. Keep the seat aligned with the camera and respect roof clearance.
- Remove opaque or excessively tinted windscreen and side-window covers while preserving body panels, window frames and interior trim.
- Improve live side-mirror placement and visibility for GT2 and GT4 cars; preserve exterior mirror paint.
- Keep the cockpit, tracked hands, virtual steering wheel and race HUD visible correctly in both eyes. Fix raised terrain showing through cabin surfaces.
- Add steering sensitivity, a centre dead zone and independent Hold/Toggle grip for each hand. Improve fine steering corrections and preserve them in replays.
- Allow PCVR Touch controls and a desktop gamepad to work together without an unheld virtual wheel overriding gamepad steering.
- Add **Original (hidden car)** to the camera cycle and remember the camera across races, retries and launches.

### Reflections, wheels and roads

- Remove the bright reflection film covering GT2 wheel arches.
- Fix broken reflection bands on GT4 bodywork in desktop and VR views.
- Improve reflection colour on dark paint and preserve night lighting and nearby scenery.
- Keep GT2 wheels within their wheel arches during steering and suspension compression, including the Arcade RUF CTR2.
- Correct GT4 body height and tyre contact on level roads and slopes. Fix GT2 cars sinking into the road after landing from a jump.
- Keep car shadows on the road through climbs, crests and slope transitions.
- Fix walls from overlapping track sections appearing through another road and flickering caused by overlapping asphalt.

### Performance and startup

- Reduce heavy tyre-smoke slowdown on supported VR hardware while preserving smoke density and the resolution of cars, scenery and HUD.
- Improve texture handling in busy scenes and reduce stalls when position badges change during overtakes.
- Improve rendering efficiency for imported cars and scenery.
- Reduce Arcade startup delays with the GT4 catalogue installed. Show a loading screen immediately after selecting a disc.
- Fix the prepared Windows launcher failing to start with an empty PowerShell path.
- Remove test launchers and duplicate desktop shortcuts. **PLAY.bat** is the single desktop launcher; PCVR retains its runtime-specific choices.

### HD media and installation

- Automatically prepare missing Python, Pillow, NumPy and .NET tools for GT4 import and installation on Windows. Clearly identify the GT4 ISO and GT2 Simulation/Arcade discs, find bundled exporters and create the installable model bundle automatically.
- Fix dependency extraction failing on long file paths in Windows PowerShell 5.1, without requiring changes to Windows settings.
- Install imported GT4 cars directly on the selected Quest or PC/PCVR destination. Automatically transfer, unpack and verify Quest files; reuse prepared bundles after an interrupted transfer without converting again. Keep the window open and save the result and import log.
- Show named progress and completed-picture counts during HD preparation. Resume completed batches after an interruption.
- Offer Original, HD menus, or HD menus and movies before processing. Preserve prepared movies when updating menus.
- Improve small lettering and logos in enlarged pictures. Fix dots and pinholes in GT Mode text, including My Home.
- Update the complete package with the current source kit, Windows/PCVR and Quest builds, browser files and platform installation scripts.

GT4 conversion requires a compatible disc image; converted game assets are not
included in the download. GT4 cars use the GT2 vehicle simulation and do not
reproduce GT4 handling exactly. The browser version does not load the native GT4
model pack. **Generated cockpits can fit some cars incorrectly; the full catalogue
has not been checked manually.**

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
