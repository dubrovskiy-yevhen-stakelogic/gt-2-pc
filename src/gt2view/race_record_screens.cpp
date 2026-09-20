#include "gt2view/race_record_screens.h"

#include <algorithm>
#include <cstdio>

#include "game/career/results.h"
#include "game/shell/title_draw.h"

namespace gt2::screens {

namespace {

// ovl0 addresses (US v1.2).
constexpr uint32_t kColourBase = 0x8005B1C4u;      // 0x02000000: the lerp base of the licence views' colours
constexpr uint32_t kColourLabel = 0x8005B1CCu;     // 0x02505028 ("B-1")
constexpr uint32_t kColourName = 0x8005B1DCu;      // the name, its marker and "- No Records -"
constexpr uint32_t kColourRank = 0x8005B1E0u;      // "1." .. "5."
constexpr uint32_t kColourBar = 0x8005B1E4u;       // the rows' rounded bars
constexpr uint32_t kLicenceLabelFormats = 0x8005B220u; // "S-%d", "IA-%d", ... (data-global)
constexpr uint32_t kRankFormat = 0x8005A97Cu;      // "%d."
constexpr uint32_t kTimeColourBase = 0x8005AB58u;  // 0x800495E4's lerp base
constexpr uint32_t kStrNoRecords = 0x801C7016u;    // "- No Records -" (data-race)
constexpr uint32_t kCapTable = 0x80091A78u;        // EXE: 13 {x, y} points of the rounded ends
// Fonts (race overlay descriptors, 0x80047EA0 state 1).
constexpr uint32_t kHeaderFont = 0x801C9130u, kMediumFont = 0x801C9150u, kSmallFont = 0x801C9120u;
// The colours 0x8004FC8C gives the five time columns of a loaded row (li constants in its code; the first, 0x02004650,
// is the colour of the optional label 0x800495E4 draws when the object's +0 is > 0 - never for the RECORD rows).
constexpr uint32_t kTimeColours[5] = {0x02505050u, 0x02505050u, 0x02505050u, 0x02505050u, 0x02003060u};

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

std::string Format(const std::string& format, int value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format.c_str(), value);
    return buffer;
}

uint32_t Lerp(const GuestImage& image, uint32_t a, uint32_t b, int t, int max) { return MenuListLerp(image.Get<uint32_t>(a), image.Get<uint32_t>(b), t, max); }

// 0x8006BEB4: a band's alpha, |anim| * 128 / steps (0 while closed).
int BandAlpha(const shell::Band& b) {
    int a = 0;
    if (b.anim >= 0) a = b.anim;
    else if (b.anim < -1) a = ~int(b.anim);
    return b.steps != 0 ? (a << 7) / b.steps : 0;
}

// 0x8006AE28 / 0x8006ADB4 over HudFont: centred text (x - width / 2).
int TextCentre(const HudFont& f, const std::string& s, int x, int y, int spacing, std::vector<HudFontSprite>& out) {
    const int w = f.TextWidth(s, spacing);
    return f.Text(s, x - (w >> 1), y, spacing, out);
}

} // namespace

// ---------------------------------------------------------------- shared

void AddRoundedBar(MenuOtSlot& ot, const GuestImage& exe, uint32_t colour, int x, int y, int w, int h, int factor) { // EXE 0x800683FC
    const int half = int(int16_t(w)) >> 1;
    const int right = x + half, left = x - half;
    ot.Add(Tile(left, y - (int(int16_t(h)) >> 1), w, h, colour));
    const int sx = h * factor;
    auto px = [&](int i) { return (int(exe.Get<int16_t>(kCapTable + uint32_t(i) * 4)) * sx) >> 17; };
    auto py = [&](int i) { return y + ((int(exe.Get<int16_t>(kCapTable + uint32_t(i) * 4 + 2)) * h) >> 12); };
    for (int i = 0; i < 12; i += 2) {
        MenuPrim r; // 0x8007D060: POLY_F4 0x28 ^ colour
        r.kind = MenuPrim::kPolyF4;
        r.semi = (colour & 0x2000000u) != 0;
        for (uint32_t& c : r.colour) c = colour & 0xFFFFFF;
        r.x[0] = int16_t(right + px(i)), r.y[0] = int16_t(py(i));
        r.x[1] = int16_t(right + px(i + 1)), r.y[1] = int16_t(py(i + 1));
        r.x[2] = int16_t(right), r.y[2] = int16_t(y);
        r.x[3] = int16_t(right + px(i + 2)), r.y[3] = int16_t(py(i + 2));
        ot.Add(r);
        MenuPrim l = r;
        l.x[3] = int16_t(left - px(i)), l.y[3] = int16_t(py(i));
        l.x[1] = int16_t(left - px(i + 1)), l.y[1] = int16_t(py(i + 1));
        l.x[0] = int16_t(left - px(i + 2)), l.y[0] = int16_t(py(i + 2));
        l.x[2] = int16_t(left), l.y[2] = int16_t(y);
        ot.Add(l);
    }
}

