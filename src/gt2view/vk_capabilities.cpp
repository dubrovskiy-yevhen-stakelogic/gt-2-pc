// Runtime capability probe for --diagnose (discussion #77): loads the Vulkan loader by name so it also runs on a
// machine where the game's own Vulkan imports cannot be resolved.
#include "gt2view/vk_capabilities.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace gt2view {
namespace {

void* OpenLoader() {
#if defined(_WIN32)
    // An already loaded loader, else load it by name (application and system search paths).
    const auto module = GetModuleHandleA("vulkan-1.dll");
    return module ? static_cast<void*>(module) : static_cast<void*>(LoadLibraryA("vulkan-1.dll"));
#else
    // The renderer links the loader, so on a working machine its symbols are already global.
    void* self = dlopen(nullptr, RTLD_NOW);
    if (self && dlsym(self, "vkGetInstanceProcAddr")) return self;
#if defined(__APPLE__)
    void* handle = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    return handle ? handle : dlopen("libvulkan.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    void* handle = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return handle ? handle : dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
#endif
}

void* LoadSymbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

bool HasExtension(const VkExtensionProperties* extensions, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++)
        if (std::strcmp(extensions[i].extensionName, name) == 0) return true;
    return false;
}

const char* TypeName(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return "software";
    default: return "other";
    }
}

std::string VersionText(uint32_t version) {
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    return text;
}

} // namespace

const GpuCapability* VulkanCapabilities::best() const {
    // The same preference as the renderer's device choice: the first discrete GPU, else the first usable one.
    const GpuCapability* best = nullptr;
    int bestScore = -1;
    for (const auto& gpu : gpus) {
        if (gpu.profile == GraphicsProfile::Unsupported) continue;
        const int score = gpu.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
        if (score > bestScore) {
            bestScore = score;
            best = &gpu;
        }
    }
    return best;
}

GraphicsProfile VulkanCapabilities::profile() const {
    const GpuCapability* gpu = best();
    return gpu ? gpu->profile : GraphicsProfile::Unsupported;
}

