#pragma once
// Pure logic of the Vulkan 1.1/1.2 render-pass fallback (discussion #77), kept apart so it is testable without a GPU.
#include <vulkan/vulkan.h>

#include <cstdint>

namespace gt2view {

// With multiview the framebuffer has one layer: the view mask picks the array layer in each attachment, and it is the
// attachment images that must cover the highest view (VUID-VkFramebufferCreateInfo-renderPass-02531 / -04536). Without
// multiview, `layers` layers are rendered together, as before.
inline uint32_t FramebufferLayers(uint32_t viewMask, uint32_t layers) {
    return viewMask ? 1u : layers;
}

struct RenderPassClears {
    VkClearValue values[3]{};
    uint32_t count = 0;
};

// The shading rate map of foveated VR: R8_UINT, one rate per tile. It is an attachment in the render-pass path and a
// struct member in the dynamic-rendering path, so both creation sites use this format.
constexpr VkFormat kShadingRateFormat = VK_FORMAT_R8_UINT;

// One clear value per attachment, in attachment order: colour, resolve (its load op ignores the value), depth.
inline RenderPassClears MakeRenderPassClears(const VkClearValue& color, bool resolve, bool depth) {
    RenderPassClears clears;
    clears.values[0] = color;
    clears.count = 1;
    if (resolve) ++clears.count;
    if (depth) clears.values[clears.count++].depthStencil = {0.0f, 0}; // reversed Z: 0 = infinitely far
    return clears;
}

} // namespace gt2view
