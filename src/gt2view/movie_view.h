#pragma once
#include <cstdint>
#include <vector>

#include "gt2view/vk_scene_renderer.h"

// RGB movie frames use a reserved external-texture range. Source dimensions keep
// the original framing and letterboxing when a prepared HD frame is larger.
namespace gt2view {

class MovieView {
public:
    static constexpr uint32_t kVertexBase = 1'048'560, kVertexLimit = 12; // above PanelView's range
    static constexpr uint32_t kTexels = 2048 * 2048;
    static constexpr uint32_t kTexelBase = VkSceneRenderer::kMovieTexelBase;

    explicit MovieView(VkSceneRenderer& renderer) : renderer_(renderer) {}

    // `rgb`: width * height * 3 bytes; each dimension is at most 2048.
    void Upload(const uint8_t* rgb, int width, int height, int sourceWidth = 0, int sourceHeight = 0);
    // The picture at (x, y) of a displayWidth x displayHeight console display shown 4:3 in a window of `windowAspect`.
    void Append(std::vector<DrawItem>& items, int displayWidth, int displayHeight, int x, int y, float windowAspect, float brightness = 1.0f);
    bool HasPicture() const { return width_ > 0; }

private:
    VkSceneRenderer& renderer_;
    int width_ = 0, height_ = 0, sourceWidth_ = 0, sourceHeight_ = 0;
    std::vector<uint32_t> texels_;
};

} // namespace gt2view
