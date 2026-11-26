// The render-pass fallback (discussion #77): profile choice, the test switch that forces it, and the two pieces of
// logic whose first versions were wrong (clear-value order, array layers for a view mask). No GPU needed.
#include "gt2view/vk_context.h"
#include "gt2view/vk_capabilities.h"
#include "gt2view/vk_render_pass.h"
#include <cstdio>
#include <cstdlib>

namespace {
void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value ? value : "");
#else
    if (value) setenv(name, value, 1); else unsetenv(name);
#endif
}
} // namespace

int main() {
    using gt2view::ChooseGraphicsProfile;
    using gt2view::FramebufferLayers;
    using gt2view::GraphicsProfile;
    using gt2view::MakeRenderPassClears;

    if (ChooseGraphicsProfile(VK_API_VERSION_1_0, false) != GraphicsProfile::Unsupported) return 1;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_1, false) != GraphicsProfile::Compatibility) return 2;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_2, false) != GraphicsProfile::Compatibility) return 3;
    // A 1.3 device without the dynamic-rendering feature still falls back to render passes.
    if (ChooseGraphicsProfile(VK_API_VERSION_1_3, false) != GraphicsProfile::Compatibility) return 4;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_3, true) != GraphicsProfile::Standard) return 5;
    if (ChooseGraphicsProfile(VK_MAKE_API_VERSION(0, 1, 4, 0), true) != GraphicsProfile::Standard) return 6;
    // The dynamic-rendering feature itself only exists from Vulkan 1.3.
    if (ChooseGraphicsProfile(VK_API_VERSION_1_1, true) != GraphicsProfile::Compatibility) return 7;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_0, true) != GraphicsProfile::Unsupported) return 8;
    if (ChooseGraphicsProfile(VK_MAKE_API_VERSION(0, 1, 4, 0), false) != GraphicsProfile::Compatibility) return 9;

    // GT2_VK_RENDERPASS=1 is what makes the fallback testable on a 1.3 GPU; only the exact value 1 opts in.
    SetEnv("GT2_VK_RENDERPASS", "1");
    if (!gt2view::ForceRenderPassPath()) return 10;
    SetEnv("GT2_VK_RENDERPASS", "0");
    if (gt2view::ForceRenderPassPath()) return 11;
    SetEnv("GT2_VK_RENDERPASS", "");
    if (gt2view::ForceRenderPassPath()) return 12;
    SetEnv("GT2_VK_RENDERPASS", nullptr);
    if (gt2view::ForceRenderPassPath()) return 13;

    // A view mask covers layers 0 .. highest set view: the gap in 0b101 needs three layers, not popcount two.
    if (FramebufferLayers(0, 1) != 1) return 14;
    if (FramebufferLayers(0, 2) != 2) return 15;
    // Multiview: one framebuffer layer, whatever the view mask; the attachment views carry the array layers.
    if (FramebufferLayers(0b1, 1) != 1) return 16;
    if (FramebufferLayers(0b11, 2) != 1) return 17;
    if (FramebufferLayers(0b101, 3) != 1) return 18;
    if (FramebufferLayers(0x80000001u, 32) != 1) return 19;

    // Clear values follow the attachment order; dropping the resolve slot put the depth clear on the wrong attachment.
    VkClearValue color{};
    color.color = {{0.1f, 0.2f, 0.3f, 1.0f}};
    auto clears = MakeRenderPassClears(color, false, false);
    if (clears.count != 1 || clears.values[0].color.float32[2] != 0.3f) return 20;
    clears = MakeRenderPassClears(color, true, false);
    if (clears.count != 2) return 21;
    clears = MakeRenderPassClears(color, false, true);
    if (clears.count != 2 || clears.values[1].depthStencil.depth != 0.0f) return 22;
    clears = MakeRenderPassClears(color, true, true);
    if (clears.count != 3 || clears.values[2].depthStencil.depth != 0.0f) return 23;

    // The foveation render pass needs vkCreateRenderPass2: core in 1.2, VK_KHR_create_renderpass2 on 1.1 (the session
    // enables that extension for exactly this, and turns foveation off when the device has neither).
    if (!gt2view::RenderPass2Available(VK_API_VERSION_1_1, true)) return 40;
    if (gt2view::RenderPass2Available(VK_API_VERSION_1_1, false)) return 41;
    if (!gt2view::RenderPass2Available(VK_API_VERSION_1_2, false)) return 42;
    if (!gt2view::RenderPass2Available(VK_API_VERSION_1_3, false)) return 43;

    std::puts("PASS: Vulkan 1.1/1.2 fallback profile, env switch, layers and clear values");
    return 0;
}
