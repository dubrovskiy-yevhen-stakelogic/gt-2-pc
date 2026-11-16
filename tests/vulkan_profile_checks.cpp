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
    if (ChooseGraphicsProfile(VK_API_VERSION_1_4, true) != GraphicsProfile::Standard) return 6;
    // The dynamic-rendering feature itself only exists from Vulkan 1.3.
    if (ChooseGraphicsProfile(VK_API_VERSION_1_1, true) != GraphicsProfile::Compatibility) return 7;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_0, true) != GraphicsProfile::Unsupported) return 8;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_4, false) != GraphicsProfile::Compatibility) return 9;

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
    if (FramebufferLayers(0b1, 1) != 1) return 16;
    if (FramebufferLayers(0b11, 2) != 2) return 17;
    if (FramebufferLayers(0b101, 3) != 3) return 18;
    if (FramebufferLayers(0x80000001u, 32) != 32) return 19;

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

    // The preset follows usable memory, not the API version: a Vulkan 1.1/1.2 card with plenty of memory is a
    // powerful GPU and keeps the standard look; the render-pass path is chosen separately from the API version.
    gt2view::GpuCapability gpu;
    gpu.apiVersion = VK_API_VERSION_1_2;
    gpu.profile = GraphicsProfile::Compatibility;
    gpu.deviceLocalBytes = 8ull << 30;
    gpu.availableBytes = 7ull << 30;
    gt2view::VulkanCapabilities plenty;
    plenty.gpus.push_back(gpu);
    if (gt2view::RecommendsLowSpecPreset(plenty)) return 24;
    if (gt2view::GraphicsMemoryTooLow(plenty)) return 25;
    gt2view::VulkanCapabilities weak = plenty;
    weak.gpus[0].availableBytes = 1ull << 30;
    if (!gt2view::RecommendsLowSpecPreset(weak)) return 26;
    if (gt2view::GraphicsMemoryTooLow(weak)) return 27;
    gt2view::VulkanCapabilities tiny = weak;
    tiny.gpus[0].availableBytes = 400ull << 20;
    if (!gt2view::GraphicsMemoryTooLow(tiny)) return 28;
    // Unknown memory (0) is not treated as low: some drivers do not report a budget, and old APIs are not old cards.
    gt2view::VulkanCapabilities unknown = plenty;
    unknown.gpus[0].availableBytes = 0;
    unknown.gpus[0].deviceLocalBytes = 0;
    if (gt2view::RecommendsLowSpecPreset(unknown)) return 29;
    if (gt2view::GraphicsMemoryTooLow(unknown)) return 30;

    std::puts("PASS: Vulkan 1.1/1.2 fallback profile, env switch, layers and clear values");
    return 0;
}
