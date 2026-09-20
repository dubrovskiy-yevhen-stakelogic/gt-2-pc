#include "game/arcade/arcade_menus.h"
#include "gt2formats/exe_profile.h"
#include "game/pc_features.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "game/arcade/arcade_battle.h"
#include "game/arcade/arcade_bonus.h"
#include "game/arcade/arcade_card.h"
#include "game/arcade/arcade_car_page.h"
#include "game/arcade/arcade_course_page.h"
#include "game/arcade/arcade_results.h"
#include "game/arcade/arcade_widgets.h"
#include "game/shell/title_draw.h"
#include "gt2formats/car_info.h"
#include "gt2formats/title_assets.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2::arcade {

namespace {

namespace mp = menu_list_pad;

// Arcade v1.1 member 2.
constexpr uint32_t kModeList = 0x8004F8BCu, kGameList = 0x8004F938u, kLevelList = 0x8004F9FCu, kClassList = 0x8004FAA0u;
constexpr uint32_t kRallyList = 0x8004FB5Cu, kViewRallyClass = 0x800521E4u; // 0x8001E284 / 0x8001E330: Rally Car / Home / Guest Garage
constexpr uint32_t kClassColours = 0x8004FAC0u, kRallyCarColour = 0x2084B6u;   // 0x8001E094 / 0x8001E330 -> 0x80052244
constexpr uint32_t kClassItemsWithS = 0x8004FA28u, kClassItemsNoS = 0x8004FA3Cu; // 0x8001DF88
constexpr uint32_t kViewMode = 0x80052040u, kViewGame = 0x80052094u, kViewLevel = 0x8005213Cu, kViewClass = 0x80052190u,
                   kViewCar = 0x80052238u, kViewCourse = 0x80052334u; // {init, update, draw, colour +0x0C, title +0x10}
constexpr uint32_t kViewGarage = 0x8005228Cu, kGarageTitles = 0x8004FC08u; // HOME / GUEST GARAGE (0x8001F8CC ..)
constexpr uint32_t kViewBonus = 0x800525C8u;                                // BONUS ITEMS (arcade_bonus.h)
constexpr uint32_t kViewGuestLoad = 0x800523DCu;                            // LOAD GUEST GARAGE (arcade_card.h)
constexpr uint32_t kViewGame2P = 0x800520E8u, kGame2PList = 0x8004F980u;    // 2P GAME SELECTION (0x8001DB10 ..)
constexpr uint32_t kViewBattle = 0x800522E0u;                               // 2PLAYER BATTLE (arcade_battle.h)
constexpr int kTransitionFields = 0x10;                                // 0x8001419C
constexpr int kViewDelay = 0x18;                                       // view + 0x14 at init

MenuPrim Quad(MenuPrim::Kind kind, std::array<int, 4> xs, std::array<int, 4> ys, std::array<uint32_t, 4> colours, bool semi, bool gouraud) {
    MenuPrim p;
    p.kind = kind;
    for (int i = 0; i < 4; i++) {
        p.x[i] = int16_t(xs[size_t(i)]);
        p.y[i] = int16_t(ys[size_t(i)]);
        p.colour[i] = colours[size_t(i)];
    }
    p.semi = semi;
    p.gouraud = gouraud;
    return p;
}

// 0x8006B5F4 / 0x80011A34: POLY_G4 0x3A over {x, y, w, h}: c0 on the left edge (v0, v2), c1 on the right (v1, v3).
MenuPrim GradientRect(int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
    return Quad(MenuPrim::kPolyG4, {x, x + w, x, x + w}, {y, y, y + h, y + h}, {c0, c1, c0, c1}, true, true);
}

// 0x8006B458: c0 + (c1 - c0) * t / n per channel (t clamped to 0..n).
uint32_t Lerp(uint32_t c0, uint32_t c1, int t, int n) {
    if (n < t) t = n;
    if (t < 0) t = 0;
    uint32_t out = 0;
    for (int k = 0; k < 3; k++) {
        const int a = int((c0 >> (8 * k)) & 0xFF), b = int((c1 >> (8 * k)) & 0xFF);
        out |= uint32_t(a + ((b - a) * t) / n) << (8 * k);
    }
    return out & 0xFFFFFF;
}

// 0x8006B724(ot, {x centre, y, w, h, c0, c1}, t): the sweep of the selected panel.
void Highlight(MenuOtSlot& ot, int cx, int y, int w, int h, uint32_t c0, uint32_t c1, int t) {
    const int left = cx - (w >> 1);
    int k = t > 0x10 ? 0x10 : t;
    int seg = w * k;
    if (seg < 0) seg += 0xF;
    seg >>= 4;
    uint32_t mode = 0x200;
    int a = 0x40 - t;
    if (a < 0) {
        a = 0;
        mode = 0;
    }
    if (a > 0x30) a = 0x30;
    const uint32_t mid = Lerp(c0, c1, a, 0x30);
    ot.Add(GradientRect(left, y, seg, h, c0, mid));
    ot.Add(GradientRect(left + (w - seg), y, seg, h, mid, c0));
    if (t <= 0x10) {
        ot.Add(GradientRect(left + seg, y, w - seg, h, mid, c0));
        ot.Add(GradientRect(left, y, w - seg, h, c0, mid));
    }
    ot.DrawMode(uint16_t(mode | 0x20));
}

// 0x80081388: a SPRT 0x64 (0x66 with colour bit 25).
MenuPrim Sprite(int x, int y, const PanelSprite& s, uint32_t colour) {
    MenuPrim p;
    p.kind = MenuPrim::kSprite;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y);
    p.w = int16_t(s.w), p.h = int16_t(s.h);
    p.u = s.u, p.v = s.v;
    p.clut = s.clut;
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

} // namespace

// 0x8001C1BC.
void PanelItemTick(PanelItem& p) {
    if (p.anim < 0) {
        if (p.anim < -1) p.anim++;
    } else {
        p.anim++;
        if (int(p.steps) + int(p.pulse) <= int(p.anim)) p.anim = p.steps;
    }
}

