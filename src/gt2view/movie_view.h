#pragma once
#include <cstdint>
#include <vector>

#include "gt2view/vk_scene_renderer.h"

// A full-screen movie picture (gt2formats/str_video.h MovieImage: 24-bit RGB) through the Vulkan renderer: the frame goes
// to the external RGBA8 texture store (its last 640 x 512 texels, which no mod mesh reaches in practice) and is drawn as
// one nearest-texel quad inside the 4:3 area of the console display it belongs to (the PS1 display the movie player
// sets up: 320 x 240 for the intro, 640 x 240 for the endings, the picture at its y offset; black around it).
namespace gt2view {

class MovieView {
public:
    static constexpr uint32_t kVertexBase = 1'048'560, kVertexLimit = 12; // above PanelView's range
    static constexpr uint32_t kTexels = 640 * 512;
    static constexpr uint32_t kTexelBase = VkSceneRenderer::kMovieTexelBase;

    explicit MovieView(VkSceneRenderer& renderer) : renderer_(renderer) {}

    // `rgb`: width * height * 3 bytes (width <= 640, height <= 512).
    void Upload(const uint8_t* rgb, int width, int height);
    // The picture at (x, y) of a displayWidth x displayHeight console display shown 4:3 in a window of `windowAspect`.
    void Append(std::vector<DrawItem>& items, int displayWidth, int displayHeight, int x, int y, float windowAspect, float brightness = 1.0f);
    bool HasPicture() const { return width_ > 0; }

private:
    VkSceneRenderer& renderer_;
    int width_ = 0, height_ = 0;
    std::vector<uint32_t> texels_;
};

} // namespace gt2view
