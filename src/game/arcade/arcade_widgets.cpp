#include "game/arcade/arcade_widgets.h"

#include <algorithm>
#include <stdexcept>

#include "game/arcade/arcade_menus.h"

namespace gt2::arcade {

namespace {

namespace mp = menu_list_pad;

// A MIPS `div` (C division towards zero) with the right shift the compiler emits for / 16: (x + 15) >> 4 for negatives.
int Shr4(int x) { return (x < 0 ? x + 15 : x) >> 4; }

MenuPrim Quad(MenuPrim::Kind kind, const std::array<int, 4>& xs, const std::array<int, 4>& ys, const std::array<uint32_t, 4>& colours, bool semi) {
    MenuPrim p;
    p.kind = kind;
    for (size_t i = 0; i < 4; i++) {
        p.x[i] = int16_t(xs[i]);
        p.y[i] = int16_t(ys[i]);
        p.colour[i] = colours[i] & 0xFFFFFF;
    }
    p.semi = semi;
    p.gouraud = kind == MenuPrim::kPolyG4;
    return p;
}

} // namespace

// ---------------------------------------------------------------- ordering table

uint16_t ViewOt::Emit(std::vector<MenuPrim>& out, uint16_t mode) const {
    for (int s = kSlots - 1; s >= 0; s--) {
        slot[size_t(s)].Emit(out, mode);
        mode = slot[size_t(s)].FinalMode(mode);
    }
    return mode;
}

// ---------------------------------------------------------------- text

void DrawGlyph(TextCtx& c, uint32_t code, int x, int y) {
    if (!c.font || !c.ot) throw std::logic_error("arcade text: no font / slot");
    std::vector<HudFontSprite> sprites;
    c.font->Glyph(code, x, y, sprites);
    EmitGlyphSprites(c, sprites);
}

void DrawTimeRight(TextCtx& c, const std::string& s, int right, int y, int advance, int narrow, int signFlag, int dotShift) {
    std::vector<HudFontSprite> sprites;
    c.font->TimeRight(s, right, y, advance, narrow, signFlag, dotShift, sprites);
    EmitGlyphSprites(c, sprites);
}

void EmitGlyphSprites(TextCtx& c, const std::vector<HudFontSprite>& sprites) {
    if (!c.ot) throw std::logic_error("arcade text: no slot");
    for (const HudFontSprite& g : sprites) { // the glyph's packet, then the accent's (0x8007DC4C)
        const uint16_t e1 = uint16_t(g.tpage | (c.mode & 3) << 5);
        MenuPrim p;
        p.kind = MenuPrim::kSprite;
        p.x[0] = int16_t(g.x), p.y[0] = int16_t(g.y), p.w = int16_t(g.w), p.h = int16_t(g.h);
        p.u = uint8_t(g.u), p.v = uint8_t(g.v);
        p.clut = g.clut;
        p.tpage = e1;
        p.colour[0] = c.colour & 0xFFFFFF;
        p.semi = (c.colour & 0x2000000u) != 0;
        c.ot->Add(p);
        c.ot->DrawMode(e1);
    }
}

void DrawText(TextCtx& c, const std::string& s, int x, int y, int spacing) { // 0x8006ABA0
    int pen = x;
    for (size_t i = 0; i < s.size(); i++) {
        DrawGlyph(c, uint8_t(s[i]), pen, y);
        pen += c.font->Advance(uint8_t(s[i]), i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0)) + spacing;
    }
}

int TextWidth(const TextCtx& c, const std::string& s, int spacing) { return c.font->TextWidth(s, spacing); } // 0x8006AC4C

void DrawNumber(TextCtx& c, const std::string& s, int x, int y, int spacing, int digitShift, int digitExtra) { // 0x8006AE50
    int pen = x;
    const int cell = c.font->cell;
    for (size_t i = 0; i < s.size(); i++) {
        const uint8_t ch = uint8_t(s[i]);
        if (uint32_t(ch) - 0x30u < 10u) {
            DrawGlyph(c, ch | HudFont::kCentre, pen + digitShift - (cell >> 1), y);
            pen += spacing + cell + digitExtra;
        } else {
            DrawGlyph(c, ch, pen, y);
            pen += c.font->Advance(ch, i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0)) + spacing;
        }
    }
}