// 0x8001C218 (a scale other than 0x80, the 2PLAYER BATTLE page's lists: 0x40, sizes * scale >> 7 and the sprites as the scaled
// quads 0x80011C2C / 0x80011D58).
void PanelItemDraw(const PanelItem& p, MenuOtSlot& ot) {
    if (p.anim == -1) return;
    const int scale = p.scale;
    auto scaled = [&](int v) { // (v * scale) >> 7, rounded toward zero
        if (scale == 0x80) return v;
        const int m = v * scale;
        return m < 0 ? (m + 0x7F) >> 7 : m >> 7;
    };
    const int steps = p.steps;
    int halfW = scaled(int16_t(p.sprite.w) >> 1);
    int x = p.x;
    const int y = p.y;
    const uint8_t anchor = p.flags & 0x60;
    if (anchor == 0x20) x += halfW;
    else if (anchor == 0x40) x -= halfW;
    int anim = p.anim;
    if (anim < 0) {
        if (anim > -2) return;
        const int t = steps + 1 + anim;
        const int w = scaled(int16_t(p.sprite.w));
        const int halfH = scaled(int16_t(p.sprite.h) >> 1);
        int hh = halfH - (halfH * t) / steps;
        int bright = (int(p.brightness) + (-int(p.brightness) * t) / steps) * 2;
        if (bright > 0xC0) bright = 0xC0;
        const int left = x - halfW;
        const int shrink = (w * t) / steps;
        const int spread = (int(p.spread) * t) / steps;
        const int width = (halfW * 2 + spread) - shrink;
        const int top = p.y - hh;
        const auto grey = [](int v) { return uint32_t(v) | uint32_t(v) << 8 | uint32_t(v) << 16; };
        ot.Add(GradientRect(left + shrink, top, width, hh * 2, grey(0), grey(bright)));
        ot.Add(GradientRect(left - spread, top, width, hh * 2, grey(bright), grey(0)));
        ot.DrawMode(0x20);
        return;
    }
    bool ghosts = false;
    int bright, ghostBright = 0, ghostSpread = 0;
    if (anim < steps) {
        bright = (int(p.brightness) * anim) / steps;
        ghostSpread = (int(p.spread) * (steps - anim)) / steps;
        ghostBright = (int(p.brightness) * (steps - anim)) / steps;
        ghosts = true;
    } else {
        bright = p.brightness;
        if (p.flags & 1) {
            int k = anim - steps;
            if (steps < anim - steps) k = steps;
            bright = bright + ((int(p.target) - bright) * (steps - k)) / steps;
        }
    }
    if ((p.flags & 4) == 0) ghosts = false;
    const uint16_t mode = uint16_t((p.flags & 0x18) << 2);
    if (scale != 0x80) { // 0x80011D58 (semi, the page word | mode) / 0x80011C2C, no E1
        const PanelSprite& s = p.sprite;
        auto grey = [](int v) { return uint32_t(v) | uint32_t(v) << 8 | uint32_t(v) << 16; };
        if (ghosts) {
            AddScaledSprite(ot, x - ghostSpread, y, s.u, s.v, s.clut, s.w, s.h, uint16_t(s.tpage | 0x20), grey(ghostBright) | 0x2000000u, scale);
            AddScaledSprite(ot, x + ghostSpread, y, s.u, s.v, s.clut, s.w, s.h, uint16_t(s.tpage | 0x20), grey(ghostBright) | 0x2000000u, scale);
        }
        if (p.flags & 2) AddScaledSprite(ot, x, y, s.u, s.v, s.clut, s.w, s.h, uint16_t(s.tpage | mode), grey(bright) | 0x2000000u, scale);
        else AddScaledSprite(ot, x, y, s.u, s.v, s.clut, s.w, s.h, s.tpage, grey(bright), scale);
        return;
    }
    const int sx = x - halfW, sy = y - (int16_t(p.sprite.h) >> 1);
    if (ghosts) {
        const uint32_t g = uint32_t(ghostBright) | uint32_t(ghostBright) << 8 | uint32_t(ghostBright) << 16 | 0x2000000u;
        ot.Add(Sprite(sx - ghostSpread, sy, p.sprite, g));
        ot.Add(Sprite(sx + ghostSpread, sy, p.sprite, g));
        ot.DrawMode(uint16_t(p.sprite.tpage | mode));
    }
    uint32_t c = uint32_t(bright) | uint32_t(bright) << 8 | uint32_t(bright) << 16;
    if (p.flags & 2) c |= 0x2000000u;
    ot.Add(Sprite(sx, sy, p.sprite, c));
    ot.DrawMode(uint16_t(p.sprite.tpage | mode));
}

namespace {

// 0x8007DFF0: POLY_F3 0x22 (a PolyF4 with the fourth vertex on the third).
MenuPrim Triangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    return Quad(MenuPrim::kPolyF4, {x0, x1, x2, x2}, {y0, y1, y2, y2}, {c, c, c, c}, true, false);
}

void OffsetPrims(std::vector<MenuPrim>& prims, size_t from, int dy) {
    if (dy == 0) return;
    for (size_t i = from; i < prims.size(); i++)
        for (int k = 0; k < 4; k++) prims[i].y[k] = int16_t(prims[i].y[k] + dy);
}

} // namespace

// ---------------------------------------------------------------- assets

ArcadeMenuAssets ArcadeMenuAssets::Load(const DiscImage& disc, const GtfsVolume& vol, uint8_t language) {
    ArcadeMenuAssets a;
    a.data = ArcadeMenuData::Load(disc, language);
    if (language != 1) throw std::runtime_error("arcade menus: only the US files are mapped");
    a.vram.UploadTimToPage(vol.Read("arcade/arc_panels_us.tim"), 6);
    a.vram.UploadTimToPage(vol.Read("arcade/arc_maker.tim"), 0x0B);
    a.vram.UploadTimToPage(vol.Read("arcade/arc_font.tim"), 0x1E);
    a.vram.UploadTimToPage(vol.Read("arcade/arc_other.tim"), 0x0F);
    a.vram.UploadTimToPage(vol.Read("arcade/arc_goodies_us.tim"), 0x1C);
    TitleAssets fonts;
    TitleAssets::LoadTitleFonts(fonts, vol);
    a.fonts = fonts.fonts;
    a.exe = UiLayout(LoadExeImage(disc), true);
    {
        const std::vector<uint8_t> global = InflateEmbeddedGzipNamed(LoadOverlayImage(disc, 1), "data-global.txd");
        const size_t at = size_t(language) * kGlobalTextStride;
        if (global.size() < at + kGlobalTextStride) throw std::runtime_error("arcade menus: data-global.txd has no block for the language");
        a.globalText.assign(global.begin() + std::ptrdiff_t(at), global.begin() + std::ptrdiff_t(at + kGlobalTextStride));
        if (ProfileOf(disc).build == ExeBuild::kArcadeEu) a.globalText = EuropeanArcadeText(global, true);
    }
    const auto fontExe = LoadExeImage(disc);
    a.exeFont = LoadHudFont(fontExe, UiAddress(fontExe, kExeSmallFont, true));
    a.carLogos = vol.Read("arcade/arc_carlogo");
    a.vol = &vol;
    a.courses = ParseCourseInfo(vol.Read(".crsinfo"));
    for (const std::vector<ArcadeCourse>& list : a.data.courses)
        for (const ArcadeCourse& c : list)
            if (!a.coursePictures.count(c.file)) a.coursePictures.emplace(c.file, LoadCoursePicture(vol, c.file));
    return a;
}

std::string ArcadeMenuAssets::AnyText(uint32_t address) const {
    if (std::string s = data.Text(address); !s.empty()) return s;
    std::string s;
    if (address >= kGlobalTextAt && address - kGlobalTextAt < globalText.size()) {
        for (size_t at = address - kGlobalTextAt; at < globalText.size() && globalText[at] != 0; at++) s.push_back(char(globalText[at]));
        return s;
    }
    for (uint32_t at = address; data.ovl2.Contains(at, 1) && data.ovl2.Get<uint8_t>(at) != 0; at++) s.push_back(char(data.ovl2.Get<uint8_t>(at)));
    return s;
}

