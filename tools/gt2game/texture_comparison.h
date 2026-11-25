#pragma once
#include "gt2formats/generated_world.h"
#include "gt2formats/hd_media.h"
#include "gt2formats/png_reader.h"
#include "gt2export/png_deflate.h"
#include <algorithm>
#include <chrono>
#include <filesystem>

namespace gt2game {
template<class Render>
void CaptureTexturePair(Render render) {
    namespace fs = std::filesystem;
    const bool generated = gt2::generated::enabled, upscale = gt2::hd::Enabled();
    const auto dir = fs::absolute(gt2::generated::root.parent_path().parent_path() / "screenshots");
    const auto stem = "comparison-" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
    const auto tmp = dir / stem;
    fs::create_directories(dir);
    if (!fs::create_directory(tmp)) throw std::runtime_error("screenshot name already in use");
    struct Restore {
        bool generated, upscale;
        fs::path temp;
        ~Restore() {
            gt2::generated::SetEnabled(generated);
            gt2::hd::SetEnabled(upscale);
            std::error_code ec;
            for (const auto* name : {"original.png","hd.png","pair.png"}) fs::remove(temp / name, ec);
            fs::remove(temp, ec);
        }
    } restore{generated, upscale, tmp};
    gt2::generated::SetEnabled(false);
    gt2::hd::SetEnabled(false);
    render((tmp / "original.png").string());
    gt2::generated::SetEnabled(true);
    gt2::hd::SetEnabled(upscale);
    render((tmp / "hd.png").string());
    const auto left = gt2::ReadPngFile((tmp / "original.png").string());
    const auto right = gt2::ReadPngFile((tmp / "hd.png").string());
    if (left.width != right.width || left.height != right.height) throw std::runtime_error("window resized during screenshot");
    const int w = left.width, scale = std::clamp(w / 400, 1, 5), header = 12 * scale;
    const size_t stride = size_t(w) * 8;
    std::vector<uint8_t> rgba(stride * (left.height + header), 255);
    for (int y = 0; y < left.height; ++y) {
        auto row = rgba.begin() + stride * (header + y);
        std::copy_n(left.rgba.begin() + stride / 2 * y, stride / 2, row);
        std::copy_n(right.rgba.begin() + stride / 2 * y, stride / 2, row + stride / 2);
    }
    // Column bitmaps for the two captions, in reading order.
    constexpr uint8_t letters[][5] = {
        {62,65,65,65,62},{127,9,25,41,70},{0,65,127,65,0},{62,65,73,73,122},
        {0,65,127,65,0},{127,4,8,16,127},{126,17,17,17,126},{127,64,64,64,64},
        {127,8,8,8,127},{127,65,65,34,28}};
    for (int i = 0; i < 10; ++i) {
        const int start = (i < 8 ? 0 : w) + scale * (2 + 6 * (i < 8 ? i : i - 8));
        for (int y = 0; y < 7 * scale; ++y) for (int x = 0; x < 5 * scale; ++x) {
            if (start+x >= (i < 8 ? w : 2*w) || !(letters[i][x/scale] & (1 << (y/scale)))) continue;
            std::fill_n(rgba.begin() + (y+2*scale)*stride + (start+x)*4, 3, 30);
        }
    }
    gt2::WritePngRgbaCompressed((tmp/"pair.png").string(),2*w,left.height+header,rgba);
    const auto output = dir / (stem + ".png");
    fs::rename(tmp/"pair.png",output);
    std::printf("F8: %s\n",output.string().c_str());
}
}
