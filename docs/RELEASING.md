# Release binaries

`.github/workflows/release.yml` builds every supported target and attaches the release files to a GitHub release. The Linux tarball stays a workflow artifact; the release Linux download is the Flatpak. Manual runs without a tag only keep workflow artifacts, so they are safe for testing on a fork.

| Job | Release assets |
|---|---|
| macOS arm64 / x86_64 | `GT2-<version>-macos-<arch>.zip`, `.dmg`, release manifest |
| Windows x64 | `GT2-<version>-windows-x64.msi` (installer) and `.zip` (portable game, tools and OpenXR loader) |
| Linux x86_64 | — (the tarball is a workflow artifact only) |
| Quest / Android arm64 | `GT2-VR-<version>.apk` |
| Browser / WebAssembly | `GT2-Web-<version>.zip` |
| Linux / Steam Deck | `GT2-<version>-linux-x86_64.flatpak` |

The Windows MSI installs the program files; the first run asks where to keep discs, saves and media. The portable ZIP starts the same flow from `INSTALL-PC.bat`. One x86_64 Flatpak serves both Steam Deck and desktop Linux.

## Launcher compatibility

Quiver Launcher (quiverlauncher.com) picks one release file per platform:

- It ignores `.dmg`, so macOS uses the ZIP. That ZIP wraps `GT2.app` with a read-me, so launchers that flatten a lone wrapper keep the app bundle intact.
- It prefers a ZIP over an MSI on Windows. The catalog entry needs `preferredExecutables.windows: ["GT2.bat", "gt2game.exe"]`; without it a launcher picks another tool from `tools/`. The MSI stays for users who want a system installer; its first Play still needs the GT2 shortcut once to import a disc.
- It prefers a `.tar.gz` over a `.flatpak`, so the Linux tarball is only a workflow artifact. The release Linux download is the Flatpak.
- The catalog entry keeps `preferredExecutables.macos: ["GT2.app"]` and may add `preferredExecutables.linux: ["gt2launcher"]`.

Do not replace assets on an already-published release: Quiver pins their SHA-256 and refuses changed files. Publish a new tag instead.

## Required secrets

Secrets belong to the repository that hosts the release, not to a contributor's fork. A fork can test the workflow with its own secrets, but the upstream repository needs its own copies before a real release.

### Android signing key

| Secret | Required | Value |
|---|---|---|
| `ANDROID_KEYSTORE_BASE64` | for releases | Base64 of the release `.jks` |
| `ANDROID_KEYSTORE_PASSWORD` | for releases | Keystore password |
| `ANDROID_KEY_PASSWORD` | optional | Key password; defaults to the keystore password |
| `ANDROID_KEY_ALIAS` | optional | Alias; defaults to `gt2-release` |

Reuse the existing `gt2-release.jks`. APKs signed with a different key cannot replace an installed app; Android refuses the update. A new key is only appropriate for a new identity and requires users to uninstall first.

A missing key still fails the Android job for a `release` event or a manual run with a tag, but every other build job uploads its own archive independently, so the release is left without only the Quest APK. Manual runs without a tag build a debug-signed APK for testing instead.

Base64-encode a keystore and set the secrets with GitHub CLI:

```sh
base64 -i work/signing/release/gt2-release.jks | tr -d '\n' | \
  gh secret set ANDROID_KEYSTORE_BASE64 --repo <owner>/<repo>
gh secret set ANDROID_KEYSTORE_PASSWORD --repo <owner>/<repo>
```

`gh secret set` prompts for the value, so it does not enter shell history.

### macOS signing and notarization

Optional, but a public macOS download is blocked by Gatekeeper on first launch unless it is signed with a Developer ID and notarized. Without these secrets the app is ad-hoc signed and works after right-click → Open.

| Secret | Purpose |
|---|---|
| `MACOS_CERTIFICATE_P12` | Base64 `.p12` with the Developer ID Application certificate |
| `MACOS_CERTIFICATE_PASSWORD` | Password for the `.p12` |
| `MACOS_SIGN_IDENTITY` | Optional identity name; discovered from the certificate otherwise |
| `MACOS_NOTARY_APPLE_ID` | Apple ID for notarization |
| `MACOS_NOTARY_TEAM_ID` | Apple team ID |
| `MACOS_NOTARY_PASSWORD` | App-specific password for notarization |

## Test before publishing

1. Add the secrets to the fork or repository.
2. Run **Actions → Release binaries → Run workflow** on `main` with the tag input empty.
3. No release assets are uploaded, because the tag input is empty. Download the workflow artifacts from the run.
4. With the Android key set, the Android job uploads `release-android` containing the signed `GT2-VR-<version>.apk`. Without it, it uploads `android-debug` instead.
5. Verify the APK certificate and check that its SHA-256 matches the key you intend to keep:

```sh
apksigner verify --verbose --print-certs GT2-VR-<version>.apk
```

## Publish a release

1. Create or publish a GitHub release with a tag.
2. The workflow runs for the `release: published` event, checks out that tag and builds every target. Each build job uploads its own `release-*` artifacts as soon as it finishes, so one failed platform does not block the rest.
3. Alternatively, run the workflow manually with the tag of an existing release.

`ANDROID_KEYSTORE_BASE64` is needed for the Quest APK. Without it the Android job fails, but the other five jobs still upload their own archives. Each job uploads only the `release-*` files it produced, so debug or unsigned test builds cannot be attached by accident.

### Combined Quest and PCVR download

The release workflow assembles `GT2-VR-<version>.zip` after both Windows and signed Android jobs succeed. It contains the APK, Windows import tools, `INSTALL-QUEST.bat` (also `INSTALL.bat`) and `INSTALL-PCVR.bat`. Players need only this archive and their own disc image, not a source kit or a separate APK download. The workflow checks package hashes and both installer entry points before uploading, then adds a direct download link to the release notes. Debug-only runs do not publish a VR archive. The assembler comes from the workflow revision; game binaries come from the selected tag in the same run.
