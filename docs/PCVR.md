# PCVR on Windows

GT2 VR 0.3.0 runs on a Windows PC through OpenXR. Menus and movies use a theatre screen; races use stereo rendering with tracked head movement. Standalone Quest remains a separate APK.

1. Connect your headset to the PC using your usual PCVR connection. Start its software (SteamVR, Meta Quest Link, or Virtual Desktop's VDXR).
2. Run **INSTALL-PCVR.bat** from the release and select your supported game discs. The installer includes the Khronos OpenXR loader; no SDK or compilation is required.
3. Open **PLAY-PCVR-META.bat** for Meta Quest Link / Air Link, **PLAY-PCVR-STEAMVR.bat** for SteamVR / Steam Link, or **PLAY-PCVR-VD.bat** for Virtual Desktop (VDXR). **PLAY-PCVR.bat** retains automatic selection. **PLAY.bat**, or **gt2game.exe** without arguments, starts desktop mode with the same saves. The optional PlayStation startup plays first, then a disc picker shows the installed Arcade and/or Simulation discs.

A Windows x64 PC and a Vulkan 1.3-capable graphics driver are required. An OpenXR runtime with Vulkan support must be installed and active. The game reports a startup error if the loader/runtime/headset is unavailable; it does not silently switch to desktop mode. Runtime compatibility and performance still require testing with your actual headset.

## Controls

- Touch controllers: the same Stick, Virtual wheel and Motion driving modes as standalone Quest, including tracked hands, grip-held steering and independent trigger pedals.
- Open VR settings with **L3 + R3**, or **both grips + Menu**. The stick-click shortcut avoids the Menu button reserved by some PCVR runtimes. Both shortcuts remain available on PC. Y retains its camera action while holding the wheel. L3 + R3 means pressing both sticks simultaneously, without grips.
- **Menu** alone pauses the game. **F10** is a keyboard fallback for VR settings.
- In VR settings, use stick up/down to select a row, triggers to change a value, A to open and B to go back.
- Xbox-compatible gamepads, DualSense and existing DirectInput controllers remain usable. The most recently used controller supplies input; the keyboard requires focus on the game window.

The packaged action bindings target Touch controllers. Other controller layouts may need runtime bindings or a gamepad; they have not been validated on hardware.

Eye resolution and other VR preferences are saved locally. Refresh-rate requests apply only if the runtime exposes the corresponding extension; otherwise change refresh rate in your PCVR software. Runtime render scaling also affects the resulting resolution. Use the in-game profiler when adjusting settings.

Set `GT2_XR_MIRROR=1` before launching to display the headset image in the PC window (extra GPU work). Without it, the desktop window handles keyboard/gamepad input while the headset displays the game.

## Saves and source builds

See [save transfer](SAVE-TRANSFER.md) for PC ↔ Quest exchange. Desktop and PCVR use the same per-disc cards on the PC.

Source builds include PCVR in `gt2game.exe`. Build normally, then place the official x64 Khronos `openxr_loader.dll` beside the executable and pass `--vr`. Player packaging takes `-OpenXRLoader <path>`; it bundles this DLL and its licence. The tested loader is Khronos OpenXR SDK Source 1.1.58. The development simulator is optional (`-DGT2_BUILD_XRSIM=ON`) and is not included in player packages.

## Runtime selection

The Meta, SteamVR and VD launchers select the named runtime for the game process and clear an inherited `XR_RUNTIME_JSON` override locally. The Meta launcher discovers the Meta Horizon / Oculus runtime independently of running SteamVR and the system default. It does not fall back to another runtime when Meta is unavailable. None of these launchers changes the Windows OpenXR registry setting.

With **PLAY-PCVR.bat** or a direct executable launch, an explicit `XR_RUNTIME_JSON` is respected. Otherwise, running SteamVR is selected before the system default. Advanced overrides are `GT2_XR_RUNTIME=meta`, `steamvr`, `vdxr`, `system` or `auto`. The VD launcher explicitly selects `vdxr`; `system` uses the current Windows OpenXR default. The SteamVR choice requires SteamVR to be running.

A headset-unavailable error includes the runtime name. Connect and wake the headset in that runtime before relaunching. The presence of SteamVR on the PC alone does not establish that its headset connection is ready.

## Shared settings

Both discs use `saves/vr-settings.txt` for common graphics, controls, HUD and media preferences. Memory cards and earned progress stay under `saves/arcade` and `saves/simulation`; cheats are not copied between discs. Desktop and VR retain their own controller bindings and resolution controls. The desktop overlay contains the same non-VR sections; arrow keys/D-pad select rows, left/right change values, Enter/A opens pages, Escape/B goes back.

The dedicated VD launcher selects the bundled VDXR runtime for this process, even when SteamVR is running. Connect through Virtual Desktop before launching. The system OpenXR default is unchanged.

## Changing games

Open VR settings with **L3 + R3** (both stick clicks) or **both grips + Menu**, choose **Change game (Arcade / Simulation)**, then **Return to disc selection**. Both discs must be installed. Save progress in the original game first; unsaved progress is discarded after confirmation. The application stays running and returns to the disc picker without repeating the PlayStation startup. Each disc keeps its own memory card, and shared VR preferences remain in use.
