// SDL supplies the window-system surface for the shared Vulkan renderer.
#include "gt2view/vk_context.h"
#include <SDL.h>
#include <SDL_vulkan.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace gt2view {
namespace {
void Check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Vulkan: ") + operation + " failed (" + std::to_string(result) + ")");
}
bool Has(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    return std::any_of(extensions.begin(), extensions.end(), [name](const auto& e) { return std::strcmp(e.extensionName, name) == 0; });
}
}

VkContext::VkContext(void*, void* window) : window_(window) {
    // A throwing constructor must release already-created Vulkan objects itself.
    try {
        auto* sdlWindow = static_cast<SDL_Window*>(window);
        unsigned extensionCount = 0;
        if (!SDL_Vulkan_GetInstanceExtensions(sdlWindow, &extensionCount, nullptr)) throw std::runtime_error(SDL_GetError());
        std::vector<const char*> extensions(extensionCount);
        if (!SDL_Vulkan_GetInstanceExtensions(sdlWindow, &extensionCount, extensions.data())) throw std::runtime_error(SDL_GetError());
        uint32_t availableCount = 0;
        Check(vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, nullptr), "instance extensions");
        std::vector<VkExtensionProperties> available(availableCount);
        Check(vkEnumerateInstanceExtensionProperties(nullptr, &availableCount, available.data()), "instance extensions");
        const bool portability = Has(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        if (portability) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "GT2";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        ici.flags = portability ? VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0;
        ici.enabledExtensionCount = uint32_t(extensions.size());
        ici.ppEnabledExtensionNames = extensions.data();
        Check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance (Vulkan 1.3 / MoltenVK 1.3 or newer required)");
        if (!SDL_Vulkan_CreateSurface(sdlWindow, instance_, &surface_)) throw std::runtime_error(SDL_GetError());

        uint32_t count = 0;
        Check(vkEnumeratePhysicalDevices(instance_, &count, nullptr), "enumerate GPUs");
        std::vector<VkPhysicalDevice> devices(count);
        Check(vkEnumeratePhysicalDevices(instance_, &count, devices.data()), "enumerate GPUs");
        int bestScore = -1;
        bool enableSubset = false;
        for (auto pd : devices) {
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(pd, &properties);
            if (properties.apiVersion < VK_API_VERSION_1_3) continue;
            VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &f13;
            vkGetPhysicalDeviceFeatures2(pd, &features);
            if (!f13.dynamicRendering) continue;
            uint32_t n = 0;
            Check(vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr), "device extensions");
            std::vector<VkExtensionProperties> devExtensions(n);
            Check(vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, devExtensions.data()), "device extensions");
            if (!Has(devExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
            std::vector<VkQueueFamilyProperties> queues(n);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, queues.data());
            for (uint32_t q = 0; q < n; ++q) {
                VkBool32 present = VK_FALSE;
                Check(vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present), "presentation support");
                if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
                const int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
                if (score > bestScore) {
                    bestScore = score;
                    physical_ = pd;
                    queueFamily_ = q;
                    // Use the extension name without requiring provisional Vulkan beta structs.
                    enableSubset = Has(devExtensions, "VK_KHR_portability_subset");
                }
                break;
            }
        }
        if (!physical_) throw std::runtime_error("No presenting Vulkan 1.3 GPU with dynamic rendering. Update the graphics driver/runtime (MoltenVK on macOS).");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &priority;
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        const char* devExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_KHR_portability_subset"};
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.pNext = &f13;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = enableSubset ? 2 : 1;
        dci.ppEnabledExtensionNames = devExtensions;
        Check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
        vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical_, &properties);
        std::printf("graphics: SDL Vulkan, %s%s\n", properties.deviceName, enableSubset ? " (portability / Metal)" : "");
    } catch (...) {
        if (device_) vkDestroyDevice(device_, nullptr);
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        throw;
    }
}

VkContext::VkContext(const Existing& e)
    : instance_(e.instance), physical_(e.physical), device_(e.device), queueFamily_(e.queueFamily), queue_(e.queue),
      owns_(false), shadingRate_(e.shadingRate) {}
VkContext::~VkContext() {
    if (!owns_) return;
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}
bool VkContext::WindowExtent(VkExtent2D& extent) const {
    if (!window_) return false;
    auto* window = static_cast<SDL_Window*>(window_);
    int width = 0, height = 0;
    if (!(SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) SDL_Vulkan_GetDrawableSize(window, &width, &height);
    extent = {uint32_t(std::max(0, width)), uint32_t(std::max(0, height))};
    return true;
}
} // namespace gt2view
