#include "gt2formats/gt_menu_images.h"
#include "gt2formats/title_assets.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "gt2vfs/gtfs.h"
#include "gt2vfs/inflate.h"

namespace gt2 {
namespace {

uint16_t U16(std::span<const uint8_t> b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("menu picture: read beyond the end");
    return uint16_t(b[o] | (b[o + 1] << 8));
}
uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(U16(b, o)) | (uint32_t(U16(b, o + 2)) << 16); }

// Overlay addresses (GT2.OVL member 4).
constexpr uint32_t kFontAGlyphs = 0x800513BCu, kFontAMetrics = 0x80052100u, kFontACount = 0x10B;
constexpr uint32_t kFontBGlyphs = 0x8005129Cu, kFontBMetrics = 0x80052040u, kFontBCount = 0x18;
constexpr uint32_t kMedalSprites = 0x80050978u;    // + (4 - result) * 12 (0x8001B9AC bit 19, licence tests)
constexpr uint32_t kLicenceLevelSprites = 0x800509B4u; // + level * 12 (type 0xCC)
constexpr uint32_t kLicenceNeedSprites = 0x800509FCu;  // + (requirement + 1) * 12 (bit 20)
constexpr uint32_t kBadgeSprite = 0x80050A50u;     // type 0 bit 18
constexpr uint32_t kTrophySprite = 0x80050A5Cu;    // bit 23, result 1
constexpr uint32_t kCursorFree = 0x80050924u, kCursorOnItem = 0x80050930u; // 0x8001EA24
constexpr uint32_t kGtStrings = 0x800242A8u;       // gzip data-gt.txd (0x80020C50)
constexpr uint32_t kGtStringsRam = 0x801C30C0u, kGtStringsSize = 0x88;
constexpr int kCursorStartX = 0x100, kCursorStartY = 0xFC; // 0x8001E22C
constexpr uint32_t kTextColour = 0x6E6E6E;

} // namespace

// ---------------------------------------------------------------- text

std::u16string MenuTextWriter::Widen(const std::string& s) {
    std::u16string w;
    for (char c : s) w.push_back(char16_t(uint8_t(c)));
    return w;
}