namespace {
class TitleCardKeyboard final : public shell::CardKeyboard {
public:
    static constexpr uint32_t kDescriptor = 0x800921A0u; // EXE (0x8007263C: 0x80073548(mgr + 0x6D8, 0x800921A0))
    explicit TitleCardKeyboard(const TitleAssets& a)
        : entry_(a.exe, a.exe, kDescriptor, [&a](uint32_t f) -> const HudFont& { return a.fonts[size_t(FontIndex(f))]; },
                            [&a](uint32_t t) { return a.Text(t); }) {
        entry_.maxLength = 0x1F; // + 0x1E
        entry_.widthLimit = 0x100; // + 0x20
        entry_.sound = [this](int id) {
            if (sounds_) sounds_->push_back(id);
        };
    }
    void Open(const std::string& name) override {
        entry_.name = name.substr(0, size_t(entry_.maxLength));
        entry_.Open();
        entry_.caret = int16_t(entry_.name.size()); // + 0x6F4 = strlen(+ 0x6FC)
    }
    void Close() override { entry_.Close(); }
    int Update(const MenuListPad* pad, std::vector<int>& sounds) override {
        sounds_ = &sounds;
        const int r = entry_.Update(pad);
        sounds_ = nullptr;
        return r;
    }
    void Draw(MenuOtSlot& ot) const override { entry_.Draw(ot); }
    std::string Name() const override { return entry_.name; }

private:
    // The manager's font pointers (0x8007284C from the view's object 0x8004B1A0: + 0xC tiny, + 0x10 small, + 0x14 medium).
    static int FontIndex(uint32_t descriptor) {
        switch (descriptor) {
        case 0x801C94D4u: return TitleAssets::kTinyFont;
        case 0x801C94B4u: return TitleAssets::kSmallFont;
        default: return TitleAssets::kMediumFont; // 0x801C94C4
        }
    }
    NameEntry entry_;
    std::vector<int>* sounds_ = nullptr;
};
} // namespace

std::unique_ptr<shell::CardKeyboard> MakeTitleCardKeyboard(const TitleAssets& assets) { return std::make_unique<TitleCardKeyboard>(assets); }

void MenuListSelect(MenuListWidget& w, int row) { // EXE 0x8006D400
    const int current = w.selection;
    if (!(row < w.count)) row = w.count - 1;
    if (row < 0) row = 0;
    if (row < current) w.scroll = -8;
    if (current < row) w.scroll = 8;
    w.blink = 0;
    if (row != current && w.callback) {
        w.callback(kMenuListLeave, w, current, nullptr);
        w.callback(kMenuListEnter, w, row, nullptr);
    }
    w.selection = int16_t(row);
}

// ---------------------------------------------------------------- keyboard widget

NameEntry::NameEntry(const RaceMenuAssets& a, uint32_t d)
    : NameEntry(a.exe, a.ovl0, d, [&a](uint32_t f) -> const HudFont& { return a.FontAt(f); }, [&a](uint32_t t) { return a.Text(t); }) {}

