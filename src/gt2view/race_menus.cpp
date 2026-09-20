#include "gt2view/race_menus.h"
#include "gt2view/race_menu_views.h"
#include "gt2view/race_record_screens.h"
#include "gt2view/race_result_screens.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/shell/title_draw.h"
#include "game/shell/title_screens.h"

namespace gt2::screens {

using shell::AddGradientQuad;
using shell::Band;

namespace {

// ovl0 addresses (US v1.2).
constexpr uint32_t kColourBase = 0x8005B1C4u;       // 0x02000000: the lerp base of the licence menu's colours
constexpr uint32_t kColourRedBase = 0x8005B1BCu;    // 0 -> 0x8005B1C0 0xC0: the red line under "License Test"
constexpr uint32_t kColourRed = 0x8005B1C0u;
constexpr uint32_t kColourText = 0x8005B1C8u;       // 0x02505050
constexpr uint32_t kColourLabel = 0x8005B1CCu;      // 0x02505028 ("B-1")
constexpr uint32_t kColourCarInfo = 0x8005B1D0u;    // 0x02505050
constexpr uint32_t kColourTimes = 0x8005B1D4u;      // 0x025E4A36
constexpr uint32_t kColourRules = 0x8005B1D8u;      // 0x0240402C
constexpr uint32_t kColourBoxSelected = 0x8005B1E8u; // 0xF4F4F4
constexpr uint32_t kColourBox = 0x8005B1ECu;        // 0x404040
constexpr uint32_t kLicenceLabelFormats = 0x8005B220u; // "S-%d", "IA-%d", ... (data-global)
constexpr uint32_t kLicenceBand = 0x8005B238u;      // band of block +0x4BC
constexpr uint32_t kCarInfoHeader = 0x8005B254u, kLicenceInfoHeader = 0x8005B274u; // 0x80048D14 descriptors
constexpr uint32_t kDescriptionBand = 0x8005B294u;  // band of block +0x500
constexpr uint32_t kLicenceRowText = 0x8005B32Cu, kLicenceRowBand = 0x8005B348u, kLicenceRowTable = 0x8005B364u;
constexpr uint32_t kLicenceWidget = 0x8005B39Cu;
constexpr uint32_t kMedalSprites = 0x8005B150u;     // 12-byte {uv | clut << 16, w, h, tpage}
constexpr uint32_t kLicenceView = 0x8005B470u;      // view: +0C colour 0xF07800, +10 title (0)
constexpr uint32_t kHeaderBand = 0x8005ABE4u, kHeaderText = 0x8005AC00u, kHeaderValueTarget = 0x8005AC1Cu;
constexpr uint32_t kCourseBand = 0x8005ABACu, kCourseText = 0x8005ABC8u; // 0x80048BD8
constexpr uint32_t kEventRowText = 0x8005D20Cu, kEventRowBand = 0x8005D228u, kEventRowTable = 0x8005D244u, kEventRowOffsets = 0x8005D27Cu;
constexpr uint32_t kTestRowTable = 0x8005D2B8u, kTestRowOffsets = 0x8005D2E8u;
constexpr uint32_t kEventWidget = 0x8005D284u;
// data-race strings.
constexpr uint32_t kStrLicenseTest = 0x801C7028u, kStrPowerFormat = 0x801C70E9u, kStrLaunchFormat = 0x801C70EEu;
constexpr uint32_t kStrDriveFf = 0x801C70D2u, kStrDriveFr = 0x801C70D7u, kStrDriveMr = 0x801C70DCu, kStrDrive4wd = 0x801C70E0u, kStrDriveRr = 0x801C70E4u;

constexpr uint16_t kMarkerClip = 0x7FFF;

uint32_t Lerp(const GuestImage& o, uint32_t a, uint32_t b, int t, int max) { return MenuListLerp(o.Get<uint32_t>(a), o.Get<uint32_t>(b), t, max); }

void AddSprites(MenuOtSlot& ot, const std::vector<HudFontSprite>& glyphs, uint32_t colour, int mode) {
    const bool semi = (colour & 0x2000000u) != 0;
    for (const HudFontSprite& g : glyphs) {
        const uint16_t e1 = uint16_t(g.tpage | (semi ? (mode & 3) << 5 : 0));
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

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour) { // 0x8007D024: TILE 0x60 ^ colour
    MenuPrim p;
    p.kind = MenuPrim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

std::string Format(const std::string& format, int value) {
    char buffer[128];
    std::snprintf(buffer, sizeof(buffer), format.c_str(), value);
    return buffer;
}

// 0x8006BA48: POLY_F3 0x22 apex (x + w, y), base (x, y + h) / (x, y - h); colour as 0x8006B988.
void AddSideArrow(MenuOtSlot& ot, int x, int y, int w, int h, int t) {
    int k = 40 - t;
    if (k > 10) k = 10;
    if (k < 0) k = 0;
    const int r = (k * 255) / 10;
    MenuPrim p;
    p.kind = MenuPrim::kPolyF4;
    p.semi = true;
    p.x[0] = int16_t(x + w), p.y[0] = int16_t(y);
    p.x[1] = int16_t(x), p.y[1] = int16_t(y + h);
    p.x[2] = int16_t(x), p.y[2] = int16_t(y - h);
    p.x[3] = p.x[2], p.y[3] = p.y[2];
    for (uint32_t& c : p.colour) c = uint32_t(r) | uint32_t(r >> 1) << 8;
    ot.Add(p);
}

// 0x80054B9C(buffer, value, format): a setting value as text by format & 0xF (0 "%d", 1 "%d.%d" of v / 10, 2
// "%d.%02d" of v / 100, 3 "%d.%03d" of v / 1000, 4 as 2 of v * 5); negative values get a '-'.
std::string SettingNumberText(int v, int format) {
    std::string out;
    if (v < 0) out = "-", v = -v;
    char b[32] = {};
    switch (format & 0xF) {
    case 0: std::snprintf(b, sizeof(b), "%d", v); break;
    case 1: std::snprintf(b, sizeof(b), "%d.%d", v / 10, v - (v / 10) * 10); break;
    case 3: std::snprintf(b, sizeof(b), "%d.%03d", v / 1000, v - (v / 1000) * 1000); break;
    case 4: v *= 5; [[fallthrough]];
    case 2: std::snprintf(b, sizeof(b), "%d.%02d", v / 100, v - (v / 100) * 100); break;
    default: break;
    }
    return out + b;
}

// 0x8006BEB4: a band's alpha (anim, ~anim while closing, 0 closed) * 128 / steps.
int BandAlpha(const Band& b) {
    const int k = b.anim >= 0 ? b.anim : b.anim < -1 ? ~b.anim : 0;
    return b.steps ? (k << 7) / b.steps : 0;
}

// A band of a template, settled (anim = steps).
Band SettledBand(const GuestImage& o, uint32_t address) {
    Band b = Band::Read(o, address);
    b.anim = b.steps;
    return b;
}

// 0x80048D14 / 0x80048E84: the "CAR INFO" / "LICENSE INFO" headers (0x50 bytes: band +0x00, text object +0x1C,
// +0x44 value (string / time / number), +0x48 value colour, +0x4C s16 value fade 0..16, +0x4E flags: bits 0..1 value
// kind 0 text / 1 time / 2 number, bit 2 the subtractive band behind).
struct InfoHeader {
    Band band;
    TextObject text;
    std::string value;
    uint32_t valueTime = 0;
    uint32_t valueColour = 0;
    int16_t fade = 16;
    uint16_t flags = 0;

    static InfoHeader FromDescriptor(const RaceMenuAssets& a, uint32_t d) {
        const GuestImage& o = a.ovl0;
        InfoHeader h;
        h.flags = o.Get<uint16_t>(d + 0x18);
        const int16_t steps = o.Get<int16_t>(d + 0x1E);
        h.text = TextObject::FromTemplate(o, kHeaderText, a.Text(o.Get<uint32_t>(d + 8)));
        h.text.steps = steps;
        h.text.c0 = o.Get<uint32_t>(d + 0xC);
        h.band = Band::Read(o, kHeaderBand);
        h.band.steps = steps;
        h.band.w = o.Get<int16_t>(d + 0x1C);
        h.band.c0 = o.Get<uint32_t>(d + 0);
        h.band.c1 = o.Get<uint32_t>(d + 4);
        h.value = a.Text(o.Get<uint32_t>(d + 0x10));
        h.valueColour = o.Get<uint32_t>(d + 0x14);
        // Opened and settled.
        h.text.Open();
        h.text.SetSettled();
        h.band.anim = h.band.steps;
        return h;
    }

    void Draw(const RaceMenuAssets& a, MenuOt& ot, int slot, int x, int y) const {
        int u = fade;
        if (u < -1) u = ~u;
        if (u > 0) {
            const uint32_t colour = MenuListLerp(valueColour, a.ovl0.Get<uint32_t>(kHeaderValueTarget), 16 - u, 16);
            const int right = x + band.w;
            std::vector<HudFontSprite> glyphs;
            if ((flags & 3) == 0) {
                const HudFont& f = a.FontAt(0x801C9150u);
                const int w = f.TextWidth(value, 1);
                f.Text(value, right - (w + 6), y + 0xD, 1, glyphs);
            } else if ((flags & 3) == 1) {
                const HudFont& f = a.FontAt(0x801C9120u);
                const std::string s = FormatRaceTime(valueTime);
                const int w = f.TimeWidth(s, 6, 5, 0);
                f.Time(s, right - (w + 6), y + 9, 6, 5, 0, 0, glyphs);
            } else {
                a.FontAt(0x801C9120u).NumberRight(value, right - 6, y + 9, 1, -2, 0, glyphs);
            }
            AddSprites(ot[slot], glyphs, colour, 1);
        }
        text.Draw(ot, slot, x + 6, y + 9, a.FontAt(text.font));
        const int top = y - (band.h >> 1);
        band.Draw(ot[slot + 1], x, top);
        ot[slot + 1].DrawMode(0x220);
        if (flags & 4) {
            Band shade = band;
            shade.c0 = 0x02C0C0C0u;
            shade.c1 = 0;
            shade.targetOut = 0;
            shade.Draw(ot[slot + 1], x, top);
            ot[slot + 1].DrawMode(0x240);
        }
    }
};

} // namespace

// ---------------------------------------------------------------- ordering table

MenuOt::MenuOt() {
    // A marker packet first in every slot (so it comes out last): its draw mode after Emit is the slot's final mode.
    for (MenuOtSlot& s : slots_) {
        MenuPrim marker;
        marker.clipX0 = int16_t(kMarkerClip);
        s.Add(marker);
    }
}

void MenuOt::Emit(std::vector<MenuPrim>& gpuOrder, uint16_t mode) const {
    for (int i = kSlots - 1; i >= 0; i--) {
        std::vector<MenuPrim> part;
        slots_[size_t(i)].Emit(part, mode);
        if (part.empty() || part.back().clipX0 != int16_t(kMarkerClip)) throw std::logic_error("MenuOt: marker lost");
        mode = part.back().tpage;
        part.pop_back();
        gpuOrder.insert(gpuOrder.end(), part.begin(), part.end());
    }
}

// ---------------------------------------------------------------- text object

TextObject TextObject::FromTemplate(const GuestImage& o, uint32_t a, std::string text) {
    TextObject t;
    t.revealDivisor = o.Get<uint8_t>(a + 0);
    t.waveDivisor = o.Get<uint8_t>(a + 1);
    t.steps = o.Get<int16_t>(a + 2);
    t.glowSpread = o.Get<int16_t>(a + 4);
    t.period = o.Get<int16_t>(a + 6);
    t.fadeSteps = o.Get<int16_t>(a + 8);
    t.extra = o.Get<int8_t>(a + 0xA);
    t.height = o.Get<uint8_t>(a + 0xB);
    t.flags = o.Get<uint16_t>(a + 0xC);
    t.font = o.Get<uint32_t>(a + 0x10);
    t.c0 = o.Get<uint32_t>(a + 0x14);
    t.c1 = o.Get<uint32_t>(a + 0x18);
    t.text = std::move(text);
    t.alpha = 128;
    t.anim = -1;
    return t;
}

void TextObject::Open(int settleAt) {
    anim = 0;
    length = int16_t(text.size());
    settle = int16_t(steps + (revealDivisor ? int(length) / int(revealDivisor) : 0));
    if (settleAt >= 0) settle = int16_t(settleAt);
}

void TextObject::Restart() {
    Open(-1);
    anim = settle;
}

void TextObject::Close() {
    anim = int16_t(~steps);
    length = int16_t(text.size());
}

void TextObject::Tick() {
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    anim++;
    if (!(anim < settle + period)) anim = settle;
}

int TextObject::Width(const HudFont& f) const {
    int w = 0;
    for (size_t i = 0; i < text.size(); i++) {
        const uint8_t c = uint8_t(text[i]);
        if ((c >= '0' && c <= '9') || c == ' ') w += f.cell;
        else w += f.Advance(c, i + 1 < text.size() ? uint8_t(text[i + 1]) : uint8_t(0));
        w += extra;
    }
    return w;
}

void TextObject::Draw(MenuOt& ot, int slot, int x, int y, const HudFont& f, bool thickUnderline) const {
    int count = anim * int(revealDivisor); // local_50
    if ((flags & 0x180) == 0x80) x -= Width(f) >> 1;
    else if ((flags & 0x180) == 0x100) x -= Width(f);
    const int tileHeight = thickUnderline ? 2 : 1;
    auto channel = [](uint32_t c, int k) { return int((c >> (8 * k)) & 0xFF); };
    if (anim < 0) {
        if (anim < -1) { // closing: two gradient quads shrinking to the text's middle
            const int k = steps + 1 + anim;
            const int width = Width(f);
            const int s = steps;
            const int half = int(int8_t(height)) >> 1;
            const int rest = s - k;
            const int spread = (glowSpread * k) / s;
            const uint32_t colour = uint32_t(channel(c0, 0) * rest / s) | uint32_t(channel(c0, 1) * rest / s) << 8 | uint32_t(channel(c0, 2) * rest / s) << 16;
            const int part = (width * k) / s;
            const int hh = half - (half * k) / s;
            const int qw = spread + width - part;
            const int qy = y - half - hh;
            AddGradientQuad(ot[slot], x + part, qy, qw, hh * 2, 0, colour);
            AddGradientQuad(ot[slot], x - spread, qy, qw, hh * 2, colour, 0);
            ot[slot].DrawMode(0x20);
        }
        return;
    }
    const int cell = f.cell;
    const uint32_t semiBit = (flags & 0x20) ? 0x2000000u : 0;
    const int mode = flags & 3;
    for (int i = 0; i < length && i < int(text.size()); i++) {
        bool glow = ((flags >> 6) & 1) != 0;
        int v8, v4;
        if (anim < settle) {
            v8 = revealDivisor ? count / int(revealDivisor) : 0;
            v4 = v8;
        } else {
            count = 1;
            v8 = fadeSteps;
            glow = false;
            v4 = steps;
            if (flags & 8) {
                v8 = (anim - settle) - i / int(waveDivisor);
                v4 = v8;
            }
        }
        if (count < 1) return;
        const int s = steps;
        if (s < v4) {
            glow = false;
            v4 = s;
        }
        if (v4 < 0) v4 = s;
        v4 = s - v4;
        int v10 = v4, div = s;
        if (settle <= anim) {
            const int fs = fadeSteps;
            if (fs < v8) v8 = fs;
            if (v8 < 0) v8 = fs;
            v10 = fs - v8;
            div = fs;
        }
        int rgb[3];
        for (int k = 0; k < 3; k++) rgb[k] = ((channel(c0, k) + ((channel(c1, k) - channel(c0, k)) * v10) / div) * int(alpha)) >> 8;
        const uint32_t colour = uint32_t(rgb[0]) | uint32_t(rgb[1]) << 8 | uint32_t(rgb[2]) << 16;
        const uint8_t c = uint8_t(text[size_t(i)]);
        const uint8_t next = size_t(i) + 1 < text.size() ? uint8_t(text[size_t(i) + 1]) : uint8_t(0);
        int advance;
        if (c == ' ') {
            advance = cell + extra;
        } else {
            std::vector<HudFontSprite> glyphs;
            int base = cell;
            if ((flags & 0x10) == 0) {
                if (c >= '0' && c <= '9') {
                    f.Glyph(uint32_t(c) | HudFont::kRightAlign, x + ((cell + extra) >> 1), y, glyphs);
                } else {
                    f.Glyph(c, x, y, glyphs);
                    base = f.Advance(c, next);
                }
            } else {
                f.Glyph(uint32_t(c) | HudFont::kCentre, x, y, glyphs);
            }
            AddSprites(ot[slot + 1], glyphs, colour | semiBit, mode);
            advance = base + extra;
            if (glow) {
                const int off = (glowSpread * v4) / s;
                const uint32_t g = uint32_t(rgb[0] * v4 / s) | uint32_t(rgb[1] * v4 / s) << 8 | uint32_t(rgb[2] * v4 / s) << 16 | 0x2000000u;
                std::vector<HudFontSprite> copies;
                f.Glyph(c, x - off, y, copies);
                f.Glyph(c, x + off, y, copies);
                AddSprites(ot[slot], copies, g, 1);
            }
        }
        if (flags & 4) ot[slot].Add(Tile(x, y, advance, tileHeight, colour << 1));
        x += advance;
        count--;
    }
}

// ---------------------------------------------------------------- shared

void AddRaceViewHeader(MenuOt& ot, const RaceMenuAssets& assets, const std::string& title, uint32_t colour, int alpha, bool entering) {
    MenuOtSlot& s = ot[4];
    if (!title.empty()) {
        const HudFont& font = assets.FontAt(0x801C9130u);
        const int spacing = entering ? 0x22 - (alpha >> 2) : 2;
        const int width = font.TextWidth(title, spacing);
        const int x = (0x160 - width) >> 1;
        s.Add(Tile(x + 1, 0x4E, width, 2, uint32_t((alpha * 0xFF) >> 7)));
        const uint32_t grey = uint32_t((alpha * 0x66) >> 7);
        std::vector<HudFontSprite> glyphs;
        font.Text(title, x, 0x4C, spacing, glyphs);
        AddSprites(s, glyphs, grey | grey << 8 | grey << 16, 0);
        glyphs.clear();
        font.Text(title, x + 2, 0x4F, spacing, glyphs);
        AddSprites(s, glyphs, 0, 0);
    }
    const uint32_t c = uint32_t(((colour & 0xFF) * uint32_t(alpha)) >> 7) | uint32_t((((colour >> 8) & 0xFF) * uint32_t(alpha)) >> 7) << 8 |
                       uint32_t((((colour >> 16) & 0xFF) * uint32_t(alpha)) >> 7) << 16;
    const int half = (alpha * 0x2D) >> 7;
    MenuPrim g; // 0x8007E0B0: POLY_G4 0x3A
    g.kind = MenuPrim::kPolyG4;
    g.semi = true;
    g.gouraud = true;
    g.x[0] = 0, g.y[0] = int16_t(0x2D - half);
    g.x[1] = 0x160, g.y[1] = int16_t(0x2D - half);
    g.x[2] = 0, g.y[2] = int16_t(0x2D + half);
    g.x[3] = 0x160, g.y[3] = int16_t(0x2D + half);
    g.colour[0] = g.colour[1] = c;
    g.colour[2] = g.colour[3] = 0;
    s.Add(g);
    s.DrawMode(0x220);
}

// ---------------------------------------------------------------- licence test menu

LicenceMenuState LicenceMenuDefaults(const RaceMenuAssets& assets, int licence, int test) {
    LicenceMenuState s;
    s.licence = licence;
    s.test = test;
    const RaceMenuAssets::LicenceInfo info = assets.Licence(licence, test);
    s.title = info.title;
    s.description = info.lines;
    return s;
}

// ---- the licence test menu as a view (race_menu_views.h)

namespace {
const MenuListPad kNoPad{};
constexpr uint32_t kTransmissionBar = 0x8005AC20u, kExitBar = 0x8005ACF0u; // EXE bar templates in ovl0 (0x8004ED00 / 0x80057EAC)

void SettleLabel(ResultLabel& l) {
    l.Open();
    l.band.anim = l.band.steps;
    l.text.SetSettled();
    l.fade = 16;
}
} // namespace

LicenceMenuView::LicenceMenuView(const RaceMenuAssets& a) : a_(a) {
    list = MenuListWidget::Read(a.ovl0, kLicenceWidget);
    AttachList();
    bar.anim = -1;
}

void LicenceMenuView::AttachList() {
    list.callback = [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); };
}

std::string LicenceMenuView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kLicenceView + 0x10)); }
uint32_t LicenceMenuView::Colour() const { return a_.ovl0.Get<uint32_t>(kLicenceView + 0x0C); }