std::string MenuThousands(uint32_t v) {
    const std::string digits = std::to_string(v);
    std::string out;
    int lead = int(digits.size() % 3);
    if (lead == 0) lead = 3;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i != 0 && (int(i) - lead) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

int MenuTextWriter::Index(const MenuFont& f, char16_t c, bool eightBit, bool second) {
    if (second) return f.map[size_t(c & 0xFF)];                             // 0x800B9108[c & 0xFF]
    if (eightBit) return f.Lookup(uint32_t(int32_t(int8_t(uint8_t(c))))); // 0x8001F2C8((short)(char)c)
    return f.Lookup(c);
}

int MenuTextWriter::Draw(const std::u16string& text, bool eightBit, bool secondFont, int x, int y, uint32_t colour) {
    const MenuFont& f = secondFont ? fonts_.b : fonts_.a;
    int width = 0;
    for (char16_t c : text) {
        const MenuFont::Glyph& gl = f.glyphs[size_t(Index(f, c, eightBit, secondFont))];
        MenuPrim p;
        p.kind = MenuPrim::kSprite;
        p.x[0] = int16_t(x + gl.dx);
        p.y[0] = int16_t(y + gl.dy);
        p.w = int16_t(gl.w);
        p.h = int16_t(gl.h);
        p.u = gl.u;
        p.v = gl.v;
        p.clut = gl.clut;
        p.tpage = uint16_t(gl.tpage | 0x40); // 0x8007DA44(ot, tpage | 0x40)
        p.colour[0] = colour & 0xFFFFFF;
        p.semi = (colour & 0x02000000u) != 0; // 0x80081478: command 0x64 ^ colour -> 0x66
        out_.push_back(p);
        x += gl.advance;
        width += gl.advance;
    }
    return width;
}

int MenuTextWriter::Width(const std::u16string& text, bool eightBit, bool secondFont) const {
    const MenuFont& f = secondFont ? fonts_.b : fonts_.a;
    int width = 0;
    for (char16_t c : text) width += f.glyphs[size_t(Index(f, c, eightBit, secondFont))].advance;
    return width;
}

MenuPrim MenuCentredSprite(const MenuTableSprite& s, int cx, int cy, int extraX, int extraY) {
    MenuPrim p;
    p.kind = MenuPrim::kSprite;
    p.x[0] = int16_t(cx - (int16_t(s.w) >> 1) + extraX);
    p.y[0] = int16_t(cy - (int16_t(s.h) >> 1) + extraY);
    p.w = int16_t(s.w);
    p.h = int16_t(s.h);
    p.u = s.u;
    p.v = s.v;
    p.clut = s.clut;
    p.tpage = s.tpage;
    return p;
}

// ---------------------------------------------------------------- GTMP and tiles

GtmpPicture ParseGtmp(std::span<const uint8_t> e) {
    if (e.size() < GtmpPicture::kHeaderSize || std::memcmp(e.data(), "GTMP", 4) != 0) throw std::runtime_error("GTMP: bad magic or short entry");
    if ((e.size() - GtmpPicture::kHeaderSize) % 4096 != 0) throw std::runtime_error("GTMP: pixel data is not whole 4 KB chunks");
    GtmpPicture p;
    for (size_t i = 0; i < p.tiles.size(); i++) p.tiles[i] = U32(e, 4 + 4 * i);
    for (size_t i = 0; i < p.clut.size(); i++) p.clut[i] = U16(e, 0x1F84 + 2 * i);
    p.textureTiles = U32(e, 0x3F84);
    p.pixels.assign(e.begin() + GtmpPicture::kHeaderSize, e.end());
    return p;
}

MenuTile DecodeBackgroundTile(uint32_t w) {
    MenuTile t;
    t.y = int((w >> 8) & 0xFF) << 3;
    if (w & 0x80) { // 0x80021D30
        const uint32_t c = w >> 16;
        t.flat = true;
        t.x = int(w & 0x3F) << 4;
        t.r = uint8_t((c & 0x1F) << 3);
        t.g = uint8_t(((c >> 5) & 0x1F) << 3);
        t.b = uint8_t(((c >> 10) & 0x1F) << 3);
        return t;
    }
    const uint32_t hi = w >> 16; // 0x80021EB0
    t.x = int(w & 0xFF) << 4;
    t.tpage = uint16_t(((hi & 0x400) >> 6) | ((hi & 0x10) >> 3) | 0x8C);
    t.u = uint8_t((hi & 0xF) << 4);
    t.v = uint8_t((w >> 18) & 0xF8);
    t.clut = uint16_t((((w >> 28) & 1) << 4) | ((((w >> 28) & 0xE) | 0x3F0) << 5));
    return t;
}

MenuTile DecodePageTile(uint32_t w) {
    if (w & 0x80) return DecodeBackgroundTile(w); // same flat tile routine 0x80021D30
    MenuTile t; // 0x80021DE8
    t.x = int(w & 0xFF) << 4;
    t.y = int((w >> 8) & 0xFF) << 3;
    t.tpage = uint16_t(((w >> 20) & 1) | 0x1A);
    t.u = uint8_t(((w >> 16) & 0xF) << 4);
    t.v = uint8_t((w >> 18) & 0xF8);
    t.clut = uint16_t(((w >> 27) & 3) | 0x24 | ((((w >> 27) & 0x1C) | 0x3E0) << 4));
    return t;
}

// ---------------------------------------------------------------- VRAM

void MenuVram::Upload(int x, int y, int w, int h, std::span<const uint8_t> d) {
    if (d.size() < size_t(w) * h * 2) throw std::runtime_error("VRAM upload: not enough data");
    for (int row = 0; row < h; row++)
        for (int col = 0; col < w; col++)
            words_[size_t((y + row) & (kHeight - 1)) * kWidth + size_t((x + col) & (kWidth - 1))] = U16(d, (size_t(row) * w + col) * 2);
}

void MenuVram::UploadTimToPage(std::span<const uint8_t> tim, uint16_t tpage) {
    if (U32(tim, 0) != 0x10) throw std::runtime_error("TIM: bad magic");
    size_t block = 8;
    if (U32(tim, 4) & 8) block += U32(tim, 8); // skip the CLUT block
    const int w = U16(tim, block + 8), h = U16(tim, block + 10);
    Upload((tpage & 0xF) * 64, (tpage & 0x10) * 16, w, h, tim.subspan(block + 12));
}

uint16_t MenuVram::Sample(uint16_t tpage, uint16_t clut, uint8_t u, uint8_t v) const {
    const int pageX = (tpage & 0xF) * 64, pageY = ((tpage >> 4) & 1) * 256;
    const int clutX = (clut & 0x3F) * 16, clutY = (clut >> 6) & 0x1FF;
    switch ((tpage >> 7) & 3) {
    case 0: return Word(clutX + ((Word(pageX + u / 4, pageY + v) >> ((u & 3) * 4)) & 0xF), clutY);
    case 1: return Word(clutX + ((Word(pageX + u / 2, pageY + v) >> ((u & 1) * 8)) & 0xFF), clutY);
    default: return Word(pageX + u, pageY + v);
    }
}

// ---------------------------------------------------------------- fonts and table sprites

int MenuFont::Lookup(uint32_t code) const {
    if (code < 0x100) return map[code];
    if (!searchWide) return 0;
    // 0x8001F2C8, literally (binary search over the codes, at most 16 steps, 0 = not found).
    const int count = int(glyphs.size());
    int hi = count, sum = count, lo = 0, steps = 0;
    for (;;) {
        const int mid = sum >> 1;
        const uint32_t c = glyphs[size_t(mid)].code;
        if (c == code) return mid;
        int newLo = mid;
        if (code < c) {
            newLo = lo;
            hi = mid;
        }
        if (lo == mid) return 0;
        if (++steps > 15) return 0;
        sum = hi + newLo;
        lo = newLo;
    }
}

namespace {
MenuFont ReadFont(const GuestImage& ovl4, uint32_t glyphs, uint32_t metrics, uint32_t count, bool wide) {
    MenuFont f;
    f.searchWide = wide;
    for (uint32_t i = 0; i < count; i++) {
        MenuFont::Glyph g;
        g.code = ovl4.Get<uint16_t>(metrics + 8 * i);
        g.dx = ovl4.Get<int16_t>(metrics + 8 * i + 2);
        g.dy = ovl4.Get<int16_t>(metrics + 8 * i + 4);
        g.advance = ovl4.Get<int16_t>(metrics + 8 * i + 6);
        g.u = ovl4.Get<uint8_t>(glyphs + 12 * i);
        g.v = ovl4.Get<uint8_t>(glyphs + 12 * i + 1);
        g.clut = ovl4.Get<uint16_t>(glyphs + 12 * i + 2);
        g.w = ovl4.Get<uint16_t>(glyphs + 12 * i + 4);
        g.h = ovl4.Get<uint16_t>(glyphs + 12 * i + 6);
        g.tpage = ovl4.Get<uint16_t>(glyphs + 12 * i + 8);
        f.glyphs.push_back(g);
    }
    for (uint32_t i = 0; i < count; i++) // 0x8001F210
        if (f.glyphs[i].code < 0x100) f.map[f.glyphs[i].code] = int16_t(i);
    return f;
}
} // namespace

MenuFonts LoadMenuFonts(const GuestImage& input) {
    const auto ovl4=UiLayout(input);
    MenuFonts f;
    f.a = ReadFont(ovl4, kFontAGlyphs, kFontAMetrics, kFontACount, true);
    f.b = ReadFont(ovl4, kFontBGlyphs, kFontBMetrics, kFontBCount, false);
    return f;
}

MenuTableSprite ReadMenuTableSprite(const GuestImage& ovl4, uint32_t a) {
    MenuTableSprite s;
    s.u = ovl4.Get<uint8_t>(a);
    s.v = ovl4.Get<uint8_t>(a + 1);
    s.clut = ovl4.Get<uint16_t>(a + 2);
    s.w = ovl4.Get<uint16_t>(a + 4);
    s.h = ovl4.Get<uint16_t>(a + 6);
    s.tpage = ovl4.Get<uint16_t>(a + 8);
    return s;
}

// ---------------------------------------------------------------- canvas

void MenuCanvas::Fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
    const uint16_t c = uint16_t((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    for (int j = std::max(0, y); j < std::min(kHeight, y + h); j++)
        for (int i = std::max(0, x); i < std::min(kWidth, x + w); i++) pixels_[size_t(j) * kWidth + size_t(i)] = c;
}

void MenuCanvas::Sprite(const MenuVram& vram, int x, int y, int w, int h, uint8_t u, uint8_t v, uint16_t tpage, uint16_t clut, uint32_t colour, bool semi) {
    const int mr = int(colour & 0xFF), mg = int((colour >> 8) & 0xFF), mb = int((colour >> 16) & 0xFF);
    const int mode = (tpage >> 5) & 3;
    for (int j = 0; j < h; j++) {
        const int py = y + j;
        if (py < 0 || py >= kHeight) continue;
        for (int i = 0; i < w; i++) {
            const int px = x + i;
            if (px < 0 || px >= kWidth) continue;
            const uint16_t t = vram.Sample(tpage, clut, uint8_t(u + i), uint8_t(v + j));
            if (t == 0) continue;
            int r = std::min(31, ((t & 31) * mr) >> 7), g = std::min(31, (((t >> 5) & 31) * mg) >> 7), b = std::min(31, (((t >> 10) & 31) * mb) >> 7);
            uint16_t& dst = pixels_[size_t(py) * kWidth + size_t(px)];
            if (semi && (t & 0x8000)) {
                const int br = dst & 31, bg = (dst >> 5) & 31, bb = (dst >> 10) & 31;
                auto mix = [mode](int back, int front) {
                    switch (mode) {
                    case 0: return (back + front) >> 1;
                    case 1: return std::min(31, back + front);
                    case 2: return std::max(0, back - front);
                    default: return std::min(31, back + (front >> 2));
                    }
                };
                r = mix(br, r);
                g = mix(bg, g);
                b = mix(bb, b);
            }
            dst = uint16_t(r | (g << 5) | (b << 10) | (t & 0x8000));
        }
    }
}

// One untextured pixel (8-bit colour) of a polygon, line or tile at (x, y) of the drawing area.
void MenuCanvas::Blend(int x, int y, int r, int g, int b, bool semi, int mode, bool dither) {
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) return;
    uint16_t& dst = pixels_[size_t(y) * kWidth + size_t(x)];
    int c[3] = {r, g, b};
    const int back[3] = {dst & 31, (dst >> 5) & 31, (dst >> 10) & 31};
    if (rules == Rules::kInterpreter) { // src/machine/gpu.cpp PlotPixel: blend in 8 bits, background expanded by << 3
        for (int k = 0; k < 3; k++) {
            int v = c[k];
            if (semi) {
                const int bk = back[k] << 3;
                switch (mode) {
                case 0: v = (bk + v) / 2; break;
                case 1: v = bk + v; break;
                case 2: v = bk - v; break;
                default: v = bk + v / 4; break;
                }
            }
            c[k] = std::clamp(v, 0, 255) >> 3;
        }
        dst = uint16_t(c[0] | (c[1] << 5) | (c[2] << 10));
        return;
    }
    // PS1: the 8-bit colour plus the 4x4 dither offset (0 without dither), >> 3 and clamped to 0..31; the blend in
    // 5 bits per channel (mode 0 (B + F) / 2, 1 B + F, 2 B - F, 3 B + F / 4, saturated).
    static constexpr int kDither[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};
    const int d = dither ? kDither[y & 3][x & 3] : 0;
    for (int k = 0; k < 3; k++) {
        int f = std::clamp((c[k] + d) >> 3, 0, 31);
        if (semi) {
            switch (mode) {
            case 0: f = (back[k] + f) >> 1; break;
            case 1: f = std::min(31, back[k] + f); break;
            case 2: f = std::max(0, back[k] - f); break;
            default: f = std::min(31, back[k] + (f >> 2)); break;
            }
        }
        c[k] = f;
    }
    dst = uint16_t(c[0] | (c[1] << 5) | (c[2] << 10));
}

namespace {
struct RasterVertex {
    int x = 0, y = 0;
    int c[3] = {};  // 8-bit r, g, b
};
using RasterPlot = std::function<void(int x, int y, int r, int g, int b)>;

// PS1 GPU triangle (untextured). Coverage: the vertices sorted by y; the long edge v0 -> v2 and the short edges
// v0 -> v1 / v1 -> v2 walked in 32.32 fixed point (a vertex at x + 1 - 2^-21, steps (dx << 32) / dy rounded away
// from zero); a span covers [left >> 32, right >> 32) on each row from the top vertex's y (inclusive) to the bottom
// one's (exclusive). A part whose far end is the leftmost vertex is walked from that end upwards. Colours: per-pixel
// steps d/dx, d/dy in 12.12 fixed point (x 4096 / determinant, truncated) from the leftmost vertex (0.5 added),
// shifted to 8.24 and wrapped as unsigned 32-bit; the colour of a pixel = bits 24..31.
void RasterTrianglePs1(RasterVertex v0, RasterVertex v1, RasterVertex v2, bool gouraud, const RasterPlot& plot) {
    int origin; // the leftmost vertex (ties: the later one on the first comparison, as the hardware picks it)
    if (v1.x <= v0.x) origin = (v2.x <= v1.x) ? 2 : 1;
    else origin = (v2.x < v0.x) ? 2 : 0;
    RasterVertex v[3] = {v0, v1, v2};
    auto swap = [&](int i, int j) {
        std::swap(v[i], v[j]);
        if (origin == i) origin = j;
        else if (origin == j) origin = i;
    };
    if (v[2].y < v[1].y) swap(1, 2);
    if (v[1].y < v[0].y) swap(0, 1);
    if (v[2].y < v[1].y) swap(1, 2);
    if (v[0].y == v[2].y) return;
    if (v[2].y - v[0].y >= 512 || std::abs(v[2].x - v[0].x) >= 1024 || std::abs(v[2].x - v[1].x) >= 1024 || std::abs(v[1].x - v[0].x) >= 1024) return;
    const int det = (v[1].x - v[0].x) * (v[2].y - v[1].y) - (v[2].x - v[1].x) * (v[1].y - v[0].y);
    if (det == 0) return;

    auto fixedX = [](int x) { return (int64_t(x) << 32) + ((int64_t(1) << 32) - (int64_t(1) << 11)); };
    auto stepX = [](int dx, int dy) {
        int64_t n = int64_t(dx) * (int64_t(1) << 32);
        if (dx < 0) n -= dy - 1;
        else if (dx > 0) n += dy - 1;
        return n / dy;
    };
    const int64_t base = fixedX(v[0].x), baseStep = stepX(v[2].x - v[0].x, v[2].y - v[0].y);
    const int64_t upperStep = v[1].y == v[0].y ? 0 : stepX(v[1].x - v[0].x, v[1].y - v[0].y);
    const int64_t lowerStep = v[2].y == v[1].y ? 0 : stepX(v[2].x - v[1].x, v[2].y - v[1].y);
    const bool rightFacing = v[1].y == v[0].y ? v[1].x > v[0].x : upperStep > baseStep;

    uint32_t start[3], dx[3], dy[3];
    const RasterVertex& o = v[origin];
    for (int c = 0; c < 3; c++) {
        dx[c] = dy[c] = 0;
        if (gouraud) {
            const int ddx = (v[1].c[c] - v[0].c[c]) * (v[2].y - v[1].y) - (v[2].c[c] - v[1].c[c]) * (v[1].y - v[0].y);
            const int ddy = (v[1].x - v[0].x) * (v[2].c[c] - v[1].c[c]) - (v[2].x - v[1].x) * (v[1].c[c] - v[0].c[c]);
            dx[c] = uint32_t(ddx * 4096 / det) << 12;
            dy[c] = uint32_t(ddy * 4096 / det) << 12;
        }
        start[c] = ((uint32_t(o.c[c]) << 12) + 2048u) << 12;
        start[c] += dx[c] * uint32_t(-o.x); // modulo 2^32, as the hardware's registers wrap
        start[c] += dy[c] * uint32_t(-o.y);
    }
    auto span = [&](int y, int64_t left, int64_t right) {
        if (y < 0 || y >= MenuCanvas::kHeight) return;
        const int x0 = int(left >> 32), x1 = int(right >> 32);
        for (int x = std::max(x0, 0); x < std::min(x1, MenuCanvas::kWidth); x++) {
            int rgb[3];
            for (int c = 0; c < 3; c++) rgb[c] = int((start[c] + dy[c] * uint32_t(y) + dx[c] * uint32_t(x)) >> 24);
            plot(x, y, rgb[0], rgb[1], rgb[2]);
        }
    };
    // One part of the triangle between rows yFrom and yTo: the side edge from (sideX, yFrom), the long edge at row
    // yFrom; walked downwards (yFrom < yTo) or upwards from yFrom (yFrom > yTo, the rows below yFrom exclusive).
    auto part = [&](int yFrom, int yTo, int64_t side, int64_t sideStep, int64_t longEdge) {
        int64_t l = rightFacing ? longEdge : side, r = rightFacing ? side : longEdge;
        const int64_t ls = rightFacing ? baseStep : sideStep, rs = rightFacing ? sideStep : baseStep;
        if (yFrom > yTo) {
            for (int y = yFrom; y > yTo;) {
                y--;
                l -= ls;
                r -= rs;
                span(y, l, r);
            }
        } else {
            for (int y = yFrom; y < yTo; y++, l += ls, r += rs) span(y, l, r);
        }
    };
    const bool upperUp = origin != 0, lowerUp = origin == 2;
    auto upper = [&] {
        if (upperUp) part(v[1].y, v[0].y, fixedX(v[1].x), upperStep, base + int64_t(v[1].y - v[0].y) * baseStep);
        else part(v[0].y, v[1].y, fixedX(v[0].x), upperStep, base);
    };
    auto lower = [&] {
        if (lowerUp) part(v[2].y, v[1].y, fixedX(v[2].x), lowerStep, base + int64_t(v[2].y - v[0].y) * baseStep);
        else part(v[1].y, v[2].y, fixedX(v[1].x), lowerStep, base + int64_t(v[1].y - v[0].y) * baseStep);
    };
    if (upperUp) {
        lower();
        upper();
    } else {
        upper();
        lower();
    }
}

// The triangle of our interpreter's GPU (src/machine/gpu.cpp DrawTriangle, untextured), expression for expression:
// bounding box scan, edge functions with a top-left bias, colours = int of the floating-point barycentric sums.
void RasterTriangleInterpreter(RasterVertex v0, RasterVertex v1, RasterVertex v2, bool gouraud, const RasterPlot& plot) {
    int64_t area = int64_t(v1.x - v0.x) * (v2.y - v0.y) - int64_t(v1.y - v0.y) * (v2.x - v0.x);
    if (area == 0) return;
    if (area < 0) {
        std::swap(v1, v2);
        area = -area;
    }
    int minX = std::min({v0.x, v1.x, v2.x}), maxX = std::max({v0.x, v1.x, v2.x});
    int minY = std::min({v0.y, v1.y, v2.y}), maxY = std::max({v0.y, v1.y, v2.y});
    if (maxX - minX > 1023 || maxY - minY > 511) return;
    minX = std::max(minX, 0);
    maxX = std::min(maxX, MenuCanvas::kWidth - 1);
    minY = std::max(minY, 0);
    maxY = std::min(maxY, MenuCanvas::kHeight - 1);
    auto edge = [](const RasterVertex& a, const RasterVertex& b, int x, int y) { return int64_t(b.x - a.x) * (y - a.y) - int64_t(b.y - a.y) * (x - a.x); };
    auto bias = [](const RasterVertex& a, const RasterVertex& b) {
        const int ddx = b.x - a.x, ddy = b.y - a.y;
        return (ddy < 0 || (ddy == 0 && ddx > 0)) ? 0 : -1;
    };
    const int b0 = bias(v1, v2), b1 = bias(v2, v0), b2 = bias(v0, v1);
    const double inv = 1.0 / double(area);
    for (int y = minY; y <= maxY; y++)
        for (int x = minX; x <= maxX; x++) {
            const int64_t w0 = edge(v1, v2, x, y), w1 = edge(v2, v0, x, y), w2 = edge(v0, v1, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            const double l0 = double(w0) * inv, l1 = double(w1) * inv, l2 = double(w2) * inv;
            int rgb[3] = {v0.c[0], v0.c[1], v0.c[2]};
            if (gouraud)
                for (int c = 0; c < 3; c++) rgb[c] = int(l0 * v0.c[c] + l1 * v1.c[c] + l2 * v2.c[c]);
            plot(x, y, rgb[0], rgb[1], rgb[2]);
        }
}

// PS1 GPU line: the major axis stepped k = max(|dx|, |dy|) times in 32.32 fixed point (steps rounded away from
// zero), starting at the left end (x + 0.5 - 2^-22, y + 0.5, minus 2^-22 when y decreases); both ends drawn.
// Gouraud colours in 12.12 steps.
void RasterLinePs1(RasterVertex a, RasterVertex b, bool gouraud, const RasterPlot& plot) {
    const int adx = std::abs(b.x - a.x), ady = std::abs(b.y - a.y), k = std::max(adx, ady);
    if (adx >= 1024 || ady >= 512) return;
    if (a.x >= b.x && k > 0) std::swap(a, b);
    auto div = [](int64_t d, int n) { return (d * (int64_t(1) << 32) - (d < 0 ? n - 1 : 0) + (d > 0 ? n - 1 : 0)) / n; };
    const int64_t sx = k ? div(b.x - a.x, k) : 0, sy = k ? div(b.y - a.y, k) : 0;
    int64_t x = (int64_t(a.x) << 32) + (int64_t(1) << 31) - 1024;
    int64_t y = (int64_t(a.y) << 32) + (int64_t(1) << 31) - (sy < 0 ? 1024 : 0);
    int32_t c[3], sc[3];
    for (int i = 0; i < 3; i++) {
        c[i] = (a.c[i] << 12) | (1 << 11);
        sc[i] = (gouraud && k) ? ((b.c[i] - a.c[i]) * 4096) / k : 0;
    }
    for (int i = 0; i <= k; i++) {
        plot(int(x >> 32), int(y >> 32), gouraud ? (c[0] >> 12) & 0xFF : a.c[0], gouraud ? (c[1] >> 12) & 0xFF : a.c[1], gouraud ? (c[2] >> 12) & 0xFF : a.c[2]);
        x += sx;
        y += sy;
        for (int j = 0; j < 3; j++) c[j] += sc[j];
    }
}

// The line of our interpreter's GPU (src/machine/gpu.cpp DrawLine): floating-point steps rounded to nearest.
void RasterLineInterpreter(const RasterVertex& a, const RasterVertex& b, const RasterPlot& plot) {
    const int adx = std::abs(b.x - a.x), ady = std::abs(b.y - a.y), steps = std::max(adx, ady);
    if (adx > 1023 || ady > 511) return;
    for (int i = 0; i <= steps; i++) {
        const double t = steps ? double(i) / steps : 0.0;
        plot(int(std::lround(a.x + (b.x - a.x) * t)), int(std::lround(a.y + (b.y - a.y) * t)), int(a.c[0] + (b.c[0] - a.c[0]) * t),
             int(a.c[1] + (b.c[1] - a.c[1]) * t), int(a.c[2] + (b.c[2] - a.c[2]) * t));
    }
}

RasterVertex MakeVertex(const MenuPrim& p, int i, int colourIndex) {
    RasterVertex v;
    v.x = p.x[i];
    v.y = p.y[i];
    const uint32_t c = p.colour[colourIndex];
    v.c[0] = int(c & 0xFF);
    v.c[1] = int((c >> 8) & 0xFF);
    v.c[2] = int((c >> 16) & 0xFF);
    return v;
}
} // namespace

void MenuCanvas::Draw(const MenuVram& vram, const MenuPrim& p) {
    const int mode = (p.tpage >> 5) & 3;
    switch (p.kind) {
    case MenuPrim::kSprite: Sprite(vram, p.x[0], p.y[0], p.w, p.h, p.u, p.v, p.tpage, p.clut, p.colour[0], p.semi); return;
    case MenuPrim::kTile: // flat rectangles are never dithered
        for (int j = 0; j < p.h; j++)
            for (int i = 0; i < p.w; i++)
                if (!p.Clipped(p.x[0] + i, p.y[0] + j))
                    Blend(p.x[0] + i, p.y[0] + j, int(p.colour[0] & 0xFF), int((p.colour[0] >> 8) & 0xFF), int((p.colour[0] >> 16) & 0xFF), p.semi, mode, false);
        return;
    case MenuPrim::kPolyF4:
    case MenuPrim::kPolyG4: { // two triangles v0 v1 v2 and v1 v2 v3; only shaded polygons are dithered
        const bool g = p.kind == MenuPrim::kPolyG4;
        const bool dither = p.dither && g;
        const RasterPlot plot = [&](int x, int y, int r, int gg, int b) {
            if (!p.Clipped(x, y)) Blend(x, y, r, gg, b, p.semi, mode, dither);
        };
        for (int t = 0; t < 2; t++) {
            const RasterVertex a = MakeVertex(p, t, g ? t : 0), b = MakeVertex(p, t + 1, g ? t + 1 : 0), c = MakeVertex(p, t + 2, g ? t + 2 : 0);
            if (rules == Rules::kInterpreter) RasterTriangleInterpreter(a, b, c, g, plot);
            else RasterTrianglePs1(a, b, c, g, plot);
        }
        return;
    }
    case MenuPrim::kLine: {
        const bool dither = p.dither && p.gouraud;
        const RasterPlot plot = [&](int x, int y, int r, int g, int b) {
            if (!p.Clipped(x, y)) Blend(x, y, r, g, b, p.semi, mode, dither);
        };
        const RasterVertex a = MakeVertex(p, 0, 0), b = MakeVertex(p, 1, p.gouraud ? 1 : 0);
        if (rules == Rules::kInterpreter) RasterLineInterpreter(a, b, plot);
        else RasterLinePs1(a, b, p.gouraud, plot);
        return;
    }
    case MenuPrim::kPolyFT4: { // triangles v0 v1 v2 and v1 v2 v3 as src/machine/gpu.cpp DrawTriangle (both rule sets)
        const int mr = int(p.colour[0] & 0xFF), mg = int((p.colour[0] >> 8) & 0xFF), mb = int((p.colour[0] >> 16) & 0xFF);
        for (int t = 0; t < 2; t++) {
            int i0 = t, i1 = t + 1, i2 = t + 2;
            int64_t area = int64_t(p.x[i1] - p.x[i0]) * (p.y[i2] - p.y[i0]) - int64_t(p.y[i1] - p.y[i0]) * (p.x[i2] - p.x[i0]);
            if (area == 0) continue;
            if (area < 0) {
                std::swap(i1, i2);
                area = -area;
            }
            const int xs[3] = {p.x[i0], p.x[i1], p.x[i2]}, ys[3] = {p.y[i0], p.y[i1], p.y[i2]};
            const int us[3] = {p.tu[i0], p.tu[i1], p.tu[i2]}, vs[3] = {p.tv[i0], p.tv[i1], p.tv[i2]};
            int minX = std::min({xs[0], xs[1], xs[2]}), maxX = std::max({xs[0], xs[1], xs[2]});
            int minY = std::min({ys[0], ys[1], ys[2]}), maxY = std::max({ys[0], ys[1], ys[2]});
            if (maxX - minX > 1023 || maxY - minY > 511) continue;
            minX = std::max(minX, 0), maxX = std::min(maxX, kWidth - 1), minY = std::max(minY, 0), maxY = std::min(maxY, kHeight - 1);
            auto edge = [&](int a, int b, int x, int y) { return int64_t(xs[b] - xs[a]) * (y - ys[a]) - int64_t(ys[b] - ys[a]) * (x - xs[a]); };
            auto bias = [&](int a, int b) {
                const int ddx = xs[b] - xs[a], ddy = ys[b] - ys[a];
                return (ddy < 0 || (ddy == 0 && ddx > 0)) ? 0 : -1;
            };
            const int b0 = bias(1, 2), b1 = bias(2, 0), b2 = bias(0, 1);
            const double inv = 1.0 / double(area);
            for (int y = minY; y <= maxY; y++)
                for (int x = minX; x <= maxX; x++) {
                    const int64_t w0 = edge(1, 2, x, y), w1 = edge(2, 0, x, y), w2 = edge(0, 1, x, y);
                    if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0 || p.Clipped(x, y)) continue;
                    const double l0 = double(w0) * inv, l1 = double(w1) * inv, l2 = double(w2) * inv;
                    const int u = int(l0 * us[0] + l1 * us[1] + l2 * us[2] + 0.5), v = int(l0 * vs[0] + l1 * vs[1] + l2 * vs[2] + 0.5);
                    const uint16_t tex = vram.Sample(p.tpage, p.clut, uint8_t(u & 0xFF), uint8_t(v & 0xFF));
                    if (tex == 0) continue;
                    Blend(x, y, ((tex & 31) << 3) * mr / 128, (((tex >> 5) & 31) << 3) * mg / 128, (((tex >> 10) & 31) << 3) * mb / 128, p.semi && (tex & 0x8000),
                          mode, false);
                }
        }
        return;
    }
    }
}

std::vector<uint8_t> MenuCanvas::Rgba() const {
    std::vector<uint8_t> out(pixels_.size() * 4);
    for (size_t i = 0; i < pixels_.size(); i++) {
        const uint16_t c = pixels_[i];
        out[i * 4 + 0] = uint8_t((c & 31) << 3);
        out[i * 4 + 1] = uint8_t(((c >> 5) & 31) << 3);
        out[i * 4 + 2] = uint8_t(((c >> 10) & 31) << 3);
        out[i * 4 + 3] = 255;
    }
    return out;
}

// ---------------------------------------------------------------- assets

MenuAssets MenuAssets::Load(const DiscImage& disc, const GtfsVolume& vol, const std::string& language) {
    MenuAssets a;
    const auto original=LoadOverlayImage(disc,4);
    a.ovl4 = UiLayout(original);
    a.fonts = LoadMenuFonts(a.ovl4);
    a.commonIndex = ParseMenuPackIndex(vol.Read("gtmenu/commonpic.idx"));
    const GtfsEntry* dat = vol.Find("gtmenu/commonpic.dat");
    if (!dat) throw std::runtime_error("no gtmenu/commonpic.dat");
    a.commonDat = vol.ReadStored(*dat);
    // 0x80013CF8: VOL files 0x2C / 0x2D / 0x2E -> tpages 9 / 0x18 / 10 (0x80013B28 + 0x80013B60).
    a.cursorTim = vol.Read("arcade/gt_cursor.tim");
    a.itemsTim = vol.Read("arcade/gt_items.tim");
    a.fontTim = vol.Read("arcade/gtmode_font.tim");
    a.iconImage = vol.Read("gtmenu/" + language + "/iconimg.dat");
    // data-gt.txd: gzip inside the overlay; 0x80020C50 copies the block of the language byte (US = 1).
    // The member is followed by other data, so the deflate stream is inflated after its gzip header (flag 8: name).
    const std::vector<uint8_t> txd = InflateEmbeddedGzipNamed(original, "data-gt.txd");
    const size_t lang = kLanguageUsa;
    if (txd.size() < (lang + 1) * kGtStringsSize) throw std::runtime_error("data-gt.txd: too short");
    a.strings.assign(txd.begin() + ptrdiff_t(lang * kGtStringsSize), txd.begin() + ptrdiff_t((lang + 1) * kGtStringsSize));
    return a;
}

std::string MenuAssets::String(uint32_t address) const {
    if (address < kGtStringsRam || address >= kGtStringsRam + strings.size()) throw std::out_of_range("data-gt.txd address");
    std::string s;
    for (size_t i = address - kGtStringsRam; i < strings.size() && strings[i]; i++) s.push_back(char(strings[i]));
    return s;
}

GtmpPicture MenuAssets::Background(uint32_t entry) const {
    if (entry >= commonIndex.Count()) throw std::out_of_range("commonpic entry");
    return ParseGtmp(MenuPackEntry(commonDat, commonIndex, entry, false));
}

// ---------------------------------------------------------------- page renderer

void ComposeMenuVram(const MenuAssets& assets, const MenuPage& page, MenuVram& vram, int* backgroundRows, int* pageRows) {
    vram.UploadTimToPage(assets.cursorTim, 9);
    vram.UploadTimToPage(assets.itemsTim, 0x18);
    vram.UploadTimToPage(assets.fontTim, 10);
    vram.Upload(704, 0, 64, 256, assets.iconImage);
    const GtmpPicture bg = assets.Background(page.picture);
    std::vector<uint8_t> clutBytes(bg.clut.size() * 2);
    std::memcpy(clutBytes.data(), bg.clut.data(), clutBytes.size());
    vram.Upload(0, 504, 512, 8, clutBytes);
    vram.Upload(768, 0, 256, bg.Rows(), bg.pixels);
    std::vector<uint8_t> pageClut(page.own.clut.size() * 2);
    std::memcpy(pageClut.data(), page.own.clut.data(), pageClut.size());
    vram.Upload(576, 248, 64, 8, pageClut);
    vram.Upload(640, 256, 128, page.own.Rows(), page.own.pixels);
    if (backgroundRows) *backgroundRows = bg.Rows();
    if (pageRows) *pageRows = page.own.Rows();
}

MenuFrame BuildMenuFrame(const MenuAssets& assets, const MenuPage& page, const MenuRenderState& state) {
    using namespace menu_item_flag;
    MenuFrame out;
    out.clearBehind = page.ClearsBehind();
    std::vector<MenuPrim>& list = out.prims;
    auto tile = [&](const MenuTile& t) {
        MenuPrim p;
        p.x[0] = int16_t(t.x);
        p.y[0] = int16_t(t.y);
        p.w = 16;
        p.h = 8;
        if (t.flat) {
            if (page.ClearsBehind() && t.r == 0 && t.g == 0 && t.b == 0) return;
            p.kind = MenuPrim::kTile;
            p.colour[0] = uint32_t(t.r) | (uint32_t(t.g) << 8) | (uint32_t(t.b) << 16);
        } else {
            p.kind = MenuPrim::kSprite;
            p.u = t.u;
            p.v = t.v;
            p.tpage = t.tpage;
            p.clut = t.clut;
        }
        list.push_back(p);
    };
    const GtmpPicture bg = assets.Background(page.picture);
    for (uint32_t w : bg.tiles) tile(DecodeBackgroundTile(w));   // 0x80021F88
    out.backgroundId = int(page.picture);
    out.backgroundPrims = list.size();
    for (uint32_t w : page.own.tiles) tile(DecodePageTile(w));   // 0x8002202C
    for (const MenuGroup& g : page.groups)                       // 0x800220C8
        for (const MenuSprite& s : g.sprites) {
            MenuPrim p;
            p.x[0] = s.x;
            p.y[0] = s.y;
            p.w = s.w;
            p.h = s.h;
            p.u = s.u;
            p.v = s.v;
            p.tpage = uint16_t(s.tpage & 0x1F);
            p.clut = s.clut;
            list.push_back(p);
        }

    // Items (0x8001B9AC).
    std::vector<MenuPrim> prims, custom, late;
    MenuTextWriter text(assets.fonts, prims);
    auto Widen = MenuTextWriter::Widen;
    auto Thousands = MenuThousands;
    auto CentredSprite = [](std::vector<MenuPrim>& v, const MenuTableSprite& s, int cx, int cy, int ex = 0, int ey = 0) { v.push_back(MenuCentredSprite(s, cx, cy, ex, ey)); };
    const GuestImage& o4 = assets.ovl4;
    char buf[64];
    for (const MenuItem& it : page.items) {
        const bool second = it.Has(kSecondFont);
        uint16_t type = it.Type();
        const size_t before = prims.size(), customBefore = custom.size();
        if (state.customItem && state.customItem(it, custom)) {
            if (custom.size() != customBefore)
                out.notes.push_back("item " + std::to_string(&it - page.items.data()) + " type " + std::to_string(type) + ": " + std::to_string(custom.size() - customBefore) +
                                    " custom primitives");
            continue;
        }
        if (it.Has(kEvent)) {
            const std::string name = it.Name();
            const std::optional<MenuRenderState::EventInfo> info = state.eventInfo ? state.eventInfo(name) : std::nullopt;
            const int result = state.eventResult ? state.eventResult(name) : 0;
            if (type == 0xCF && info) text.Right(Widen(Thousands(uint32_t(info->bonus))), false, second, it.x1, it.y1, kTextColour);
            if (it.Has(kEventResult) && result != 0) {
                if (result < 0 || result > 3) {
                    std::snprintf(buf, sizeof buf, "%d", result);
                    text.Left(Widen(buf), true, second, it.CenterX() - 4, it.CenterY() + 4, kTextColour);
                } else {
                    CentredSprite(prims, ReadMenuTableSprite(o4, kMedalSprites + uint32_t(4 - result) * 12), it.CenterX(), it.CenterY());
                }
            }
            if (it.Has(kEventTrophy) && result != 0) {
                if (result == 1) {
                    CentredSprite(prims, ReadMenuTableSprite(o4, kTrophySprite), it.CenterX(), it.CenterY(), -2, 6);
                } else {
                    std::snprintf(buf, sizeof buf, "%d", result);
                    text.Left(Widen(buf), true, second, it.x0, it.y1, kTextColour);
                }
            }
            if (it.Has(kEventPrize) && info && it.Byte4B() >= 1 && it.Byte4B() <= 6)
                text.Right(Widen(Thousands(uint32_t(info->prize[size_t(it.Byte4B() - 1)]))), false, second, it.x1, it.y1, kTextColour);
            if (it.Has(kEventPower) && info) {
                std::string s = assets.String(0x801C30E2u); // "free"
                if (info->powerLimit > 0) {
                    std::snprintf(buf, sizeof buf, assets.String(0x801C30DCu).c_str(), (info->powerLimit * 1000) / 0x3F6); // "~%dhp"
                    s = buf;
                }
                text.Left(Widen(s), true, second, it.x0, it.y1, kTextColour);
            }
            if (it.Has(kEventLicence) && info && info->licence + 1 >= 0)
                CentredSprite(prims, ReadMenuTableSprite(o4, kLicenceNeedSprites + uint32_t(info->licence + 1) * 12), it.CenterX(), it.CenterY());
        }
        if (state.lateItem && state.lateItem(it, late)) type = 0xFFFF;             // the second OT (view +0x88)
        else if (state.dynamicItem && state.dynamicItem(it, prims)) type = 0xFFFF; // handled by the caller
        switch (type) {
        case 0x06:
            if (!it.Has(kHidePrice) && state.carPrice)
                if (const std::optional<uint32_t> price = state.carPrice(it.CarId()))
                    text.Right(Widen(Thousands(*price)), false, second, it.x1 - 0x50, it.y1 + 0x14, 0x2808080u);
            break;
        case 0x47:
            if (!state.currentCar.empty()) text.Left(Widen(state.currentCar), true, second, it.x0, it.y1, kTextColour); // 0x8001B818 simplified
            break;
        case 0x4D: text.Right(Widen(state.money == 0 ? "0" : Thousands(uint32_t(state.money))), false, second, it.x1, it.y1, kTextColour); break;
        case 0x4F:
            std::snprintf(buf, sizeof buf, "%u", state.day); // 0x80020110 "%d" (member 4 0x800242A4)
            text.Right(Widen(buf), false, second, it.x1, it.y1, kTextColour);
            break;
        default: break;
        }
        if (prims.size() != before) out.notes.push_back("item " + std::to_string(&it - page.items.data()) + " type " + std::to_string(type) + ": " +
                                                        std::to_string(prims.size() - before) + " sprites");
    }
    list.insert(list.end(), prims.rbegin(), prims.rend());
    list.insert(list.end(), custom.begin(), custom.end());
    out.layer3dAt = list.size();
    list.insert(list.end(), late.rbegin(), late.rend());

    if (state.cursor) { // 0x8001E924 + 0x8001EA24: the arrow's tip at the item centre
        const MenuItem* at = nullptr;
        int x = kCursorStartX, y = kCursorStartY;
        bool onItem = state.cursorOnItem;
        if (state.cursorAt) {
            x = (*state.cursorAt)[0];
            y = (*state.cursorAt)[1];
        } else {
            at = page.DefaultItem();
            if (at) {
                x = at->CenterX();
                y = at->CenterY();
            } else {
                for (const MenuItem& it : page.items) // 0x8001D954: a selectable item under the cursor
                    if (MenuItemSelectable(it) && it.x0 < x && x < it.x1 && it.y0 < y && y < it.y1) {
                        at = &it;
                        break;
                    }
            }
            onItem = at != nullptr;
        }
        const MenuTableSprite s = ReadMenuTableSprite(o4, onItem ? kCursorOnItem : kCursorFree);
        MenuPrim p;
        p.x[0] = int16_t(x);
        p.y[0] = int16_t(y);
        p.w = int16_t(s.w);
        p.h = int16_t(s.h);
        p.u = s.u;
        p.v = s.v;
        p.tpage = s.tpage;
        p.clut = s.clut;
        list.push_back(p);
    }
    return out;
}

MenuRender RenderMenuPage(const MenuAssets& assets, const MenuPage& page, const MenuRenderState& state) {
    MenuRender out;
    ComposeMenuVram(assets, page, out.vram, &out.backgroundRows, &out.pageRows);
    MenuFrame frame = BuildMenuFrame(assets, page, state);
    for (const MenuPrim& p : frame.prims) out.canvas.Draw(out.vram, p);
    out.notes = std::move(frame.notes);
    return out;
}

} // namespace gt2
