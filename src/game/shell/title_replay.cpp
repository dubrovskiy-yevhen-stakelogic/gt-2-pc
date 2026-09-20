#include "game/shell/title_replay.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/shell/title_draw.h"
#include "game/shell/title_screens.h"
#include "gt2vfs/gtfs.h"

namespace gt2::shell {

namespace {

// Strings of data-global.txd (the 0x801EF6B0 block).
constexpr uint32_t kSectorFormat = 0x801EFCCDu, kNewFile = 0x801EFCDCu, kExitRow = 0x801EFCF0u, kOkRow = 0x801EFCFCu;
constexpr uint32_t kGhost = 0x801EFD16u, kRace = 0x801EFD26u, kTimeTrial = 0x801EFD2Eu, kLicense = 0x801EFD3Fu, kTwoPlayer = 0x801EFD4Cu,
                   kTestRun = 0x801EFDEEu, k400m = 0x801EFE0Eu, k1000m = 0x801EFE18u, kMaxSpeed = 0x801EFE23u;
constexpr uint32_t kLicenceFormats = 0x80091E80u; // 6 pointers: "S-%d", "IA-%d", "IB-%d", "IC-%d", "A-%d", "B-%d"
// Colours of the row printer (EXE 0x80091E64..; 0x80091E7C = 0 is the target of every fade).
constexpr uint32_t kRuleColour = 0x028E8452u, kNewFileColour = 0x02566024u, kTitleColour = 0x02565656u, kTitleBand = 0x0214377Au,
                   kSectorColour = 0x02381A10u, kMarkerColour = 0x02102E4Cu;

std::string Format(const std::string& format, int value) {
    const size_t at = format.find("%d");
    if (at == std::string::npos) return format;
    return format.substr(0, at) + std::to_string(value) + format.substr(at + 2);
}

// 0x8006B77C: POLY_G4 0x38 (| the semi-transparency bit 25 of c0) (x, y, w, h), c0 on the left, c1 on the right.
void AddOpaqueGradient(MenuOtSlot& ot, int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
    MenuPrim g;
    g.kind = MenuPrim::kPolyG4;
    g.gouraud = true;
    g.semi = (c0 & 0x02000000u) != 0;
    g.x[0] = int16_t(x), g.y[0] = int16_t(y);
    g.x[1] = int16_t(x + w), g.y[1] = int16_t(y);
    g.x[2] = int16_t(x), g.y[2] = int16_t(y + h);
    g.x[3] = int16_t(x + w), g.y[3] = int16_t(y + h);
    g.colour[0] = g.colour[2] = c0 & 0xFFFFFF;
    g.colour[1] = g.colour[3] = c1 & 0xFFFFFF;
    ot.Add(g);
}

// 0x8007D024: a TILE of the colour (semi-transparent with its bit 25).
void AddTile(MenuOtSlot& ot, int x, int y, int w, int h, uint32_t colour) {
    MenuPrim t;
    t.kind = MenuPrim::kTile;
    t.x[0] = int16_t(x), t.y[0] = int16_t(y), t.w = int16_t(w), t.h = int16_t(h);
    t.colour[0] = colour & 0xFFFFFF;
    t.semi = (colour & 0x02000000u) != 0;
    ot.Add(t);
}

} // namespace

ReplayRowText ReplayRowText::Load(const GtfsVolume& vol, const TitleAssets& assets) {
    ReplayRowText t;
    t.courses = ParseCourseInfo(vol.Read(".crsinfo"));
    const uint8_t* e = assets.exe.At(assets.exe.Sim(0x8008FA74u), 4);
    t.ellipsis = std::string(reinterpret_cast<const char*>(e), strnlen(reinterpret_cast<const char*>(e), 4));
    return t;
}

void DrawReplayRow(MenuOtSlot& ot, const TitleAssets& assets, const ReplayRowText& text, const ReplayRowStyle& style, const ReplayCardEntry* entry, int kind, int x, int y,
                   int alpha) {
    int t = 0x80 - alpha;
    if (t < 0) t = 0;
    const HudFont& titleFont = assets.fonts[size_t(style.titleFont)];
    const HudFont& detailFont = assets.fonts[size_t(style.detailFont)];
    const uint32_t rule = MenuListLerp(kRuleColour, 0, t, 0x80);
    const uint32_t marker = MenuListLerp(kMarkerColour, 0, t, 0x80);
    // 0x8006A3FC: a detail text behind its marker (the text cut to `maxWidth` by 0x8006AE98 and "..." appended)
    auto marked = [&](std::string s, int maxWidth, int mx, int my) {
        int w = 0;
        for (size_t i = 0; i < s.size(); i++) {
            w += detailFont.Advance(uint8_t(s[i]), i + 1 < s.size() ? uint8_t(s[i + 1]) : 0) + style.detailSpacing;
            if (maxWidth < w) {
                s = s.substr(0, i) + text.ellipsis;
                break;
            }
        }
        AddNumberText(ot, detailFont, s, mx + 5, my, style.detailSpacing, -2, 0, MenuListLerp(marker, 0, t, 0x80), 1);
        AddTile(ot, mx, my - 0xC, 4, 6, marker);
        ot.DrawMode(0x220);
    };
    if (kind == kReplayRowEntry && entry) {
        AddText(ot, titleFont, entry->Title(), x - 0x80, y - 10, style.titleSpacing, MenuListLerp(kTitleColour, 0, t, 0x80), 1);
        AddOpaqueGradient(ot, x - 0x80, y - 0x12, 0x100, 8, MenuListLerp(kTitleBand, 0, t, 0x80), 0);
        ot.DrawMode(0x200);
        AddGradientQuad(ot, x - 0x80, y - 6, 0x100, 1, rule, rule);
        AddGradientQuad(ot, x - 0x80, y + 0xE, 0x100, 1, rule, rule);
        AddGradientQuad(ot, x - 0x80, y + 0x26, 0x100, 1, rule, rule);
        ot.DrawMode(0x200);
        const std::string sectors = Format(assets.Text(kSectorFormat), entry->sectors);
        const int w = detailFont.NumberWidth(sectors, style.detailSpacing, 0);
        AddNumberText(ot, detailFont, sectors, x - (w - 0x7C), y + 0x26, style.detailSpacing, -2, 0, MenuListLerp(kSectorColour, 0, t, 0x80), 1);
        uint32_t kindText = kGhost;
        if (!entry->Ghost()) {
            switch (entry->GameMode()) {
            case 0: kindText = kTwoPlayer; break;
            case 1:
            case 10: kindText = kTestRun; break;
            case 3: kindText = kLicense; break;
            case 6: kindText = kTimeTrial; break;
            case 7: kindText = k400m; break;
            case 8: kindText = k1000m; break;
            case 9: kindText = kMaxSpeed; break;
            default: kindText = kRace; break;
            }
        }
        marked(assets.Text(kindText), 0x48, x - 0x7A, y + 0xE);
        std::string course;
        if (entry->GameMode() == 3) { // the licence and test packed into +0x44 (0x801D589C of a licence race)
            uint32_t licence = entry->CourseId() >> 16;
            if (licence > 5) licence = 0;
            course = Format(assets.Text(assets.exe.Get<uint32_t>(assets.exe.Sim(kLicenceFormats) + licence * 4)), int(entry->CourseId() & 0xFF));
        } else { // 0x80060EB4 / 0x80060E94
            size_t index = 0;
            for (size_t i = 0; i < text.courses.entries.size(); i++)
                if (text.courses.entries[i].fileId == entry->CourseId()) {
                    index = i;
                    break;
                }
            if (index < text.courses.entries.size()) course = text.courses.entries[index].name;
        }
        marked(course, 0xAC, x - 0x7A, y + 0x26);
        marked(entry->CarName(), 0x94, x - 0x20, y + 0xE);
    } else if (kind >= kReplayRowNewFile && kind <= kReplayRowExit) {
        const uint32_t label = kind == kReplayRowNewFile ? kNewFile : kind == kReplayRowOk ? kOkRow : kExitRow;
        AddText(ot, titleFont, assets.Text(label), x, y - 10, style.titleSpacing, MenuListLerp(kNewFileColour, 0, t, 0x80), 1, TextAlign::kCentre);
    }
    const uint32_t background = MenuListLerp(style.rowColour, 0, t, 0x80);
    AddOpaqueGradient(ot, x - 0x90, y - 0x2A, 0x120, 0x54, background, MenuListLerp(background, 0, 0x60, 0x80));
    ot.DrawMode(0x220);
}

bool ReplayRowPlacement(const MenuListWidget& w, const MenuListRowDraw& draw, int& x, int& alpha) {
    if (w.state == -1 || w.fadeMax == 0) return false;
    const int ratio = (int(w.fade) << 7) / int(w.fadeMax);
    alpha = (ratio * int(draw.alpha)) >> 7;
    if (alpha == 0) return false;
    int slide = 0x80 - ratio, twice = slide * 2;
    if (slide < 0) slide = 0, twice = 0;
    x = draw.x + (((twice * 4 + slide * 3) * 0x10) >> 7);
    return true;
}

// ---------------------------------------------------------------- REPLAY THEATER

ReplayTheaterMenu::ReplayTheaterMenu(const TitleAssets& assets, const PanelMenuLayout& layout) : assets_(assets), layout_(layout) {
    const GuestImage& o = assets.ovl1;
    widget_ = MenuListWidget::Read(o, o.Sim(layout.widget));
    if (widget_.count > kRows) throw std::runtime_error("panel list: more rows than the port holds");
    templateFlags_ = o.Get<uint8_t>(o.Sim(layout.rowTemplate));
    templateBrightness_ = o.Get<uint8_t>(o.Sim(layout.rowTemplate) + 1);
    templateReveal_ = o.Get<int16_t>(o.Sim(layout.rowTemplate) + 2);
    for (int r = 0; r < widget_.count; r++) sprites_[size_t(r)] = TitleSprite::Read(o, o.Sim(layout.sprites) + uint32_t(r) * 12);
    Start(false);
}

void ReplayTheaterMenu::Start(bool back) {
    delay_ = 0x14;
    MenuListReset(widget_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { return RowCallback(command, w, row, draw); });
    widget_.selection = back ? int16_t(choice) : int16_t(0);
    sounds.clear();
}

// 0x800125D0 (every command returns 1: every row enabled).
int32_t ReplayTheaterMenu::RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) {
    (void)w;
    if (row < 0 || row >= widget_.count) return 1;
    TitleRowState& r = rows_[size_t(row)];
    switch (command) {
    case kMenuListReset: // 0x80016394 from the template 0x8004B168, then the row's sprite
        r.flags = templateFlags_;
        r.revealWidth = templateReveal_;
        r.anim = -1;
        r.alpha = 0x80;
        r.brightness = templateBrightness_;
        r.sprite = sprites_[size_t(row)];
        break;
    case kMenuListReveal: r.anim = 0; break;
    case kMenuListClose: r.anim = -13; break; // (the collapse 0x8006B61C draws is not ported: the closing rows are not drawn)
    case kMenuListTick: // 0x800163C8
        if (r.anim < 0) {
            if (r.anim < -1) r.anim++;
        } else if (++r.anim > 11) {
            r.anim = 12;
        }
        break;
    case kMenuListDraw:
        if (!draw || !draw->ot) break;
        r.x = draw->x;
        r.y = draw->y;
        DrawTitleRow(r, *draw->ot, false); // 0x80016410(row, ot, 0)
        break;
    case kMenuListEnter: r.anim = 12; break;
    default: break;
    }
    return 1;
}