void LicenceMenuView::Setup(const LicenceMenuState& in, bool replay, bool again) { // 0x8004ED00
    const GuestImage& o = a_.ovl0;
    counter = 12;
    dialog = 0;
    rowEnabled[kLicenceReplay] = replay;     // 0x8005B370
    rowEnabled[kLicenceSaveReplay] = replay; // 0x8005B388
    info = in;
    if (!again) {
        list = MenuListWidget::Read(o, kLicenceWidget);
        MenuListReset(list, [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); });
        list.selection = kLicenceStart;
    }
    carLabel = ResultLabel::Init(a_, kCarInfoHeader);     // 0x80048D14(W + 0x418, 0x8005B254)
    licenceLabel = ResultLabel::Init(a_, kLicenceInfoHeader);
    carInfoFade = 8;                                       // block + 0x51C
    bar = ResultBar::Init(a_, kTransmissionBar, 0);        // 0x8006E1CC(W + 0xEC, 0x8005AC20, 0)
    bar.x = 0xB0, bar.y = 0x19A;
    band = Band::Read(o, kLicenceBand);
    band.anim = 0;
    descBand = Band::Read(o, kDescriptionBand);
    descBand.anim = 0;
    action_ = 0;
    testChanged = false;
}

int32_t LicenceMenuView::ListCallback(int command, int row, const MenuListRowDraw* d) { // 0x8004D474
    if (row < 0 || row >= kLicenceRows) return 0;
    TextObject& text = rowText[size_t(row)];
    Band& b = rowBand[size_t(row)];
    switch (command) {
    case kMenuListReset:
        text = TextObject::FromTemplate(a_.ovl0, kLicenceRowText, a_.Text(a_.ovl0.Get<uint32_t>(kLicenceRowTable + uint32_t(row) * 8)));
        b = Band::Read(a_.ovl0, kLicenceRowBand);
        b.anim = -1;
        break;
    case kMenuListReveal: text.Open(-1), b.anim = 0; break;
    case kMenuListClose: text.Close(), b.anim = int16_t(~b.steps); break;
    case kMenuListTick: text.Tick(), b.Tick(); break;
    case kMenuListDraw:
        if (row != 0 && d && drawOt_) {
            TextObject t = text;
            t.alpha = uint8_t(d->alpha);
            t.Draw(*drawOt_, 0, d->x, d->y + 0x10, a_.FontAt(t.font));
            b.Draw((*drawOt_)[1], d->x - 8, d->y - 4);
            (*drawOt_)[1].DrawMode(0x220);
        }
        break;
    case kMenuListLeave: text.c0 = 0x707070, text.flags &= 0xFFF7; break;
    case kMenuListEnter: text.c0 = 0x907040, text.flags |= 8, text.Restart(); break;
    case kMenuListOpen: text.c0 = 0x907040, text.flags |= 8; break;
    case kMenuListEnabled: return rowEnabled[size_t(row)] ? 1 : 0;
    default: break;
    }
    return 0;
}

