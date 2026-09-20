#include "gt2view/race_result_screens.h"

#include <algorithm>
#include <cstdio>

#include "game/shell/title_draw.h"

namespace gt2::screens {

using shell::AddGradientQuad;
using shell::Band;

namespace {

// ovl0 addresses (US v1.2).
constexpr uint32_t kLabelBand = 0x8005ABE4u, kLabelText = 0x8005AC00u, kLabelValueTarget = 0x8005AC1Cu;
constexpr uint32_t kCourseBand = 0x8005ABACu, kCourseText = 0x8005ABC8u;
constexpr uint32_t kPlaceStrings = 0x8005AB5Cu, kPlaceColours = 0x8005AB84u;
constexpr uint32_t kTimeBase = 0x8005AB58u; // 0x02000000: the lerp target of 0x800492C4
// RESULTS
constexpr uint32_t kResLabel = 0x8005B60Cu, kResPlace = 0x8005B62Cu, kResPlaceBand = 0x8005B648u, kResTimeStyle = 0x8005B664u,
                   kResLapStyle = 0x8005B678u, kResTotalLabel = 0x8005B6A0u, kResTotalFade = 0x8005B6C0u, kResTotalBand = 0x8005B6C4u,
                   kResFastestLabel = 0x8005B6E0u, kResFastestFade = 0x8005B700u, kResFastestBand = 0x8005B704u, kResLapLabel = 0x8005B720u,
                   kResLapFade = 0x8005B740u, kResLapBand = 0x8005B744u, kResRevealTable = 0x8005B760u, kResList = 0x8005B76Cu, kResBar = 0x8005B5ACu,
                   kResDialog = 0x8005B5DCu;
// BONUS
constexpr uint32_t kBonResLabel = 0x8005D3D8u, kBonPlace = 0x8005D3F8u, kBonLabel = 0x8005D414u, kBonMoneyLabel = 0x8005D434u, kBonNewCarBand = 0x8005D454u,
                   kBonBar = 0x8005D470u, kBonDialog = 0x8005D4A0u;
constexpr uint32_t kStrNewCar = 0x801C7943u;
// post-race menu
constexpr uint32_t kPostResLabel = 0x8005AC50u, kPostTotalLabel = 0x8005AC70u, kPostFastestLabel = 0x8005AC90u, kPostBar = 0x8005ACF0u,
                   kPostRowText = 0x8005AD20u, kPostRowBand = 0x8005AD3Cu, kPostRows2p = 0x8005AD58u, kPostRowsChampNext = 0x8005AD78u,
                   kPostRowsChamp = 0x8005AD98u, kPostRowsRally = 0x8005ADB0u, kPostList = 0x8005ADC0u, kPostPlaces = 0x8005ADF4u;
constexpr uint32_t kStrChampionship = 0x801C78FCu, kStr2Player = 0x801C6E3Du, kStrRally = 0x801C8470u, kStrSingleRace = 0x801C6E29u,
                   kStrRetire = 0x801C6F21u, kStrLap = 0x801C6C50u;
// "PLAYER 1 WINS !!" / "PLAYER 2 WINS !!": the RESULTS setup's place text in game mode 0 (0x8005105C: 0x801C7814, + 23 for
// player 2); "win %d" of the post-race menu's labels in mode 0 (0x80049F84) and their descriptors.
constexpr uint32_t kStrPlayer1Wins = 0x801C7814u, kStrPlayer2Wins = 0x801C782Bu, kStrWinCount = 0x801C7842u;
constexpr uint32_t kPostWinsLabel1 = 0x8005ACB0u, kPostWinsLabel2 = 0x8005ACD0u;
// EXE
constexpr uint32_t kDialogText = 0x80091EA8u, kDialogZero = 0x80091EC4u, kBarText = 0x80091EC8u, kBarZero = 0x80091EE4u;

uint32_t Grey(uint32_t v) { return v | v << 8 | v << 16; }

// The glyphs of the EXE text engine (0x8007DD3C): every SPRT gets the draw mode page | the context's mode << 5, also
// for opaque colours (captured: "E1 tpage=026" before the opaque value glyphs of the post-race menu's labels).
void AddSprites(MenuOtSlot& ot, const std::vector<HudFontSprite>& glyphs, uint32_t colour, int mode) {
    const bool semi = (colour & 0x2000000u) != 0;
    for (const HudFontSprite& g : glyphs) {
        const uint16_t e1 = uint16_t(g.tpage | (mode & 3) << 5);
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

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour) { // EXE 0x8007D024
    MenuPrim p;
    p.kind = MenuPrim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

// EXE 0x8006BEB4: a band's alpha (0..128).
int BandAlpha(const Band& b) {
    const int a = b.anim >= 0 ? b.anim : b.anim < -1 ? ~b.anim : 0;
    return b.steps ? (a << 7) / b.steps : 0;
}

void CloseBand(Band& b) { b.anim = int16_t(~b.steps); }

// EXE 0x8006D400: select row `s` (clamped) with the scroll animation, commands 5 / 6.
void MenuListSelect(MenuListWidget& w, int s) {
    const int old = w.selection;
    if (s >= w.count) s = w.count - 1;
    if (s < 0) s = 0;
    if (s < old) w.scroll = -8;
    if (old < s) w.scroll = 8;
    w.blink = 0;
    if (s != old) {
        w.callback(kMenuListLeave, w, old, nullptr);
        w.callback(kMenuListEnter, w, s, nullptr);
    }
    w.selection = int16_t(s);
}

// EXE 0x8006CF64: the list's alpha (0..128): the fade, or while closing fadeMax - 59 - state.
int MenuListAlpha(const MenuListWidget& w) {
    int a = w.fade;
    if (w.state < -1) a = std::clamp(w.fadeMax - 59 - w.state, 0, int(w.fadeMax));
    return w.fadeMax ? (a << 7) / w.fadeMax : 0;
}

const MenuListPad kNoButtons{};

} // namespace

// ---------------------------------------------------------------- building blocks

FadePair FadePair::Read(const GuestImage& o, uint32_t a) {
    FadePair f;
    f.steps = o.Get<int16_t>(a);
    f.anim = o.Get<int16_t>(a + 2);
    return f;
}

void FadePair::Tick() { // 0x80049274
    if (anim < 0) {
        if (anim < -1) anim++;
        return;
    }
    if (++anim >= steps) anim = steps;
}

int FadePair::Alpha() const { // EXE 0x8006BE20
    if (anim >= 0) return steps ? (anim << 7) / steps : 0;
    if (anim < -1) return steps ? (int(~anim) << 7) / steps : 0;
    return 0;
}

ResultLabel ResultLabel::Init(const RaceMenuAssets& a, uint32_t d) { // 0x80048D14
    const GuestImage& o = a.ovl0;
    ResultLabel l;
    const int16_t steps = o.Get<int16_t>(d + 0x1E); // written into both templates' steps first
    l.text = TextObject::FromTemplate(o, kLabelText, a.Text(o.Get<uint32_t>(d + 8)));
    l.text.steps = steps;
    l.text.c0 = o.Get<uint32_t>(d + 0xC);
    l.band = Band::Read(o, kLabelBand);
    l.band.steps = steps;
    l.band.c0 = o.Get<uint32_t>(d + 0);
    l.band.c1 = o.Get<uint32_t>(d + 4);
    l.band.w = o.Get<int16_t>(d + 0x1C);
    l.band.anim = -1;
    l.flags = o.Get<uint16_t>(d + 0x18);
    const uint32_t v = o.Get<uint32_t>(d + 0x10);
    if ((l.flags & 3) == 1) l.time = v;
    else l.value = a.Text(v);
    l.valueColour = o.Get<uint32_t>(d + 0x14);
    l.fade = -1;
    return l;
}

void ResultLabel::Open() {
    band.anim = 0;
    text.Open(-1);
    fade = 0;
}

void ResultLabel::Close() {
    CloseBand(band);
    text.Close();
    fade = -17;
}

void ResultLabel::Tick() { // 0x80048E14
    text.Tick();
    band.Tick();
    if (fade < 0) {
        if (fade < -1) fade++;
    } else if (++fade > 16) {
        fade = 16;
    }
}

void ResultLabel::Draw(const RaceMenuAssets& a, MenuOt& ot, int slot, int x, int y) const { // 0x80048E84
    int u = fade;
    if (u < -1) u = ~u;
    if (u > 0) {
        const uint32_t colour = MenuListLerp(valueColour, a.ovl0.Get<uint32_t>(kLabelValueTarget), 16 - u, 16);
        const int right = x + band.w;
        std::vector<HudFontSprite> glyphs;
        if ((flags & 3) == 0) {
            const HudFont& f = a.FontAt(0x801C9150u);
            const int w = f.TextWidth(value, 1);
            f.Text(value, right - (w + 6), y + 13, 1, glyphs);
        } else if ((flags & 3) == 1) {
            const HudFont& f = a.FontAt(0x801C9120u);
            const std::string s = FormatRaceTime(time);
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

CourseTitle CourseTitle::Init(const RaceMenuAssets& a, const std::string& name) { // 0x80048BD8
    CourseTitle c;
    c.text = TextObject::FromTemplate(a.ovl0, kCourseText, name);
    c.band = Band::Read(a.ovl0, kCourseBand);
    c.band.w = 0x100;
    c.band.anim = -1;
    return c;
}

void CourseTitle::Open() {
    band.anim = 0;
    text.Open(-1);
}

void CourseTitle::Close() {
    CloseBand(band);
    text.Close();
}

void CourseTitle::Tick() {
    band.Tick();
    text.Tick();
}

void CourseTitle::Draw(const RaceMenuAssets& a, MenuOt& ot, int x, int y) const { // 0x80048C60
    const HudFont& f = a.FontAt(text.font);
    text.Draw(ot, 0, x - (text.Width(f) >> 1), y + 8, f);
    band.Draw(ot[1], x - 0x80, y - (band.h >> 1));
    ot[1].DrawMode(0x220);
}

// ---- dialog

ResultDialog ResultDialog::Init(const RaceMenuAssets& a, uint32_t t) { // EXE 0x8006DCB8
    const GuestImage& o = a.ovl0;
    ResultDialog d;
    d.x = o.Get<int16_t>(t), d.y = o.Get<int16_t>(t + 2);
    d.flags = o.Get<uint8_t>(t + 0x14);
    const int size = d.flags & 6;
    d.w = int16_t(size == 6 ? 0x80 : (size == 2 || size == 4) ? 0x60 : 0x50);
    d.h = int16_t((size == 4 || size == 6) ? 0x18 : 0x0C);
    // The template EXE 0x80091EA8 gets the font (+0x18), the height (+0x16) and c0 (+8) first.
    d.text = TextObject::FromTemplate(a.exe, kDialogText, a.Text(o.Get<uint32_t>(t + 4)));
    d.text.font = o.Get<uint32_t>(t + 0x18);
    d.text.height = o.Get<uint8_t>(t + 0x16);
    d.text.c0 = o.Get<uint32_t>(t + 8);
    d.text.extra = o.Get<int8_t>(t + 0x15);
    d.fill = o.Get<uint32_t>(t + 0xC);
    d.gradient = o.Get<uint32_t>(t + 0x10);
    d.anim = -1;
    return d;
}

void ResultDialog::Open() { // EXE 0x8006DDD0
    text.Open(-1);
    anim = 0;
    flags |= 0x80;
}

void ResultDialog::Close() { // EXE 0x8006DE1C
    anim = -13;
    text.Close();
}

int ResultDialog::Update(const MenuListPad* pad) { // EXE 0x8006DE44
    if (anim < 0) {
        if (anim < -1) anim++;
        return -2;
    }
    if (++anim >= 72) {
        anim = 12;
        flags &= 0xFF7F;
    }
    int r = -2;
    if (pad) {
        if (pad->pressed & menu_list_pad::kChoose) r = 0;
        if (pad->pressed & menu_list_pad::kBack) r = -1;
    }
    text.Tick();
    return r;
}

void ResultDialog::Draw(const RaceMenuAssets& a, MenuOt& ot) const { // EXE 0x8006DEEC
    const uint32_t zero = a.exe.Get<uint32_t>(kDialogZero);
    const int left = x - (w >> 1);
    const int textDy = (flags & 4) ? -4 : -2;
    if (anim < 0) {
        if (anim < -1) {
            const int k = anim + 13;
            const int wd = w - (w * k) / 12;
            const uint32_t c = MenuListLerp(fill, zero, k, 12);
            AddGradientQuad(ot[0], x - (wd >> 1), y, wd, h, c, c);
            ot[0].DrawMode(0x20);
        }
        return;
    }
    const int g = anim < 12 ? (anim * 127) / 12 : 127;
    MenuListFrame(ot[0], Grey(uint32_t(g)), left, y, w, h);
    AddGradientQuad(ot[2], left, y, w, h, gradient, zero);
    ot[0].DrawMode(0x200);
    TextObject t = text;
    t.alpha = uint8_t(g);
    t.Draw(ot, 0, left + (w >> 1), y + h + textDy, a.FontAt(t.font));
    int k = 0;
    if (anim >= 12) k = std::max(0, 60 - (anim - 12) * 4);
    const uint32_t white = (flags & 0x80) ? 0xFFu : 0x80u;
    uint32_t c = MenuListLerp(fill, Grey(white), k, 60);
    c = MenuListLerp(c, zero, std::max(0, 12 - int(anim)), 12);
    ot[1].Add(Tile(left, y, w, h, c));
    ot[1].DrawMode(0x220);
}

// ---- bar

ResultBar ResultBar::Init(const RaceMenuAssets& a, uint32_t t, int cursor) { // EXE 0x8006E1CC
    const GuestImage& o = a.ovl0;
    ResultBar b;
    b.x = o.Get<int16_t>(t), b.y = o.Get<int16_t>(t + 2);
    b.flags = o.Get<uint16_t>(t + 0x20);
    const int size = b.flags & 6;
    b.w = int16_t(size == 6 ? 0x80 : (size == 2 || size == 4) ? 0x60 : 0x50);
    b.h = int16_t((size == 4 || size == 6) ? 0x18 : 0x0C);
    // The template EXE 0x80091EC8 gets the font (+0x28), the height (+0x24), the flags and c0 of each text.
    TextObject base = TextObject::FromTemplate(a.exe, kBarText, "");
    base.font = o.Get<uint32_t>(t + 0x28);
    base.height = o.Get<uint8_t>(t + 0x24);
    const uint32_t titleColour = o.Get<uint32_t>(t + 0x10), labelColour = o.Get<uint32_t>(t + 0x14);
    b.title = base;
    b.title.flags = (titleColour & 0x2000000u) ? 0xE9 : 0xC8;
    b.title.c0 = titleColour;
    b.title.text = a.Text(o.Get<uint32_t>(t + 4));
    b.label0 = base;
    b.label0.flags = (labelColour & 0x2000000u) ? 0xE1 : 0xC0;
    b.label0.c0 = labelColour;
    b.label1 = b.label0;
    b.label0.text = a.Text(o.Get<uint32_t>(t + 8));
    b.label1.text = a.Text(o.Get<uint32_t>(t + 0xC));
    b.label0.extra = b.label1.extra = o.Get<int8_t>(t + 0x22);
    b.cursor = int8_t(cursor != 0);
    b.sound = o.Get<int8_t>(t + 0x2C);
    b.fill = o.Get<uint32_t>(t + 0x18);
    b.gradient = o.Get<uint32_t>(t + 0x1C);
    b.anim = -1;
    b.slide = 0;
    return b;
}

void ResultBar::Open() { // EXE 0x8006E388
    title.Open(60);
    label0.Open(-1);
    label1.Open(-1);
    anim = 0;
    slide = 0;
    flags |= 0x80;
}

void ResultBar::Close() { // EXE 0x8006E3FC
    anim = -17;
    title.Close();
    label0.Close();
    label1.Close();
}

int ResultBar::Update(const MenuListPad* pad, std::vector<int>* sounds) { // EXE 0x8006E43C
    if (anim < 0) {
        if (anim < -1) anim++;
        title.Tick();
        return -2;
    }
    if (++anim >= 72) {
        anim = 12;
        flags &= 0xFF7F;
    }
    if (slide > 0) slide--;
    int r = -2;
    if (pad) {
        const uint32_t pressed = pad->pressed;
        if (pressed & menu_list_pad::kBack) {
            r = -1;
            if (flags & 8) Close();
        } else if (pressed & menu_list_pad::kChoose) {
            r = cursor;
            if (flags & 8) Close();
        } else {
            int c = cursor;
            if (pressed & menu_list_pad::kLeft) c = 0;
            if (pressed & menu_list_pad::kRight) c = 1;
            if (c != cursor) {
                r = -3;
                if (anim >= 12) anim = 12;
                slide = 6;
                flags |= 0x80;
                if (sound >= 0 && sounds) sounds->push_back(sound);
            }
            cursor = int8_t(c != 0);
        }
    }
    title.Tick();
    label0.Tick();
    label1.Tick();
    return r;
}

void ResultBar::Draw(const RaceMenuAssets& a, MenuOt& ot) const { // EXE 0x8006E5B8
    if (anim == -1) return;
    const uint32_t zero = a.exe.Get<uint32_t>(kBarZero);
    const int textDy = (flags & 4) ? -4 : -2;
    if (anim < -1) { // closing: the chosen button shrinks
        const int k = anim + 17;
        int cx = x + (w >> 1);
        if (cursor == 0) cx -= w;
        int s = w * k;
        if (s < 0) s += 15;
        const int wd = w - (s >> 4);
        const uint32_t c = MenuListLerp(fill, zero, k, 16);
        AddGradientQuad(ot[0], cx - (wd >> 1), y, wd, h, c, c);
        ot[0].DrawMode(0x20);
        title.Draw(ot, 0, x, y + textDy, a.FontAt(title.font));
        return;
    }
    int g = 127, off = 0;
    if (anim < 12) {
        off = (w * 2 * (12 - anim)) / 12;
        g = (anim * 127) / 12;
    }
    if (flags & 1) off = -off;
    const int cx = x + off, left = cx - w, right = cx;
    MenuListFrame(ot[0], Grey(uint32_t(g)), left, y, w, h);
    MenuListFrame(ot[0], Grey(uint32_t(g)), right, y, w, h);
    AddGradientQuad(ot[2], left, y, w, h, gradient, zero);
    AddGradientQuad(ot[2], right, y, w, h, gradient, zero);
    ot[0].DrawMode(0x200);
    title.Draw(ot, 0, x, y + textDy, a.FontAt(title.font));
    TextObject l0 = label0, l1 = label1;
    l0.alpha = l1.alpha = uint8_t(g);
    l0.Draw(ot, 0, left + (w >> 1), y + h + textDy, a.FontAt(l0.font));
    l1.Draw(ot, 0, right + (w >> 1), y + h + textDy, a.FontAt(l1.font));
    int k = 0;
    if (anim >= 12) k = std::max(0, 60 - (anim - 12) * 4);
    const uint32_t white = (flags & 0x80) ? 0xFFu : 0x80u;
    uint32_t c = MenuListLerp(fill, Grey(white), k, 60);
    c = MenuListLerp(c, zero, std::max(0, 12 - int(anim)), 12);
    ot[1].Add(Tile(cursor ? right : left, y, w, h, c));
    if (slide > 0) {
        const int sw = (w * slide) / 6;
        if (cursor) AddGradientQuad(ot[1], right - sw, y, sw, h, zero, c);
        else AddGradientQuad(ot[1], right, y, sw, h, c, zero);
    }
    ot[1].DrawMode(0x220);
}

// ---- texts

TimeStyle TimeStyle::Read(const GuestImage& o, uint32_t a) {
    TimeStyle s;
    s.colour = o.Get<uint32_t>(a), s.colour2 = o.Get<uint32_t>(a + 4);
    s.advance = o.Get<int16_t>(a + 8), s.narrow = o.Get<int16_t>(a + 0xA);
    s.lapOffset = o.Get<int16_t>(a + 0xC), s.digitShift = o.Get<int16_t>(a + 0xE);
    s.font = o.Get<uint32_t>(a + 0x10);
    return s;
}

void AddTimeDisplay(const RaceMenuAssets& a, MenuOtSlot& ot, const FadePair& fade, uint32_t value, int x, int y, int mode, const TimeStyle& style, bool centre) {
    if (fade.anim == -1) return;
    int u = fade.anim;
    bool ghosts = true;
    if (u < 0) {
        u = ~u;
        ghosts = false;
    }
    const HudFont& f = a.FontAt(style.font);
    const std::string s = FormatRaceTime(value);
    if (centre) x += f.TimeWidth(s, style.advance, style.narrow, 0) >> 1;
    const int k = 128 - (u << 7) / fade.steps;
    std::vector<HudFontSprite> glyphs;
    if (fade.anim < fade.steps && ghosts) {
        const uint32_t g = Grey(uint32_t(k)) | 0x2000000u;
        f.TimeRight(s, x - (k >> 2), y, style.advance, style.narrow, 0, 0, glyphs);
        AddSprites(ot, glyphs, g, 1);
        glyphs.clear();
        f.TimeRight(s, x + (k >> 2), y, style.advance, style.narrow, 0, 0, glyphs);
        AddSprites(ot, glyphs, g, 1);
        glyphs.clear();
    }
    const uint32_t colour = MenuListLerp(style.colour, a.ovl0.Get<uint32_t>(kTimeBase), k, 128);
    f.TimeRight(s, x, y, style.advance, style.narrow, 0, 0, glyphs);
    AddSprites(ot, glyphs, colour, mode & 3);
}

void AddLapRow(const RaceMenuAssets& a, MenuOtSlot& ot, uint32_t time, int lap, int right, int y, const TimeStyle& style, int mode) {
    const HudFont& f = a.FontAt(style.font);
    const std::string s = FormatRaceTime(time);
    std::vector<HudFontSprite> glyphs;
    const int w = f.TimeWidth(s, style.advance, style.narrow, 0);
    f.Time(s, right - w, y, style.advance, style.narrow, 0, 0, glyphs);
    AddSprites(ot, glyphs, style.colour, mode & 3);
    if (lap < 0) return;
    const int x = right - style.lapOffset;
    glyphs.clear();
    const int w2 = f.Text(a.Text(kStrLap), x, y, 1, glyphs);
    AddSprites(ot, glyphs, style.colour2, mode & 3);
    glyphs.clear();
    f.Number(std::to_string(lap), x + w2 + 6, y, 1, style.digitShift, 0, glyphs);
    AddSprites(ot, glyphs, style.colour2, mode & 3);
}

std::string MoneyText(uint32_t value) {
    const std::string digits = std::to_string(value);
    std::string out;
    for (size_t i = 0; i < digits.size(); i++) {
        if (i > 0 && (digits.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(digits[i]);
    }
    return out;
}

void AddMoneyNumber(const RaceMenuAssets& a, MenuOtSlot& ot, int right, int y, uint32_t value, int t, uint32_t colour) { // 0x8005A11C
    if (t < 0) return;
    if (t > 12) t = 12;
    const uint32_t c = MenuListLerp(0x02000000u, colour, t, 12);
    std::vector<HudFontSprite> glyphs;
    a.FontAt(0x801C9130u).NumberRight(MoneyText(value), right, y, 2, 0, 0, glyphs);
    AddSprites(ot, glyphs, c, 1);
}

// ---------------------------------------------------------------- views

WaitView::WaitView(const RaceMenuAssets& a, uint32_t view, int fields) : counter(int16_t(fields)) {
    title_ = a.Text(a.ovl0.Get<uint32_t>(view + 0x10));
    colour_ = a.ovl0.Get<uint32_t>(view + 0x0C);
}

int WaitView::Update(const MenuListPad*, bool) {
    sounds.clear();
    if (counter > 0 && --counter == 0) return 1;
    return 0;
}

// ---- A. RESULTS

ResultsView::ResultsView(const RaceMenuAssets& a) : a_(a) {
    list = MenuListWidget::Read(a.ovl0, kResList);
    AttachList();
}

void ResultsView::AttachList() {
    list.callback = [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); };
}

std::string ResultsView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t ResultsView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

void ResultsView::Setup(const ResultsInput& in) { // 0x80050FD0
    const GuestImage& o = a_.ovl0;
    battle = in.battle ? 1 : 0; // W+0 = (race block + 0x0A == 0)
    if (in.battle) {            // 0x80051058: the winner's text (0x801C7814 + 23 * winner), the colour of place 1
        place = TextObject::FromTemplate(o, kResPlace, a_.Text(in.winner ? kStrPlayer2Wins : kStrPlayer1Wins));
        place.c0 = o.Get<uint32_t>(kPlaceColours);
    } else {
        uint32_t p = uint32_t(in.place - 1);
        if (p >= 6) p = 0;
        place = TextObject::FromTemplate(o, kResPlace, a_.Text(o.Get<uint32_t>(kPlaceStrings + p * 4)));
        place.c0 = o.Get<uint32_t>(kPlaceColours + p * 4);
    }
    placeBand = Band::Read(o, kResPlaceBand);
    placeBand.anim = -1;
    total = in.totalTime;
    fastest = in.fastestLap;
    total2 = in.battle ? in.totalTime2 : 0;
    fastest2 = in.battle ? in.fastestLap2 : 0;
    lapCount = int16_t(std::min<size_t>(in.laps.size(), 10));
    lapTimes.assign(in.laps.begin(), in.laps.begin() + lapCount);
    lapTimes2.assign(size_t(lapCount), 0xFFFFFFFFu);
    for (size_t i = 0; in.battle && i < size_t(lapCount) && i < in.laps2.size(); i++) lapTimes2[i] = in.laps2[i];
    lapNumbers.clear();
    for (int i = 0; i < lapCount; i++) lapNumbers.push_back(int16_t(in.firstLap + i));
    t = 0, car = -1, done = 0;
    course = CourseTitle::Init(a_, in.course);
    course.Open();
    resultsLabel = ResultLabel::Init(a_, kResLabel);
    totalLabel = ResultLabel::Init(a_, kResTotalLabel);
    fastestLabel = ResultLabel::Init(a_, kResFastestLabel);
    lapLabel = ResultLabel::Init(a_, kResLapLabel);
    totalFade = {FadePair::Read(o, kResTotalFade).steps, -1};
    fastestFade = {FadePair::Read(o, kResFastestFade).steps, -1};
    lapFade = {FadePair::Read(o, kResLapFade).steps, -1};
    totalBand = Band::Read(o, kResTotalBand), totalBand.anim = -1;
    fastestBand = Band::Read(o, kResFastestBand), fastestBand.anim = -1;
    lapBand = Band::Read(o, kResLapBand), lapBand.anim = -1;
    list = MenuListWidget::Read(o, kResList);
    list.count = lapCount;
    MenuListReset(list, [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); });
    revealed = 0, revealPeriod = 0;
    saveBar = in.saveBar;
    bar = ResultBar::Init(a_, kResBar, 0);
    dialog = ResultDialog::Init(a_, kResDialog);
    carCamera = menu::ResultsModelCamera(in.vsync); // 0x80050BC4
    save_ = false;
}

std::optional<PostRaceModel> ResultsView::Model() const { // 0x80051BF4: 0x80048754(W+0x94, M+0xC0, M+0xD0, W+0x1D8, 0)
    if (car < 0) return std::nullopt;
    PostRaceModel m;
    m.camera = carCamera;
    m.envX = 0, m.envY = 0x96; // 0x8008034C(M+0xC0, (0, 0x96, 0x160, 300))
    return m;
}

int32_t ResultsView::ListCallback(int command, int row, const MenuListRowDraw* d) { // 0x800505AC
    const GuestImage& o = a_.ovl0;
    if (command == kMenuListEnabled) return 1;
    if (command == kMenuListReveal) {
        revealed++;
        int k = revealed / 10;
        if (k >= 5) k = 4;
        revealPeriod = o.Get<int16_t>(kResRevealTable + uint32_t(k) * 2);
        list.revealPeriod = revealPeriod;
        lapFade.anim = 0;
        lapBand = Band::Read(o, kResLapBand);
        lapBand.anim = 0;
        MenuListSelect(list, row - 2);
        return 0;
    }
    if (command != kMenuListDraw || !d || !drawOt_) return 0;
    MenuOt& ot = *drawOt_; // d->ot = slot 2
    // x 250 (game mode 0: player 1 at 210, player 2's column {W+0x40 + row * 4, lap -1} at 300)
    const int xr = battle ? 0xD2 : 0xFA, xr2 = xr + 90, y = d->y + 8;
    const uint32_t time = row < int(lapTimes.size()) ? lapTimes[size_t(row)] : 0;
    const uint32_t time2 = row < int(lapTimes2.size()) ? lapTimes2[size_t(row)] : 0xFFFFFFFFu;
    const int lap = row < int(lapNumbers.size()) ? lapNumbers[size_t(row)] : -1;
    TimeStyle style = TimeStyle::Read(o, kResLapStyle);
    if (list.state >= 0) {
        if (row < revealed) {
            int a = (d->alpha * 100) >> 7;
            if (row == revealed - 1) {
                a = (a * lapFade.Alpha()) >> 7;
                if (lapFade.anim != lapFade.steps) {
                    const int g = std::clamp(128 - a, 0, 128);
                    TimeStyle ghost = style;
                    ghost.colour = ghost.colour2 = Grey(uint32_t(g)) | 0x2000000u;
                    AddLapRow(a_, ot[3], time, lap, xr - (g >> 2), y, ghost, 1);
                    AddLapRow(a_, ot[3], time, lap, xr + (g >> 2), y, ghost, 1);
                    if (battle) { // 0x800508C0: the second column's ghosts are player 1's row again (with its lap label)
                        AddLapRow(a_, ot[3], time, lap, xr2 - (g >> 2), y, ghost, 1);
                        AddLapRow(a_, ot[3], time, lap, xr2 + (g >> 2), y, ghost, 1);
                    }
                }
            }
            style.colour = Grey(uint32_t(a));
            style.colour2 = uint32_t(a >> 1) | uint32_t(a >> 1) << 8 | uint32_t(a) << 16;
            AddLapRow(a_, ot[3], time, lap, xr, y, style, 0);
            if (battle) AddLapRow(a_, ot[3], time2, -1, xr2, y, style, 0);
        }
        if (row == revealed - 1) {
            lapBand.Draw(ot[2], d->x - 70, y - 16);
            ot[2].DrawMode(0x220);
        }
    }
    if (list.state < -1) { // closing
        const int k = MenuListAlpha(list);
        const int a = (d->alpha * 100) >> 7;
        const int v = a * k;
        const int s = v >> 7;
        style.colour = Grey(uint32_t(s));
        style.colour2 = uint32_t(v >> 8) | uint32_t(v >> 8) << 8 | uint32_t(s) << 16;
        AddLapRow(a_, ot[3], time, lap, xr, y, style, 0);
        if (battle) AddLapRow(a_, ot[3], time2, -1, xr2, y, style, 0);
        lapBand.Draw(ot[2], d->x - 70, y - 16);
        ot[2].DrawMode(0x220);
    }
    return 0;
}

int ResultsView::Update(const MenuListPad* pad, bool input) { // 0x8005162C
    sounds.clear();
    menu::TurnModelCamera(carCamera, 12, frameLength); // 0x80050CC4 (at the end of every update; only the draw reads it)
    int s = t;
    if (done == 0) s++;
    switch (s) {
    case 24: car = 0; break;
    case 48: resultsLabel.Open(); break;
    case 60: place.Open(-1), placeBand.anim = 0; break;
    case 72: totalLabel.Open(); break;
    case 84: totalFade.anim = 0, totalBand.anim = 0; break;
    case 96: fastestLabel.Open(); break;
    case 108: fastestFade.anim = 0, fastestBand.anim = 0; break;
    case 120: lapLabel.Open(); break;
    case 132: MenuListOpen(list); break;
    case 133:
        s = 132;
        if (revealed >= lapCount) {
            MenuListClamp(list, 2, lapCount - 3);
            s = 133;
        }
        break;
    case 163:
        done = 1;
        if (saveBar) {
            bar.Open();
            bar.cursor = 1;
        } else {
            dialog.Open();
        }
        s++;
        break;
    default: break;
    }
    t = int16_t(s);
    course.Tick();
    resultsLabel.Tick();
    place.Tick();
    placeBand.Tick();
    totalLabel.Tick();
    totalFade.Tick();
    totalBand.Tick();
    fastestLabel.Tick();
    fastestFade.Tick();
    fastestBand.Tick();
    lapLabel.Tick();
    MenuListUpdate(list, revealed >= lapCount ? (pad ? pad : &kNoButtons) : nullptr);
    lapFade.Tick();
    lapBand.Tick();
    const MenuListPad* buttons = (s < 163 || !input) ? nullptr : (pad ? pad : &kNoButtons);
    int r;
    if (saveBar) {
        r = bar.Update(buttons, &sounds);
    } else {
        r = dialog.Update(buttons);
        if (r == 0) r = 1;
    }
    if (r == -1) sounds.push_back(0);
    if (r != 0 && r != 1) return 0;
    save_ = r == 0;
    if (saveBar) bar.Close();
    else dialog.Close();
    sounds.push_back(3);
    course.Close();
    resultsLabel.Close();
    place.Close();
    CloseBand(placeBand);
    totalLabel.Close();
    totalFade.Close();
    CloseBand(totalBand);
    fastestLabel.Close();
    fastestFade.Close();
    CloseBand(fastestBand);
    lapLabel.Close();
    MenuListClose(list);
    lapFade.Close();
    CloseBand(lapBand);
    car = -1;
    return 1;
}

void ResultsView::Draw(MenuOt& ot) const { // 0x80051BF4 (the 3D car 0x80048754: Model())
    const RaceMenuAssets& a = a_;
    const GuestImage& o = a.ovl0;
    const int X = battle ? 0xD2 : 0xFA; // W+0 != 0: 210, the second column at 300
    course.Draw(a, ot, 0xB0, 100);
    bar.Draw(a, ot);
    dialog.Draw(a, ot);
    resultsLabel.Draw(a, ot, 2, 0x28, 0x88);
    place.Draw(ot, 0, 0xB0, 0xB8, a.FontAt(place.font));
    placeBand.Draw(ot[1], 0x60, 0x88);
    ot[1].DrawMode(0x220);
    const TimeStyle style = TimeStyle::Read(o, kResTimeStyle);
    totalLabel.Draw(a, ot, 2, 0x28, 0xD2);
    totalBand.Draw(ot[1], 0x74, 0xE2);
    ot[1].DrawMode(0x220);
    AddTimeDisplay(a, ot[1], totalFade, total, X, 0xF2, 0, style);
    if (battle) AddTimeDisplay(a, ot[1], totalFade, total2, X + 90, 0xF2, 0, style);
    fastestLabel.Draw(a, ot, 2, 0x28, 0x101);
    fastestBand.Draw(ot[1], 0x74, 0x111);
    ot[1].DrawMode(0x220);
    AddTimeDisplay(a, ot[1], fastestFade, fastest, X, 0x121, 0, style);
    if (battle) AddTimeDisplay(a, ot[1], fastestFade, fastest2, X + 90, 0x121, 0, style);
    lapLabel.Draw(a, ot, 1, 0x28, 0x130);
    drawOt_ = &ot;
    MenuListDraw(list, ot[2]);
    drawOt_ = nullptr;
}

// ---- B. BONUS

BonusView::BonusView(const RaceMenuAssets& a) : a_(a) {}

std::string BonusView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(view + 0x10)); }
uint32_t BonusView::Colour() const { return a_.ovl0.Get<uint32_t>(view + 0x0C); }

void BonusView::Setup(const BonusInput& in) {
    const GuestImage& o = a_.ovl0;
    view = in.kind == BonusKind::kSingleRace ? kViewSingle : in.kind == BonusKind::kChampionshipRace ? kViewChampionship : kViewChampionshipEnd;
    // 0x800595E0(place - 1, prize, car)
    placeIndex = int16_t(in.kind == BonusKind::kChampionshipEnd ? 0 : std::clamp(in.place - 1, 0, 5));
    done = 0, closing = 0;
    remaining = in.prize;
    car = in.prizeCar ? 1u : 0u;
    t = 0;
    shown = in.money - in.prize;
    speed = 3;
    prizeFade = moneyFade = 0;
    model = 0, modelAnim = 0;
    resultsLabel = ResultLabel::Init(a_, kBonResLabel);
    place = TextObject::FromTemplate(o, kBonPlace, a_.Text(o.Get<uint32_t>(kPlaceStrings + uint32_t(placeIndex) * 4)));
    place.c0 = o.Get<uint32_t>(kPlaceColours + uint32_t(placeIndex) * 4);
    bonusLabel = ResultLabel::Init(a_, kBonLabel);
    moneyLabel = ResultLabel::Init(a_, kBonMoneyLabel);
    newCarBand = Band::Read(o, kBonNewCarBand);
    newCarBand.anim = -1;
    bar = ResultBar::Init(a_, kBonBar, 0);
    dialog = ResultDialog::Init(a_, kBonDialog);
    useBar = in.kind == BonusKind::kChampionshipRace ? 0u : 1u;
    if (in.kind == BonusKind::kChampionshipEnd) { // 0x80059800: the trophy model, the count 45 fields later
        trophyCamera = menu::ModelViewCamera(0x160, 0x1E0);
        trophyCamera.floor = false; // W+0x428 = camera +0xD4 = 0 (no floor disc under the trophy)
        model = 1;
        t = -45;
    }
    save_ = false;
}

std::optional<PostRaceModel> BonusView::Model() const { // 0x8005A218: 0x80048754(W+0x210, M+0xC0, M+0xD0, W+0x354, 1)
    if (model == 0 || modelAnim <= 0x17) return std::nullopt;
    PostRaceModel m;
    m.trophy = true;
    m.camera = trophyCamera;
    m.envX = modelEnvX, m.envY = modelEnvY;
    m.envClut = 0x6028, m.envPage = 0x1A, m.reflection = 0x20; // 0x80059800: W+0x210 = 0x6028, W+0x212 = 0x1A, W+0x219 = 0x20
    return m;
}

int BonusView::Update(const MenuListPad* pad, bool input) { // 0x80059BAC
    sounds.clear();
    int s = t;
    if (done == 0) s++;
    switch (s) {
    case 48: resultsLabel.Open(), sounds.push_back(1); break;
    case 60: place.Open(-1), sounds.push_back(1); break;
    case 72: bonusLabel.Open(), sounds.push_back(1); break;
    case 84: sounds.push_back(1); break;
    case 96: moneyLabel.Open(), sounds.push_back(1); break;
    case 108: sounds.push_back(1); break;
    case 144:
        if (remaining != 0) {
            if (++speed > 3000) speed = 3000;
            const uint32_t k = speed / 3;
            uint32_t step = (k * k) / 50 + 1;
            if (pad && (pad->held & menu_list_pad::kChoose)) step = step * 10 + 1;
            s = 143;
            if (remaining < step) step = remaining;
            remaining -= step;
            shown += step;
            sounds.push_back(remaining != 0 ? 8 : 3);
        }
        break;
    case 146:
        if (car) {
            sounds.push_back(3);
            newCarBand.anim = 0;
        }
        break;
    case 206:
        done = 1;
        if (useBar) {
            bar.Open();
            bar.cursor = 1;
        } else {
            dialog.Open();
        }
        s++;
        break;
    default: break;
    }
    t = int16_t(s);
    resultsLabel.Tick();
    place.Tick();
    bonusLabel.Tick();
    moneyLabel.Tick();
    newCarBand.Tick();
    prizeFade = int16_t(s - 84);
    moneyFade = int16_t(s - 108);
    if (closing) prizeFade = moneyFade = -1;
    if (model && modelAnim >= 0 && ++modelAnim > 0x18) { // the trophy's pose (0x800593F4) from 0x19 updates on
        if (modelAnim > 0x90) modelAnim = 0x90;
        menu::ChampionModelMotion(trophyCamera, modelAnim - 0x18);
    }
    const MenuListPad* buttons = (s < 206 || !input) ? nullptr : (pad ? pad : &kNoButtons);
    int r;
    if (useBar) {
        r = bar.Update(buttons, &sounds);
    } else {
        r = dialog.Update(buttons);
        if (r == 0) r = 1;
    }
    if (r == -1) sounds.push_back(0);
    if (r != 0 && r != 1) return 0;
    save_ = r == 0;
    sounds.push_back(3);
    closing = 1;
    resultsLabel.Close();
    place.Close();
    bonusLabel.Close();
    moneyLabel.Close();
    if (car) CloseBand(newCarBand);
    if (useBar) bar.Close();
    else dialog.Close();
    modelAnim = -1;
    return 1;
}

void BonusView::Draw(MenuOt& ot) const { // 0x8005A218 (the trophy 0x80048754: Model())
    const RaceMenuAssets& a = a_;
    bar.Draw(a, ot);
    dialog.Draw(a, ot);
    if (newCarBand.anim >= 0) {
        const uint32_t colour = MenuListLerp(0x02000000u, 0x02505050u, BandAlpha(newCarBand), 128);
        const HudFont& f = a.FontAt(0x801C9150u);
        const std::string text = a.Text(kStrNewCar);
        std::vector<HudFontSprite> glyphs;
        f.Text(text, 0xB0 - (f.TextWidth(text, 1) >> 1), 0x128, 1, glyphs); // 0x8006ADB4
        AddSprites(ot[0], glyphs, colour, 1);
    }
    newCarBand.Draw(ot[0], 0, 0x10C);
    ot[0].DrawMode(0x220);
    resultsLabel.Draw(a, ot, 2, 0x28, 0x88);
    place.Draw(ot, 0, 0xB0, 0xB8, a.FontAt(place.font));
    bonusLabel.Draw(a, ot, 2, 0x28, 0xD2);
    AddMoneyNumber(a, ot[0], 0x100, 0x102, remaining, prizeFade, 0x02503C28u);
    moneyLabel.Draw(a, ot, 2, 0x28, 0x140);
    AddMoneyNumber(a, ot[0], 0x100, 0x170, shown, moneyFade, 0x02505050u);
}

// ---- C. post-race menu

PostRaceMenuView::PostRaceMenuView(const RaceMenuAssets& a) : a_(a) {
    list = MenuListWidget::Read(a.ovl0, kPostList);
    AttachList();
}

void PostRaceMenuView::AttachList() {
    list.callback = [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); };
}

uint32_t PostRaceMenuView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

void PostRaceMenuView::Setup(const PostMenuInput& in) { // 0x80049D90
    const GuestImage& o = a_.ovl0;
    counter = 16, car = -1, confirming = 0;
    mode = in.mode;
    uint32_t titleAddress = kStrSingleRace, table = kPostRows2p;
    int count = 4;
    if (mode == 2) {
        titleAddress = kStrChampionship;
        if (in.race + 1 < in.races) table = kPostRowsChampNext, count = 4;
        else table = kPostRowsChamp, count = 3;
    } else if (mode == 0) {
        titleAddress = kStr2Player;
    } else if (mode == 11) {
        titleAddress = kStrRally, table = kPostRowsRally, count = 2;
    }
    title = a_.Text(titleAddress);
    if (in.firstEntry || rows.size() != size_t(count)) {
        rows.clear();
        for (int r = 0; r < count; r++) {
            const uint32_t e = table + uint32_t(r) * 8;
            rows.push_back({a_.Text(o.Get<uint32_t>(e)), o.Get<int8_t>(e + 4) != 0, o.Get<int8_t>(e + 5)});
        }
        rowText.assign(size_t(count), TextObject{});
        rowBand.assign(size_t(count), Band{});
        list = MenuListWidget::Read(o, kPostList);
        list.count = list.visible = int16_t(count);
        MenuListReset(list, [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); });
    }
    bar = ResultBar::Init(a_, kPostBar, 0);
    course = CourseTitle::Init(a_, in.course);
    course.Open();
    resultsLabel = ResultLabel::Init(a_, kPostResLabel);
    resultsLabel.value = a_.Text(in.place > 0 ? o.Get<uint32_t>(kPostPlaces + uint32_t(in.place - 1) * 4) : kStrRetire);
    totalLabel = ResultLabel::Init(a_, kPostTotalLabel);
    totalLabel.time = in.totalTime;
    fastestLabel = ResultLabel::Init(a_, kPostFastestLabel);
    fastestLabel.time = in.fastestLap;
    if (mode == 0) { // 0x80049F78: sprintf(W+0x614 / W+0x634, 0x801C7842 "win %d", career + 0xFC / + 0xFE), the labels' values
        auto wins = [&](uint32_t descriptor, uint16_t count) {
            ResultLabel l = ResultLabel::Init(a_, descriptor);
            char text[32];
            std::snprintf(text, sizeof text, a_.Text(kStrWinCount).c_str(), unsigned(count));
            l.value = text;
            return l;
        };
        winsLabel1 = wins(kPostWinsLabel1, in.wins[0]);
        winsLabel2 = wins(kPostWinsLabel2, in.wins[1]);
    }
    carCamera = menu::ModelViewCamera(200, 200); // 0x80049780(W+0x364, 200, 200)
    action_ = 0;
}

std::optional<PostRaceModel> PostRaceMenuView::Model() const { // 0x8004A55C: 0x80048754(W+0x220, M+0xC0, M+0xD0, W+0x364, 0)
    if (car < 0) return std::nullopt;
    PostRaceModel m;
    m.camera = carCamera;
    m.envX = 0x7C, m.envY = 0xA0; // 0x8008034C(M+0xC0, (0x7C, 0xA0, 200, 200))
    return m;
}

int32_t PostRaceMenuView::ListCallback(int command, int row, const MenuListRowDraw* d) { // 0x80049A14
    if (row < 0 || row >= int(rows.size())) return 0;
    TextObject& text = rowText[size_t(row)];
    Band& band = rowBand[size_t(row)];
    switch (command) {
    case kMenuListReset:
        text = TextObject::FromTemplate(a_.ovl0, kPostRowText, rows[size_t(row)].text);
        band = Band::Read(a_.ovl0, kPostRowBand);
        band.anim = -1;
        break;
    case kMenuListReveal: text.Open(-1), band.anim = 0; break;
    case kMenuListClose: text.Close(), CloseBand(band); break;
    case kMenuListTick: text.Tick(), band.Tick(); break;
    case kMenuListDraw:
        if (d && drawOt_) {
            TextObject t = text;
            t.alpha = uint8_t(d->alpha);
            t.Draw(*drawOt_, 0, d->x, d->y + 0x18, a_.FontAt(t.font));
            band.Draw((*drawOt_)[1], d->x - 8, d->y - 2);
            (*drawOt_)[1].DrawMode(0x220);
        }
        break;
    case kMenuListLeave: text.c0 = 0x707070, text.flags &= 0xFFF7; break;
    case kMenuListEnter: text.c0 = 0x907040, text.flags |= 8, text.Restart(); break;
    case kMenuListOpen: text.c0 = 0x907040, text.flags |= 8; break;
    case kMenuListEnabled: return rows[size_t(row)].enabled ? 1 : 0;
    default: break;
    }
    return 0;
}

int PostRaceMenuView::Update(const MenuListPad* pad, bool input) { // 0x8004A0BC (mode 0: the two win labels, else the 1P labels)
    sounds.clear();
    menu::TurnModelCamera(carCamera, 16, frameLength); // 0x80049874
    const bool battle = mode == 0;
    if (counter > 0 && --counter == 0) {
        MenuListOpen(list);
        list.revealPeriod = -1;
        car = 0;
        if (battle) {
            winsLabel1.Open();
            winsLabel2.Open();
        } else {
            resultsLabel.Open();
            totalLabel.Open();
            fastestLabel.Open();
        }
    }
    course.Tick();
    if (battle) {
        winsLabel1.Tick();
        winsLabel2.Tick();
    } else {
        resultsLabel.Tick();
        totalLabel.Tick();
        fastestLabel.Tick();
    }
    const MenuListPad* buttons = input ? (pad ? pad : &kNoButtons) : nullptr;
    bool leave = false;
    if (confirming == 1) {
        MenuListUpdate(list, nullptr);
        const int r = bar.Update(buttons, &sounds);
        if (r == -1) {
            sounds.push_back(2);
            confirming = 0;
        } else if (r == 0) {
            sounds.push_back(3);
            leave = true;
            action_ = 2;
        } else if (r == 1 || r < -3) {
            sounds.push_back(1);
            confirming = 0;
        }
    } else {
        bar.Update(nullptr, &sounds);
        const int r = MenuListUpdate(list, buttons);
        if (r == -3) {
            sounds.push_back(6);
        } else if (r == -4 || r == -1) {
            sounds.push_back(0);
        } else if (r >= 0 && r < int(rows.size())) {
            const int act = rows[size_t(r)].action;
            if (mode == 2 && act == 2) {
                sounds.push_back(1);
                bar.Open();
                bar.cursor = 1;
                confirming = 1;
            } else {
                leave = true;
                sounds.push_back(3);
                action_ = act;
            }
        }
    }
    if (!leave) return 0;
    MenuListClose(list);
    course.Close();
    if (battle) {
        winsLabel1.Close();
        winsLabel2.Close();
    } else {
        resultsLabel.Close();
        totalLabel.Close();
        fastestLabel.Close();
    }
    car = -1;
    return 1;
}

void PostRaceMenuView::Draw(MenuOt& ot) const { // 0x8004A55C (the 3D car 0x80048754: Model())
    const RaceMenuAssets& a = a_;
    bar.Draw(a, ot);
    course.Draw(a, ot, 0xB0, 0x68);
    if (mode == 0) { // 0x8004A5C4: the win labels at (0x20, 0x186) / (0xD0, 0x186)
        winsLabel1.Draw(a, ot, 1, 0x20, 0x186);
        winsLabel2.Draw(a, ot, 1, 0xD0, 0x186);
    } else {
        resultsLabel.Draw(a, ot, 1, 0x30, 0x186);
        totalLabel.Draw(a, ot, 1, 0xA8, 0x186);
        fastestLabel.Draw(a, ot, 1, 0xA8, 0x1A2);
    }
    drawOt_ = &ot;
    MenuListDraw(list, ot[0]);
    drawOt_ = nullptr;
}

// ---------------------------------------------------------------- frames

std::vector<MenuPrim> BuildPostRaceFrame(const RaceMenuAssets& a, const PostRaceView& view, int headerAlpha) {
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    AddRaceViewHeader(ot, a, view.Title(), view.Colour(), headerAlpha, true);
    view.Draw(ot);
    ot.Emit(prims, 0x200);
    return prims;
}

void PostRaceFlow::Start(std::unique_ptr<PostRaceView> first, bool transition) {
    previous_.reset();
    current_ = std::move(first);
    transition_ = transition ? 16 : 0;
}

void PostRaceFlow::Switch(std::unique_ptr<PostRaceView> next) {
    previous_ = std::move(current_);
    current_ = std::move(next);
    transition_ = 16;
}

int PostRaceFlow::Update(const MenuListPad* pad) { // 0x800474F4
    if (transition_ > 0) {
        transition_--;
        if (transition_ > 0 && previous_) previous_->Update(nullptr, false);
        if (transition_ == 0) previous_.reset();
    }
    return current_ ? current_->Update(pad, true) : 0;
}

std::vector<MenuPrim> BuildPostRaceViewPart(const RaceMenuAssets& a, const PostRaceView& view, int headerAlpha, bool entering) {
    std::vector<MenuPrim> part;
    MenuOt ot;
    AddRaceViewHeader(ot, a, view.Title(), view.Colour(), headerAlpha, entering);
    view.Draw(ot);
    ot.Emit(part, 0x200);
    return part;
}

std::vector<MenuPrim> BuildPostRaceTransitionFrame(const RaceMenuAssets& a, const PostRaceView* previous, const PostRaceView& current, int c) { // 0x800479AC
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    // One view in its draw environment: offset (0, dy), drawing area (0, max(dy, 0)) - (351, 479). Its top edge is the
    // frame's for the leaving view (dy <= 0) and below everything the entering view draws (its primitives lie at
    // y >= 0 before the offset), so the frame's own clipping is the drawing area; sprites and tiles are cut to the frame
    // here (the texel rows kept), as the GPU clips them.
    auto add = [&](const PostRaceView& view, int alpha, bool entering, int dy) {
        for (MenuPrim p : BuildPostRaceViewPart(a, view, alpha, entering)) {
            for (int k = 0; k < 4; k++) p.y[k] = int16_t(p.y[k] + dy);
            if (p.kind == MenuPrim::kSprite || p.kind == MenuPrim::kTile) {
                const int cut = std::max(0, -int(p.y[0]));
                const int h = std::min(int(p.h) - cut, RaceMenuAssets::kScreenHeight - std::max(0, int(p.y[0])));
                if (h <= 0) continue;
                p.y[0] = int16_t(p.y[0] + cut);
                if (p.kind == MenuPrim::kSprite) p.v = uint8_t(p.v + cut);
                p.h = int16_t(h);
            }
            prims.push_back(p);
        }
    };
    if (c > 0 && previous) add(*previous, c * 8, false, (c - 16) * 5);
    add(current, 128 - c * 8, true, (c * 200) >> 4);
    return prims;
}

std::vector<MenuPrim> PostRaceFlow::Frame(const RaceMenuAssets& a) const {
    if (!current_) return shell::TitleFrameStart();
    return BuildPostRaceTransitionFrame(a, previous_.get(), *current_, transition_);
}

std::vector<MenuPrim> BuildPostRaceModelFloor(const PostRaceModel& model) {
    if (!model.camera.floor) return {};
    return menu::OverlayModelFloor(model.Projection(false), model.camera);
}

std::vector<MenuPrim> PostRaceFlow::Frame(const RaceMenuAssets& a, size_t& modelAt, std::optional<PostRaceModel>& model) const {
    std::vector<MenuPrim> prims = Frame(a);
    model.reset();
    modelAt = prims.size();
    if (!current_) return prims;
    // 0x800479AC: environment 0 (the clear), then the model environment M+0xC0 (the views' draws put the model there),
    // then the views. Only the view whose model flag is on draws (the flags are off during the switches).
    model = current_->Model();
    if (!model && Previous()) model = Previous()->Model();
    const size_t clear = shell::TitleFrameStart().size();
    modelAt = clear;
    if (!model) return prims;
    const std::vector<MenuPrim> floor = BuildPostRaceModelFloor(*model);
    prims.insert(prims.begin() + std::ptrdiff_t(clear), floor.begin(), floor.end());
    modelAt = clear + floor.size();
    return prims;
}

} // namespace gt2::screens
