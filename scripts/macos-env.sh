# Sourced by the macOS build and play launchers. No downloads or installations.
if [ "$(uname -s)" != Darwin ]; then
    echo 'This launcher requires macOS.' >&2
    return 1
fi
# Recover Homebrew dependencies in a fresh Finder/Terminal session.
# Explicit SDK/prefix choices still take precedence for development builds.
if [ -z "${GT2_VULKAN_PREFIX:-}" ] && [ -z "${VULKAN_SDK:-}" ]; then
    case "$(uname -m)" in
        arm64) gt2_brew_prefix=/opt/homebrew ;;
        x86_64) gt2_brew_prefix=/usr/local ;;
        *) gt2_brew_prefix='' ;;
    esac
    if [ -n "$gt2_brew_prefix" ] && [ -f "$gt2_brew_prefix/lib/libvulkan.dylib" ]; then
        export GT2_VULKAN_PREFIX="$gt2_brew_prefix"
    fi
fi
# Prefer explicit Homebrew component prefixes set by the automatic installer.
if [ -n "${GT2_VULKAN_PREFIX:-}" ]; then
    if [ -z "${GT2_SDL_PREFIX:-}" ]; then
        gt2_cached_sdl="$HOME/Library/Caches/GT2/build-$(uname -m)/sdl2-install"
        if [ -f "$gt2_cached_sdl/lib/libSDL2.dylib" ]; then export GT2_SDL_PREFIX="$gt2_cached_sdl"; fi
    fi
    export PATH="$GT2_VULKAN_PREFIX/bin:$PATH"
    export DYLD_LIBRARY_PATH="$GT2_VULKAN_PREFIX/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
    if [ -n "${GT2_SDL_PREFIX:-}" ]; then
        export DYLD_LIBRARY_PATH="$GT2_SDL_PREFIX/lib:$DYLD_LIBRARY_PATH"
    fi
    if [ -f "$GT2_VULKAN_PREFIX/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json" ]; then
        export VK_DRIVER_FILES="$GT2_VULKAN_PREFIX/opt/molten-vk/etc/vulkan/icd.d/MoltenVK_icd.json"
    elif [ -f "$GT2_VULKAN_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json" ]; then
        export VK_DRIVER_FILES="$GT2_VULKAN_PREFIX/etc/vulkan/icd.d/MoltenVK_icd.json"
    elif [ -f "$GT2_VULKAN_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json" ]; then
        export VK_DRIVER_FILES="$GT2_VULKAN_PREFIX/share/vulkan/icd.d/MoltenVK_icd.json"
    fi
    return 0
fi
if [ -z "${VULKAN_SDK:-}" ]; then
    for gt2_sdk in "$HOME"/VulkanSDK/*/macOS; do
        if [ -f "$gt2_sdk/include/vulkan/vulkan.h" ]; then VULKAN_SDK="$gt2_sdk"; fi
    done
fi
if [ -z "${VULKAN_SDK:-}" ] || [ ! -x "$VULKAN_SDK/bin/glslc" ]; then
    echo 'Install the current macOS Vulkan SDK (including MoltenVK) from https://vulkan.lunarg.com/sdk/home' >&2
    echo 'If installed elsewhere, set VULKAN_SDK to its macOS directory and run again.' >&2
    return 1
fi
export VULKAN_SDK
export PATH="$VULKAN_SDK/bin:$PATH"
export DYLD_LIBRARY_PATH="$VULKAN_SDK/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
if [ -f "$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json" ]; then
    export VK_DRIVER_FILES="$VULKAN_SDK/share/vulkan/icd.d/MoltenVK_icd.json"
fi
# Finder does not inherit the Homebrew path from the user's interactive shell.
if [ -d /usr/local/bin ]; then export PATH="/usr/local/bin:$PATH"; fi
if [ -d /opt/homebrew/bin ]; then export PATH="/opt/homebrew/bin:$PATH"; fi
