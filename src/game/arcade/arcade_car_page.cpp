#include "game/arcade/arcade_car_page.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "game/career/garage.h"
#include "game/sim/car_setup.h"
#include "gt2formats/car_info.h"
#include "gt2vfs/gtfs.h"

namespace gt2::arcade {

namespace {

namespace mp = menu_list_pad;

// Arcade v1.1 member 2 (and EXE) addresses.
constexpr uint32_t kCarouselTemplate = 0x8004FB7Cu;  // {x 0xB0, y 0x96, w 0x100, h 0x40, callback 0x8001E5F8, context}
constexpr uint32_t kRule = 0x8004FB8Cu;              // EXE widget 0x8006BC18: {c0, c1, x, y, w, h, steps, anim}
constexpr uint32_t kTransmissionBar = 0x8004FBA0u, kSettingsBar = 0x8004FBB8u;
constexpr uint32_t kPowerTextTemplate = 0x8004F460u, kBadgeTemplate = 0x8004F47Cu, kGraphTemplate = 0x8004F488u, kBarTemplate = 0x8004F4A8u;
constexpr uint32_t kSpecLabelTemplate = 0x8004F4C4u, kSpecValueTemplate = 0x8004F4E0u;
constexpr uint32_t kBadgeInitialSprite = 0x8004530Cu;   // 0x80019048
constexpr uint32_t kBadgeSprites = 0x8004F444u;         // [language]: 12-byte sprites of the drive badges
constexpr uint32_t kMakerSprites = 0x8004F5F0u;         // 12-byte sprites (u, v, clut, w, h, tpage) of arc_maker, by the class's second number
constexpr uint32_t kCentredLogoCar0 = 0x80027050u, kCentredLogoCar1 = 0x80027068u; // 0x8001E5F8: no maker logo, the name logo centred
// data-arcade.txd strings (RAM 0x800F81E0 + ...)
constexpr uint32_t kHp = 0x800F829Au, kLbFt = 0x800F829Du, kRpm1000 = 0x800F82A3u, kUnknown = 0x800F8361u;
constexpr uint32_t kBarLabels[3] = {0x800F8334u, 0x800F8347u, 0x800F8352u};
constexpr uint32_t kWeight = 0x800F836Du, kMaxPower = 0x800F837Au, kMaxTorque = 0x800F839Fu;
constexpr uint32_t kFmtWeight = 0x800F8375u, kFmtPowerRpm = 0x800F838Au, kFmtPower = 0x800F839Au, kFmtTorqueRpm = 0x800F83B1u, kFmtTorqueAbout = 0x800F83C4u;
constexpr uint32_t kFmtTorque = 0x80026F2Cu, kNoTorque = 0x80026F24u; // member-2 image strings
constexpr int kPageY[5] = {0x1A4, 0xF0, 0x194, 0x174, 0x194};           // 0x8001E5F8: the widgets' y
constexpr int kPageDx[5] = {0, 0x58, -0x60, -0x84, 0xA0};              // ... and their x offsets from the page centre

// The EXE's sprintf (0x8008CE44) for the formats of the car page: %d and %s only.
struct FmtArg {
    bool isText = false;
    int value = 0;
    std::string text;
};
std::string Format(const std::string& fmt, std::initializer_list<FmtArg> args) {
    std::string out;
    auto it = args.begin();
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%' || i + 1 >= fmt.size()) {
            out.push_back(fmt[i]);
            continue;
        }
        const char k = fmt[++i];
        if (k == '%') {
            out.push_back('%');
            continue;
        }
        if (it == args.end()) throw std::logic_error("arcade car page: format " + fmt + " needs more arguments");
        if (k == 'd' && !it->isText) out += std::to_string(it->value);
        else if (k == 's' && it->isText) out += it->text;
        else throw std::logic_error("arcade car page: format " + fmt + " is not ported");
        ++it;
    }
    return out;
}
FmtArg D(int v) { return {false, v, {}}; }
FmtArg S(std::string s) { return {true, 0, std::move(s)}; }

uint16_t U16(std::span<const uint8_t> b, size_t o) { return uint16_t(b[o] | b[o + 1] << 8); }
int16_t S16(std::span<const uint8_t> b, size_t o) { return int16_t(U16(b, o)); }
uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(U16(b, o)) | uint32_t(U16(b, o + 2)) << 16; }

// 0x800194A4(x, x0, x1, v0, v1).
int Interpolate(int x, int x0, int x1, int v0, int v1) {
    if (x0 < 0) return -1;
    if (x <= x0) return v0;
    if (x < x1) return v0 + ((v1 - v0) * (x - x0)) / (x1 - x0);
    return v1;
}

PanelSprite ReadSprite(const GuestImage& o, uint32_t a) {
    PanelSprite s;
    s.u = o.Get<uint8_t>(a);
    s.v = o.Get<uint8_t>(a + 1);
    s.clut = o.Get<uint16_t>(a + 2);
    s.w = o.Get<uint16_t>(a + 4);
    s.h = o.Get<uint16_t>(a + 6);
    s.tpage = o.Get<uint16_t>(a + 8);
    return s;
}

// 0x8001E520: the car camera's reset.
menu::OverlayModelCamera CarCamera() {
    menu::OverlayModelCamera c;
    c.position = {0, 0, 0x94CCC};
    c.pitch = 0x5E;
    c.yaw = 0x1500;
    c.roll = 0;
    c.x = 0, c.y = 0, c.w = 0x100, c.h = 0xF0;
    c.left = -0x80, c.right = 0x80, c.top = 0x5A, c.bottom = -0x32, c.distance = 400, c.farZ = 0x7FFF;
    c.floor = true;
    c.floorSemi = true;
    c.floorColour = 0xA2A2A2;
    return c;
}
constexpr int16_t kCarEnvX = 0x30, kCarEnvY = 0xB4, kCarEnvW = 0x100, kCarEnvH = 200; // 0x8001F124
constexpr int32_t kFrameLength = 0x117; // view + 0x218 = u32 0x801F0088 in the captures (<= 0xFB90: no zoom step)

// Channel ramps of the widgets: c + (255 - c) * e / d.
int Whiten(int c, int e, int d) { return c + ((255 - c) * e) / d; }

} // namespace

// ---------------------------------------------------------------- graph

void ArcGraph::Init(const ArcadeMenuAssets& a, uint32_t t) { // 0x800121C4
    const GuestImage& o = a.data.ovl2;
    w = o.Get<int16_t>(t);
    h = o.Get<int16_t>(t + 2);
    font = &a.FontAt(o.Get<uint32_t>(t + 0x1C));
    labelGap = o.Get<int8_t>(t + 4);
    digitShift = o.Get<int8_t>(t + 5);
    labelSize = o.Get<int8_t>(t + 6);
    maxRpm = o.Get<int16_t>(t + 8);
    maxHp = o.Get<int16_t>(t + 10);
    maxTorque = o.Get<int16_t>(t + 12);
    power = o.Get<uint32_t>(t + 16);
    torque = o.Get<uint32_t>(t + 20);
    grid = o.Get<uint32_t>(t + 24);
    mode = 1;
    reveal = 0x14;
    anim = -1;
    animMax = 0x5F;
}

void ArcGraph::Open() { // 0x80012288 + 0x8001234C
    hpStep = maxHp > 300 ? 50 : 20;
    hpTicks = int16_t(((maxHp * 11) / 10) / hpStep + 1);
    hpMax = int16_t(hpTicks * hpStep);
    tqStep = maxTorque > 400 ? 5 : 2;
    tqTicks = int16_t(((((maxTorque * 12) / 10) * 11) / 10) / (tqStep * 10) + 1);
    tqMax = int16_t(tqTicks * tqStep);
    rpmTicks = int16_t((maxRpm * 11) / 10000 + 1);
    samples = int16_t(rpmTicks * 4);
    anim = 0;
    animMax = int16_t(reveal + 0x4B);
}

void ArcGraph::Tick() { // 0x800122EC
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (animMax < anim) anim = int16_t(reveal + 0x0F);
}