int ReplayTheaterMenu::Update(const MenuListPad* pad) {
    sounds.clear();
    if (delay_ > 0 && --delay_ == 0) MenuListOpen(widget_);
    const int32_t r = MenuListUpdate(widget_, pad);
    if (r == -3) {
        sounds.push_back(5);
        return 0;
    }
    if (r == -4 || r == -2) return 0;
    if (r == -1) {
        sounds.push_back(4);
        MenuListClose(widget_);
        return 2;
    }
    sounds.push_back(3);
    choice = r;
    MenuListClose(widget_);
    return 1;
}

std::vector<MenuPrim> ReplayTheaterMenu::Frame() const {
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot list, header;
    MenuListDraw(widget_, list);
    AddViewHeader(header, assets_, assets_.Text(layout_.viewTitle), layout_.viewColour, 0x80);
    list.Emit(prims, 0x200);
    header.Emit(prims, 0x200);
    return prims;
}

// ---------------------------------------------------------------- DEMONSTRATION

std::string DemoFilePath(const GuestImage& ovl1, uint8_t language) {
    const uint32_t id = ovl1.Get<uint32_t>(ovl1.Sim(0x8004C8A8u) + uint32_t(language) * 4);
    switch (id) { // the boot's file table 0x801E2EF0 (work/re/theater RAM): 0x23 -> 0x2B, 0x24 -> 0x2C, 0x25 -> 0x2D
    case 0x23: return "arcade/demofile_eu.gmr";
    case 0x24: return "arcade/demofile_jp.gmr";
    case 0x25: return "arcade/demofile_us.gmr";
    default: throw std::runtime_error("demo file id " + std::to_string(id) + " of language " + std::to_string(language) + " is not known");
    }
}