NameEntry::NameEntry(const GuestImage& exeImage, const GuestImage& o, uint32_t d, FontLookup fontAt, TextLookup text)
    : exe_(exeImage), fontAt_(std::move(fontAt)), text_(std::move(text)) { // EXE 0x80073548
    font = o.Get<uint32_t>(d + 0);
    cellWidth = o.Get<int8_t>(d + 4);
    cellHeight = o.Get<int8_t>(d + 5);
    cellGap = o.Get<int8_t>(d + 6);
    rowGap = o.Get<int8_t>(d + 7);
    page = o.Get<int16_t>(d + 8);
    glyphBaseline = o.Get<int16_t>(d + 0xA);
    spacing = o.Get<int16_t>(d + 0xC);
    centreX = o.Get<int16_t>(d + 0xE);
    nameY = o.Get<int16_t>(d + 0x10);
    anim = -1;
    row = column = 0;
    caret = caretPhase = pulse = 0;
    band = shell::Band::Read(exe_, kBandTemplate);
    band.anim = -1;
    list = MenuListWidget::Read(exe_, kListTemplate);
    MenuListReset(list, [this](int command, const MenuListWidget& w, int r, const MenuListRowDraw* draw) { return RowCallback(command, w, r, draw); });
    // The view's 0x8004E5E8: the name buffer, 11 characters, 256 pixels.
    maxLength = 11;
    widthLimit = 256;
}

void NameEntry::Open() { // EXE 0x8007364C
    row = 0;
    band.w = widthLimit;
    anim = 0;
    column = 0;
    caret = 0;
    caretPhase = 0;
    pulse = 0;
    band.anim = 0;
    list.selection = row;
    list.rowHeight = cellHeight;
    list.rowGap = rowGap;
    list.x = centreX;
    list.y = int16_t(nameY + cellHeight - 8);
    MenuListOpen(list);
}

void NameEntry::Close() { // EXE 0x800736E0
    band.anim = int16_t(~band.steps);
    MenuListClose(list);
    anim = -1;
}

int NameEntry::Width(const std::string& s) const { return fontAt_(font).TextWidth(s, spacing); } // 0x8006AD3C

int NameEntry::Update(const MenuListPad* pad) { // EXE 0x80073720
    MenuListUpdate(list, pad);
    band.Tick();
    if (anim < 0) {
        if (anim < -1) anim++;
        return kNothing;
    }
    if (++anim > 12) anim = 12;
    if (++pulse > 60) pulse = 0;
    if (++caretPhase > 40) caretPhase = 0;
    if (!pad) return kNothing;
    namespace pb = menu_list_pad;
    if ((pad->pressed | pad->repeat) & pb::kBack) { // triangle / square: delete the character before the caret
        if (caret > 0) {
            Sound(2);
            caret--;
            name.erase(size_t(caret), 1); // 0x80073524
        } else {
            Sound(0);
        }
        return kNothing;
    }
    if (pad->pressed & pb::kChoose) {
        if (row == kButtonRow) return column < 8 ? kCancel : kOk;
        if (!(int(name.size()) < maxLength)) {
            Sound(2);
            return kNothing;
        }
        const size_t at = std::min(size_t(std::max<int>(caret, 0)), name.size());
        name.insert(at, 1, char(exe_.Get<uint8_t>(kLayout + uint32_t(row) * kColumns + uint32_t(column))));
        if (!(Width(name) < widthLimit)) {
            name.erase(at, 1);
            Sound(0);
            return kNothing;
        }
        Sound(1);
        caret++;
        return kNothing;
    }
    if (pad->pressed & pb::kStart) { // to OK
        row = kButtonRow;
        MenuListSelect(list, kButtonRow);
        column = 8;
        Sound(5);
        return kMoved;
    }
    const uint32_t bits = pad->pressed | pad->repeat;
    if (bits & pb::kL1) {
        if (--caret < 0) caret = 0;
    }
    if (bits & pb::kR1) {
        caret++;
        if (int(name.size()) < caret) caret = int16_t(name.size());
    }
    row = int8_t(list.selection);
    if (bits & pb::kLeft) {
        if (--column < 0) column = kColumns - 1;
        if (row == kButtonRow) column = 7;
    }
    if (bits & pb::kRight) {
        if (++column >= kColumns) column = 0;
        if (row == kButtonRow) column = 8;
    }
    if (bits & (pb::kUp | pb::kDown | pb::kLeft | pb::kRight | pb::kL1 | pb::kR1)) {
        Sound(5);
        return kMoved;
    }
    return kNothing;
}

