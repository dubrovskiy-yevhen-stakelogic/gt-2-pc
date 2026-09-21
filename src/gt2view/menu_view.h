#pragma once
// The GT-mode menus through the Vulkan renderer's 2D path: the page's VRAM (the console layout the menus use:
// commonpic background, the page's 4-bit picture, the fonts, cursors, icons; gt2formats/gt_menu_images.h) is
// uploaded into the renderer's VRAM rows, and every primitive of a gt2::MenuFrame (the same list the software
// canvas rasterises, in the GPU's draw order) becomes a quad sampled from it with the PS1 texture rules
// (4-bit / 8-bit CLUT, texel 0 transparent, integer colour modulation, STP semi-transparency by the texpage mode).
// The 512 x 480 frame is scaled to the window: 4:3 with pillar / letter boxes, or square pixels (a window of
// 512 x 480 then shows the console's frame 1:1 for comparisons). Pages with flag bit 9 leave the 3D car area to
// whatever was drawn before (their black tiles are not drawn).
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "game/menu/menu_car.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2view/vk_scene_renderer.h"

namespace gt2 {
class GtfsVolume;
}

namespace gt2view {

class MenuView {
public:
    static constexpr uint32_t kVertexBase = 880'000, kVertexLimit = 110'000;

    // `rowBase` = the first renderer VRAM row of the menu's 512 console rows (0: the menus own the console rows
    // while no course is loaded).
    explicit MenuView(VkSceneRenderer& renderer, uint32_t rowBase = 0) : renderer_(renderer), rowBase_(rowBase) {}

    // Uploads the VRAM of a page (call when the page, i.e. its pictures, changed).
    void UploadVram(const gt2::MenuVram& vram);
    // Appends the draw items of `frame` (identity transforms, NDC positions, in the frame's order). With `layer3d`,
    // the primitives before frame.layer3dAt are drawn at the far depth, then `layer3d` appends its items (the 3D car
    // view, depth-tested among themselves and over those primitives), then the rest in front.
    void Build(const gt2::MenuFrame& frame, float windowAspect, std::vector<DrawItem>& items, bool squarePixels = false,
               const std::function<void(std::vector<DrawItem>&)>& layer3d = {});
    // The frame -> NDC mapping of Build: ndc = (frame / size * 2 - 1) * scale.
    static void FrameScale(float windowAspect, bool squarePixels, float& scaleX, float& scaleY);
    // Polygon / line rules (gt2::MenuCanvas::Rules): the PS1 GPU's (default), or our interpreter GPU's to compare
    // with gt2play captures, which that GPU produced.
    void SetRasterRules(gt2::MenuCanvas::Rules rules) { coverA_.rules = coverB_.rules = rules; }
    // The frame's size (default the menus' 512 x 480; the race overlay's full-screen views are 352 x 480, shown at the
    // same 4:3 display aspect). At most the canvas' size.
    void SetFrameSize(int width, int height) { frameWidth_ = width, frameHeight_ = height; }
    // Polygons as interpolated triangles (vertex colours, no dither; those with a drawing area of their own clipped to
    // it geometrically) instead of the exact rasterised pixels: for frames with large gouraud areas (the race overlay's
    // view headers), whose pixel runs would not fit the vertex range.
    void SetInterpolatedPolygons(bool on) { interpolated_ = on; }

private:
    struct Quad {
        float x[4], y[4], u[4], v[4];  // corners TL TR BR BL (or a triangle pair for polygons, see Build)
        float colour[4][3];
        uint32_t page = 0, clut = 0, flags = 0;
        uint32_t blend = kBlendOpaque, stpPass = 0;
        bool triangles = false;         // x/y are the GPU quad v0 v1 v2 v3 (triangles 0-1-2, 1-2-3)
    };
    void Emit(const gt2::MenuPrim& p);
    // Polygons and lines: the pixels the PS1 rasteriser covers (gt2::MenuCanvas, the same rules as the software
    // renderer: top-left fill, gouraud + dither) become 1 x 1 quads of their exact 5-bit colour; semi-transparent
    // ones blend on the GPU by the primitive's mode.
    void EmitRasterised(const gt2::MenuPrim& p);
    void EmitInterpolated(const gt2::MenuPrim& p);

