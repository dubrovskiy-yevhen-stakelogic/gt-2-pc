#include "gt2view/menu_view.h"

#include <algorithm>
#include <array>
#include <string>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "gt2formats/car_info.h"
#include "gt2view/scene_assets.h"

namespace gt2view {

void MenuView::UploadVram(const gt2::MenuVram& vram) { renderer_.UploadVram(rowBase_, gt2::MenuVram::kHeight, vram.Words().data()); }

void MenuView::Emit(const gt2::MenuPrim& p) {
    const uint32_t mode = uint32_t(p.tpage >> 5) & 3;
    auto rgb = [](uint32_t c, float out[3], bool quantise) {
        for (int k = 0; k < 3; k++) {
            const uint32_t v = (c >> (8 * k)) & 0xFF;
            out[k] = quantise ? float(v >> 3) / 31.0f : float(v) / 255.0f; // untextured: the 5-bit VRAM colour
        }
    };
    auto rect = [&](float x0, float y0, float x1, float y1, uint32_t colour, bool quantise) {
        Quad q{};
        const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
        for (int k = 0; k < 4; k++) {
            q.x[k] = xs[k];
            q.y[k] = ys[k];
            rgb(colour, q.colour[k], quantise);
        }
        return q;
    };
    switch (p.kind) {
    case gt2::MenuPrim::kSprite: {
        Quad q = rect(p.x[0], p.y[0], float(p.x[0] + p.w), float(p.y[0] + p.h), p.colour[0], false);
        const float us[4] = {float(p.u), float(p.u + p.w), float(p.u + p.w), float(p.u)}, vs[4] = {float(p.v), float(p.v), float(p.v + p.h), float(p.v + p.h)};
        std::copy(us, us + 4, q.u);
        std::copy(vs, vs + 4, q.v);
        q.page = uint32_t((p.tpage & 0xF) * 64) | ((rowBase_ + uint32_t(((p.tpage >> 4) & 1) * 256)) << 16);
        q.clut = uint32_t((p.clut & 0x3F) * 16) | ((rowBase_ + uint32_t(p.clut >> 6)) << 16);
        q.flags = kTextured | kIntegerModulate | ((uint32_t(p.tpage >> 7) & 3) << 8);
        if (p.semi) { // PS1: only the texels with the STP bit blend
            q.stpPass = 1;
            quads_.push_back(q);
            q.stpPass = 2;
            q.blend = mode;
        }
        quads_.push_back(q);
        return;
    }
    case gt2::MenuPrim::kTile: {
        Quad q = rect(p.x[0], p.y[0], float(p.x[0] + p.w), float(p.y[0] + p.h), p.colour[0], true);
        if (p.semi) q.blend = mode;
        quads_.push_back(q);
        return;
    }
    case gt2::MenuPrim::kPolyF4:
    case gt2::MenuPrim::kPolyG4:
        if (interpolated_) EmitInterpolated(p);
        else EmitRasterised(p);
        return;
    case gt2::MenuPrim::kLine: EmitRasterised(p); return;
    case gt2::MenuPrim::kPolyFT4: { // a textured quad (GPU order), modulated as a sprite
        Quad q{};
        q.triangles = true;
        for (int k = 0; k < 4; k++) {
            q.x[k] = p.x[k], q.y[k] = p.y[k], q.u[k] = p.tu[k], q.v[k] = p.tv[k];
            for (int j = 0; j < 3; j++) q.colour[k][j] = float((p.colour[0] >> (8 * j)) & 0xFF) / 255.0f;
        }
        q.page = uint32_t((p.tpage & 0xF) * 64) | ((rowBase_ + uint32_t(((p.tpage >> 4) & 1) * 256)) << 16);
        q.clut = uint32_t((p.clut & 0x3F) * 16) | ((rowBase_ + uint32_t(p.clut >> 6)) << 16);
        q.flags = kTextured | kIntegerModulate | ((uint32_t(p.tpage >> 7) & 3) << 8);
        if (p.semi) {
            q.stpPass = 1;
            quads_.push_back(q);
            q.stpPass = 2;
            q.blend = mode;
        }
        quads_.push_back(q);
        return;
    }
    }
}

void MenuView::EmitInterpolated(const gt2::MenuPrim& p) {
    struct Corner { float x, y, c[3]; };
    auto corner = [&](int k) {
        Corner v{float(p.x[k]), float(p.y[k]), {}};
        const uint32_t c = p.kind == gt2::MenuPrim::kPolyG4 ? p.colour[k] : p.colour[0];
        for (int j = 0; j < 3; j++) v.c[j] = float(((c >> (8 * j)) & 0xFF) >> 3) / 31.0f; // the 5-bit VRAM colour
        return v;
    };
    const bool triangle = p.x[3] == p.x[2] && p.y[3] == p.y[2];
    const int tris[2][3] = {{0, 1, 2}, {1, 2, 3}}; // GPU quad order
    const bool clipped = p.clipX1 >= p.clipX0;
    const uint32_t mode = uint32_t(p.tpage >> 5) & 3;
    for (int t = 0; t < (triangle ? 1 : 2); t++) {
        std::vector<Corner> poly = {corner(tris[t][0]), corner(tris[t][1]), corner(tris[t][2])};
        if (clipped) { // Sutherland-Hodgman against the drawing area (pixel edges)
            const float edges[4] = {float(p.clipX0), float(p.clipX1 + 1), float(p.clipY0), float(p.clipY1 + 1)};
            for (int e = 0; e < 4 && !poly.empty(); e++) {
                std::vector<Corner> out;
                auto inside = [&](const Corner& v) { return e == 0 ? v.x >= edges[0] : e == 1 ? v.x <= edges[1] : e == 2 ? v.y >= edges[2] : v.y <= edges[3]; };
                for (size_t i = 0; i < poly.size(); i++) {
                    const Corner& a = poly[i];
                    const Corner& b = poly[(i + 1) % poly.size()];
                    if (inside(a)) out.push_back(a);
                    if (inside(a) != inside(b)) {
                        const float da = e < 2 ? a.x - edges[e] : a.y - edges[e], db = e < 2 ? b.x - edges[e] : b.y - edges[e];
                        const float s = da / (da - db);
                        Corner m{a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, {}};
                        for (int j = 0; j < 3; j++) m.c[j] = a.c[j] + (b.c[j] - a.c[j]) * s;
                        out.push_back(m);
                    }
                }
                poly.swap(out);
            }
        }
        for (size_t i = 1; i + 1 < poly.size(); i++) { // fan; each triangle as a GPU quad with v3 = v2
            Quad q{};
            const Corner* c[4] = {&poly[0], &poly[i], &poly[i + 1], &poly[i + 1]};
            for (int k = 0; k < 4; k++) {
                q.x[k] = c[k]->x;
                q.y[k] = c[k]->y;
                for (int j = 0; j < 3; j++) q.colour[k][j] = c[k]->c[j];
            }
            q.triangles = true;
            if (p.semi) q.blend = mode;
            quads_.push_back(q);
        }
    }
}

void MenuView::EmitRasterised(const gt2::MenuPrim& p) {
    const int n = p.kind == gt2::MenuPrim::kLine ? 2 : 4;
    int x0 = p.x[0], x1 = p.x[0], y0 = p.y[0], y1 = p.y[0];
    for (int k = 1; k < n; k++) {
        x0 = std::min(x0, int(p.x[k])), x1 = std::max(x1, int(p.x[k]));
        y0 = std::min(y0, int(p.y[k])), y1 = std::max(y1, int(p.y[k]));
    }
    x0 = std::max(x0, 0), y0 = std::max(y0, 0);
    x1 = std::min(x1, frameWidth_ - 1), y1 = std::min(y1, frameHeight_ - 1);
    if (x0 > x1 || y0 > y1) return;
    coverA_.Fill(x0, y0, x1 - x0 + 1, y1 - y0 + 1, 0, 0, 0);
    coverB_.Fill(x0, y0, x1 - x0 + 1, y1 - y0 + 1, 255, 255, 255);
    gt2::MenuPrim opaque = p;
    opaque.semi = false;
    coverA_.Draw(noTextures_, opaque);
    coverB_.Draw(noTextures_, opaque);
    const uint32_t mode = uint32_t(p.tpage >> 5) & 3;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            const uint16_t a = coverA_.At(x, y) & 0x7FFF;
            if (a != (coverB_.At(x, y) & 0x7FFF)) continue; // not covered
            int end = x + 1; // a run of covered pixels of the same colour becomes one quad
            while (end <= x1 && (coverA_.At(end, y) & 0x7FFF) == a && (coverB_.At(end, y) & 0x7FFF) == a) end++;
            Quad q{};
            const float xs[4] = {float(x), float(end), float(end), float(x)}, ys[4] = {float(y), float(y), float(y + 1), float(y + 1)};
            x = end - 1;
            for (int k = 0; k < 4; k++) {
                q.x[k] = xs[k];
                q.y[k] = ys[k];
                q.colour[k][0] = float(a & 31) / 31.0f;
                q.colour[k][1] = float((a >> 5) & 31) / 31.0f;
                q.colour[k][2] = float((a >> 10) & 31) / 31.0f;
            }
            if (p.semi) q.blend = mode;
            quads_.push_back(q);
        }
}