const HudFont& ArcadeMenuAssets::FontAt(uint32_t descriptor) const {
    switch (descriptor) {
    case 0x801294F0u: return fonts[0];
    case 0x801294E0u: return fonts[1];
    case 0x801234D0u: return fonts[2];
    case 0x80129500u: return fonts[3];
    case kExeSmallFont: return exeFont;
    default: throw std::logic_error("arcade menus: font descriptor " + std::to_string(descriptor) + " is not mapped");
    }
}

// ---------------------------------------------------------------- panel list

PanelList PanelList::Read(const GuestImage& o, uint32_t a, uint32_t itemsAddress, int countOverride) {
    PanelList l;
    l.count = countOverride >= 0 ? int8_t(countOverride) : o.Get<int8_t>(a);
    l.flags = o.Get<uint8_t>(a + 1);
    l.visible = o.Get<int8_t>(a + 2);
    l.width = o.Get<int16_t>(a + 4);
    l.rowHeight = o.Get<int16_t>(a + 6);
    l.gap = o.Get<int16_t>(a + 8);
    l.x = o.Get<int16_t>(a + 0x10);
    l.y = o.Get<int16_t>(a + 0x12);
    const uint32_t items = itemsAddress ? itemsAddress : o.Get<uint32_t>(a + 0x0C);
    for (int i = 0; i < l.count; i++) {
        const uint32_t it = items + uint32_t(i) * 0x14;
        PanelItem p;
        p.kind = o.Get<uint8_t>(it);
        p.height = o.Get<int16_t>(it + 2);
        const uint32_t tmpl = o.Get<uint32_t>(it + 8), spr = o.Get<uint32_t>(it + 0x0C);
        p.result = o.Get<int32_t>(it + 0x10);
        if ((p.kind & 1) != 1) throw std::logic_error("arcade menus: text rows (0x80019FC8) in a panel list are not ported");
        if (tmpl == 0) { // 0x8001C0FC defaults
            p.pulse = 2, p.steps = 0x1E, p.spread = 0x20, p.brightness = 0x80, p.target = 0x40, p.scale = 0x80, p.flags = 0;
        } else {
            p.flags = o.Get<uint8_t>(tmpl);
            p.pulse = o.Get<int16_t>(tmpl + 2);
            p.steps = o.Get<int16_t>(tmpl + 4);
            p.brightness = o.Get<uint8_t>(tmpl + 6);
            p.target = o.Get<uint8_t>(tmpl + 7);
            p.spread = o.Get<int16_t>(tmpl + 8);
            p.scale = o.Get<int16_t>(tmpl + 10);
        }
        p.sprite.u = o.Get<uint8_t>(spr);
        p.sprite.v = o.Get<uint8_t>(spr + 1);
        p.sprite.clut = o.Get<uint16_t>(spr + 2);
        p.sprite.w = o.Get<uint16_t>(spr + 4);
        p.sprite.h = o.Get<uint16_t>(spr + 6);
        p.sprite.tpage = o.Get<uint16_t>(spr + 8);
        l.items.push_back(p);
    }
    return l;
}

void PanelList::Init() { // 0x8001B6C0 (+ 0x8001B360 per row: the panel object from its template, anim -1)
    state = -1;
    selected = 0;
    highlight = 0;
    scroll = 0;
    revealed = 0;
    revealCountdown = 6;
    for (PanelItem& p : items) {
        p.anim = -1;
        p.flags &= uint8_t(0xFE);
    }
}

void PanelList::Open() { // 0x8001B744
    int n = int(selected) - int(visible);
    state = 0;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) items[size_t(i)].anim = 0; // 0x8001C194
    revealCountdown = 6;
    revealed = int8_t(n);
    highlight = 0x1E;
}

void PanelList::Close() { // 0x8001B7D4
    state = -0x41;
    for (int i = 0; i < count; i++)
        if (i != selected) items[size_t(i)].anim = int16_t(~items[size_t(i)].steps); // 0x8001C1A8
}

int32_t PanelList::Update(const MenuListPad* pad) { // 0x8001B84C
    int32_t out = -2;
    for (PanelItem& p : items) PanelItemTick(p);
    if (state < 0) {
        if (state != -1) {
            state++;
            if (state == -0x3A) items[size_t(selected)].anim = int16_t(~items[size_t(selected)].steps);
        }
        return -2;
    }
    if (scroll < 0) scroll++;
    if (scroll > 0) scroll--;
    if (++highlight > 0x3C) highlight = 0;
    if (++state > 0x2D) state = 0;
    if (pad) {
        uint32_t bits = pad->pressed;
        if (bits & mp::kBack) out = -1;
        else if (bits & mp::kChoose) {
            const PanelItem& it = items[size_t(selected)];
            out = int8_t(it.kind) < 0 ? -4 : it.result;
        } else {
            bits |= pad->repeat;
            int sel = selected;
            if (bits & mp::kUp) {
                const int next = sel - 1;
                items[size_t(sel)].flags &= uint8_t(0xFE); // 0x8001B664
                scroll = -8;
                sel = next;
                if (sel < 0) {
                    sel = 0;
                    if (flags & 4) sel = count - 1;
                }
                out = -3;
                if (items[size_t(sel)].kind == 1) { // 0x8001B5EC: kind 1 exactly
                    items[size_t(sel)].flags |= 1;
                    items[size_t(sel)].anim = items[size_t(sel)].steps;
                }
                highlight = 0;
            }
            if (bits & mp::kDown) {
                const int next = sel + 1;
                items[size_t(sel)].flags &= uint8_t(0xFE);
                scroll = 8;
                sel = next;
                if (sel >= count) {
                    sel = count - 1;
                    if (flags & 4) sel = 0;
                }
                out = -3;
                if (items[size_t(sel)].kind == 1) {
                    items[size_t(sel)].flags |= 1;
                    items[size_t(sel)].anim = items[size_t(sel)].steps;
                }
                highlight = 0;
            }
            selected = int8_t(sel);
        }
    }
    if (revealed < count && --revealCountdown == 0) {
        items[size_t(revealed)].anim = 0;
        revealCountdown = 6;
        revealed++;
    }
    return out;
}

