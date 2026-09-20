#pragma once
// The Vulkan objects a renderer works with, separated from the renderer itself (docs/research/vr_port_plan.md, M0):
// instance, physical device, device, graphics queue - and, for a window, the presentation surface. VkSceneRenderer
// takes a VkContext and creates none of this, so that the window path keeps exactly its former behaviour while the
// XR session (M1) can hand the renderer the instance and device that the OpenXR runtime requires
// (xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR) and the images of an XrSwapchain to render into.
#include <vulkan/vulkan.h>

#include <cstdint>

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
};

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
    };

    // The window path (Windows): a Vulkan 1.3 instance with the surface extensions, the surface of `window`
    // (`instanceHandle` = the module's HINSTANCE), the best GPU with a graphics + present queue for it, and a device
    // with dynamic rendering and the swapchain extension.
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
};

} // namespace gt2view