VulkanCapabilities ProbeVulkan() {
    VulkanCapabilities out;
    void* loader = OpenLoader();
    if (!loader) {
        out.error = "Vulkan loader not found (vulkan-1.dll / libvulkan.so.1 / libvulkan.1.dylib)";
        return out;
    }
    out.loaderFound = true;

    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(LoadSymbol(loader, "vkGetInstanceProcAddr"));
    if (!gipa) {
        out.error = "the Vulkan loader has no vkGetInstanceProcAddr";
        return out;
    }
    const auto createInstance = reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance"));
    const auto enumerateInstanceVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(gipa(nullptr, "vkEnumerateInstanceVersion"));
    const auto enumerateInstanceExtensions =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(gipa(nullptr, "vkEnumerateInstanceExtensionProperties"));
    if (!createInstance || !enumerateInstanceExtensions) {
        out.error = "the Vulkan loader has no vkCreateInstance";
        return out;
    }
    out.loaderVersion = VK_API_VERSION_1_0;
    if (enumerateInstanceVersion) enumerateInstanceVersion(&out.loaderVersion);

    // macOS enumerates no GPU without the portability extension, and the loader hides it without the flag.
    uint32_t extensionCount = 0;
    enumerateInstanceExtensions(nullptr, &extensionCount, nullptr);
    std::vector<VkExtensionProperties> available(extensionCount);
    if (extensionCount) enumerateInstanceExtensions(nullptr, &extensionCount, available.data());
    const bool portability = HasExtension(available.data(), extensionCount, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);

    const char* extensions[1] = {VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME};
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gt2game diagnose";
    app.apiVersion = out.loaderVersion < VK_API_VERSION_1_3 ? out.loaderVersion : VK_API_VERSION_1_3;
    if (app.apiVersion < VK_API_VERSION_1_1) app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
    ici.enabledExtensionCount = portability ? 1u : 0u;
    ici.ppEnabledExtensionNames = portability ? extensions : nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    const VkResult created = createInstance(&ici, nullptr, &instance);
    if (created != VK_SUCCESS) {
        out.error = "vkCreateInstance failed (" + std::to_string(int(created)) + ")";
        return out;
    }

    const auto destroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
    const auto enumerateDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(gipa(instance, "vkEnumeratePhysicalDevices"));
    const auto getProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(instance, "vkGetPhysicalDeviceProperties"));
    const auto getFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(gipa(instance, "vkGetPhysicalDeviceFeatures2"));
    const auto getMemory = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties"));
    const auto getMemory2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(gipa(instance, "vkGetPhysicalDeviceMemoryProperties2"));
    const auto enumerateDeviceExtensions =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(gipa(instance, "vkEnumerateDeviceExtensionProperties"));
    struct Destroy {
        PFN_vkDestroyInstance fn;
        VkInstance instance;
        ~Destroy() { if (fn && instance) fn(instance, nullptr); }
    } destroyInstanceGuard{destroyInstance, instance};

    if (!enumerateDevices || !getProperties) {
        out.error = "the Vulkan loader has no device enumeration";
        return out;
    }
    uint32_t deviceCount = 0;
    enumerateDevices(instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    if (deviceCount) enumerateDevices(instance, &deviceCount, devices.data());
    if (devices.empty()) {
        out.error = "the Vulkan loader found no GPU (install or update the graphics driver)";
        return out;
    }

    for (VkPhysicalDevice device : devices) {
        VkPhysicalDeviceProperties properties{};
        getProperties(device, &properties);
        GpuCapability gpu;
        gpu.name = properties.deviceName;
        gpu.type = properties.deviceType;
        gpu.apiVersion = properties.apiVersion;
        gpu.driverVersion = properties.driverVersion;
        gpu.maxImageDimension2D = properties.limits.maxImageDimension2D;
        VkPhysicalDeviceMemoryProperties memory{};
        if (getMemory) {
            getMemory(device, &memory);
            for (uint32_t i = 0; i < memory.memoryHeapCount; i++)
                if (memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) gpu.deviceLocalBytes += memory.memoryHeaps[i].size;
        }
        if (getFeatures2 && gpu.apiVersion >= VK_API_VERSION_1_1) {
            VkPhysicalDeviceMultiviewFeatures f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &f11;
            if (gpu.apiVersion >= VK_API_VERSION_1_3) f11.pNext = &f13;
            getFeatures2(device, &features);
            gpu.multiview = f11.multiview == VK_TRUE;
            gpu.dynamicRendering = gpu.apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering == VK_TRUE;
        }
        if (enumerateDeviceExtensions) {
            uint32_t count = 0;
            enumerateDeviceExtensions(device, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> deviceExtensions(count);
            if (count) enumerateDeviceExtensions(device, nullptr, &count, deviceExtensions.data());
            gpu.swapchain = HasExtension(deviceExtensions.data(), count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
            // How much device-local memory this process may actually use, not just the heap sizes (the desktop and
            // other applications already hold part of it). Without VK_EXT_memory_budget the total is the best guess.
            if (getMemory && getMemory2 && HasExtension(deviceExtensions.data(), count, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
                VkPhysicalDeviceMemoryBudgetPropertiesEXT budget{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
                VkPhysicalDeviceMemoryProperties2 memory2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
                memory2.pNext = &budget;
                getMemory2(device, &memory2);
                gpu.memoryBudgetKnown = true;
                for (uint32_t i = 0; i < memory2.memoryProperties.memoryHeapCount; i++)
                    if (memory2.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && budget.heapBudget[i] > budget.heapUsage[i])
                        gpu.availableBytes += budget.heapBudget[i] - budget.heapUsage[i];
            }
        }
        // The GPU version can exceed the instance's (an old loader); the renderer can only use the lower of the two.
        gpu.profile = ChooseGraphicsProfile(std::min(gpu.apiVersion, app.apiVersion), gpu.dynamicRendering);
        // Dev/support switch, like GT2_VK_RENDERPASS: pretend the GPU has N MiB free to try the adaptive plan (or a
        // too-low machine's refusal) on a big one. Never set by the game itself.
        if (const char* fake = std::getenv("GT2_FAKE_VIDEO_MIB")) {
            const unsigned long long mib = std::strtoull(fake, nullptr, 10);
            gpu.availableBytes = mib << 20;
            gpu.memoryBudgetKnown = true;
            gpu.deviceLocalBytes = std::max<uint64_t>(gpu.deviceLocalBytes, mib << 20);
        }
        out.gpus.push_back(std::move(gpu));
    }
    return out;
}

bool VulkanLoaderInstalled() {
    return OpenLoader() != nullptr;
}

std::string VulkanDiagnosticReport() {
    const VulkanCapabilities caps = ProbeVulkan();
    std::string text = "Vulkan diagnose\n";
    if (!caps.loaderFound) {
        text += "  loader: not found\n";
        text += "  details: " + caps.error + "\n";
        text += "  result: this machine cannot run the Vulkan renderer. Install or update the graphics driver.\n";
        return text;
    }
    text += "  loader: " + VersionText(caps.loaderVersion) + "\n";
    if (!caps.error.empty()) text += "  details: " + caps.error + "\n";
    for (size_t i = 0; i < caps.gpus.size(); i++) {
        const GpuCapability& gpu = caps.gpus[i];
        char line[512];
        const std::string available = gpu.availableBytes
            ? " (" + std::to_string(gpu.availableBytes / (1024 * 1024)) + " MiB available)" : "";
        std::snprintf(line, sizeof(line), "  GPU %zu: %s (%s), Vulkan %s, driver 0x%08x, %llu MiB video memory%s\n", i, gpu.name.c_str(),
                      TypeName(gpu.type), VersionText(gpu.apiVersion).c_str(), gpu.driverVersion,
                      static_cast<unsigned long long>(gpu.deviceLocalBytes / (1024 * 1024)), available.c_str());
        text += line;
        std::snprintf(line, sizeof(line), "    max texture %u, dynamic rendering: %s, multiview: %s, swapchain: %s\n",
                      gpu.maxImageDimension2D,
                      gpu.dynamicRendering ? "yes" : "no", gpu.multiview ? "yes" : "no", gpu.swapchain ? "yes" : "no");
        text += line;
        text += std::string("    profile: ") + GraphicsProfileName(gpu.profile) + "\n";
    }
    const GraphicsProfile profile = caps.profile();
    text += std::string("  result: ") + GraphicsProfileName(profile);
    if (profile == GraphicsProfile::Standard) text += " (Vulkan 1.3 dynamic rendering)";
    else if (profile == GraphicsProfile::Compatibility) text += " (Vulkan 1.1/1.2 render passes)";
    else text += " (Vulkan 1.1 or newer is required)";
    if (const char* forced = std::getenv("GT2_VK_RENDERPASS"); forced && std::strcmp(forced, "1") == 0)
        text += " - forced by GT2_VK_RENDERPASS=1";
    text += "\n";
    if (caps.profile() != GraphicsProfile::Unsupported) {
        const gt2view::GpuCapability* best = caps.best();
        const uint64_t usable = best ? UsableMemory(*best) : 0;
        const uint32_t output = RecommendedOutputHeight(usable, 0);
        // The adaptive plan: the cache takes what the frame leaves over, MSAA only when that fits, and the two big
        // content choices default off when the cache would be under half.
        const uint32_t layers = RecommendedDecodedCacheLayers(usable, output);
        const uint32_t cache = layers * kDecodedCacheLayerKiB / 1024; // MiB of GPU memory, upload and image
        const uint32_t msaa = RecommendedMsaa(usable, output, 4);
        text += std::string("  recommended: ") + (output ? std::to_string(output) + "p" : "display") +
                ", MSAA " + (msaa > 1 ? std::to_string(msaa) + "x" : std::string("off")) +
                ", decoded cache " + std::to_string(cache) + " MiB, HD media and GT4 models " +
                (cache < 64 ? "off" : "on") + "\n";
        if (GraphicsMemoryTooLow(caps, output))
            text += "  warning: the game needs at least " + std::to_string(GraphicsMemoryFloor(output) / (1024 * 1024)) +
                    " MiB of available video memory with this output; close other applications or use a stronger GPU.\n";
    }
    return text;
}

} // namespace gt2view