void ArcGraph::Axis(MenuOtSlot* ot, TextCtx& c, bool tq) const { // 0x80012484
    const uint32_t col = tq ? torque : power;
    const int step = tq ? tqStep : hpStep, ticks = tq ? tqTicks : hpTicks, axisMax = tq ? tqMax : hpMax;
    const int ax = tq ? x + w - 2 : x;
    c.ot = ot;
    c.font = font;
    c.mode = mode & 3;
    const int top = y + (labelSize >> 1);
    int tickH = h / ticks - 2;
    if (tickH < 1) tickH = 1;
    int limit = y + h;
    int value = step;
    const int cr = int(col & 0xFF), cg = int((col >> 8) & 0xFF), cb = int((col >> 16) & 0xFF);
    for (int k = 1; k <= ticks; k++) {
        const int t = anim - (reveal * k) / ticks;
        if (t < 0) break;
        int e = 15 - t;
        if (e < 0) e = 0;
        const int r = Whiten(cr, e, 15), g = Whiten(cg, e, 15), b = Whiten(cb, e, 15);
        const uint32_t full = uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | 0x2000000u;
        const uint32_t half = uint32_t(r >> 1) | uint32_t(g >> 1) << 8 | uint32_t(b >> 1) << 16 | 0x2000000u;
        const int ty = y + h - (h * value) / axisMax;
        ot->Add(Tile(ax, ty, 2, tickH, full));
        const int ly = ty + (labelSize >> 1);
        if (ly < limit && top + labelSize + (labelSize >> 2) < ly) {
            const std::string s = Decimal(value);
            int lx, mx;
            if (!tq) {
                lx = ax - (NumberWidth(c, s, 1, 0) + 4);
                mx = ax - 2;
            } else {
                lx = ax + 4;
                mx = ax + 2;
            }
            c.colour = half;
            DrawNumber(c, s, lx, ly, 1, digitShift, 0);
            ot->Add(Tile(mx, ty, 2, 1, full));
            limit = (ly - labelSize) - labelGap;
        }
        if (k == ticks) {
            const std::string& u = tq ? unitTorque : unitPower;
            const int ux = tq ? ax + 3 : ax - (TextWidth(c, u, 1) + 2);
            c.colour = half;
            DrawText(c, u, ux, top, 1);
        }
        value += step;
    }
}

void ArcGraph::RpmAxis(MenuOtSlot* ot, TextCtx& c) const { // 0x8001292C
    const int n = rpmTicks;
    c.ot = ot;
    c.font = font;
    c.mode = mode & 3;
    int lastEnd = x;
    const int ly = y + h + labelSize + 3;
    const int tickW = w / n - 1;
    const int cr = int(grid & 0xFF), cg = int((grid >> 8) & 0xFF), cb = int((grid >> 16) & 0xFF);
    for (int i = 0; i < n; i++) {
        int t = anim - (reveal * i) / n;
        if (t < 0) return;
        if (t > 15) t = 15;
        const int r = (cr * t) / 15, g = (cg * t) / 15, b = (cb * t) / 15;
        const uint32_t half = uint32_t(r >> 1) | uint32_t(g >> 1) << 8 | uint32_t(b >> 1) << 16 | 0x2000000u;
        const uint32_t quarter = uint32_t(r >> 2) | uint32_t(g >> 2) << 8 | uint32_t(b >> 2) << 16 | 0x2000000u;
        const int xi = x + (w * i) / n;
        ot->Add(Tile(xi, y + h, tickW, 2, uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16));
        const std::string s = Decimal(i);
        const int sw = TextWidth(c, s, digitShift);
        const int lx = (xi - (sw >> 1)) - 1;
        if (lastEnd < lx) {
            c.colour = half;
            DrawText(c, s, lx, ly, digitShift);
            lastEnd = lx + sw + 1;
            ot->Add(Line(xi, y + (h * (((15 - t) * (15 - t)) / 15)) / 15, xi, y + h, quarter));
        }
        if (i == n - 1) {
            c.colour = half;
            const int w1 = TextWidth(c, unitRpm, 1);
            DrawText(c, unitRpm, ((x + w) - w1) - 5, y + h - 2, 1);
        }
    }
}

void ArcGraph::Curve(MenuOtSlot* ot, bool tq) const { // 0x80012D1C
    const uint32_t col = tq ? torque : power;
    const int scale = tq ? tqMax * 10 : hpMax;
    const std::array<int16_t, 80>& v = tq ? torqueSamples : powerSamples;
    const int cr = int(col & 0xFF), cg = int((col >> 8) & 0xFF), cb = int((col >> 16) & 0xFF);
    int prevX = 0, prevY = 0;
    bool prevValid = false;
    int rpm = 0;
    for (int i = 0; i < samples; i++) {
        const int t = anim - (reveal * i) / samples;
        int e = 15 - t;
        if (t < 0) return;
        if (e < 0) e = 0;
        int cx = prevX, cy = prevY;
        bool valid = prevValid;
        if (rpm <= maxRpm) {
            const int s = i < int(v.size()) ? v[size_t(i)] : -1;
            valid = s >= 0;
            cx = x + (w * i) / samples;
            cy = (y + h) - (((h * s) / scale) * (((15 - e) * (15 - e)) / 15)) / 15;
            if (i > 0 && valid && prevValid)
                ot->Add(Line(prevX, prevY, cx, cy, uint32_t(Whiten(cr, e, 15)) | uint32_t(Whiten(cg, e, 15)) << 8 | uint32_t(Whiten(cb, e, 15)) << 16));
        }
        rpm += 250;
        prevX = cx, prevY = cy, prevValid = valid;
    }
}

void ArcGraph::Draw(MenuOtSlot* ot, TextCtx& c) const { // 0x80013034
    if (anim == -1) return;
    if (anim < -1) {
        const int u = ~int(anim);
        const int half = (h + labelSize) >> 1;
        const int ww = (w * (15 - u)) / 15;
        const int dh = (half * u) / 15;
        const int grey = (u * 0xC0) / 15;
        const uint32_t g = uint32_t(grey) | uint32_t(grey) << 8 | uint32_t(grey) << 16;
        const int qy = (y + half) - dh, qw = (w + ww) - (ww >> 1);
        ot->Add(Gradient(x + (ww >> 1), qy, qw, dh * 2, 0, g));
        ot->Add(Gradient(x - ww, qy, qw, dh * 2, g, 0));
        ot->DrawMode(0x20);
        return;
    }
    Axis(ot, c, false);
    Axis(ot, c, true);
    RpmAxis(ot, c);
    Curve(ot, false);
    Curve(ot, true);
    ot->DrawMode(mode);
}

// ---------------------------------------------------------------- bars

void ArcBar::Tick() { // 0x800185D4
    label.Tick();
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (anim > 0x20) anim = 0x20;
}

void ArcBar::Draw(MenuOtSlot* ot, int x, int y, TextCtx& c, int value) const { // 0x8001863C
    ArcText l = label;
    l.x = int16_t(x - 4);
    l.y = int16_t(y);
    l.Draw(ot, c);
    if (anim < 0) return;
    if (value < 0) {
        int e = 8 - anim;
        if (e < 0) e = 0;
        c.font = &unknownFont->FontAt(0x80129500u);
        c.colour = uint32_t(((e * 0x90) >> 3) + 0x6F) | uint32_t(((e * 0xB4) >> 3) + 0x4B) << 8 | uint32_t(((e * 0xD2) >> 3) + 0x2D) << 16 | 0x2000000u;
        c.ot = ot;
        c.mode = 1;
        DrawText(c, unknownText, x, y, 1);
    }
    for (int k = 0; k < 10; k++) {
        const int sx = x + k * 5;
        int t = anim - (k * 24) / 10;
        if (t >= 0) {
            t = 8 - t;
            if (t < 0) t = 0;
            int br = 0xDE, bg = 0x6F, bb = 0x10;
            if (k >= 8) br = 0xF2, bg = 0x0B, bb = 0x0B;
            int e = t;
            if (value <= k) br = bg = bb = e = 0;
            const int r = br + (((255 - br) * e) >> 3), g = bg + (((255 - bg) * e) >> 3), b = bb + (((255 - bb) * e) >> 3);
            ot->Add(Tile(sx, y - 12, 4, 12, uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16 | 0x2000000u));
            ot->DrawMode(0x20);
        }
        ot->Add(Tile(sx, y - 12, 4, 12, 0x2081020u));
        ot->DrawMode(0);
    }
}

