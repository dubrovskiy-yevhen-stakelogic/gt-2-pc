#include "machine/gpu.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace gt2 {
namespace {

int SignExtend11(uint32_t v) { return int(int32_t(v << 21) >> 21); }
int Expand5(uint16_t c, int shift) { return ((c >> shift) & 31) << 3; }

} // namespace

// ---------------------------------------------------------------- ports

size_t Gpu::CommandLength(uint32_t first) const {
    const uint32_t cmd = first >> 24;
    switch (cmd >> 5) {
    case 0: return cmd == 0x02 ? 3 : 1;
    case 1: {
        const size_t n = (cmd & 0x08) ? 4 : 3;
        return 1 + n + ((cmd & 0x04) ? n : 0) + ((cmd & 0x10) ? n - 1 : 0);
    }
    case 2: return (cmd & 0x08) ? SIZE_MAX : ((cmd & 0x10) ? 4 : 3);
    case 3: return 2 + ((cmd & 0x04) ? 1 : 0) + (((cmd >> 3) & 3) == 0 ? 1 : 0);
    case 4: return 4;
    case 5: case 6: return 3;
    default: return 1;
    }
}

void Gpu::WriteGp0(uint32_t word) {
    if (upload_.remaining > 0) { // image data of a CPU -> VRAM transfer: two pixels per word
        for (int half = 0; half < 2 && upload_.remaining > 0; half++, upload_.remaining--, upload_.index++) {
            const int x = (upload_.x + int(upload_.index % upload_.w)) & (kVramWidth - 1);
            const int y = (upload_.y + int(upload_.index / upload_.w)) & (kVramHeight - 1);
            vram_[size_t(y) * kVramWidth + size_t(x)] = uint16_t(word >> (half * 16));
        }
        return;
    }
    if (command_.empty()) {
        expected_ = CommandLength(word);
        polyline_ = expected_ == SIZE_MAX;
    }
    command_.push_back(word);
    const bool terminator = polyline_ && command_.size() >= 3 && (word & 0xF000F000u) == 0x50005000u;
    if (terminator || (!polyline_ && command_.size() >= expected_) || command_.size() > 4096) {
        const uint32_t group = command_[0] >> 29;
        if (inList_ && group != 4 && group != 5 && group != 6) { // transfers must run now: their data words follow
            deferred_.push_back({command_, IsProjectedPolygon(command_)});
        } else {
            ExecuteCommand();
        }
        command_.clear();
    }
}

bool Gpu::IsProjectedPolygon(const std::vector<uint32_t>& words) const {
    const uint32_t cmd = words[0] >> 24;
    if ((cmd >> 5) != 1 || !isProjected) return false;
    const bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04;
    size_t idx = 1;
    for (size_t i = 0; i < (quad ? 4u : 3u); i++) {
        if (i > 0 && gouraud) idx++;
        if (idx >= words.size() || !isProjected(words[idx] & 0x07FF07FFu)) return false;
        idx += textured ? 2 : 1;
    }
    return true;
}

void Gpu::EndList() {
    if (!inList_) return;
    inList_ = false;
    size_t cutoff = 0; // number of leading commands that belong to the 3D layer
    for (size_t i = 0; i < deferred_.size(); i++)
        if (deferred_[i].projectedPolygon) cutoff = i + 1;
    std::vector<uint32_t> saved = std::move(command_);
    for (size_t i = 0; i < deferred_.size(); i++) {
        command_ = std::move(deferred_[i].words);
        polyline_ = CommandLength(command_[0]) == SIZE_MAX;
        skipDraw_ = i < cutoff; // state commands (E1-E6) still execute
        ExecuteCommand();
    }
    skipDraw_ = false;
    deferred_.clear();
    command_ = std::move(saved);
}

void Gpu::WriteGp1(uint32_t word) {
    switch (word >> 24) {
    case 0x00:
        command_.clear();
        upload_ = {};
        displayEnabled_ = false;
        displayMode_ = 0;
        drawMode_ = 0;
        offsetX_ = offsetY_ = 0;
        areaLeft_ = areaTop_ = 0;
        areaRight_ = kVramWidth - 1;
        areaBottom_ = kVramHeight - 1;
        break;
    case 0x01: command_.clear(); upload_ = {}; break;
    case 0x03: displayEnabled_ = (word & 1) == 0; break;
    case 0x04: dmaDirection_ = word & 3; break;
    case 0x05: {
        const int x = int(word & 0x3FF), y = int((word >> 10) & 0x1FF);
        const bool changed = x != displayX_ || y != displayY_;
        displayX_ = x;
        displayY_ = y;
        if (changed && onDisplayFlip) onDisplayFlip();
        if (changed) backgroundSeen_ = false;
        break;
    }
    case 0x08: displayMode_ = word & 0xFF; break;
    case 0x10: readback_.push_back((word & 0xF) == 7 ? 2u : 0u); break; // GPU info: version 2
    default: break; // display ranges (06/07) do not matter for a VRAM-side picture
    }
}

