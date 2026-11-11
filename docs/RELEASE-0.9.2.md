## 0.9.2 - Installation, car lights and HUD fixes

### Game fixes

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


