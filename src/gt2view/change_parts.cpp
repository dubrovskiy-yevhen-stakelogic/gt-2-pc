// The race overlay's CHANGE PARTS page. See change_parts.h.
#include "gt2view/change_parts.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/career/tuning.h"
#include "game/shell/title_draw.h"

namespace gt2::screens {

namespace {

// ovl0 data (US Simulation v1.2 addresses; the arcade assets are laid out at them).
constexpr uint32_t kGroupTables[3] = {0x8005C364u, 0x8005C384u, 0x8005C3A4u}; // turbo + NA / turbo / neither
constexpr uint32_t kIcons = 0x8005B830u;       // 12-byte sprites {uv | clut << 16, w, h, tpage}
constexpr uint32_t kRowSprite = 0x8005B89Cu;   // the part row's background
constexpr uint32_t kPictures = 0x8005B8B4u;    // the parts' pictures (part + 0xE)
constexpr uint32_t kColourBase = 0x8005B95Cu;  // 0x02000000
constexpr uint32_t kRowBase = 0x8005B960u;
constexpr uint32_t kColourNoRow = 0x8005B964u, kColourDisabled = 0x8005B968u, kColourFlash = 0x8005B96Cu, kColourStage = 0x8005B970u;
constexpr uint32_t kColourFigures = 0x8005B978u, kColourRestricted = 0x8005B97Cu;
constexpr uint32_t kPageBand = 0x8005C3C0u, kListWidget = 0x8005C3DCu, kListBand = 0x8005C410u, kGraph = 0x8005C42Cu;
constexpr uint32_t kFontLarge = 0x801C9150u, kFontMedium = 0x801C9120u, kFontSmall = 0x801C9110u;
// data-race.txd / data-global.txd strings
constexpr uint32_t kStrPartsSetting = 0x801C83E6u, kStrPowerFigures = 0x801C717Au, kStrTorqueFigures = 0x801C7185u, kStrRestricted = 0x801C8442u;
constexpr uint32_t kStrHp = 0x801EF6B6u, kStrLbFt = 0x801EF6B0u, kStr1000Rpm = 0x801EF6B9u;
constexpr uint32_t kDescriptionColour = 0x0278500Au; // state 1's part description (US branch)

// 0x8005EE4C's jump table (EXE 0x8008F8D0): the sheet offset of each kind's row index words.
constexpr uint16_t kSlotRows[23] = {0x0AB8, 0x0AE0, 0x0B08, 0x0BB0, 0x0F48, 0x0688, 0x12A8, 0x12F8, 0x1338, 0x1360, 0x1380, 0x13A0,
                                    0x13C0, 0x13F8, 0x1420, 0x148C, 0x14C4, 0x1500, 0x1540, 0x15DC, 0x1610, 0x1638, 0x1700};

uint32_t Lerp(const GuestImage& o, uint32_t a, uint32_t b, int t, int max) { return MenuListLerp(o.Get<uint32_t>(a), o.Get<uint32_t>(b), t, max); }

// The text context's glyphs (0x8006AC90 and friends): mode 1 (the page word masked & 0xFF9FFFFF | 0x200000).
void AddGlyphs(MenuOtSlot& ot, const std::vector<HudFontSprite>& glyphs, uint32_t colour) {
    const bool semi = (colour & 0x2000000u) != 0;
    for (const HudFontSprite& g : glyphs) {
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

void Text(MenuOtSlot& ot, const HudFont& font, const std::string& s, int x, int y, int spacing, uint32_t colour) { // 0x8006AC90
    std::vector<HudFontSprite> g;
    font.Text(s, x, y, spacing, g);
    AddGlyphs(ot, g, colour);
}

void CentredText(MenuOtSlot& ot, const HudFont& font, const std::string& s, int x, int y, int spacing, uint32_t colour) { // 0x8006ADB4
    Text(ot, font, s, x - (font.TextWidth(s, spacing) >> 1), y, spacing, colour);
}

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour) { // 0x8007D024
    MenuPrim p;
    p.kind = MenuPrim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

MenuPrim Sprite(const GuestImage& o, uint32_t entry, int x, int y, uint32_t colour, uint16_t modeBits) { // 0x80081478 + E1
    MenuPrim p;
    p.kind = MenuPrim::kSprite;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y);
    p.u = o.Get<uint8_t>(entry), p.v = o.Get<uint8_t>(entry + 1);
    p.clut = o.Get<uint16_t>(entry + 2);
    p.w = o.Get<int16_t>(entry + 4), p.h = o.Get<int16_t>(entry + 6);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    p.tpage = uint16_t(o.Get<uint16_t>(entry + 8) | modeBits);
    return p;
}

// 0x8007E738: a polyline 0x48 of five points (the segments added last-first: the slot's reversal restores their order).
void Polyline(MenuOtSlot& ot, uint32_t colour, const std::array<std::pair<int, int>, 5>& pt) {
    MenuPrim segs[4];
    for (int i = 0; i < 4; i++) {
        MenuPrim& p = segs[i];
        p.kind = MenuPrim::kLine;
        p.x[0] = int16_t(pt[size_t(i)].first), p.y[0] = int16_t(pt[size_t(i)].second);
        p.x[1] = int16_t(pt[size_t(i) + 1].first), p.y[1] = int16_t(pt[size_t(i) + 1].second);
        p.colour[0] = p.colour[1] = colour & 0xFFFFFF;
        p.semi = (colour & 0x2000000u) != 0;
    }
    for (int i = 3; i >= 0; i--) ot.Add(segs[i]);
}

// 0x8006BA48: POLY_F3 apex (x + w, y), base (x, y + h) / (x, y - h); colour as 0x8006B988.
void SideArrow(MenuOtSlot& ot, int x, int y, int w, int h, int t) {
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

int BandAlpha(const shell::Band& b) { // 0x8006BEB4
    const int a = b.anim >= 0 ? b.anim : b.anim < -1 ? ~b.anim : 0;
    return b.steps ? (a << 7) / b.steps : 0;
}

std::string Format(const std::string& f, int a, int b = 0, int c = 0) {
    char s[128];
    std::snprintf(s, sizeof(s), f.c_str(), a, b, c);
    return s;
}

} // namespace

int32_t SheetSlotRow(const career::TuneSheet& sheet, int32_t kind, int32_t stage) { // 0x8005EE4C
    if (kind < 0 || kind >= 23)
        throw std::logic_error("0x8005EE4C: kind " + std::to_string(kind) + " stage " + std::to_string(stage) + " (the original returns its fourth argument register)");
    int32_t v;
    std::memcpy(&v, reinterpret_cast<const uint8_t*>(&sheet) + kSlotRows[kind] + size_t(stage) * 4, 4);
    return v;
}

// ---------------------------------------------------------------- the preview queue

void PartsPreview::Reset() { // 0x800576D8 (0x80057628 also sets the buffer)
    count = 0;
    done.fill(false);
}

void PartsPreview::Queue(int stages, int16_t kind) { // 0x80057654
    index = 0;
    count = int8_t(stages + 1);
    done[0] = false;
    for (int i = 0; i < stages && i + 1 < int(slots.size()); i++) {
        slots[size_t(i) + 1].kind = kind;
        slots[size_t(i) + 1].stage = int16_t(i);
        done[size_t(i) + 1] = false;
    }
}

bool PartsPreview::Step(const ChangePartsContext& c) { // 0x800576FC
    if (index >= count) return false;
    Slot& s = slots[size_t(index)];
    bool built = false;
    sim::CarParams record{};
    if (index == 0) { // 0x8005F410 + 0x80077214
        record = career::SheetRecord(*c.sheet, c.data->tables);
        built = true;
    } else if (SheetSlotRow(*c.sheet, s.kind, s.stage) >= 0) { // 0x8005F044 + 0x80077214
        record = career::PreviewRecord(*c.sheet, s.kind, s.stage, *c.data);
        built = true;
    }
    if (built) {
        career::CarPowerFigures(record, s.figures.data()); // 0x80075930
        const int points = s.Figure(0xA);
        int16_t rpm[32], power[32], torque[32];
        for (int i = 0; i < 16; i++) {
            rpm[i] = int16_t(s.Figure(0xC + size_t(i) * 2));
            power[i] = int16_t(s.Figure(0x2C + size_t(i) * 2));
            torque[i] = int16_t(s.Figure(0x4C + size_t(i) * 2));
        }
        if (points > 16) throw std::logic_error("0x8007489C: more than 16 figure points");
        BuildPowerGraphCurves(points, rpm, power, torque, s.power.data(), s.torque.data());
        done[size_t(index)] = true;
    }
    index++;
    return index == count;
}

void PartsPreview::Maxima(int& rpm, int& power, int& torque) const { // 0x80057854
    rpm = -1, power = -1, torque = -1;
    for (int i = 0; i < count; i++) {
        if (!done[size_t(i)]) continue;
        const Slot& s = slots[size_t(i)];
        const int points = s.Figure(0xA);
        const int r = int16_t(s.Figure(size_t(0xC + (points - 1) * 2))); // points 0: + 0xA, the count itself
        if (rpm < r) rpm = r;
        if (power < int(s.Figure(0))) power = s.Figure(0);
        if (torque < int(s.Figure(4))) torque = s.Figure(4);
    }
}

// ---------------------------------------------------------------- the view

ChangePartsView::ChangePartsView(const RaceMenuAssets& a, const ChangePartsContext& c) : a_(a), c_(c) {
    if (!c_.sheet || !c_.data) throw std::invalid_argument("CHANGE PARTS: no sheet / career data");
    const GuestImage& o = a_.ovl0;
    // 0x800572C4: view + 0x14 = 12, 0x80053558(page, 0, 100)
    page_.x = 0, page_.y = 100;
    group_ = part_ = 0;
    input_ = true;
    groupText_ = partText_ = 0x8005A9A8u; // an empty string
    page_.state = 0;
    page_.open = -1;
    page_.arrowPhase = 0;
    page_.flash = 0;
    page_.previousColour = page_.colour = 0; // +0x34 = 0 before 0x800534B4
    SelectGroup(0);
    SelectPart(0);
    page_.groupCount = 7; // 0x80052D40: 8 with a turbo kit (0x8005F7C8), else 7
    for (int s = 1; s < 5; s++)
        if (SheetSlotRow(*c_.sheet, career::kTuneTurbo, s) >= 0) page_.groupCount = 8;
    page_.band = shell::Band::Read(o, kPageBand);
    page_.band.anim = -1; // page + 0x2C
    list_ = MenuListWidget::Read(o, kListWidget);
    MenuListReset(list_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) { return ListCallback(command, w, row, d); });
    listBand_ = shell::Band::Read(o, kListBand);
    listBand_.anim = -1; // 0x8005C428
    preview_.Reset();    // 0x80057628
    graph_ = PowerGraph::Read(o, kGraph);
    graph_.Reset();      // 0x80073CE4
    graphText_.font = &a_.FontAt(graph_.font);
    graphText_.hp = a_.Text(kStrHp);
    graphText_.lbft = a_.Text(kStrLbFt);
    graphText_.rpm1000 = a_.Text(kStr1000Rpm);
}

std::string ChangePartsView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t ChangePartsView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

uint32_t ChangePartsView::GroupRecord(int group) const { // 0x80052CB4
    bool turbo = false, na = false;
    for (int s = 1; s < 5; s++) turbo = turbo || SheetSlotRow(*c_.sheet, career::kTuneTurbo, s) >= 0; // 0x8005F7C8
    for (int s = 1; s < 4; s++) na = na || SheetSlotRow(*c_.sheet, career::kTuneNaTune, s) >= 0;       // 0x8005F790
    const uint32_t table = !turbo ? kGroupTables[2] : !na ? kGroupTables[1] : kGroupTables[0];
    return a_.ovl0.Get<uint32_t>(table + uint32_t(group) * 4);
}

int ChangePartsView::PartCount(uint32_t record) const { // 0x80052958
    int n = 0;
    while (n < 8 && a_.ovl0.Get<uint32_t>(record + 0x10 + uint32_t(n) * 0x10) != 0) n++;
    return n;
}

int ChangePartsView::StageCount(uint32_t record, int part) const { // 0x8005298C
    uint32_t e = a_.ovl0.Get<uint32_t>(record + uint32_t(part) * 0x10 + 0x18);
    int n = 0;
    while (a_.ovl0.Get<uint32_t>(e) != 0) n++, e += 12;
    return n;
}

void ChangePartsView::SelectGroup(int group) { // 0x800534B4
    const uint32_t r = GroupRecord(group);
    page_.previousColour = page_.colour;
    page_.group = int8_t(group);
    page_.colour = a_.ovl0.Get<uint32_t>(r + 8);
    groupText_ = a_.ovl0.Get<uint32_t>(r + 4); // 0x80052838 (the JP branch draws it)
}

void ChangePartsView::SelectPart(int part) { // 0x8005350C
    page_.part = int8_t(part);
    page_.flash = 0;
    const uint32_t r = GroupRecord(group_);
    partText_ = a_.ovl0.Get<uint32_t>(r + uint32_t(part) * 0x10 + 0x14); // 0x800528B0
}

int ChangePartsView::ListFade() const { return list_.fadeMax ? (int(list_.fade) << 7) / list_.fadeMax : 0; }

int ChangePartsView::Update(const MenuListPad* pad, bool input) { // 0x8005731C
    sounds.clear();
    if (viewDelay > 0 && --viewDelay == 0) { // 0x80053674
        page_.scroll = 0;
        page_.band.anim = 0;
        page_.open = 0;
    }
    const int32_t r = PageUpdate(input ? pad : nullptr, input);
    int sound = -1, result = 0;
    switch (r) {
    case -8: // L1: 0x80048374(M, 0x8005D1E4) PARTS SETTING, 0x80056FF0, 0x80053684
        exit = kPartsSetting;
        sound = 3;
        result = 1;
        break;
    case -7: sound = 0; break;
    case -6: sound = 2; break;
    case -5: sound = 1; break;
    case -4: // leave: 0x80056FF0, 0x80053684, the manager goes back
        exit = kLeave;
        sound = 4;
        result = 2;
        break;
    case -2: sound = 6; break;
    default: break;
    }
    if (result) { // 0x80053684: the page band closes, the page stops
        page_.band.anim = int16_t(~page_.band.steps);
        page_.open = -1;
    }
    if (sound >= 0) sounds.push_back(sound);
    return result;
}

int32_t ChangePartsView::PageUpdate(const MenuListPad* pad, bool input) { // 0x800536A4
    input_ = input;
    if (preview_.Step(c_)) {
        int rpm = 0, power = 0, torque = 0;
        preview_.Maxima(rpm, power, torque);
        graph_.Open(rpm, power, torque); // 0x8005C44C.. then 0x80073CFC
    }
    graph_.Tick();
    page_.band.Tick();
    if (page_.scroll < 0) page_.scroll++;
    if (page_.scroll > 0) page_.scroll--;
    if (++page_.flash > 0x3C) page_.flash = 0;
    if (++page_.arrowPhase > 0x2D) page_.arrowPhase = 0;
    listBand_.Tick();
    if (page_.open < 0) {
        MenuListUpdate(list_, nullptr);
        return -1;
    }
    namespace pb = menu_list_pad;
    auto enterPart = [&](int part) { // LAB_80053c6c
        SelectPart(part);
    };
    switch (page_.state) {
    case 0: {
        MenuListUpdate(list_, nullptr);
        if (!pad) return -1;
        const uint32_t pressed = pad->pressed;
        if (pressed & pb::kL1) {
            input_ = false;
            return -8;
        }
        if (pressed & (pb::kTriangle | pb::kSquare)) {
            input_ = false;
            return -4;
        }
        if ((pressed & (pb::kCross | pb::kCircle | pb::kRight)) == 0) {
            const uint32_t bits = pressed | pad->repeat;
            const int g0 = page_.group;
            int g = g0;
            if (bits & pb::kUp) {
                g = g0 - 1;
                if (g < 0) g = 0;
                if (g != g0) page_.scroll = -6;
            }
            if (bits & pb::kDown) {
                g = g + 1;
                if (page_.groupCount <= g) g = page_.groupCount - 1;
                if (g == page_.group) return -1;
                page_.scroll = 6;
            }
            if (g != page_.group) {
                SelectGroup(g);
                return -2;
            }
            return -1;
        }
        group_ = page_.group;
        page_.state = 1;
        enterPart(0);
        return -5;
    }
    case 1: {
        MenuListUpdate(list_, nullptr);
        if (!pad) return -1;
        const uint32_t pressed = pad->pressed;
        if (pressed & pb::kL1) {
            input_ = false;
            return -8;
        }
        if (pressed & (pb::kTriangle | pb::kSquare | pb::kLeft)) {
            page_.state = 0;
            return -6;
        }
        if ((pressed & (pb::kCross | pb::kCircle)) == 0) {
            int p = page_.part;
            const uint32_t bits = pressed | pad->repeat;
            if ((bits & pb::kUp) && --p < 0) p = 0;
            const int count = PartCount(GroupRecord(group_));
            if ((bits & pb::kDown) && ++p >= count) p = count - 1;
            if (p != page_.part) {
                SelectPart(p);
                return -2;
            }
            return -1;
        }
        part_ = page_.part;
        const uint32_t record = GroupRecord(group_);
        const uint32_t entry = record + uint32_t(part_) * 0x10;
        const int16_t kind = a_.ovl0.Get<int16_t>(entry + 0x1C);
        const int stages = StageCount(record, part_);
        const uint8_t flags = a_.ovl0.Get<uint8_t>(entry + 0x1F);
        graphPart_ = (flags >> 7) != 0;
        if (flags & 0x40) return -7; // a sub-part: no stage list
        const int rule = (c_.restrictions >> 1) & 3;
        if ((rule == 1 && uint32_t(kind - 15) <= 1) || (rule == 2 && kind == 13)) return -7;
        listBand_.anim = 0;
        list_.count = int16_t(stages);
        list_.selection = c_.sheet->stage[kind];
        MenuListOpen(list_); // 0x8006CE70
        if (graphPart_) preview_.Queue(stages, kind); // the pairs {kind, i} at page + 0x2A54, 0x80057654
        page_.flash = 0;
        page_.state = 2;
        return -5;
    }
    case 2: {
        const int32_t r = MenuListUpdate(list_, pad);
        if (r == -3) {
            page_.flash = 0;
            return -2;
        }
        if (r == -4) return -7;
        if (r == -2) return -1;
        int32_t code = -5;
        if (r >= 0) { // 0x80052928 = 0x8005EAC0(sheet, kind, row)
            const int16_t kind = a_.ovl0.Get<int16_t>(GroupRecord(group_) + uint32_t(part_) * 0x10 + 0x1C);
            career::SetTuneStage(*c_.sheet, kind, r, *c_.data);
            changes++;
        } else if (r == -1) {
            code = -6;
        } else {
            return -1;
        }
        page_.flash = 0;
        listBand_.anim = int16_t(~listBand_.steps); // ~0x8005C414
        MenuListClose(list_);
        graph_.Close();
        preview_.Reset();
        page_.state = 1;
        SelectPart(page_.part);
        return code;
    }
    default:
        return -1;
    }
}

int32_t ChangePartsView::ListCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) { // 0x80052D84
    const GuestImage& o = a_.ovl0;
    const uint32_t record = GroupRecord(group_);
    const uint32_t groupColour = o.Get<uint32_t>(record + 8);
    const uint32_t stageEntry = o.Get<uint32_t>(record + uint32_t(part_) * 0x10 + 0x18) + uint32_t(row) * 12;
    const int16_t kind = o.Get<int16_t>(record + uint32_t(part_) * 0x10 + 0x1C);
    switch (command) {
    case kMenuListDraw: {
        if (!d || !d->ot) return 0;
        MenuOtSlot& ot = *d->ot;
        const HudFont& medium = a_.FontAt(kFontMedium);
        if (w.state >= 0 && row == w.selection && input_) // US: the stage's description
            CentredText(ot, a_.FontAt(kFontLarge), a_.Text(o.Get<uint32_t>(stageEntry + 4)), 0xB0, 0x1BC, 1, Lerp(o, kColourBase, kColourStage, ListFade(), 0x80));
        if (w.state == -1) return 0;
        const int a = ListFade();
        if (a <= 0) return 0;
        const bool exists = SheetSlotRow(*c_.sheet, kind, row) >= 0;
        int x = d->x - 0x30;
        const int y = d->y + 8;
        if (w.state >= 0) x -= a - 0x80;
        if (d->drawn == 0) { // the part's picture and its frame
            const int y0 = w.y;
            const uint32_t pic = kPictures + uint32_t(o.Get<int8_t>(record + uint32_t(part_) * 0x10 + 0x1E)) * 12;
            const MenuPrim p = Sprite(o, pic, x - 0x2C, y0, uint32_t(a) * 0x010101u | 0x2000000u, 0x220);
            ot.Add(p);
            ot.DrawMode(p.tpage);
            // 0x8006B548 of the group colour
            const uint32_t frame = MenuListLerp(o.Get<uint32_t>(kColourBase), groupColour, a, 0x100);
            Polyline(ot, frame, {{{x - 0x2D, y0 - 1}, {x - 6, y0 - 1}, {x - 6, y0 + 0x42}, {x - 0x2D, y0 + 0x42}, {x - 0x2D, y0 - 1}}});
            ot.DrawMode(0x220);
        }
        const int rowAlpha = (a * d->alpha) >> 7;
        uint32_t textTarget = o.Get<uint32_t>(kColourStage);
        if (!d->enabled) textTarget = o.Get<uint32_t>(kColourDisabled);
        if (!exists) textTarget = o.Get<uint32_t>(kColourNoRow);
        uint32_t tile = MenuListLerp(o.Get<uint32_t>(kColourBase), groupColour, rowAlpha, 0x300);
        const uint32_t frame = MenuListLerp(o.Get<uint32_t>(kColourBase), groupColour, rowAlpha, 0x180);
        if (row == w.selection) {
            int e = 0x18 - page_.flash;
            if (e < 0) e = 0;
            tile = MenuListLerp(o.Get<uint32_t>(kColourBase), groupColour, rowAlpha, 0x180);
            tile = MenuListLerp(tile, o.Get<uint32_t>(kColourFlash), e, 0x18);
        }
        Text(ot, medium, a_.Text(o.Get<uint32_t>(stageEntry)), x + 4, y, 1, MenuListLerp(o.Get<uint32_t>(kColourBase), textTarget, rowAlpha, 0x80));
        ot.Add(Tile(x + 2, y - 0x14, 0x5D, 0x17, tile));
        Polyline(ot, frame, {{{x, y - 0x16}, {x + 0x60, y - 0x16}, {x + 0x60, y + 4}, {x, y + 4}, {x, y - 0x16}}});
        ot.DrawMode(0x220);
        ot.Add(Tile(x, y - 0x16, 0x60, 0x1A, 0x02000000u));
        ot.DrawMode(0x200);
        return 0;
    }
    case kMenuListEnter:
    case kMenuListOpen:
        partText_ = o.Get<uint32_t>(stageEntry + 4); // 0x800528B0
        return 0;
    case kMenuListEnabled: {
        if (w.state < 0) return 0;
        int32_t enabled = SheetSlotRow(*c_.sheet, kind, row) >= 0 ? 1 : 0;
        if (row > 0) { // 0x8005E874 on the race block's garage car
            if (!c_.garageCar || !career::PartOwned(*c_.garageCar, o.Get<int16_t>(stageEntry + 8))) enabled = 0;
        }
        if (c_.powerLimit > 0 && graphPart_) { // 0x80057A28 of the stage's preview
            const int32_t power = preview_.done[size_t(row) + 1] ? int32_t(preview_.slots[size_t(row) + 1].Figure(0)) : -1;
            if (power < 0) enabled = 0;
            if (c_.powerLimit < power) enabled = 0;
        }
        if (uint32_t(kind - 3) < 2) { // tyres: the course's surface
            if ((c_.restrictions & 1) == 0) {
                if (row == 7) return enabled;
                enabled = 0;
            } else {
                if (row != 7) return enabled;
                enabled = 0;
            }
        }
        return enabled;
    }
    default:
        return 0;
    }
}

void ChangePartsView::DrawParts(MenuOt& ot, uint32_t record, int selected, int x, int y, int alpha, int flash) const { // 0x800529C0
    const GuestImage& o = a_.ovl0;
    const int count = PartCount(record);
    const int sixth = alpha / 6;
    const uint32_t grey = uint32_t(sixth) | uint32_t(sixth) << 8 | uint32_t(sixth) << 16;
    for (int i = 0; i < count; i++, x += 4, y += 0x20) {
        const uint32_t part = record + 0x10 + uint32_t(i) * 0x10;
        const int16_t kind = o.Get<int16_t>(part + 0xC);
        const int16_t stage = c_.sheet->stage[kind];
        uint32_t base = 0x0278500Au;
        int yy = y;
        if (o.Get<uint8_t>(part + 0xF) & 0x40) base = 0x020C3060u, yy += 8;
        Text(ot[4], a_.FontAt(kFontMedium), a_.Text(o.Get<uint32_t>(part)), x + 4, yy, 1, MenuListLerp(o.Get<uint32_t>(kRowBase), base, alpha, 0x80));
        Text(ot[4], a_.FontAt(kFontLarge), a_.Text(o.Get<uint32_t>(o.Get<uint32_t>(part + 8) + uint32_t(stage) * 12)), x + 0x94, yy, 1,
             Lerp(o, kRowBase, kColourStage, alpha, 0x80));
        uint32_t colour = grey;
        if (i == selected) {
            int e = 8 - flash;
            if (e < 0) e = 0;
            colour = MenuListLerp(MenuListLerp(o.Get<uint32_t>(kColourBase), o.Get<uint32_t>(record + 8), alpha, 0x180), o.Get<uint32_t>(kColourFlash), e, 8);
        }
        MenuPrim p = Sprite(o, kRowSprite, x - 8, yy - 0x18, colour, 0x220);
        ot[4].Add(p);
        ot[4].DrawMode(p.tpage);
    }
}

void ChangePartsView::Draw(MenuOt& ot) const { // 0x80053CA8(page, ot + 4 entries): param_2 = ot[4], param_2 + 4 = ot[5]
    const GuestImage& o = a_.ovl0;
    drawOt_ = &ot;
    const int groupShown = page_.group;
    const uint32_t record = GroupRecord(groupShown);
    const int x = page_.x;
    int iy = page_.y + page_.scroll * 10 - 0x1C;
    MenuOtSlot& s4 = ot[4];
    MenuOtSlot& s5 = ot[5];
    listBand_.Draw(s4, 0, 0xD2);
    s4.DrawMode(0x220);
    MenuListDraw(list_, s4);
    if (listBand_.anim >= 0) { // the entered part's name over the stage list's band
        const int a = BandAlpha(listBand_);
        CentredText(s4, a_.FontAt(kFontLarge), a_.Text(o.Get<uint32_t>(record + 0x10 + uint32_t(part_) * 0x10)), 0xB0, 0xEC, 1,
                    Lerp(o, kColourBase, kColourStage, a, 0x80));
    }
    if (preview_.done[0]) { // the current curves, faint
        DrawPowerGraphCurve(graph_, s4, preview_.slots[0].power.data(), 0, 0x30);
        DrawPowerGraphCurve(graph_, s4, preview_.slots[0].torque.data(), 1, 0x30);
    }
    if (page_.state == 2 && preview_.done[size_t(list_.selection) + 1]) { // the selected stage's curves and figures
        const PartsPreview::Slot& s = preview_.slots[size_t(list_.selection) + 1];
        DrawPowerGraphCurve(graph_, s4, s.power.data(), 0, 0x80);
        DrawPowerGraphCurve(graph_, s4, s.torque.data(), 1, 0x80);
        const uint32_t colour = Lerp(o, kColourBase, kColourFigures, ListFade(), 0x80);
        const HudFont& small = a_.FontAt(kFontSmall);
        std::vector<HudFontSprite> g;
        small.Number(Format(a_.Text(kStrPowerFigures), s.Figure(0), s.Figure(2)), 0x78, 0x186, 1, -1, 0, g);
        AddGlyphs(s4, g, colour);
        g.clear();
        const int tq = s.Figure(4);
        small.Number(Format(a_.Text(kStrTorqueFigures), tq / 10, tq % 10, s.Figure(6)), 0x78, 0x19A, 1, -1, 0, g);
        AddGlyphs(s4, g, colour);
    }
    DrawPowerGraphAxes(graph_, s4, graphText_);

    int k = (page_.scroll << 7) / 6;
    if (k < 0) k = -k;
    const int bandAlpha = BandAlpha(page_.band);
    shell::Band band = page_.band;
    band.c0 = MenuListLerp(page_.previousColour, page_.colour, 0x80 - k, 0x80); // page + 0x1C
    if (page_.band.anim == -1) {
        drawOt_ = nullptr;
        return;
    }
    // the group icons: the current one and its neighbours, a scrolling one fading in / out
    int first = page_.group - 1, count = 4, leaving = -1, entering = -1;
    if (page_.scroll < 0) {
        entering = page_.group + 3;
        count++;
        leaving = first;
    }
    if (page_.scroll > 0) {
        first = page_.group - 2;
        leaving = page_.group + 2;
        iy -= 0x3C;
        count++;
        entering = first;
    }
    for (int g = first; g < first + count; g++, iy += 0x3C) {
        if (g < 0 || g >= page_.groupCount) continue;
        const uint32_t e = kIcons + uint32_t(o.Get<int8_t>(GroupRecord(g) + 0xC)) * 12;
        int grey = bandAlpha;
        if (g == leaving) grey = (bandAlpha * (0x80 - k)) >> 7;
        if (g == entering) grey = (bandAlpha * k) >> 7;
        if (g != page_.group) grey >>= 2;
        const int w = o.Get<int16_t>(e + 4), h = o.Get<int16_t>(e + 6);
        MenuPrim p = Sprite(o, e, x - (w >> 1) + 0x2A, iy - (h >> 1), uint32_t(grey) * 0x010101u, 0x20);
        s5.Add(p);
        s5.DrawMode(p.tpage);
    }
    if (page_.state >= 0 && page_.state < 2 && input_) {
        std::vector<HudFontSprite> g;
        const int w = a_.FontAt(kFontMedium).TextRight(a_.Text(kStrPartsSetting), 0x14C, 0x68, 0, g);
        AddGlyphs(s4, g, 0x02503C28u);
        SideArrow(s4, 0x149 - w, 0x5F, -5, 10, page_.arrowPhase);
    }
    {
        int t = bandAlpha * 0x60;
        if (t < 0) t += 0x7F;
        const uint32_t grey = uint32_t(t >> 7);
        Text(s4, a_.FontAt(kFontLarge), a_.Text(o.Get<uint32_t>(record)), x + 0x44, page_.y + 0x1E, 1, grey | grey << 8 | grey << 16 | 0x2000000u);
    }
    s5.Add(Tile(x + 0x1A, page_.y + 4, 0x20, 0x38, 0));
    s5.DrawMode(0x200);
    band.Draw(s5, x, page_.y);
    s5.DrawMode(0x200);
    int selected = -1, flash = page_.flash, alpha = bandAlpha;
    if (page_.state == 1) {
        selected = page_.part;
        if (input_) CentredText(s4, a_.FontAt(kFontLarge), a_.Text(partText_), 0xB0, 0x1BC, 1, kDescriptionColour);
    } else if (page_.state == 0) {
        if (page_.group > 0) MenuListArrow(s4, x + 0x2A, page_.y + 2, 6, -10, page_.arrowPhase);
        if (page_.group < page_.groupCount - 1) MenuListArrow(s4, x + 0x2A, page_.y + 0x3E, 6, 10, page_.arrowPhase);
    } else if (page_.state == 2) {
        if (c_.powerLimit > 0) {
            std::vector<HudFontSprite> g;
            a_.FontAt(kFontMedium).NumberRight(Format(a_.Text(kStrRestricted), (c_.powerLimit * 1000) / 0x3F6), 0x140, 0xCA, 1, -3, 0, g);
            AddGlyphs(s4, g, Lerp(o, kColourBase, kColourRestricted, bandAlpha, 0x80));
        }
        selected = page_.part;
        int f = ListFade() * 100;
        if (f < 0) f += 0x7F;
        flash = 8;
        alpha = bandAlpha - (f >> 7);
    }
    DrawParts(ot, record, selected, x + 0x48, page_.y + 100, alpha, flash);
    drawOt_ = nullptr;
}

// ---- PARTS SETTING as a view (0x8005D1E4)

PartsSettingView::PartsSettingView(const RaceMenuAssets& a, const ChangePartsContext& c) : a_(a), c_(c) {
    if (!c_.sheet || !c_.data) throw std::invalid_argument("PARTS SETTING: no sheet / career data");
    page_.Init(a_.ovl0, *c_.sheet, *c_.data); // 0x8005747C: 0x80055E90(P, 0, 100)
}

std::string PartsSettingView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t PartsSettingView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

int PartsSettingView::Update(const MenuListPad* pad, bool input) { // 0x800574C0
    sounds.clear();
    if (viewDelay > 0 && --viewDelay == 0) page_.StartOpen(); // 0x80055FD0
    const career::TuneSheet before = *c_.sheet;
    const int code = page_.Update(input ? pad : nullptr, input, *c_.sheet, *c_.data); // 0x80056194
    sounds = page_.sounds; // the slider's 0x80060840(8)
    if (std::memcmp(&before, c_.sheet, sizeof before) != 0) changed = true;
    if (const int s = MachineSettingsPage::Sound(code); s >= 0) sounds.push_back(s); // the jump table 0x8005AB04
    if (code == MachineSettingsPage::kChangeParts || code == MachineSettingsPage::kLeave) {
        // -8: 0x80048374(M, 0x8005D1C0), 0x80056FF0, 0x80055FE0, return 6; -4: 0x80056FF0, 0x80055FE0, return 2.
        exit = code == MachineSettingsPage::kChangeParts ? kChangeParts : kLeave;
        page_.StartClose();
        return 1;
    }
    return 0;
}

void PartsSettingView::Draw(MenuOt& ot) const { page_.Draw(ot, a_, *c_.sheet, *c_.data); } // 0x800575F8

} // namespace gt2::screens