// ---------------------------------------------------------------- chips

void ArcChips::Set(int n, const std::vector<uint16_t>& chips) { // 0x80018240
    if (n <= 0 || chips.empty()) return;
    anim = 0;
    count = int16_t(n);
    for (int i = 0; i < n && i < int(colours.size()) && size_t(i) < chips.size(); i++) colours[size_t(i)] = chips[size_t(i)];
}

void ArcChips::Tick() { // 0x80018290
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (anim > 0x20) anim = 0x20;
}

void ArcChips::Draw(MenuOtSlot& ot, int x, int y) const { // 0x800182E0
    if (anim < 0) return;
    int py = y - count * 10;
    for (int k = 0; k < count; k++) {
        int t = anim - (k * 24) / count;
        if (t >= 0) {
            t = 8 - t;
            if (t < 0) t = 0;
            const uint32_t c = colours[size_t(k)];
            // the original's channel decode: r = bits 0..4 << 3, g = bits 5..9 << 3, b = bits 11..15 << 4 (0xF800 >> 7)
            const int r0 = int(c & 0x1F) * 8, g0 = int((c & 0x3E0) >> 2), b0 = int((c & 0xF800) >> 7);
            const int r = r0 + (((255 - r0) * t) >> 3), g = g0 + (((255 - g0) * t) >> 3), b = b0 + (((255 - b0) * t) >> 3);
            const uint32_t full = uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16;
            const uint32_t dark = uint32_t(r >> 2) | uint32_t(g >> 2) << 8 | uint32_t(b >> 2) << 16;
            ot.Add(Gradient(x, py, 8, 6, full, dark));
            ot.Add(Tile(x, py, 8, 6, 0));
            const uint32_t grey = k == selected ? 0x80u : 0x28u;
            const uint32_t border = grey | grey << 8 | grey << 16;
            // POLY_F polyline 0x48 (0x8007E648): (x-1, y-1) (x+8, y-1) (x+8, y+6) (x-1, y+6) (x-1, y-1), one packet: its
            // segments are added last to first so that the GPU walks them in order.
            ot.Add(Line(x - 1, py + 6, x - 1, py - 1, border));
            ot.Add(Line(x + 8, py + 6, x - 1, py + 6, border));
            ot.Add(Line(x + 8, py - 1, x + 8, py + 6, border));
            ot.Add(Line(x - 1, py - 1, x + 8, py - 1, border));
        }
        py += 10;
    }
    ot.DrawMode(0x20);
}

// ---------------------------------------------------------------- spec box

void ArcSpecBox::Init(const ArcadeMenuAssets& a) { // 0x80018930
    static constexpr uint32_t kLabelColours[3] = {0xC0C0C0u, 0x3E8EDEu, 0xF08E3Cu}; // written to the template's c0 (0x8004F4D8)
    static constexpr uint32_t kLabels[3] = {kWeight, kMaxPower, kMaxTorque};
    for (int k = 0; k < 3; k++) {
        ArcText& l = text[size_t(k) * 2];
        l.Init(a, kSpecLabelTemplate);
        l.c0 = kLabelColours[k];
        l.text = a.data.Text(kLabels[k]);
        ArcText& v = text[size_t(k) * 2 + 1];
        v.Init(a, kSpecValueTemplate);
        v.text = l.text;
    }
    anim = -1;
}

void ArcSpecBox::Fill(const ArcadeMenuAssets& a, const std::array<int16_t, 5>& f, TextCtx& c) { // 0x80018A3C
    text[0].text = a.data.Text(kWeight);
    text[0].Open(-1);
    text[1].text = Format(a.data.Text(kFmtWeight), {D((f[4] * 0x561E) / 10000)});
    text[1].Open(-1);
    text[2].text = a.data.Text(kMaxPower);
    text[2].Open(-1);
    const int hp = (f[0] * 1000) / 0x3F6;
    text[3].text = f[1] == 0 ? Format(a.data.Text(kFmtPower), {D(hp)}) : Format(a.data.Text(kFmtPowerRpm), {D(hp), D(f[1])});
    text[3].Open(-1);
    text[4].text = a.data.Text(kMaxTorque);
    text[4].Open(-1);
    const int t = f[2];
    if (t == 0) {
        text[5].text = a.data.ImageString(kNoTorque);
    } else {
        const int v = (t * 0x11A89) / 10000 - t / 0x73FB;
        const int rpm = f[3];
        if (rpm < 0) text[5].text = Format(a.data.Text(kFmtTorqueAbout), {D((v - v % 10) / 10), D(v % 10), D(-rpm)});
        else if (rpm != 0) text[5].text = Format(a.data.Text(kFmtTorqueRpm), {D((v - v % 10) / 10), D(v % 10), D(rpm)});
        else text[5].text = Format(a.data.ImageString(kFmtTorque), {D(v / 10), D(v % 10), S(a.data.Text(kLbFt))});
    }
    text[5].Open(-1);
    labelWidth = int16_t(std::max({text[0].Width(c), text[2].Width(c), text[4].Width(c)}));
    valueWidth = int16_t(std::max({text[1].Width(c), text[3].Width(c), text[5].Width(c)}));
    anim = 0;
}

void ArcSpecBox::Reset() { // 0x80018DE0
    for (ArcText& t : text) t.anim = -1;
    anim = -1;
}

void ArcSpecBox::Close() { // 0x80018E04
    if (anim < 0) return;
    for (ArcText& t : text) t.Close();
    anim = -1;
}

void ArcSpecBox::Tick() { // 0x80018E6C
    for (ArcText& t : text) t.Tick();
}

void ArcSpecBox::Draw(MenuOtSlot* ot, int x, int y, TextCtx& c) const { // 0x80018EB8
    const int px = x - valueWidth;
    for (int k = 0; k < 3; k++) {
        ArcText l = text[size_t(k) * 2], v = text[size_t(k) * 2 + 1];
        l.x = int16_t(px - 3), l.y = int16_t(y + 16 * k);
        l.Draw(ot, c);
        v.x = int16_t(px + 2), v.y = int16_t(y + 16 * k);
        v.Draw(ot, c);
    }
    if (anim < 0) return;
    ot->Add(Line((px - labelWidth) - 5, y - 15, valueWidth + px + 4, y - 15, 0x3E3E3E));
    ot->Add(Line((px - labelWidth) - 5, y + 0x24, valueWidth + px + 4, y + 0x24, 0x3E3E3E));
}

// ---------------------------------------------------------------- the widget object

void ArcCarSpecs::Init(const ArcadeMenuAssets& a) { // 0x80019048
    for (Copy& k : copy_) {
        k.filled = false;
        k.power.Init(a, kPowerTextTemplate);
        k.power.text.clear();
        const GuestImage& o = a.data.ovl2;
        PanelItem& b = k.badge; // 0x8001C0FC with the template 0x8004F47C
        b.flags = o.Get<uint8_t>(kBadgeTemplate);
        b.pulse = o.Get<int16_t>(kBadgeTemplate + 2);
        b.steps = o.Get<int16_t>(kBadgeTemplate + 4);
        b.brightness = o.Get<uint8_t>(kBadgeTemplate + 6);
        b.target = o.Get<uint8_t>(kBadgeTemplate + 7);
        b.spread = o.Get<int16_t>(kBadgeTemplate + 8);
        b.scale = o.Get<int16_t>(kBadgeTemplate + 10);
        b.anim = -1;
        b.sprite = ReadSprite(o, kBadgeInitialSprite);
        k.graph.Init(a, kGraphTemplate);
        k.graph.unitPower = a.data.Text(kHp);
        k.graph.unitTorque = a.data.Text(kLbFt);
        k.graph.unitRpm = a.data.Text(kRpm1000);
        for (int i = 0; i < 3; i++) {
            ArcBar& bar = k.bars[size_t(i)];
            bar.anim = -1;
            bar.label.Init(a, kBarTemplate);
            bar.label.text = a.data.Text(kBarLabels[i]);
            bar.unknownFont = &a;
            bar.unknownText = a.data.Text(kUnknown);
        }
        k.chips.anim = -1;
        k.chips.selected = 0;
        k.spec.Init(a);
    }
    current_ = 0;
}

