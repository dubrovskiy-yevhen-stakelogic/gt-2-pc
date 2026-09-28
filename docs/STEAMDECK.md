# Steam Deck / SteamOS

The native Linux game uses SDL2 and Vulkan. Install in Desktop Mode using the
included Flatpak scripts. `INSTALL-LINUX.sh` installs the Quest game from a Linux
computer; use `INSTALL-STEAMDECK.sh` for the desktop game.

## Install on Steam Deck

1. Switch to **Desktop Mode** and extract **GT2-0.8.0.zip**
   into a writable folder. Open the extracted folder, not the ZIP preview.
2. Double-click **INSTALL-STEAMDECK.sh** and select **Execute**. A terminal window
   opens automatically and stays open after completion or an error.
3. Wait for **GT2 INSTALLED**. Open the new **GT2** desktop shortcut to choose
   your disc. If KDE asks whether to trust the shortcut, allow it to launch.

No commands need to be typed. The installer downloads the build tool and
Freedesktop 25.08 SDK/runtime from Flathub, compiles the game, runs its tests and
installs the resulting package. Allow several GB of free space and keep the
window open during downloads and compilation. No `sudo`, `pacman` or changes
to the SteamOS system partition are needed.

Build files and the package cache live in `~/.cache/gt2/steamdeck-build` (or under
`XDG_CACHE_HOME` when set). The extracted installer can be on a removable drive.
No FUSE setup is required. Installed Flatpak tools and runtimes remain available
for subsequent builds.

If a prebuilt **GT2-0.8.0-steamdeck.flatpak** is included beside the installer,
it installs that package directly without downloading build tools or compiling.
The complete release contains source, so its first installation builds locally.

If installation stops, the window shows the failed step. The full log is
**GT2-SteamDeck-install.log**, beside **INSTALL-STEAMDECK.sh**. Send that file
with the error report. Re-running the installer keeps installed discs and saves.

The package includes the game, disc importer and media tools. Runtime libraries
and graphics drivers are supplied through Flatpak. Updates retain application
data. Uninstalling with `--delete-data` removes it; do not use that option if you
want to keep your discs and saves.

## Discs and Gaming Mode

On the first launch, choose your own supported **raw 2352-byte BIN or ISO** in the
file picker. For a BIN/CUE pair, select the BIN. Extract ZIP/7z archives first.
A 2048-byte ISO omits required audio/video sectors and cannot be used. Supported
disc revisions are listed in the main README; the importer verifies the executable
hash. Game images and BIOS are not downloaded or included.

The installed desktop launcher uses KDE's file picker and passes only the selected
image into Flatpak with read-only access for that launch. The importer copies
the image into the app's data directory. Existing discs and saves are kept if
extraction fails. Adding the
Simulation disc keeps Arcade installed, and vice versa. Importing an already
installed disc is rejected without replacing it.

To add the second disc, open **View + Menu -> Import another game disc...**,
confirm, and choose the image in Desktop Mode. You can also double-click
**IMPORT-DISC-STEAMDECK.sh** in the extracted installer folder. Save in the game
before importing or changing discs: both actions close the current session.

The installer copies its launch scripts into `~/.local/share/gt2-launcher` (or
`$XDG_DATA_HOME/gt2-launcher`) and creates the **GT2** desktop shortcut. You can
remove the installer folder afterwards. Use that shortcut for the first
launch and disc import. A direct `flatpak run` still uses the in-app file picker.

For Gaming Mode, right-click the updated **GT2 desktop shortcut** and choose
**Add to Steam**. Select the **Gamepad** controller template and leave forced
compatibility tools disabled. If an older Steam shortcut launches Flatpak directly,
replace it with this desktop shortcut so disc import uses the same launcher.
Gaming Mode operation has not yet been checked on the device.

## Steam Deck controls

These mappings assume that Steam Input presents a gamepad. A Desktop Mode
keyboard/mouse layout has its own assignments. In Steam's controller settings,
use a gamepad layout for GT2; do not enable repeated/turbo presses for Menu.

| Button | Action |
| --- | --- |
| Menu (small button above ABXY) | Original race pause; press again to resume. Holding it must not repeat. |
| View + Menu (two small buttons beside the screen) | Open/close the GT2 settings overlay. Either press order works. |
| D-pad up/down | Select an overlay row. |
| D-pad left/right | Change the selected setting. |
| A | Open/confirm in the overlay. |
| B or Y | Back in the overlay; from its first page, resume the game. |
| Menu while the overlay is open | Resume the game. |
| A / B / X / Y in original game menus | PS1 Cross / Circle / Square / Triangle. |
| L1 / R1, L2 / R2, stick clicks | Original PS1 shoulder buttons and L3/R3. |
| Left stick | Steering with the original analogue-controller profile. |

The overlay includes **Graphics and performance**, **HUD elements**, **Controls**,
**Cockpit / driver view**, **Change game (Arcade / Simulation)**,
**Import another game disc...**, **Resume game** and **Quit game**.
**Change game** returns to the installed-disc selector. **Quit game** closes GT2
normally; it does not return to the disc selector. Changes to graphics and
controls are saved automatically; career progress uses the game's save system.

Driving buttons follow the original game's controller settings until you enable
**Controls -> Gamepad bindings: Custom**. The default custom layout is:

| Button | Driving action |
| --- | --- |
| Left stick | Steering |
| R2 / L2 | Analogue accelerator / brake |
| B | Handbrake |
| Y | Reverse |
| R1 / L1 | Shift up / down with manual transmission |
| Right stick click | Change camera |
| Left stick click | Look back |

You can change these in **Controls -> Button bindings**. They do not change menu
navigation. Steam Input can map a rear button (for example L4) to **F10** as a
single-button overlay shortcut. F10 and Shift+Q also work from a keyboard.
Trackpads or the touchscreen operate the Desktop Mode file picker.

## Data and diagnostics

Flatpak stores data under:

```text
~/.var/app/io.github.gt2pc.GT2/data/GT2/
    arcade/
    simulation/
    saves/
    gt2game.log
```

Copy the log to Downloads after closing the game:

```sh
cp "$HOME/.var/app/io.github.gt2pc.GT2/data/GT2/gt2game.log" "$HOME/Downloads/GT2-SteamDeck.log"
```

The settings FPS profiler can record frame timings. Keep the original media for
the first test. Prepared HD packs may be copied into the matching disc's `hd`
folder after closing the game. A Deck-native HD upscaling command is not included.

For bug reports, include your Deck model, Desktop or Gaming Mode, the selected
disc and a log. For performance problems, also include the graphics settings
and a profiler recording.

## Other Linux systems

Install a C++20 compiler, CMake 3.22+, Ninja, SDL2 2.26+ development files, GTK3
development files and Vulkan 1.3 development files including `glslc`. Then run
`bash scripts/build-linux.sh`. Launch `build_linux/gt2launcher`; data defaults to
`$XDG_DATA_HOME/GT2` or `~/.local/share/GT2`. Headless tools remain buildable with
`GT2_BUILD_LINUX_CLIENT=OFF`.

References: [Valve Steam Deck FAQ](https://partner.steamgames.com/doc/steamdeck/faq),
[Flatpak build guide](https://docs.flatpak.org/en/latest/first-build.html),
[Flatpak permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html).
