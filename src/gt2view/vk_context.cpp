// The Windows creation of the Vulkan context of vk_context.h (moved unchanged out of VkSceneRenderer::CreateDevice).
#define VK_USE_PLATFORM_WIN32_KHR
#include "gt2view/vk_context.h"

#include <windows.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace gt2view {
namespace {

void Check(VkResult r, const char* what) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string("Vulkan: ") + what + " failed (" + std::to_string(r) + ")");
}

} // namespace

VkContext::VkContext(void* instanceHandle, void* window) : window_(window) {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "gt2view";
    app.apiVersion = VK_API_VERSION_1_3;
    const char* instExt[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = instExt;
    Check(vkCreateInstance(&ici, nullptr, &instance_), "vkCreateInstance");

    VkWin32SurfaceCreateInfoKHR wsi{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wsi.hinstance = static_cast<HINSTANCE>(instanceHandle);
    wsi.hwnd = static_cast<HWND>(window);
    Check(vkCreateWin32SurfaceKHR(instance_, &wsi, nullptr, &surface_), "vkCreateWin32SurfaceKHR");

    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    int bestScore = -1;
    for (VkPhysicalDevice pd : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        if (props.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qs(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
        for (uint32_t q = 0; q < qn; q++) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, surface_, &present);
            if (!(qs[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !present) continue;
            int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 2 : 1;
            if (score > bestScore) {
                bestScore = score;
                physical_ = pd;
                queueFamily_ = q;
            }
            break;
        }
    }
    if (!physical_) throw std::runtime_error("no Vulkan 1.3 GPU with graphics+present queue found");

    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    const char* devExt[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExt;
    Check(vkCreateDevice(physical_, &dci, nullptr, &device_), "vkCreateDevice");
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);
}

VkContext::VkContext(const Existing& existing)
    : instance_(existing.instance), physical_(existing.physical), device_(existing.device), queueFamily_(existing.queueFamily), queue_(existing.queue),
      owns_(false), shadingRate_(existing.shadingRate) {}

VkContext::~VkContext() {
    if (!owns_) return;
    if (device_) vkDestroyDevice(device_, nullptr);
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VkContext::WindowExtent(VkExtent2D& extent) const {
    if (!window_) return false;
    RECT rc;
    if (!GetClientRect(static_cast<HWND>(window_), &rc)) return false;
    extent = {uint32_t(rc.right - rc.left), uint32_t(rc.bottom - rc.top)};
    return true;
}

} // namespace gt2view
