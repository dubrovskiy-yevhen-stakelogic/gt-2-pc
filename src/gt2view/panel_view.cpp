#include "gt2view/panel_view.h"

#include <algorithm>
#include <cstdlib>

#include "gt2view/menu_view.h"

namespace gt2view {

namespace {
constexpr uint32_t kPanelVertices = 6'000;                                 // Build: the first part of the range
constexpr uint32_t kOverlayBase = PanelView::kVertexBase + kPanelVertices; // BuildOverlay: the rest
} // namespace

void PanelView::UploadVram(const gt2::MenuVram& vram) { renderer_.UploadVram(kRowBase, gt2::MenuVram::kHeight, vram.Words().data()); }
void PanelView::UploadVram(const std::vector<uint16_t>& words) { renderer_.UploadVram(kRowBase, 512, words.data()); }

void PanelView::Build(const std::vector<gt2::MenuPrim>& prims, float windowAspect, std::vector<DrawItem>& items) {
    Build(prims, gt2::MenuCanvas::kWidth, gt2::MenuCanvas::kHeight, windowAspect, items);
}

void PanelView::Build(const std::vector<gt2::MenuPrim>& prims, int frameWidth, int frameHeight, float windowAspect, std::vector<DrawItem>& items) {
    std::vector<Quad> quads;
    for (const gt2::MenuPrim& p : prims) {
        if (p.clipX1 >= p.clipX0) continue; // separate draw environments (the menus' 3D car) are not used here
        const uint32_t mode = uint32_t(p.tpage >> 5) & 3;
        Quad q{};
        auto flat = [&](uint32_t c, int k) {
            for (int j = 0; j < 3; j++) q.colour[k][j] = float(((c >> (8 * j)) & 0xFF) >> 3) / 31.0f;
        };
        auto rect = [&](float x0, float y0, float x1, float y1) { // clipped to the drawing area (E3 / E4 = the frame)
            if (x0 > -1e3f) x0 = std::clamp(x0, 0.0f, float(frameWidth)), x1 = std::clamp(x1, 0.0f, float(frameWidth));
            if (y0 > -1e3f) y0 = std::clamp(y0, 0.0f, float(frameHeight)), y1 = std::clamp(y1, 0.0f, float(frameHeight));
            const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
            std::copy(xs, xs + 4, q.x), std::copy(ys, ys + 4, q.y);
        };
        switch (p.kind) {
        case gt2::MenuPrim::kSprite: {
            rect(p.x[0], p.y[0], float(p.x[0] + p.w), float(p.y[0] + p.h));
            const float u0 = p.u, u1 = float(p.u + p.w), v0 = p.v, v1 = float(p.v + p.h);
            const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
            std::copy(us, us + 4, q.u), std::copy(vs, vs + 4, q.v);
            for (int k = 0; k < 4; k++)
                for (int j = 0; j < 3; j++) q.colour[k][j] = float((p.colour[0] >> (8 * j)) & 0xFF) / 255.0f;
            q.page = uint32_t((p.tpage & 0xF) * 64) | ((kRowBase + uint32_t(((p.tpage >> 4) & 1) * 256)) << 16);
            q.clut = uint32_t((p.clut & 0x3F) * 16) | ((kRowBase + uint32_t(p.clut >> 6)) << 16);
            q.flags = kTextured | kIntegerModulate | ((uint32_t(p.tpage >> 7) & 3) << 8);
            if (p.semi) {
                q.stpPass = 1;
                quads.push_back(q);
                q.stpPass = 2;
                q.blend = mode;
            }
            quads.push_back(q);
            continue;
        }
        case gt2::MenuPrim::kTile:
            if (p.x[0] <= 0 && p.y[0] <= 0 && p.x[0] + p.w >= frameWidth && p.y[0] + p.h >= frameHeight) rect(-1e4f, -1e4f, 1e4f, 1e4f); // the frame's clear: the whole window
            else rect(p.x[0], p.y[0], float(p.x[0] + p.w), float(p.y[0] + p.h));
            for (int k = 0; k < 4; k++) flat(p.colour[0], k);
            break;
        case gt2::MenuPrim::kPolyF4:
        case gt2::MenuPrim::kPolyG4:
            for (int k = 0; k < 4; k++) q.x[k] = p.x[k], q.y[k] = p.y[k], flat(p.kind == gt2::MenuPrim::kPolyG4 ? p.colour[k] : p.colour[0], k);
            break;
        case gt2::MenuPrim::kLine: { // a one-pixel band along the line (both end points included)
            const bool vertical = std::abs(p.x[1] - p.x[0]) < std::abs(p.y[1] - p.y[0]);
            rect(float(std::min(p.x[0], p.x[1])), float(std::min(p.y[0], p.y[1])), float(std::max(p.x[0], p.x[1]) + 1), float(std::max(p.y[0], p.y[1]) + 1));
            const uint32_t a = p.colour[0], b = p.gouraud ? p.colour[1] : p.colour[0];
            const bool forward = vertical ? p.y[0] <= p.y[1] : p.x[0] <= p.x[1];
            const uint32_t start = forward ? a : b, end = forward ? b : a;
            flat(start, 0), flat(vertical ? start : end, 1), flat(vertical ? end : start, 2), flat(end, 3);
            break;
        }
        case gt2::MenuPrim::kPolyFT4: { // a textured quad (GPU order), modulated as a sprite
            for (int k = 0; k < 4; k++) {
                q.x[k] = p.x[k], q.y[k] = p.y[k], q.u[k] = p.tu[k], q.v[k] = p.tv[k];
                for (int j = 0; j < 3; j++) q.colour[k][j] = float((p.colour[0] >> (8 * j)) & 0xFF) / 255.0f;
            }
            q.page = uint32_t((p.tpage & 0xF) * 64) | ((kRowBase + uint32_t(((p.tpage >> 4) & 1) * 256)) << 16);
            q.clut = uint32_t((p.clut & 0x3F) * 16) | ((kRowBase + uint32_t(p.clut >> 6)) << 16);
            q.flags = kTextured | kIntegerModulate | ((uint32_t(p.tpage >> 7) & 3) << 8);
            if (p.semi) {
                q.stpPass = 1;
                quads.push_back(q);
                q.stpPass = 2;
                q.blend = mode;
            }
            quads.push_back(q);
            continue;
        }
        }
        if (p.semi) q.blend = mode;
        quads.push_back(q);
    }
    Submit(quads, frameWidth, frameHeight, windowAspect, kVertexBase, kPanelVertices, items);
}

void PanelView::BuildOverlay(const std::vector<gt2::raceui::Gp0Prim>& prims, float windowAspect, std::vector<DrawItem>& items) {
    using gt2::raceui::Gp0Prim;
    std::vector<Quad> quads;
    uint16_t mode = 0; // the draw mode in effect (E1): texture page of sprites, blend mode of every semi primitive
    auto page = [](uint16_t e1) { return uint32_t((e1 & 0xF) * 64) | ((kRowBase + uint32_t(((e1 >> 4) & 1) * 256)) << 16); };
    auto clut = [](uint16_t c) { return uint32_t((c & 0x3F) * 16) | ((kRowBase + uint32_t(c >> 6)) << 16); };
    for (const Gp0Prim& p : prims) {
        Quad q{};
        const bool textured = p.kind == Gp0Prim::kSprite || p.kind == Gp0Prim::kPolyFT4;
        for (int k = 0; k < 3; k++) {
            const uint32_t c = (p.colour >> (8 * k)) & 0xFF;
            for (int v = 0; v < 4; v++) q.colour[v][k] = textured ? float(c) / 255.0f : float(c >> 3) / 31.0f;
        }
        switch (p.kind) {
        case Gp0Prim::kMode: mode = p.e1; continue;
        case Gp0Prim::kSprite:
        case Gp0Prim::kTile: {
            float x0 = p.x[0], x1 = float(p.x[0] + p.w);
            if (p.kind == Gp0Prim::kTile && p.x[0] <= 0 && p.x[0] + p.w >= 320) x0 = -1e4f, x1 = 1e4f; // the whole window
            const float y0 = p.y[0], y1 = float(p.y[0] + p.h);
            const float xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
            const float us[4] = {float(p.u[0]), float(p.u[0] + p.w), float(p.u[0]), float(p.u[0] + p.w)};
            const float vs[4] = {float(p.v[0]), float(p.v[0]), float(p.v[0] + p.h), float(p.v[0] + p.h)};
            std::copy(xs, xs + 4, q.x), std::copy(ys, ys + 4, q.y), std::copy(us, us + 4, q.u), std::copy(vs, vs + 4, q.v);
            if (textured) q.page = page(mode), q.clut = clut(p.clut), q.flags = kTextured | kIntegerModulate | ((uint32_t(mode >> 7) & 3) << 8);
            break;
        }
        case Gp0Prim::kPolyF4:
        case Gp0Prim::kPolyFT4:
            for (int k = 0; k < 4; k++) q.x[k] = p.x[k], q.y[k] = p.y[k], q.u[k] = p.u[k], q.v[k] = p.v[k];
            if (textured) {
                mode = uint16_t((mode & ~0x9FFu) | (p.e1 & 0x9FF));
                q.page = page(mode), q.clut = clut(p.clut), q.flags = kTextured | kIntegerModulate | ((uint32_t(mode >> 7) & 3) << 8);
            }
            break;
        }
        if (p.semi) {
            if (textured) {
                q.stpPass = 1;
                quads.push_back(q);
                q.stpPass = 2;
            }
            q.blend = uint32_t(mode >> 5) & 3;
        }
        quads.push_back(q);
    }
    Submit(quads, 320, 240, windowAspect, kOverlayBase, kVertexLimit - kPanelVertices, items);
}

void PanelView::Submit(std::vector<Quad>& quads, int frameWidth, int frameHeight, float windowAspect, uint32_t base, uint32_t limit, std::vector<DrawItem>& items) {
    if (quads.size() * 6 > limit) quads.resize(limit / 6);
    if (quads.empty()) return;
    const float kW = float(frameWidth), kH = float(frameHeight);
    float sx = 1, sy = 1;
    MenuView::FrameScale(windowAspect, false, sx, sy);
    std::vector<SceneVertex> vertices;
    vertices.reserve(quads.size() * 6);
    for (const Quad& q : quads)
        for (int k : {0, 1, 2, 1, 2, 3}) { // GPU quad order: triangles v0 v1 v2 and v1 v2 v3
            SceneVertex v{};
            const bool wide = q.x[k] <= -1e3f || q.x[k] >= 1e3f; // a full-frame tile: the whole window
            v.pos[0] = q.x[k] <= -1e3f ? -1.0f : q.x[k] >= 1e3f ? 1.0f : (q.x[k] / kW * 2.0f - 1.0f) * sx;
            v.pos[1] = (q.y[k] / kH * 2.0f - 1.0f) * sy;
            if (wide) v.pos[1] = q.y[k] <= 0 ? -1.0f : q.y[k] >= kH ? 1.0f : v.pos[1]; // (y -1e4 / 1e4 of a clear: -1 / 1)
            v.pos[2] = 1.0f; // reversed Z: in front of the scene; later quads cover earlier ones
            v.texel[0] = q.u[k];
            v.texel[1] = q.v[k];
            for (int j = 0; j < 3; j++) v.color[j] = q.colour[k][j];
            v.page = q.page;
            v.clut = q.clut;
            v.flags = q.flags;
            vertices.push_back(v);
        }
    renderer_.SetVertices(base, vertices);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    size_t first = 0;
    while (first < quads.size()) {
        size_t last = first;
        while (last + 1 < quads.size() && quads[last + 1].blend == quads[first].blend && quads[last + 1].stpPass == quads[first].stpPass) last++;
        DrawItem item;
        item.firstVertex = base + uint32_t(first * 6);
        item.vertexCount = uint32_t((last - first + 1) * 6);
        item.blend = quads[first].blend;
        item.stpPass = quads[first].stpPass;
        std::copy(identity, identity + 16, item.mvp);
        items.push_back(item);
        first = last + 1;
    }
}

} // namespace gt2view
