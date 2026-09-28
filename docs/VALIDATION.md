# Platform compatibility

| Platform | Requirements and supported setup |
| --- | --- |
| Windows | Native desktop game; OpenXR for PCVR through SteamVR, Meta Link or Virtual Desktop |
| Quest | Standalone ARM64 application; installation and controls in [QUEST.md](QUEST.md) |
| macOS | Native Apple Silicon or Intel build, SDL2 and Vulkan through MoltenVK; see [MACOS.md](MACOS.md) |
| Steam Deck / Linux | SDL2 and Vulkan; Steam Deck installation uses Desktop Mode and Flatpak |
| Browser | WebAssembly and WebGL2, sufficient RAM for the full BIN, 8192-pixel textures and 512 array layers |

Platform-specific controls and installation requirements are documented in the
linked guides. Native VR, physical-wheel force feedback and optional HD media
are not available on every platform. See [browser limits](WEB.md).

Cockpit geometry follows the original low-polygon car body; unusual cars or
extreme seat positions may need manual seat adjustment. Browser performance
depends on the graphics driver and memory limits. Steam Deck Gaming Mode and
suspend/resume behavior may differ from Desktop Mode.

When reporting a problem, include the release version, platform, graphics device,
disc revision and the steps needed to reproduce it. Do not attach disc images,
firmware, credentials or personal saves to a public report.
