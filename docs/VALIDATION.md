# Validation

## Hardware I tested

| Platform | Hardware / connection | What I checked |
| --- | --- | --- |
| macOS | MacBook Pro 16-inch (2021), M1 Pro, 16 GB RAM, macOS Tahoe 26.5.1 | Colours, cars in Arcade selection, movies, fullscreen and Shift+Q settings |
| SteamOS | Steam Deck OLED, Desktop Mode | Installation, disc import, gameplay, picture quality and performance |
| Standalone VR | Quest 3 | Driving, cockpit view, Midfield scenery and European Arcade race audio |
| Windows PCVR | SteamVR / Steam Link, Meta Link and Virtual Desktop | Launching and playing through each runtime |
| Windows wheel input | Fanatec Gran Turismo DD Pro (8 Nm), Thrustmaster TH8A Shifter | Driving and weak countersteering assistance |

## Automated checks

CTest covers core regressions, menu shortcuts, wheel input, display surface
formats, asset geometry, HD media, VR camera maths, VR driving and Quest career
features. Run it against a configured and built test directory:

```sh
ctest --test-dir <build-directory> --output-on-failure
```

The scripts in `tests/` provide additional checks with local disc data:

- `check_pcvr.py`, `check_player_startup.py` and `check_game_switch.py` exercise
  startup, settings, disc selection, theatre/stereo transitions and exit through
  the local OpenXR simulator.
- `check_cockpit.py`, `check_cockpit_countdown.py` and `run_cockpit_audit.py` check
  camera views, cockpit settings, mirrors and fitted body geometry. See
  [cockpit captures](COCKPIT.md#developer-captures).
- `check_linux_disc_parity.py` compares the Python disc reader with native
  extraction. The four supported US/European disc profiles produced 45,066
  matching asset paths and payloads.
- `check_startup_audio.py` checks prepared startup playback duration with the
  current Windows audio output. It requires locally prepared media.
- Installer tests cover invalid paths, malformed input, archive integrity,
  retained saves, rollback, device authorization and signing failures. Mocked
  device tests do not check physical USB access.

The cockpit corpus check covers 1096 models, using the first paint, a default
seat and four fixed views. The renderer produces 4384 audit images. Course
checks cover detailed/scenery geometry, shadows and texture seams across all
four supported discs. These checks do not replace driving every car and course.

HD pack checks validate filenames, disc profiles, hashes, image decoding and
dimensions. Save-transfer tests cover card validation, directory checksums,
concurrent writes, backups and byte-exact replacement. Android save-provider
upload/download checks have also run on Quest; a complete public-APK
PC-to-Quest-to-PC game-load round trip remains untested.

## Remaining hardware checks

- Steam Deck: the View + Menu overlay shortcut, pause while holding Menu and
  importing a second disc from the overlay still need a device check. Gaming
  Mode, suspend/resume and the LCD model are untested.
- Mac: other models and a complete `PREPARE-HD.command` run are untested.
- Wheels: the device catalogue contains mappings, not a physical test of each
  rig. Force direction and strength should be checked with each setup.
- VR: simulator image/submission checks do not measure headset comfort or
  sustained frame rate. Sustained 175% eye resolution at 90 FPS is not achieved
  across races. See [renderer benchmarking](RENDER_BENCHMARK.md).

## Source and package checks

`scripts/audit-source.ps1 -FileSystem` checks a source folder without requiring
Git metadata. The exporter records file hashes in `SOURCE-MANIFEST.json`.
Packaging verifies copied files and archive contents, excludes game data and
private keys, and retains dependency licences. Android release packaging checks
signing, alignment and application metadata.

Builds, archive checks and simulator runs are separate from the hardware tests
listed above. Current platform instructions are in [Quest](QUEST.md),
[PCVR](PCVR.md), [macOS](MACOS.md) and [Steam Deck](STEAMDECK.md).