int32_t NameEntry::RowCallback(int command, const MenuListWidget& w, int r, const MenuListRowDraw* d) const { // EXE 0x8007306C
    if (command != kMenuListDraw || !d || !d->ot) return 1;
    if (w.state == -1) return 1;
    const GuestImage& exe = exe_;
    const HudFont& f = fontAt_(font);
    MenuOtSlot& ot = *d->ot;
    const int fade = (int(w.fade) << 7) / int(w.fadeMax);
    const int level = (fade * int(d->alpha)) >> 7;
    if (level <= 0) return 1;
    const uint32_t grey = uint32_t((level * 9) >> 4) * 0x010101u | 0x2000000u;
    const uint32_t base = MenuListLerp(exe.Get<uint32_t>(w.state >= 0 ? kColourCellOpening : kColourBase), exe.Get<uint32_t>(kColourCell), fade, 128);
    const uint32_t cell = MenuListLerp(exe.Get<uint32_t>(kColourBase), base, d->alpha, 128);
    const uint32_t c0 = uint32_t(level >> 1) | 0x2000000u, c1 = uint32_t(level) * 0x010101u | 0x2000000u;
    const int t = (int(pulse) << 7) / 60;
    const int pitch = cellWidth + cellGap;
    const int half = pitch * 8;
    int x = d->x - half + (pitch >> 1);
    const int top = d->y - (cellHeight >> 1);
    std::vector<HudFontSprite> glyphs;
    if (r == kButtonRow) { // CANCEL / OK
        x -= cellWidth >> 1;
        const int bw = half - cellGap;
        if (row == kButtonRow) {
            const int hx = column < 8 ? x + (bw >> 1) : x + bw + cellGap + (bw >> 1);
            MenuListHighlight(ot, hx, top, bw, cellHeight, c0, c1, t);
            ot.DrawMode(0x20);
        }
        TextCentre(f, text_(kStrCancel), x + (bw >> 1), top + glyphBaseline, spacing, glyphs);
        AddSprites(ot, glyphs, grey, 1);
        ot.Add(Tile(x, top, bw, cellHeight, cell));
        ot.DrawMode(0x220);
        x = d->x;
        glyphs.clear();
        TextCentre(f, text_(kStrOk), x + (bw >> 1), top + glyphBaseline, spacing, glyphs);
        AddSprites(ot, glyphs, grey, 1);
        ot.Add(Tile(x, top, bw, cellHeight, cell));
        ot.DrawMode(0x220);
        return 1;
    }
    for (int c = 0; c < kColumns; c++) {
        if (row == r && column == c) {
            MenuListHighlight(ot, x, top, cellWidth, cellHeight, c0, c1, t);
            ot.DrawMode(0x20);
        }
        glyphs.clear();
        f.Glyph(uint32_t(exe.Get<uint8_t>(kLayout + uint32_t(r) * kColumns + uint32_t(c))) | HudFont::kRightAlign, x, top + glyphBaseline, glyphs);
        AddSprites(ot, glyphs, grey, 1);
        ot.Add(Tile(x - (cellWidth >> 1), top, cellWidth, cellHeight, cell));
        ot.DrawMode(0x220);
        x += pitch;
    }
    return 1;
}

void NameEntry::Draw(MenuOtSlot& ot) const { // EXE 0x80073AFC
    MenuListDraw(list, ot);
    if (anim == -1) return;
    const int a = anim >= 0 ? anim : ~int(anim);
    const int alpha = (a * 80) / 12;
    const int left = centreX - (int(widthLimit) >> 1);
    const HudFont& f = fontAt_(font);
    const uint32_t colour = uint32_t(alpha >> 1) | uint32_t(alpha) << 8 | uint32_t(alpha) << 16 | 0x2000000u;
    for (int mode : {1, 2}) {
        std::vector<HudFontSprite> glyphs;
        f.Text(name, left, nameY, spacing, glyphs);
        AddSprites(ot, glyphs, colour, mode);
    }
    band.Draw(ot, left, nameY - 6);
    ot.DrawMode(0x220);
    const int caretX = left + Width(name.substr(0, size_t(std::clamp<int>(caret, 0, int(name.size())))));
    MenuListArrow(ot, caretX, nameY + 10, 4, -7, caretPhase);
    ot.DrawMode(0x20);
}