void MenuView::FrameScale(float windowAspect, bool squarePixels, float& scaleX, float& scaleY) {
    // The 512 x 480 frame in the window: 4:3 display aspect (the console's 640 x 480 picture), or square pixels.
    constexpr float kW = float(gt2::MenuCanvas::kWidth), kH = float(gt2::MenuCanvas::kHeight);
    const float frameAspect = squarePixels ? kW / kH : 4.0f / 3.0f;
    scaleX = std::min(1.0f, frameAspect / windowAspect);
    scaleY = std::min(1.0f, windowAspect / frameAspect);
}

void MenuView::Build(const gt2::MenuFrame& frame, float windowAspect, std::vector<DrawItem>& items, bool squarePixels,
                     const std::function<void(std::vector<DrawItem>&)>& layer3d) {
    quads_.clear();
    size_t splitQuad = SIZE_MAX; // quads before it are drawn at the far depth, before the 3D layer
    for (size_t i = 0; i < frame.prims.size(); i++) {
        if (layer3d && i == frame.layer3dAt) splitQuad = quads_.size();
        Emit(frame.prims[i]);
    }
    if (layer3d && splitQuad == SIZE_MAX) splitQuad = quads_.size();
    if (quads_.size() * 6 > kVertexLimit) quads_.resize(kVertexLimit / 6);
    splitQuad = std::min(splitQuad, quads_.size());
    const float kW = float(frameWidth_), kH = float(frameHeight_);
    float sx = 1, sy = 1;
    FrameScale(windowAspect, squarePixels, sx, sy);
    std::vector<SceneVertex> vertices;
    vertices.reserve(quads_.size() * 6);
    size_t quadIndex = 0;
    auto vertex = [&](const Quad& q, int k) {
        SceneVertex v{};
        v.pos[0] = (q.x[k] / kW * 2.0f - 1.0f) * sx;
        v.pos[1] = (q.y[k] / kH * 2.0f - 1.0f) * sy;
        // Reversed Z: 1 = in front, 0 = the far plane (the quads under a 3D layer); equal depth lets later quads
        // cover earlier ones.
        v.pos[2] = quadIndex < splitQuad ? 0.0f : 1.0f;
        v.texel[0] = q.u[k];
        v.texel[1] = q.v[k];
        for (int j = 0; j < 3; j++) v.color[j] = q.colour[k][j];
        v.page = q.page;
        v.clut = q.clut;
        v.flags = q.flags;
        vertices.push_back(v);
    };
    for (quadIndex = 0; quadIndex < quads_.size(); quadIndex++) {
        const Quad& q = quads_[quadIndex];
        static constexpr int kRect[6] = {0, 1, 2, 0, 2, 3}, kGpu[6] = {0, 1, 2, 1, 2, 3};
        for (int k : q.triangles ? kGpu : kRect) vertex(q, k);
    }
    renderer_.SetVertices(kVertexBase, vertices);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    size_t first = 0;
    bool layerDone = !layer3d;
    while (first < quads_.size() || !layerDone) {
        if (!layerDone && first == splitQuad) {
            layer3d(items);
            layerDone = true;
            continue;
        }
        size_t last = first;
        while (last + 1 < quads_.size() && last + 1 != splitQuad && quads_[last + 1].blend == quads_[first].blend &&
               quads_[last + 1].stpPass == quads_[first].stpPass)
            last++;
        DrawItem item;
        item.firstVertex = kVertexBase + uint32_t(first * 6);
        item.vertexCount = uint32_t((last - first + 1) * 6);
        item.blend = quads_[first].blend;
        item.stpPass = quads_[first].stpPass;
        std::copy(identity, identity + 16, item.mvp);
        items.push_back(item);
        first = last + 1;
    }
}