uint32_t Gpu::ReadStatus(bool oddField) const {
    uint32_t s = 0x1C000000u | (dmaDirection_ << 29) | (drawMode_ & 0x7FFu);
    s |= (displayMode_ & 3) << 17;
    if (displayMode_ & 0x40) s |= 1u << 16;
    if (displayMode_ & 0x04) s |= 1u << 19;
    if (displayMode_ & 0x08) s |= 1u << 20;
    if (displayMode_ & 0x10) s |= 1u << 21;
    if (displayMode_ & 0x20) s |= 1u << 22;
    if (!displayEnabled_) s |= 1u << 23;
    if (forceMask_) s |= 1u << 11;
    if (checkMask_) s |= 1u << 12;
    if (oddField) s |= (1u << 31) | (1u << 13);
    return s;
}

uint32_t Gpu::ReadData() {
    if (readback_.empty()) return 0;
    uint32_t v = readback_.front();
    readback_.pop_front();
    return v;
}

int Gpu::DisplayWidth() const {
    static constexpr int kWidths[4] = {256, 320, 512, 640};
    return (displayMode_ & 0x40) ? 368 : kWidths[displayMode_ & 3];
}

std::vector<uint8_t> Gpu::DisplayRgba(int& width, int& height) const {
    width = DisplayWidth();
    height = DisplayHeight();
    std::vector<uint8_t> rgba(size_t(width) * height * 4);
    if (displayMode_ & 0x10) { // 24-bit display (GP1(08h) bit 4, the movies): 3 bytes per pixel, R G B, from the display start
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
                for (int k = 0; k < 3; k++) {
                    const int byte = displayX_ * 2 + x * 3 + k;
                    const uint16_t hw = vram_[size_t((displayY_ + y) & (kVramHeight - 1)) * kVramWidth + size_t((byte >> 1) & (kVramWidth - 1))];
                    p[k] = uint8_t(hw >> ((byte & 1) * 8));
                }
                p[3] = 255;
            }
        return rgba;
    }
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const uint16_t c = vram_[size_t((displayY_ + y) & (kVramHeight - 1)) * kVramWidth + size_t((displayX_ + x) & (kVramWidth - 1))];
            uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
            auto expand = [](int v5) { return uint8_t((v5 << 3) | (v5 >> 2)); };
            p[0] = expand(c & 31);
            p[1] = expand((c >> 5) & 31);
            p[2] = expand((c >> 10) & 31);
            p[3] = 255;
        }
    return rgba;
}

std::vector<uint8_t> Gpu::DisplayRgbaTouched(int& width, int& height) const {
    std::vector<uint8_t> rgba = DisplayRgba(width, height);
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++)
            rgba[(size_t(y) * width + x) * 4 + 3] =
                touched_[size_t((displayY_ + y) & (kVramHeight - 1)) * kVramWidth + size_t((displayX_ + x) & (kVramWidth - 1))] ? 255 : 0;
    return rgba;
}

// ---------------------------------------------------------------- commands

