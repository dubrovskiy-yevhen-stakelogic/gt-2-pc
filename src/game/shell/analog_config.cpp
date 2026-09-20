#include "game/shell/analog_config.h"

#include <cstdio>
#include <cstring>

#include "game/shell/title_draw.h"
#include "game/shell/title_screens.h"

namespace gt2::shell {

namespace {

constexpr uint32_t kSetButtons = 0x11000; // R1 (the neGcon's R) / START
constexpr uint32_t kChoose = 0xA00, kBack = 0x500;
constexpr uint32_t kCalibration = 0x3E, kNegconTable = 0x16;

int16_t S16(const uint8_t* p, uint32_t o) { int16_t v; std::memcpy(&v, p + o, 2); return v; }
uint16_t U16(const uint8_t* p, uint32_t o) { uint16_t v; std::memcpy(&v, p + o, 2); return v; }
void Put16(uint8_t* p, uint32_t o, int32_t v) { const int16_t s = int16_t(v); std::memcpy(p + o, &s, 2); }

// 0x8001BF84 / 0x8001BFC8 / 0x8001C00C: the axis (0..3) of the neGcon table's steering / accelerate / brake, or -1.
int AxisOf(const uint8_t* block, int function) {
    const int e = block[kNegconTable + uint32_t(function == 0 ? 0 : function == 1 ? 2 : 3)];
    return (e > 0x7F && unsigned(e - 0x80) < 4u) ? e - 0x80 : -1;
}
// 0x8001C050 / 0x8001C0B0 / 0x8001C110: a neGcon in the port and the function on an axis entry.
bool OnAxis(uint8_t padType, const uint8_t* block, int function) {
    return padType == 2 && block[kNegconTable + uint32_t(function == 0 ? 0 : function == 1 ? 2 : 3)] > 0x7F;
}

void ClampSteer(int32_t* m) { // 0x8001ACB4: centre, lock, margin, max, min
    int32_t v = m[0];
    if (m[0] < 0x40) v = 0x40;
    if (0xC0 < m[0]) v = 0xC0;
    if (v < m[4]) v = m[4];
    if (m[3] < v) v = m[3];
    m[0] = v;
    if (v - m[1] < m[4]) m[1] = v - m[4];
    if (m[3] < m[0] + m[1]) m[1] = m[3] - m[0];
    if (m[1] < 0) m[1] = 0;
    if (m[1] < m[2]) m[2] = m[1];
    if (m[2] < 0) m[2] = 0;
}
void SetRange(int32_t* m, int32_t a, int32_t b) { // 0x8001ADFC
    int32_t hi = a;
    if (a < b) hi = b, b = a;
    m[3] = hi;
    m[4] = b;
    ClampSteer(m);
}
void ClampPedal(int32_t* m) { // 0x8001B7F8: lock, margin
    if (0xFF < m[0]) m[0] = 0xFF;
    if (m[0] < 2) m[0] = 2;
    if (m[0] <= m[1]) m[1] = m[0] - 1;
    if (m[1] < 1) m[1] = 1;
}

void LoadSteer(const uint8_t* cal, int32_t* m, int axis) { // 0x8001C170
    if (axis != 0) return;
    const int32_t centre = int32_t(uint32_t(U16(cal, 2)) + uint32_t(U16(cal, 4))) >> 1;
    m[2] = int32_t(U16(cal, 4)) - centre;
    m[0] = centre;
    m[1] = int32_t(U16(cal, 6)) - centre;
    SetRange(m, 0xFF, 0);
}
void LoadPedal(const uint8_t* cal, int32_t* m, int axis) { // 0x8001C1C8
    if (unsigned(axis - 1) >= 3u) return;
    const uint8_t* e = cal + (axis - 1) * 4;
    m[0] = U16(e, 10);
    m[1] = U16(e, 8);
    ClampPedal(m);
}
void StoreSteer(uint8_t* cal, const int32_t* m, int axis) { // 0x8001C210
    if (axis != 0) return;
    Put16(cal, 0, m[0] - m[1]);
    Put16(cal, 2, m[0] - m[2]);
    Put16(cal, 4, m[0] + m[2]);
    Put16(cal, 6, m[0] + m[1]);
}
void StorePedal(uint8_t* cal, const int32_t* m, int axis) { // 0x8001C270
    if (unsigned(axis - 1) >= 3u) return;
    uint8_t* e = cal + (axis - 1) * 4;
    Put16(e, 8, m[1]);
    Put16(e, 10, m[0]);
}

Band BandOf(const uint8_t* b) {
    Band band;
    band.w = S16(b, 0), band.h = S16(b, 2), band.steps = S16(b, 4), band.flags = S16(b, 6);
    std::memcpy(&band.c0, b + 8, 4);
    std::memcpy(&band.c1, b + 0xC, 4);
    std::memcpy(&band.target, b + 0x10, 4);
    std::memcpy(&band.targetOut, b + 0x14, 4);
    band.anim = S16(b, 0x18);
    return band;
}
void TickBand(uint8_t* b) { // 0x8006BE64 on the object's band
    Band band = BandOf(b);
    band.Tick();
    Put16(b, 0x18, band.anim);
}

} // namespace

AnalogConfigData AnalogConfigData::Read(const GuestImage& ovl1, const GuestImage& exe) {
    AnalogConfigData d;
    d.keys = KeyConfigData::Read(ovl1, exe);
    for (uint32_t b = 0; b < 3; b++)
        for (uint32_t k = 0; k < 0x1C; k++) d.bandTemplates[b][k] = ovl1.Get<uint8_t>(kBandTemplates + b * 0x1C + k);
    for (uint32_t c = 0; c < d.palette.size(); c++) d.palette[c] = ovl1.Get<uint32_t>(kPalette + c * 4);
    return d;
}

bool AnalogItems(AnalogGlobals& g, uint8_t padType, const uint8_t* block) { // 0x8001C2A0
    g.steerOn = g.accelOn = g.brakeOn = 0;
    if (padType != 2) {
        g.count = 0;
        return false;
    }
    int n = 0;
    if (OnAxis(padType, block, 0)) {
        g.items[0].kind = 0, g.items[0].sub = 0, g.items[0].text = AnalogConfigData::kCentre;
        g.items[1].kind = 0, g.items[1].sub = 1, g.items[1].text = AnalogConfigData::kSteerLock;
        g.items[2].kind = 0, g.items[2].sub = 2, g.items[2].text = AnalogConfigData::kSteerMargin;
        g.steerOn = 1;
        n = 3;
    }
    if (OnAxis(padType, block, 1)) {
        g.items[size_t(n)].kind = 1, g.items[size_t(n)].sub = 0, g.items[size_t(n)].text = AnalogConfigData::kAccelLock;
        g.items[size_t(n + 1)].kind = 1, g.items[size_t(n + 1)].sub = 1, g.items[size_t(n + 1)].text = AnalogConfigData::kAccelMargin;
        g.accelOn = 1;
        n += 2;
    }
    if (OnAxis(padType, block, 2)) {
        g.items[size_t(n)].kind = 2, g.items[size_t(n)].sub = 0, g.items[size_t(n)].text = AnalogConfigData::kBrakeLock;
        g.items[size_t(n + 1)].kind = 2, g.items[size_t(n + 1)].sub = 1, g.items[size_t(n + 1)].text = AnalogConfigData::kBrakeMargin;
        g.brakeOn = 1;
        n += 2;
    }
    g.count = n + 1;
    g.items[size_t(n)].kind = 3; // (its sub is left as it was)
    g.items[size_t(n)].text = AnalogConfigData::kExit;
    return g.count > 1;
}

void AnalogReset(AnalogPageObject& o, int port, const uint8_t* block, const AnalogConfigData& data) { // 0x8001C48C
    o.state = 3;
    o.port = int16_t(port);
    const uint8_t* cal = block + kCalibration;
    LoadSteer(cal, o.steer, AxisOf(block, 0));
    LoadPedal(cal, o.accel, AxisOf(block, 1));
    LoadPedal(cal, o.brake, AxisOf(block, 2));
    for (size_t b = 0; b < 3; b++) {
        std::memcpy(o.bands[b], data.bandTemplates[b].data(), 0x1C);
        Put16(o.bands[b], 0x18, 0);
    }
}

int AnalogEnter(AnalogPageObject& o, AnalogGlobals& g, uint8_t padType, const uint8_t* block) { // 0x8001C690
    if (!AnalogItems(g, padType, block)) return 0;
    g.steerRaw = g.accelRaw = g.brakeRaw = 0;
    o.item = 0;
    if (g.steerOn == 0) {
        o.state = 2;
        g.text = g.items[size_t(o.item)].text;
    } else {
        o.state = 0;
        g.text = AnalogConfigData::kTurnRight;
    }
    const uint8_t* cal = block + kCalibration;
    LoadSteer(cal, o.steer, AxisOf(block, 0));
    LoadPedal(cal, o.accel, AxisOf(block, 1));
    LoadPedal(cal, o.brake, AxisOf(block, 2));
    return 1;
}

int AnalogUpdate(AnalogPageObject& o, AnalogGlobals& g, const std::array<uint8_t, 2>& padTypes, const std::array<MenuListPad, 2>& pads,
                 const std::array<uint8_t, 4>& raw, uint8_t* block, std::vector<int>& sounds) { // 0x8001C7B8
    const size_t port = size_t(o.port & 1);
    for (auto& band : o.bands) TickBand(band);
    if (o.state == 3) return -1;
    if (padTypes[port] != 2) { // the controller went away
        sounds.push_back(0);
        o.state = 3;
        return -2;
    }
    const int steerAxis = AxisOf(block, 0), accelAxis = AxisOf(block, 1), brakeAxis = AxisOf(block, 2);
    if (steerAxis >= 0) g.steerRaw = raw[size_t(steerAxis)];
    if (accelAxis >= 0) g.accelRaw = raw[size_t(accelAxis)];
    if (brakeAxis >= 0) g.brakeRaw = raw[size_t(brakeAxis)];
    const uint32_t both = pads[port].pressed | pads[1 - port].pressed;
    if (o.state == 0 || o.state == 1) {
        if (both & kSetButtons) {
            if (o.state == 0) { // the far right end taken; now the far left
                g.text = AnalogConfigData::kTurnLeft;
                g.rightEnd = g.steerRaw;
                o.state = 1;
                sounds.push_back(1);
                return -1;
            }
            g.leftEnd = g.steerRaw;
            SetRange(o.steer, g.rightEnd, g.steerRaw);
            o.state = 2;
            sounds.push_back(1);
            g.text = g.items[size_t(o.item)].text;
            return -1;
        }
        if ((both & kBack) == 0) return -1;
        sounds.push_back(2);
        o.state = 3;
        return -2;
    }
    if (o.state != 2) return -1;
    const AnalogGlobals::Item item = g.items[size_t(o.item) & 15];
    const uint32_t pressed = pads[port].pressed, edges = pressed | pads[port].repeat;
    if (item.kind == 3 && (pressed & kChoose)) { // Exit: the models into the calibration
        uint8_t* cal = block + kCalibration;
        StoreSteer(cal, o.steer, steerAxis);
        StorePedal(cal, o.accel, accelAxis);
        StorePedal(cal, o.brake, brakeAxis);
        o.state = 3;
        return 0;
    }
    if (pressed & kSetButtons) { // the live value
        if (item.kind == 1 || item.kind == 2) {
            int32_t* m = item.kind == 1 ? o.accel : o.brake;
            const int32_t v = item.kind == 1 ? g.accelRaw : g.brakeRaw;
            if (item.sub == 0) m[0] = v, ClampPedal(m);
            else if (item.sub == 1) m[1] = v, ClampPedal(m);
            sounds.push_back(1);
        } else if (item.kind == 0) {
            int32_t d = int32_t(g.steerRaw) - o.steer[0];
            if (d < 0) d = -d;
            if (item.sub == 1) o.steer[1] = d, ClampSteer(o.steer);
            else if (item.sub == 0) o.steer[0] = g.steerRaw, ClampSteer(o.steer);
            else if (item.sub == 2) o.steer[2] = d, ClampSteer(o.steer);
            sounds.push_back(1);
        }
    }
    int step = 0;
    if (edges & 4) step = -1;
    if (edges & 8) step++;
    if (step != 0) {
        if (item.kind == 1 || item.kind == 2) {
            int32_t* m = item.kind == 1 ? o.accel : o.brake;
            if (item.sub == 0) m[0] += step, ClampPedal(m);
            else if (item.sub == 1) m[1] += step, ClampPedal(m);
            sounds.push_back(5);
        } else if (item.kind == 0) {
            if (item.sub == 1) o.steer[1] += step, ClampSteer(o.steer);
            else if (item.sub == 0) o.steer[0] += step, ClampSteer(o.steer);
            else if (item.sub == 2) o.steer[2] += step, ClampSteer(o.steer);
            sounds.push_back(5);
        }
    }
    int row = o.item;
    if ((edges & 1) && --row < 0) row = g.count - 1;
    if ((edges & 2) && ++row >= g.count) row = 0;
    if (row == o.item) return -1;
    o.item = int16_t(row);
    sounds.push_back(6);
    g.text = g.items[size_t(row) & 15].text;
    return -1;
}

void AnalogPageLeft(AnalogPageObject& o) { // 0x8001C648
    for (auto& band : o.bands) Put16(band, 0x18, ~int32_t(U16(band, 4)));
}

void AnalogPageEntered(AnalogPageObject& o, const uint8_t* block, const AnalogConfigData& data) { // 0x8001C610
    AnalogReset(o, o.port, block, data);
    for (auto& band : o.bands) Put16(band, 0x18, 0);
}

// ---------------------------------------------------------------- drawing

namespace {

struct Painter {
    MenuOtSlot& ot;
    const TitleAssets& assets;
    const AnalogConfigData& data;
    int alpha;
    uint32_t Lerp(int a, int b, int max) const { return MenuListLerp(data.palette[size_t(a)], data.palette[size_t(b)], alpha, max); }
    void Tile(int x, int y, int w, int h, uint32_t colour) const { // 0x8007D024
        MenuPrim p;
        p.kind = MenuPrim::kTile;
        p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
        p.colour[0] = colour & 0xFFFFFF;
        p.semi = (colour & 0x2000000u) != 0;
        ot.Add(p);
    }
    void Box(int x, int y, int w, int h, uint32_t colour) const { // 0x8007E780: the outline as a closed polyline
        const int x1 = x + w - 1, y1 = y + h - 1;
        const int px[5] = {x, x1, x1, x, x}, py[5] = {y, y, y1, y1, y};
        for (int k = 0; k < 4; k++) {
            MenuPrim p;
            p.kind = MenuPrim::kLine;
            p.x[0] = int16_t(px[k]), p.y[0] = int16_t(py[k]), p.x[1] = int16_t(px[k + 1]), p.y[1] = int16_t(py[k + 1]);
            p.colour[0] = p.colour[1] = colour & 0xFFFFFF;
            p.semi = (colour & 0x2000000u) != 0;
            ot.Add(p);
        }
    }
    void Marker(int x, int y) const { // 0x8006B988(x, y, 3, 0xE, phase 0): a triangle pointing down
        MenuPrim p;
        p.kind = MenuPrim::kPolyF4;
        p.semi = true;
        p.x[0] = int16_t(x), p.y[0] = int16_t(y + 0xE);
        p.x[1] = int16_t(x + 3), p.y[1] = int16_t(y);
        p.x[2] = int16_t(x - 3), p.y[2] = int16_t(y);
        p.x[3] = p.x[2], p.y[3] = p.y[2];
        p.colour[0] = 0xFF | 0x7F << 8;
        ot.Add(p);
    }
    void Label(const HudFont& font, uint32_t text, int x, int y, uint32_t colour, TextAlign align) const {
        AddText(ot, font, assets.Text(text), x, y, 1, colour, 1, align);
    }
    void Value(int v, int x, int y, uint32_t colour) const { // "%d" (0x80022D7C) through 0x8006B184
        char s[16];
        std::snprintf(s, sizeof s, "%d", v);
        AddNumberText(ot, assets.fonts[TitleAssets::kSmallFont], s, x, y, 1, -2, 0, colour, 1, true);
    }
};

// Palette indices (0x8004C31C + 4 * i).
enum : int { kBase = 0, kBlack = 1, kBackground = 2, kFrame = 3, kMarginZone = 4, kLockZone = 5, kOutOfRange = 6, kCentreLine = 7,
             kLive = 8, kLabel = 9, kValue = 10, kTitle = 11, kPress = 12 };

void NotAvailable(const Painter& P, uint32_t label, int x, int y, int selected) { // 0x8001AE34 / 0x8001B8A0
    const int max = selected >= 0 ? 0x180 : 0x80;
    P.Label(P.assets.fonts[TitleAssets::kMediumFont], label, x - 0x76, y + 0x2C, P.Lerp(kBase, kTitle, max), TextAlign::kLeft);
    P.Label(P.assets.fonts[TitleAssets::kSmallFont], AnalogConfigData::kNa, x + 0x6E, y + 0x2C, P.Lerp(kBase, kLabel, max), TextAlign::kRight);
}

void SteerGauge(const Painter& P, const int32_t* m, uint32_t label, int x, int y, int raw, int selected, int mode, int marker, int marker2) { // 0x8001B2C0
    const int base = x - 0x80, top = y - 6;
    P.Box(x - 0x81, y - 7, 0x102, 0xE, P.Lerp(kBase, kFrame, 0x80));
    P.Box(x - 0x82, y - 8, 0x104, 0x10, P.Lerp(kBase, kFrame, 0x80));
    P.ot.DrawMode(0x20);
    const int c = m[0];
    if (mode >= 0 && mode < 2) {
        { // 0x8001AF6C: the title and the three values
            int cc = 0x80, cmax = 0x80, cmargin = 0x80;
            if (selected >= 0) {
                cc = cmax = cmargin = 0x180;
                if (selected == 1) cmax = 0x80;
                else if (selected == 0) cc = 0x80;
                else if (selected == 2) cmargin = 0x80;
            }
            const HudFont& small = P.assets.fonts[TitleAssets::kSmallFont];
            P.Label(P.assets.fonts[TitleAssets::kMediumFont], label, x - 0x76, y + 0x2C, P.Lerp(kBase, kTitle, 0x80), TextAlign::kLeft);
            P.Label(small, AnalogConfigData::kCentreLabel, x + 0x48, y + 0x1E, P.Lerp(kBase, kLabel, cc), TextAlign::kRight);
            P.Label(small, AnalogConfigData::kMaxLabel, x + 0x48, y + 0x32, P.Lerp(kBase, kLabel, cmax), TextAlign::kRight);
            P.Label(small, AnalogConfigData::kMarginLabel, x + 0x48, y + 0x46, P.Lerp(kBase, kLabel, cmargin), TextAlign::kRight);
            P.Value(m[0] - 0x80, x + 0x6E, y + 0x1E, P.Lerp(kBase, kValue, cc));
            P.Value(m[1], x + 0x6E, y + 0x32, P.Lerp(kBase, kValue, cmax));
            P.Value(m[2], x + 0x6E, y + 0x46, P.Lerp(kBase, kValue, cmargin));
        }
        if (raw >= 0) {
            P.Tile(base + raw, top, 1, 0xC, P.Lerp(kBlack, kLive, 0x80));
            P.Marker(base + raw, y - 0x16);
        }
        P.Tile(base + c, top, 1, 6, P.Lerp(kBlack, kCentreLine, 0x80));
        P.Tile(base + c - m[2], top, 2 * m[2], 0xC, P.Lerp(kBase, kMarginZone, 0x80));
        P.Tile(base + c + m[2], top, m[1] - m[2], 0xC, P.Lerp(kBase, kLockZone, 0x80));
        P.Tile(base + c - m[1], top, m[1] - m[2], 0xC, P.Lerp(kBase, kLockZone, 0x80));
        if (mode == 1) {
            const int right = 0x100 - m[3];
            if (right > 0) P.Tile(base + m[3], top, right, 0xC, P.Lerp(kBase, kOutOfRange, 0x80));
            if (m[4] > 0) P.Tile(base, top, m[4], 0xC, P.Lerp(kBase, kOutOfRange, 0x80));
        }
    } else if (mode == 2) {
        if (marker >= 0) P.Tile(base + marker, top, 1, 0xC, P.Lerp(kBlack, kLive, 0x80));
        if (marker2 >= 0) P.Tile(base + marker2, top, 1, 0xC, P.Lerp(kBlack, kLive, 0x80));
    }
    P.Tile(base, top, 0x100, 0xC, P.Lerp(kBase, kBackground, 0x80));
    P.ot.DrawMode(0);
}

void PedalGauge(const Painter& P, const int32_t* m, uint32_t label, int x, int y, int raw, int selected) { // 0x8001BC5C
    const int base = x - 0x80, top = y - 6;
    { // 0x8001B9D8
        int cmax = 0x80, cmargin = 0x80;
        if (selected >= 0) {
            cmax = cmargin = 0x180;
            if (selected == 0) cmax = 0x80;
            else if (selected == 1) cmargin = 0x80;
        }
        const HudFont& small = P.assets.fonts[TitleAssets::kSmallFont];
        P.Label(P.assets.fonts[TitleAssets::kMediumFont], label, x - 0x76, y + 0x2C, P.Lerp(kBase, kTitle, 0x80), TextAlign::kLeft);
        P.Label(small, AnalogConfigData::kMaxLabel, x + 0x48, y + 0x1E, P.Lerp(kBase, kLabel, cmax), TextAlign::kRight);
        P.Label(small, AnalogConfigData::kMarginLabel, x + 0x48, y + 0x32, P.Lerp(kBase, kLabel, cmargin), TextAlign::kRight);
        P.Value(m[0], x + 0x6E, y + 0x1E, P.Lerp(kBase, kValue, cmax));
        P.Value(m[1], x + 0x6E, y + 0x32, P.Lerp(kBase, kValue, cmargin));
    }
    P.Box(x - 0x81, y - 7, 0x102, 0xE, P.Lerp(kBase, kFrame, 0x80));
    P.Box(x - 0x82, y - 8, 0x104, 0x10, P.Lerp(kBase, kFrame, 0x80));
    P.ot.DrawMode(0x20);
    if (raw >= 0) {
        P.Tile(base + raw, top, 1, 0xC, P.Lerp(kBlack, kLive, 0x80));
        P.Marker(base + raw, y - 0x16);
    }
    P.Tile(base, top, m[1], 0xC, P.Lerp(kBase, kMarginZone, 0x80));
    P.Tile(base + m[1], top, m[0] - m[1], 0xC, P.Lerp(kBase, kLockZone, 0x80));
    P.Tile(base, top, 0x100, 0xC, P.Lerp(kBase, kBackground, 0x80));
    P.ot.DrawMode(0);
}

} // namespace

void AnalogDraw(MenuOtSlot& ot, const TitleAssets& assets, const AnalogConfigData& data, const AnalogPageObject& o, const AnalogGlobals& g,
                uint8_t padType, const uint8_t* block, int x, int y, int alpha) { // 0x8001CE28
    const Painter P{ot, assets, data, alpha};
    int steerRaw = g.steerRaw, accelRaw = g.accelRaw, brakeRaw = g.brakeRaw;
    int mode = 0, marker = -1, marker2 = -1;
    bool showSteer = true, showAccel = true, showBrake = true, exitShown = false;
    int selSteer = -1, selAccel = -1, selBrake = -1;
    if (o.state == 2) // (the help line of 0x8001AB40 is drawn only in the Japanese game)
        P.Label(assets.fonts[TitleAssets::kSmallFont], AnalogConfigData::kPress, 0xB0, 0x84, P.Lerp(kBase, kPress, 0x80), TextAlign::kCentre);
    if (o.state == 1) {
        mode = 2, showSteer = showAccel = showBrake = false;
        marker = g.rightEnd, marker2 = steerRaw;
    } else if (o.state == 0) {
        mode = 2, showSteer = showAccel = showBrake = false;
        marker = steerRaw;
    } else if (o.state == 2) {
        exitShown = true;
        mode = 1;
        const AnalogGlobals::Item& item = g.items[size_t(o.item) & 15];
        if (item.kind >= 0 && item.kind <= 3) selSteer = selAccel = selBrake = 0x40; // another gauge's item: dimmed
        if (item.kind == 1) selAccel = item.sub;
        else if (item.kind == 0) selSteer = item.sub;
        else if (item.kind == 2) selBrake = item.sub;
    } else if (o.state == 3) {
        steerRaw = accelRaw = brakeRaw = -1;
    }
    if (!OnAxis(padType, block, 0)) NotAvailable(P, AnalogConfigData::kSteering, x, y, selSteer);
    else SteerGauge(P, o.steer, AnalogConfigData::kSteering, x, y, steerRaw, selSteer, mode, marker, marker2);
    auto band = [&](int b, int by) {
        BandOf(o.bands[b]).Draw(ot, x - 0x7A, by);
        ot.DrawMode(0x220);
    };
    if (showSteer) band(0, y + 0x14);
    auto pedal = [&](int function, const int32_t* m, uint32_t label, int py, int raw, int selected, int iconY) {
        if (!OnAxis(padType, block, function)) {
            NotAvailable(P, label, x, py, selected);
            return;
        }
        PedalGauge(P, m, label, x, py, raw, selected);
        // 0x80019308: the neGcon button icon of the pedal's axis (I / II / L)
        const KeyConfigData& keys = data.keys;
        const uint8_t b = NegconButtonOf(block[kNegconTable + uint32_t(function == 1 ? 2 : 3)]);
        if (unsigned(b - 9) < 2u || b == 4) {
            const int index = KeyConfigData::kNegconIcons + keys.negconIconOf[b];
            const KeyConfigData::Icon& icon = keys.icons[size_t(index)];
            MenuPrim p;
            p.kind = MenuPrim::kSprite;
            p.x[0] = int16_t(x - (int16_t(icon.w) >> 1)), p.y[0] = int16_t(iconY - (int16_t(icon.h) >> 1));
            p.w = int16_t(icon.w), p.h = int16_t(icon.h), p.u = icon.u, p.v = icon.v, p.clut = icon.clut;
            p.tpage = uint16_t(icon.tpage | 0x20);
            const uint32_t grey = uint32_t(alpha) & 0xFF;
            p.colour[0] = grey | grey << 8 | grey << 16;
            ot.Add(p);
            ot.DrawMode(uint16_t(icon.tpage | 0x20));
        }
    };
    if (showAccel) {
        pedal(1, o.accel, AnalogConfigData::kAcceleration, y + 0x60, accelRaw, selAccel, y + 0x84);
        band(1, y + 0x74);
    }
    if (showBrake) {
        pedal(2, o.brake, AnalogConfigData::kBrake, y + 0xB4, brakeRaw, selBrake, y + 0xD8);
        band(2, y + 200);
    }
    if (exitShown) {
        const int max = g.items[size_t(o.item) & 15].kind == 3 ? 0x80 : 0x180;
        AddNumberText(ot, assets.fonts[TitleAssets::kSmallFont], assets.Text(AnalogConfigData::kExitLabel), x + 0x48, y - 0x10, 1, -2, 0,
                      P.Lerp(kBase, kLabel, max), 1, true);
    }
}

} // namespace gt2::shell