void ArcCarSpecs::Fill(const ArcadeMenuAssets& a, const ArcCarFigures& f, uint8_t language, TextCtx& c) { // 0x800194FC
    Copy& k = copy_[size_t(current_)];
    const GuestImage& o = a.data.ovl2;
    k.power.text = Decimal((f.figures[0] * 1000) / 0x3F6) + a.data.Text(kHp);
    k.power.Open(-1);
    k.badge.sprite = ReadSprite(o, o.Get<uint32_t>(kBadgeSprites + uint32_t(language) * 4) + uint32_t(f.badge) * 12);
    k.badge.anim = 0; // 0x8001C194
    k.values = f.bars;
    for (ArcBar& b : k.bars) b.Open();
    k.chips.Set(int(f.chips.size()), f.chips);
    k.spec.Fill(a, f.figures, c);
    ArcGraph& g = k.graph;
    if (!f.curve) {
        g.anim = -1; // 0x800122C4
    } else {
        const std::span<const uint8_t> pc(f.powerCurve);
        const int count = U16(pc, 0x0A);
        g.maxRpm = S16(pc, 0x0C + size_t(std::max(0, count - 1)) * 2);
        g.maxHp = int16_t((f.figures[0] * 1000) / 0x3F6);
        g.maxTorque = int16_t((f.figures[2] * 0x11A89) / 10000);
        g.Open();
        std::vector<int> rpm(size_t(count) + 2, -1), pw(size_t(count) + 2, -1), tq(size_t(count) + 2, -1);
        const int maxPower = U16(pc, 0), maxTorque = U16(pc, 4);
        for (int i = 1; i <= count; i++) {
            rpm[size_t(i)] = S16(pc, 0x0C + size_t(i - 1) * 2);
            pw[size_t(i)] = (((S16(pc, 0x2C + size_t(i - 1) * 2) * f.figures[0]) / maxPower) * 1000) / 0x3F6;
            tq[size_t(i)] = (((S16(pc, 0x4C + size_t(i - 1) * 2) * f.figures[2]) / maxTorque) * 0x11A89) / 10000;
        }
        int seg = 1, r = 0, n = 0;
        do {
            if (rpm[size_t(seg)] < r) {
                seg++;
                if (count + 1 <= seg) break;
            } else {
                g.powerSamples[size_t(n)] = int16_t(Interpolate(r, rpm[size_t(seg - 1)], rpm[size_t(seg)], pw[size_t(seg - 1)], pw[size_t(seg)]));
                g.torqueSamples[size_t(n)] = int16_t(Interpolate(r, rpm[size_t(seg - 1)], rpm[size_t(seg)], tq[size_t(seg - 1)], tq[size_t(seg)]));
                n++;
                r += 250;
            }
        } while (n < 0x4F);
        for (; n < 0x50; n++) g.powerSamples[size_t(n)] = g.torqueSamples[size_t(n)] = -1;
    }
    k.filled = true;
}

void ArcCarSpecs::CloseCurrent() { // 0x800191F4
    Copy& k = copy_[size_t(current_)];
    if (!k.filled) return;
    k.power.Close();
    k.badge.anim = int16_t(~k.badge.steps); // 0x8001C1A8
    k.graph.Close();
    for (ArcBar& b : k.bars) b.Close();
    k.chips.Close();
    k.spec.Close();
}

void ArcCarSpecs::Swap() { // 0x80019330
    if (copy_[size_t(current_)].filled) {
        CloseCurrent();
        copy_[size_t(current_)].filled = false;
    }
    current_ = (current_ + 1) & 1;
    Copy& k = copy_[size_t(current_)];
    k.power.anim = -1;
    k.badge.anim = -1;
    k.spec.Reset();
    k.filled = false;
}

void ArcCarSpecs::Tick() { // 0x80019A80
    for (Copy& k : copy_) {
        k.power.Tick();
        PanelItemTick(k.badge);
        k.graph.Tick();
        for (ArcBar& b : k.bars) b.Tick();
        k.chips.Tick();
        k.spec.Tick();
    }
}

void ArcCarSpecs::Draw(int which, bool current, MenuOtSlot* ot, int x, int y, TextCtx& c) const { // 0x80019B64
    for (int i = 0; i < 2; i++) {
        bool draw = i == current_;
        if (!current) draw = !draw;
        if (!draw) continue;
        const Copy& k = copy_[size_t(i)];
        switch (which) {
        case 0: {
            ArcText p = k.power;
            p.x = int16_t(x + 1);
            p.y = int16_t(y + 12);
            p.Draw(ot, c);
            PanelItem b = k.badge;
            b.x = int16_t((x - (int16_t(b.sprite.w) >> 1)) - 1);
            b.y = int16_t(y);
            PanelItemDraw(b, *ot);
            break;
        }
        case 1: {
            ArcGraph g = k.graph;
            g.x = int16_t(x);
            g.y = int16_t(y);
            g.Draw(ot, c);
            break;
        }
        case 2:
            for (int b = 0; b < 3; b++) k.bars[size_t(b)].Draw(ot, x, y + 16 * b, c, int8_t(k.values[size_t(b)]));
            break;
        case 3: k.chips.Draw(*ot, x, y); break;
        case 4: k.spec.Draw(ot, x, y, c); break;
        default: break;
        }
    }
}

// ---------------------------------------------------------------- name logo

void ArcNameLogo::Set(const ArcadeMenuAssets& a, MenuVram& vram, int model, uint32_t carId) { // 0x80016ECC + 0x80016CA4
    slot = (slot + 1) & 1;
    if (loaded[size_t(slot)] && ids[size_t(slot)] == carId) return;
    loaded[size_t(slot)] = false;
    ids[size_t(slot)] = carId;
    std::vector<uint8_t> file;
    std::span<const uint8_t> tim;
    if (model < 0) {
        // 0x80016FB4: VOL file (the car directory entry's logo file + 1) of the car. The boot's car directory (0x801DF030:
        // {u32 id, u16 .cdo file, u16 logo file}) names <id>n--.tim, else <id>l--.tim, else carlogo's first file (1110 of 1110
        // entries of work/re/arcade_menu/ram.bin), so the menus show the file behind it: <id>o--, <id>m-- or a-a7rm--.
        if (!a.vol) throw std::logic_error("arcade car page: no volume for the VOL name logos");
        const std::string id = UnpackCarId(carId);
        const GtfsEntry* named = nullptr;
        for (const char* kind : {"n", "l"}) {
            const std::string path = "carlogo/" + id + kind + "--.tim";
            named = a.vol->Find(path);
            if (!named) named = a.vol->Find(path + ".gz");
            if (named) break;
        }
        if (!named)
            for (const GtfsEntry& f : a.vol->Files())
                if (f.path.rfind("carlogo/", 0) == 0 && (!named || f.index < named->index)) named = &f;
        if (!named) throw std::runtime_error("arcade car page: no carlogo/ files");
        const GtfsEntry* next = nullptr;
        for (const GtfsEntry& f : a.vol->Files())
            if (f.index == named->index + 1) next = &f;
        if (!next) throw std::runtime_error("arcade car page: no VOL file behind " + named->path);
        file = a.vol->Read(*next);
        tim = file;
    } else {
        const std::span<const uint8_t> c(a.carLogos);
        if (c.size() < 8 || uint32_t(model) >= U32(c, 0)) throw std::runtime_error("arcade/arc_carlogo: no entry " + std::to_string(model));
        const size_t at = U32(c, 4 + size_t(model) * 4);
        tim = c.subspan(at);
    }
    if (U32(tim, 0) != 0x10 || (U32(tim, 4) & 8) == 0) throw std::runtime_error("arcade/arc_carlogo: entry is not a TIM with a CLUT");
    const int px = (tpage & 0xF) * 64, py = (tpage & 0x10) * 16 + slot * 128;
    const size_t clutLen = U32(tim, 8);
    const int cw = U16(tim, 16), ch = U16(tim, 18);
    vram.Upload(px, py, cw, ch, tim.subspan(20, size_t(cw) * ch * 2));
    const size_t img = 8 + clutLen;
    const int iw = U16(tim, img + 8), ih = U16(tim, img + 10);
    vram.Upload(px, py + 1, iw, ih, tim.subspan(img + 12, size_t(iw) * ih * 2));
    w[size_t(slot)] = int16_t(iw * 4); // 4-bit
    h[size_t(slot)] = int16_t(ih);
    loaded[size_t(slot)] = true;
    uploads++;
}