// ---------------------------------------------------------------- the car view

MenuCarView::MenuCarView(VkSceneRenderer& renderer, const gt2::GtfsVolume& vol) : renderer_(renderer), vol_(vol) {}
MenuCarView::~MenuCarView() = default;
void MenuCarView::LoadMenuReflection() {
    gt2::MenuVram map;
    map.UploadTimToPage(vol_.Read("arcade/gt_cursor.tim"), 9);
    renderer_.UploadVram(VkSceneRenderer::kMenuReflectionRow, gt2::MenuVram::kHeight, map.Words().data());
    envPage_ = 9; envClut_ = uint16_t((576 / 16) | (152 << 6)); envColour_ = 0x40;
    envRowBase_ = VkSceneRenderer::kMenuReflectionRow;
}

bool MenuCarView::Use(uint32_t modelId) {
    if (slot_ >= 0 && modelId == model_) return true;
    dish_ = -1; // a new car: 0x8001AC20's default wheel area (0x80061504 + 0x80061308)
    return Load(modelId);
}

bool MenuCarView::Load(uint32_t modelId) {
    // A fresh set of car slots per car (the view shows one car; the slot's vertex range and texture rows are reused).
    assets_ = std::make_unique<SceneAssets>(renderer_, vol_);
    slot_ = -1;
    model_ = modelId;
    try {
        const gt2::CarModel model = gt2::ParseCarModel(vol_.Read("carobj/" + gt2::UnpackCarId(modelId) + ".cdo"));
        if (dish_ >= 0) { // 0x8001AEF8: 0x80061504 defaults with the dish of the wheel, then 0x80061308
            std::array<gt2::WheelAxleDims, 2> dims = gt2::DefaultWheelDims(model);
            dims[0].dish = dims[1].dish = int16_t(dish_);
            static const gt2::WheelTemplates templates = gt2::GeneratedWheelTemplates();
            const gt2::WheelArea area = gt2::GenerateWheelArea(dims, templates);
            slot_ = assets_->UseCar(gt2::UnpackCarId(modelId), std::string(), &area);
        } else {
            slot_ = assets_->UseCar(gt2::UnpackCarId(modelId));
        }
        lift_ = gt2::menu::MenuCarLift(model.wheelRadiusFront, model.wheels[0].y); // 0x80061544
    } catch (const std::exception& e) {
        std::printf("menu car view: %s: %s\n", gt2::UnpackCarId(modelId).c_str(), e.what());
        slot_ = -1;
    }
    return slot_ >= 0;
}

