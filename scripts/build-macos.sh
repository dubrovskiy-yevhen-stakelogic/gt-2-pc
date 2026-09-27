#!/bin/bash
set -euo pipefail
gt2_root="$(cd "$(dirname "$0")/.." && pwd)"
source "$gt2_root/scripts/macos-env.sh"
if ! xcrun --find clang++ >/dev/null 2>&1; then
    echo 'Install Apple Command Line Tools first: xcode-select --install' >&2
    exit 1
fi
if ! command -v cmake >/dev/null 2>&1; then
    echo 'Build dependencies are missing. Run INSTALL-MACOS.command to install them automatically.' >&2
    exit 1
fi
gt2_build="${GT2_BUILD_DIR:-$gt2_root/build_macos}"
# Default to native Apple Silicon; an Intel Mac can build its own x86_64 client.
gt2_arch="$(uname -m)"
if [ "$(sysctl -in sysctl.proc_translated 2>/dev/null || true)" = 1 ]; then
    echo 'Open a native Terminal (Rosetta disabled), then run this script again.' >&2
    exit 1
fi
gt2_prefix="${CMAKE_PREFIX_PATH:-}"
if [ -n "${GT2_SDL_PREFIX:-}" ]; then
    gt2_prefix="$GT2_SDL_PREFIX${gt2_prefix:+;$gt2_prefix}"
fi
gt2_cmake_args=(-DGT2_BUILD_MACOS_CLIENT=ON -DBUILD_TESTING=ON)
if [ -n "${GT2_SDL_PREFIX:-}" ]; then gt2_cmake_args+=("-DSDL2_DIR=$GT2_SDL_PREFIX/lib/cmake/SDL2"); fi
if [ -n "${GT2_VULKAN_PREFIX:-}" ]; then
    gt2_cmake_args+=("-DVulkan_INCLUDE_DIR=$GT2_VULKAN_PREFIX/include" "-DVulkan_LIBRARY=$GT2_VULKAN_PREFIX/lib/libvulkan.dylib"
        "-DVulkan_GLSLC_EXECUTABLE=$GT2_VULKAN_PREFIX/bin/glslc")
fi
cmake -S "$gt2_root" -B "$gt2_build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$gt2_arch" -DCMAKE_OSX_DEPLOYMENT_TARGET="${GT2_MACOS_MIN:-14.0}" \
    -DCMAKE_PREFIX_PATH="$gt2_prefix" "${gt2_cmake_args[@]}"
cmake --build "$gt2_build" --parallel "$(sysctl -n hw.logicalcpu)" --target \
    gt2game gt2install gt2media gt2maclauncher gt2checks gt2assetchecks gt2hdchecks gt2wheelchecks \
    gt2vrchecks gt2drivingchecks gt2questchecks gt2sdlchecks gt2surfacechecks
ctest --test-dir "$gt2_build" --output-on-failure
printf '\nBuild and automated checks passed (%s). Launch PLAY-MACOS.command.\n' "$gt2_arch"