void Gpu::ExecuteCommand() {
    const uint32_t first = command_[0], cmd = first >> 24;
    auto color = [](uint32_t w, Vertex& v) { v.r = int(w & 0xFF); v.g = int((w >> 8) & 0xFF); v.b = int((w >> 16) & 0xFF); };
    auto position = [&](uint32_t w, Vertex& v) { v.x = SignExtend11(w & 0x7FF) + offsetX_; v.y = SignExtend11((w >> 16) & 0x7FF) + offsetY_; };

    switch (cmd >> 5) {
    case 0:
        if (cmd == 0x02 && !isProjected && !skipDraw_) { // HUD-only mode: background clears belong to the 3D layer
            FillRect(first, int(command_[1] & 0xFFFF), int(command_[1] >> 16), int(command_[2] & 0xFFFF), int(command_[2] >> 16));
            primitivesDrawn++;
        }
        break;
    case 1: { // polygons
        const bool gouraud = cmd & 0x10, quad = cmd & 0x08, textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
        Vertex v[4] = {};
        size_t idx = 1;
        bool allProjected = true;
        for (size_t i = 0; i < (quad ? 4u : 3u); i++) {
            if (i == 0 || !gouraud) color(first, v[i]);
            else color(command_[idx++], v[i]);
            const uint32_t xy = command_[idx++];
            position(xy, v[i]);
            if (isProjected && !isProjected(xy & 0x07FF07FFu)) allProjected = false;
            if (textured) {
                const uint32_t w = command_[idx++];
                v[i].u = int(w & 0xFF);
                v[i].v = int((w >> 8) & 0xFF);
                if (i == 0) clut_ = uint16_t(w >> 16);
                if (i == 1) drawMode_ = uint16_t((drawMode_ & ~0x9FFu) | ((w >> 16) & 0x9FF));
            }
        }
        if (skipDraw_ && !textured && !backgroundSeen_ &&
            std::max({v[0].x, v[1].x, v[2].x}) - std::min({v[0].x, v[1].x, v[2].x}) >= 256) { // the sky band comes first
            backgroundColor = first & 0xFFFFFF;
            backgroundSeen_ = true;
        }
        if (!skip3dRaster && !skipDraw_ && !(isProjected && allProjected)) {
            DrawTriangle(v[0], v[1], v[2], textured, gouraud, semi, raw);
            if (quad) DrawTriangle(v[1], v[2], v[3], textured, gouraud, semi, raw);
        }
        primitivesDrawn++;
        break;
    }
    case 2: { // lines
        const bool gouraud = cmd & 0x10, semi = cmd & 0x02;
        std::vector<Vertex> points;
        for (size_t idx = 0; idx < command_.size();) {
            if (polyline_ && idx > 0 && (command_[idx] & 0xF000F000u) == 0x50005000u) break;
            Vertex p{};
            if (idx == 0) color(first, p), idx++;
            else if (gouraud) color(command_[idx++], p);
            else color(first, p);
            if (idx >= command_.size()) break;
            position(command_[idx++], p);
            points.push_back(p);
        }
        if (skipDraw_) break;
        for (size_t i = 0; i + 1 < points.size(); i++) DrawLine(points[i], points[i + 1], semi);
        primitivesDrawn++;
        break;
    }
    case 3: { // rectangles / sprites
        const bool textured = cmd & 0x04, semi = cmd & 0x02, raw = cmd & 0x01;
        Vertex p{};
        position(command_[1], p);
        size_t idx = 2;
        int u = 0, v = 0;
        if (textured) {
            u = int(command_[idx] & 0xFF);
            v = int((command_[idx] >> 8) & 0xFF);
            clut_ = uint16_t(command_[idx] >> 16);
            idx++;
        }
        int w = 1, h = 1;
        switch ((cmd >> 3) & 3) {
        case 0: w = int(command_[idx] & 0x3FF); h = int((command_[idx] >> 16) & 0x1FF); break;
        case 2: w = h = 8; break;
        case 3: w = h = 16; break;
        default: break;
        }
        if (skipDraw_) {
            if (!textured && w >= 256 && h >= 48 && !backgroundSeen_) { backgroundColor = first & 0xFFFFFF; backgroundSeen_ = true; }
            break;
        }
        DrawRectangle(p.x, p.y, w, h, u, v, first, textured, semi, raw);
        primitivesDrawn++;
        break;
    }
    case 4: { // VRAM -> VRAM
        const int sx = int(command_[1] & 0x3FF), sy = int((command_[1] >> 16) & 0x1FF);
        const int dx = int(command_[2] & 0x3FF), dy = int((command_[2] >> 16) & 0x1FF);
        const int w = int(((command_[3] & 0xFFFF) - 1) & 0x3FF) + 1, h = int(((command_[3] >> 16) - 1) & 0x1FF) + 1;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                vram_[size_t((dy + y) & 511) * kVramWidth + size_t((dx + x) & 1023)] =
                    vram_[size_t((sy + y) & 511) * kVramWidth + size_t((sx + x) & 1023)];
        break;
    }
    case 5: // CPU -> VRAM
        upload_.x = int(command_[1] & 0x3FF);
        upload_.y = int((command_[1] >> 16) & 0x1FF);
        upload_.w = int(((command_[2] & 0xFFFF) - 1) & 0x3FF) + 1;
        upload_.h = int(((command_[2] >> 16) - 1) & 0x1FF) + 1;
        upload_.remaining = int64_t(upload_.w) * upload_.h;
        upload_.index = 0;
        break;
    case 6: { // VRAM -> CPU
        const int x0 = int(command_[1] & 0x3FF), y0 = int((command_[1] >> 16) & 0x1FF);
        const int w = int(((command_[2] & 0xFFFF) - 1) & 0x3FF) + 1, h = int(((command_[2] >> 16) - 1) & 0x1FF) + 1;
        uint32_t word = 0;
        int n = 0;
        for (int i = 0; i < w * h; i++) {
            const uint16_t px = vram_[size_t((y0 + i / w) & 511) * kVramWidth + size_t((x0 + i % w) & 1023)];
            word |= uint32_t(px) << (n * 16);
            if (++n == 2) { readback_.push_back(word); word = 0; n = 0; }
        }
        if (n) readback_.push_back(word);
        break;
    }
    default: // environment
        switch (cmd) {
        case 0xE1: drawMode_ = uint16_t(first & 0x7FF); break;
        case 0xE2:
            texWindowMaskX_ = int(first & 0x1F) * 8;
            texWindowMaskY_ = int((first >> 5) & 0x1F) * 8;
            texWindowOffX_ = int((first >> 10) & 0x1F) * 8;
            texWindowOffY_ = int((first >> 15) & 0x1F) * 8;
            break;
        case 0xE3: areaLeft_ = int(first & 0x3FF); areaTop_ = int((first >> 10) & 0x1FF); break;
        case 0xE4: areaRight_ = int(first & 0x3FF); areaBottom_ = int((first >> 10) & 0x1FF); break;
        case 0xE5: offsetX_ = SignExtend11(first & 0x7FF); offsetY_ = SignExtend11((first >> 11) & 0x7FF); break;
        case 0xE6: forceMask_ = first & 1; checkMask_ = first & 2; break;
        default: break;
        }
        break;
    }
}