void LicenceMenuView::Close() { // 0x8004EEB0's leaving part
    MenuListClose(list);
    band.anim = int16_t(~band.steps);
    descBand.anim = int16_t(~descBand.steps);
    carLabel.Close();
    licenceLabel.Close();
}

int LicenceMenuView::Update(const MenuListPad* pad, bool inputArg) { // 0x8004EEB0
    sounds.clear();
    testChanged = false;
    if (++arrowPhase > 0x2D) arrowPhase = 0;
    input = inputArg;
    if (counter > 0 && --counter == 0) { // the objects open
        MenuListOpen(list);
        list.revealPeriod = -1;
        carLabel.Open();
        licenceLabel.Open();
    }
    if (carInfoFade > 0) carInfoFade--;
    band.Tick();
    descBand.Tick();
    carLabel.Tick();
    licenceLabel.Tick();
    const MenuListPad* buttons = inputArg ? (pad ? pad : &kNoPad) : nullptr;
    bool leave = false;
    if (dialog == 1) { // the TRANSMISSION bar
        MenuListUpdate(list, nullptr);
        const int r = bar.Update(buttons, &sounds);
        if (r == -1) {
            sounds.push_back(2);
            dialog = 0;
        } else if (r >= 0 || r <= -4) {
            transmission = r;
            lastTransmission = uint8_t(r);
            sounds.push_back(3);
            input = false;
            action_ = 1;
            leave = true;
        }
    } else {
        bar.Update(nullptr, &sounds);
        const int r = MenuListUpdate(list, buttons);
        if (r == -3) {
            sounds.push_back(6);
        } else if (r == -4 || r == -1) {
            sounds.push_back(0);
        } else if (r == -2) {
            if (list.selection == 0 && buttons) { // the selector: left / right (pressed or repeated) change the test
                const uint32_t bits = buttons->pressed | buttons->repeat;
                if (bits & menu_list_pad::kLeft) {
                    sounds.push_back(5);
                    info.test = info.test - 1 < 0 ? 9 : info.test - 1;
                    carInfoFade = 8;
                    testChanged = true;
                }
                if (bits & menu_list_pad::kRight) {
                    sounds.push_back(5);
                    info.test = info.test + 1 > 9 ? 0 : info.test + 1;
                    carInfoFade = 8;
                    testChanged = true;
                }
            }
        } else if (r == 0) { // the selector chosen: the selection goes to Start
            sounds.push_back(1);
            MenuListSelect(list, kLicenceStart);
        } else if (r == kLicenceStart) { // the TRANSMISSION bar opens on the last choice
            sounds.push_back(1);
            bar.cursor = int8_t(lastTransmission != 0 ? 1 : 0);
            bar.Open();
            dialog = 1;
        } else if (r > 0 && r < kLicenceRows) {
            sounds.push_back(3);
            action_ = a_.ovl0.Get<int8_t>(kLicenceRowTable + uint32_t(r) * 8 + 5);
            input = false;
            leave = true;
        }
    }
    if (!leave) return 0;
    Close();
    return 1;
}

void LicenceMenuView::Draw(MenuOt& ot) const { // 0x8004F474
    const RaceMenuAssets& a = a_;
    const GuestImage& o = a.ovl0;
    const HudFont& small = a.FontAt(0x801C9120u);
    bar.Draw(a, ot); // 0x8006E5B8(W + 0xEC, ot)
    const int alpha = BandAlpha(band);
    std::vector<HudFontSprite> glyphs;
    auto text = [&](int slot, const HudFont& f, const std::string& s, int x, int y, uint32_t colour, int mode) {
        glyphs.clear();
        f.Text(s, x, y, 1, glyphs);
        AddSprites(ot[slot], glyphs, colour, mode);
    };
    {
        const std::string label = a.Text(kStrLicenseTest);
        glyphs.clear();
        small.TextRight(label, 0x154, 0x38, 1, glyphs);
        AddSprites(ot[0], glyphs, Lerp(o, kColourBase, kColourText, alpha, 0x80), 1);
    }
    ot[0].Add(Tile(0, 0x38, 0x160, 2, Lerp(o, kColourRedBase, kColourRed, alpha, 0x80)));
    if (input) text(0, small, info.title, 0x5E, 0x68, 0x808080, 0); // 0x8004CFC0 (English; view + 0x18 != 0)
    band.Draw(ot[2], 0, 0x50);
    ot[2].DrawMode(0x220);

    // The list (0x8006D50C) with the row callback 0x8004D474.
    drawOt_ = &ot;
    MenuListDraw(list, ot[0]);
    drawOt_ = nullptr;

    carLabel.Draw(a, ot, 0, 0x78, 0x9E);
    licenceLabel.Draw(a, ot, 0, 0x78, 0xEC);

    // 0x8004D7D0: car and licence info at max(0, 0x80 - fade * 0x80 / 8) of the band's alpha (block + 0x51C).
    {
        const int x = 0x78;
        int fadeIn = carInfoFade * 0x80;
        if (fadeIn < 0) fadeIn += 7;
        const int infoAlpha = (std::max(0, 0x80 - (fadeIn >> 3)) * alpha) >> 7;
        const uint32_t colour = Lerp(o, kColourBase, kColourCarInfo, infoAlpha, 0x80);
        text(0, small, info.carName, x + 10, 0xC0, colour, 1);
        glyphs.clear();
        small.Number(Format(a.Text(kStrPowerFormat), int(info.carPower) * 1000 / 0x3F6), x + 100, 0xD6, 1, -3, 0, glyphs);
        AddSprites(ot[0], glyphs, colour, 1);
        uint32_t drive = kStrDriveFf;
        switch (info.carDrive) {
        case 0: drive = kStrDriveFr; break;
        case 2: drive = kStrDrive4wd; break;
        case 3: drive = kStrDriveMr; break;
        case 4: drive = kStrDriveRr; break;
        default: break;
        }
        text(0, small, a.Text(drive), x + 0x10, 0xD6, colour, 1);
        text(0, small, Format(a.Text(kStrLaunchFormat), int(uint32_t(info.launchSpeed) * 10000u / 0x3EDDu)), x + 10, 0x10E, colour, 1);
        static constexpr uint16_t kChips[3] = {0x1AD8, 0x5A6E, 0x0935};
        for (int m = 0; m < 3; m++) { // 0x8004D6E4
            const int right = x + 0x48 * (m + 1);
            glyphs.clear();
            const std::string time = FormatRaceTime(info.medalTimes[size_t(m)]);
            const int w = small.TimeRight(time, right, 0x124, 6, 5, 0, 0, glyphs);
            AddSprites(ot[0], glyphs, Lerp(o, kColourBase, kColourTimes, infoAlpha, 0x80), 1);
            MenuListPaintChip(ot[0], right - w - 10, 0x116, 8, 0xE, kChips[m], infoAlpha);
        }
    }
    for (int y : {0xC0, 0xD6, 0x10E, 0x124}) ot[0].Add(Tile(0x7E, y, 0x160, 1, Lerp(o, kColourBase, kColourRules, alpha, 0x80))); // 0x8004F408

    // 0x8004DAD4: the ten tests.
    for (int i = 0, x = 0x78; i < 10; i++, x += 0x16) {
        const uint32_t colour = Lerp(o, kColourBase, i == info.test ? kColourBoxSelected : kColourBox, alpha, 0x80);
        MenuListFrame(ot[0], colour, x, 0x72, 0x15, 0x1D);
        const int medal = int(info.medals[size_t(i)]) - 1;
        if (medal >= 0 && medal < 3) {
            const uint32_t e = kMedalSprites + uint32_t(medal) * 12;
            MenuPrim p; // 0x80081478: SPRT 0x64 of the alpha grey
            p.kind = MenuPrim::kSprite;
            p.w = o.Get<int16_t>(e + 4), p.h = o.Get<int16_t>(e + 6);
            p.x[0] = int16_t(x - (p.w >> 1) + 10), p.y[0] = int16_t(0x72 - (p.h >> 1) + 14);
            p.u = o.Get<uint8_t>(e), p.v = o.Get<uint8_t>(e + 1);
            p.clut = o.Get<uint16_t>(e + 2);
            const uint32_t g = uint32_t(alpha);
            p.colour[0] = g | g << 8 | g << 16;
            p.tpage = uint16_t(o.Get<uint16_t>(e + 8) | 0x20);
            ot[0].Add(p);
            ot[0].DrawMode(p.tpage);
        }
    }

    // 0x8004CB6C while the list is not closed: the licence label ("B-1") at the list's fade, the arrows while row 0 is selected.
    if (list.state != -1) {
        const HudFont& big = a.FontAt(0x801C9130u);
        const int listAlpha = list.fadeMax ? (int(list.fade) << 7) / list.fadeMax : 0;
        const std::string label = Format(a.Text(o.Get<uint32_t>(kLicenceLabelFormats + uint32_t(info.licence) * 4)), info.test + 1);
        glyphs.clear();
        const int w = big.Number(label, 0x14, 0x6C, 2, 1, 4, glyphs);
        AddSprites(ot[0], glyphs, Lerp(o, kColourBase, kColourLabel, listAlpha, 0x80), 1);
        if (list.selection == 0) {
            AddSideArrow(ot[0], 0x14 + w + 5, 0x6C - 0x16, 6, 10, arrowPhase);
            AddSideArrow(ot[0], 0x14 - 4, 0x6C - 0x16, -6, 10, arrowPhase);
        }
        ot[0].DrawMode(0x220);
    }

    // Slot 1: the description (0x8004D2EC, English; view + 0x18 != 0) over its band (block + 0x500), a third while the bar is up.
    int descAlpha = alpha;
    Band desc = descBand;
    desc.c0 = 0x5A4A3E, desc.c1 = 0x3E362A;
    if (dialog == 1) {
        descAlpha = alpha / 3;
        desc.c0 = 0x26201A, desc.c1 = 0x1A150F;
    }
    if (input)
        for (size_t i = 0; i < info.description.size(); i++) {
            const int y = 0x146 + int(i) * 0x16;
            text(1, small, info.description[i], 0xF, y, Lerp(o, kColourBase, kColourText, descAlpha, 0x80), 1);
            ot[1].Add(Tile(0xF, y, 0x144, 1, Lerp(o, kColourBase, kColourRules, descAlpha, 0x180)));
            ot[1].DrawMode(0x20);
        }
    desc.Draw(ot[1], 0, 300);
    ot[1].DrawMode(0x220);
}

std::unique_ptr<LicenceMenuView> SettledLicenceMenuView(const RaceMenuAssets& a, const LicenceMenuState& st) {
    auto v = std::make_unique<LicenceMenuView>(a);
    const GuestImage& o = a.ovl0;
    v->info = st;
    v->counter = 0;
    v->input = true;
    v->dialog = st.dimDescription ? 1 : 0;
    v->carInfoFade = int16_t(st.carInfoFade);
    v->arrowPhase = int16_t(st.arrowPhase);
    v->rowEnabled = st.rowEnabled;
    v->list = MenuListWidget::Read(o, kLicenceWidget);
    v->AttachList();
    v->list.selection = int16_t(std::clamp(st.selectedRow, 0, int(v->list.count) - 1));
    v->list.state = 0;
    v->list.active = 0;
    v->list.fade = int16_t((st.listAlpha * v->list.fadeMax) / 128);
    for (int row = 0; row < kLicenceRows; row++) {
        TextObject& t = v->rowText[size_t(row)];
        t = TextObject::FromTemplate(o, kLicenceRowText, a.Text(o.Get<uint32_t>(kLicenceRowTable + uint32_t(row) * 8)));
        t.Open();
        if (row == v->list.selection) { // 0x8004D474 cases 6 / 7
            t.c0 = 0x907040;
            t.flags |= 8;
            t.SetSettled(st.flashPhase);
        } else { // case 5
            t.c0 = 0x707070;
            t.SetSettled();
        }
        v->rowBand[size_t(row)] = SettledBand(o, kLicenceRowBand);
    }
    v->carLabel = ResultLabel::Init(a, kCarInfoHeader);
    SettleLabel(v->carLabel);
    v->licenceLabel = ResultLabel::Init(a, kLicenceInfoHeader);
    SettleLabel(v->licenceLabel);
    v->band = Band::Read(o, kLicenceBand);
    v->band.anim = int16_t((st.alpha * v->band.steps) / 128);
    v->descBand = SettledBand(o, kDescriptionBand);
    if (st.transmissionBar) v->bar = *st.transmissionBar;
    else v->bar.anim = -1;
    return v;
}

