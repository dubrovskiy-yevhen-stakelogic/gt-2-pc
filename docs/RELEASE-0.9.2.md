## 0.9.2 - Installation, car lights and HUD fixes

### Game fixes

- Apply the configured music and sound-effect volumes to Arcade, Simulation and title menus. ([#48](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/48))
- Keep short menu-music notes from being cut at their start and release completed voices correctly. ([#58](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/58))
- Run GT Mode event races with the correct game mode, fixing unintended damage behaviour. ([#55](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/55))
- Support Silent's combined disc with Arcade and Simulation selection, separate saves, and the correct Arcade intro, ending and menu music. ([#59](https://github.com/dubrovskiy-yevhen-stakelogic/gt-2-pc/pull/59))
- Keep HUD, menus and licence-result panels above the cockpit on the direct Vulkan path, including with MSAA disabled.
- Restore stock car lamps over their coplanar housings and use night models/textures on night courses. Cars without a night pair retain their day assets.
- Drive both split-screen players' brake lights from their own brake input.
- Fix combined-disc selection on Quest and switching between Arcade and Simulation in direct-launch sessions, keeping mode-specific save paths.
- Preserve rolling starts when entering GT Mode events with the corrected race shell.
- Show the running game version in the title of the custom settings menu on desktop, VR and browser builds.

### Release scope

This release uses the public main branch. Network multiplayer, new HD car models
and fully authored 3D cockpits are planned for future releases and remain in development.
Existing split-screen, GT4 imports and current cockpit rendering remain available.

### Installation and packaging

- Fix first-run Windows argument binding and validate standalone Arcade discs with their correct profile, preventing false installer failures.
- Ship per-platform release files: a Windows MSI, a macOS DMG, one x86_64 Flatpak for Linux and Steam Deck, the Quest APK and a portable Windows ZIP.
- Ask where to store discs, saves and media on Windows instead of always using the C: drive, and reuse the recorded location on updates.
- Let the Windows GT2 launcher import the disc on first run and start the game, and refresh the game in the chosen data folder after a launcher update without importing again.
- Skip the optional HD-media and BIOS console prompts when a launcher starts the Windows installer; original media and no BIOS are used, and HD can be prepared later.
- Apply the Steam Deck window preset from the Deck launcher so the same Flatpak behaves normally on desktop Linux.
- Name the Linux Flatpak for its architecture and keep the macOS ZIP's app bundle intact so launchers can install it.

### Steam Deck installation

Install the Linux x86_64 Flatpak with `INSTALL-STEAMDECK.sh`, then add the
installed **GT2** desktop shortcut to Steam as a non-Steam game. An existing
Gran Turismo 2 shortcut pointing to DuckStation or RetroArch still starts the
emulator; replace its launch command with the installed native launcher or use
the new GT2 shortcut. The native game's custom settings menu displays **v0.9.2**.

A clean install, an update preserving saves, both original disc imports and
launching through Steam in Gaming Mode were checked on Steam Deck. The owner
also reported the installed game working. This is not a performance benchmark.

### Game data and optional media

Game disc images and personal saves are not included. Use your own original
discs. HD media and GT4 car imports remain optional installation steps; the
release downloads do not contain the prepared media from the local test install.