    VkSceneRenderer& renderer_;
    uint32_t rowBase_;
    std::vector<Quad> quads_;
    gt2::MenuCanvas coverA_, coverB_;   // two backgrounds (black / white): a pixel equal in both is covered
    gt2::MenuVram noTextures_;
    int frameWidth_ = gt2::MenuCanvas::kWidth, frameHeight_ = gt2::MenuCanvas::kHeight;
    bool interpolated_ = false;
    int hdBackground_ = -2;
    uint64_t hdGeneration_ = ~uint64_t(0);
    uint32_t hdSize_ = 0;
};

class SceneAssets;

// The 3D car of the menus' car view (game/menu/menu_car.h): the disc car (.cdo LOD 0 with wheels, .cdp paints)
// drawn with the race renderer's car items (body, ground shadow, reflection pass) through the view's projection,
// clipped to the viewport; the reflection samples the map inside arcade/gt_cursor.tim (page 9, CLUT 0x2624) at
// colour 0x40 as 0x8001A8A4 sets it.
class MenuCarView {
public:
    MenuCarView(VkSceneRenderer& renderer, const gt2::GtfsVolume& vol);
    ~MenuCarView();
    // Loads the car of `modelId` (packed id) when it is not the one loaded; false when it cannot be loaded.
    bool Use(uint32_t modelId);
    // Appends the car's items: `projection` = the car's camera (menu::MenuCarProject with the yaw).
    void Append(std::vector<DrawItem>& items, const gt2::menu::MenuCarProjection& projection, int paint, float windowAspect, bool squarePixels);
    // The frame size of the projection's pixels (default 512 x 480; the race overlay's views: 352 x 480).
    void SetFrameSize(int width, int height) { frameWidth_ = width, frameHeight_ = height; }
    // The reflection pass's map and colour (default the menus': page 9, CLUT 0x2624, 0x40; the race overlay's car:
    // page 9, CLUT 0x7FD7 = the course's map left in VRAM; colour 0 = no reflection pass).
    void SetReflection(uint16_t tpage, uint16_t clut, uint8_t colour) { envPage_ = tpage, envClut_ = clut, envColour_ = colour; envRowBase_ = 0; }
    void LoadMenuReflection(); // Dedicated rows keep the map intact when Arcade uploads its UI textures.
    // Fitted wheels of the shown car (0x8001AC20 with a garage car's CarConfig +0x00, 0x8001AEF8 the wheel shop's preview):
    // the carwheel/ TIM on the rims (nullptr = the car's own rims) and the rims' dish (0x80061308: rim at W / 2 - dish;
    // < 0 = the default of 0x80061504, 122). Call when they change (reloads the car for a new dish).
    void SetWheels(const gt2::menu::MenuWheelTexture* wheel, int dish);
    // The ground shadow (default on; the race overlay's trophy 0x80048528 draws none).
    void SetShadow(bool on) { shadow_ = on; }
    // Two cars at once (the arcade 2PLAYER BATTLE page, game/arcade/arcade_battle.h): both loaded into one set of car slots
    // (packed ids; 0 = none), reloaded when either changes; AppendPair draws car `which` (0 / 1) as Append draws the single car.
    bool UsePair(uint32_t a, uint32_t b);
    void AppendPair(std::vector<DrawItem>& items, int which, const gt2::menu::MenuCarProjection& projection, int paint, float windowAspect, bool squarePixels);

private:
    VkSceneRenderer& renderer_;
    const gt2::GtfsVolume& vol_;
    std::unique_ptr<SceneAssets> assets_;
    uint32_t model_ = 0;
    int slot_ = -1;
    int32_t lift_ = 0;
    int frameWidth_ = gt2::MenuCanvas::kWidth, frameHeight_ = gt2::MenuCanvas::kHeight;
    uint16_t envPage_ = 9, envClut_ = uint16_t((576 / 16) | (152 << 6)); // 0x2624
    uint8_t envColour_ = 0x40;
    uint32_t envRowBase_ = 0;
    bool shadow_ = true;
    int dish_ = -1;
    std::array<uint32_t, 2> pairModels_{};
    std::array<int, 2> pairSlots_{-1, -1};
    std::array<int32_t, 2> pairLifts_{};
    bool Load(uint32_t modelId);
    void AppendSlot(std::vector<DrawItem>& items, int slot, int32_t lift, const gt2::menu::MenuCarProjection& projection, int paint, float windowAspect, bool squarePixels);
};

} // namespace gt2view