void PanelList::Draw(MenuOtSlot& ot) const { // 0x8001BB6C
    const int pitch = gap + rowHeight;
    int shift = scroll * pitch;
    if (shift < 0) shift += 7;
    int rowY = y + (shift >> 3);
    int fadeOut = scroll * 0x80;
    if (fadeOut < 0) fadeOut += 7;
    fadeOut >>= 3;
    if (fadeOut < 0) fadeOut = -fadeOut;
    const int fadeIn = 0x80 - fadeOut;
    const int vis = visible, half = int(int8_t(visible)) >> 1, rest = vis - half;
    const int sel = selected, n = count;
    int first = sel - half;
    if (n <= first + vis) first = n - vis;
    if (first < 0) first = 0;
    int rowIn = -1, rowOut = -1, rows = vis;
    if (scroll < 0) {
        rowOut = sel + rest;
        rowIn = sel - half;
        if (n - vis <= sel - half) rowIn = -1;
        if (rowOut < vis) rowOut = -1;
        rows = vis + 1;
        if (sel == n - 1) {
            rowIn = n - vis;
            if (n <= vis) {
                rowIn = -1;
                rowY = y;
                rows = vis;
            }
        } else if (sel < half || n - rest <= sel) {
            rowY = y;
            rows = vis;
        }
    }
    int start = first;
    if (scroll > 0) {
        rowIn = sel + rest - 1;
        rowOut = sel - half - 1;
        if (rowIn < vis) rowIn = -1;
        if (n - vis <= rowOut) rowOut = -1;
        rowY -= pitch;
        if (sel == 0) {
            rowIn = vis - 1;
            start = -1;
            rows = vis + 1;
            if (n <= vis) {
                rowY = y;
                rowIn = -1;
                start = 0;
                rows = vis;
            }
        } else {
            start = first - 1;
            rows = rows + 1;
            if (sel <= half || n - rest < sel) {
                rowY = y;
                start = first;
                rows = vis;
            }
        }
    }
    const int end = rows + start;
    for (int r = start; r < end; r++) {
        if (r < 0 || r >= n) continue;
        PanelItem it = items[size_t(r)];
        int h = it.height;
        if (h < 1) h = rowHeight;
        if (r == sel && state >= 0 && (it.kind & 1) == 1) {
            const bool enabled = int8_t(it.kind) >= 0;
            Highlight(ot, x, rowY, width, h, enabled ? 0x1E1E1Eu : 0x07071Eu, enabled ? 0x5E5E5Eu : 0x17175Eu, (highlight * 0x80) / 0x3C);
        }
        int a = int8_t(it.kind) < 0 ? 0x20 : 0x80;
        if (r == rowIn) a = (a * fadeIn) >> 7;
        if (r == rowOut) a = (a * fadeOut) >> 7;
        it.brightness = uint8_t(a);              // 0x8001B5B0
        it.x = x;                                // 0x8001B564
        it.y = int16_t(rowY + (h >> 1));
        PanelItemDraw(it, ot);
        rowY += gap + h;
    }
    if (state >= 0) {
        int k = 0x28 - state;
        if (k > 10) k = 10;
        if (k < 0) k = 0;
        const uint32_t v = uint32_t((k * 0xFF) / 10);
        const uint32_t c = v | (v >> 1) << 8;
        if (start > 0) {
            const int yy = y - gap;
            ot.Add(Triangle(x, yy - 10, x + 6, yy, x - 6, yy, c));
        }
        if (end < n) {
            const int yy = y + pitch * vis;
            ot.Add(Triangle(x, yy + 10, x - 6, yy, x + 6, yy, c));
        }
        ot.DrawMode(0x20);
    }
}

// ---------------------------------------------------------------- header

void AddArcadeHeader(MenuOtSlot& textSlot, MenuOtSlot& gradientSlot, const HudFont& font, const std::string& title, uint32_t colour, int alpha) {
    const int spacing = 0x22 - (alpha >> 2);
    const int width = font.TextWidth(title, spacing);
    const int x = (0x160 - width) >> 1;
    MenuPrim line;
    line.kind = MenuPrim::kTile;
    line.x[0] = int16_t(x + 1), line.y[0] = 0x4E, line.w = int16_t(width), line.h = 2;
    line.colour[0] = uint32_t((alpha * 0xF5) >> 7) | uint32_t((alpha * 0x5C) >> 7) << 8 | uint32_t((alpha * 0x19) >> 7) << 16;
    textSlot.Add(line);
    const uint32_t c = uint32_t((alpha * 0xF5) >> 8) | uint32_t((alpha * 0x5C) >> 8) << 8 | uint32_t((alpha * 0x19) >> 8) << 16;
    shell::AddText(textSlot, font, title, x, 0x4C, spacing, c, 0);
    shell::AddText(textSlot, font, title, x + 3, 0x4F, spacing, 0, 0);
    const uint32_t g = uint32_t(((colour & 0xFF) * uint32_t(alpha)) >> 7) | uint32_t((((colour >> 8) & 0xFF) * uint32_t(alpha)) >> 7) << 8 |
                       uint32_t((((colour >> 16) & 0xFF) * uint32_t(alpha)) >> 7) << 16;
    const int half = (alpha * 0x2D) >> 7;
    gradientSlot.Add(Quad(MenuPrim::kPolyG4, {0, 0x160, 0, 0x160}, {0x2D - half, 0x2D - half, 0x2D + half, 0x2D + half}, {g, g, 0, 0}, true, true));
    gradientSlot.DrawMode(0x220);
}

// ---------------------------------------------------------------- menus

ArcadeMenus::ArcadeMenus(const ArcadeMenuAssets& assets, const ArcadeData& data, const CarInfoDirectory& cars, std::span<const uint8_t> career)
    : a_(assets), data_(data), cars_(cars), career_(career.begin(), career.end()) {
    if (career_.size() < 0x7C9C) throw std::invalid_argument("arcade menus: the career block is shorter than 0x7C9C bytes");
    const GuestImage& o = a_.data.ovl2;
    mode_ = PanelList::Read(o, kModeList);
    game_ = PanelList::Read(o, kGameList);
    level_ = PanelList::Read(o, kLevelList);
    class_ = PanelList::Read(o, kClassList);
    rally_ = PanelList::Read(o, kRallyList);
    carColour_ = o.Get<uint32_t>(kViewCar + 0x0C);
    vram_ = a_.vram;
    guestGarage_.assign(kGarageStride, 0); // RAM 0x801D0FDC: empty until LOAD GUEST GARAGE
    carPage_ = std::make_unique<ArcadeCarPage>(a_, data_, cars_);
    battlePage_ = std::make_unique<ArcadeBattlePage>(a_, data_, cars_);
    game2p_ = PanelList::Read(o, kGame2PList);
    coursePage_ = std::make_unique<ArcadeCoursePage>(a_, cars_);
    bonusPage_ = std::make_unique<ArcadeBonusPage>(a_);
    creditsPage_ = std::make_unique<ArcadeCreditsPage>(a_);
    guestLoad_ = std::make_unique<ArcadeGuestLoad>(a_, std::array<shell::CardSlot, 2>{});
    Reset();
}

void ArcadeMenus::Reset(bool afterRace) {
    unlockRevision_ = pc::unlockRevision;
    // 0x8001D54C (first entry): availability, list kind 5; the selection block keeps what the last build left.
    unlocks_ = ComputeClassUnlocks(a_.data, career_);
    for (size_t l = 0; l < ArcadeMenuData::kCourseListCount; l++) courseFlags_[l] = CourseAvailability(a_.data.courses[l], l == 1 || l == 3, career_);
    listKind_ = 5;
    stack_.assign(1, ArcadeView::kRoot);
    transition_ = 0;
    endingRow_ = -1;
    endingTimer_ = 0;
    viewTimer_[int(ArcadeView::kRoot)] = 1; // the overlay's entry runs the root view's first init (0x80011954) also after a race
    viewChoice_[int(ArcadeView::kRoot)] = 0;
    if (!afterRace) std::fill(region_.begin(), region_.end(), uint8_t(0));
    sounds.clear();
}