int NumberWidth(const TextCtx& c, const std::string& s, int spacing, int digitExtra) { return c.font->NumberWidth(s, spacing, digitExtra); } // 0x8006AF54

std::string Decimal(int value) { // 0x80011B94
    static constexpr int kPowers[8] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000};
    int i = 7;
    while (i > 0 && kPowers[i] > value) i--;
    std::string s;
    for (; i > 0; i--) {
        const int d = value / kPowers[i];
        s.push_back(char('0' + d));
        value -= d * kPowers[i];
    }
    s.push_back(char('0' + value));
    return s;
}

// ---------------------------------------------------------------- primitives

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour) {
    MenuPrim p;
    p.kind = MenuPrim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

MenuPrim Line(int x0, int y0, int x1, int y1, uint32_t colour) {
    MenuPrim p;
    p.kind = MenuPrim::kLine;
    p.x[0] = int16_t(x0), p.y[0] = int16_t(y0), p.x[1] = int16_t(x1), p.y[1] = int16_t(y1);
    p.colour[0] = p.colour[1] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

MenuPrim FlatQuad(const std::array<int, 4>& xs, const std::array<int, 4>& ys, uint32_t colour) {
    return Quad(MenuPrim::kPolyF4, xs, ys, {colour, colour, colour, colour}, (colour & 0x2000000u) != 0);
}

MenuPrim FlatTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t colour) {
    return FlatQuad({x0, x1, x2, x2}, {y0, y1, y2, y2}, colour);
}

MenuPrim Gradient(int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
    return Quad(MenuPrim::kPolyG4, {x, x + w, x, x + w}, {y, y, y + h, y + h}, {c0, c1, c0, c1}, true);
}

void AddSprite(MenuOtSlot& ot, int x, int y, int u, int v, uint16_t clut, int w, int h, uint16_t tpage, uint32_t colour) {
    MenuPrim p;
    p.kind = MenuPrim::kSprite;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.u = uint8_t(u), p.v = uint8_t(v);
    p.clut = clut;
    p.tpage = tpage;
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    ot.Add(p);
    ot.DrawMode(tpage);
}

void AddScaledSprite(MenuOtSlot& ot, int x, int y, int u, int v, uint16_t clut, int w, int h, uint16_t tpage, uint32_t colour, int scale) {
    const int16_t hw = int16_t(uint32_t(int16_t(w) * scale) >> 8), hh = int16_t(uint32_t(int16_t(h) * scale) >> 8);
    MenuPrim p;
    p.kind = MenuPrim::kPolyFT4;
    const int xs[4] = {x - hw, x + hw, x - hw, x + hw}, ys[4] = {y - hh, y - hh, y + hh, y + hh};
    const uint8_t u0 = uint8_t(u), v0 = uint8_t(v), u1 = uint8_t(u + w - 1), v1 = uint8_t(v + h - 1);
    const uint8_t us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
    for (int k = 0; k < 4; k++) p.x[k] = int16_t(int16_t(xs[k])), p.y[k] = int16_t(int16_t(ys[k])), p.tu[k] = us[k], p.tv[k] = vs[k];
    p.clut = clut;
    p.tpage = tpage;
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    ot.Add(p);
}

uint32_t LerpColour(uint32_t c0, uint32_t c1, int t, int n) { // 0x8006B458
    if (n < t) t = n;
    if (t < 0) t = 0;
    uint32_t out = 0;
    for (int k = 0; k < 3; k++) {
        const int a = int((c0 >> (8 * k)) & 0xFF), b = int((c1 >> (8 * k)) & 0xFF);
        out |= uint32_t(a + ((b - a) * t) / n) << (8 * k);
    }
    return (c0 & 0xFF000000u) | (out & 0xFFFFFF);
}

// ---------------------------------------------------------------- text object

void ArcText::Init(const ArcadeMenuAssets& a, uint32_t t, uint32_t fontWord) { // 0x80019FC8
    const GuestImage& o = a.data.ovl2;
    if (t == 0) {
        revealDivisor = 0x80, waveDivisor = 2, steps = 0x1E, period = 0x1E, extra = 1, glowSpread = 0x10, fadeSteps = 0x10, height = 0x10, flags = 0;
        font = &a.FontAt(kExeSmallFont);
        c0 = c1 = 0xFFFFFF;
    } else {
        revealDivisor = o.Get<uint8_t>(t);
        waveDivisor = o.Get<uint8_t>(t + 1);
        steps = o.Get<int16_t>(t + 2);
        glowSpread = o.Get<int16_t>(t + 4);
        period = o.Get<int16_t>(t + 6);
        fadeSteps = o.Get<int16_t>(t + 8);
        extra = o.Get<int8_t>(t + 10);
        height = o.Get<uint8_t>(t + 14);
        flags = o.Get<uint16_t>(t + 12);
        font = &a.FontAt(fontWord ? fontWord : o.Get<uint32_t>(t + 16));
        c0 = o.Get<uint32_t>(t + 20);
        c1 = o.Get<uint32_t>(t + 24);
    }
    alpha = 0x80;
    anim = -1;
}

void ArcText::Open(int settleAt) { // 0x8001A0D8
    anim = 0;
    length = int16_t(text.size());
    settle = int16_t(steps + int(uint32_t(length) / uint32_t(revealDivisor)));
    if (settleAt >= 0) settle = int16_t(settleAt);
}

void ArcText::Restart() { // 0x8001A13C
    Open(-1);
    anim = settle;
}

void ArcText::Close() { // 0x8001A170
    anim = int16_t(~steps);
    length = int16_t(text.size());
}

void ArcText::Tick() { // 0x8001A1A8
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (int(settle) + int(period) <= int(anim)) anim = settle;
}

int ArcText::Width(TextCtx& c) const { // 0x8001A9D0
    c.font = font;
    int w = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const uint8_t ch = uint8_t(text[i]);
        if (uint32_t(ch) - 0x30u < 10u || ch == 0x20) w += c.font->cell;
        else w += c.font->Advance(ch, i + 1 < text.size() ? uint8_t(text[i + 1]) : uint8_t(0));
        w += extra;
    }
    return w;
}

