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
