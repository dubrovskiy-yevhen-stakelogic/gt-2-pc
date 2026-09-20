#include "gt2formats/boot_images.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/title_assets.h"
#include <cstring>
#include <stdexcept>

namespace gt2 {
BootImage DecodeBootTim(const std::vector<uint8_t>& b) {
    auto require = [&](size_t at, size_t size) {
        if (at > b.size() || size > b.size() - at) throw std::runtime_error("boot TIM: truncated block");
    };
    auto u16 = [&](size_t at) { require(at, 2); return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8; };
    auto u32 = [&](size_t at) { require(at, 4); return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 |
        uint32_t(b[at + 2]) << 16 | uint32_t(b[at + 3]) << 24; };
    if (u32(0) != 0x10 || (u32(4) != 8 && u32(4) != 9)) throw std::runtime_error("boot TIM: unsupported format");
    const bool fourBit = u32(4) == 8;
    const size_t colors = fourBit ? 16 : 256;
    const size_t clutSize = u32(8);
    require(8, clutSize);
    if (clutSize < 12 + colors * 2 || u16(16) < colors || u16(18) == 0)
        throw std::runtime_error("boot TIM: incomplete palette");
    const size_t image = 8 + clutSize;
    const size_t imageSize = u32(image);
    require(image, imageSize);
    if (imageSize < 12) throw std::runtime_error("boot TIM: invalid image block");
    BootImage result;
    const size_t rowBytes = u16(image + 8) * 2;
    result.width = int(rowBytes * (fourBit ? 2 : 1));
    result.height = int(u16(image + 10));
    if (result.width <= 0 || result.width > 640 || result.height <= 0 || result.height > 512 ||
        rowBytes * size_t(result.height) > imageSize - 12) throw std::runtime_error("boot TIM: invalid dimensions");
    result.rgb.resize(size_t(result.width) * size_t(result.height) * 3);
    for (size_t i = 0; i < result.rgb.size() / 3; ++i) {
        const uint8_t packed = b[image + 12 + (fourBit ? i / 2 : i)];
        const size_t index = fourBit ? (packed >> ((i & 1) * 4)) & 15 : packed;
        const uint32_t color = u16(20 + index * 2);
        for (size_t c = 0; c < 3; ++c) {
            const uint32_t v = (color >> (c * 5)) & 31;
            result.rgb[i * 3 + c] = uint8_t((v << 3) | (v >> 2));
        }
    }
    return result;
}

BootImage LoadBootImage(const GuestImage& exe, const char* name) {
    if (exe.profile && (exe.profile->build == ExeBuild::kSimEu || exe.profile->build == ExeBuild::kArcadeEu)) {
        if (std::strcmp(name,"logo-scea.tim")==0) name="logo-scee.tim";
        else if (std::strcmp(name,"notice.tim")==0) name="notice_eu.tim";
    }
    const size_t length = std::strlen(name) + 1;
    for (size_t i = 0; i + 10 + length <= exe.bytes.size(); ++i) {
        const auto* p = exe.bytes.data() + i;
        if (p[0] == 0x1f && p[1] == 0x8b && p[2] == 8 && p[3] == 8 &&
            std::memcmp(p + 10, name, length) == 0)
            return DecodeBootTim(InflateEmbeddedGzip(exe, exe.base + uint32_t(i)));
    }
    throw std::runtime_error(std::string("boot image not found: ") + name);
}
}