// ---------------------------------------------------------------- NEW RECORD

void NewRecordView::Enter(const std::string& lastName) { // ovl0 0x8004E5E8
    delay = 24;
    keyboard.name = lastName.substr(0, size_t(keyboard.maxLength));
}

bool NewRecordView::Update(const MenuListPad* pad) { // ovl0 0x8004E658
    if (delay > 0 && --delay == 0) {
        keyboard.Open();
        keyboard.caret = int16_t(keyboard.name.size());
    }
    const int result = keyboard.Update(pad);
    if (result == NameEntry::kCancel) {
        if (keyboard.sound) keyboard.sound(0);
        return false;
    }
    if (result != NameEntry::kOk) return false;
    keyboard.Close();
    if (keyboard.sound) keyboard.sound(3);
    return true;
}

std::vector<MenuPrim> BuildNewRecordFrame(const RaceMenuAssets& a, const NameEntry& keyboard, int headerAlpha) {
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    AddRaceViewHeader(ot, a, a.Text(a.ovl0.Get<uint32_t>(NewRecordView::kView + 0x10)), a.ovl0.Get<uint32_t>(NewRecordView::kView + 0x0C), headerAlpha);
    keyboard.Draw(ot[0]); // 0x8004E7D8
    ot.Emit(prims, 0x200);
    return prims;
}

// ---------------------------------------------------------------- RECORD

void RecordsView::Enter(int l, int t) { // ovl0 0x8004FDC4
    licence = l;
    test = t;
    delay = 12;
    slide = -1;
    arrowPhase = 0;
    active = true;
    band = shell::Band::Read(assets_.ovl0, kBandTemplate);
    band.anim = 0;
}

bool RecordsView::Update(const MenuListPad* pad, bool isActive) { // ovl0 0x8004FE40
    if (++arrowPhase > 45) arrowPhase = 0;
    active = isActive;
    bool input = true;
    if (delay > 0) {
        input = false;
        if (--delay == 0) slide = 0; // 0x8004FC8C: the rows load
    }
    if (slide >= 0 && ++slide > 128) slide = 128;
    band.Tick();
    if (!(isActive && input) || !pad) return false;
    const uint32_t bits = pad->pressed | pad->repeat;
    namespace pb = menu_list_pad;
    if (bits & pb::kLeft) {
        if (sound) sound(7);
        slide = 0;
        if (--test < 0) test = 9;
    }
    if (bits & pb::kRight) {
        if (sound) sound(7);
        slide = 0;
        if (++test >= 10) test = 0;
    }
    if (pad->pressed & (pb::kTriangle | pb::kCross | pb::kSquare | pb::kCircle)) {
        active = false;
        if (sound) sound(3);
        slide = -1;
        band.anim = int16_t(~band.steps);
        return true;
    }
    return false;
}

void LoadRecordRows(RecordsState& s, const career::LicenceTestRecord& record) { // ovl0 0x8004FC8C
    s.count = 0;
    for (int i = 0; i < 5; i++) {
        s.times[size_t(i)] = record.times[i];
        if (record.times[i].time[0] != -1) s.count++;
        std::string name;
        for (size_t k = 0; k < 0x0C && record.entries[i][k] != 0; k++) name.push_back(char(record.entries[i][k]));
        s.names[size_t(i)] = name;
    }
}

RecordsState RecordsView::State(const career::CareerState& career) const {
    RecordsState s;
    s.licence = licence;
    s.test = test;
    s.title = assets_.Licence(licence, test).title;
    if (licence >= 0 && licence < int(career::kLicenceCount) && test >= 0 && test < int(career::kLicenceTests))
        LoadRecordRows(s, career.licences[licence][test]);
    s.slide = slide;
    s.active = active;
    s.arrowPhase = arrowPhase;
    s.band = band;
    return s;
}