// ---------------------------------------------------------------- rasteriser

void Gpu::FillRect(uint32_t colorWord, int x, int y, int w, int h) {
    x &= 0x3F0;
    y &= 0x1FF;
    w = ((w & 0x3FF) + 0xF) & ~0xF;
    h &= 0x1FF;
    const uint16_t c = uint16_t(((colorWord & 0xFF) >> 3) | (((colorWord >> 8) & 0xFF) >> 3) << 5 | (((colorWord >> 16) & 0xFF) >> 3) << 10);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) vram_[size_t((y + yy) & 511) * kVramWidth + size_t((x + xx) & 1023)] = c;
}

uint16_t Gpu::Texel(int u, int v) const {
    u = (u & ~texWindowMaskX_) | (texWindowOffX_ & texWindowMaskX_);
    v = (v & ~texWindowMaskY_) | (texWindowOffY_ & texWindowMaskY_);
    u &= 0xFF;
    v &= 0xFF;
    const int pageX = (drawMode_ & 0xF) * 64, pageY = ((drawMode_ >> 4) & 1) * 256;
    const int clutX = (clut_ & 0x3F) * 16, clutY = (clut_ >> 6) & 0x1FF;
    auto at = [&](int x, int y) { return vram_[size_t(y & 511) * kVramWidth + size_t(x & 1023)]; };
    switch ((drawMode_ >> 7) & 3) {
    case 0: return at(clutX + ((at(pageX + u / 4, pageY + v) >> ((u & 3) * 4)) & 0xF), clutY);
    case 1: return at(clutX + ((at(pageX + u / 2, pageY + v) >> ((u & 1) * 8)) & 0xFF), clutY);
    default: return at(pageX + u, pageY + v);
    }
}