std::vector<MenuPrim> BuildLicenceMenuFrame(const RaceMenuAssets& a, const LicenceMenuState& st) {
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    // 0x80047024 with the view 0x8005B470 (no title), then 0x8004F474.
    AddRaceViewHeader(ot, a, a.Text(a.ovl0.Get<uint32_t>(kLicenceView + 0x10)), a.ovl0.Get<uint32_t>(kLicenceView + 0x0C), st.headerAlpha);
    SettledLicenceMenuView(a, st)->Draw(ot);
    ot.Emit(prims, 0x200);
    return prims;
}

// ---------------------------------------------------------------- event pre-race menu

EventMenuView::EventMenuView(const RaceMenuAssets& a) : a_(a) {
    list = MenuListWidget::Read(a.ovl0, kEventWidget);
    AttachList();
    transmissionBar.anim = exitBar.anim = -1;
    constexpr uint32_t kEventView = 0x8005D36Cu;
    title = a.Text(a.ovl0.Get<uint32_t>(kEventView + 0x10));
    titleColour = a.ovl0.Get<uint32_t>(kEventView + 0x0C);
}

void EventMenuView::AttachList() {
    list.callback = [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); };
}

void EventMenuView::Setup(const EventMenuState& in, bool replay, bool withCar, bool again) { // 0x80057EAC
    const GuestImage& o = a_.ovl0;
    counter = 16;
    carShown = -1;
    dialog = 0;
    exitBar = ResultBar::Init(a_, kExitBar, 0);
    exitBar.x = 0xB0, exitBar.y = 0x19A;
    transmissionBar = ResultBar::Init(a_, kTransmissionBar, 0);
    transmissionBar.x = 0xB0, transmissionBar.y = 0x19A;
    if (!again) settingsChosen = false;
    if (!in.title.empty()) title = in.title;
    if (in.titleColour != 0xFFFFFFFFu) titleColour = in.titleColour;
    rowEnabled[kEventReplay] = !settingsChosen && replay;     // 0x8005D248
    rowEnabled[kEventSaveReplay] = rowEnabled[kEventReplay];  // 0x8005D260
    const int16_t keep = list.selection;
    list = MenuListWidget::Read(o, kEventWidget);
    list.count = list.visible = 6; // outside race mode 10: row 5 is the Exit of row 6
    MenuListReset(list, [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); });
    if (again) list.selection = keep;
    car = withCar;
    if (car) carCamera = menu::ModelViewCamera(200, 200);
    course = CourseTitle::Init(a_, in.course);
    course.Open();
    action_ = 0;
}

int32_t EventMenuView::ListCallback(int command, int row, const MenuListRowDraw* d) { // 0x80057A70
    if (row < 0 || row >= kEventRows) return 0;
    // 0x80057EAC outside mode 10: row 5 takes row 6's string, code and y offset; the machine tests (modes 7..9) their own rows.
    const int entry = (!ghostOptions && !machineTest && row == 5) ? 6 : row;
    const uint32_t table = machineTest ? kTestRowTable : kEventRowTable, offsets = machineTest ? kTestRowOffsets : kEventRowOffsets;
    TextObject& text = rowText[size_t(row)];
    Band& b = rowBand[size_t(row)];
    switch (command) {
    case kMenuListReset:
        text = TextObject::FromTemplate(a_.ovl0, kEventRowText, a_.Text(a_.ovl0.Get<uint32_t>(table + uint32_t(entry) * 8)));
        b = Band::Read(a_.ovl0, kEventRowBand);
        b.anim = -1;
        break;
    case kMenuListReveal: text.Open(-1), b.anim = 0; break;
    case kMenuListClose: text.Close(), b.anim = int16_t(~b.steps); break;
    case kMenuListTick: text.Tick(), b.Tick(); break;
    case kMenuListDraw:
        if (d && drawOt_) {
            TextObject t = text;
            t.alpha = uint8_t(d->alpha);
            const int y = d->y + a_.ovl0.Get<int8_t>(offsets + uint32_t(entry));
            t.Draw(*drawOt_, 0, d->x, y + 0x18, a_.FontAt(t.font));
            b.Draw((*drawOt_)[1], d->x - 8, y - 2);
            (*drawOt_)[1].DrawMode(0x220);
        }
        break;
    case kMenuListLeave: text.c0 = 0x707070, text.flags &= 0xFFF7; break;
    case kMenuListEnter: text.c0 = 0x907040, text.flags |= 8, text.Restart(); break;
    case kMenuListOpen: text.c0 = 0x907040, text.flags |= 8; break;
    case kMenuListEnabled: return rowEnabled[size_t(row)] ? 1 : 0; // the (patched) table's byte of the row
    default: break;
    }
    return 0;
}

void EventMenuView::Close() {
    MenuListClose(list);
    course.Close();
    carShown = -1;
}

int EventMenuView::Update(const MenuListPad* pad, bool input) { // 0x80058108
    sounds.clear();
    if (counter > 0 && --counter == 0) {
        MenuListOpen(list);
        list.revealPeriod = -1;
        carShown = 0;
    }
    course.Tick();
    if (car) menu::TurnModelCamera(carCamera, 16, frameLength); // 0x80049874
    const MenuListPad* buttons = input ? (pad ? pad : &kNoPad) : nullptr;
    bool leave = false;
    if (dialog == 1) { // "Exit?"
        MenuListUpdate(list, nullptr);
        transmissionBar.Update(nullptr, &sounds);
        const int r = exitBar.Update(buttons, &sounds);
        if (r == 0) {
            sounds.push_back(3);
            action_ = 2;
            leave = true;
        } else if (r == -1) {
            sounds.push_back(2);
            dialog = 0;
        } else if (r > 0 || r <= -4) {
            sounds.push_back(1);
            dialog = 0;
        }
    } else if (dialog == 2) { // TRANSMISSION
        MenuListUpdate(list, nullptr);
        exitBar.Update(nullptr, &sounds);
        const int r = transmissionBar.Update(buttons, &sounds);
        if (r == -1) {
            sounds.push_back(2);
            dialog = 0;
        } else if (r >= 0 || r <= -4) {
            transmission = r;
            lastTransmission = uint8_t(r);
            sounds.push_back(3);
            action_ = dialogCode;
            leave = true;
        }
    } else {
        transmissionBar.Update(nullptr, &sounds);
        exitBar.Update(nullptr, &sounds);
        const int r = MenuListUpdate(list, buttons);
        if (r == -3) {
            sounds.push_back(6);
        } else if (r == -4 || r == -1) {
            sounds.push_back(0);
        } else if (r >= 0 && r < int(list.count)) {
            const int entry = r == 5 ? 6 : r;
            const int code = a_.ovl0.Get<int8_t>(kEventRowTable + uint32_t(entry) * 8 + 5);
            if ((code == 1 || code == 4) && !noTransmission) {
                sounds.push_back(1);
                transmissionBar.cursor = int8_t(lastTransmission != 0 ? 1 : 0);
                transmissionBar.Open();
                dialog = 2;
                dialogCode = int16_t(code);
            } else if (code == 2) {
                sounds.push_back(1);
                exitBar.Open();
                exitBar.cursor = 1;
                dialog = 1;
            } else {
                sounds.push_back(3);
                if (code == -3) settingsChosen = true; // 0x801C90F4 = 1 (view 0x8005D1C0)
                action_ = code;
                leave = true;
            }
        }
    }
    if (!leave) return 0;
    Close();
    return 1;
}

void EventMenuView::Draw(MenuOt& ot) const { // 0x800585C0
    const RaceMenuAssets& a = a_;
    transmissionBar.Draw(a, ot);
    exitBar.Draw(a, ot);
    course.Draw(a, ot, 0xB0, 0x68);
    drawOt_ = &ot;
    MenuListDraw(list, ot[0]);
    drawOt_ = nullptr;
}

std::optional<PostRaceModel> EventMenuView::Model() const {
    if (carShown < 0 || !car) return std::nullopt;
    PostRaceModel m;
    m.camera = carCamera;
    m.envX = 0x7C, m.envY = 0xA0; // 0x8008034C(M + 0xC0, (0x7C, 0xA0, 200, 200))
    return m;
}

std::unique_ptr<EventMenuView> SettledEventMenuView(const RaceMenuAssets& a, const EventMenuState& st) {
    auto v = std::make_unique<EventMenuView>(a);
    const GuestImage& o = a.ovl0;
    if (!st.title.empty()) v->title = st.title;
    if (st.titleColour != 0xFFFFFFFFu) v->titleColour = st.titleColour;
    v->counter = 0;
    v->carShown = -1;
    v->rowEnabled = st.rowEnabled;
    v->machineTest = st.machineTest;
    v->ghostOptions = st.ghostOptions;
    v->list = MenuListWidget::Read(o, kEventWidget);
    v->AttachList();
    if (!st.ghostOptions) v->list.count = v->list.visible = 6;
    v->list.selection = int16_t(std::clamp(st.selectedRow, 0, int(v->list.count) - 1));
    v->list.state = 0;
    v->list.active = 0;
    const uint32_t table = st.machineTest ? kTestRowTable : kEventRowTable;
    for (int row = 0; row < kEventRows; row++) {
        const int entry = (!st.ghostOptions && !st.machineTest && row == 5) ? 6 : row;
        TextObject& t = v->rowText[size_t(row)];
        t = TextObject::FromTemplate(o, kEventRowText, a.Text(o.Get<uint32_t>(table + uint32_t(entry) * 8)));
        t.Open();
        if (row == v->list.selection) {
            t.c0 = 0x907040;
            t.flags |= 8;
            t.SetSettled(st.flashPhase);
        } else {
            t.c0 = 0x707070;
            t.SetSettled();
        }
        v->rowBand[size_t(row)] = SettledBand(o, kEventRowBand);
    }
    v->course = CourseTitle::Init(a, st.course);
    v->course.Open();
    v->course.text.SetSettled();
    v->course.band.anim = v->course.band.steps;
    if (st.transmissionBar) v->transmissionBar = *st.transmissionBar;
    if (st.exitBar) v->exitBar = *st.exitBar;
    return v;
}

std::vector<MenuPrim> BuildEventMenuFrame(const RaceMenuAssets& a, const EventMenuState& st) {
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    const std::unique_ptr<EventMenuView> v = SettledEventMenuView(a, st);
    AddRaceViewHeader(ot, a, v->title, v->titleColour, st.headerAlpha);
    v->Draw(ot);
    ot.Emit(prims, 0x200);
    return prims;
}


// ---------------------------------------------------------------- settings: CHANGE PARTS