std::vector<MenuPrim> BuildRecordsFrame(const RaceMenuAssets& a, const RecordsState& st) {
    const GuestImage& o = a.ovl0;
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    MenuOt ot;
    AddRaceViewHeader(ot, a, a.Text(o.Get<uint32_t>(RecordsView::kView + 0x10)), o.Get<uint32_t>(RecordsView::kView + 0x0C), st.headerAlpha);
    std::vector<HudFontSprite> glyphs;
    MenuOtSlot& s0 = ot[0];

    // 0x80050084.
    if (st.slide >= 0) {
        // 0x8004FB30(ot, 16, 176, slide): five rows, 4 pixels right and 54 lower each, each sliding in from x + 128
        // over 8 steps, 4 steps after the previous one.
        for (int i = 0, x = 16, y = 176; i < 5; i++, x += 4, y += 54) {
            int v = st.slide - 4 * i;
            if (v < 0) continue;
            if (v > 8) v = 8;
            const int alpha = (v << 7) >> 3;
            const int rx = x + 128 - alpha;
            // 0x8004F8B4(ot, row object or 0, name, rank, rx, y, alpha).
            glyphs.clear();
            a.FontAt(kMediumFont).Number(Format(a.Text(kRankFormat), i + 1), rx, y + 12, 1, -2, 0, glyphs);
            AddSprites(s0, glyphs, Lerp(o, kColourBase, kColourRank, alpha, 128), 1);
            const uint32_t nameColour = Lerp(o, kColourBase, kColourName, alpha, 128);
            const HudFont& small = a.FontAt(kSmallFont);
            if (i >= st.count) {
                glyphs.clear();
                TextCentre(small, a.Text(kStrNoRecords), rx + 151, y + 16, 2, glyphs);
                AddSprites(s0, glyphs, nameColour, 1);
            } else {
                // 0x800495E4(object, ot, ctx, rx, y, alpha): four sector times (0x8005DD94) and the total, right-aligned
                // every 60 pixels.
                const career::TimeRecord& t = st.times[size_t(i)];
                for (int c = 0, right = rx + 60; c < 5; c++, right += 60) {
                    const int32_t value = c < 4 ? career::TimeRecordSector(t, c) : t.time[0];
                    glyphs.clear();
                    small.TimeRight(FormatRaceTime(uint32_t(value)), right, y, 6, 5, 0, 0, glyphs);
                    AddSprites(s0, glyphs, MenuListLerp(o.Get<uint32_t>(kTimeColourBase), kTimeColours[c], alpha, 128), 1);
                }
                glyphs.clear();
                const int w = small.TextRight(st.names[size_t(i)], rx + 302, y + 22, 2, glyphs);
                AddSprites(s0, glyphs, nameColour, 1);
                s0.Add(Tile(rx + 296 - w, y + 10, 4, 6, nameColour));
                s0.DrawMode(0x220);
            }
            AddRoundedBar(s0, a.exe, Lerp(o, kColourBase, kColourBar, alpha, 128), rx + 184, y, 352, 48, 24);
            s0.DrawMode(0);
        }
        if (st.active) { // 0x8004CE94 (Japanese only) and 0x8004CFC0(ot, 94, 128, 128): the test's title
            glyphs.clear();
            a.FontAt(kSmallFont).Text(st.title, 94, 128, 1, glyphs);
            AddSprites(s0, glyphs, 0x808080, 0);
        }
    }
    // 0x8004CB6C(ot, 20, 130, licence, test, band alpha, arrow phase): the licence label with its arrows.
    {
        const HudFont& big = a.FontAt(kHeaderFont);
        glyphs.clear();
        const std::string label = Format(a.Text(o.Get<uint32_t>(kLicenceLabelFormats + uint32_t(st.licence) * 4)), st.test + 1);
        const int w = big.Number(label, 20, 130, 2, 1, 4, glyphs);
        AddSprites(s0, glyphs, Lerp(o, kColourBase, kColourLabel, BandAlpha(st.band), 128), 1);
        if (st.arrowPhase >= 0) {
            AddSideArrow(s0, 20 + w + 5, 130 - 22, 6, 10, st.arrowPhase);
            AddSideArrow(s0, 20 - 4, 130 - 22, -6, 10, st.arrowPhase);
        }
        s0.DrawMode(0x220);
    }
    st.band.Draw(ot[2], 0, 102);
    ot[2].DrawMode(0x220);

    ot.Emit(prims, 0x200);
    return prims;
}

} // namespace gt2::screens
