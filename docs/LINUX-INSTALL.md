# Install GT2 VR on Quest from Linux or Steam Deck

This installs the **standalone Quest game**, not a Linux desktop game or a streaming setup. Use the same **GT2-0.5.0.zip** as Windows. No Wine, ISO conversion, C++ compiler, `sudo`, `pacman` or `steamos-readonly disable` is needed. Installation tools, disc staging and download caches stay in your home directory. Python 3.10 or newer is required.

## Steam Deck: step by step

1. Switch the Deck to **Desktop Mode**.
2. Download the complete release ZIP and extract it with Ark into a folder in your home directory, for example `Downloads/GT2-0.5.0`. Do not run a script from inside the ZIP viewer.
3. Extract your own GT2 disc archives with Ark too. Keep each `.cue` beside its matching `.bin`. The installer reads BIN/CUE directly; do not convert the disc to a 2048-byte ISO. That conversion loses race music and movie sectors.
4. Enable developer mode on Quest, connect it by a USB data cable, put it on and accept **Allow USB debugging**. Leave the headset connected and awake during copying.
5. In Dolphin, open the extracted release folder and choose **Open Terminal Here** (Konsole). Run:

   ```sh
   bash INSTALL-LINUX.sh
   ```

6. Enter or drag in the path to the first BIN or CUE. Optionally supply the other disc at the next prompt. Supply one image per disc, not both BIN and CUE for the same disc.
7. At the BIOS prompt, **press Enter to skip**. A BIOS is entirely optional: it is used only to prepare the two original PlayStation startup screens and their sound. If you want them, give the path to your own matching 512 KiB BIOS dump. The installer does not obtain firmware for you.
8. Wait for **Installation verified**. Open **GT2 VR** in **Unknown Sources** on Quest. Disconnect the Deck; the game runs on the headset itself.

The installer checks release-file hashes and the executable identity of both US/European discs, extracts the assets, preserves the complete raw disc for music/video, updates the APK, and checks the copied disc's SHA-256 and the application's read access. It never launches the game, uninstalls the application or clears saves/settings. Previous local disc files are backed up before replacement.

Allow several GB per disc for staging, data and previous-install backups. Data defaults to `~/.local/share/gt2-vr/runtime`, with download tools under `~/.cache/gt2-vr`; `XDG_DATA_HOME` and `XDG_CACHE_HOME` are respected. Keep the prepared data for later updates. ADB is found on PATH or downloaded from Google's pinned Linux Platform Tools archive; see [Google's SDK terms](https://developer.android.com/studio/terms).

## Command-line examples

```sh
bash INSTALL-LINUX.sh --disc "$HOME/Games/GT2/Arcade.cue" --disc "$HOME/Games/GT2/Simulation.cue" --no-bios
bash INSTALL-LINUX.sh --disc "$HOME/Games/GT2/Arcade.cue" --bios "$HOME/BIOS/scph5501.bin"
bash INSTALL-LINUX.sh --verify-only
```

Use `--runtime "/path/to/writable/folder"` for another staging location, `--adb /path/to/adb` for an existing ADB executable, or `--serial DEVICE` to select one of several authorized devices. `--prepare-only` extracts locally without accessing a Quest.

Automatic ADB and optional capture downloads target **x86_64 Linux**, including Steam Deck. Other architectures need their own ADB; optional BIOS capture also needs a compatible software core and Pillow. Missing Python is reported before installation; use a user-owned Python 3.10+ installation and call `python3 scripts/install-linux.py`, rather than changing the SteamOS system partition.

## Optional startup and HD media

Without a BIOS, both the Arcade and Simulation games work normally and start without the console startup sequence. Existing prepared intros are retained on updates; disable **PlayStation intro** in the VR menu to hide one. No BIOS or pre-recorded console startup is distributed in the release.

When a BIOS is supplied, preparation downloads a separately licensed software Beetle PSX core and, if needed, a pinned Pillow wheel into the user cache. There is no system-wide pip installation. Temporary BIOS copies are deleted after capture, and the BIOS itself is never copied to Quest. The game only receives the captured movie. A firmware/disc pairing that cannot show both complete screens is rejected. A changed upstream core download is rejected by checksum; `--capture-core /path/to/mednafen_psx_libretro.so` accepts a trusted local software core.

This Linux installer prepares **original game assets** and optional original console startup. The neural HD preparation wizard is Windows-only. If you already have a matching prepared `hd` folder in the runtime, Linux installation transfers it too. This limitation does not affect original textures, texture filtering, mipmaps or standalone VR.

## USB access

- **Unauthorized**: accept the USB debugging prompt inside Quest. Reconnect the cable if the prompt was missed.
- **No device**: check developer mode, a data-capable cable and the Deck's USB host connection. Close other installers that may be using the headset.
- **No permissions**: this is Linux USB device access, not the read-only system partition. The installer stops without changing OS configuration. If your existing Quest installation tool already has a working ADB connection, pass that tool's native ADB with `--adb`. On systems lacking suitable USB permissions, an administrator must grant device access; the installer does not silently run as root or alter udev rules. An already configured, authorized network ADB connection can also be selected with `--serial ADDRESS:PORT`.
- **INSTALL_FAILED_UPDATE_INCOMPATIBLE**: the installed app uses another signing key. Do not uninstall just to bypass this error: export saves/settings before a deliberate manual migration. Public releases use the release signing key; development APKs use a different key.

## Verification status

The portable reader is checked against Windows extraction of all four supported US/European disc profiles. Automated tests cover path validation, malformed input, retained saves, rollback after file-move failure, unauthorized ADB, signing failure and the device-copy verification flow. The ctypes BIOS host is also checked with the Windows software core and the native G2MEDIA reader. A physical Steam Deck installation remains a separate hardware check; mock ADB tests do not establish USB driver permissions on every Linux distribution.
