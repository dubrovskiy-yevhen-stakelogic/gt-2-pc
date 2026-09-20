# Validation — public release 0.1.0

Checks performed on 2026-09-21. Build verification and automated PC tests are separate from headset acceptance.

## Completed checks

- Windows Release build from a new build directory using the source install target; all four CTest suites passed (core regression, VR rig, driving controls, Quest career features).
- Disc-specific regression checks: 55 on each Arcade disc and 52 on each Simulation disc. These include publisher/warning decoding, music pause/resume sample equality, Arcade menu text, matching movie/preview AC tables and frame decoding, Simulation menu/career loading and regional attract-resource resolution.
- European Arcade and Simulation were installed from the user's complete raw BIN images into an isolated test directory. The installer extracted 11,292 and 11,578 assets respectively, verified copied disc SHA-256, scanned 126 courses per disc and passed the six-car physics self-test. Additional menu/media checks passed on the installed loose-data paths.
- Automated Windows screenshots checked European Arcade title, selection menus/car reflections and a race start; European Simulation title, GT Mode home and the first licence test. The European intro decoded all 4,374 frames; a course preview decoded all 419 frames without errors.
- Android ARM64 debug and release builds completed. Public APK metadata is versionName 0.1.0, versionCode 13, non-debuggable. APK signature and alignment are checked by the release scripts. Only the game and OpenXR native libraries are included; there is no disc-data assets directory.
- Positional billboard tests cover viewer position, height independence and the coincident-position fallback. The shared stereo viewer position replaces head-right orientation for tree billboards.
- PowerShell scripts were parsed, and the source audit rejects game payloads, build outputs, keys and agent instruction files. Player packaging validates every file hash and rechecks every ZIP entry against the assembled folder.

The published source is a separate ordinary folder/repository. Private retail images, captures, profiling logs, older internal planning documents and signing keys are not part of it.

## Performance evidence

The preserved user session at 175% showed CPU scene preparation around 0.4–0.8 ms, with GPU averages commonly around 9–12 ms and higher peaks. The GPU remains the main limit in that recording. A 90 FPS frame budget is 11.11 ms including work beyond the game's measured GPU region. Selected refresh rate and average FPS do not describe isolated long-frame stalls.

Holding the virtual wheel now uses a fixed closed-fist pose, avoiding pressure-driven mesh updates while held. No new measured GPU speedup or sustained 175%/90 FPS claim is made for 0.1.0. Existing texture caching, staged uploads, mirror clipping and foveation improvements remain in place. Settings are retained; the release does not force a lower resolution onto existing installations.

## Headset acceptance and remaining coverage

The release tester accepted the latest Quest build, including the tree and held-fist fixes. Its selected settings are now the initial Quest preset (150%, 80 Hz, MSAA 2x, medium foveation). The checklist below remains useful for regression checks and broader disc coverage; acceptance is not a claim that every career branch has been played through.

- In the first braking licence, stop moving and rotate the head: trees should retain their orientation. Inspect both eyes while driving past them.
- Grab the wheel with each hand. Both fists should remain closed while alternately applying/releasing accelerator and brake; releasing a grip should restore ordinary finger poses.
- Check Motion steering in both directions with the grip held, independently of the accelerator.
- Pause a rally race: timer, music and road sound should all stop. Check the HUD/mirror, saved visibility controls and settings after restart.
- Install/test both European discs through the player installer on a compatible public-signature installation; complete races, licence/career progress, saves and cheats. Automated smoke tests do not establish every branch of a full playthrough.

The full PlayStation BIOS animation/audio remains unimplemented. Wider VR views can expose missing original geometry; trees are still flat billboards. Two-player VR uses the theatre screen. Loading/start stalls and sustained high-resolution performance remain release limitations.
A compatible debug-signed 0.1.0 test update was installed on the connected Quest. Its installed APK hash matched the local file, and all six existing settings/backup files retained their hashes. The game was not launched and headset disc data was not copied. This verifies deployment, not the remaining headset behavior checks.
