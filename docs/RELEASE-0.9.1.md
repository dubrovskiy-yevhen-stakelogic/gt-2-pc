# GT2 0.9.1

The complete archive contains shared source, Windows/PCVR executables, a signed Quest APK, compiled browser files and macOS/Linux build and installation scripts. No disc images, BIOS dumps, converted game assets or saves are included.

See [CHANGELOG.md](../CHANGELOG.md#091---cockpits-race-results-and-platform-fixes) for the full change list and contributor PR links.

## Known cockpit issues

GT4 cockpit fitting is improved but not complete. Catalogue checks still flag window overlaps on 20 models and source-body openings on 92; these sets overlap. [The follow-up checklist](COCKPIT-NEXT-RELEASE.md) and [model-level results](COCKPIT-REMAINING-0.9.1.json) preserve the remaining work.

## Updating

Use the installer for the target platform. Keep the existing installation and save directories. Quest updates require the same signing identity as the installed APK; the standalone development APK and the public release APK have different signing identities. Do not uninstall to work around a signing mismatch without first transferring saves.

Windows desktop and PCVR share the same executable. macOS and Linux build from the supplied source. The browser uses flat-screen WebGL rendering; native GT4 model packs and WebXR are not supported there.