void MenuCarView::SetWheels(const gt2::menu::MenuWheelTexture* wheel, int dish) {
    if (slot_ < 0 || !assets_) return;
    if (dish != dish_) {
        dish_ = dish;
        Load(model_);
        if (slot_ < 0) return;
    }
    if (wheel) assets_->SetCarWheelTexture(slot_, wheel->image, wheel->words, wheel->rows, wheel->clut);
    else assets_->SetCarWheelTexture(slot_, {}, 0, 0, {});
}

bool MenuCarView::UsePair(uint32_t a, uint32_t b) {
    if (slot_ < 0 && assets_ && pairModels_[0] == a && pairModels_[1] == b) return true;
    assets_ = std::make_unique<SceneAssets>(renderer_, vol_);
    slot_ = -1;
    model_ = 0;
    pairModels_ = {a, b};
    pairSlots_ = {-1, -1};
    for (size_t k = 0; k < 2; k++) {
        if (pairModels_[k] == 0) continue;
        try {
            const gt2::CarModel model = gt2::ParseCarModel(vol_.Read("carobj/" + gt2::UnpackCarId(pairModels_[k]) + ".cdo"));
            // the same car twice: a slot of its own (alias) so that each keeps its reflection pass
            pairSlots_[k] = assets_->UseCar(gt2::UnpackCarId(pairModels_[k]), k == 1 && a == b ? "pair1" : std::string());
            pairLifts_[k] = gt2::menu::MenuCarLift(model.wheelRadiusFront, model.wheels[0].y); // 0x80061544
        } catch (const std::exception& e) {
            std::printf("menu car view: %s: %s\n", gt2::UnpackCarId(pairModels_[k]).c_str(), e.what());
            pairSlots_[k] = -1;
        }
    }
    return true;
}