void ArcNameLogo::Draw(MenuOtSlot& ot, uint32_t carId, int x, int y, int brightness, bool centred, int scale) const { // 0x8001713C
    int i = 0;
    for (; i < 2; i++)
        if (ids[size_t(i)] == carId && loaded[size_t(i)]) break;
    if (i >= 2) return;
    const uint16_t clut = uint16_t(((tpage & 0x10) << 10) | ((tpage & 0xF) * 4 + i * 0x2000));
    if (scale > 0) { // 0x8007E774: POLY_FT4 (x -+ w * scale >> 8; not centred: y + 2 .. y + (h * scale >> 7) + 2)
        const int16_t hw = int16_t(uint32_t(w[size_t(i)] * scale) >> 8);
        int y0 = y + 2, y1 = y + int16_t(int16_t(int16_t(h[size_t(i)]) * scale >> 7) + 2);
        if (centred) {
            const int16_t hh = int16_t(uint32_t(h[size_t(i)] * scale) >> 8);
            y0 = y - hh, y1 = y + hh;
        }
        MenuPrim p;
        p.kind = MenuPrim::kPolyFT4;
        const int xs[4] = {x - hw, x + hw, x - hw, x + hw}, ys[4] = {y0, y0, y1, y1};
        const uint8_t v0 = uint8_t(i * 0x80 + 1), v1 = uint8_t(h[size_t(i)] + i * 0x80), u1 = uint8_t(w[size_t(i)] - 1);
        const uint8_t us[4] = {0, u1, 0, u1}, vs[4] = {v0, v0, v1, v1};
        for (int k = 0; k < 4; k++) p.x[k] = int16_t(xs[k]), p.y[k] = int16_t(ys[k]), p.tu[k] = us[k], p.tv[k] = vs[k];
        p.tpage = tpage;
        p.clut = clut;
        p.colour[0] = uint32_t(brightness) | uint32_t(brightness) << 8 | uint32_t(brightness) << 16;
        ot.Add(p);
        return;
    }
    const int sx = x - (w[size_t(i)] >> 1), sy = centred ? y - (h[size_t(i)] >> 1) : y + 2;
    const uint32_t g = uint32_t(brightness) | uint32_t(brightness) << 8 | uint32_t(brightness) << 16;
    AddSprite(ot, sx, sy, 0, uint8_t(i * 128 + 1), clut, w[size_t(i)], h[size_t(i)], tpage, g);
}

// ---------------------------------------------------------------- the page

ArcadeCarPage::ArcadeCarPage(const ArcadeMenuAssets& a, const ArcadeData& data, const CarInfoDirectory& cars) : a_(a), data_(data), cars_(cars) {
    const GuestImage& o = a.data.ovl2;
    carousel_.x = o.Get<int16_t>(kCarouselTemplate);
    carousel_.y = o.Get<int16_t>(kCarouselTemplate + 2);
    carousel_.w = o.Get<int16_t>(kCarouselTemplate + 4);
    carousel_.h = o.Get<int16_t>(kCarouselTemplate + 6);
    rule_.Read(o, kRule);
    specs_.Init(a);
    transmission_.Init(a, kTransmissionBar);
    settings_.Init(a, kSettingsBar);
}

const ArcadeClassCars& ArcadeCarPage::Class() const { return a_.data.classes.at(size_t(std::clamp(cls_, 0, 6))); }

ArcCarFigures ClassCarFigures(const ArcadeMenuAssets& a, const ArcadeData& data_, const CarInfoDirectory& cars_, int cls, int index) {
    const ArcadeClassCars& cc = a.data.classes.at(size_t(std::clamp(cls, 0, 6)));
    ArcCarFigures f;
    const uint32_t carId = PackCarId(cc.cars.at(size_t(index)));
    const int32_t info = cars_.IndexOf(carId); // 0x80060998 (record 0 when absent)
    const CarInfoRecord& r = cars_.At(info < 0 ? 0 : size_t(info));
    f.chips.assign(r.chipColors.begin(), r.chipColors.begin() + std::ptrdiff_t(std::min(r.PaintCount(), r.chipColors.size())));
    f.bars = cc.bars.at(size_t(index));
    const std::array<uint8_t, 10>& fig = cc.figures.at(size_t(index));
    for (size_t k = 0; k < 5; k++) f.figures[k] = int16_t(fig[k * 2] | fig[k * 2 + 1] << 8);
    // 0x80076864(carId, config) in data mode 1: the car's row of player table 32 by the binary search 0x80077E64 (row 0 when
    // absent), 0x80076ED0; the record 0x800770BC; the figures 0x80075840.
    const size_t n = data_.PlayerCarCount();
    size_t row = 0;
    {
        int64_t lo = -1, hi = int64_t(n);
        int64_t mid = (int64_t(n) - 1) >> 1;
        for (;;) {
            const uint32_t id = data_.PlayerCarId(size_t(mid));
            if (id == carId) {
                row = size_t(mid);
                break;
            }
            if (id < carId) lo = mid;
            else hi = mid;
            if (lo + 1 == hi) break;
            mid = (lo + hi) >> 1;
        }
    }
    CarConfig config = data_.PlayerCarConfig(row);
    sim::CarParams record{};
    career::BuildMenuRecord(data_.Tables(), config, record);
    career::CarPowerFigures(record, f.powerCurve.data());
    f.curve = true;
    switch (record.driveType) { // 0x80016A10
    case 0: f.badge = 1; break;
    case 1: f.badge = 0; break;
    case 2: f.badge = 2; break;
    case 3: f.badge = 3; break;
    case 4: f.badge = 4; break;
    default: f.badge = 0; break;
    }
    return f;
}

ArcCarFigures ArcadeCarPage::FiguresOf(int index) const { return ClassCarFigures(a_, data_, cars_, cls_, index); }

void ArcadeCarPage::SetModel(int index) { // 0x80016530(model, view, name, paint index)
    const uint32_t carId = PackCarId(Class().cars.at(size_t(index)));
    const int32_t info = cars_.IndexOf(carId);
    modelPaints_ = int(cars_.At(info < 0 ? 0 : size_t(info)).PaintCount());
    modelCar_ = carId;
    modelData_ = false;
    modelLoaded_ = false;
    (void)modelPaint_;
}

void ArcadeCarPage::Enter(int cls, const std::vector<uint8_t>& available, bool reenter, uint8_t language, MenuVram& vram, TextCtx& c) { // 0x8001E890
    (void)c;
    language_ = language;
    if (!reenter) {
        cls_ = cls;
        available_ = available;
        openTimer_ = 0x18;
        carHidden_ = 0x18;
        reopenTimer_ = -1;
        stage_ = 0;
        int first = 0; // 0x8001D358: the first open car of lists 0 and 6
        if (cls == 0 || cls == 6)
            for (size_t i = 0; i < available.size(); i++)
                if (available[i]) {
                    first = int(i);
                    break;
                }
        carousel_.available = &available_;
        carousel_.counts[0] = int16_t(Class().cars.size());
        carousel_.groups = 1;
        carousel_.group = 0;
        carousel_.index = int16_t(first);
        carousel_.prevGroup = carousel_.prevIndex = -1;
        carousel_.anim = -1;
        carousel_.slide = 0;
        carousel_.vertical = 0;
        carousel_.arrows = false;
        rule_.Open();
        transmission_.Init(a_, kTransmissionBar);
        settings_.Init(a_, kSettingsBar);
        SetModel(first);
        modelPaint_ = 0 % modelPaints_;
        camera_ = CarCamera();
        logo_.Clear();
        logo_.Set(a_, vram, Class().model.at(size_t(first)), PackCarId(Class().cars.at(size_t(first))));
        specs_.Init(a_);
        specs_.Swap();
        fillPending_ = 1;
    } else {
        carHidden_ = 0x18;
        reopenTimer_ = 0x20;
        rule_.Open();
        stage_ = 2;
        logo_.Clear();
        const int i = carousel_.index;
        logo_.Set(a_, vram, Class().model.at(size_t(i)), PackCarId(Class().cars.at(size_t(i))));
    }
}