void ArcText::Draw(MenuOtSlot* ot, TextCtx& c) const { // 0x8001A204
    int remaining = int(anim) * int(revealDivisor);
    c.font = font;
    int px = x;
    const int py = y;
    const int cell = c.font->cell;
    if ((flags & 0x180) == 0x80) px -= Width(c) >> 1;
    else if ((flags & 0x180) == 0x100) px -= Width(c);
    const int thick = c.thickUnderline ? 2 : 1;
    if (anim < 0) {
        if (anim >= -1) return;
        const int t = steps + 1 + anim;
        const int w = Width(c);
        const int shrink = (w * t) / steps;
        const int half = int(int8_t(height)) >> 1;
        const int hh = half - (half * t) / steps;
        const uint32_t col = uint32_t(int(c0 & 0xFF) + (-int(c0 & 0xFF) * t) / steps) | uint32_t(int((c0 >> 8) & 0xFF) + (-int((c0 >> 8) & 0xFF) * t) / steps) << 8 |
                             uint32_t(int((c0 >> 16) & 0xFF) + (-int((c0 >> 16) & 0xFF) * t) / steps) << 16;
        const int spread = (glowSpread * t) / steps;
        const int top = py - half - hh;
        ot->Add(Gradient(px + shrink, top, spread + w - shrink, hh * 2, 0, col));
        ot->Add(Gradient(px - spread, top, spread + w - shrink, hh * 2, col, 0));
        ot->DrawMode(0x20);
        return;
    }
    const uint32_t semiBit = uint32_t((flags >> 4) & 2) << 24;
    for (int k = 0; k < length && size_t(k) < text.size(); k++) {
        bool glow = ((flags >> 6) & 1) != 0;
        int s1, t2;
        if (anim >= settle) {
            remaining = 1;
            s1 = steps;
            t2 = fadeSteps;
            glow = false;
            if (flags & 8) {
                t2 = (anim - settle) - k / int(waveDivisor);
                s1 = t2;
            }
        } else {
            t2 = remaining / int(revealDivisor);
            s1 = t2;
        }
        if (remaining <= 0) return;
        const int st = steps;
        if (st < s1) {
            glow = false;
            s1 = st;
        }
        if (s1 < 0) s1 = st;
        const int s0 = st - s1;
        int r, g, b;
        if (anim < settle) {
            r = int(c0 & 0xFF) + ((int(c1 & 0xFF) - int(c0 & 0xFF)) * s0) / st;
            g = int((c0 >> 8) & 0xFF) + ((int((c1 >> 8) & 0xFF) - int((c0 >> 8) & 0xFF)) * s0) / st;
            b = int((c0 >> 16) & 0xFF) + ((int((c1 >> 16) & 0xFF) - int((c0 >> 16) & 0xFF)) * s0) / st;
        } else {
            const int f = fadeSteps;
            if (f < t2) t2 = f;
            if (t2 < 0) t2 = f;
            t2 = f - t2;
            r = int(c0 & 0xFF) + ((int(c1 & 0xFF) - int(c0 & 0xFF)) * t2) / f;
            g = int((c0 >> 8) & 0xFF) + ((int((c1 >> 8) & 0xFF) - int((c0 >> 8) & 0xFF)) * t2) / f;
            b = int((c0 >> 16) & 0xFF) + ((int((c1 >> 16) & 0xFF) - int((c0 >> 16) & 0xFF)) * t2) / f;
        }
        r = (r * int(alpha)) >> 8;
        g = (g * int(alpha)) >> 8;
        b = (b * int(alpha)) >> 8;
        const uint32_t colour = uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16;
        const int8_t ch = int8_t(text[size_t(k)]);
        int adv;
        if (ch == 0x20) {
            adv = cell + extra;
        } else {
            c.ot = ot + 1;
            c.colour = colour | semiBit;
            c.mode = flags & 3;
            const uint8_t u = uint8_t(ch);
            if (flags & 0x10) {
                DrawGlyph(c, u | HudFont::kCentre, px, py);
                adv = cell;
            } else if (uint32_t(u) - 0x30u < 10u) {
                DrawGlyph(c, u | HudFont::kRightAlign, px + ((cell + extra) >> 1), py);
                adv = cell;
            } else {
                DrawGlyph(c, u, px, py);
                adv = c.font->Advance(u, size_t(k) + 1 < text.size() ? uint8_t(text[size_t(k) + 1]) : uint8_t(0));
            }
            adv += extra;
            if (glow) {
                const int gr = (r * s0) / st, gg = (g * s0) / st, gb = (b * s0) / st;
                const int off = (glowSpread * s0) / st;
                c.ot = ot;
                c.mode = 1;
                c.colour = uint32_t(gr) | uint32_t(gg) << 8 | uint32_t(gb) << 16 | 0x2000000u;
                DrawGlyph(c, u, px - off, py);
                DrawGlyph(c, u, px + off, py);
            }
        }
        if (flags & 4) ot->Add(Tile(px, py, adv, thick, colour << 1));
        px += adv;
        remaining--;
    }
}