std::vector<MenuPrim> BuildPartsPageFrame(const RaceMenuAssets& a, const PartsPageState& st) {
    const GuestImage& o = a.ovl0;
    constexpr uint32_t kView = 0x8005D1C0u, kGroupTables[3] = {0x8005C364u, 0x8005C384u, 0x8005C3A4u};
    constexpr uint32_t kIcons = 0x8005B830u, kRowSprite = 0x8005B89Cu, kPageBand = 0x8005C3C0u;
    constexpr uint32_t kRowBase = 0x8005B960u, kStageColour = 0x8005B970u, kPartsSetting = 0x801C83E6u;
    constexpr int kX = 0, kY = 100; // 0x80053558(page, 0, 100)
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    AddRaceViewHeader(ot, a, a.Text(o.Get<uint32_t>(kView + 0x10)), o.Get<uint32_t>(kView + 0x0C), st.headerAlpha);

    const uint32_t table = kGroupTables[std::clamp(st.groupTable, 0, 2)];
    auto group = [&](int g) { return o.Get<uint32_t>(table + uint32_t(g) * 4); };
    const uint32_t current = group(std::clamp(st.group, 0, st.groupCount - 1));
    const int alpha = st.alpha;
    std::vector<HudFontSprite> glyphs;

    // 0x80053CA8, state 0. The band 0x8005C410 (closed), the list 0x8005C3DC (closed) and the graph 0x8005C42C draw
    // nothing here; the band's E1 is still added.
    ot[4].DrawMode(0x220);
    Band band = Band::Read(o, kPageBand);
    band.anim = band.steps;
    band.c0 = MenuListLerp(st.previousColour, o.Get<uint32_t>(current + 8), 128, 0x80);
    // Group icons (current and neighbours; the others at a quarter).
    for (int g = st.group - 1, y = kY - 0x1C; g <= st.group + 2; g++, y += 0x3C) {
        if (g < 0 || g >= st.groupCount) continue;
        const uint32_t e = kIcons + uint32_t(o.Get<int8_t>(group(g) + 0xC)) * 12;
        int grey = alpha;
        if (g != st.group) grey >>= 2;
        MenuPrim p;
        p.kind = MenuPrim::kSprite;
        p.w = o.Get<int16_t>(e + 4), p.h = o.Get<int16_t>(e + 6);
        p.x[0] = int16_t(kX - (p.w >> 1) + 0x2A), p.y[0] = int16_t(y - (p.h >> 1));
        p.u = o.Get<uint8_t>(e), p.v = o.Get<uint8_t>(e + 1);
        p.clut = o.Get<uint16_t>(e + 2);
        p.colour[0] = uint32_t(grey) * 0x010101u;
        p.tpage = uint16_t(o.Get<uint16_t>(e + 8) | 0x20);
        ot[5].Add(p);
        ot[5].DrawMode(p.tpage);
    }
    if (st.partsSetting) {
        glyphs.clear();
        const int w = a.FontAt(0x801C9120u).TextRight(a.Text(kPartsSetting), 0x14C, 0x68, 0, glyphs);
        AddSprites(ot[4], glyphs, 0x2503C28u, 1);
        AddSideArrow(ot[4], 0x149 - w, 0x5F, -5, 10, st.arrowPhase);
    }
    {
        const uint32_t grey = uint32_t((alpha * 0x60) >> 7);
        glyphs.clear();
        a.FontAt(0x801C9150u).Text(a.Text(o.Get<uint32_t>(current)), kX + 0x44, kY + 0x1E, 1, glyphs);
        AddSprites(ot[4], glyphs, grey * 0x010101u | 0x2000000u, 1);
    }
    ot[5].Add(Tile(kX + 0x1A, kY + 4, 0x20, 0x38, 0));
    ot[5].DrawMode(0x200);
    band.Draw(ot[5], kX, kY);
    ot[5].DrawMode(0x200);
    if (st.group > 0) MenuListArrow(ot[4], kX + 0x2A, kY + 2, 6, -10, st.arrowPhase);
    if (st.group < st.groupCount - 1) MenuListArrow(ot[4], kX + 0x2A, kY + 0x3E, 6, 10, st.arrowPhase);

    // 0x800529C0: the parts of the group (none selected in state 0).
    for (int i = 0, x = kX + 0x48, y = kY + 100; i < 8; i++, x += 4, y += 0x20) {
        const uint32_t part = current + 0x10 + uint32_t(i) * 0x10;
        if (o.Get<uint32_t>(part) == 0) break;
        uint32_t colour = 0x0278500Au;
        int yy = y;
        if (o.Get<uint8_t>(part + 0xF) & 0x40) colour = 0x020C3060u, yy += 8;
        glyphs.clear();
        a.FontAt(0x801C9120u).Text(a.Text(o.Get<uint32_t>(part)), x + 4, yy, 1, glyphs);
        AddSprites(ot[4], glyphs, MenuListLerp(o.Get<uint32_t>(kRowBase), colour, alpha, 0x80), 1);
        const int kind = o.Get<int16_t>(part + 0xC);
        const int stage = (kind >= 0 && kind < int(st.stages.size())) ? st.stages[size_t(kind)] : 0;
        if (stage >= 0) {
            glyphs.clear();
            a.FontAt(0x801C9150u).Text(a.Text(o.Get<uint32_t>(o.Get<uint32_t>(part + 8) + uint32_t(stage) * 12)), x + 0x94, yy, 1, glyphs);
            AddSprites(ot[4], glyphs, Lerp(o, kRowBase, kStageColour, alpha, 0x80), 1);
        }
        MenuPrim p; // 0x80081478: SPRT 0x64 at a sixth
        p.kind = MenuPrim::kSprite;
        p.x[0] = int16_t(x - 8), p.y[0] = int16_t(yy - 0x18);
        p.u = o.Get<uint8_t>(kRowSprite), p.v = o.Get<uint8_t>(kRowSprite + 1);
        p.clut = o.Get<uint16_t>(kRowSprite + 2);
        p.w = o.Get<int16_t>(kRowSprite + 4), p.h = o.Get<int16_t>(kRowSprite + 6);
        p.colour[0] = uint32_t(alpha / 6) * 0x010101u;
        p.tpage = uint16_t(o.Get<uint16_t>(kRowSprite + 8) | 0x220);
        ot[4].Add(p);
        ot[4].DrawMode(p.tpage);
    }

    ot.Emit(prims, 0x200);
    return prims;
}

// ---------------------------------------------------------------- settings: PARTS SETTING

std::string SettingValueText(const RaceMenuAssets& a, uint32_t row, const std::array<uint8_t, 0x80>& s, bool front) {
    const GuestImage& o = a.ovl0;
    const uint8_t kind = o.Get<uint8_t>(row + 0xC), format = o.Get<uint8_t>(row + 0xD);
    auto byte = [&](int rear, int frontOffset) { return int(s[size_t(front ? frontOffset : rear)]); };
    int v = 0;
    switch (kind) { // 0x800551BC
    case 0: v = byte(0x61, 0x60); break;
    case 1: v = byte(0x5D, 0x5C); break;
    case 2: v = byte(0x6A, 0x66); break;
    case 3: v = byte(0x68, 0x64); break;
    case 4: v = byte(0x5B, 0x5A); break;
    case 5: v = byte(0x5F, 0x5E) - 0x80; break;
    case 6: v = byte(0x6D, 0x6C); break;
    case 7: v = byte(0x51, 0x50); break;
    case 9: case 10: case 11: case 12: case 13: case 14: case 15: case 16: {
        const size_t at = 0x3C + size_t(kind - 8) * 2;
        v = int(s[at] | s[at + 1] << 8);
        break;
    }
    case 0x11: v = byte(0x53, 0x52); break;
    case 0x16: v = int(s[0x74]); break;
    case 0x17: v = int(s[0x75]); break;
    default: break;
    }
    return SettingNumberText(v, format);
}

std::vector<MenuPrim> BuildMachineSettingsFrame(const RaceMenuAssets& a, const MachineSettingsState& st) {
    const GuestImage& o = a.ovl0;
    constexpr uint32_t kView = 0x8005D1E4u, kIcons = 0x8005B830u, kRowSprite = 0x8005B8A8u, kPageBand = 0x8005C3C0u;
    constexpr uint32_t kBase = 0x8005C46Cu, kNameAdjustable = 0x8005C470u, kName = 0x8005C474u, kValue = 0x8005C478u, kUnit = 0x8005C47Cu,
                       kMarker = 0x8005C484u, kChangeParts = 0x801C83FEu;
    constexpr int kX = 0, kY = 100;
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    AddRaceViewHeader(ot, a, a.Text(o.Get<uint32_t>(kView + 0x10)), o.Get<uint32_t>(kView + 0x0C), st.headerAlpha);
    if (st.groups.empty()) {
        ot.Emit(prims, 0x200);
        return prims;
    }
    const int count = int(st.groups.size());
    const int groupIndex = std::clamp(st.group, 0, count - 1);
    const uint32_t current = st.groups[size_t(groupIndex)];
    const int alpha = st.alpha;
    std::vector<HudFontSprite> glyphs;
    const HudFont& small = a.FontAt(0x801C9120u);

    // 0x80056810, state 0.
    Band band = Band::Read(o, kPageBand);
    band.anim = band.steps;
    band.c0 = MenuListLerp(st.previousColour, o.Get<uint32_t>(current + 8), 128, 0x80);
    for (int g = groupIndex - 1, y = kY - 0x1C; g <= groupIndex + 2; g++, y += 0x3C) {
        if (g < 0 || g >= count) continue;
        const uint32_t e = kIcons + uint32_t(o.Get<int8_t>(st.groups[size_t(g)] + 0xC)) * 12;
        int grey = alpha;
        if (g != groupIndex) grey >>= 2;
        MenuPrim p;
        p.kind = MenuPrim::kSprite;
        p.w = o.Get<int16_t>(e + 4), p.h = o.Get<int16_t>(e + 6);
        p.x[0] = int16_t(kX - (p.w >> 1) + 0x2A), p.y[0] = int16_t(y - (p.h >> 1));
        p.u = o.Get<uint8_t>(e), p.v = o.Get<uint8_t>(e + 1);
        p.clut = o.Get<uint16_t>(e + 2);
        p.colour[0] = uint32_t(grey) * 0x010101u;
        p.tpage = uint16_t(o.Get<uint16_t>(e + 8) | 0x20);
        ot[5].Add(p);
        ot[5].DrawMode(p.tpage);
    }
    if (st.changeParts) {
        glyphs.clear();
        small.TextRight(a.Text(kChangeParts), 0x14C, 0x68, 0, glyphs);
        AddSprites(ot[4], glyphs, 0x2503C28u, 1);
        AddSideArrow(ot[4], 0x14F, 0x5F, 5, 10, st.arrowPhase);
    }
    {
        const uint32_t grey = uint32_t((alpha * 0x60) >> 7);
        glyphs.clear();
        a.FontAt(0x801C9150u).Text(a.Text(o.Get<uint32_t>(current)), kX + 0x44, kY + 0x1E, 1, glyphs);
        AddSprites(ot[4], glyphs, grey * 0x010101u | 0x2000000u, 1);
    }
    ot[5].Add(Tile(kX + 0x1A, kY + 4, 0x20, 0x38, 0));
    ot[5].DrawMode(0x200);
    band.Draw(ot[5], kX, kY);
    ot[5].DrawMode(0x200);
    // The list 0x8005D154 and the band 0x8005D188 are closed in state 0; the band's E1 is added.
    ot[4].DrawMode(0x220);
    if (groupIndex > 0) MenuListArrow(ot[4], kX + 0x2A, kY + 2, 6, -10, st.arrowPhase);
    if (groupIndex < count - 1) MenuListArrow(ot[4], kX + 0x2A, kY + 0x3E, 6, 10, st.arrowPhase);

    // 0x80055840: the rows (none selected in state 0).
    std::vector<uint32_t> rows;
    if (!st.rows.empty()) {
        for (const auto& r : st.rows) rows.push_back(r.first);
    } else {
        for (uint32_t i = 0; i < 16 && o.Get<uint32_t>(current + 0x10 + i * 8) != 0; i++) rows.push_back(o.Get<uint32_t>(current + 0x10 + i * 8));
    }
    int x = kX + 0x48, y = kY + 100;
    for (size_t i = 0; i < rows.size() && i < 16; i++, x += 4, y += 0x1E) {
        const uint32_t row = rows[i];
        const bool adjustable = st.adjustable[i] > 0;
        if (adjustable) ot[4].Add(Tile(x - 8, y - 0xC, 4, 0xC, Lerp(o, kBase, kMarker, alpha, 0x80)));
        // 0x80055328.
        glyphs.clear();
        small.Number(a.Text(o.Get<uint32_t>(row)), x, y, 1, -3, 0, glyphs);
        AddSprites(ot[4], glyphs, Lerp(o, kBase, adjustable ? kNameAdjustable : kName, alpha, 0x80), 1);
        const uint8_t kind = o.Get<uint8_t>(row + 0xC);
        const bool slider = kind == 8 || (kind >= 0x12 && kind <= 0x17);
        if (!slider && (kind != 0 || st.springValues)) {
            const std::string unit = a.Text(o.Get<uint32_t>(row + 8));
            const int sides = (o.Get<uint8_t>(row + 0xD) & 0x80) ? 2 : 1;
            for (int k = 0; k < sides; k++) {
                glyphs.clear();
                small.NumberRight(SettingValueText(a, row, st.sheet, k == 0), x + 0x78 + 0x40 * k, y, 1, -3, 0, glyphs);
                AddSprites(ot[4], glyphs, Lerp(o, kBase, kValue, alpha, 0x80), 1);
                glyphs.clear();
                small.Number(unit, x + 0x7A + 0x40 * k, y, 0, -2, 0, glyphs);
                AddSprites(ot[4], glyphs, Lerp(o, kBase, kUnit, alpha, 0x80), 1);
            }
        }
        const int grey = adjustable ? alpha / 5 : alpha / 6;
        MenuPrim p; // 0x80081478: SPRT 0x64
        p.kind = MenuPrim::kSprite;
        p.x[0] = int16_t(x - 8), p.y[0] = int16_t(y - 0x18);
        p.u = o.Get<uint8_t>(kRowSprite), p.v = o.Get<uint8_t>(kRowSprite + 1);
        p.clut = o.Get<uint16_t>(kRowSprite + 2);
        p.w = o.Get<int16_t>(kRowSprite + 4), p.h = o.Get<int16_t>(kRowSprite + 6);
        p.colour[0] = uint32_t(grey) * 0x010101u;
        p.tpage = uint16_t(o.Get<uint16_t>(kRowSprite + 8) | 0x220);
        ot[4].Add(p);
        ot[4].DrawMode(p.tpage);
    }

    ot.Emit(prims, 0x200);
    return prims;
}