DemonstrationScreen::DemonstrationScreen(const TitleAssets& assets, const ReplayRowText& text, const ReplayCardFile& demo)
    : assets_(assets), text_(text), demo_(demo) {
    const GuestImage& o = assets.ovl1;
    widget_ = MenuListWidget::Read(o, o.Sim(kWidget));
    // style 0x8004B20C: {0x801B9620 medium, spacing 1, 0x801B95C0 small, spacing 1, 0x5A4A3E}
    style_.titleSpacing = o.Get<int16_t>(o.Sim(0x8004B20Cu) + 6);
    style_.detailSpacing = o.Get<int16_t>(o.Sim(0x8004B20Cu) + 0xE);
    style_.rowColour = o.Get<uint32_t>(o.Sim(0x8004B20Cu) + 0x10);
    Start();
}

void DemonstrationScreen::Start() { // 0x80012CE4(0)
    delay_ = 0x18;
    widget_.count = int16_t(demo_.Count());
    MenuListReset(widget_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) -> int32_t {
        if (command != kMenuListDraw || !draw || !draw->ot || row < 0 || row >= demo_.Count()) return 1; // 0x80012B84: command 4 only
        int x = 0, alpha = 0;
        if (!ReplayRowPlacement(w, *draw, x, alpha)) return 1;
        if (w.state < -1) x = draw->x; // closing: no slide
        const ReplayCardEntry e = demo_.Entry(row);
        DrawReplayRow(rowSlot_ ? *rowSlot_ : *draw->ot, assets_, text_, style_, &e, kReplayRowEntry, x, draw->y, alpha); // the list's OT slot + 1
        return 1;
    });
    sounds.clear();
}

int DemonstrationScreen::Update(const MenuListPad* pad) { // 0x80012D34
    sounds.clear();
    if (delay_ > 0 && --delay_ == 0) {
        sounds.push_back(7);
        MenuListOpen(widget_);
    }
    const int32_t r = MenuListUpdate(widget_, pad);
    if (r == -3) {
        sounds.push_back(6);
        return 0;
    }
    if (r == -4) {
        sounds.push_back(0);
        return 0;
    }
    if (r == -2) return 0;
    if (r == -1) {
        sounds.push_back(4);
        MenuListClose(widget_);
        return 2;
    }
    sounds.push_back(3);
    MenuListClose(widget_);
    choice = r;
    return 1;
}

std::vector<MenuPrim> DemonstrationScreen::Frame() const {
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot rows, list, header;
    rowSlot_ = &rows;
    MenuListDraw(widget_, list);
    rowSlot_ = nullptr;
    AddViewHeader(header, assets_, assets_.Text(kViewTitle), kViewColour, 0x80);
    rows.Emit(prims, 0x200);
    list.Emit(prims, rows.FinalMode(0x200));
    header.Emit(prims, 0x200);
    return prims;
}

} // namespace gt2::shell
