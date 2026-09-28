# Player installation

Extract the complete **GT2-0.8.0.zip** into a writable folder. It contains shared
source, all platform scripts, compiled Windows/Quest files under `native/` and
compiled browser files under `web/`. Keep the directory structure intact.

| Platform | Installer / entry point |
|---|---|
| Windows monitor | `INSTALL-PC.bat` |
| Windows PCVR | `INSTALL-PCVR.bat` |
| Quest standalone, from Windows | `INSTALL-QUEST.bat` |
| Quest standalone, from Linux | `bash native/INSTALL-LINUX.sh` |
| macOS | `INSTALL-MACOS.command` |
| Steam Deck / Linux desktop | `INSTALL-STEAMDECK.sh` |
| Browser | Serve the contents of `web/` over HTTP(S) |

Windows and Quest installation uses precompiled tools. Mac and Steam Deck
installation builds from the included source. The root `INSTALL.bat` remains the
Windows source-build entry point; the platform-specific wrappers above use the
precompiled package. No old release is needed for a different platform.

Supply your supported complete BIN/CUE images. See [disc support](EUROPEAN-DISCS.md).
A 2048-byte ISO lacks required audio/video sectors. Game data and firmware are not
bundled or downloaded. Select one image per disc, not both its BIN and CUE.

## Windows and PCVR

Run the chosen installer and select your disc images. It verifies package hashes,
prepares data and creates launchers in the installed game folder. Use `PLAY.bat`
for monitor play, or the META, STEAMVR or VD launcher for your OpenXR runtime.
See [PCVR setup](PCVR.md). The headset software must be installed separately.
Settings open with F10; in the browser use Shift+Q.

## Quest

Enable developer mode, connect by USB and accept USB debugging inside the headset.
Run `INSTALL-QUEST.bat`. Installation updates the app in place, preserves saves,
verifies copied disc data and does not launch the game. Open **GT2 VR** under
**Unknown Sources** yourself. If signatures differ, export your saves and resolve
the signing identity before replacing the installed application.

For Linux-to-Quest setup use the bundled `native/INSTALL-LINUX.sh` and the
[Linux instructions](LINUX-INSTALL.md). Steam Deck desktop play uses a different
entry point, `INSTALL-STEAMDECK.sh`.

## Mac, Steam Deck and browser

Follow [macOS](MACOS.md), [Steam Deck](STEAMDECK.md) or [browser](WEB.md) instructions.
To deploy the browser game, upload the contents of `web/` together. HTML requires
its adjacent JS and Wasm files. Saves stay in the browser's storage for the site
address; export a backup before clearing storage or moving to another address.

## Optional media and saves

Native installers can prepare HD media from your disc; see [HD media](HD-MEDIA.md).
A BIOS is optional and only used to prepare the PlayStation startup. Skip it for
normal gameplay. The game never needs a bundled BIOS.

Use the supplied transfer scripts for [PC/Quest save exchange](SAVE-TRANSFER.md).
Close the game on both devices before transferring. Browser backups use Export
saves and Import saves. Keep backups before changing devices or clearing storage.
