# GT2 0.4.0 player installation

**GT2-0.4.0.zip includes all three versions:** Windows PC on a normal monitor, Windows PCVR and Quest 3 standalone. Choose the installer for how you want to play; you do not need a headset for the flat PC version.

## Quest 3

1. Extract the complete release ZIP into a writable folder on Windows.
2. Enable Quest developer mode, connect USB and accept USB debugging inside the headset.
3. Run **INSTALL.bat**. Select your supported Arcade and/or Simulation image. You can select both at once; select either its BIN or CUE, not both copies of the same disc.
4. The installer validates package hashes, finds/downloads ADB, extracts and validates your disc data, installs the signed APK and verifies the headset's raw-disc hash and application access.
5. Open **GT2 VR** in Unknown Sources. Select a disc with the stick and A. A/B skips movies.

The installation wizard asks whether to prepare the original PlayStation startup from your own BIOS. Choose **No** to play without a BIOS or that sequence. Choose **Yes** and select a matching 512 KiB BIOS dump to capture both screens and sound. The BIOS is not copied to Quest. `-NoBios` skips the question; `-Bios 'path'` enables capture explicitly. Command-line installs with `-DiscImage` do not prompt for BIOS unless it is supplied.

For **Linux / Steam Deck**, use `bash INSTALL-LINUX.sh` from the same extracted release. The complete [Linux instructions](LINUX-INSTALL.md) require no Wine or SteamOS system changes.

The headset runs independently of the PC after installation. The default PC staging folder is `%LOCALAPPDATA%\GT2-VR\runtime`; keep it to speed up later updates. Allow several GB per disc for raw data, extracted assets, temporary staging and previous-install backups. A 7z input requires 7-Zip; the installer can install it through WinGet. ADB's pinned download comes from Google; its terms are at https://developer.android.com/studio/terms.

## Windows PC

Run **INSTALL-PC.bat** with the same images. It uses the included Windows executables and needs no build toolchain. Open **PLAY.bat**, or **gt2game.exe** directly, in the install folder. After the optional PlayStation intro, choose your installed disc. Press F10 for graphics, controls, HUD, media and Arcade/Simulation cheats. A Vulkan 1.3 graphics driver is required. DualSense adaptive pedals and vibration are supported through the native physical USB/Bluetooth device.

Windows installation prepares **HD pictures, fonts and HUD for every installed disc**, including Simulation. A Vulkan-capable GPU and time for offline preparation are required. No game pictures are bundled: they are generated from your own disc. Use `-HdMedia Original` to skip this step, or `-HdMedia MenusAndMovies` for additional full-screen movies.

## Existing installations and saves

For Windows PCVR, run **INSTALL-PCVR.bat**, then use **PLAY-PCVR-META.bat** for Meta Quest Link / Air Link, **PLAY-PCVR-STEAMVR.bat** for SteamVR / Steam Link, or **PLAY-PCVR-VD.bat** for Virtual Desktop (VDXR). Start the selected headset connection first, then choose your disc in the game. **PLAY-PCVR.bat** retains automatic selection (running SteamVR, otherwise the system default). **L3 + R3** opens VR settings; **both grips + Menu** also remains available. See [PCVR setup and controls](PCVR.md).

Version 0.3.0 adds **TRANSFER_QUEST_SAVES_TO_PC.bat** and **TRANSFER_PC_SAVES_TO_QUEST.bat** for USB transfers of both disc memory cards. Close the game on both devices, connect Quest, then run the desired helper. Each replacement is validated, backed up and verified. The public Quest APK must be updated to 0.3.0 first; see [save-transfer instructions](SAVE-TRANSFER.md).

The installer updates a compatible APK with `adb install -r`; it never uninstalls or clears app data. If Android reports `INSTALL_FAILED_UPDATE_INCOMPATIBLE`, the installed application uses a different signing key. Stop and export its saves/settings before considering a manual migration. Development and public APKs use different keys. Save export from an old development build can use `adb shell run-as io.github.gt2pc.quest`; this is unavailable on the non-debuggable public build.

Quest disc data is in `/sdcard/Android/data/io.github.gt2pc.quest/files/{arcade,simulation}`. Saves and settings are separate from the installer-managed disc files. PC saves are under the install folder's `saves/`. Reinstalling disc data retains saves. The installer does not start the game or alter your graphics preferences.

## Command line

```powershell
.\scripts\install-player.ps1 -Target Quest -DiscImage 'D:\Arcade.bin','D:\Simulation.bin'
.\scripts\install-player.ps1 -Target PC -DiscImage 'D:\Arcade.bin' -InstallDir 'D:\Games\GT2'
.\scripts\install-player.ps1 -VerifyOnly
```

Use `-Adb` for an existing adb.exe and `-Serial` to choose one of multiple connected headsets. Unsupported disc hashes and incomplete 2048-byte ISO images are rejected. The full supported-disc list and features are in README.md.

## First run

Menu pauses; both grips + Menu opens VR settings. Stick up/down selects a setting; triggers change it. Start with the saved/default graphics settings, then watch APP FPS while adjusting them. Eye resolution needs an app restart. New profiles target 72 Hz; existing saved refresh settings are retained. The refresh rate is a target, not a guarantee of sustained application FPS. See QUEST.md for all controls and cheats.
