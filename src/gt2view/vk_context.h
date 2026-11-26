#pragma once
// The Vulkan objects a renderer works with, separated from the renderer itself (docs/research/vr_port_plan.md):
// instance, physical device, device, graphics queue - and, for a window, the presentation surface. VkSceneRenderer
// takes a VkContext and creates none of this, so that the window path keeps exactly its former behaviour while the
// XR session can hand the renderer the instance and device that the OpenXR runtime requires
// (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR) and the images of an XrSwapchain to render into.
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace gt2view {

// An image a frame is recorded into: the window's swapchain image and the offscreen scene target today; with the XR
// path the colour (and optional depth) images of an XrSwapchain, which are array images with one layer per eye -
// hence `layers`, which becomes the rendering info's layer count / view mask.
struct RenderTarget {
    VkImageView colorView = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkImageView depthView = VK_NULL_HANDLE; // VK_NULL_HANDLE: no depth attachment
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{};
    uint32_t layers = 1;
    // The renderer's fallback path (Vulkan 1.1/1.2 render passes) needs the sample count and multiview mask that
    // dynamic rendering reads from the attachment/pipeline instead.
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    uint32_t viewMask = 0;
};

// What the renderer can use on a device: 1.3 dynamic rendering, and the multiview feature of stereo targets.
struct VkDeviceFeatures {
    bool dynamicRendering = false;
    bool multiview = false;
};

inline VkDeviceFeatures QueryDeviceFeatures(VkPhysicalDevice physical, uint32_t apiVersion) {
    VkDeviceFeatures out;
    if (apiVersion < VK_API_VERSION_1_1) return out;
    VkPhysicalDeviceMultiviewFeatures f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    if (apiVersion >= VK_API_VERSION_1_3) f11.pNext = &f13;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &f11;
    vkGetPhysicalDeviceFeatures2(physical, &features);
    out.multiview = f11.multiview == VK_TRUE;
    out.dynamicRendering = apiVersion >= VK_API_VERSION_1_3 && f13.dynamicRendering == VK_TRUE;
    return out;
}

// A device extension, by name. The XR session enables create_renderpass2 for the foveation render pass on a 1.1 GPU.
inline bool HasDeviceExtension(VkPhysicalDevice physical, const char* name) {
    uint32_t count = 0;
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr) != VK_SUCCESS) return false;
    std::vector<VkExtensionProperties> extensions(count);
    if (vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data()) != VK_SUCCESS) return false;
    for (const auto& extension : extensions)
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    return false;
}

// vkCreateRenderPass2 is core in 1.2 and VK_KHR_create_renderpass2 on 1.1: the foveation render pass needs it because
// a fragment shading rate attachment can only be declared in VkSubpassDescription2.
inline bool RenderPass2Available(uint32_t apiVersion, bool extension) {
    return apiVersion >= VK_API_VERSION_1_2 || extension;
}

// Opt-in compatibility path: GT2_VK_RENDERPASS=1 uses render passes on a 1.3 device too (testing, or a driver that
// misbehaves on the 1.3 path).
inline bool ForceRenderPassPath() {
    const char* value = std::getenv("GT2_VK_RENDERPASS");
    return value && std::strcmp(value, "1") == 0;
}

// The instance version to request: what the loader supports, clamped to the renderer's 1.1 floor and 1.3 (some
// implementations, MoltenVK among them, report the device as no newer than the instance).
inline uint32_t RequestedInstanceApiVersion() {
    uint32_t supported = VK_API_VERSION_1_1;
    if (const auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
        if (enumerate(&supported) != VK_SUCCESS) supported = VK_API_VERSION_1_1;
    if (supported < VK_API_VERSION_1_1) return VK_API_VERSION_1_1;
    return supported < VK_API_VERSION_1_3 ? supported : VK_API_VERSION_1_3;
}

class VkContext {
public:
    // Handles somebody else created and destroys (the XR path: xrCreateVulkanDeviceKHR). The context creates and
    // destroys nothing of them and has no surface, so a renderer built on it draws into supplied RenderTargets.
    struct Existing {
        VkInstance instance = VK_NULL_HANDLE;
        VkPhysicalDevice physical = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        uint32_t queueFamily = 0;
        VkQueue queue = VK_NULL_HANDLE;
        bool shadingRate = false;
        // What the device was created with (the XR runtime's device): 1.1/1.2 has no dynamic rendering, so the
        // renderer uses render passes. Both default to the fallback that always works, never to an assumed feature.
        bool dynamicRendering = false;
        bool multiview = false;
        bool astc = false;
    };

    // The window path (Windows): a Vulkan 1.1+ instance with the surface extensions, the surface of `window`
    // (`instanceHandle` = the module's HINSTANCE), the best GPU with a graphics + present queue for it, and a device
    // with dynamic rendering (1.3) or render passes (1.1/1.2) plus the swapchain extension.
    VkContext(void* instanceHandle, void* window);
    explicit VkContext(const Existing& existing);
    ~VkContext();
    VkContext(const VkContext&) = delete;
    VkContext& operator=(const VkContext&) = delete;

    VkInstance Instance() const { return instance_; }
    // VK_NULL_HANDLE when there is no window (the XR path): the renderer then has no swapchain of its own.
    VkSurfaceKHR Surface() const { return surface_; }
    VkPhysicalDevice PhysicalDevice() const { return physical_; }
    VkDevice Device() const { return device_; }
    uint32_t QueueFamily() const { return queueFamily_; }
    VkQueue Queue() const { return queue_; }
    bool ShadingRate() const { return shadingRate_; }
    // False on a Vulkan 1.1/1.2 device: the renderer then uses render passes (discussion #77).
    bool DynamicRendering() const { return dynamicRendering_; }
    bool Multiview() const { return multiview_; }
    bool AstcTextures() const { return astc_; }

    // The window's client size, for a surface that does not report its own extent. False without a window.
    bool WindowExtent(VkExtent2D& extent) const;

private:
    [[maybe_unused]] void* window_ = nullptr; // HWND on Windows
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    [[maybe_unused]] bool owns_ = true;
    bool shadingRate_ = false; // false for Existing: destroy nothing
    bool dynamicRendering_ = false;
    bool multiview_ = false;
    bool astc_ = false;
};

} // namespace gt2view