std::string ArcadeMenus::TitleOf(ArcadeView v) const {
    uint32_t view = 0;
    switch (v) {
    case ArcadeView::kMode: view = kViewMode; break;
    case ArcadeView::kGame: view = kViewGame; break;
    case ArcadeView::kLevel: view = kViewLevel; break;
    case ArcadeView::kClass: view = kViewClass; break;
    case ArcadeView::kCar: view = kViewCar; break;
    case ArcadeView::kCourse: view = kViewCourse; break;
    case ArcadeView::kRallyClass: view = kViewRallyClass; break;
    case ArcadeView::kBonus: view = kViewBonus; break;
    case ArcadeView::kGuestLoad: view = kViewGuestLoad; break;
    case ArcadeView::kGame2P: view = kViewGame2P; break;
    case ArcadeView::kBattle: view = kViewBattle; break;
    case ArcadeView::kGarage: { // 0x8001E094 / 0x8001E330: the view's title word = 0x8004FC08[garage]
        int16_t g = 0;
        std::memcpy(&g, Sel() + Sel::kGarage, 2);
        return a_.data.Text(a_.data.ovl2.Get<uint32_t>(kGarageTitles + uint32_t(std::clamp<int>(g, 0, 1)) * 4));
    }
    default: return {};
    }
    return a_.data.Text(a_.data.ovl2.Get<uint32_t>(view + 0x10));
}

uint32_t ArcadeMenus::ColourOf(ArcadeView v) const {
    switch (v) {
    case ArcadeView::kMode: return a_.data.ovl2.Get<uint32_t>(kViewMode + 0x0C);
    case ArcadeView::kGame: return a_.data.ovl2.Get<uint32_t>(kViewGame + 0x0C);
    case ArcadeView::kLevel: return a_.data.ovl2.Get<uint32_t>(kViewLevel + 0x0C);
    case ArcadeView::kClass: return a_.data.ovl2.Get<uint32_t>(kViewClass + 0x0C);
    case ArcadeView::kCar: return carColour_;
    case ArcadeView::kRallyClass: return a_.data.ovl2.Get<uint32_t>(kViewRallyClass + 0x0C);
    case ArcadeView::kCourse: return a_.data.ovl2.Get<uint32_t>(kViewCourse + 0x0C);
    case ArcadeView::kGarage: return a_.data.ovl2.Get<uint32_t>(kViewGarage + 0x0C);
    case ArcadeView::kBonus: return a_.data.ovl2.Get<uint32_t>(kViewBonus + 0x0C);
    case ArcadeView::kGuestLoad: return a_.data.ovl2.Get<uint32_t>(kViewGuestLoad + 0x0C);
    case ArcadeView::kGame2P: return a_.data.ovl2.Get<uint32_t>(kViewGame2P + 0x0C);
    case ArcadeView::kBattle: return a_.data.ovl2.Get<uint32_t>(kViewBattle + 0x0C);
    default: return 0;
    }
}

PanelList* ArcadeMenus::ListOf(ArcadeView v) {
    switch (v) {
    case ArcadeView::kMode: return &mode_;
    case ArcadeView::kGame: return &game_;
    case ArcadeView::kLevel: return &level_;
    case ArcadeView::kClass: return &class_;
    case ArcadeView::kRallyClass: return &rally_;
    case ArcadeView::kGame2P: return &game2p_;
    default: return nullptr;
    }
}
const PanelList* ArcadeMenus::ListOf(ArcadeView v) const { return const_cast<ArcadeMenus*>(this)->ListOf(v); }

const std::vector<std::string>& ArcadeMenus::ClassCars() const {
    const int cls = int8_t(Sel()[Sel::kClass]);
    return a_.data.classes.at(size_t(std::clamp(cls, 0, 6))).cars;
}

void ArcadeMenus::Push(ArcadeView v) { // 0x800157A4 + 0x8001419C case 1
    old_ = stack_.back();
    stack_.push_back(v);
    InitView(v, false);
    transition_ = kTransitionFields;
    popping_ = false;
}

void ArcadeMenus::Pop() { // 0x800157D8 + case 2
    old_ = stack_.back();
    stack_.pop_back();
    InitView(stack_.back(), true);
    transition_ = kTransitionFields;
    popping_ = true;
}