// ---------------------------------------------------------------- button bar

void ArcButtonBar::Init(const ArcadeMenuAssets& a, uint32_t d) { // 0x8001AA98
    const GuestImage& o = a.data.ovl2;
    const uint16_t f = o.Get<uint16_t>(d + 0x0E);
    switch (f & 0xC) {
    case 8: halfWidth = 0x60, halfHeight = 0x18; break;
    case 0xC: halfWidth = 0x80, halfHeight = 0x18; break;
    default: halfWidth = 0x50, halfHeight = 0x0C; break;
    }
    const uint32_t fontWord = o.Get<uint32_t>(d + 0x14);
    const HudFont& font = a.FontAt(fontWord);
    caption.Init(a, kButtonCaptionTemplate, fontWord); // its font word is overwritten with the definition's before the copy
    caption.text = a.data.Text(o.Get<uint32_t>(d));
    for (int k = 0; k < 2; k++) {
        label[k].Init(a, 0);
        label[k].font = &font;
        label[k].flags = 0x40;
        label[k].extra = o.Get<int8_t>(d + 0x10);
        label[k].text = a.data.Text(o.Get<uint32_t>(d + 4 + uint32_t(k) * 4));
    }
    selected = o.Get<int8_t>(d + 0x0C);
    anim = -1;
    flash = 0;
    flags = f;
}