// ---------------------------------------------------------------- settings: PARTS SETTING, the interactive page

namespace {

// ovl0: the page's colours (0x8005C46C..), tables and strings.
constexpr uint32_t kMsBase = 0x8005C46Cu, kMsNameAdjustable = 0x8005C470u, kMsName = 0x8005C474u, kMsValue = 0x8005C478u, kMsUnit = 0x8005C47Cu,
                   kMsFlash = 0x8005C480u, kMsMarker = 0x8005C484u, kMsSliderNumber = 0x8005C488u, kMsKnob = 0x8005C48Cu, kMsOutline = 0x8005C490u,
                   kMsBar = 0x8005C494u, kMsBarSelected = 0x8005C498u, kMsPopupTitle = 0x8005C49Cu;
constexpr uint32_t kMsDescription = 0x02808080u;      // 0x80056F34: the row description (US)
constexpr uint32_t kMsChangePartsColour = 0x02503C28u; // 0x80056B48
constexpr uint32_t kMsView = 0x8005D1E4u, kMsIcons = 0x8005B830u, kMsRowSprite = 0x8005B8A8u;
constexpr uint32_t kMsGearGroups = 0x8005D100u, kMsGearGroupsClose = 0x8005D120u, kMsGearLabels = 0x8005C4A4u, kMsNumberFormat = 0x8005A9ACu;
constexpr uint32_t kStrFinal = 0x801C6F4Cu, kStrFront = 0x801C75F4u, kStrRear = 0x801C75FEu, kStrSoft = 0x801C71C1u, kStrHard = 0x801C71D9u,
                   kStrSports = 0x801C71EEu, kStrWide = 0x801C7A36u, kStrChangeParts = 0x801C83FEu;
constexpr uint32_t kFontTiny = 0x801C9110u, kFontSmall = 0x801C9120u, kFontMedium = 0x801C9150u;


// The page's text / slider drawing with the font objects of 0x8006AC68(obj, 6) (glyph E1 = tpage | 0x20 when the
// colour is semi-transparent).
struct MsPainter {
    const RaceMenuAssets& a;
    const GuestImage& o;
    uint32_t Lerp(uint32_t to, int t, int max) const { return MenuListLerp(o.Get<uint32_t>(kMsBase), o.Get<uint32_t>(to), t, max); }
    void Put(MenuOtSlot& ot, const std::vector<HudFontSprite>& glyphs, uint32_t colour) const { AddSprites(ot, glyphs, colour, 1); }
    void Text(MenuOtSlot& ot, uint32_t font, const std::string& s, int x, int y, int spacing, uint32_t colour) const { // 0x8006AC90
        std::vector<HudFontSprite> g;
        a.FontAt(font).Text(s, x, y, spacing, g);
        Put(ot, g, colour);
    }
    void Centred(MenuOtSlot& ot, uint32_t font, const std::string& s, int x, int y, int spacing, uint32_t colour) const { // 0x8006ADB4
        const HudFont& f = a.FontAt(font);
        std::vector<HudFontSprite> g;
        f.Text(s, x - (f.TextWidth(s, spacing) >> 1), y, spacing, g);
        Put(ot, g, colour);
    }
    void Number(MenuOtSlot& ot, uint32_t font, const std::string& s, int x, int y, int spacing, int shift, int extra, uint32_t colour) const { // 0x8006AF40
        std::vector<HudFontSprite> g;
        a.FontAt(font).Number(s, x, y, spacing, shift, extra, g);
        Put(ot, g, colour);
    }
    void NumberRight(MenuOtSlot& ot, uint32_t font, const std::string& s, int x, int y, int spacing, int shift, int extra, uint32_t colour) const { // 0x8006B184
        std::vector<HudFontSprite> g;
        a.FontAt(font).NumberRight(s, x, y, spacing, shift, extra, g);
        Put(ot, g, colour);
    }

    // 0x8005480C(ot, {value, min, max}, alpha, x, y, phase, w, left, right, number).
    void Slider(MenuOtSlot& ot, int value, int min, int max, int alpha, int x, int y, int phase, int w, uint32_t left, uint32_t right, bool number) const {
        const uint32_t labels = Lerp(kMsUnit, alpha, 0x100);
        if (left) Centred(ot, kFontTiny, a.Text(left), x + 8, y + 4, 1, labels);
        if (right) Centred(ot, kFontTiny, a.Text(right), x + w - 8, y + 4, 1, labels);
        if (number) Number(ot, kFontSmall, Format(a.Text(kMsNumberFormat), value - min + 1), x + w + 2, y + 0x13, 0, -3, 0, Lerp(kMsSliderNumber, alpha, 0x80));
        const int range = max - min;
        if (range > 0) ot.Add(Tile(x + (w * (value - min)) / range - 2, y + 5, 4, 12, Lerp(kMsKnob, alpha, 0x80)));
        const uint32_t base = o.Get<uint32_t>(kMsBase), bar = o.Get<uint32_t>(phase >= 0 ? kMsBarSelected : kMsBar);
        uint32_t l = MenuListLerp(base, bar, alpha, 0x100), r = MenuListLerp(base, bar, alpha, 0x80);
        if (phase >= 0) {
            const int k = std::max(8 - phase, 0);
            l = MenuListLerp(l, o.Get<uint32_t>(kMsFlash), k, 8);
            r = MenuListLerp(r, o.Get<uint32_t>(kMsFlash), k, 8);
        }
        AddGradientQuad(ot, x + 1, y + 9, w - 2, 5, l, r);        // 0x8006B77C: POLY_G4 0x3A {x + 1, y + 9, w - 2, 5}
        MenuListFrame(ot, Lerp(kMsOutline, alpha, 0x80), x, y + 8, w, 7); // 0x8007E738: polyline 0x4A
        ot.DrawMode(0x220);
    }
};

} // namespace

bool MachineSettingsSpringValues(const career::TuneSheet& s) { // 0x8005F92C
    const size_t at = 0x9D3 + size_t(int(s.stage[career::kTuneSuspension]) * 0x4C);
    if (at >= sizeof(career::TuneSheet)) throw std::logic_error("0x8005F92C: suspension stage outside the sheet");
    return reinterpret_cast<const uint8_t*>(&s)[at] == 0;
}

MachineSettingsGroups BuildMachineSettingsGroups(const GuestImage& o, const career::TuneSheet& s, const career::CareerData& d) { // 0x80055B14
    using namespace career;
    MachineSettingsGroups out;
    const size_t gearAt = 0x1221 + size_t(int(s.stage[kTuneGearbox]) * 0x24); // 0x8005F834
    if (gearAt >= sizeof(TuneSheet)) throw std::logic_error("0x8005F834: gearbox stage outside the sheet");
    const uint32_t gears = reinterpret_cast<const uint8_t*>(&s)[gearAt];
    out.groups.push_back(s.stage[kTuneSuspension] == 3 ? 0x8005C650u : 0x8005C6E0u); // 0x8005F800
    if (s.stage[kTuneBrakeController] > 0) out.groups.push_back(0x8005C770u);        // 0x8005F814
    out.groups.push_back(o.Get<uint32_t>((s.stage[kTuneGearbox] == 3 ? kMsGearGroupsClose : kMsGearGroups) + gears * 4)); // 0x8005F820
    out.groups.push_back(0x8005CFE0u);
    SettingValue scratch[9]{};
    if (s.stage[kTuneLsd] == 5) { // 0x8005F8F0
        out.otherRows.push_back({0x8005C620u, int16_t(kSettingLsdRearInitial)});
    } else if (GetSetting(s, kSettingLsdInitial, scratch, d) > 0 || GetSetting(s, kSettingLsdAccel, scratch, d) > 0 ||
               GetSetting(s, kSettingLsdDecel, scratch, d) > 0) { // 0x8005F88C
        out.otherRows.push_back({0x8005C5F0u, int16_t(kSettingLsdInitial)});
        out.otherRows.push_back({0x8005C600u, int16_t(kSettingLsdAccel)});
        out.otherRows.push_back({0x8005C610u, int16_t(kSettingLsdDecel)});
    }
    if (s.stage[kTuneAsm] == 1) out.otherRows.push_back({0x8005C630u, int16_t(kSettingAsm)}); // 0x8005F904
    if (s.stage[kTuneTcs] == 1) out.otherRows.push_back({0x8005C640u, int16_t(kSettingTcs)}); // 0x8005F918
    if (!out.otherRows.empty()) out.groups.push_back(MachineSettingsPage::kOthersGroup);
    return out;
}

uint32_t MachineSettingsPage::GroupAt(int g) const {
    if (g < 0 || g >= int(groups.groups.size())) throw std::out_of_range("PARTS SETTING: group index");
    return groups.groups[size_t(g)];
}

int MachineSettingsPage::RowCount(const GuestImage& o, uint32_t grp) const { // 0x80055808
    if (grp == kOthersGroup) return int(std::min<size_t>(groups.otherRows.size(), 16));
    int n = 0;
    while (n < 16 && o.Get<uint32_t>(grp + 0x10 + uint32_t(n) * 8) != 0) n++;
    return n;
}

uint32_t MachineSettingsPage::RowRecord(const GuestImage& o, uint32_t grp, int r) const {
    if (grp == kOthersGroup) return (r >= 0 && r < int(groups.otherRows.size())) ? groups.otherRows[size_t(r)].first : 0;
    return o.Get<uint32_t>(grp + 0x10 + uint32_t(r) * 8);
}

int16_t MachineSettingsPage::RowSetting(const GuestImage& o, uint32_t grp, int r) const {
    if (grp == kOthersGroup) return (r >= 0 && r < int(groups.otherRows.size())) ? groups.otherRows[size_t(r)].second : int16_t(-1);
    return o.Get<int16_t>(grp + 0x14 + uint32_t(r) * 8);
}