void ArcadeMenus::InitView(ArcadeView v, bool reenter) {
    const int vi = int(v);
    uint8_t* sel = Sel();
    switch (v) {
    case ArcadeView::kRoot: // 0x8001D54C (re-entered: the title follows after 24 fields)
        viewTimer_[vi] = reenter ? kViewDelay : 1;
        viewChoice_[vi] = reenter ? 1 : 0;
        return;
    case ArcadeView::kMode: { // 0x8001D640
        viewTimer_[vi] = kViewDelay;
        mode_.Init();
        mode_.selected = int8_t(reenter ? viewChoice_[vi] : (cursorMode_ > 3 ? 0 : cursorMode_));
        return;
    }
    case ArcadeView::kGame: // 0x8001D868
        viewTimer_[vi] = kViewDelay;
        game_.Init();
        game_.selected = int8_t(reenter ? viewChoice_[vi] : (cursorGame_ > 2 ? 0 : cursorGame_));
        return;
    case ArcadeView::kLevel: { // 0x8001DD28: 3 levels (4 when 0x80023574 opens the fourth: never in this build)
        viewTimer_[vi] = kViewDelay;
        int8_t c = cursorLevel_ > 3 ? 0 : cursorLevel_;
        if (c > 2) c = 0;
        level_.count = 3, level_.visible = 3, level_.gap = 0x14, level_.y = 0x96;
        level_.Init();
        level_.selected = reenter ? int8_t(sel[Sel::kLevel]) : c;
        return;
    }
    case ArcadeView::kClass: { // 0x8001DF88
        viewTimer_[vi] = kViewDelay;
        int c = cursorClass_ > 4 ? 0 : cursorClass_;
        const bool withS = unlocks_.classSOpen;
        class_ = PanelList::Read(a_.data.ovl2, kClassList, withS ? kClassItemsWithS : kClassItemsNoS, withS ? 6 : 5);
        if (!withS) {
            c--;
            if (c < 0) c = 0;
        }
        if (c >= class_.count) c = class_.count - 1;
        // The garage rows are disabled while the garage blocks are empty (0x801CCFB4 / 0x801D0FDC: career + 0x3C74 + g * 0x4028).
        for (PanelItem& p : class_.items)
            if (p.result >= 4 && GarageCount(GarageBlock(p.result - 4)) == 0) p.kind |= 0x80;
        class_.Init();
        class_.selected = int8_t(c);
        return;
    }
    case ArcadeView::kRallyClass: { // 0x8001E284: the cursor 0x801D5008 (> 2 -> 0); the garage rows disabled while empty
        viewTimer_[vi] = kViewDelay;
        rally_ = PanelList::Read(a_.data.ovl2, kRallyList);
        for (PanelItem& p : rally_.items)
            if ((p.result == 4 || p.result == 5) && GarageCount(GarageBlock(p.result - 4)) == 0) p.kind |= 0x80;
        rally_.Init();
        rally_.selected = reenter ? int8_t(viewChoice_[vi]) : int8_t(cursorRally_ > 2 ? 0 : cursorRally_);
        return;
    }
    case ArcadeView::kGarage: { // 0x8001F8CC (game/arcade/arcade_car_page.h)
        viewTimer_[vi] = kViewDelay;
        int16_t g = 0;
        std::memcpy(&g, sel + Sel::kGarage, 2);
        const uint32_t word2cc = uint32_t(sel[Sel::kWord2CC]) | uint32_t(sel[Sel::kWord2CC + 1]) << 8;
        carPage_->EnterGarage(g, reenter, GarageBlock(g), word2cc == 1, career_[0], vram_);
        return;
    }
    case ArcadeView::kCar: { // 0x8001E890 (game/arcade/arcade_car_page.h)
        viewTimer_[vi] = kViewDelay;
        const int cls = int8_t(sel[Sel::kClass]);
        carPage_->Enter(cls, ClassFlags(cls), reenter, career_[0], vram_, ctx_);
        return;
    }
    case ArcadeView::kCourse: { // 0x80022C34 (game/arcade/arcade_course_page.h)
        viewTimer_[vi] = kViewDelay;
        coursePage_->Enter(listKind_, courseFlags_, career_, vram_);
        return;
    }
    case ArcadeView::kFinal: // 0x80023430
        viewTimer_[vi] = kViewDelay;
        return;
    case ArcadeView::kNotice:
        viewTimer_[vi] = 0;
        return;
    case ArcadeView::kBonus: // 0x80023CA8 (game/arcade/arcade_bonus.h)
        bonusPage_->Enter(career_);
        return;
    case ArcadeView::kCredits: // 0x800240BC
        creditsPage_->Enter(career_);
        return;
    case ArcadeView::kGuestLoad: // 0x80023478
        guestLoad_->Enter();
        return;
    case ArcadeView::kGame2P: // 0x8001DB10: the cursor 0x801D5009 (> 1 -> 0)
        viewTimer_[vi] = kViewDelay;
        game2p_.Init();
        game2p_.selected = cursor2P_ > 1 ? int8_t(0) : cursor2P_;
        return;
    case ArcadeView::kBattle: { // 0x80020700 (game/arcade/arcade_battle.h)
        ArcadeBattlePage::Context c;
        c.listKind = listKind_;
        c.classSOpen = unlocks_.classSOpen;
        for (int k = 0; k < 7; k++) c.classFlags[size_t(k)] = ClassFlags(k);
        c.garages = {GarageBlock(0), GarageBlock(1)};
        uint32_t word2cc = 0;
        std::memcpy(&word2cc, sel + Sel::kWord2CC, 4);
        c.rally = word2cc == 1;
        c.language = career_[0];
        battlePage_->Enter(c, reenter, sel, vram_);
        return;
    }
    }
}

