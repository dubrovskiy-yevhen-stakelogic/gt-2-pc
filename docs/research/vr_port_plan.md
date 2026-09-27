# VR architecture

GT2 uses a shared Vulkan/OpenXR renderer for Windows PCVR and standalone Quest.
Player setup and controls are documented in [PCVR](../PCVR.md) and
[Quest](../QUEST.md); checked hardware and automated coverage are listed in
[validation](../VALIDATION.md).

## Platform boundary

`tools/gt2game/game_window.h` exposes frame timing, input and drawing to game
screens. Windows, SDL, Windows OpenXR and Android backends implement this
interface. OS paths live in `src/platform/os/`, while audio backends live in
`src/game/audio/`. Simulation, formats and the PS1 controller model are shared.

`src/platform/xr/xr_session.*` owns the OpenXR instance, system, session,
reference spaces, swapchains and frame lifecycle. It creates the Vulkan
instance/device through `XR_KHR_vulkan_enable2` and supplies them to the scene
renderer through `VkContext`. Android provides the Java VM and activity to the
loader. The platform backend handles lifecycle and focus changes.

## Stereo rendering

The camera rig in `src/platform/xr/vr_rig.*` composes the race camera, horizon
adjustment, seat offset and predicted eye pose:

```text
world_from_eye = camera * horizon * seat * local_from_eye
```

Simulation runs at 30 Hz. Presentation interpolates the race camera and vehicle
poses between simulation steps; it uses the runtime's predicted head pose
without interpolating it. Coordinates use metres and the OpenXR right/up/back
convention. Each eye has an asymmetric field of view and a reversed-Z projection.

The draw list is built once relative to the midpoint between the eyes. World
items use each eye's view-projection matrix; sky items omit eye translation.
The renderer supports multiview and two-pass stereo. Menus and movies use a
world-locked cinema quad, while races use a projection layer with depth when the
runtime supports it. Two-player split-screen stays on the cinema screen.

HUD placement is shared between the eye views. The cockpit mirror samples one
rear-camera image for both eyes. See [cockpit rendering](../COCKPIT.md#mirror-and-rendering)
for its geometry and controls. Texture caching, mipmaps, MSAA and fixed peripheral
foveation are implemented in the Vulkan renderer. Foveation is not eye tracking.

## Input and audio

`xr_actions.*` binds Touch actions and haptics. `vr_driving.*` implements Stick,
Virtual wheel and Motion driving. The resulting controls feed the shared PS1
pad path. Graphics, HUD and control preferences are shared between discs;
memory cards and career progress remain separate.

Android uses AAudio and Windows uses the Windows audio backend. The platform
window pauses simulation and audio for lifecycle/focus interruptions. The
interactive VR wheel and hands follow the vehicle body while controller input
remains in seated coordinates.

## Android packaging and data

The Gradle project is under `android/`; it builds the native ARM64 game and
packages the OpenXR loader. See [Quest build instructions](../QUEST.md) for the
toolchain and build commands.

The installer reads supported disc images on the computer, extracts assets and
copies the data to the application. The runtime uses extracted assets and raw
disc sectors; it does not require a fully predecoded texture/audio pack. Game
data, BIOS and saves are not bundled with the APK. Optional HD preparation is
documented in [HD media](../HD-MEDIA.md).

## Verification

[xrsim](xrsim.md) provides scripted poses, Touch input, focus changes and stereo
captures on Windows. Unit checks cover camera maths and driving controls;
integration checks cover startup, settings and disc switching. Simulator checks
do not measure headset comfort or sustained performance. Device testing must
also exercise controller tracking, recentering, pause/resume and saving.
