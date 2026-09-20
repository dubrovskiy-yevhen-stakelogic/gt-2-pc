#include "gt2view/movie_view.h"

#include <algorithm>
#include <stdexcept>

namespace gt2view {

void MovieView::Upload(const uint8_t* rgb, int width, int height) {
    if (width <= 0 || height <= 0 || width > 640 || height > 512) throw std::runtime_error("movie picture larger than 640 x 512");
    texels_.resize(size_t(width) * size_t(height));
    for (size_t i = 0; i < texels_.size(); i++)
        texels_[i] = uint32_t(rgb[i * 3]) | uint32_t(rgb[i * 3 + 1]) << 8 | uint32_t(rgb[i * 3 + 2]) << 16 | 0xFF000000u;
    renderer_.UploadExternalTexture(kTexelBase, uint32_t(texels_.size()), texels_.data());
    width_ = width;
    height_ = height;
}

void MovieView::Append(std::vector<DrawItem>& items, int displayWidth, int displayHeight, int x, int y, float windowAspect, float brightness) {
    if (!HasPicture()) return;
    // The display's 4:3 area centred in the window (as SceneAssets::SetOverlay), then the picture's rectangle in it.
    const float k = std::min(1.0f, (4.0f / 3.0f) / windowAspect), ky = std::min(1.0f, windowAspect / (4.0f / 3.0f));
    auto nx = [&](int px) { return -k + 2.0f * k * float(px) / float(displayWidth); };
    auto ny = [&](int py) { return -ky + 2.0f * ky * float(py) / float(displayHeight); };
    const float corners[4][4] = {{nx(x), ny(y), 0, 0},
                                 {nx(x + width_), ny(y), float(width_), 0},
                                 {nx(x + width_), ny(y + height_), float(width_), float(height_)},
                                 {nx(x), ny(y + height_), 0, float(height_)}};
    std::vector<SceneVertex> quad;
    for (int i : {0, 1, 2, 0, 2, 3}) {
        SceneVertex v{};
        v.pos[0] = corners[i][0];
        v.pos[1] = corners[i][1];
        v.pos[2] = 1.0f; // reversed Z: in front
        v.texel[0] = corners[i][2];
        v.texel[1] = corners[i][3];
        v.color[0] = v.color[1] = v.color[2] = std::clamp(brightness, 0.0f, 1.0f);
        v.page = kTexelBase;
        v.clut = uint32_t(width_) | uint32_t(height_) << 16;
        v.flags = kTextured | kExternalTexture;
        quad.push_back(v);
    }
    renderer_.SetVertices(kVertexBase, quad);
    DrawItem item;
    item.firstVertex = kVertexBase;
    item.vertexCount = 6;
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::copy(identity, identity + 16, item.mvp);
    items.push_back(item);
}

} // namespace gt2view