int ArcadeMenus::UpdateView(ArcadeView v, const MenuListPad* pad) {
    const int vi = int(v);
    uint8_t* sel = Sel();
    auto tickOpen = [&](PanelList& l) {
        if (viewTimer_[vi] > 0 && --viewTimer_[vi] == 0) l.Open();
    };
    auto listResult = [&](PanelList& l) -> int32_t {
        tickOpen(l);
        const int32_t r = l.Update(pad);
        if (r == -3) sounds.push_back(6);
        else if (r == -4) sounds.push_back(0);
        else if (r == -1) {
            sounds.push_back(4);
            l.Close();
        }
        return r;
    };
    switch (v) {
    case ArcadeView::kRoot: // 0x8001D5DC
        if (viewTimer_[vi] > 0 && --viewTimer_[vi] == 0) {
            if (viewChoice_[vi] == 1) return 4;
            Push(ArcadeView::kMode);
            return 1;
        }
        return 0;
    case ArcadeView::kMode: { // 0x8001D6C8
        const int32_t r = listResult(mode_);
        if (r == -1) return 2;
        if (r < 0) return 0;
        cursorMode_ = int8_t(r);
        const uint8_t laps = r == 1 ? career_[6] : career_[3];
        sel[Sel::kLaps] = laps, sel[Sel::kLaps + 1] = 0;
        sounds.push_back(3);
        mode_.Close();
        viewChoice_[vi] = r;
        if (r == 0) {
            Push(ArcadeView::kGame);
        } else if (r == 2) { // BONUS ITEMS 0x800525C8
            Push(ArcadeView::kBonus);
        } else if (r == 3) { // LOAD GUEST GARAGE 0x800523DC
            Push(ArcadeView::kGuestLoad);
        } else { // 2 player Battle: 2P GAME SELECTION 0x800520E8
            Push(ArcadeView::kGame2P);
        }
        return 1;
    }
    case ArcadeView::kGame: { // 0x8001D8F0
        const int32_t r = listResult(game_);
        if (r == -1) return 2;
        if (r < 0) return 0;
        sounds.push_back(3);
        game_.Close();
        viewChoice_[vi] = r;
        cursorGame_ = int8_t(r);
        sel[Sel::kMode] = uint8_t(a_.data.gameModes[size_t(r)]);
        const uint32_t word2cc = r == 1 ? 1u : 0u; // 0x800F0298: 1 for Rally -> selection + 0x2CC
        std::memcpy(sel + Sel::kWord2CC, &word2cc, 4);
        if (r == 0) {
            listKind_ = 0;
            Push(ArcadeView::kLevel);
        } else if (r == 1) { // Rally: the class view 0x800521E4, course list kind 2
            listKind_ = 2;
            Push(ArcadeView::kRallyClass);
        } else { // Time Trial: CLASS SELECTION 0x80052190, course list kind 1
            listKind_ = 1;
            Push(ArcadeView::kClass);
        }
        return 1;
    }
    case ArcadeView::kLevel: { // 0x8001DE14
        const int32_t r = listResult(level_);
        if (r == -1) return 2;
        if (r < 0) return 0;
        sounds.push_back(3);
        sel[Sel::kLevel] = uint8_t(r);
        cursorLevel_ = int8_t(r);
        level_.Close();
        Push(ArcadeView::kClass);
        return 1;
    }
    case ArcadeView::kClass: { // 0x8001E094
        const int32_t r = listResult(class_);
        if (r == -1) return 2;
        if (r < 0) return 0;
        cursorClass_ = int8_t(r);
        sounds.push_back(3);
        class_.Close();
        sel[Sel::kClass] = uint8_t(r);
        if (r >= 4) { // the garages: the view 0x8005228C with the title 0x8004FC08[garage]
            const int16_t g = int16_t(r - 4);
            std::memcpy(sel + Sel::kGarage, &g, 2);
            Push(ArcadeView::kGarage);
            return 1;
        }
        const int16_t none = -1;
        std::memcpy(sel + Sel::kGarage, &none, 2);
        carColour_ = a_.data.ovl2.Get<uint32_t>(kClassColours + uint32_t(r) * 4); // 0x80052244 = 0x8004FAC0[class]
        Push(ArcadeView::kCar);
        return 1;
    }
    case ArcadeView::kRallyClass: { // 0x8001E330
        const int32_t r = listResult(rally_);
        if (r == -1) return 2;
        if (r < 0) return 0;
        sounds.push_back(3);
        rally_.Close();
        viewChoice_[vi] = rally_.selected;
        if (r == 4 || r == 5) { // the garages (the view 0x8005228C)
            cursorRally_ = int8_t(r - 3);
            const int16_t g = int16_t(r - 4);
            std::memcpy(sel + Sel::kGarage, &g, 2);
            sel[Sel::kClass] = uint8_t(r);
            Push(ArcadeView::kGarage);
            return 1;
        }
        cursorRally_ = int8_t(r - 6);
        sel[Sel::kClass] = 6; // the rally cars and the bonus cars of class list 6
        const int16_t none = -1;
        std::memcpy(sel + Sel::kGarage, &none, 2);
        carColour_ = kRallyCarColour;
        Push(ArcadeView::kCar);
        return 1;
    }
    case ArcadeView::kGarage: { // 0x8001FA8C (game/arcade/arcade_car_page.h)
        const ArcadeCarPage::Result r = carPage_->UpdateGarage(pad, sel, sounds, vram_, ctx_);
        if (r == ArcadeCarPage::kBack) return 2;
        if (r == ArcadeCarPage::kChosen) {
            Push(ArcadeView::kCourse);
            return 1;
        }
        return 0;
    }
    case ArcadeView::kCar: { // 0x8001EAA8 (game/arcade/arcade_car_page.h)
        const ArcadeCarPage::Result r = carPage_->Update(pad, sel, sounds, vram_, ctx_);
        if (r == ArcadeCarPage::kBack) return 2;
        if (r == ArcadeCarPage::kChosen) {
            Push(ArcadeView::kCourse);
            return 1;
        }
        return 0;
    }
    case ArcadeView::kCourse: { // 0x80022D98 (game/arcade/arcade_course_page.h)
        const ArcadeCoursePage::Result r = coursePage_->Update(pad, sel, sounds, vram_);
        if (r == ArcadeCoursePage::kBack) return 2;
        if (r == ArcadeCoursePage::kChosen) {
            Push(ArcadeView::kFinal);
            return 1;
        }
        return 0;
    }
    case ArcadeView::kFinal: // 0x80023440
        if (viewTimer_[vi] > 0 && --viewTimer_[vi] == 0) return 3;
        return 0;
    case ArcadeView::kBonus: { // 0x80023D58
        const ArcadeBonusPage::Result r = bonusPage_->Update(pad, sounds);
        if (r == ArcadeBonusPage::kBack) return 2;
        if (r == ArcadeBonusPage::kNext) {
            Push(ArcadeView::kCredits);
            return 1;
        }
        return 0;
    }
    case ArcadeView::kCredits: { // 0x8002416C
        const int r = creditsPage_->Update(pad, sounds);
        if (r == -2) return 2;
        if (r < 0) return 0;
        // View 0x80052670 (init 0x800243E4: 24 fields; update 0x800243F4: then 5 / 6 by 0x800F36AE = row != 0; draws nothing):
        // the top level plays the row's ending movie (member 5 0x800114E0) and then goes to the title.
        endingRow_ = r;
        endingTimer_ = 24;
        return 0;
    }
    case ArcadeView::kGuestLoad: { // 0x800234C8
        const int r = guestLoad_->Update(pad, sounds);
        if (r == 2) return 2;
        if (r == 3) SetGuestGarage(guestLoad_->Garage()); // + 0x80019F44: the guest summary (the class views read it)
        return 0;
    }
    case ArcadeView::kGame2P: { // 0x8001DB74
        const int32_t r = listResult(game2p_);
        if (r == -1) return 2;
        if (r < 0) return 0;
        sounds.push_back(3);
        game2p_.Close();
        cursor2P_ = int8_t(r);
        listKind_ = r == 1 ? 4 : 3; // 0x800F364C: the 2P course lists (dirt for Rally)
        const uint32_t word2cc = r == 1 ? 1u : 0u; // 0x800F0298 -> selection + 0x2CC
        std::memcpy(sel + Sel::kWord2CC, &word2cc, 4);
        sel[Sel::kMode] = 0; // selection + 2: game mode 0
        Push(ArcadeView::kBattle);
        return 1;
    }
    case ArcadeView::kBattle: { // 0x8002229C (game/arcade/arcade_battle.h)
        const std::array<const MenuListPad*, 2> pads = {pad, pad ? pad2_ : nullptr};
        const ArcadeBattlePage::Result r = battlePage_->Update(pads, sel, sounds, vram_, ctx_);
        if (r == ArcadeBattlePage::kBack) return 2;
        if (r == ArcadeBattlePage::kChosen) {
            Push(ArcadeView::kCourse);
            return 1;
        }
        return 0;
    }
    case ArcadeView::kNotice:
        if (pad && (pad->pressed & (mp::kBack | mp::kChoose))) {
            sounds.push_back(4);
            return 2;
        }
        return 0;
    }
    return 0;
}

ArcadeMenus::Outcome ArcadeMenus::Update(const MenuListPad& pad, const MenuListPad* pad2) { // 0x8001419C
    if (unlockRevision_ != pc::unlockRevision) Reset();
    sounds.clear();
    static const MenuListPad kNoInput{};
    pad2_ = pad2 ? pad2 : &kNoInput;
    if (endingTimer_ > 0) { // view 0x80052670 (the credits page stays drawn: ours; the original's empty view shows nothing)
        if (--endingTimer_ == 0) return kEnding;
        return kStay;
    }
    if (transition_ > 0 && --transition_ > 0) UpdateView(old_, nullptr);
    const ArcadeView v = stack_.back();
    const int r = UpdateView(v, transition_ > 0 ? nullptr : &pad);
    carPage_->ModelTick(); // 0x80016624 after the views
    battlePage_->ModelTick();
    if (r == 2) {
        if (stack_.size() <= 1) return kTitle;
        Pop();
    } else if (r == 3) {
        return kRace;
    } else if (r == 4) {
        return kTitle;
    }
    return kStay;
}

bool ArcadeMenus::ApplyRaceResult(std::span<const uint8_t> raceBlock, int32_t place) { return ApplyArcadeRaceResult(career_, raceBlock, place); }

void ArcadeMenus::SetCareer(std::span<const uint8_t> career) {
    if (career.size() < 0x7C9C) throw std::invalid_argument("arcade menus: the career block is shorter than 0x7C9C bytes");
    career_.assign(career.begin(), career.end());
}

void ArcadeMenus::RaceNotAvailable(const std::string& title) {
    old_ = stack_.back();
    stack_.back() = ArcadeView::kNotice;
    notice_ = title;
    InitView(ArcadeView::kNotice, false);
    transition_ = kTransitionFields;
    popping_ = false;
}

