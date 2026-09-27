#pragma once
#include <vulkan/vulkan.h>
#include <initializer_list>
#include <span>
#include <stdexcept>

namespace gt2view {
// Shaders and PS1 blending already produce display-referred sRGB values.
// Both the attachment encoding and the presentation colour space must match.
inline VkSurfaceFormatKHR ChooseDisplaySurfaceFormat(std::span<const VkSurfaceFormatKHR> formats) {
    for (const auto preferred : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        for (const auto& f : formats) {
            if (f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (f.format == preferred || (formats.size() == 1 && f.format == VK_FORMAT_UNDEFINED)))
                return {preferred, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        }
    }
    // An sRGB attachment would encode the values a second time; linear/P3/HDR
    // presentation would reinterpret them. Neither is a valid silent fallback.
    throw std::runtime_error("Vulkan surface has no 8-bit UNORM / sRGB nonlinear display format");
}
} // namespace gt2view