ArcadeCarPage::Result ArcadeCarPage::Update(const MenuListPad* pad, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c) { // 0x8001EAA8
    if (openTimer_ > 0 && --openTimer_ == 0) carousel_.Open();
    if (reopenTimer_ > 0 && --reopenTimer_ == 0) {
        settings_.Open();
        carousel_.Open();
    }
    if (carHidden_ > 0) carHidden_--;
    rule_.Tick();
    if (fillPending_ && modelData_) {
        specs_.Fill(a_, FiguresOf(carousel_.index), language_, c);
        fillPending_ = 0;
    }
    if (modelLoaded_) menu::TurnModelCamera(camera_, 16, kFrameLength); // 0x8001E5BC
    specs_.Tick();
    Result result = kStay;
    if (stage_ == 1) {
        carousel_.Update(nullptr);
        settings_.Update(nullptr);
        const int b = transmission_.Update(pad);
        if (b == -2) return result;
        if (b == -3) {
            sounds.push_back(5);
            return result;
        }
        if (b == -1) {
            sounds.push_back(2);
            specs_.Swap();
            fillPending_ = 1;
            transmission_.Close();
            stage_ = 0;
            return result;
        }
        sounds.push_back(1);
        settings_.Init(a_, kSettingsBar);
        settings_.x = 0xB0, settings_.y = 0x1A4;
        settings_.Open();
        transmission_.Close();
        stage_ = 2;
        sel[Sel::kTransmission] = uint8_t(a_.data.transmissions[size_t(b)]);
        return result;
    }
    if (stage_ == 2) {
        carousel_.Update(nullptr);
        transmission_.Update(nullptr);
        const int b = settings_.Update(pad);
        if (b == -2) return result;
        if (b == -3) {
            sounds.push_back(5);
            return result;
        }
        if (b == -1) {
            sounds.push_back(2);
            transmission_.Open();
            settings_.Close();
            stage_ = 1;
            return result;
        }
        if (!modelLoaded_) {
            sounds.push_back(0);
            return result;
        }
        sounds.push_back(3);
        carousel_.Close();
        rule_.Close();
        settings_.Close();
        logo_.Clear();
        carHidden_ = -1;
        sel[Sel::kTyres] = uint8_t(b);
        return kChosen;
    }
    // stage 0: the car
    const uint32_t bits = pad ? (pad->pressed | pad->repeat) : 0;
    if ((bits & mp::kUp) && modelLoaded_) modelPaint_ = (modelPaint_ - 1 + modelPaints_) % modelPaints_; // 0x80015E9C(model, -1)
    if ((bits & mp::kDown) && modelLoaded_) modelPaint_ = (modelPaint_ + 1) % modelPaints_;
    if (bits & (mp::kUp | mp::kDown)) sounds.push_back(5);
    specs_.SelectChip(modelPaint_);
    transmission_.Update(nullptr);
    settings_.Update(nullptr);
    const int m = carousel_.Update(pad);
    const int idx = carousel_.index;
    if (m == -2) return result;
    if (m == -3) {
        sounds.push_back(7);
        const uint32_t carId = PackCarId(Class().cars.at(size_t(idx)));
        logo_.Set(a_, vram, Class().model.at(size_t(idx)), carId);
        SetModel(idx);
        modelPaint_ = idx % modelPaints_; // 0x80016530: the paint = the list index % paints
        camera_ = CarCamera();
        specs_.Swap();
        fillPending_ = 1;
        return result;
    }
    if (m == -1) {
        sounds.push_back(4);
        carousel_.Close();
        rule_.Close();
        specs_.CloseCurrent();
        logo_.Clear();
        modelLoaded_ = modelData_ = false; // 0x800165CC
        return kBack;
    }
    sounds.push_back(1);
    transmission_.Init(a_, kTransmissionBar);
    transmission_.x = 0xB0, transmission_.y = 0x1A4;
    transmission_.Open();
    fillPending_ = 0;
    specs_.CloseCurrent();
    stage_ = 1;
    const uint32_t carId = PackCarId(Class().cars.at(size_t(idx)));
    std::memcpy(sel + Sel::kCarShown, &carId, 4);
    std::memcpy(sel + Sel::kCar, &carId, 4);
    const int16_t paint = int16_t(modelPaint_);
    std::memcpy(sel + Sel::kColour, &paint, 2);
    return result;
}

void ArcadeCarPage::ModelTick() { // 0x80016624 after the views' update: the data at once, the model files (ours) at once
    if (modelCar_ == 0) return;
    modelData_ = true;
    modelLoaded_ = true;
}