bool ArcadeMenus::CarShown() const {
    auto carView = [](ArcadeView v) { return v == ArcadeView::kCar || v == ArcadeView::kGarage; };
    const bool onView = carView(Current()) || (transition_ > 0 && carView(old_));
    return onView && carPage_->CarShown();
}
uint32_t ArcadeMenus::CarId() const { return carPage_->CarId(); }

const ArcadeBattlePage* ArcadeMenus::BattleShown() const {
    const bool onView = Current() == ArcadeView::kBattle || (transition_ > 0 && old_ == ArcadeView::kBattle);
    return onView ? battlePage_.get() : nullptr;
}

std::span<const uint8_t> ArcadeMenus::GarageBlock(int g) const {
    if (g == 0) return std::span<const uint8_t>(career_).subspan(kGarageBlock, kGarageStride);
    return guestGarage_;
}

ArcadeRaceSetup ArcadeMenus::BuildRace(const ArcadeSetupData& d, uint32_t p1, uint32_t p2, uint32_t vsync) {
    std::array<std::vector<uint8_t>, 2> blocks;
    for (int g = 0; g < 2; g++) blocks[size_t(g)].assign(GarageBlock(g).begin(), GarageBlock(g).end());
    ArcadeRaceSetup setup = BuildArcadeRace(d, region_, career_, p1, p2, vsync, GarageBlocks{blocks[0], blocks[1]});
    uint32_t garageFlag = 0;
    std::memcpy(&garageFlag, setup.menuRegion.data() + (kSelectionAddress - kMenuRegionAddress) + Sel::kGarageFlag, 4);
    if (garageFlag == 0) return setup;
    if (!gtData_) {
        if (!a_.vol) throw std::logic_error("arcade menus: no volume for the GT-mode tables");
        gtData_ = std::make_unique<career::CareerData>(career::CareerData{CarParamTables::Load(*a_.vol), CarInfoDirectory::Load(*a_.vol),
                                                                          GtModeRaceData::Load(*a_.vol), {}, GuestImage{}, GuestImage{}});
    }
    RebuildGarageEntries(setup, {std::span<uint8_t>(blocks[0]), std::span<uint8_t>(blocks[1])}, *gtData_); // ovl3 0x8001290C
    std::copy(blocks[0].begin(), blocks[0].end(), career_.begin() + std::ptrdiff_t(kGarageBlock));
    guestGarage_ = blocks[1];
    return setup;
}

void ArcadeMenus::SetCards(const std::array<shell::CardSlot, 2>& slots) { guestLoad_ = std::make_unique<ArcadeGuestLoad>(a_, slots); }

void ArcadeMenus::SetGuestGarage(std::span<const uint8_t> block) {
    if (block.size() != kGarageStride) throw std::invalid_argument("arcade menus: a guest garage block is 0x4028 bytes");
    guestGarage_.assign(block.begin(), block.end());
}
void ArcadeMenus::SetCourseMovieSource(CourseMovieSource* source) { coursePage_->SetMovieSource(source); }
uint32_t ArcadeMenus::VramVersion() const { return carPage_->Uploads() + coursePage_->Uploads() + battlePage_->Uploads(); }
ArcadeMenus::~ArcadeMenus() = default;
int ArcadeMenus::CarPaint() const { return carPage_->Paint(); }

std::vector<uint8_t> ArcadeMenus::ClassFlags(int cls) const { // 0x80051F30[class] (lists 0 and 6 rewritten by 0x8001D418)
    if (cls == 0) return unlocks_.classS;
    if (cls == 6) return unlocks_.bonus;
    const ArcadeClassCars& cc = a_.data.classes.at(size_t(std::clamp(cls, 0, 6)));
    std::vector<uint8_t> f;
    for (size_t i = 0; i < cc.cars.size(); i++) f.push_back(a_.data.ovl2.Get<uint8_t>(cc.flagsAddress + uint32_t(i)));
    return f;
}

void ArcadeMenus::DrawView(ArcadeView v, int alpha, int dy, std::vector<MenuPrim>& out) const { // 0x80013828
    MenuOtSlot contents, text, gradient;
    const std::string title = v == ArcadeView::kNotice ? notice_ : TitleOf(v);
    if (v == ArcadeView::kCar || v == ArcadeView::kCourse || v == ArcadeView::kGarage || v == ArcadeView::kBonus || v == ArcadeView::kCredits ||
        v == ArcadeView::kGuestLoad || v == ArcadeView::kBattle) {
        // the view's ordering table: header 6 / 4, the page 4 .. 0
        ViewOt ot;
        if (!title.empty()) AddArcadeHeader(ot.slot[4], ot.slot[6], a_.fonts[0], title, ColourOf(v), alpha);
        if (v == ArcadeView::kCar) carPage_->Draw(ot, ctx_);
        else if (v == ArcadeView::kGarage) carPage_->DrawGarage(ot, ctx_);
        else if (v == ArcadeView::kBonus) bonusPage_->Draw(ot, ctx_);
        else if (v == ArcadeView::kCredits) creditsPage_->Draw(ot.slot[0]);
        else if (v == ArcadeView::kGuestLoad) guestLoad_->Draw(ot);
        else if (v == ArcadeView::kBattle) battlePage_->Draw(ot, ctx_);
        else coursePage_->Draw(ot, ctx_);
        const size_t from = out.size();
        ot.Emit(out, 0x200);
        OffsetPrims(out, from, dy);
        return;
    }
    if (!title.empty()) AddArcadeHeader(text, gradient, a_.fonts[0], title, v == ArcadeView::kNotice ? 0x0010A0FFu : ColourOf(v), alpha);
    if (const PanelList* l = ListOf(v)) l->Draw(contents);
    else if (v == ArcadeView::kNotice) {
        shell::AddText(contents, a_.fonts[1], "Not available yet", 176, 240, 1, 0x808080u, 0, shell::TextAlign::kCentre);
        shell::AddText(contents, a_.fonts[2], "(not ported: docs/research/arcade_disc.md 16)", 176, 262, 1, 0x505050u, 0, shell::TextAlign::kCentre);
    }
    const size_t from = out.size();
    gradient.Emit(out, 0x200);
    text.Emit(out, 0x220);
    contents.Emit(out, 0x200);
    OffsetPrims(out, from, dy);
}

std::vector<MenuPrim> ArcadeMenus::Frame() const { // 0x80014544 (+ 0x80014470 offsets)
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    int t = transition_ * 0x80;
    if (t < 0) t += 0xF;
    const int oldAlpha = t >> 4;
    int newDy, oldDy;
    if (!popping_) {
        int d = transition_ * 200;
        if (d < 0) d += 0xF;
        newDy = d >> 4;
        oldDy = (transition_ - 0x10) * 5;
    } else {
        oldDy = (0x10 - transition_) * 5;
        int d = transition_ * -200;
        if (d < 0) d += 0xF;
        newDy = d >> 4;
    }
    if (transition_ > 0) DrawView(old_, oldAlpha, oldDy, prims);
    DrawView(stack_.back(), 0x80 - oldAlpha, newDy, prims);
    return prims;
}

} // namespace gt2::arcade
