#include "gt2view/surface_format.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <vector>

int main() {
    using gt2view::ChooseDisplaySurfaceFormat;
    const VkSurfaceFormatKHR sdr{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    const std::array offers{
        sdr,
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT},
        VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_DISPLAY_P3_LINEAR_EXT},
        VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    };
    // Driver enumeration order must not replace SDR with the last P3 match.
    std::array order{0, 1, 2, 3, 4};
    unsigned permutations = 0;
    do {
        std::vector<VkSurfaceFormatKHR> formats;
        for (int i : order) formats.push_back(offers[i]);
        const auto picked = ChooseDisplaySurfaceFormat(formats);
        if (picked.format != sdr.format || picked.colorSpace != sdr.colorSpace) return 1;
        ++permutations;
    } while (std::next_permutation(order.begin(), order.end()));
    const std::array rgba{offers[3], offers[4]};
    if (ChooseDisplaySurfaceFormat(rgba).format != VK_FORMAT_R8G8B8A8_UNORM) return 2;
    const std::array any{VkSurfaceFormatKHR{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
    if (ChooseDisplaySurfaceFormat(any).format != sdr.format) return 3;
    const std::array<VkSurfaceFormatKHR, 0> empty{};
    const std::array onlySrgbAttachment{offers[1]};
    const std::array onlyP3{offers[3]};
    for (const auto formats : {std::span<const VkSurfaceFormatKHR>(empty),
                              std::span<const VkSurfaceFormatKHR>(onlySrgbAttachment),
                              std::span<const VkSurfaceFormatKHR>(onlyP3)}) {
        bool rejected = false;
        try { (void)ChooseDisplaySurfaceFormat(formats); }
        catch (const std::runtime_error&) { rejected = true; }
        if (!rejected) return 4;
    }
    std::printf("PASS: %u format orders, RGBA/undefined fallback, unsupported/empty surfaces\n", permutations);
}