void ArcadeCarPage::PageDraw(const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const { // 0x8001E5F8
    const bool current = it.current < 0 || it.index == it.current;
    for (int k = 0; k < 5; k++) specs_.Draw(k, current, &ot, it.x + kPageDx[k], kPageY[k], c);
    if (it.current < 0) return;
    const ArcadeClassCars& cc = Class();
    const std::string& name = cc.cars.at(size_t(it.index));
    const bool centred = name == a_.data.ImageString(kCentredLogoCar0) || name == a_.data.ImageString(kCentredLogoCar1);
    const uint32_t grey = uint32_t(it.brightness) | uint32_t(it.brightness) << 8 | uint32_t(it.brightness) << 16;
    if (!centred) {
        const GuestImage& o = a_.data.ovl2;
        const uint32_t e = kMakerSprites + uint32_t(cc.number2.at(size_t(it.index))) * 12;
        const int16_t w = o.Get<int16_t>(e + 4), h = o.Get<int16_t>(e + 6);
        // the xy word is built as one 32-bit sum: a negative x borrows one from y
        const int32_t xy = int32_t(it.x - (w >> 1)) + int32_t(uint32_t(it.y) << 16) + (h >> 1) * -0x20000 - 0x20000;
        AddSprite(ot, int16_t(uint32_t(xy) & 0xFFFF), int16_t(uint32_t(xy) >> 16), o.Get<uint8_t>(e), o.Get<uint8_t>(e + 1), o.Get<uint16_t>(e + 2), w, h,
                  o.Get<uint16_t>(e + 8), grey);
    }
    logo_.Draw(ot, PackCarId(name), it.x, it.y, it.brightness, centred);
}

menu::MenuCarProjection ArcadeCarPage::Projection(bool withYaw) const {
    menu::MenuCarProjection p = menu::OverlayModelProject(camera_, kCarEnvX, kCarEnvY, withYaw);
    p.w = kCarEnvW; // the drawing area (the projection's centre stays the camera's 256 x 240 view)
    p.h = kCarEnvH;
    return p;
}

std::vector<MenuPrim> ArcadeCarPage::Floor() const { return menu::OverlayModelFloor(Projection(false), camera_); }

void ArcadeCarPage::Draw(ViewOt& ot, TextCtx& c) const { // 0x8001F124
    MenuOtSlot& s2 = ot.slot[2];
    carousel_.Draw(s2, [&](const ArcCarousel::Item& it) { PageDraw(it, s2, c); });
    rule_.Draw(s2);
    transmission_.Draw(&ot.slot[0], c);
    settings_.Draw(&ot.slot[0], c);
}


// ---------------------------------------------------------------- the garages

namespace {

constexpr uint32_t kGarageList = 0x8004FBD4u;      // the EXE list widget object of the garage view
constexpr uint32_t kGarageRowSprite = 0x800453FCu; // 12-byte sprite: the row's banner (256 x 24, arc_other)
constexpr uint32_t kClassLetters = 0x8004FADCu;    // char* [4]: the class letters of the rows
constexpr uint32_t kGarageBars = 0x80051FBCu;      // 3 bytes 0xFF: no bar values ("unknown")
constexpr uint32_t kFmtHp = 0x800F8295u;           // "%dhp"

int32_t GarageTest(int power, int weight) { // 0x80019E44
    // mult power, power (mflo); weight * 300000 by shifts wraps at 32 bits (weight up to 0x1FFF); div only when the square > 0
    const int32_t square = int32_t(uint32_t(power) * uint32_t(power));
    if (square > 0) return int32_t(uint32_t(weight) * 300000u) / square;
    return 0x88B9;
}

// EXE 0x8006BA18(ot, {x, y, w, h, colour15}, a): a chip - POLY_G4 0x38 inside the rectangle (grey a * 244 >> 7 at the top left,
// the 15-bit colour scaled by a / 16 at the top right, black at the bottom), a black TILE behind it, E1 0x200.
void AddChip(MenuOtSlot& ot, int x, int y, int w, int h, uint16_t colour, int a) {
    int r = int(((colour & 0x1Fu) * uint32_t(a)) >> 4), g = int(((colour & 0x3E0u) * uint32_t(a)) >> 9), b = int(((colour & 0x7C00u) * uint32_t(a)) >> 14);
    int grey = (a * 244) >> 7;
    grey = std::min(grey, 255), r = std::min(r, 255), g = std::min(g, 255), b = std::min(b, 255);
    MenuPrim p;
    p.kind = MenuPrim::kPolyG4;
    p.gouraud = true;
    const int xs[4] = {x + 1, x + w - 1, x + 1, x + w - 1}, ys[4] = {y + 1, y + 1, y + h - 1, y + h - 1};
    const uint32_t cs[4] = {uint32_t(grey) | uint32_t(grey) << 8 | uint32_t(grey) << 16, uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16, 0, 0};
    for (int i = 0; i < 4; i++) p.x[i] = int16_t(xs[i]), p.y[i] = int16_t(ys[i]), p.colour[i] = cs[i];
    ot.Add(p);
    ot.Add(Tile(x, y, w, h, 0));
    ot.DrawMode(0x200);
}

} // namespace

int GarageCount(std::span<const uint8_t> garage) {
    if (garage.size() < kGarageStride) throw std::out_of_range("arcade garage: block too short");
    return S16(garage, 0);
}

ArcGarageEntry GarageEntryOf(std::span<const uint8_t> car) { // 0x80019E88
    ArcGarageEntry e;
    e.rally = (car[0x9A + 0x2D / 8] >> (0x2D % 8)) & 1; // 0x8005E784(car, 0x2D)
    const uint16_t wd = U16(car, 0x94);
    e.drive = uint8_t(wd >> 13);
    e.power = int16_t(U16(car, 0x98) & 0x3FFF);
    e.torque = S16(car, 0x96);
    e.weight = int16_t(wd & 0x1FFF);
    e.cls = 3;
    const int32_t test = GarageTest(e.power, e.weight);
    if (test < 0x2AE5) e.cls = 2;
    if (test < 0x170C) e.cls = 1;
    if (test < 0xE10) e.cls = 0;
    return e;
}

std::vector<ArcGarageEntry> GarageSummary(std::span<const uint8_t> garage) { // 0x80019F44
    std::vector<ArcGarageEntry> out;
    const int n = std::clamp(GarageCount(garage), 0, 100);
    for (int i = 0; i < n; i++) out.push_back(GarageEntryOf(garage.subspan(4 + size_t(i) * kGarageCarSize, kGarageCarSize)));
    return out;
}

void ArcadeCarPage::SetGarageModel(int index) { // 0x8001634C(model, view, garage, index)
    const std::span<const uint8_t> car = std::span<const uint8_t>(garageBlock_).subspan(4 + size_t(index) * kGarageCarSize, kGarageCarSize);
    modelCar_ = U32(car, 0x8C);
    const int32_t info = cars_.IndexOf(modelCar_);
    const CarInfoRecord& r = cars_.At(info < 0 ? 0 : size_t(info));
    modelPaints_ = int(r.PaintCount());
    modelPaint_ = 0;
    const int32_t paint = int32_t(U32(car, 4));
    for (size_t i = 0; i < r.PaintCount() && i < r.paintIds.size(); i++)
        if (int32_t(int8_t(r.paintIds[i])) == paint) modelPaint_ = int(i);
    const uint16_t wd = U16(car, 0x94);
    garageDrive_ = wd >> 13;
    garageFigures_ = {int16_t(U16(car, 0x98) & 0x3FFF), 0, S16(car, 0x96), 0, int16_t(wd & 0x1FFF)}; // 0x80016A7C
    modelData_ = false;
    modelLoaded_ = false;
}

void ArcadeCarPage::EnterGarage(int garage, bool reenter, std::span<const uint8_t> block, bool rally, uint8_t language, MenuVram& vram) { // 0x8001F8CC
    language_ = language;
    if (!reenter) {
        garage_ = garage;
        rally_ = rally;
        garageBlock_.assign(block.begin(), block.begin() + std::ptrdiff_t(kGarageStride));
        garageInfo_ = GarageSummary(block);
        carHidden_ = 0x18;
        reopenTimer_ = -1;
        stage_ = 0;
        fillPending_ = 0;
        listTimer_ = 0x0C;
        list_ = MenuListWidget::Read(a_.data.ovl2, kGarageList);
        list_.count = int16_t(GarageCount(block));
        MenuListReset(list_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) { return GarageRow(command, w, row, d); });
        carousel_.available = nullptr;
        carousel_.counts[0] = 1;
        carousel_.groups = 1;
        carousel_.group = 0;
        carousel_.index = 0;
        carousel_.prevGroup = carousel_.prevIndex = -1;
        carousel_.anim = -1;
        carousel_.slide = 0;
        carousel_.vertical = 0;
        carousel_.arrows = false;
        rule_.anim = -1;
        transmission_.Init(a_, kTransmissionBar);
        camera_ = CarCamera();
        logo_.Clear();
        specs_.Init(a_);
        for (int i = 0; i < 100; i++) order_[size_t(i)] = int16_t(i);
        return;
    }
    carHidden_ = 0x18;
    reopenTimer_ = 0x20;
    listTimer_ = 0x0C;
    rule_.Open();
    stage_ = 2;
    logo_.Clear();
    logo_.Set(a_, vram, -1, modelCar_);
}

