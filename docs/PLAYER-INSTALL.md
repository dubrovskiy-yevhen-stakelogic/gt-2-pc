# GT2 VR 0.1.0 player installation

## Quest 3

1. Extract the complete release ZIP into a writable folder on Windows.
2. Enable Quest developer mode, connect USB and accept USB debugging inside the headset.
3. Run **INSTALL.bat**. Select your supported Arcade and/or Simulation image. You can select both at once; select either its BIN or CUE, not both copies of the same disc.
4. The installer validates package hashes, finds/downloads ADB, extracts and validates your disc data, installs the signed APK and verifies the headset's raw-disc hash and application access.
5. Open **GT2 VR** in Unknown Sources. Select a disc with the stick and A. A/B skips movies.

The headset runs independently of the PC after installation. The default PC staging folder is `%LOCALAPPDATA%\GT2-VR\runtime`; keep it to speed up later updates. Allow several GB per disc for raw data, extracted assets, temporary staging and previous-install backups. A 7z input requires 7-Zip; the installer can install it through WinGet. ADB's pinned download comes from Google; its terms are at https://developer.android.com/studio/terms.

## Windows PC

Run **INSTALL-PC.bat** with the same images. It uses the included Windows executables and needs no build toolchain. Open the generated **PLAY-arcade.bat** or **PLAY-simulation.bat** in the install folder. Press F10 for graphics, vibration and Arcade cheats. A Vulkan 1.3 graphics driver is required. DualSense adaptive pedals and vibration are supported through the native physical USB/Bluetooth device.

## Existing installations and saves

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

Menu pauses; both grips + Menu opens VR settings. Stick up/down selects a setting; triggers change it. Start with the saved/default graphics settings, then watch APP FPS while adjusting them. Eye resolution needs an app restart. The selected 90 Hz refresh rate is a target, not a guarantee of 90 application FPS. See QUEST.md for all controls and cheats.