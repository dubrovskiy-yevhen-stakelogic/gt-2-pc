#include "gt2view/title_view.h"
#include "gt2view/hd_picture.h"

#include <algorithm>

namespace gt2view {

void TitleView::UploadVram(const gt2::MenuVram& vram) {
    renderer_.UploadVram(rowBase_, gt2::MenuVram::kHeight, vram.Words().data());
    if (hdGeneration_ != gt2::hd::Generation()) {
        hdGeneration_ = gt2::hd::Generation();
        hdSize_ = UploadHdPicture(renderer_, "title.png", 352, 480);
    }
}

void TitleView::Emit(const gt2::MenuPrim& p, int frameWidth) {
    const uint32_t mode = uint32_t(p.tpage >> 5) & 3;
    auto rect = [&](float x0, float y0, float x1, float y1, uint32_t colour, bool quantise) {
        Quad q{};
        const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
        for (int k = 0; k < 4; k++) {
            q.x[k] = xs[k];
            q.y[k] = ys[k];
            for (int c = 0; c < 3; c++) {
                const uint32_t v = (colour >> (8 * c)) & 0xFF;
                q.colour[k][c] = quantise ? float(v >> 3) / 31.0f : float(v) / 255.0f;
            }
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
        if (hdSize_ && p.clut == 0x7fd8 && !p.semi && p.u == 0 && p.v == 0 &&
            (p.tpage == 0x86 || p.tpage == 0x96 || p.tpage == 0x88 || p.tpage == 0x98) &&
            p.x[0] == ((p.tpage & 8) ? 256 : 0) && p.y[0] == ((p.tpage & 16) ? 256 : 0)) {
            for (int k=0;k<4;++k) {
                q.u[k] = q.x[k] * float(hdSize_ & 65535) / 352.f;
                q.v[k] = q.y[k] * float(hdSize_ >> 16) / 480.f;
                for (int c=0;c<3;++c) q.colour[k][c] *= 255.f/128.f;
            }
            q.page=VkSceneRenderer::kHdMenuTexelBase; q.clut=hdSize_; q.flags=kTextured|kExternalTexture;
        }

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
        if (!p.semi) { // exact pixels: quantised like the VRAM holds them
            quads_.push_back(rect(p.x[0], p.y[0], float(p.x[0] + p.w), float(p.y[0] + p.h), p.colour[0], true));
            return;
        }
        Quad q = rect(p.x[0], p.y[0], float(p.x[0] + p.w), float(p.y[0] + p.h), p.colour[0], true);
        q.blend = mode;
        quads_.push_back(q);
        return;
    }
    case gt2::MenuPrim::kPolyF4:
    case gt2::MenuPrim::kPolyG4: {
        // Polygons as two triangles with vertex colours (the GPU's gouraud interpolation; the per-pixel path of the
        // lines would need one quad per pixel, too many for the full-width header gradients of these screens).
        Quad q{};
        q.triangles = true;
        for (int k = 0; k < 4; k++) {
            q.x[k] = p.x[k];
            q.y[k] = p.y[k];
            const uint32_t c = p.kind == gt2::MenuPrim::kPolyG4 ? p.colour[k] : p.colour[0];
            for (int j = 0; j < 3; j++) q.colour[k][j] = float((c >> (8 * j)) & 0xFF) / 255.0f;
        }
        if (p.semi) q.blend = mode;
        quads_.push_back(q);
        return;
    }
    case gt2::MenuPrim::kLine: EmitRasterised(p, frameWidth); return;
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

void TitleView::EmitRasterised(const gt2::MenuPrim& p, int frameWidth) {
    const int n = p.kind == gt2::MenuPrim::kLine ? 2 : 4;
    int x0 = p.x[0], x1 = p.x[0], y0 = p.y[0], y1 = p.y[0];
    for (int k = 1; k < n; k++) {
        x0 = std::min(x0, int(p.x[k])), x1 = std::max(x1, int(p.x[k]));
        y0 = std::min(y0, int(p.y[k])), y1 = std::max(y1, int(p.y[k]));
    }
    x0 = std::max(x0, 0), y0 = std::max(y0, 0);
    x1 = std::min(x1, std::min(frameWidth, int(gt2::MenuCanvas::kWidth)) - 1), y1 = std::min(y1, gt2::MenuCanvas::kHeight - 1);
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
            Quad q{};
            const float xs[4] = {float(x), float(x + 1), float(x + 1), float(x)}, ys[4] = {float(y), float(y), float(y + 1), float(y + 1)};
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

void TitleView::Build(const std::vector<gt2::MenuPrim>& prims, int frameWidth, float windowAspect, std::vector<DrawItem>& items, bool squarePixels) {
    if(hdGeneration_ != gt2::hd::Generation()) {
        hdGeneration_=gt2::hd::Generation(); hdSize_=UploadHdPicture(renderer_,"title.png",352,480);
    }
    quads_.clear();
    for (const gt2::MenuPrim& p : prims) Emit(p, frameWidth);
    if (quads_.size() * 6 > kVertexLimit) quads_.resize(kVertexLimit / 6);
    const float kW = float(frameWidth), kH = float(gt2::MenuCanvas::kHeight);
    const float frameAspect = squarePixels ? kW / kH : 4.0f / 3.0f;
    const float sx = std::min(1.0f, frameAspect / windowAspect), sy = std::min(1.0f, windowAspect / frameAspect);
    std::vector<SceneVertex> vertices;
    vertices.reserve(quads_.size() * 6);
    for (const Quad& q : quads_) {
        static constexpr int kRect[6] = {0, 1, 2, 0, 2, 3}, kGpu[6] = {0, 1, 2, 1, 2, 3};
        for (int k : q.triangles ? kGpu : kRect) {
            SceneVertex v{};
            v.pos[0] = (q.x[k] / kW * 2.0f - 1.0f) * sx;
            v.pos[1] = (q.y[k] / kH * 2.0f - 1.0f) * sy;
            v.pos[2] = 1.0f;
            v.texel[0] = q.u[k];
            v.texel[1] = q.v[k];
            for (int j = 0; j < 3; j++) v.color[j] = q.colour[k][j];
            v.page = q.page;
            v.clut = q.clut;
            v.flags = q.flags;
            vertices.push_back(v);
        }
    }
    renderer_.ApplyHdUi(vertices);
    renderer_.SetVertices(kVertexBase, vertices);
    const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    size_t first = 0;
    while (first < quads_.size()) {
        size_t last = first;
        while (last + 1 < quads_.size() && quads_[last + 1].blend == quads_[first].blend && quads_[last + 1].stpPass == quads_[first].stpPass) last++;
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

} // namespace gt2view