void MenuCarView::AppendPair(std::vector<DrawItem>& items, int which, const gt2::menu::MenuCarProjection& projection, int paint, float windowAspect, bool squarePixels) {
    if (which < 0 || which > 1 || slot_ >= 0) return;
    AppendSlot(items, pairSlots_[size_t(which)], pairLifts_[size_t(which)], projection, paint, windowAspect, squarePixels);
}

void MenuCarView::Append(std::vector<DrawItem>& items, const gt2::menu::MenuCarProjection& projection, int paint, float windowAspect, bool squarePixels) {
    AppendSlot(items, slot_, lift_, projection, paint, windowAspect, squarePixels);
}

void MenuCarView::AppendSlot(std::vector<DrawItem>& items, int slot, int32_t lift, const gt2::menu::MenuCarProjection& projection, int paint, float windowAspect,
                             bool squarePixels) {
    if (slot < 0 || !assets_) return;
    const gt2::menu::MenuCarLinearView L = gt2::menu::MenuCarLinear(projection);
    float asx = 1, asy = 1;
    MenuView::FrameScale(windowAspect, squarePixels, asx, asy);
    const float kW = float(frameWidth_), kH = float(frameHeight_);
    // clip = (asx ((2 / W) (originX a.z + H a.x) - a.z), asy ((2 / H) (originY a.z + H a.y) - a.z), zn, a.z) with
    // a = L (world, 1): the GTE's screen = offset + H * a.xy / a.z in frame pixels, then MenuView's frame -> NDC.
    constexpr float kNear = 0.05f; // reversed Z (vk_scene_renderer.h): z_ndc = kNear / distance
    float viewProj[16] = {};
    for (int c = 0; c < 4; c++) {
        viewProj[c * 4 + 0] = asx * ((2.0f / kW) * (L.originX * L.rows[2][c] + L.H * L.rows[0][c]) - L.rows[2][c]);
        viewProj[c * 4 + 1] = asy * ((2.0f / kH) * (L.originY * L.rows[2][c] + L.H * L.rows[1][c]) - L.rows[2][c]);
        viewProj[c * 4 + 2] = c == 3 ? kNear : 0.0f;
        viewProj[c * 4 + 3] = L.rows[2][c];
    }
    // The car at (0, lift, 0) with no rotation (0x8001A8A4's model matrix), metres.
    float model[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, float(lift) / 65536.0f, 0, 1};
    float mvp[16];
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += viewProj[k * 4 + r] * model[c * 4 + k];
            mvp[c * 4 + r] = s;
        }
    // Reflection pass: rows right / down of the camera (V's columns; 0x80067444 feeds V^T M >> 3 with row 1 negated).
    std::array<std::array<float, 3>, 3> axes{};
    for (int k = 0; k < 3; k++) {
        axes[0][size_t(k)] = float(projection.view[size_t(k)][0]) / 4096.0f;
        axes[1][size_t(k)] = -float(projection.view[size_t(k)][1]) / 4096.0f;
        axes[2][size_t(k)] = float(projection.view[size_t(k)][2]) / 4096.0f;
    }
    // The menus: page 9, CLUT 0x2624 (the captured second-pass primitives), 0x40 (SetReflection for other views).
    if (envColour_ != 0) assets_->UpdateCarReflection(slot, model, axes, envPage_, envClut_, envColour_, 0, envRowBase_);
    const size_t first = items.size();
    assets_->AppendCarItems(items, slot, mvp, uint32_t(std::max(0, paint)), 0);
    if (!shadow_) // the ground shadow is the car's subtractive item (blend mode 2, SceneAssets::AppendCarItems)
        items.erase(std::remove_if(items.begin() + std::ptrdiff_t(first), items.end(), [](const DrawItem& d) { return d.blend == 2; }), items.end());
    // The drawing area of the car's environment (0x8008034C with the viewport rectangle), in window fractions.
    auto toWindowX = [&](float x) { return ((x / kW * 2.0f - 1.0f) * asx + 1.0f) * 0.5f; };
    auto toWindowY = [&](float y) { return ((y / kH * 2.0f - 1.0f) * asy + 1.0f) * 0.5f; };
    for (size_t i = first; i < items.size(); i++) {
        items[i].scissor[0] = toWindowX(float(projection.x0));
        items[i].scissor[1] = toWindowY(float(projection.y0));
        items[i].scissor[2] = toWindowX(float(projection.x0 + projection.w));
        items[i].scissor[3] = toWindowY(float(projection.y0 + projection.h));
    }
}

} // namespace gt2view
