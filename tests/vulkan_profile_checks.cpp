// The --diagnose profile choice (discussion #77): a GPU without dynamic rendering must get the render-pass
// compatibility path, not be reported as usable on the fixed-function 1.3 path.
#include "gt2view/vk_capabilities.h"
#include <cstdio>

int main() {
    using gt2view::ChooseGraphicsProfile;
    using gt2view::GraphicsProfile;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_0, false) != GraphicsProfile::Unsupported) return 1;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_1, false) != GraphicsProfile::Compatibility) return 2;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_2, false) != GraphicsProfile::Compatibility) return 3;
    // A 1.3 device without the dynamic-rendering feature still falls back to render passes.
    if (ChooseGraphicsProfile(VK_API_VERSION_1_3, false) != GraphicsProfile::Compatibility) return 4;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_3, true) != GraphicsProfile::Standard) return 5;
    if (ChooseGraphicsProfile(VK_API_VERSION_1_4, true) != GraphicsProfile::Standard) return 6;
    std::puts("PASS: Vulkan profiles classify 1.1/1.2 GPUs as the render-pass compatibility path");
    return 0;
}
