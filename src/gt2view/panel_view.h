#pragma once
// 2D layers of gt2game drawn over whatever the frame already holds (the race scene and the HUD, or nothing):
//   - GPU primitive lists of a 2D frame (gt2formats MenuPrim): the race overlay's 352 x 480 menus
//     (gt2view/race_menus.h: licence test menu, event pre-race menu) and our own 512 x 480 panels (the menus' font
//     from the disc, arcade/gtmode_font.tim) where the original's screen is not ported yet (Build);
//   - the race overlay's 320 x 240 screens over the race (gt2view/race_overlay_screens.h: pause, race end) with their
//     draw modes (BuildOverlay).
// Polygons and lines become interpolated quads; the pixel-exact reference of every screen is the software canvas of
// its comparison tool. The layers have their own vertex range and VRAM rows (1024..1535, unused by the race and the
// GT-mode menus), so they can be drawn in the same frame as the race's items or the HUD; the rows hold one 1024 x 512
// console VRAM image that the caller composes (tools/gt2game/panel.h).
#include <cstdint>
#include <vector>

#include "gt2formats/gt_menu_images.h"
#include "gt2view/race_overlay_screens.h"
#include "gt2view/vk_scene_renderer.h"

namespace gt2view {

class PanelView {
public:
    static constexpr uint32_t kVertexBase = 1'040'128, kVertexLimit = 8'400; // Build: the first 6000, BuildOverlay: the rest
    static constexpr uint32_t kRowBase = 1024;

    explicit PanelView(VkSceneRenderer& renderer) : renderer_(renderer) {}
    // A console VRAM image (1024 x 512 words) into rows 1024..1535.
    void UploadVram(const gt2::MenuVram& vram);
    void UploadVram(const std::vector<uint16_t>& words);
    // Appends the items of `prims` (GPU order) of a 512 x 480 frame, or of a frame of any size, shown at 4:3 in front of
    // everything drawn before.
    void Build(const std::vector<gt2::MenuPrim>& prims, float windowAspect, std::vector<DrawItem>& items);
    void Build(const std::vector<gt2::MenuPrim>& prims, int frameWidth, int frameHeight, float windowAspect, std::vector<DrawItem>& items);
    // Appends the race overlay's primitives (GPU order, draw modes included) of a 320 x 240 frame shown like the race's
    // 4:3 picture, centred in the window; tiles over the whole frame width (the darkening) cover the whole window.
    void BuildOverlay(const std::vector<gt2::raceui::Gp0Prim>& prims, float windowAspect, std::vector<DrawItem>& items);

private:
    struct Quad {
        float x[4], y[4], u[4], v[4]; // GPU quad order v0 v1 v2 v3 (triangles 0-1-2, 1-2-3)
        float colour[4][3];
        uint32_t page = 0, clut = 0, flags = 0, blend = kBlendOpaque, stpPass = 0;
    };
    void Submit(std::vector<Quad>& quads, int frameWidth, int frameHeight, float windowAspect, uint32_t base, uint32_t limit, std::vector<DrawItem>& items);
    VkSceneRenderer& renderer_;
};

} // namespace gt2view
