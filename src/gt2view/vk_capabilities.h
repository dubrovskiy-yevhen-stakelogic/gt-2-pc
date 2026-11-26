#pragma once
// What this machine can render, probed without the renderer (discussion #77, phase 0). The probe loads the Vulkan
// loader at run time, so it also reports machines where vulkan-1.dll is missing and the process could not start a
// statically linked Vulkan client at all.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <algorithm>
#include <vector>

namespace gt2view {

// The renderer path a GPU gets: Vulkan 1.3 dynamic rendering, or the 1.1/1.2 render-pass fallback.
enum class GraphicsProfile { Unsupported, Compatibility, Standard };

inline GraphicsProfile ChooseGraphicsProfile(uint32_t apiVersion, bool dynamicRendering) {
    if (apiVersion < VK_API_VERSION_1_1) return GraphicsProfile::Unsupported;
    return apiVersion >= VK_API_VERSION_1_3 && dynamicRendering ? GraphicsProfile::Standard : GraphicsProfile::Compatibility;
}

inline const char* GraphicsProfileName(GraphicsProfile profile) {
    switch (profile) {
    case GraphicsProfile::Standard: return "standard";
    case GraphicsProfile::Compatibility: return "compatibility";
    default: return "unsupported";
    }
}

struct GpuCapability {
    std::string name;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    uint32_t apiVersion = 0, driverVersion = 0;
    uint64_t deviceLocalBytes = 0; // size of the device-local heaps
    uint64_t availableBytes = 0;   // device-local budget left (VK_EXT_memory_budget)
    bool memoryBudgetKnown = false;
    uint32_t maxImageDimension2D = 0;
    bool dynamicRendering = false, multiview = false, swapchain = false;
    GraphicsProfile profile = GraphicsProfile::Unsupported;
};

struct VulkanCapabilities {
    bool loaderFound = false;
    uint32_t loaderVersion = 0;
    std::vector<GpuCapability> gpus;
    std::string error; // empty when the loader and a 1.1 instance were available
    // The GPU the renderer would choose (first discrete, else the first usable one); null without a usable GPU.
    const GpuCapability* best() const;
    // The best GPU's profile.
    GraphicsProfile profile() const;
};

// What a race really used at peak (device-local memory, VK_EXT_memory_budget, six cars, MoltenVK on an M4): low spec
// 720p 44 MiB, low spec 1080p 66, MSAA 4 + max detail 720p 92, MSAA 4 + max detail 1080p 167, title 720p 39. The
// floors are about three times the low spec preset, and the decision below keeps a low-memory machine on settings
// that fit it (see RecommendedMsaa and RecommendedDecodedCacheLayers) instead of persisting a "low memory" mode.
inline constexpr uint64_t kGraphicsMemoryFloorDisplay = 192ull * 1024 * 1024;
inline constexpr uint64_t kGraphicsMemoryFloor720 = 128ull * 1024 * 1024;
inline uint64_t GraphicsMemoryFloor(uint32_t outputHeight) {
    return outputHeight && outputHeight <= 720 ? kGraphicsMemoryFloor720 : kGraphicsMemoryFloorDisplay;
}

// What the GPU can actually hand out: the budget the driver reports, else the heaps' total (0 = unknown).
inline uint64_t UsableMemory(const GpuCapability& gpu) {
    // Policy helpers reserve zero for unknown memory; an exhausted known budget must stay below every floor.
    return gpu.memoryBudgetKnown ? std::max<uint64_t>(1, gpu.availableBytes)
                                 : gpu.availableBytes ? gpu.availableBytes : gpu.deviceLocalBytes;
}

// A decoded cache layer is a 256x256 RGBA page plus its sampled image with 9 mips: ~600 KiB of GPU memory (rounded
// up here). The full cache (512 layers) is therefore ~320 MiB, not the 128 MiB of its upload buffer alone.
inline constexpr uint32_t kDecodedCacheLayerKiB = 640, kDecodedCacheLayers = 512;
// The decoded texture cache is opportunistic, not a mode: it takes a third of the video memory left above the floor,
// up to all 512 layers, so the frame itself (output targets, MSAA, growth) keeps two thirds of the headroom. A device
// at the floor gets none (it still runs the race through the PS1 VRAM path), one with a few hundred MiB left gets a
// partial cache, and a big GPU gets all of it. Unknown memory keeps the full cache: a driver that reports nothing is not
// evidence of a small one.
inline uint32_t RecommendedDecodedCacheLayers(uint64_t available, uint32_t outputHeight) {
    if (!available) return kDecodedCacheLayers;
    const uint64_t layer = uint64_t(kDecodedCacheLayerKiB) * 1024;
    const uint64_t floor = GraphicsMemoryFloor(outputHeight);
    const uint64_t spare = available > floor ? available - floor : 0;
    return uint32_t(std::min<uint64_t>(spare / 3, uint64_t(kDecodedCacheLayers) * layer) / layer);
}

// MSAA is the most expensive setting per pixel (4x at 1080p costs ~100 MiB more than off). Keep it only when the
// memory covers twice the measured 4x peak for this output; otherwise 1x. 8x asks for twice again.
inline uint32_t RecommendedMsaa(uint64_t available, uint32_t outputHeight, uint32_t requested) {
    if (requested <= 1 || !available) return requested;
    const uint64_t fourTimes = (outputHeight && outputHeight <= 720 ? 92ull : 167ull) << 20;
    const uint64_t need = 2 * fourTimes * (requested > 4 ? 2 : 1);
    return available < need ? 1 : requested;
}

// Hard floor: below this the low spec frame itself cannot fit, so the game refuses to start rather than run out of
// memory during a race. Unknown memory (0) is not treated as low - some drivers simply do not report it.
inline bool GraphicsMemoryTooLow(const VulkanCapabilities& caps, uint32_t outputHeight) {
    const GpuCapability* gpu = caps.best();
    return gpu && caps.profile() != GraphicsProfile::Unsupported && UsableMemory(*gpu) &&
           UsableMemory(*gpu) < GraphicsMemoryFloor(outputHeight);
}

// Largest output height that fits the memory: 720p under the display floor, 1080p under 1 GiB, otherwise the user's.
inline uint32_t RecommendedOutputHeight(uint64_t available, uint32_t current) {
    if (!available) return current; // the driver does not report a budget: do not guess
    if (available < kGraphicsMemoryFloorDisplay) return 720;
    if (available < 1ull * 1024 * 1024 * 1024) return current && current <= 1080 ? current : 1080;
    return current;
}

// Probes the Vulkan loader dynamically. Never throws and keeps working when the loader is absent.
VulkanCapabilities ProbeVulkan();
// True when the loader library exists; the Vulkan entry points are load-time imports on Windows, so callers use
// this before touching any of them.
bool VulkanLoaderInstalled();
// The multi-line --diagnose text: loader, GPUs, limits and the profile the renderer will choose.
std::string VulkanDiagnosticReport();

} // namespace gt2view