void ArcButtonBar::Open() { // 0x8001ABD8
    caption.Open(0x3C);
    label[0].Open(-1);
    label[1].Open(-1);
    anim = 0;
    flash = 0;
}

void ArcButtonBar::Close() { // 0x8001AC40
    anim = -17;
    caption.Close();
}

int ArcButtonBar::Update(const MenuListPad* pad) { // 0x8001AC68
    if (anim < 0) {
        if (anim < -1) anim++;
        caption.Tick();
        return -2;
    }
    int r = -2;
    anim++;
    if (anim > 0x47) {
        anim = 0x0C;
        flags = uint16_t(flags & 0xFF7Fu);
    }
    if (flash > 0) flash--;
    if (pad) {
        const uint32_t bits = pad->pressed;
        int sel = selected;
        if (bits & mp::kLeft) sel = 0;
        if (bits & mp::kRight) sel = 1;
        if (sel != selected) {
            r = -3;
            if (anim > 11) anim = 0x0C;
            flash = 6;
            flags |= 0x80;
        }
        selected = int8_t(sel);
        if (bits & mp::kChoose) r = sel;
        if (bits & mp::kBack) r = -1;
    }
    caption.Tick();
    label[0].Tick();
    label[1].Tick();
    return r;
}

void ArcButtonBar::Draw(MenuOtSlot* ot, TextCtx& c) const { // 0x8001ADA8
    const int captionDy = (flags & 8) ? -4 : -2;
    const int hw = halfWidth;
    ArcText cap = caption;
    if (anim < 0) {
        if (anim >= -1) return;
        const int v = 255 - Shr4((anim + 17) * 255);
        int s3 = 0, s0 = v, s4 = 0, s1 = 0;
        if (flags & 1) {
            s3 = v;
            s0 = 0;
        }
        if (selected != 0) {
            s1 = s0;
            s4 = s3;
            s3 = 0;
            s0 = 0;
        }
        const int half = hw - Shr4(hw * (anim + 17));
        ot->Add(Gradient(x - half, y, half * 2, halfHeight, uint32_t(s3) | uint32_t(s0) << 16, uint32_t(s4) | uint32_t(s1) << 16));
        ot->DrawMode(0x20);
        cap.x = int16_t(x - (cap.Width(c) >> 1));
        cap.y = int16_t(y + captionDy);
        cap.Draw(ot, c);
        return;
    }
    int s5 = 127, a2 = 0;
    if (anim < 12) {
        s5 = (anim * 127) / 12;
        a2 = (hw * 2 * (12 - anim)) / 12;
    }
    if (flags & 2) a2 = -a2;
    const uint32_t grey = uint32_t(s5) | uint32_t(s5) << 8 | uint32_t(s5) << 16;
    const int cx = x + a2, left = cx - hw, right = cx + hw, top = y, bottom = y + halfHeight;
    ot->Add(Line(left, top, right, top, grey));
    ot->Add(Line(left, bottom, right, bottom, grey));
    ot->Add(Line(left, top, left, bottom, grey));
    ot->Add(Line(cx, top, cx, bottom, grey));
    ot->Add(Line(right, top, right, bottom, grey));
    cap.x = int16_t(x - (cap.Width(c) >> 1));
    cap.y = int16_t(y + captionDy);
    cap.Draw(ot, c);
    for (int k = 0; k < 2; k++) {
        ArcText l = label[k];
        const int w = l.Width(c);
        l.alpha = uint8_t(s5);
        l.y = int16_t(bottom + captionDy);
        l.x = int16_t((k == 0 ? left : cx) + ((hw - w) >> 1));
        l.Draw(ot, c);
    }
    const int slideOff = (hw * flash) / 6;
    int fillLeft = left, fillRight = right, gx0, gx1;
    if (selected == 0) {
        fillRight = cx;
        gx0 = cx;
        gx1 = cx + slideOff;
    } else {
        fillLeft = cx;
        gx0 = cx;
        gx1 = cx - slideOff;
    }
    int blink = 0;
    if (anim >= 12) {
        blink = 60 - (anim - 12) * 4;
        if (blink < 0) blink = 0;
    }
    const int bright = (s5 * 255) >> 7;
    const int v = (((flags & 0x80) ? 255 : 128) * blink) / 60;
    uint32_t colour, edge;
    if (flags & 1) {
        colour = uint32_t(bright) | uint32_t(v) << 8 | uint32_t(v) << 16;
        edge = uint32_t(bright);
    } else {
        colour = uint32_t(v) | uint32_t(v) << 8 | uint32_t(bright) << 16;
        edge = uint32_t(bright) << 16;
    }
    MenuOtSlot* hi = ot + 1;
    if (flash > 0) hi->Add(Quad(MenuPrim::kPolyG4, {gx0, gx1, gx0, gx1}, {top, top, bottom, bottom}, {colour, edge, colour, edge}, true));
    hi->Add(FlatQuad({fillLeft, fillRight, fillLeft, fillRight}, {top, top, bottom, bottom}, colour | 0x2000000u));
    hi->DrawMode(0x20);
}

