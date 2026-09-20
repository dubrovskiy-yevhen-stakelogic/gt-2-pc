// The executable's power / torque graph widget. See power_graph.h.
#include "gt2view/power_graph.h"

#include <cstdio>

namespace gt2::screens {

namespace {

constexpr uint32_t kWhite = 0x02DCDCDCu; // EXE 0x80092388: the reveal's start colour of the curves and value axes
constexpr uint32_t kBlack = 0x02000000u; // EXE 0x8009238C: the lerp base of the rpm axis and of the curves' alpha

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour) { // 0x8007D024: TILE 0x60
    MenuPrim p;
    p.kind = MenuPrim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

MenuPrim Line(int x0, int y0, int x1, int y1, uint32_t colour) { // 0x8007F7F4: LINE_F2 0x40
    MenuPrim p;
    p.kind = MenuPrim::kLine;
    p.x[0] = int16_t(x0), p.y[0] = int16_t(y0), p.x[1] = int16_t(x1), p.y[1] = int16_t(y1);
    p.colour[0] = p.colour[1] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

// The text context of 0x80073EDC / 0x80074274: 0x8006AC68(ctx, graph + 0xA), the font of graph + 0x10, the mode word
// masked to semi-transparency mode 1 (& 0xFF9FFFFF | 0x200000), the colour set per label (ctx + 0x14).
void AddGlyphs(MenuOtSlot& ot, const std::vector<HudFontSprite>& glyphs, uint32_t colour, int8_t page) {
    const bool semi = (colour & 0x2000000u) != 0;
    for (const HudFontSprite& g : glyphs) {
        (void)page;
        const uint16_t e1 = uint16_t(g.tpage | (semi ? 1 << 5 : 0));
        MenuPrim p;
        p.kind = MenuPrim::kSprite;
        p.x[0] = int16_t(g.x), p.y[0] = int16_t(g.y), p.w = int16_t(g.w), p.h = int16_t(g.h);
        p.u = uint8_t(g.u), p.v = uint8_t(g.v);
        p.clut = g.clut;
        p.tpage = e1;
        p.colour[0] = colour & 0xFFFFFF;
        p.semi = semi;
        ot.Add(p);
        ot.DrawMode(e1);
    }
}

std::string Decimal(int v) { // 0x8008CF34(buffer, "%d" (EXE 0x8008FB38), v)
    char b[16];
    std::snprintf(b, sizeof(b), "%d", v);
    return b;
}

int Interpolate(int x, int x0, int x1, int v0, int v1) { // 0x80074844 (a fifth argument on the stack: v1)
    if (x0 < 0) return -1;
    if (!(x0 < x)) return v0;
    if (!(x < x1)) return v1;
    return v0 + int(uint32_t(v1 - v0) * uint32_t(x - x0)) / (x1 - x0); // mult / mflo: the low word
}

// 0x80073EDC(graph, ot, which): the power (0: left edge) or torque (1: right edge) axis.
void DrawValueAxis(const PowerGraph& g, MenuOtSlot& ot, const PowerGraphText& text, int which) {
    const int h = g.h;
    const int max = which == 0 ? g.powerScale : g.torqueScale;
    const int step = which == 0 ? g.powerStep : g.torqueStep;
    const int ticks = which == 0 ? g.powerTicks : g.torqueTicks;
    const uint32_t colour = which == 0 ? g.powerColour : g.torqueColour;
    const std::string& unit = which == 0 ? text.hp : text.lbft;
    const int x = which == 0 ? g.x : g.x + g.w - 2;
    const int lh = g.labelHeight;
    const int top = g.y + (lh >> 1);
    int tickH = h / ticks - 2;
    if (tickH < 1) tickH = 1;
    int limit = g.y + h;
    int value = step;
    std::vector<HudFontSprite> glyphs;
    for (int k = 1; k <= ticks; k++) {
        int t = g.anim - (g.span * k) / ticks;
        if (t < 0) break;
        t = 8 - t;
        if (t < 0) t = 0;
        const uint32_t tick = MenuListLerp(colour, kWhite, t, 8);
        const uint32_t label = MenuListLerp(colour, kWhite, t, 0x10);
        const int ty = (g.y + h) - (h * value) / max;
        ot.Add(Tile(x, ty, 2, tickH, tick));
        const int ly = ty + (lh >> 1);
        if (ly < limit && top + lh + (lh >> 2) < ly) {
            const std::string s = Decimal(value);
            int lx = x + 4, markX = x + 2;
            if (which == 0) {
                lx = x - (text.font->NumberWidth(s, 1, 0) + 4);
                markX = x - 2;
            }
            glyphs.clear();
            text.font->Number(s, lx, ly, 0, g.digitShift, 0, glyphs);
            AddGlyphs(ot, glyphs, label, g.page);
            ot.Add(Tile(markX, ty, 2, 1, tick));
            limit = (ly - lh) - g.labelGap;
        }
        if (k == ticks) {
            int ux = x + 3;
            if (which == 0) ux = x - (text.font->TextWidth(unit, 1) + 2);
            glyphs.clear();
            text.font->Text(unit, ux, top, 1, glyphs);
            AddGlyphs(ot, glyphs, label, g.page);
        }
        value += step;
    }
}

// 0x80074274(graph, ot): the rpm axis: a tick per 1000 rpm, its number, a vertical grid line, "1000rpm".
void DrawRpmAxis(const PowerGraph& g, MenuOtSlot& ot, const PowerGraphText& text) {
    const int n = g.rpmTicks;
    int lastEnd = g.x;
    const int ly = g.y + g.h + g.labelHeight + 3;
    const int tickW = g.w / n - 1;
    std::vector<HudFontSprite> glyphs;
    for (int i = 0; i < n; i++) {
        int t = g.anim - (g.span * i) / n;
        if (t < 0) return;
        if (t > 8) t = 8;
        const uint32_t tick = MenuListLerp(kBlack, g.axisColour, t, 8);
        const uint32_t label = MenuListLerp(kBlack, g.axisColour, t, 0x10);
        const uint32_t grid = MenuListLerp(kBlack, g.axisColour, t, 0x20);
        const int xi = g.x + (g.w * i) / n;
        ot.Add(Tile(xi, g.y + g.h, tickW, 2, tick));
        const std::string s = Decimal(i);
        const int sw = text.font->TextWidth(s, g.digitShift);
        const int lx = (xi - (sw >> 1)) - 1;
        if (lastEnd < lx) {
            glyphs.clear();
            text.font->Text(s, lx, ly, g.digitShift, glyphs);
            AddGlyphs(ot, glyphs, label, g.page);
            int e = (8 - t) * (8 - t);
            if (e < 0) e += 7;
            lastEnd = lx + sw + 1;
            int d = g.h * (e >> 3);
            if (d < 0) d += 7;
            ot.Add(Line(xi, g.y + (d >> 3), xi, g.y + g.h, grid));
        }
        if (i == n - 1) {
            const int w1 = text.font->TextWidth(text.rpm1000, 1);
            glyphs.clear();
            text.font->Text(text.rpm1000, ((g.x + g.w) - w1) - 5, g.y + g.h - 2, 1, glyphs);
            AddGlyphs(ot, glyphs, label, g.page);
        }
    }
}

} // namespace

void BuildPowerGraphCurves(int count, const int16_t* rpm, const int16_t* power, const int16_t* torque, int16_t* outPower, int16_t* outTorque) {
    std::array<int, 66> r{}, p{}, q{}; // the 64-word stack arrays + the end marker
    r[0] = p[0] = q[0] = -1;
    int i = 0;
    for (; i < count && i + 2 < int(r.size()); i++) {
        r[size_t(i) + 1] = rpm[i];
        p[size_t(i) + 1] = power[i];
        q[size_t(i) + 1] = torque[i];
    }
    const int points = count > 0 ? count : 0;
    r[size_t(points) + 1] = p[size_t(points) + 1] = q[size_t(points) + 1] = -1;
    int seg = 1, x = 0, n = 0;
    do {
        if (r[size_t(seg)] < x) {
            seg++;
            if (points + 1 <= seg) break;
        } else {
            outPower[n] = int16_t(Interpolate(x, r[size_t(seg) - 1], r[size_t(seg)], p[size_t(seg) - 1], p[size_t(seg)]));
            outTorque[n] = int16_t(Interpolate(x, r[size_t(seg) - 1], r[size_t(seg)], q[size_t(seg) - 1], q[size_t(seg)]));
            n++;
            x += 250;
        }
    } while (n < 0x4F);
    for (; n < kPowerGraphSamples; n++) outPower[n] = outTorque[n] = -1;
}

PowerGraph PowerGraph::Read(const GuestImage& o, uint32_t a) {
    PowerGraph g;
    g.span = o.Get<int16_t>(a);
    g.x = o.Get<int16_t>(a + 2);
    g.y = o.Get<int16_t>(a + 4);
    g.w = o.Get<int16_t>(a + 6);
    g.h = o.Get<int16_t>(a + 8);
    g.page = o.Get<int8_t>(a + 0xA);
    g.labelGap = o.Get<int8_t>(a + 0xB);
    g.digitShift = o.Get<int8_t>(a + 0xC);
    g.labelHeight = o.Get<int8_t>(a + 0xD);
    g.font = o.Get<uint32_t>(a + 0x10);
    g.powerColour = o.Get<uint32_t>(a + 0x14);
    g.torqueColour = o.Get<uint32_t>(a + 0x18);
    g.axisColour = o.Get<uint32_t>(a + 0x1C);
    g.anim = o.Get<int16_t>(a + 0x26);
    g.animEnd = o.Get<int16_t>(a + 0x38);
    return g;
}

void PowerGraph::Reset() { // 0x80073CE4
    anim = -1;
    animEnd = int16_t(span + 0x44);
}

void PowerGraph::Open(int rpm, int power, int torque) { // 0x80073CFC
    rpmMax = int16_t(rpm), powerMax = int16_t(power), torqueMax = int16_t(torque);
    // 0x80073DA4
    int s = powerMax > 300 ? 50 : 20;
    powerStep = int16_t(s);
    powerTicks = int16_t(((powerMax * 11) / 10) / s + 1);
    powerScale = int16_t(powerTicks * s);
    s = torqueMax > 400 ? 5 : 2;
    torqueStep = int16_t(s);
    torqueTicks = int16_t(((((torqueMax * 12) / 10) * 11) / 10) / (s * 10) + 1);
    rpmTicks = int16_t((rpmMax * 11) / 10000 + 1);
    samples = int16_t(rpmTicks * 4);
    torqueScale = int16_t(torqueTicks * s);
    anim = 0;
    animEnd = int16_t(span + 0x44);
}

void PowerGraph::Close() { // 0x80073D30
    if (anim >= 0) anim = -16;
}

void PowerGraph::Tick() { // 0x80073D4C
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (animEnd < anim) anim = int16_t(span + 8);
}

void DrawPowerGraphCurve(const PowerGraph& g, MenuOtSlot& ot, const int16_t* samples, int which, int alpha) { // 0x800745B0
    if (g.anim < 0 || !samples) return;
    const int h = g.h;
    const uint32_t colour = which == 0 ? g.powerColour : g.torqueColour;
    const int scale = which == 0 ? g.powerScale : g.torqueScale * 10;
    const int n = g.samples;
    bool prevValid = false;
    int prevX = 0, prevY = 0, rpm = 0;
    for (int i = 0; i < n; i++) {
        int t = g.anim - (g.span * i) / n;
        if (t < 0) break;
        t = 8 - t;
        if (t < 0) t = 0;
        uint32_t c = MenuListLerp(colour, kWhite, t, 8);
        c = MenuListLerp(kBlack, c, alpha, 0x80);
        bool valid = prevValid;
        int yi = prevY, xi = prevX;
        if (rpm <= g.rpmMax) {
            valid = samples[i] >= 0;
            xi = g.x + (g.w * i) / n;
            yi = (g.y + h) - (h * samples[i]) / scale;
            if (i > 0 && valid && prevValid) ot.Add(Line(prevX, prevY, xi, yi, c));
        }
        rpm += 250;
        prevValid = valid, prevY = yi, prevX = xi;
    }
    ot.DrawMode(0x20);
}

void DrawPowerGraphAxes(const PowerGraph& g, MenuOtSlot& ot, const PowerGraphText& text) { // 0x800747D0
    if (g.anim == -1 || g.anim < -1 || !text.font) return;
    DrawValueAxis(g, ot, text, 0);
    DrawValueAxis(g, ot, text, 1);
    DrawRpmAxis(g, ot, text);
    ot.DrawMode(0x20);
}

} // namespace gt2::screens
