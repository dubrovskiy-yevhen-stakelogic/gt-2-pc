# GT2 for macOS

The native desktop game includes Arcade, Simulation, movies, sound, controller
support and the cockpit view. Installation builds from the included source.

## Install

Extract **GT2-0.8.0.zip** into Downloads. Close an existing GT2 instance,
then open **INSTALL-MACOS.command**, or run in Terminal:

```sh
bash "$HOME/Downloads/GT2-0.8.0/INSTALL-MACOS.command"
```

This archive installs from source. The first run prepares Apple Command Line
Tools, Homebrew, CMake, Python, Vulkan/MoltenVK and SDL 2.32.10, builds the game
and runs the checks. macOS may request an administrator password for tools.
It then packages the runtime dependencies into **~/Applications/GT2.app**.
The completed app does not need a separate Vulkan SDK to play.

The build requires macOS 14+ and a native arm64 or x86_64 terminal session.
Rosetta builds are not supported. The actual app minimum OS version is recorded
from its bundled libraries and may be higher than 14.

If installation stops, the previous app may remain installed. Logs are under
`~/Library/Caches/GT2/build-arm64` or `build-x86_64`. Updates retain installed
discs and saves, and keep the previous application as a backup.

Open GT2.app or **PLAY-MACOS.command**. On first launch, select your own supported
Arcade and/or Simulation BIN/CUE images. Use a single-track MODE2/2352 image;
extract compressed archives before selecting them. No disc image, BIOS or save
is supplied with the release.

To add the other disc later, open **Shift+Q → Import another game disc**. Both
discs can be selected together. Importing one disc preserves the other disc
and existing saves.

## Controls

- **Shift+Q**: settings, using the physical Q key with either keyboard layout.
- **Option+Enter**: toggle fullscreen. `--windowed` starts in a window.
- **Shift+Q → Quit game → Quit game**: close the application.
- Arrows, Enter, Space and Backspace: directions, Cross, Circle and Triangle.
- Forward Delete / Fn+Delete: Square. S: Start. C: camera.
- Standard gamepads: buttons, sticks, analogue triggers and ordinary rumble.

Fullscreen uses a desktop-sized window. F10 remains available but may be
intercepted by macOS. Windows wheel/force-feedback drivers, adaptive DualSense
triggers and VR are not supported by the Mac port.

## HD pictures, text and movies

Install the release and import your discs first. Close GT2, then open
**PREPARE-HD.command**, or run:

```sh
bash "$HOME/Downloads/GT2-0.8.0/PREPARE-HD.command"
```

Choose pictures, menu text and HUD, or also full-screen Arcade movies.
The command obtains the official macOS Real-ESRGAN package and uses the native
gt2media tool included with the application. Preparation works on your installed
discs, retains originals and saves, and backs up replaced HD packs. Movies need
substantial processing time and tens of GB of working space. Finished work is
reused from `.hd-work` in the game-data directory. Existing prepared movies and
PlayStation startup files are preserved when preparing pictures only.

Enable **Shift+Q → Graphics and performance → HD textures and media** to use
the results. This covers interface artwork and movies; car and track textures
remain original. BIOS startup capture is not part of this Mac command. See
[HD media coverage](HD-MEDIA.md).

For automation: `PREPARE-HD.command --media Menus` or `--media MenusAndMovies`.
`--upscaler /path/to/realesrgan-ncnn-vulkan` uses a local copy with its `models`
folder beside it. `--gpu -1` selects CPU processing. `--runtime` and `--tools`
override the installed data and native tool directories.

## Data and diagnostics

Game data lives in `~/Library/Application Support/GT2`, with separate `arcade`
and `simulation` folders, plus `saves` and `logs`. To copy the game log to Downloads:

```sh
cp "$HOME/Library/Application Support/GT2/logs/gt2game.log" "$HOME/Downloads/gt2game.log"
```

`GT2_DATA_ROOT` and `GT2_SAVE_ROOT` override the defaults when launching the
native app executable directly. The source diagnostics
`scripts/check-macos-render.sh` and `scripts/check-macos-colour.sh` use temporary
profiles and write capture ZIPs to Downloads. The colour comparison explicitly
tests working and failing driver settings; it does not change normal play.

## Building and distributing the app

**BUILD-MACOS.command** builds with already installed dependencies.
`GT2_BUILD_DIR`, `GT2_SDL_PREFIX` and `GT2_VULKAN_PREFIX` override tool locations.
Use `-DGT2_BUILD_MACOS_CLIENT=OFF` for headless tools only.

The installer also produces an app ZIP and DMG under the extracted source's
`dist` directory. Local builds use an ad-hoc signature and are not notarized.
A prebuilt public app needs the maintainer's Developer ID certificate and an
Apple notarytool keychain profile:

```sh
bash INSTALL-MACOS.command \
  --identity 'Developer ID Application: YOUR NAME (TEAMID)' \
  --notary-profile 'gt2-notary'
```

These explicit options upload the packaged app and DMG to Apple for notarization.
Credentials are not stored in the package. The packager checks nested libraries,
signatures, minimum OS versions and notarization before reporting success.