void Gpu::PlotPixel(int x, int y, int r, int g, int b, bool semi, bool setMask) {
    if (x < areaLeft_ || x > areaRight_ || y < areaTop_ || y > areaBottom_ || x < 0 || y < 0 || x >= kVramWidth || y >= kVramHeight) return;
    uint16_t& dst = vram_[size_t(y) * kVramWidth + size_t(x)];
    if (checkMask_ && (dst & 0x8000)) return;
    if (semi) {
        const int br = Expand5(dst, 0), bg = Expand5(dst, 5), bb = Expand5(dst, 10);
        switch ((drawMode_ >> 5) & 3) {
        case 0: r = (br + r) / 2; g = (bg + g) / 2; b = (bb + b) / 2; break;
        case 1: r = br + r; g = bg + g; b = bb + b; break;
        case 2: r = br - r; g = bg - g; b = bb - b; break;
        default: r = br + r / 4; g = bg + g / 4; b = bb + b / 4; break;
        }
    }
    r = std::clamp(r, 0, 255);
    g = std::clamp(g, 0, 255);
    b = std::clamp(b, 0, 255);
    dst = uint16_t((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | ((setMask || forceMask_) ? 0x8000 : 0));
    touched_[size_t(y) * kVramWidth + size_t(x)] = 1;
}

void Gpu::DrawTriangle(const Vertex& v0, const Vertex& v1in, const Vertex& v2in, bool textured, bool gouraud, bool semi, bool raw) {
    Vertex v1 = v1in, v2 = v2in;
    int64_t area = int64_t(v1.x - v0.x) * (v2.y - v0.y) - int64_t(v1.y - v0.y) * (v2.x - v0.x);
    if (area == 0) return;
    if (area < 0) { std::swap(v1, v2); area = -area; }

    int minX = std::min({v0.x, v1.x, v2.x}), maxX = std::max({v0.x, v1.x, v2.x});
    int minY = std::min({v0.y, v1.y, v2.y}), maxY = std::max({v0.y, v1.y, v2.y});
    if (maxX - minX > 1023 || maxY - minY > 511) return; // the hardware rejects oversized polygons
    minX = std::max(minX, areaLeft_);
    maxX = std::min(maxX, areaRight_);
    minY = std::max(minY, areaTop_);
    maxY = std::min(maxY, areaBottom_);

    auto edge = [](const Vertex& a, const Vertex& b, int x, int y) { return int64_t(b.x - a.x) * (y - a.y) - int64_t(b.y - a.y) * (x - a.x); };
    auto bias = [](const Vertex& a, const Vertex& b) { // top-left fill rule
        const int dx = b.x - a.x, dy = b.y - a.y;
        return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1;
    };
    const int b0 = bias(v1, v2), b1 = bias(v2, v0), b2 = bias(v0, v1);
    const double inv = 1.0 / double(area);

    for (int y = minY; y <= maxY; y++)
        for (int x = minX; x <= maxX; x++) {
            const int64_t w0 = edge(v1, v2, x, y), w1 = edge(v2, v0, x, y), w2 = edge(v0, v1, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            const double l0 = w0 * inv, l1 = w1 * inv, l2 = w2 * inv;
            int r = v0.r, g = v0.g, b = v0.b;
            if (gouraud) {
                r = int(l0 * v0.r + l1 * v1.r + l2 * v2.r);
                g = int(l0 * v0.g + l1 * v1.g + l2 * v2.g);
                b = int(l0 * v0.b + l1 * v1.b + l2 * v2.b);
            }
            if (!textured) { PlotPixel(x, y, r, g, b, semi, false); continue; }
            const uint16_t t = Texel(int(l0 * v0.u + l1 * v1.u + l2 * v2.u + 0.5), int(l0 * v0.v + l1 * v1.v + l2 * v2.v + 0.5));
            if (t == 0) continue;
            int tr = Expand5(t, 0), tg = Expand5(t, 5), tb = Expand5(t, 10);
            if (!raw) { tr = tr * r / 128; tg = tg * g / 128; tb = tb * b / 128; }
            PlotPixel(x, y, tr, tg, tb, semi && (t & 0x8000), (t & 0x8000) != 0);
        }
}

void Gpu::DrawRectangle(int x0, int y0, int w, int h, int u0, int v0, uint32_t colorWord, bool textured, bool semi, bool raw) {
    const int r = int(colorWord & 0xFF), g = int((colorWord >> 8) & 0xFF), b = int((colorWord >> 16) & 0xFF);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if (!textured) { PlotPixel(x0 + x, y0 + y, r, g, b, semi, false); continue; }
            const uint16_t t = Texel(u0 + x, v0 + y);
            if (t == 0) continue;
            int tr = Expand5(t, 0), tg = Expand5(t, 5), tb = Expand5(t, 10);
            if (!raw) { tr = tr * r / 128; tg = tg * g / 128; tb = tb * b / 128; }
            PlotPixel(x0 + x, y0 + y, tr, tg, tb, semi && (t & 0x8000), (t & 0x8000) != 0);
        }
}

void Gpu::DrawLine(Vertex a, Vertex b, bool semi) {
    const int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y), steps = std::max(dx, dy);
    if (dx > 1023 || dy > 511) return;
    for (int i = 0; i <= steps; i++) {
        const double t = steps ? double(i) / steps : 0.0;
        PlotPixel(int(std::lround(a.x + (b.x - a.x) * t)), int(std::lround(a.y + (b.y - a.y) * t)), int(a.r + (b.r - a.r) * t),
                  int(a.g + (b.g - a.g) * t), int(a.b + (b.b - a.b) * t), semi, false);
    }
}

} // namespace gt2
