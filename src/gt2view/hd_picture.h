#pragma once
#include "gt2formats/hd_media.h"
#include "gt2formats/png_reader.h"
#include "gt2view/vk_scene_renderer.h"
#include <cstdio>
#include <stdexcept>

namespace gt2view {
inline uint32_t UploadHdPicture(VkSceneRenderer& renderer, const std::string& name, int sourceWidth, int sourceHeight) {
    const auto path = gt2::hd::Asset("images/" + name);
    if (path.empty()) return 0;
    try {
        const auto p = gt2::ReadPngFile(path);
        if (p.width <= 0 || p.height <= 0 || p.width > 2048 || p.height > 2048 ||
            int64_t(p.width) * sourceHeight != int64_t(p.height) * sourceWidth)
            throw std::runtime_error("unexpected HD picture size");
        std::vector<uint32_t> rgba(size_t(p.width) * p.height);
        for (size_t i = 0; i < rgba.size(); ++i)
            rgba[i] = uint32_t(p.rgba[i*4]) | uint32_t(p.rgba[i*4+1])<<8 | uint32_t(p.rgba[i*4+2])<<16 | uint32_t(p.rgba[i*4+3])<<24;
        renderer.UploadExternalTexture(VkSceneRenderer::kHdMenuTexelBase, uint32_t(rgba.size()), rgba.data());
        return uint32_t(p.width) | uint32_t(p.height)<<16;
    } catch (const std::exception& e) { std::printf("HD picture fallback: %s: %s\n", name.c_str(), e.what()); return 0; }
}
}
