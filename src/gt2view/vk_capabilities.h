#pragma once
// What this machine can render, probed without the renderer (discussion #77, phase 0). The probe loads the Vulkan
// loader at run time, so it also reports machines where vulkan-1.dll is missing and the process could not start a
// statically linked Vulkan client at all.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
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
    uint32_t apiVersion = 0, driverVersion = 0, vendorID = 0, deviceID = 0;
    uint64_t deviceLocalBytes = 0; // size of the device-local heaps
    uint64_t availableBytes = 0;   // device-local budget left (VK_EXT_memory_budget); 0 when the driver does not say
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

// The standard settings' measured peak is ~470 MiB (a race at 720p, decoded cache included) plus the swapchain and
// driver overhead, so ~700 MiB is the real need; below 1 GiB available the cache stays off with headroom to spare.
inline constexpr uint64_t kStandardMemoryNeed = 1ull * 1024 * 1024 * 1024;
// The 137 MiB of fixed buffers plus swapchain, depth and scene targets and driver overhead. A 1080p/display output
// needs ~512 MiB; a 720p output (the low spec preset) needs ~384 MiB.
inline constexpr uint64_t kGraphicsMemoryFloorDisplay = 512ull * 1024 * 1024;
inline constexpr uint64_t kGraphicsMemoryFloor720 = 384ull * 1024 * 1024;
inline uint64_t GraphicsMemoryFloor(uint32_t outputHeight) {
    return outputHeight && outputHeight <= 720 ? kGraphicsMemoryFloor720 : kGraphicsMemoryFloorDisplay;
}

// What the GPU can actually hand out: the budget the driver reports, else the heaps' total (0 = unknown).
inline uint64_t UsableMemory(const GpuCapability& gpu) {
    return gpu.availableBytes ? gpu.availableBytes : gpu.deviceLocalBytes;
}

// Whether the decoded texture cache must stay off. This is about memory only: a Vulkan 1.1/1.2 card with plenty of
// memory is a powerful GPU and keeps its preset (the render-pass path is chosen separately, from the API version).
inline bool RecommendsLowMemory(const VulkanCapabilities& caps) {
    const GpuCapability* gpu = caps.best();
    return gpu && UsableMemory(*gpu) && UsableMemory(*gpu) < kStandardMemoryNeed;
}

// Hard floor: the low spec preset cannot be squeezed below this, so the game refuses to start rather than run
// out of memory during a race. Unknown memory (0) is not treated as low - some drivers simply do not report it.
inline bool GraphicsMemoryTooLow(const VulkanCapabilities& caps, uint32_t outputHeight) {
    const GpuCapability* gpu = caps.best();
    return gpu && caps.profile() != GraphicsProfile::Unsupported && UsableMemory(*gpu) &&
           UsableMemory(*gpu) < GraphicsMemoryFloor(outputHeight);
}

// Largest output height that fits the memory: 720p under 512 MiB, 1080p under 1 GiB, otherwise keep the user's.
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