void MachineSettingsPage::Init(const GuestImage& o, const career::TuneSheet& sheet, const career::CareerData& data) { // 0x80055E90(P, 0, 100)
    ovl0_ = &o;
    entryCount = 0;
    entryGroup = 0;
    input = true;
    groupDescription = description = kEmptyString;
    groups = BuildMachineSettingsGroups(o, sheet, data);
    x = 0, y = 100;
    state = 0;
    row = 0;
    opening = -1;
    scroll = 0;
    arrowPhase = 0;
    flash = 0;
    colour = 0;
    entries = {};
    counts = {};
    SelectGroup(o, 0, sheet, data);
    groupCount = int16_t(groups.groups.size());
    for (Slider& s : sliders) s = Slider{}; // 0x80054CC8: active -1
    widget = MenuListWidget::Read(o, kWidget);
    MenuListReset(widget, nullptr); // callback flags 0x81: no command 0
    popupBand = Band::Read(o, kPopupBand);
    popupBand.anim = -1;
    band = Band::Read(o, kPageBand);
    band.anim = -1;
    sounds.clear();
}

void MachineSettingsPage::StartOpen() { // 0x80055FD0
    scroll = 0;
    band.anim = 0;
    opening = 0;
}

void MachineSettingsPage::StartClose() { // 0x80055FE0
    band.anim = int16_t(~band.steps);
    opening = -1;
}

void MachineSettingsPage::SelectGroup(const GuestImage& o, int g, const career::TuneSheet& sheet, const career::CareerData& data) { // 0x80056000
    previousColour = colour;
    group = int8_t(g);
    const uint32_t grp = GroupAt(g);
    colour = o.Get<uint32_t>(grp + 8);
    bool gears = false;
    int8_t gearCount = 0;
    for (int i = 0, n = RowCount(o, grp); i < n; i++) {
        const int16_t setting = RowSetting(o, grp, i);
        if (setting == career::kSettingGears) { // every gear row of the group gets the count of the first
            if (!gears) gearCount = int8_t(career::GetSetting(sheet, setting, entries.data(), data));
            gears = true;
            counts[size_t(i)] = gearCount;
        } else {
            counts[size_t(i)] = int8_t(career::GetSetting(sheet, setting, entries.data(), data));
        }
    }
    groupDescription = o.Get<uint32_t>(grp + 4); // 0x8005471C
}

void MachineSettingsPage::SelectRow(const GuestImage& o, int r) { // 0x8005613C
    row = int8_t(r);
    flash = 0;
    description = o.Get<uint32_t>(RowRecord(o, GroupAt(entryGroup), r) + 4); // 0x80054794
}

void MachineSettingsPage::SliderTick(Slider& s, const MenuListPad* pad) { // 0x80054D10
    if (s.active < 0) return;
    if (++s.phase > 60) s.phase = 0;
    if (!pad) return;
    const int16_t v = career::SliderStep(s.value, s.min, s.max, career::SliderDelta(pad->held, pad->pressed, pad->repeat));
    if (v != s.value) {
        s.value = v;
        sounds.push_back(8); // 0x80060840(8)
    }
}

int32_t MachineSettingsPage::WidgetUpdate(const MenuListPad* pad) {
    // 0x80055D50 commands 3 (0x80054D10 with the pad for the selected row), 6 (a row entered: its slider's phase 0),
    // 8 (enabled: 1).
    widget.callback = [this, pad](int command, const MenuListWidget& w, int r, const MenuListRowDraw*) -> int32_t {
        if (command == kMenuListEnabled) return 1;
        if (r < 0 || r >= int(sliders.size())) return 0;
        if (command == kMenuListTick) SliderTick(sliders[size_t(r)], r == w.selection ? pad : nullptr);
        else if (command == kMenuListEnter && r != w.selection) sliders[size_t(r)].phase = 0;
        return 0;
    };
    return MenuListUpdate(widget, pad);
}

int MachineSettingsPage::Update(const MenuListPad* pad, bool inputNow, career::TuneSheet& sheet, const career::CareerData& data) { // 0x80056194
    using namespace menu_list_pad;
    if (!ovl0_) throw std::logic_error("PARTS SETTING: Update before Init");
    const GuestImage& o = *ovl0_;
    sounds.clear();
    input = inputNow;
    band.Tick();
    if (scroll < 0) scroll++;
    if (scroll > 0) scroll--;
    if (++flash > 60) flash = 0;
    if (++arrowPhase > 45) arrowPhase = 0;
    popupBand.Tick();
    if (opening < 0) {
        WidgetUpdate(nullptr);
        return kNothing;
    }
    if (state == 0) {
        WidgetUpdate(nullptr);
        if (!pad) return kNothing;
        uint32_t bits = pad->pressed;
        if (bits & kR1) {
            input = false;
            return kChangeParts;
        }
        if (bits & (kTriangle | kSquare)) {
            input = false;
            return kLeave;
        }
        if (bits & (kCross | kCircle | kRight)) {
            entryGroup = group;
            state = 1;
            SelectRow(o, 0);
            return kEntered;
        }
        bits |= pad->repeat;
        int g = group;
        if (bits & kUp) {
            if (--g < 0) g = 0;
            if (g != group) scroll = -6;
        }
        if (bits & kDown) {
            if (!(++g < groupCount)) g = groupCount - 1;
            if (g == group) return kNothing;
            scroll = 6;
        }
        if (g == group) return kNothing;
        SelectGroup(o, g, sheet, data);
        return kMoved;
    }
    if (state == 1) {
        WidgetUpdate(nullptr);
        if (!pad) return kNothing;
        uint32_t bits = pad->pressed;
        if (bits & kR1) {
            input = false;
            return kChangeParts;
        }
        if (bits & (kTriangle | kSquare | kLeft)) {
            state = 0;
            return kBack;
        }
        const uint32_t grp = GroupAt(entryGroup);
        if (bits & kStart) {
            career::DefaultSetting(sheet, RowSetting(o, grp, row), data); // 0x80060410 (no sound)
            return kNothing;
        }
        if (bits & (kCross | kCircle)) {
            const uint32_t record = RowRecord(o, grp, row);
            const int16_t setting = RowSetting(o, grp, row);
            const int32_t n = career::GetSetting(sheet, setting, entries.data(), data);
            if (n < 1) return kNotAdjustable;
            int selection = 0;
            uint32_t label = kEmptyString;
            for (int i = 0; i < n && i < int(sliders.size()); i++) {
                if (setting == career::kSettingGearAuto) {
                    label = kEmptyString;
                } else if (setting < career::kSettingLsdInitial) {
                    if (setting == career::kSettingGears) {
                        label = i == n - 1 ? kStrFinal : o.Get<uint32_t>(kMsGearLabels + uint32_t(i) * 4);
                        selection = row;
                    } else {
                        label = i == 0 ? kStrFront : kStrRear;
                    }
                } else if (setting < career::kSettingLsdRearInitial) {
                    const int16_t field = entries[size_t(i)].field; // jump table 0x8005AACC: fields 0 / 2 / 4 front, 1 / 3 / 5 rear
                    if (uint16_t(field) < 6) label = (field & 1) ? kStrRear : kStrFront;
                } else {
                    label = i == 0 ? kStrFront : kStrRear;
                }
                Slider& s = sliders[size_t(i)]; // 0x80054CD4
                s.label = label;
                s.unit = o.Get<uint32_t>(record + 8);
                s.format = uint8_t(o.Get<uint8_t>(record + 0xD) & 0xF);
                s.value = entries[size_t(i)].value;
                s.min = entries[size_t(i)].min;
                s.max = entries[size_t(i)].max;
                s.field = entries[size_t(i)].field;
                s.active = 0;
                s.phase = 0;
            }
            entryCount = int16_t(n);
            state = 2;
            widget.count = int16_t(n);
            widget.selection = int16_t(selection);
            MenuListOpen(widget); // callback flags 0x81: no command 7
            popupBand.anim = 0;
            return kEntered;
        }
        bits |= pad->repeat;
        int r = row;
        if (bits & kUp) {
            if (--r < 0) r = 0;
        }
        const int rows = RowCount(o, grp);
        if (bits & kDown) {
            if (!(++r < rows)) r = rows - 1;
        }
        if (r == row) return kNothing;
        SelectRow(o, r);
        return kMoved;
    }
    if (state != 2) return kNothing;
    const int32_t r = WidgetUpdate(pad);
    if (r == -2) return kNothing;
    if (r == -3) return kMoved;
    if (r != -1) { // cross / circle (a row, or -4): the sliders' values written
        const uint32_t grp = GroupAt(entryGroup);
        for (int i = 0; i < entryCount && i < int(sliders.size()); i++) {
            const Slider& s = sliders[size_t(i)];
            entries[size_t(i)] = career::SettingValue{s.value, s.min, s.max, s.field};
        }
        career::SetSetting(sheet, RowSetting(o, grp, row), entries.data(), entryCount, data); // 0x8005F9DC
    }
    flash = 0;
    popupBand.anim = int16_t(~popupBand.steps);
    MenuListClose(widget);
    state = 1;
    return kBack;
}

int MachineSettingsPage::Sound(int code) { // 0x800574C0 (jump table 0x8005AB04)
    switch (code) {
    case kChangeParts: return 3;
    case kNotAdjustable: return 0;
    case kBack: return 2;
    case kEntered: return 1;
    case kLeave: return 4;
    case kMoved: return 6;
    default: return -1;
    }
}

std::vector<MenuPrim> MachineSettingsPage::Frame(const RaceMenuAssets& a, const career::TuneSheet& sheet, const career::CareerData& data, int headerAlpha) const {
    const GuestImage& o = a.ovl0;
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    AddRaceViewHeader(ot, a, a.Text(o.Get<uint32_t>(kMsView + 0x10)), o.Get<uint32_t>(kMsView + 0x0C), headerAlpha);
    Draw(ot, a, sheet, data);
    ot.Emit(prims, 0x200);
    return prims;
}

