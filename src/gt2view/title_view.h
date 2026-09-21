#pragma once
// The title overlay's screens (title, options, save / load) through the Vulkan renderer's 2D path. The same rules as
// gt2view/menu_view.h (the frame's GPU primitives become quads sampled from the uploaded VRAM with the PS1 texture
// rules; polygons and lines as the pixels the PS1 rasteriser covers), for a frame of any width: the title overlay
// draws a 352 x 480 frame (E3 0,0 / E4 351,479), which the console shows across the full 4:3 picture.
#include <cstdint>
#include <vector>

#include "gt2formats/gt_menu_images.h"
#include "gt2view/vk_scene_renderer.h"

namespace gt2view {

class TitleView {
public:
    static constexpr uint32_t kVertexBase = 880'000, kVertexLimit = 110'000;

    explicit TitleView(VkSceneRenderer& renderer, uint32_t rowBase = 0) : renderer_(renderer), rowBase_(rowBase) {}

    void UploadVram(const gt2::MenuVram& vram);
    // Appends the draw items of `prims` (GPU order) for a frame `frameWidth` x 480 pixels shown at 4:3, or with square
    // pixels (a frameWidth x 480 window then shows it 1:1).
    void Build(const std::vector<gt2::MenuPrim>& prims, int frameWidth, float windowAspect, std::vector<DrawItem>& items, bool squarePixels = false);
    void SetRasterRules(gt2::MenuCanvas::Rules rules) { coverA_.rules = coverB_.rules = rules; }

private:
    struct Quad {
        float x[4], y[4], u[4], v[4];
        float colour[4][3];
        uint32_t page = 0, clut = 0, flags = 0;
        uint32_t blend = kBlendOpaque, stpPass = 0;
        bool triangles = false; // x/y are the GPU quad v0 v1 v2 v3 (triangles 0-1-2, 1-2-3)
    };
    void Emit(const gt2::MenuPrim& p, int frameWidth);
    void EmitRasterised(const gt2::MenuPrim& p, int frameWidth);

    VkSceneRenderer& renderer_;
    uint32_t rowBase_;
    uint32_t hdSize_ = 0;
    uint64_t hdGeneration_ = ~uint64_t(0);
    std::vector<Quad> quads_;
    gt2::MenuCanvas coverA_, coverB_;
    gt2::MenuVram noTextures_;
};

} // namespace gt2view