int32_t GarageListRow(const GarageRowSource& g, int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) { // 0x8001F50C
    if (row < 0 || row >= 100) throw std::out_of_range("arcade garage list: row");
    const int index = g.order[row];
    const ArcGarageEntry& e = g.info->at(size_t(index));
    if (command == kMenuListEnabled) return g.rally ? (e.rally != 0 ? 1 : 0) : 1;
    if (command != kMenuListDraw || !d || !g.ctx) return 0;
    const ArcadeMenuAssets& a_ = *g.a;
    const CarInfoDirectory& cars_ = *g.cars;
    TextCtx& c = *g.ctx;
    MenuOtSlot* ot = d->ot + 1;
    c.ot = ot;
    c.font = &a_.FontAt(0x801234D0u);
    int fade = (int(w.fade) << 7) / w.fadeMax;
    if (row == w.selection) { // 0x8006CE74: the selected row's fade, held while the list closes
        int f = w.fade;
        if (w.state < -1) {
            f = w.fadeMax - 59 - w.state;
            if (f < 0) f = 0;
            if (w.fadeMax < f) f = w.fadeMax;
        }
        fade = -((f << 7) / w.fadeMax);
    }
    if (fade == 0 || w.state == -1) return 0;
    // 0x8001F21C(ot, garage, index, entry, x, y, alpha, fade, ctx, enabled)
    const int x = d->x, y = d->y;
    c.mode = 1;
    int v = fade < 0 ? d->alpha + 0x80 + fade : (d->alpha * fade) >> 7;
    if (v > 0xFF) v = 0xFF;
    if (!d->enabled) v = v / 2;
    c.colour = uint32_t(v / 2) | uint32_t(v / 2) << 8 | uint32_t(v / 3) << 16 | 0x2000000u;
    const std::span<const uint8_t> car = g.block.subspan(4 + size_t(index) * kGarageCarSize, kGarageCarSize);
    const uint32_t model = U32(car, 0x8C);
    const int32_t info = cars_.IndexOf(model);
    const CarInfoRecord& r = cars_.At(info < 0 ? 0 : size_t(info));
    DrawText(c, r.rawName, x - 100, y + 8, 1); // 0x800609F8 of the model
    uint16_t chip = 0;                         // 0x80060C38(model, paint)
    {
        auto lower = [](int32_t ch) { return uint32_t(ch - 0x41) < 0x1A ? ch + 0x20 : ch; };
        size_t k = 0;
        for (size_t i = 0; i < r.PaintCount() && i < r.paintIds.size(); i++)
            if (lower(int32_t(int8_t(r.paintIds[i]))) == lower(int32_t(U32(car, 4)))) {
                k = i;
                break;
            }
        chip = r.chipColors.at(k);
    }
    AddChip(*ot, x - 116, y - 8, 9, 16, chip, v);
    const std::string hp = Format(a_.data.Text(kFmtHp), {D((e.power * 1000) / 0x3F6)});
    const int hw = NumberWidth(c, hp, 1, 0);
    DrawNumber(c, hp, x - (hw - 100), y + 8, 1, -3, 0);
    const std::string letter = a_.data.Text(a_.data.ovl2.Get<uint32_t>(kClassLetters + uint32_t(e.cls) * 4));
    DrawText(c, letter, (x + 110) - (TextWidth(c, letter, 1) >> 1), y + 8, 1); // 0x8006ACC4
    const GuestImage& o = a_.data.ovl2;
    const int16_t sw = o.Get<int16_t>(kGarageRowSprite + 4), sh = o.Get<int16_t>(kGarageRowSprite + 6);
    AddSprite(*ot, x - (sw >> 1), y - (sh >> 1), o.Get<uint8_t>(kGarageRowSprite), o.Get<uint8_t>(kGarageRowSprite + 1), o.Get<uint16_t>(kGarageRowSprite + 2), sw, sh,
              o.Get<uint16_t>(kGarageRowSprite + 8), uint32_t(v) | uint32_t(v) << 8 | uint32_t(v) << 16);
    return 0;
}

int32_t ArcadeCarPage::GarageRow(int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) const {
    GarageRowSource g;
    g.a = &a_, g.cars = &cars_, g.block = garageBlock_, g.info = &garageInfo_, g.order = order_.data(), g.rally = rally_, g.ctx = rowCtx_;
    return GarageListRow(g, command, w, row, d);
}

void ArcadeCarPage::GaragePageDraw(const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const { // 0x8001F754
    const bool current = it.current < 0 || it.index == it.current;
    for (int k = 0; k < 5; k++) specs_.Draw(k, current, &ot, it.x + kPageDx[k], kPageY[k], c);
    if (it.current >= 0) logo_.Draw(ot, modelCar_, it.x, it.y, it.brightness, false);
}

ArcadeCarPage::Result ArcadeCarPage::UpdateGarage(const MenuListPad* pad, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c) { // 0x8001FA8C
    rowCtx_ = &c;
    if (fillPending_ && modelData_) { // 0x800194FC with the garage figures: no curve, bars 0x80051FBC, no chips
        ArcCarFigures f;
        switch (garageDrive_) { // 0x80016A10
        case 0: f.badge = 1; break;
        case 2: f.badge = 2; break;
        case 3: f.badge = 3; break;
        case 4: f.badge = 4; break;
        default: f.badge = 0; break;
        }
        f.figures = garageFigures_;
        for (size_t k = 0; k < 3; k++) f.bars[k] = a_.data.ovl2.Get<uint8_t>(kGarageBars + uint32_t(k));
        f.curve = false;
        specs_.Fill(a_, f, language_, c);
        fillPending_ = 0;
    }
    if (reopenTimer_ > 0 && --reopenTimer_ == 0) {
        transmission_.Open();
        carousel_.Open();
    }
    rule_.Tick();
    if (carHidden_ > 0) carHidden_--;
    if (modelLoaded_) menu::TurnModelCamera(camera_, 16, kFrameLength);
    specs_.Tick();
    if (stage_ == 0) {
        if (listTimer_ > 0 && --listTimer_ == 0) MenuListOpen(list_);
        const int32_t r = MenuListUpdate(list_, pad);
        carousel_.Update(nullptr);
        transmission_.Update(nullptr);
        if (r == -3) {
            sounds.push_back(6);
            return kStay;
        }
        if (r == -4) {
            sounds.push_back(0);
            return kStay;
        }
        if (r == -2) return kStay;
        if (r == -1) {
            sounds.push_back(4);
            sel[Sel::kClass] = uint8_t(garage_ + 4);
            MenuListClose(list_);
            logo_.Clear();
            return kBack;
        }
        const int index = order_.at(size_t(r));
        sounds.push_back(1);
        MenuListClose(list_);
        const std::span<const uint8_t> car = std::span<const uint8_t>(garageBlock_).subspan(4 + size_t(index) * kGarageCarSize, kGarageCarSize);
        SetGarageModel(index);
        logo_.Set(a_, vram, -1, modelCar_);
        camera_ = CarCamera();
        rule_.Open();
        stage_ = 1;
        listTimer_ = 0x18;
        garageCar_ = index;
        sel[Sel::kClass] = garageInfo_.at(size_t(index)).cls;
        const int16_t slot = int16_t(index);
        std::memcpy(sel + Sel::kGarageSlot, &slot, 2);
        std::memcpy(sel + Sel::kCarShown, car.data(), 4);   // +0xC = the garage car's id
        std::memcpy(sel + Sel::kCar, car.data() + 0x8C, 4); // +0x10 = its model id
        const int16_t paint = int16_t(modelPaint_);
        std::memcpy(sel + Sel::kColour, &paint, 2);
        return kStay;
    }
    if (stage_ == 1) {
        if (listTimer_ > 0 && --listTimer_ == 0) {
            carousel_.Open();
            specs_.Swap();
            fillPending_ = 1;
        }
        MenuListUpdate(list_, nullptr);
        const int m = carousel_.Update(pad);
        transmission_.Update(nullptr);
        if (m == -2) return kStay;
        if (m == -3 || m == -1) {
            sounds.push_back(2);
            carousel_.Close();
            modelLoaded_ = modelData_ = false; // 0x800165CC
            fillPending_ = 0;
            specs_.CloseCurrent();
            rule_.Close();
            stage_ = 0;
            listTimer_ = 0x0C;
            return kStay;
        }
        sounds.push_back(1);
        fillPending_ = 0;
        specs_.CloseCurrent();
        transmission_.Init(a_, kTransmissionBar);
        transmission_.x = 0xB0, transmission_.y = 0x1A4;
        transmission_.Open();
        stage_ = 2;
        return kStay;
    }
    MenuListUpdate(list_, nullptr);
    carousel_.Update(nullptr);
    const int b = transmission_.Update(pad);
    if (b == -2) return kStay;
    if (b == -3) {
        sounds.push_back(5);
        return kStay;
    }
    if (b == -1) {
        sounds.push_back(2);
        transmission_.Close();
        specs_.Swap();
        fillPending_ = 1;
        stage_ = 1;
        listTimer_ = 0;
        return kStay;
    }
    if (!modelLoaded_) {
        sounds.push_back(0);
        return kStay;
    }
    sounds.push_back(3);
    carousel_.Close();
    rule_.Close();
    transmission_.Close();
    logo_.Clear();
    carHidden_ = -1;
    sel[Sel::kTransmission] = uint8_t(a_.data.transmissions[size_t(b)]);
    return kChosen;
}

void ArcadeCarPage::DrawGarage(ViewOt& ot, TextCtx& c) const { // 0x8002007C
    rowCtx_ = &c;
    MenuListDraw(list_, ot.slot[2]);
    carousel_.Draw(ot.slot[2], [&](const ArcCarousel::Item& it) { GaragePageDraw(it, ot.slot[2], c); });
    rule_.Draw(ot.slot[2]);
    transmission_.Draw(&ot.slot[0], c);
}

} // namespace gt2::arcade
