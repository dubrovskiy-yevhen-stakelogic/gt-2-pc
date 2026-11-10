# Player installation

Download the file for your platform from the latest release. The packaged builds
need no compiler, SDK or dependency installation. Game data and firmware are not
bundled or downloaded; supply your own supported BIN/CUE images.

| Platform | Release file | Installer / entry point |
|---|---|---|
| Windows monitor / PCVR | `GT2-<version>-windows-x64.msi` | `GT2` shortcut |
| Windows portable | `GT2-<version>-windows-x64.zip` | `INSTALL-PC.bat` |
| Quest standalone | `GT2-VR-<version>.apk` | Headset installer or ADB |
| macOS | `GT2-<version>-macos-<arch>.dmg` | Drag GT2 to Applications |
| Linux desktop / Steam Deck | `GT2-<version>-steamdeck.flatpak` | `flatpak install --user <file>` |
| Browser | `GT2-Web-<version>.zip` | Serve the contents over HTTP(S) |

The same x86_64 Flatpak runs on Steam Deck and other Flatpak-enabled Linux
desktops; `INSTALL-STEAMDECK.sh` adds the Deck desktop shortcut and preset. The
tagged source archive remains available for building any platform. No old
release is needed for a different platform.

Supply your supported complete BIN/CUE images. See [disc support](EUROPEAN-DISCS.md).
A 2048-byte ISO lacks required audio/video sectors. Game data and firmware are not
bundled or downloaded. Select one image per disc, not both its BIN and CUE.

## Windows and PCVR

Run the MSI and choose the program folder. The first launch asks where to keep
discs, saves and HD media; choose another drive there if C: is short on space.
Keep the data folder outside the program folder: uninstalling GT2 removes the
program files but must not remove your discs or saves.

Release MSIs are not code-signed yet; if SmartScreen appears, compare the file
with `SHA256SUMS-windows-x64.txt`, then choose **More info → Run anyway**. The
portable ZIP runs the same first-run flow from `INSTALL-PC.bat`. Pass
`-InstallDir "D:\GT2"` to skip the folder prompt, or edit the recorded
`install-location.txt` later. Installation verifies package hashes, prepares
data and creates launchers in the data folder. Use `GT2.bat` or `PLAY.bat` for
monitor play, or the META, STEAMVR or VD launcher for your OpenXR runtime.
See [PCVR setup](PCVR.md). The headset software must be installed separately.
Settings open with F10; in the browser use Shift+Q.

## Quest

Install `GT2-VR-<version>.apk`. The APK contains the game but not disc data:
enable developer mode, connect by USB and use the installer scripts to prepare
your own images on the headset. Installation updates the app in place, preserves
saves, verifies copied disc data and does not launch the game. Open **GT2 VR**
under **Unknown Sources** yourself. If signatures differ, export your saves and
resolve the signing identity before replacing the installed application.

For Linux-to-Quest setup use `INSTALL-LINUX.sh` and the
[Linux instructions](LINUX-INSTALL.md). Steam Deck desktop play uses a different
entry point, `INSTALL-STEAMDECK.sh`.

## Mac, Steam Deck and browser

Open the DMG and drag GT2 to Applications; no build tools are needed. Install the
Flatpak on a Steam Deck or any Flatpak-enabled Linux desktop, then open the GT2
shortcut and select your disc. Follow [macOS](MACOS.md), [Steam Deck](STEAMDECK.md)
or [browser](WEB.md) for first-run details. To deploy the browser game, upload the
contents of `web/` together. HTML requires its adjacent JS and Wasm files. Saves
stay in the browser's storage for the site address; export a backup before
clearing storage or moving to another address.

## Optional media and saves

Native installers can prepare HD media from your disc; see [HD media](HD-MEDIA.md).
A BIOS is optional and only used to prepare the PlayStation startup. Skip it for
normal gameplay. The game never needs a bundled BIOS.

Use the supplied transfer scripts for [PC/Quest save exchange](SAVE-TRANSFER.md).
Close the game on both devices before transferring. Browser backups use Export
saves and Import saves. Keep backups before changing devices or clearing storage.