void MachineSettingsPage::Draw(MenuOt& ot, const RaceMenuAssets& a, const career::TuneSheet& sheet, const career::CareerData& data) const {
    const GuestImage& o = a.ovl0;
    const MsPainter p{a, o};
    MenuOtSlot& s4 = ot[4];
    MenuOtSlot& s5 = ot[5];

    // 0x80056810 (nothing while the page band is closed: before 0x80055FD0 and after the close).
    const int alpha = BandAlpha(band); // sp+100
    int s = (int(scroll) << 7) / 6;
    if (s < 0) s = -s;
    Band pageBand = band;
    pageBand.c0 = MenuListLerp(previousColour, colour, 128 - s, 128); // P + 0x22C
    if (band.anim == -1) return;
    const uint32_t current = GroupAt(group);
    // Group icons: the current group's at the full alpha, the others at a quarter; while scrolling one more.
    {
        int iconY = y + scroll * 10 - 28;
        const int alphaIn = (alpha * (128 - s)) >> 7, alphaOut = (alpha * s) >> 7;
        int first = group - 1, n = 4, rowIn = -1, rowOut = -1;
        if (scroll < 0) {
            n = 5;
            rowIn = group - 1;
            rowOut = group + 3;
        }
        if (scroll > 0) {
            iconY -= 60;
            n = 5;
            first = group - 2;
            rowIn = group + 2;
            rowOut = first;
        }
        for (int g = first; g < first + n; g++, iconY += 60) {
            if (g < 0 || g >= groupCount) continue;
            const uint32_t e = kMsIcons + uint32_t(o.Get<int8_t>(GroupAt(g) + 0xC)) * 12;
            int grey = alpha;
            if (g == rowIn) grey = alphaIn;
            if (g == rowOut) grey = alphaOut;
            if (g != group) grey >>= 2;
            MenuPrim sp; // 0x80081478: SPRT 0x64
            sp.kind = MenuPrim::kSprite;
            sp.w = o.Get<int16_t>(e + 4), sp.h = o.Get<int16_t>(e + 6);
            sp.x[0] = int16_t(x - (sp.w >> 1) + 0x2A), sp.y[0] = int16_t(iconY - (sp.h >> 1));
            sp.u = o.Get<uint8_t>(e), sp.v = o.Get<uint8_t>(e + 1);
            sp.clut = o.Get<uint16_t>(e + 2);
            sp.colour[0] = uint32_t(grey) * 0x010101u;
            sp.tpage = uint16_t(o.Get<uint16_t>(e + 8) | 0x20);
            s5.Add(sp);
            s5.DrawMode(sp.tpage);
        }
    }
    if (state >= 0 && state < 2 && input) {
        std::vector<HudFontSprite> glyphs;
        a.FontAt(kFontSmall).TextRight(a.Text(kStrChangeParts), 0x14C, 0x68, 0, glyphs);
        p.Put(s4, glyphs, kMsChangePartsColour);
        AddSideArrow(s4, 0x14F, 0x5F, 5, 10, arrowPhase);
    }
    {
        const uint32_t grey = uint32_t((alpha * 0x60) >> 7);
        p.Text(s4, kFontMedium, a.Text(o.Get<uint32_t>(current)), x + 0x44, y + 0x1E, 1, grey * 0x010101u | 0x2000000u);
    }
    s5.Add(Tile(x + 0x1A, y + 4, 0x20, 0x38, 0));
    s5.DrawMode(0x200);
    pageBand.Draw(s5, x, y);
    s5.DrawMode(0x200);

    // The popup list (0x8006D50C) with the row callback 0x80055D50 command 4.
    {
        MenuListWidget w = widget;
        w.callback = [&](int command, const MenuListWidget& lw, int r, const MenuListRowDraw* d) -> int32_t {
            if (command == kMenuListEnabled) return 1;
            if (command != kMenuListDraw || !d || r < 0 || r >= int(sliders.size())) return 0;
            if (lw.state == -1 || lw.fadeMax == 0) return 0;
            const int t = (int(lw.fade) << 7) / lw.fadeMax;
            if (t <= 0) return 0;
            const int rx = d->x + (lw.state >= 0 ? 128 - t : 0);
            const int ra = (t * d->alpha) >> 7;
            const Slider& sl = sliders[size_t(r)];
            // 0x80054E20(slider, ot, x, y, selected, alpha).
            const bool selected = r == lw.selection;
            const int format = sl.format & 0xF;
            bool number = true;
            if (format == 6) {
                const uint32_t c = p.Lerp(kMsUnit, ra, 0x80);
                p.Centred(*d->ot, kFontTiny, a.Text(kStrSoft), rx + 4, d->y, 1, c);
                p.Centred(*d->ot, kFontTiny, a.Text(kStrHard), rx + 0x7C, d->y, 1, c);
                p.Centred(*d->ot, kFontSmall, a.Text(sl.label), rx + 0x40, d->y, 1, p.Lerp(kMsNameAdjustable, ra, 0x80));
            } else if (format == 5 || format == 7) {
                const uint32_t c = p.Lerp(kMsUnit, ra, 0x80);
                p.Centred(*d->ot, kFontSmall, a.Text(format == 5 ? kStrSports : kStrSoft), rx + 8, d->y, 1, c);
                p.Centred(*d->ot, kFontSmall, a.Text(format == 5 ? kStrWide : kStrHard), rx + 0x78, d->y, 1, c);
            } else {
                number = false;
                p.Number(*d->ot, kFontSmall, a.Text(sl.label), rx, d->y, 1, -3, 0, p.Lerp(kMsNameAdjustable, ra, 0x80));
                p.NumberRight(*d->ot, kFontSmall, SettingNumberText(sl.value, sl.format), rx + 0x50, d->y, 1, -3, 0, p.Lerp(kMsValue, ra, 0x80));
                p.Number(*d->ot, kFontSmall, a.Text(sl.unit), rx + 0x54, d->y, 0, -2, 0, p.Lerp(kMsUnit, ra, 0x80));
            }
            p.Slider(*d->ot, sl.value, sl.min, sl.max, ra, rx + 8, d->y, selected ? sl.phase : -1, 0x70, 0, 0, number);
            return 0;
        };
        MenuListDraw(w, s4);
    }
    popupBand.Draw(s4, 0, 0xBE);
    s4.DrawMode(0x220);
    if (popupBand.anim >= 0)
        p.Centred(s4, kFontMedium, a.Text(o.Get<uint32_t>(RowRecord(o, current, row))), 0xB0, 0xD8, 1, p.Lerp(kMsPopupTitle, BandAlpha(popupBand), 0x80));

    int rowSel = -1, rowFlash = flash, rowAlpha = alpha;
    if (state == 0) {
        if (group > 0) MenuListArrow(s4, x + 0x2A, y + 2, 6, -10, arrowPhase);
        if (group < groupCount - 1) MenuListArrow(s4, x + 0x2A, y + 0x3E, 6, 10, arrowPhase);
    } else if (state == 1 || state == 2) {
        rowSel = row;
        if (state == 2) {
            rowFlash = 8;
            int v = (widget.fadeMax ? (int(widget.fade) << 7) / widget.fadeMax : 0) * 100;
            if (v < 0) v += 127;
            rowAlpha = alpha - (v >> 7);
        }
        if (input) p.Centred(s4, kFontMedium, a.Text(description), 0xB0, 0x1BC, 1, kMsDescription); // US (0x801C98E0 != 0)
    }

    // 0x80055840(group, ot, rowSel, x + 0x48, y + 100, alpha, flash, sheet, P + 0x92).
    std::array<uint8_t, 0x80> config{};
    std::memcpy(config.data(), &sheet.config, config.size());
    const bool springs = MachineSettingsSpringValues(sheet);
    int rx = x + 0x48, ry = y + 100;
    for (int i = 0, n = RowCount(o, current); i < n; i++, rx += 4, ry += 0x1E) {
        const int count = counts[size_t(i)];
        const uint32_t record = RowRecord(o, current, i);
        if (count > 0) s4.Add(Tile(rx - 8, ry - 0xC, 4, 0xC, p.Lerp(kMsMarker, rowAlpha, 0x80)));
        // 0x80055328(record, ot, x, y, alpha, adjustable).
        p.Number(s4, kFontSmall, a.Text(o.Get<uint32_t>(record)), rx, ry, 1, -3, 0, p.Lerp(count > 0 ? kMsNameAdjustable : kMsName, rowAlpha, 0x80));
        const int8_t kind = o.Get<int8_t>(record + 0xC);
        career::SettingValue e[9]{};
        auto slider = [&](const career::SettingValue& v, int sx, int w, uint32_t left, uint32_t right) {
            p.Slider(s4, v.value, v.min, v.max, rowAlpha, sx, ry - 0x10, -1, w, left, right, true);
        };
        if (kind == 8) { // the gear auto-set (no count check)
            career::GetSetting(sheet, career::kSettingGearAuto, e, data);
            slider(e[0], rx + 0x6A, 0x70, kStrSports, kStrWide);
        } else if (kind >= 0x15 && kind <= 0x17) { // settings 13 / 14 / 15
            if (career::GetSetting(sheet, career::kSettingLsdRearInitial + (kind - 0x15), e, data) > 0) slider(e[0], rx + 0x6A, 0x70, kStrSoft, kStrHard);
        } else if (kind >= 0x12 && kind <= 0x14) { // LSD 10 / 11 / 12: the entries of fields 2k (front) and 2k + 1 (rear)
            const int k = kind - 0x12;
            const int32_t n2 = career::GetSetting(sheet, career::kSettingLsdInitial + k, e, data);
            for (int j = 0; j < n2 && j < 9; j++) {
                if (e[j].field == 2 * k) slider(e[j], rx + 0x6A, 0x34, kStrSoft, kStrHard);
                else if (e[j].field == 2 * k + 1) slider(e[j], rx + 0xB2, 0x34, kStrSoft, kStrHard);
            }
        } else if (kind >= 0 && kind < 0x18 && (kind != 0 || springs)) {
            const std::string unit = a.Text(o.Get<uint32_t>(record + 8));
            const int sides = (o.Get<uint8_t>(record + 0xD) & 0x80) ? 2 : 1;
            for (int k = 0; k < sides; k++) {
                p.NumberRight(s4, kFontSmall, SettingValueText(a, record, config, k == 0), rx + 0x78 + 0x40 * k, ry, 1, -3, 0, p.Lerp(kMsValue, rowAlpha, 0x80));
                p.Number(s4, kFontSmall, unit, rx + 0x7A + 0x40 * k, ry, 0, -2, 0, p.Lerp(kMsUnit, rowAlpha, 0x80));
            }
        }
        MenuPrim sp; // 0x80081478
        sp.kind = MenuPrim::kSprite;
        sp.x[0] = int16_t(rx - 8), sp.y[0] = int16_t(ry - 0x18);
        sp.u = o.Get<uint8_t>(kMsRowSprite), sp.v = o.Get<uint8_t>(kMsRowSprite + 1);
        sp.clut = o.Get<uint16_t>(kMsRowSprite + 2);
        sp.w = o.Get<int16_t>(kMsRowSprite + 4), sp.h = o.Get<int16_t>(kMsRowSprite + 6);
        sp.tpage = uint16_t(o.Get<uint16_t>(kMsRowSprite + 8) | 0x220);
        if (i == rowSel) { // SPRT 0x66: the group colour at a third, flashing to white after a move
            uint32_t c = MenuListLerp(o.Get<uint32_t>(kMsBase), o.Get<uint32_t>(current + 8), rowAlpha, 0x180);
            c = MenuListLerp(c, o.Get<uint32_t>(kMsFlash), std::max(8 - rowFlash, 0), 8);
            sp.colour[0] = c & 0xFFFFFF;
            sp.semi = (c & 0x2000000u) != 0;
        } else { // SPRT 0x64 grey
            sp.colour[0] = uint32_t(count > 0 ? rowAlpha / 5 : rowAlpha / 6) * 0x010101u;
        }
        s4.Add(sp);
        s4.DrawMode(sp.tpage);
    }
}

// ---------------------------------------------------------------- rendering

MenuCanvas RenderRaceMenuFrame(const RaceMenuAssets& assets, const std::vector<MenuPrim>& prims, bool captureRules) {
    MenuCanvas canvas;
    canvas.rules = captureRules ? MenuCanvas::Rules::kInterpreter : MenuCanvas::Rules::kPs1;
    for (const MenuPrim& p : prims) {
        if (!captureRules || p.kind != MenuPrim::kSprite) {
            canvas.Draw(assets.vram, p);
            continue;
        }
        // The interpreter GPU's sprites (src/machine/gpu.cpp): texel << 3, * colour / 128, blending in 8 bits.
        const int mr = int(p.colour[0] & 0xFF), mg = int((p.colour[0] >> 8) & 0xFF), mb = int((p.colour[0] >> 16) & 0xFF);
        const int mode = (p.tpage >> 5) & 3;
        for (int j = 0; j < p.h; j++)
            for (int i = 0; i < p.w; i++) {
                const int x = p.x[0] + i, y = p.y[0] + j;
                if (x < 0 || y < 0 || x >= MenuCanvas::kWidth || y >= MenuCanvas::kHeight) continue;
                const uint16_t t = assets.vram.Sample(p.tpage, p.clut, uint8_t(p.u + i), uint8_t(p.v + j));
                if (t == 0) continue;
                int c[3] = {((t & 31) << 3) * mr / 128, (((t >> 5) & 31) << 3) * mg / 128, (((t >> 10) & 31) << 3) * mb / 128};
                if (p.semi && (t & 0x8000)) {
                    const uint16_t d = canvas.At(x, y);
                    const int back[3] = {(d & 31) << 3, ((d >> 5) & 31) << 3, ((d >> 10) & 31) << 3};
                    for (int k = 0; k < 3; k++) {
                        switch (mode) {
                        case 0: c[k] = (back[k] + c[k]) / 2; break;
                        case 1: c[k] = back[k] + c[k]; break;
                        case 2: c[k] = back[k] - c[k]; break;
                        default: c[k] = back[k] + c[k] / 4; break;
                        }
                    }
                }
                for (int& v : c) v = (std::clamp(v, 0, 255) >> 3) << 3;
                canvas.Fill(x, y, 1, 1, uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2]));
            }
    }
    return canvas;
}

} // namespace gt2::screens
