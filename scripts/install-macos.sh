#!/bin/bash
# Source-kit bootstrap. The packaged player app needs none of these build tools.
set -euo pipefail
gt2_root="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$(uname -s)" != Darwin ]; then echo 'This installer requires macOS.' >&2; exit 1; fi
if [ "$(id -u)" = 0 ]; then echo 'Run as your normal user, not root.' >&2; exit 1; fi
if [ "$(sysctl -in sysctl.proc_translated 2>/dev/null || true)" = 1 ]; then
    echo 'Run from native Terminal with Rosetta disabled.' >&2; exit 1
fi
gt2_major="$(sw_vers -productVersion | cut -d. -f1)"
if [ "$gt2_major" -lt 14 ]; then echo 'Automatic dependency installation requires macOS 14 or newer.' >&2; exit 1; fi
gt2_arch="$(uname -m)"
case "$gt2_arch" in arm64) gt2_brew=/opt/homebrew/bin/brew ;; x86_64) gt2_brew=/usr/local/bin/brew ;; *) exit 1 ;; esac
gt2_cache="$HOME/Library/Caches/GT2/build-$gt2_arch"
mkdir -p "$gt2_cache"
gt2_log="$gt2_cache/install-$(date +%Y%m%d-%H%M%S).log"
exec > >(tee -a "$gt2_log") 2>&1
echo "GT2 macOS installer — log: $gt2_log"
echo 'Downloads build dependencies from Homebrew and SDL. macOS may request your administrator password for Apple tools/Homebrew.'

# HTTPS download + pinned SHA256, including the Homebrew bootstrap before executing it.
gt2_fetch() {
    local url="$1" expected="$2" destination="$3" temporary
    if [ -f "$destination" ] && [ "$(shasum -a 256 "$destination" | awk '{print $1}')" = "$expected" ]; then return; fi
    temporary="$(mktemp "$gt2_cache/download.XXXXXX")"
    if ! curl --fail --location --proto '=https' --tlsv1.2 --retry 3 "$url" -o "$temporary"; then
        rm -f "$temporary"; return 1
    fi
    if [ "$(shasum -a 256 "$temporary" | awk '{print $1}')" != "$expected" ]; then
        rm -f "$temporary"; echo "Download checksum mismatch: $url" >&2; return 1
    fi
    mv "$temporary" "$destination"
}
if [ ! -x "$gt2_brew" ]; then
    gt2_fetch 'https://raw.githubusercontent.com/Homebrew/install/e53db71afc381d41c46c8baaeba10c091acf4b44/install.sh' \
        f31a38f097f3b5bbfdc110658e4a9876d0c023ccc9ef2e70527f5b8a762e505e "$gt2_cache/homebrew-install.sh"
    # Homebrew's official installer also obtains missing Apple Command Line Tools.
    sudo -v
    NONINTERACTIVE=1 /bin/bash "$gt2_cache/homebrew-install.sh"
fi
if ! xcrun --find clang++ >/dev/null 2>&1 || ! xcrun --show-sdk-path >/dev/null 2>&1; then
    echo 'Opening the Apple Command Line Tools installer. Finish its dialog; GT2 will continue automatically.'
    xcode-select --install || true
    gt2_wait=0
    until xcrun --find clang++ >/dev/null 2>&1 && xcrun --show-sdk-path >/dev/null 2>&1; do
        sleep 5; gt2_wait=$((gt2_wait + 5))
        if [ "$gt2_wait" -ge 3600 ]; then echo 'Apple tool installation is not complete. Run INSTALL-MACOS.command again when it finishes.' >&2; exit 1; fi
    done
fi
gt2_prefix="$("$gt2_brew" --prefix)"
export PATH="$gt2_prefix/bin:$PATH"
export HOMEBREW_NO_ANALYTICS=1 HOMEBREW_NO_INSTALL_CLEANUP=1 HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1
# Homebrew verifies its package checksums; update the named prerequisites when necessary.
"$gt2_brew" install cmake python@3.13 shaderc vulkan-headers vulkan-loader molten-vk
gt2_python="$("$gt2_brew" --prefix python@3.13)/bin/python3.13"

# SDL2 is pinned: Homebrew's former sdl2 formula now redirects to an SDL3 compatibility layer.
gt2_fetch 'https://www.libsdl.org/release/SDL2-2.32.10.tar.gz' \
    5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165 "$gt2_cache/SDL2-2.32.10.tar.gz"
gt2_sdl="$gt2_cache/SDL2-2.32.10"
if [ ! -f "$gt2_sdl/CMakeLists.txt" ]; then tar -xzf "$gt2_cache/SDL2-2.32.10.tar.gz" -C "$gt2_cache"; fi
export GT2_SDL_PREFIX="$gt2_cache/sdl2-install"
cmake -S "$gt2_sdl" -B "$gt2_cache/sdl2-build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$gt2_arch" -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DCMAKE_INSTALL_PREFIX="$GT2_SDL_PREFIX" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
cmake --build "$gt2_cache/sdl2-build" --parallel "$(sysctl -n hw.logicalcpu)"
cmake --install "$gt2_cache/sdl2-build"
mkdir -p "$GT2_SDL_PREFIX/share/licenses/SDL2"
cp "$gt2_sdl/LICENSE.txt" "$GT2_SDL_PREFIX/share/licenses/SDL2/LICENSE.txt"
export CMAKE_PREFIX_PATH="$GT2_SDL_PREFIX;$gt2_prefix${CMAKE_PREFIX_PATH:+;$CMAKE_PREFIX_PATH}"
export GT2_VULKAN_PREFIX="$gt2_prefix"
export GT2_MOLTENVK="$("$gt2_brew" --prefix molten-vk)/lib/libMoltenVK.dylib"
export VK_DRIVER_FILES="$("$gt2_brew" --prefix molten-vk)/etc/vulkan/icd.d/MoltenVK_icd.json"
export GT2_MACOS_MIN=14.0
bash "$gt2_root/scripts/build-macos.sh"
gt2_build="${GT2_BUILD_DIR:-$gt2_root/build_macos}"
"$gt2_brew" info --json=v2 cmake python@3.13 shaderc vulkan-headers vulkan-loader molten-vk > "$gt2_build/macos-dependencies.json"
"$gt2_python" "$gt2_root/scripts/package-macos.py" --build "$gt2_build" \
    --moltenvk "$GT2_MOLTENVK" --library-dir "$GT2_SDL_PREFIX/lib" --library-dir "$gt2_prefix/lib" \
    --license-dir "$GT2_SDL_PREFIX/share/licenses" --install "$@"
printf '\nInstalled ~/Applications/GT2.app. Open it to choose your own GT2 disc images.\n'