// ---------------------------------------------------------------- carousel

void ArcCarousel::Open() { // 0x8001C820
    prevGroup = -1;
    prevIndex = -1;
    anim = 0;
    slide = 0;
}

void ArcCarousel::Close() { anim = -17; } // 0x8001C864

int ArcCarousel::Update(const MenuListPad* pad) { // 0x8001C870
    if (anim < 0) {
        if (anim < -1) anim++;
        return -2;
    }
    int r = -2;
    anim++;
    if (anim > 0x38) anim = 0x0C;
    const int passes = vertical == 1 ? 2 : 1;
    for (int i = 0; i < passes; i++) {
        if (slide > 0) slide--;
        if (slide < 0) slide++;
        if (slide == 0) {
            prevGroup = -1;
            prevIndex = -1;
        }
    }
    const auto open = [&](int i) { return !available || (size_t(i) < available->size() && (*available)[size_t(i)] != 0); };
    if (available && !open(index))
        for (int i = 0; i < counts[size_t(group)]; i++)
            if (open(i)) {
                index = int16_t(i);
                break;
            }
    arrows = false;
    if (!pad) return r;
    uint32_t bits = pad->pressed;
    if (bits & mp::kBack) return -1;
    if (bits & mp::kChoose) return index;
    bits |= pad->repeat;
    const int n = counts[size_t(group)];
    if (n > 1) {
        const int old = index;
        int i = old;
        arrows = true;
        if (bits & mp::kLeft) {
            do {
                if (--i < 0) i = n - 1;
            } while (available && !open(i));
            if (i != old) {
                r = -3;
                prevGroup = group;
                prevIndex = int16_t(old);
                slide = -12;
                vertical = 0;
            }
        }
        if (bits & mp::kRight) {
            do {
                if (++i >= n) i = 0;
            } while (available && !open(i));
            if (i != old) {
                r = -3;
                prevGroup = group;
                prevIndex = int16_t(old);
                slide = 12;
                vertical = 0;
            }
        }
        index = int16_t(i);
    }
    if (groups > 1) {
        const int old = group;
        int g = old;
        if (bits & mp::kUp) {
            g = std::max(0, g - 1);
            if (g != old) {
                r = -3;
                prevGroup = int16_t(old);
                slide = -12;
                vertical = 1;
                prevIndex = index;
            }
        }
        if (bits & mp::kDown) {
            g = std::min(groups - 1, g + 1);
            if (g != group) {
                r = -3;
                slide = 12;
                prevGroup = int16_t(old);
                vertical = 1;
                prevIndex = index;
            }
        }
        group = int16_t(g);
    }
    return r;
}

