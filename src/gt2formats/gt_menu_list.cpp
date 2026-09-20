#include "gt2formats/gt_menu_list.h"

#include <algorithm>
#include <stdexcept>

#include "gt2vfs/gtfs.h"

namespace gt2 {
namespace {

// ovl4 addresses.
constexpr uint32_t kNoName = 0x80050B9Cu;        // u"No Name" (0x800182A8 / 0x800182FC)
constexpr uint32_t kRacingPrefix = 0x80050BB0u;  // u"\x7F" (0x8001828C(1)); 0x80050BAC = u"" for 0
constexpr uint32_t kEmptyText = 0x801C30C0u;     // data-gt.txd string drawn by 0x8002068C / 0x80020B70
constexpr uint32_t kEmptyColour = 0xF0780Au;
constexpr int kAnchorX = 0x100, kGarageAnchorY = 0xB0, kUsedCarAnchorY = 0x9C; // 0x8001B9AC types 9 / 0x50

std::u16string WideAt(const GuestImage& img, uint32_t address) {
    std::u16string s;
    for (uint32_t a = address;; a += 2) {
        const char16_t c = char16_t(img.Get<uint16_t>(a));
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

MenuPrim Quad(MenuPrim::Kind kind, const int (&xs)[4], const int (&ys)[4], const uint32_t (&cs)[4], bool semi) {
    MenuPrim p;
    p.kind = kind;
    for (int i = 0; i < 4; i++) {
        p.x[i] = int16_t(xs[i]);
        p.y[i] = int16_t(ys[i]);
        p.colour[i] = cs[i] & 0xFFFFFF;
    }
    p.semi = semi;
    return p;
}

// 0x8006B6E4: POLY_G4 0x3A (semi-transparent), left colour at v0 / v2, right colour at v1 / v3.
void HighlightQuad(MenuOtSlot& ot, int x, int y, int w, int h, uint32_t left, uint32_t right) {
    ot.Add(Quad(MenuPrim::kPolyG4, {x, x + w, x, x + w}, {y, y, y + h, y + h}, {left, right, left, right}, true));
}

} // namespace

// ---------------------------------------------------------------- OT slot

void MenuOtSlot::AddGlyphs(const std::vector<MenuPrim>& glyphs) {
    for (const MenuPrim& g : glyphs) {
        Add(g);
        DrawMode(g.tpage);
    }
}

void MenuOtSlot::Emit(std::vector<MenuPrim>& gpuOrder, uint16_t mode) const {
    for (auto it = packets_.rbegin(); it != packets_.rend(); ++it) {
        if (it->mode) {
            mode = it->e1;
            continue;
        }
        MenuPrim p = it->prim;
        if (p.kind == MenuPrim::kPolyFT4) mode = uint16_t((mode & ~0x1FFu) | (p.tpage & 0x1FFu)); // the primitive's texture page word
        p.tpage = uint16_t(mode & 0x7FF);
        if (p.kind != MenuPrim::kSprite) p.dither = (mode & 0x200) != 0;
        gpuOrder.push_back(p);
    }
}

uint16_t MenuOtSlot::FinalMode(uint16_t mode) const {
    for (auto it = packets_.rbegin(); it != packets_.rend(); ++it)
        if (it->mode) mode = it->e1;
        else if (it->prim.kind == MenuPrim::kPolyFT4) mode = uint16_t((mode & ~0x1FFu) | (it->prim.tpage & 0x1FFu));
    return mode;
}

// ---------------------------------------------------------------- primitive helpers

uint32_t MenuListLerp(uint32_t a, uint32_t b, int t, int max) { // 0x8006B548
    if (max < t) t = max;
    if (t < 0) t = 0;
    uint32_t out = 0;
    for (int c = 0; c < 3; c++) {
        const int ca = int((a >> (8 * c)) & 0xFF), cb = int((b >> (8 * c)) & 0xFF);
        out |= uint32_t(ca + (cb - ca) * t / max) << (8 * c); // C division (towards zero), as `div`
    }
    return (a & 0xFF000000u) | (out & 0xFFFFFF);
}

void MenuListHighlight(MenuOtSlot& ot, int x, int y, int w, int h, uint32_t c0, uint32_t c1, int t) { // 0x8006B814
    const int left = x - (w >> 1);
    bool inner = true;
    int grow = t;
    if (grow > 16) {
        inner = false;
        grow = 16;
    }
    int prod = w * grow;
    if (prod < 0) prod += 15;
    const int bar = prod >> 4;
    int fadeT = 64 - t;
    uint16_t dither = 0x200;
    if (fadeT < 0) {
        fadeT = 0;
        dither = 0;
    }
    if (fadeT > 48) fadeT = 48;
    const uint32_t mid = MenuListLerp(c0, c1, fadeT, 48);
    HighlightQuad(ot, left, y, bar, h, c0, mid);
    HighlightQuad(ot, left + (w - bar), y, bar, h, mid, c0);
    if (inner) {
        HighlightQuad(ot, left + bar, y, w - bar, h, mid, c0);
        HighlightQuad(ot, left, y, w - bar, h, c0, mid);
    }
    ot.DrawMode(uint16_t(dither | 0x20));
}

void MenuListPaintChip(MenuOtSlot& ot, int x, int y, int w, int h, uint16_t c, int alpha) { // 0x8006BB08
    int grey = (alpha * 0xF4) >> 7;
    int r = int((uint32_t(c & 0x1F) * uint32_t(alpha)) >> 4);        // srl: unsigned
    int g = int((uint32_t(c & 0x3E0) * uint32_t(alpha)) >> 9);
    int b = int((uint32_t(c & 0x7C00) * uint32_t(alpha)) >> 14);
    grey = std::min(grey, 255);
    r = std::min(r, 255);
    g = std::min(g, 255);
    b = std::min(b, 255);
    const uint32_t c0 = uint32_t(grey) * 0x010101u, c1 = uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16;
    ot.Add(Quad(MenuPrim::kPolyG4, {x + 1, x + w - 1, x + 1, x + w - 1}, {y + 1, y + 1, y + h - 1, y + h - 1}, {c0, c1, 0, 0}, false));
    MenuPrim tile; // 0x8007D024(ot, 0): TILE 0x60, colour 0
    tile.kind = MenuPrim::kTile;
    tile.x[0] = int16_t(x);
    tile.y[0] = int16_t(y);
    tile.w = int16_t(w);
    tile.h = int16_t(h);
    tile.colour[0] = 0;
    ot.Add(tile);
    ot.DrawMode(0x200);
}

void MenuListFrame(MenuOtSlot& ot, uint32_t colour, int x, int y, int w, int h) { // 0x8007E780
    if (w == 0 || h == 0) return;
    auto line = [&](int x0, int y0, int x1, int y1) {
        MenuPrim p;
        p.kind = MenuPrim::kLine;
        p.x[0] = int16_t(x0);
        p.y[0] = int16_t(y0);
        p.x[1] = int16_t(x1);
        p.y[1] = int16_t(y1);
        p.colour[0] = p.colour[1] = colour & 0xFFFFFF;
        p.semi = (colour & 0x02000000u) != 0; // 0x40 / 0x48 ^ colour
        return p;
    };
    if (h == 1) {
        ot.Add(line(x, y, x + w, y)); // 0x8007F7F4: LINE 0x40
        return;
    }
    if (w == 1) {
        ot.Add(line(x, y, x, y + h));
        return;
    }
    // 0x8007E738: polyline 0x48, (x, y) (x + w - 1, y) (x + w - 1, y + h - 1) (x, y + h - 1) (x, y), 0x50005000.
    // The segments of one polyline are drawn in order; kept as one packet so their order survives the reversal.
    const int x1 = x + w - 1, y1 = y + h - 1;
    const MenuPrim segs[4] = {line(x, y, x1, y), line(x1, y, x1, y1), line(x1, y1, x, y1), line(x, y1, x, y)};
    // Added last-first: Emit reverses the slot, which restores the polyline's own order.
    for (int i = 3; i >= 0; i--) ot.Add(segs[i]);
}

void MenuListArrow(MenuOtSlot& ot, int x, int y, int w, int h, int t) { // 0x8006B988
    int k = 40 - t;
    if (k > 10) k = 10;
    if (k < 0) k = 0;
    const int r = (k * 255) / 10;
    const uint32_t c = uint32_t(r) | uint32_t(r >> 1) << 8; // | 0x2000000 -> POLY_F3 0x22 (0x8007E0E0 xors 0x20000000)
    ot.Add(Quad(MenuPrim::kPolyF4, {x, x + w, x - w, x - w}, {y + h, y, y, y}, {c, c, c, c}, true));
}

// ---------------------------------------------------------------- widget

MenuListWidget MenuListWidget::Read(const GuestImage& ovl4, uint32_t a) {
    MenuListWidget w;
    w.count = ovl4.Get<int16_t>(a + 0x00);
    w.flags = ovl4.Get<uint16_t>(a + 0x02);
    w.visible = ovl4.Get<int16_t>(a + 0x04);
    w.selection = ovl4.Get<int16_t>(a + 0x06);
    w.width = ovl4.Get<int16_t>(a + 0x08);
    w.rowHeight = ovl4.Get<int16_t>(a + 0x0A);
    w.rowGap = ovl4.Get<int16_t>(a + 0x0C);
    w.arrowHalfWidth = ovl4.Get<int8_t>(a + 0x0E);
    w.arrowHeight = ovl4.Get<int8_t>(a + 0x0F);
    w.x = ovl4.Get<int16_t>(a + 0x10);
    w.y = ovl4.Get<int16_t>(a + 0x12);
    w.fadeMax = ovl4.Get<int16_t>(a + 0x14);
    w.callbackFlags = ovl4.Get<uint16_t>(a + 0x16);
    w.revealed = ovl4.Get<int16_t>(a + 0x18);
    w.revealDelay = ovl4.Get<int16_t>(a + 0x1A);
    w.revealPeriod = ovl4.Get<int16_t>(a + 0x1C);
    w.active = ovl4.Get<uint8_t>(a + 0x1E);
    w.byte1F = ovl4.Get<uint8_t>(a + 0x1F);
    w.scroll = ovl4.Get<int16_t>(a + 0x20);
    w.blink = ovl4.Get<int16_t>(a + 0x22);
    w.fade = ovl4.Get<int16_t>(a + 0x24);
    w.state = ovl4.Get<int16_t>(a + 0x26);
    return w;
}

void MenuListReset(MenuListWidget& w, MenuListCallback callback) { // 0x8006CDCC
    w.state = -1;
    w.selection = 0;
    w.blink = 0;
    w.scroll = 0;
    w.revealed = 0;
    w.revealPeriod = 6;
    w.revealDelay = 6;
    w.fade = 0;
    w.callback = std::move(callback);
    if (!(w.callbackFlags & 1))
        for (int i = 0; i < w.count; i++) w.callback(kMenuListReset, w, i, nullptr);
}

void MenuListOpen(MenuListWidget& w) { // 0x8006CE70
    w.revealPeriod = 6;
    w.revealDelay = 6;
    w.scroll = 0;
    w.revealed = 0;
    w.fade = 0;
    w.state = 0;
    w.blink = 30;
    if (!(w.callbackFlags & 0x80)) w.callback(kMenuListOpen, w, w.selection, nullptr);
}

void MenuListClose(MenuListWidget& w) { // 0x8006CED8
    w.state = -65;
    w.fade = w.fadeMax;
    if (!(w.callbackFlags & 4))
        for (int i = 0; i < w.count; i++)
            if (i != w.selection) w.callback(kMenuListClose, w, i, nullptr);
}

void MenuListClamp(MenuListWidget& w, int lo, int hi) { // 0x8006D4B0
    int s = w.selection;
    if (s < lo) {
        w.scroll = 0;
        s = lo;
    }
    if (hi < s) {
        w.scroll = 0;
        s = hi;
    }
    if (w.count <= s) s = w.count - 1;
    if (s < 0) s = 0;
    w.selection = int16_t(s);
}

int32_t MenuListUpdate(MenuListWidget& w, const MenuListPad* pad) { // 0x8006CFC4
    int32_t result = -2;
    int command = -1;
    w.active = 0;
    if (!(w.callbackFlags & 8))
        for (int i = 0; i < w.count; i++) w.callback(kMenuListTick, w, i, nullptr);
    if (w.scroll < 0) w.scroll++;
    if (w.scroll > 0) w.scroll--;
    if (++w.blink > 60) w.blink = 0;
    if (w.state < 0) {
        if (w.state == -1) return -2; // closed
        w.state++;
        if (!(w.callbackFlags & 4) && w.state == -58) w.callback(kMenuListClose, w, w.selection, nullptr);
        if (--w.fade < 0) w.fade = 0;
        return -2;
    }
    if (++w.fade > w.fadeMax) w.fade = w.fadeMax;
    if (++w.state > 45) w.state = 0;
    if (pad) {
        w.active = 1;
        uint32_t bits = pad->pressed;
        if (bits & menu_list_pad::kBack) {
            result = -1;
        } else if (bits & menu_list_pad::kChoose) {
            result = w.selection;
            if (w.callback(kMenuListEnabled, w, w.selection, nullptr) == 0) result = -4;
        } else {
            bits |= pad->repeat;
            int sel = w.selection, from = 0;
            if (bits & menu_list_pad::kUp) {
                from = sel;
                sel--;
                command = kMenuListLeave;
                if (sel < 0) sel = (w.flags & 4) ? w.count - 1 : 0;
                if (sel != w.selection) {
                    result = -3;
                    w.scroll = -8;
                    w.blink = 0;
                }
            }
            if (bits & menu_list_pad::kDown) {
                from = sel;
                sel++;
                command = kMenuListLeave;
                if (sel >= w.count) sel = (w.flags & 4) ? 0 : w.count - 1;
                if (sel != w.selection) {
                    result = -3;
                    w.scroll = 8;
                    w.blink = 0;
                }
            }
            if (w.flags & 0x10) {
                if (bits & menu_list_pad::kLeft) {
                    from = sel;
                    sel = sel - w.visible - 1;
                    command = kMenuListLeave;
                    if (sel < 0) sel = 0;
                    result = -3;
                    w.scroll = -8;
                    w.blink = 0;
                }
                if (bits & menu_list_pad::kRight) {
                    from = sel;
                    sel = sel + w.visible - 1;
                    command = kMenuListLeave;
                    if (sel >= w.count) sel = w.count - 1;
                    result = -3;
                    w.scroll = 8;
                    w.blink = 0;
                }
            }
            if (command >= 0) {
                w.callback(kMenuListLeave, w, from, nullptr);
                w.callback(kMenuListEnter, w, sel, nullptr);
            }
            w.selection = int16_t(sel);
        }
    }
    // Rows appear one by one (command 1 every revealPeriod updates).
    while (w.revealed < w.count) {
        if (--w.revealDelay > 0) break;
        if (!(w.callbackFlags & 2)) w.callback(kMenuListReveal, w, w.revealed, nullptr);
        w.revealDelay = w.revealPeriod;
        w.revealed++;
        if (!(w.revealed < w.count && w.revealDelay < 0)) break;
    }
    return result;
}

void MenuListDraw(const MenuListWidget& w, MenuOtSlot& ot) { // 0x8006D50C
    const int sel = w.selection, count = w.count, vis = w.visible, half = vis >> 1;
    const int pitch = w.rowHeight + w.rowGap;
    const int scroll = w.scroll;
    int prod = scroll * pitch;
    if (prod < 0) prod += 7;
    int y = w.y + (prod >> 3);                 // s3
    int alphaLeaving = scroll << 7;
    if (alphaLeaving < 0) alphaLeaving += 7;
    alphaLeaving >>= 3;
    if (alphaLeaving < 0) alphaLeaving = -alphaLeaving;           // sp84: |scroll| * 16, the row scrolling out of view
    const int alphaEntering = 128 - alphaLeaving;           // sp80: the row scrolling into view
    const int lower = vis - half;               // t0
    int first = sel - half;                     // s5
    if (!(first + vis < count)) first = count - vis;
    if (first < 0) first = 0;
    int end = vis;                              // s7 (rows, then the end index)
    int rowEntering = -1, rowLeaving = -1;                // s6, sp76
    if (scroll < 0) {
        rowEntering = sel - half;
        if (!(rowEntering < count - vis)) rowEntering = -1;
        rowLeaving = sel + lower;
        if (rowLeaving < vis) rowLeaving = -1;
        end = vis + 1;
        if (sel == count - 1) {
            if (vis < count) {
                rowEntering = count - vis;
            } else {
                end = w.visible;
                y = w.y;
                rowEntering = -1;
            }
        } else if (sel < half || !(sel < count - lower)) {
            end = vis;
            y = w.y;
        }
    }
    bool skipAdd = false;
    if (scroll > 0) {
        rowEntering = sel + lower - 1;
        rowLeaving = sel - half - 1;
        if (rowEntering < vis) rowEntering = -1;
        if (!(rowLeaving < count - vis)) rowLeaving = -1;
        end = end + 1;
        first = first - 1;
        y -= pitch;
        if (sel == 0) {
            first = -1;
            end = vis + 1;
            rowEntering = vis + first;
            if (!(vis < count)) {
                y = w.y;
                end = vis;
                rowEntering = first;
                first = 0;
            }
        } else if (!(!(half < sel) || count - lower < sel)) {
            end += first;
            skipAdd = true;
        } else {
            first += 1;
            end = vis;
            y = w.y;
        }
    }
    if (!skipAdd) end += first;
    y += w.rowHeight >> 1;

    MenuListRowDraw block;
    block.ot = &ot;
    block.drawn = 0;
    for (int row = first; row < end; row++, y += pitch) {
        if (row < 0 || row >= count) continue;
        if (row == sel && w.state >= 0 && (w.flags & 8))
            MenuListHighlight(ot, w.x, y - (w.rowHeight >> 1), w.width, w.rowHeight, 0x0F0F0F, 0x363636, (w.blink << 7) / 60);
        int alpha = 128;
        if (row == rowEntering) alpha = alphaEntering;
        if (row == rowLeaving) alpha = alphaLeaving;
        block.enabled = true;
        if (w.callback(kMenuListEnabled, w, row, &block) == 0) {
            alpha >>= 1;
            block.enabled = false;
        }
        block.alpha = int16_t(alpha);
        block.x = w.x;
        block.y = int16_t(y);
        if (!(w.callbackFlags & 0x10)) w.callback(kMenuListDraw, w, row, &block);
        block.drawn++;
    }
    if (w.state < 0 || w.active == 0) return;
    const int t = (w.flags & 0x20) ? 0 : w.state;
    if (first > 0) MenuListArrow(ot, w.x, w.y - w.rowGap, w.arrowHalfWidth, -w.arrowHeight, t);
    if (end < count) MenuListArrow(ot, w.x, w.y + pitch * vis, w.arrowHalfWidth, w.arrowHeight, t);
    ot.DrawMode(0x20);
}

// ---------------------------------------------------------------- rows and names

std::vector<MenuUsedCarRow> MenuUsedCarRows(const UsedCarLists& lots, const CarInfoDirectory& cars, uint32_t day, uint8_t maker, uint8_t language) {
    std::vector<MenuUsedCarRow> rows;
    if (maker >= kUsedCarMakerCount) return rows;
    for (const UsedCarEntry& e : lots.Lot(UsedCarLists::PeriodOfCounter(day), maker, cars, language))
        rows.push_back({e.carId, e.price & 0xFFFFFFu, int8_t(e.paintId)});
    return rows;
}

MenuListNames MenuListNames::Load(const GtfsVolume& vol, const GuestImage& ovl4) {
    MenuListNames n{CarInfoDirectory::Load(vol), CarParamTables::Load(vol), ParseUniStrDb(vol.Read("carparam/usa_unistrdb.dat")), {}, {}};
    n.noName = WideAt(ovl4, kNoName);
    n.racingPrefix = WideAt(ovl4, kRacingPrefix);
    return n;
}

std::u16string MenuListNames::Model(uint32_t carId) const {
    const std::optional<size_t> row = FindCatalogueRow(params, carId);
    if (!row) return noName;
    const uint16_t index = CarCatalogueAt(params, *row).modelName;
    return index < strings.size() ? strings[index] : noName;
}

std::u16string MenuListNames::Grade(uint32_t carId) const {
    const std::optional<size_t> row = FindCatalogueRow(params, carId);
    if (!row) return noName;
    const uint16_t index = CarCatalogueAt(params, *row).gradeName;
    return index < strings.size() ? strings[index] : noName;
}

uint16_t MenuListNames::ChipColour(uint32_t carId, int32_t paint) const { // 0x80060D28 -> 0x80060BEC + 0x80060C90
    const CarInfoRecord* r = cars.Find(carId);
    if (!r || r->chipColors.empty()) return 0;
    auto lower = [](int32_t c) { return uint32_t(c - 0x41) < 0x1Au ? c + 0x20 : c; };
    const int32_t want = lower(paint);
    size_t index = 0;
    for (size_t i = 0; i < r->PaintCount() && i < r->paintIds.size(); i++)
        if (lower(int32_t(int8_t(r->paintIds[i]))) == want) {
            index = i;
            break;
        }
    return index < r->chipColors.size() ? r->chipColors[index] : 0;
}

MenuListColours MenuListColours::Read(const GuestImage& ovl4, bool garage) {
    MenuListColours c;
    if (garage) {
        c.base = ovl4.Get<uint32_t>(0x8005298Cu);
        c.text = ovl4.Get<uint32_t>(0x80052990u);
        c.frame = ovl4.Get<uint32_t>(0x80052994u);
        c.marker = ovl4.Get<uint32_t>(0x80052998u);
    } else {
        c.base = ovl4.Get<uint32_t>(0x800529D0u);
        c.text = ovl4.Get<uint32_t>(0x800529D4u);
        c.price = ovl4.Get<uint32_t>(0x800529D8u);
        c.frame = ovl4.Get<uint32_t>(0x800529DCu);
    }
    return c;
}

// ---------------------------------------------------------------- the two lists

MenuPopupList::MenuPopupList(MenuListKind k, const MenuAssets& assets, const MenuListNames& names)
    : kind(k), widget(MenuListWidget::Read(assets.ovl4, k == MenuListKind::kGarage ? MenuListWidget::kGarage : MenuListWidget::kUsedCars)),
      colours(MenuListColours::Read(assets.ovl4, k == MenuListKind::kGarage)), assets_(assets), names_(names) {
    widget.callback = [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { return RowCallback(command, w, row, draw); };
}

void MenuPopupList::Reset() { // 0x80020490 / 0x800209B0
    MenuListReset(widget, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { return RowCallback(command, w, row, draw); });
    count = 0;
    flash = -1;
}

void MenuPopupList::LoadGarage(const MenuItem* it, std::vector<MenuGarageRow> rows, int16_t current) { // 0x800204EC
    garageRows = std::move(rows);
    currentCar = current;
    item = it;
    const int16_t n = int16_t(garageRows.size());
    if (n > 0) {
        widget.count = n;
        MenuListOpen(widget);
        if (!(widget.selection < n)) widget.selection = int16_t(n - 1);
    }
    count = n;
}

void MenuPopupList::LoadUsedCars(const MenuItem* it, std::vector<MenuUsedCarRow> rows) { // 0x80020A0C
    usedCarRows = std::move(rows);
    item = it;
    const int16_t n = int16_t(usedCarRows.size());
    if (n > 0) {
        widget.count = n;
        MenuListOpen(widget);
        if (!(widget.selection < n)) widget.selection = int16_t(n - 1);
    }
    count = n;
}

int32_t MenuPopupList::Update(const MenuListPad* pad, bool active) { // 0x8002055C / 0x80020A94
    if (!active) {
        flash = -1;
    } else if (++flash > 60) {
        flash = 0;
    }
    if (count <= 0) return -1;
    const int32_t r = MenuListUpdate(widget, pad);
    if (r == -3) {
        if (sound) sound(6);
        return -1;
    }
    if (r < -2) { // -4: choose on a disabled row
        if (r == -4) return -1;
        chosen = int16_t(r);
        return r;
    }
    if (r == -2) {
        // 0x8002055C reads pad +4 even when the pad is null (then RAM word 4, 0 in every dump we have).
        if (kind == MenuListKind::kGarage && pad && (pad->pressed & menu_list_pad::kStart)) {
            if (moveToTop) moveToTop(widget.selection);
            if (sound) sound(1);
        }
        return -1;
    }
    if (r == -1) return -2;
    chosen = int16_t(r);
    return r;
}

void MenuPopupList::Draw(MenuOtSlot& ot) {
    const bool garage = kind == MenuListKind::kGarage;
    widget.x = int16_t(kAnchorX); // 0x800204D8 / 0x800209F8
    widget.y = int16_t(garage ? kGarageAnchorY : kUsedCarAnchorY);
    if (count < 1) {
        std::vector<MenuPrim> glyphs;
        MenuTextWriter text(assets_.fonts, glyphs);
        text.Left(MenuTextWriter::Widen(assets_.String(kEmptyText)), true, false, widget.x - (garage ? 152 : 188), widget.y + 64, kEmptyColour);
        ot.AddGlyphs(glyphs);
        return;
    }
    MenuListDraw(widget, ot);
}

void MenuPopupList::Draw(std::vector<MenuPrim>& gpuOrder) {
    MenuOtSlot ot;
    Draw(ot);
    ot.Emit(gpuOrder);
}

MenuUsedCarRow MenuPopupList::ChosenUsedCar() const {
    if (widget.selection < 0 || size_t(widget.selection) >= usedCarRows.size()) return {};
    return usedCarRows[size_t(widget.selection)];
}

int32_t MenuPopupList::RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) {
    if (command == kMenuListEnter) {
        flash = 0;
        return 1;
    }
    if (command != kMenuListDraw || !draw || row < 0) return 1;
    MenuOtSlot& ot = *draw->ot;
    const int x = draw->x, y = draw->y + 4, alpha = draw->alpha;
    const bool garage = kind == MenuListKind::kGarage;
    // The pulsing bar of the selected row while the list is the active popup.
    if (row == w.selection && flash >= 0) {
        if (garage) MenuListHighlight(ot, x, y - 14, 0x180, 20, 0x141414, 0x505050, (flash << 7) / 60);
        else MenuListHighlight(ot, x, y - 16, 0x1A0, 24, 0x141414, 0x505050, (flash << 7) / 60);
        ot.DrawMode(0x220);
    }
    auto text = [&](const std::u16string& s, int tx, uint32_t colour) {
        std::vector<MenuPrim> glyphs;
        MenuTextWriter writer(assets_.fonts, glyphs);
        const int width = writer.Draw(s, false, false, tx, y, colour); // 0x8001F38C: UTF-16, font A
        ot.AddGlyphs(glyphs);
        return width;
    };
    if (garage) { // 0x80020198
        if (size_t(row) >= garageRows.size()) return 1;
        const MenuGarageRow& s = garageRows[size_t(row)];
        const std::u16string prefix = (s.word98 >> 15) ? names_.racingPrefix : std::u16string();
        const uint32_t tc = MenuListLerp(colours.base, colours.text, alpha, 128);
        int tx = x - 152;
        tx += text(prefix, tx, tc);
        tx += text(names_.Model(s.carId), tx, tc);
        text(names_.Grade(s.carId), tx + 5, tc);
        MenuListPaintChip(ot, x - 165, y - 9, 9, 12, names_.ChipColour(s.modelId, int32_t(s.paint)), alpha);
        if (currentCar == row) { // 0x8007D024(ot, colour): TILE 0x60 ^ colour
            MenuPrim tile;
            tile.kind = MenuPrim::kTile;
            const uint32_t mc = MenuListLerp(colours.base, colours.marker, alpha, 128);
            tile.colour[0] = mc & 0xFFFFFF;
            tile.semi = (mc & 0x02000000u) != 0;
            tile.x[0] = int16_t(x - 184);
            tile.y[0] = int16_t(y - 8);
            tile.w = 14;
            tile.h = 8;
            ot.Add(tile);
        }
        MenuListFrame(ot, MenuListLerp(colours.base, colours.frame, alpha, 128), x - 192, y - 14, 384, 20);
        return 1;
    }
    // 0x800206F8
    if (size_t(row) >= usedCarRows.size()) return 1;
    const MenuUsedCarRow& e = usedCarRows[size_t(row)];
    const uint32_t tc = MenuListLerp(colours.base, colours.text, alpha, 128);
    const int tx = x - 188;
    const int modelWidth = text(names_.Model(e.carId), tx, tc);
    text(names_.Grade(e.carId), tx + modelWidth + 5, tc);
    MenuListPaintChip(ot, x - 201, y - 9, 9, 12, names_.ChipColour(e.carId, e.paint), alpha);
    const uint32_t pc = MenuListLerp(colours.base, colours.price, alpha, 128);
    const std::u16string price = MenuTextWriter::Widen(MenuThousands(e.price)); // 0x8001FCDC
    std::vector<MenuPrim> scratch;
    const int width = MenuTextWriter(assets_.fonts, scratch).Width(price, false, false); // 0x8001F6CC
    text(price, x - (width - 200), pc);
    MenuListFrame(ot, MenuListLerp(colours.base, colours.frame, alpha, 128), x - 208, y - 16, 416, 24);
    return 1;
}

} // namespace gt2
