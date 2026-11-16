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
    uint64_t deviceLocalBytes = 0;
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

// The first-run recommendation for the PC settings preset: the render-pass path, or a GPU with less than 2 GB of
// device-local memory (the renderer's fixed buffers are several hundred MB).
inline bool RecommendsCompatibility(const VulkanCapabilities& caps) {
    if (caps.profile() == GraphicsProfile::Compatibility) return true;
    const GpuCapability* gpu = caps.best();
    return gpu && gpu->deviceLocalBytes && gpu->deviceLocalBytes < 2ull * 1024 * 1024 * 1024;
}

// Probes the Vulkan loader dynamically. Never throws and keeps working when the loader is absent.
VulkanCapabilities ProbeVulkan();
// True when the loader library exists; the Vulkan entry points are load-time imports on Windows, so callers use
// this before touching any of them.
bool VulkanLoaderInstalled();
// The multi-line --diagnose text: loader, GPUs, limits and the profile the renderer will choose.
std::string VulkanDiagnosticReport();

} // namespace gt2view