void ArcCarousel::Draw(MenuOtSlot& ot, const std::function<void(const Item&)>& page) const { // 0x8001CBF4
    Item it;
    if (anim < 0) {
        if (anim >= -1) return;
        const int u = anim + 0x11;       // 1 .. 15
        const int nu = ~int(anim);       // 16 - u
        const int shrink = Shr4(w * u);
        const int halfShrink = Shr4((w >> 1) * u);
        const int hh = (h * nu < 0 ? h * nu + 15 : h * nu);
        const int grey = Shr4(nu * 0x80);
        const int left = x - (w >> 1);
        const int qy = y - (hh >> 5), qh = hh >> 4;
        const uint32_t g = uint32_t(grey) | uint32_t(grey) << 8 | uint32_t(grey) << 16;
        ot.Add(Gradient(left + shrink, qy, (w + halfShrink) - shrink, qh, 0, g));
        ot.Add(Gradient(left - halfShrink, qy, (w + halfShrink) - shrink, qh, g, 0));
        ot.DrawMode(0x20);
        it.x = x, it.y = y, it.brightness = grey, it.group = -1, it.index = -1, it.current = -1;
        page(it);
        return;
    }
    if (anim > 11 && arrows) {
        int k = 0x34 - anim;
        if (k > 10) k = 10;
        if (k < 0) k = 0;
        const uint32_t v = uint32_t((k * 0xFF) / 10);
        const uint32_t c = v | (v >> 1) << 8 | 0x2000000u;
        const int hw = (w >> 1) + 8;
        ot.Add(FlatTriangle(x + hw + 6, y, x + hw, y + 10, x + hw, y - 10, c));
        ot.Add(FlatTriangle(x - hw - 6, y, x - hw, y + 10, x - hw, y - 10, c));
        ot.DrawMode(0x20);
    }
    if (anim > 11 && groups > 1) {
        int k = 0x34 - anim;
        if (k > 10) k = 10;
        if (k < 0) k = 0;
        const uint32_t v = uint32_t((k * 0xFF) / 10);
        const uint32_t c = v | (v >> 1) << 8 | 0x2000000u;
        const int hh = (h >> 1) + 6;
        if (group > 0) ot.Add(FlatTriangle(x, y - hh - 8, x + 6, y - hh, x - 6, y - hh, c));
        if (group < groups - 1) ot.Add(FlatTriangle(x, y + hh + 8, x + 6, y + hh, x - 6, y + hh, c));
        ot.DrawMode(0x20);
    }
    const int bright = anim < 12 ? (int(anim) << 7) / 12 : 0x80;
    int t = slide;
    int dx = (w * t) / 12, dy = (h * t) / 12;
    int ox = -w, oy = -h;
    if (t < 0) {
        t = -t;
        ox = w;
        oy = h;
    }
    if (vertical == 1) dx = 0, ox = 0;
    else dy = 0, oy = 0;
    const int level = (bright * (12 - t)) / 12;
    if (prevIndex >= 0) {
        it.x = x + dx + ox, it.y = y + dy + oy, it.brightness = 0x80 - level;
        it.group = prevGroup, it.index = prevIndex, it.current = index;
        page(it);
    }
    it.x = x + dx, it.y = y + dy, it.brightness = level;
    it.group = group, it.index = index, it.current = index;
    page(it);
}

// ---------------------------------------------------------------- growing line

void ArcGrowLine::Read(const GuestImage& o, uint32_t a) {
    c0 = o.Get<uint32_t>(a);
    c1 = o.Get<uint32_t>(a + 4);
    x = o.Get<int16_t>(a + 8);
    y = o.Get<int16_t>(a + 10);
    w = o.Get<int16_t>(a + 12);
    h = o.Get<int16_t>(a + 14);
    steps = o.Get<int16_t>(a + 16);
    anim = o.Get<int16_t>(a + 18);
}

void ArcGrowLine::Tick() { // 0x8006BBC8
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (steps < anim) anim = steps;
}

void ArcGrowLine::Draw(MenuOtSlot& ot) const { // 0x8006BC18
    if (anim == -1) return;
    int t = anim, rem = steps - anim;
    if (anim < 0) {
        t = ~int(anim);
        rem = steps + 1 + anim;
    }
    const int ww = (w * t) / steps;
    ot.Add(Tile(x - (ww >> 1), y, ww, h, LerpColour(c0, c1, rem, steps)));
}

} // namespace gt2::arcade
