#!/bin/bash
set -euo pipefail
gt2_root="$(cd "$(dirname "$0")" && pwd)"
if [ "$(uname -s)" != Linux ] || [ "$(uname -m)" != x86_64 ]; then
    echo 'Build this package on x86_64 Linux or Steam Deck Desktop Mode.' >&2
    exit 1
fi
if ! command -v flatpak >/dev/null; then
    echo 'Install Flatpak from your Linux distribution, then run this script again.' >&2
    exit 1
fi
gt2_builder=()
gt2_output="${XDG_CACHE_HOME:-$HOME/.cache}/gt2/steamdeck-build"
mkdir -p "$gt2_output"
gt2_output="$(cd "$gt2_output" && pwd)"
if flatpak info --user org.flatpak.Builder >/dev/null 2>&1; then
    gt2_builder=(flatpak run --user "--filesystem=$gt2_output" "--filesystem=$gt2_root:ro" org.flatpak.Builder)
elif command -v flatpak-builder >/dev/null; then
    gt2_builder=(flatpak-builder)
else
    echo 'Install the build tool in Desktop Mode, then run this script again:' >&2
    echo 'flatpak install --user flathub org.flatpak.Builder' >&2
    exit 1
fi
# A fresh build directory avoids deleting previous build output or user paths.
gt2_run="$(mktemp -d "$gt2_output/run.XXXXXX")"
printf 'Build cache: %s\n' "$gt2_output"
cd "$gt2_root"
# rofiles-fuse cannot mount over some sandbox or removable-drive filesystems.
"${gt2_builder[@]}" --disable-rofiles-fuse --user --arch=x86_64 --jobs="${GT2_BUILD_JOBS:-4}" \
    --state-dir="$gt2_output/builder-state" --repo="$gt2_output/repo" \
    "$gt2_run/app" tools/gt2linux/io.github.gt2pc.GT2.json
gt2_bundle="$gt2_run/GT2-0.7.0-steamdeck.flatpak"
flatpak build-bundle --arch=x86_64 "$gt2_output/repo" "$gt2_bundle" io.github.gt2pc.GT2 \
    --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo
sha256sum "$gt2_bundle" > "$gt2_bundle.sha256"
if [ -n "${GT2_BUILD_RESULT_FILE:-}" ]; then
    printf '%s\n' "$gt2_bundle" > "$GT2_BUILD_RESULT_FILE"
fi
printf '\nBuilt and checked: %s\nInstall: bash "%s/INSTALL-STEAMDECK.sh" "%s"\n' "$gt2_bundle" "$gt2_root" "$gt2_bundle"
