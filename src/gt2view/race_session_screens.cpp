#include "gt2view/race_session_screens.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/career/results.h"
#include "game/shell/title_draw.h"

namespace gt2::screens {

using shell::Band;

namespace {

// ovl0 addresses (US Simulation v1.2).
constexpr uint32_t kColourBase = 0x8005AE78u, kColourHeader = 0x8005AE7Cu, kColourGrey = 0x8005AE80u;
constexpr uint32_t kResultsList = 0x8005AE84u, kRule = 0x8005AEB8u, kRecordBand = 0x8005AECCu;
constexpr uint32_t kSessionLabel = 0x8005AEE8u, kRecordLabel = 0x8005AF08u, kCarLabel = 0x8005AF28u, kNameLabel = 0x8005AF48u;
constexpr uint32_t kMenuList = 0x8005AF7Cu, kGhostList = 0x8005AFB0u, kRowText = 0x8005AFE4u, kRowBand = 0x8005B000u, kRows = 0x8005B01Cu;
constexpr int kRowCount = 9;
constexpr uint32_t kGhostLabels = 0x8005B08Cu;                       // "No Ghost", "Type1", "Type2", "Type3"
constexpr uint32_t kTimeBase = 0x8005AB58u;                          // 0x800495E4's lerp base (0x02000000)
constexpr uint32_t kLapFormat = 0x8005A864u;                         // 0x800495E4's lap label format
// data-race strings
constexpr uint32_t kStrLap = 0x801C6C50u, kStrSector = 0x801C6F6Fu, kStrTotal = 0x801C6F7Au, kStrMaxSpeed = 0x801C6F81u, kStrSpeedUnit = 0x801C6C87u;
// fonts
constexpr uint32_t kSmallFont = 0x801C9120u, kMediumFont = 0x801C9150u;
// the lap colours of 0x8004AD40 / 0x8004AE08 (li constants)
constexpr uint32_t kLapLabelColour = 0x0214465Au, kTimeColour = 0x025A5A5Au, kBestColour = 0x02013060u, kRecordTotalColour = 0x02011660u;

uint32_t Grey(uint32_t v) { return v | v << 8 | v << 16; }

// The EXE text engine's glyphs (0x8007DD3C): {SPRT, E1 page | mode << 5} per glyph.
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

int BandAlpha(const Band& b) { // EXE 0x8006BEB4
    const int a = b.anim >= 0 ? b.anim : b.anim < -1 ? ~b.anim : 0;
    return b.steps ? (a << 7) / b.steps : 0;
}

int ListAlpha(const MenuListWidget& w) { return w.fadeMax ? (int(w.fade) << 7) / w.fadeMax : 0; } // fade * 128 / fadeMax

int32_t Sector(const sim::LapEntry& e, int s) { // EXE 0x8005DD94
    career::TimeRecord r;
    std::memcpy(&r, &e, sizeof r);
    return career::TimeRecordSector(r, s);
}

std::string Format(const std::string& format, int value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), format.c_str(), value);
    return buffer;
}

const MenuListPad kNoButtons{};

} // namespace

// ---------------------------------------------------------------- wait / leave

int SessionWaitView::Update(const MenuListPad*, bool) { // 0x8004A7B4
    sounds.clear();
    cdTracks.clear();
    if (counter <= 0) return 0;
    if (--counter != 0) return 0;
    cdTracks.push_back(8); // 0x800481C8(8)
    if (!raceRun_) next = kMenu;
    else next = newRecord_ ? kEnterName : kResults; // *W = 1 (SessionResultsView::fromName)
    return 1;
}

int SessionLeaveView::Update(const MenuListPad*, bool) { // 0x8004A8E8
    sounds.clear();
    const uint16_t v = uint16_t(uint16_t(counter) - 1);
    counter = int16_t(v);
    return int((uint32_t(int32_t(int16_t(v))) >> 13) & 4);
}

// ---------------------------------------------------------------- ENTER YOUR NAME

SessionNameView::SessionNameView(const RaceMenuAssets& a, const std::string& name) : keyboard(a, kDescriptor), a_(a) { // 0x8004A920
    delay = 24;
    keyboard.maxLength = 11;
    keyboard.widthLimit = 0x100;
    keyboard.name = name.substr(0, 11);
    keyboard.sound = [this](int id) { sounds.push_back(id); };
}

std::string SessionNameView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t SessionNameView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

int SessionNameView::Update(const MenuListPad* pad, bool) { // 0x8004A990 (the pad block M+0x1A8: read in both manager calls)
    sounds.clear();
    if (delay > 0 && --delay == 0) {
        keyboard.Open();
        keyboard.caret = int16_t(keyboard.name.size()); // 0x8008CFC4 = strlen
    }
    const int r = keyboard.Update(pad ? pad : &kNoButtons);
    if (r == NameEntry::kCancel) {
        sounds.push_back(0);
        return 0;
    }
    if (r != NameEntry::kOk) return 0;
    sounds.push_back(3);
    keyboard.Close();
    return 1; // the caller stores the record's name / car (0x8005E764 + 0x18 / + 0x14, race block + 0x53C)
}

// ---------------------------------------------------------------- the time rows

void AddTimeRow(const RaceMenuAssets& a, MenuOtSlot& ot, const TimeRow& row, int x, int y, int alpha) { // 0x800495E4
    const HudFont& f = a.FontAt(kSmallFont);
    const uint32_t base = a.ovl0.Get<uint32_t>(kTimeBase);
    std::vector<HudFontSprite> glyphs;
    if (row.lap > 0) {
        f.NumberRight(Format(a.Text(kLapFormat), row.lap), x, y, 1, -2, 0, glyphs); // 0x8006B184
        AddSprites(ot, glyphs, MenuListLerp(base, row.colours[0], alpha, 0x80), 1);
    }
    for (int i = 0; i < 5; i++) {
        x += 0x3C;
        const int32_t value = i < 4 ? Sector(row.entry, i) : row.entry.time;
        glyphs.clear();
        f.TimeRight(FormatRaceTime(uint32_t(value)), x, y, 6, 5, 0, 0, glyphs); // 0x80068734 + 0x8006B3F4
        AddSprites(ot, glyphs, MenuListLerp(base, row.colours[size_t(1 + i)], alpha, 0x80), 1);
    }
}

// ---------------------------------------------------------------- SESSION RESULTS

SessionResultsView::SessionResultsView(const RaceMenuAssets& a, const SessionInput& input, bool name) : fromName(name), in(input), a_(a) { // 0x8004AE08
    const GuestImage& o = a.ovl0;
    const sim::PlayerResults& r = in.results;
    course = CourseTitle::Init(a, in.course);
    course.Open();
    // 0x8004AC20: the best time of every sector over the kept laps and the session's best entry (unsigned: -1 is none).
    std::array<uint32_t, 4> best;
    best.fill(0xFFFFFFFFu);
    for (int i = 0; i < r.count && i < 10; i++)
        for (int s = 0; s < 4; s++) best[size_t(s)] = std::min(best[size_t(s)], uint32_t(Sector(r.laps[i], s)));
    for (int s = 0; s < 4; s++) best[size_t(s)] = std::min(best[size_t(s)], uint32_t(Sector(r.best, s)));
    auto colours = [&](bool bestLap, const sim::LapEntry& e) { // 0x8004AD40
        std::array<uint32_t, 6> c{};
        c[0] = kLapLabelColour;
        c[5] = bestLap ? kBestColour : kTimeColour;
        for (int s = 0; s < 4; s++) c[size_t(1 + s)] = uint32_t(Sector(e, s)) <= best[size_t(s)] ? kBestColour : kTimeColour;
        return c;
    };
    const int first = r.lapNumber - r.count;
    for (int i = 0; i < r.count && i < 10; i++) {
        TimeRow row;
        const int lap = first + i;
        row.lap = int16_t(lap + 1);
        row.entry = r.laps[i];
        row.colours = colours(lap == r.bestLapNumber, r.laps[i]);
        laps.push_back(row);
    }
    sessionRow.lap = int16_t(r.bestLapNumber + 1);
    sessionRow.entry = r.best;
    sessionRow.colours = colours(true, r.best);
    sessionBand = Band::Read(o, kRecordBand);
    sessionBand.anim = -1;
    sessionLabel = ResultLabel::Init(a, kSessionLabel);
    recordRow.lap = -1;
    recordRow.entry = in.courseRecord;
    recordRow.colours.fill(kTimeColour);
    recordRow.colours[5] = kRecordTotalColour;
    recordBand = Band::Read(o, kRecordBand);
    recordBand.anim = -1;
    recordLabel = ResultLabel::Init(a, kRecordLabel);
    {
        const HudFont& f = a.FontAt(kSmallFont);
        // 0x8006AD3C of 0x801C6FBD / 0x801C6FC8: the strings of the two descriptors (+ 8; the same addresses in the Simulation
        // build, and the translated pointers of the Arcade build's Simulation-layout copy)
        const int w = std::max(f.TextWidth(a.Text(o.Get<uint32_t>(kCarLabel + 8)), 1), f.TextWidth(a.Text(o.Get<uint32_t>(kNameLabel + 8)), 1));
        labelWidth = int16_t(w + 0x20);
        carLabel = ResultLabel::Init(a, kCarLabel);
        carLabel.band.w = int16_t(w + 4); // 0x8005AF44 / 0x8005AF64 (the descriptors' band width, written before the inits)
        nameLabel = ResultLabel::Init(a, kNameLabel);
        nameLabel.band.w = int16_t(w + 4);
    }
    list = MenuListWidget::Read(o, kResultsList);
    list.count = int16_t(laps.size());
    MenuListReset(list, [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); });
    rule.c0 = o.Get<uint32_t>(kRule), rule.c1 = o.Get<uint32_t>(kRule + 4);
    rule.x = o.Get<int16_t>(kRule + 8), rule.y = o.Get<int16_t>(kRule + 0xA), rule.w = o.Get<int16_t>(kRule + 0xC), rule.h = o.Get<int16_t>(kRule + 0xE);
    rule.steps = o.Get<int16_t>(kRule + 0x10);
    rule.anim = -1;
    counter = 24;
}

std::string SessionResultsView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t SessionResultsView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

int32_t SessionResultsView::ListCallback(int command, int row, const MenuListRowDraw* d) { // 0x8004AB04
    if (command == kMenuListEnabled) return 1;
    if (command != kMenuListDraw || !d || !drawOt_ || list.state == -1 || row < 0 || row >= int(laps.size())) return 0;
    const int alpha = (int(d->alpha) * ListAlpha(list)) >> 7;
    AddTimeRow(a_, *d->ot, laps[size_t(row)], 0x22, d->y + 8, alpha);
    return 0;
}

int SessionResultsView::Update(const MenuListPad* pad, bool input) { // 0x8004B19C
    sounds.clear();
    if (counter > 0) {
        --counter;
        if (counter == 0) {
            MenuListOpen(list);
        } else if (counter == 8) {
            sessionBand.anim = 0;
            recordBand.anim = 0;
        } else if (counter == 12) {
            sessionLabel.Open();
            recordLabel.Open();
            carLabel.Open();
            nameLabel.Open();
            rule.anim = 0;
        }
    }
    MenuListUpdate(list, pad ? pad : &kNoButtons); // the pad block M+0x1A8 in both manager calls
    MenuListClamp(list, 3, list.count - 3);
    // 0x8006BCB8: the rule's tick
    if (rule.anim < 0) {
        if (rule.anim < -1) rule.anim++;
    } else if (++rule.anim > rule.steps) {
        rule.anim = rule.steps;
    }
    sessionBand.Tick();
    sessionLabel.Tick();
    recordBand.Tick();
    recordLabel.Tick();
    carLabel.Tick();
    nameLabel.Tick();
    course.Tick();
    if (counter != 0 || !input || !pad) return 0;
    const bool choose = (pad->pressed & menu_list_pad::kChoose) != 0, back = (pad->pressed & menu_list_pad::kBack) != 0;
    if (!choose && !back) return 0;
    if (back) {
        if (fromName) {
            sounds.push_back(0);
            return 0;
        }
        sounds.push_back(4);
    } else {
        sounds.push_back(3);
    }
    course.Close();
    MenuListClose(list);
    sessionBand.anim = int16_t(~sessionBand.steps);
    sessionLabel.Close();
    recordBand.anim = int16_t(~recordBand.steps);
    recordLabel.Close();
    carLabel.Close();
    nameLabel.Close();
    rule.anim = int16_t(~rule.steps);
    return fromName ? 1 : 2; // *W != 0: on to the menu (0x800483A4(M, 0x8005B12C)); else back to it
}

void SessionResultsView::Draw(MenuOt& ot) const { // 0x8004B530
    const RaceMenuAssets& a = a_;
    const GuestImage& o = a.ovl0;
    const HudFont& small = a.FontAt(kSmallFont);
    MenuOtSlot& s0 = ot[0];
    std::vector<HudFontSprite> glyphs;
    uint32_t ctxColour = 0x808080; // the text context's colour (0x8006AC68(ctx, 6)); 0x800495E4 leaves its last colour in it
    course.Draw(a, ot, 0xB0, 0x5E);
    drawOt_ = &ot;
    MenuListDraw(list, ot[2]);
    drawOt_ = nullptr;
    if (list.state >= 0) {
        ctxColour = MenuListLerp(o.Get<uint32_t>(kColourBase), o.Get<uint32_t>(kColourHeader), ListAlpha(list), 0x80);
        glyphs.clear();
        small.TextRight(a.Text(kStrLap), 0x2A, 0x88, 1, glyphs); // 0x8006AE28
        AddSprites(s0, glyphs, ctxColour, 1);
        int x = 0x4A;
        for (int i = 1; i <= 4; i++, x += 0x3C) { // 0x8006ADB4: centred
            const std::string s = Format(a.Text(kStrSector), i);
            glyphs.clear();
            small.Text(s, x - (small.TextWidth(s, 1) >> 1), 0x88, 1, glyphs);
            AddSprites(s0, glyphs, ctxColour, 1);
        }
        const std::string total = a.Text(kStrTotal);
        glyphs.clear();
        small.Text(total, x - (small.TextWidth(total, 1) >> 1), 0x88, 1, glyphs);
        AddSprites(s0, glyphs, ctxColour, 1);
    }
    auto maxSpeed = [&](const sim::LapEntry& e, int y) { // 0x80068CA0 + strcat, 0x8006B184 / 0x8006AE28 in the row's total colour
        glyphs.clear();
        small.NumberRight(FormatRaceSpeed(uint32_t(uint16_t(e.maxSpeed))) + a.Text(kStrSpeedUnit), 0x150, y, 1, -3, 0, glyphs);
        AddSprites(s0, glyphs, ctxColour, 1);
        glyphs.clear();
        small.TextRight(a.Text(kStrMaxSpeed), 0x108, y, 1, glyphs);
        AddSprites(s0, glyphs, ctxColour, 1);
    };
    sessionBand.Draw(s0, 0, 0x130);
    s0.DrawMode(0x220);
    if (sessionBand.anim != -1) {
        const int alpha = BandAlpha(sessionBand);
        AddTimeRow(a, s0, sessionRow, 0x22, 0x140, alpha);
        ctxColour = MenuListLerp(o.Get<uint32_t>(kTimeBase), sessionRow.colours[5], alpha, 0x80);
        if (in.results.best.time != -1) maxSpeed(in.results.best, 300);
    }
    sessionLabel.Draw(a, ot, 0, 0x14, 0x122);
    if (rule.anim != -1) { // 0x8006BD08
        int t = rule.anim, k = rule.steps - rule.anim;
        if (rule.anim < 0) t = ~rule.anim, k = rule.steps + 1 + rule.anim;
        const int w = rule.steps ? (rule.w * t) / rule.steps : 0;
        s0.Add(Tile(rule.x - (w >> 1), rule.y, w, rule.h, MenuListLerp(rule.c0, rule.c1, k, rule.steps)));
    }
    recordBand.Draw(s0, 0, 0x16C);
    s0.DrawMode(0x220);
    if (recordBand.anim != -1) {
        const int alpha = BandAlpha(recordBand);
        AddTimeRow(a, s0, recordRow, 0x22, 0x17C, alpha);
        ctxColour = MenuListLerp(o.Get<uint32_t>(kTimeBase), recordRow.colours[5], alpha, 0x80);
        if (in.courseRecord.time != -1) {
            maxSpeed(in.courseRecord, 0x168);
            ctxColour = MenuListLerp(o.Get<uint32_t>(kColourBase), o.Get<uint32_t>(kColourGrey), alpha, 0x80);
            glyphs.clear();
            small.Text(in.recordCar, labelWidth + 6, 0x194, 1, glyphs); // 0x8006AC90
            AddSprites(s0, glyphs, ctxColour, 1);
            s0.Add(Tile(labelWidth, 0x188, 4, 6, ctxColour));
            glyphs.clear();
            small.Text(in.recordName, labelWidth + 6, 0x1AA, 1, glyphs);
            AddSprites(s0, glyphs, ctxColour, 1);
            s0.Add(Tile(labelWidth, 0x19E, 4, 6, ctxColour));
            s0.DrawMode(0x220);
            carLabel.Draw(a, ot, 0, 0x14, 0x18C);
            nameLabel.Draw(a, ot, 0, 0x14, 0x1A2);
        }
    }
    recordLabel.Draw(a, ot, 0, 0x14, 0x15E);
}

// ---------------------------------------------------------------- TIME TRIAL

TimeTrialMenuView::TimeTrialMenuView(const RaceMenuAssets& a, const SessionInput& input) : in(input), a_(a) {
    const GuestImage& o = a.ovl0;
    for (int r = 0; r < kRowCount; r++) {
        const uint32_t e = kRows + uint32_t(r) * 12;
        rows.push_back({a.Text(o.Get<uint32_t>(e + 4)), o.Get<uint32_t>(e), o.Get<int8_t>(e + 8) != 0, o.Get<int8_t>(e + 9)});
    }
    rowText.assign(rows.size(), TextObject{});
    rowBand.assign(rows.size(), Band{});
    list = MenuListWidget::Read(o, kMenuList);
    ghostList = MenuListWidget::Read(o, kGhostList);
    ghostOption = in.ghostOption;
    Setup(false);
}

std::string TimeTrialMenuView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t TimeTrialMenuView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

void TimeTrialMenuView::Setup(bool back) { // 0x8004C034
    counter = 16, car = -1, ghostOpen = 0;
    chosen_ = 0;
    if (!back) {
        in.replayAvailable = true; // 0x801C90B4 = 0 (set by the Settings row's view)
        MenuListReset(list, [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); });
    }
    MenuListReset(ghostList, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) { return GhostCallback(command, w, row, d); });
    rows[2].enabled = in.garageCar;                    // 0x8005B03C: "Settings ..." (0x801D5DDE / 0x801D5DE0: a garage car)
    rows[0].enabled = rows[6].enabled = in.replayAvailable; // 0x8005B024 / 0x8005B06C: "Replay" / "Save Replay ..."
    rows[4].enabled = in.savedBest && !in.ghostLoaded; // 0x8005B054: "Save Ghost ..." = 0x8002F4B1 unless 0x801D55AA
    for (size_t r = 0; r < rows.size(); r++)
        if (unsupported & (1u << r)) rows[r].enabled = false;
    carCamera = menu::ModelViewCamera(200, 200);       // 0x80049780(W+0x608, 200, 200)
    course = CourseTitle::Init(a_, in.course);
    course.Open();
}

std::optional<PostRaceModel> TimeTrialMenuView::Model() const { // 0x8004C5C8: 0x80048754(W+0x4C4, M+0xC0, M+0xD0, W+0x608, 0)
    if (car < 0) return std::nullopt;
    PostRaceModel m;
    m.camera = carCamera;
    m.envX = 0x8A, m.envY = 0xF0; // 0x8008034C(M+0xC0, (0x8A, 0xF0, 200, 200))
    return m;
}

int32_t TimeTrialMenuView::ListCallback(int command, int row, const MenuListRowDraw* d) { // 0x8004BA40
    if (row < 0 || row >= int(rows.size())) return 0;
    TextObject& text = rowText[size_t(row)];
    Band& band = rowBand[size_t(row)];
    switch (command) {
    case kMenuListReset: {
        const HudFont& f = a_.FontAt(kMediumFont);
        int width = 0;
        for (const Row& r : rows) width = std::max(width, f.TextWidth(r.text, 2)); // 0x8006AD3C over the nine labels
        text = TextObject::FromTemplate(a_.ovl0, kRowText, rows[size_t(row)].text);
        band = Band::Read(a_.ovl0, kRowBand);
        band.w = int16_t(width + 0x10);
        band.anim = -1;
        break;
    }
    case kMenuListReveal: text.Open(-1), band.anim = 0; break;
    case kMenuListClose: text.Close(), band.anim = int16_t(~band.steps); break;
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

int32_t TimeTrialMenuView::GhostCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) { // 0x8004BD1C
    if (command > 4) return command == kMenuListEnabled ? 1 : 0;
    if (command != kMenuListDraw || !d || !d->ot || w.state == -1) return 0;
    const int alpha = ListAlpha(w);
    if (alpha <= 0) return 0;
    const GuestImage& o = a_.ovl0;
    const uint32_t base = o.Get<uint32_t>(kColourBase);
    const uint32_t c48 = 0x323C50u, c44 = 0x02A2A2A2u, c3c = 0x646464u;
    int x = d->x;
    const int y = d->y;
    if (w.state >= 0) x += (0x80 - alpha) >> 1;
    uint32_t frame = MenuListLerp(base, c48, alpha, 0x200);
    uint32_t fill = MenuListLerp(base, c48, alpha, 0x100);
    const int k = std::min<int>(blink, 12);
    if (row == w.selection) {
        frame = MenuListLerp(base, c48, alpha, 0x80);
        fill = MenuListLerp(c44, frame, k, 12);
    }
    const uint32_t textColour = MenuListLerp(base, c3c, alpha, 0x80);
    MenuOtSlot& ot = *d->ot;
    const HudFont& f = a_.FontAt(kSmallFont);
    const std::string label = a_.Text(o.Get<uint32_t>(kGhostLabels + uint32_t(row) * 4));
    std::vector<HudFontSprite> glyphs;
    f.Text(label, x + 0x28 - (f.TextWidth(label, 1) >> 1), y + 0x1C, 1, glyphs); // 0x8006ADB4
    AddSprites(ot, glyphs, textColour, 1);
    ot.Add(Tile(x + 2, y + 10, 0x4D, 0x15, fill));
    MenuListFrame(ot, frame, x, y + 8, 0x51, 0x19); // 0x8007E738: (x, y + 8) .. (x + 0x50, y + 0x20)
    ot.DrawMode(0x220);
    ot.Add(Tile(x, y + 8, 0x50, 0x18, 0x02000000u));
    ot.DrawMode(0x200);
    return 0;
}

int TimeTrialMenuView::Update(const MenuListPad* pad, bool input) { // 0x8004C1EC
    sounds.clear();
    if (counter > 0 && --counter == 0) {
        MenuListOpen(list);
        list.revealPeriod = -1;
        car = 0;
    }
    course.Tick();
    menu::TurnModelCamera(carCamera, 16, frameLength); // 0x80049874(W+0x608, M+0x234)
    const MenuListPad* buttons = input ? (pad ? pad : &kNoButtons) : nullptr;
    if (ghostOpen == 1) {
        if (++blink > 0x2D) blink = 0;
        MenuListUpdate(list, nullptr);
        const int r = MenuListUpdate(ghostList, buttons);
        if (r == -2) return 0;
        if (r < -1) {
            if (r == -3) {
                sounds.push_back(5);
                return 0;
            }
        } else if (r == -1) {
            sounds.push_back(2);
            ghostOpen = 0;
            MenuListClose(ghostList);
            return 0;
        }
        ghostOption = uint8_t(r); // 0x801C9995 (the row; a -4 would store 0xFC, as the original - its rows are all enabled)
        sounds.push_back(1);
        ghostOpen = 0;
        MenuListClose(ghostList);
        return 0;
    }
    MenuListUpdate(ghostList, nullptr);
    const int r = MenuListUpdate(list, buttons);
    if (r == -3) {
        sounds.push_back(6);
        return 0;
    }
    if (r == -4 || r == -1) {
        sounds.push_back(0);
        return 0;
    }
    if (r < 0 || r >= int(rows.size())) return 0;
    const int8_t action = rows[size_t(r)].action;
    if (action == kGhostOptions) {
        blink = 0;
        sounds.push_back(1);
        ghostOpen = 1;
        ghostList.selection = int16_t(ghostOption); // 0x8005AFB6
        MenuListOpen(ghostList);
        return 0;
    }
    chosen_ = action; // 0xFA.. push their views (card managers, settings, SESSION RESULTS); 0 / 1 / 2: M+0x7C, the leave view
    sounds.push_back(3);
    course.Close();
    MenuListClose(list);
    car = -1;
    return 1;
}

void TimeTrialMenuView::Draw(MenuOt& ot) const { // 0x8004C5C8
    const RaceMenuAssets& a = a_;
    MenuListDraw(ghostList, ot[0]);
    course.Draw(a, ot, 0xB0, 0x5E);
    drawOt_ = &ot;
    MenuListDraw(list, ot[0]);
    drawOt_ = nullptr;
    // The course picture (gt2formats/course_map.h, page 0x37): a SPRT 0x64 / 0x66 into ot[2], grey fade of the list | 0x02000000.
    const uint32_t g = uint32_t(ListAlpha(list));
    MenuPrim p;
    p.kind = MenuPrim::kSprite;
    p.x[0] = 0x94, p.y[0] = 0x6C, p.w = 0xBC, p.h = 200;
    p.u = 0, p.v = 1;
    p.clut = 0x401C;
    p.tpage = 0x37;
    p.colour[0] = Grey(g);
    p.semi = true;
    ot[2].Add(p);
    ot[2].DrawMode(0x37);
}

// ---------------------------------------------------------------- the view manager

void SessionViewStack::Start(std::unique_ptr<PostRaceView> first, bool transition) {
    stack_.clear();
    popped_.reset();
    stack_.push_back(std::move(first));
    previous_ = nullptr;
    transition_ = transition ? 16 : 0;
    back_ = false;
    slide_ = 0;
}

void SessionViewStack::Push(std::unique_ptr<PostRaceView> next) {
    popped_.reset();
    previous_ = stack_.empty() ? nullptr : stack_.back().get();
    stack_.push_back(std::move(next));
    transition_ = 16;
    back_ = false;
    slide_ = 0;
}

bool SessionViewStack::Pop() { // 0x800483D8
    if (stack_.size() < 2) return false;
    popped_ = std::move(stack_.back());
    stack_.pop_back();
    previous_ = popped_.get();
    transition_ = 16;
    back_ = true;
    slide_ = 0;
    return true;
}

void SessionViewStack::Replace(std::unique_ptr<PostRaceView> next, Slide slide) { // 0x80048374, then the manager's case 5 / 6
    if (stack_.empty()) throw std::logic_error("SessionViewStack::Replace without a view");
    popped_ = std::move(stack_.back());
    stack_.back() = std::move(next);
    previous_ = popped_.get();
    transition_ = 16;
    back_ = false;
    slide_ = int(slide);
}

int SessionViewStack::Update(const MenuListPad* pad) { // 0x800474F4
    if (transition_ > 0) {
        transition_--;
        if (transition_ > 0 && previous_) previous_->Update(pad, false);
        if (transition_ == 0) {
            previous_ = nullptr;
            popped_.reset();
        }
    }
    return Top() ? Top()->Update(pad, true) : 0;
}

std::vector<MenuPrim> SessionViewStack::Frame(const RaceMenuAssets& a, size_t& modelAt, std::optional<PostRaceModel>& model) const { // 0x800479AC
    std::vector<MenuPrim> prims = shell::TitleFrameStart();
    const size_t clear = prims.size();
    model.reset();
    modelAt = clear;
    const PostRaceView* current = Top();
    if (!current) return prims;
    auto add = [&](const PostRaceView& view, int alpha, bool entering, int dx, int dy) {
        // The view's drawing area (0x8008034C: E3 / E4 = the frame's rectangle moved by (dx, dy), clamped to the frame).
        const int left = std::max(0, dx), top = std::max(0, dy);
        const int right = std::min(RaceMenuAssets::kScreenWidth, dx + RaceMenuAssets::kScreenWidth) - 1;
        const int bottom = std::min(RaceMenuAssets::kScreenHeight, dy + RaceMenuAssets::kScreenHeight) - 1;
        for (MenuPrim p : BuildPostRaceViewPart(a, view, alpha, entering)) {
            for (int k = 0; k < 4; k++) p.x[k] = int16_t(p.x[k] + dx), p.y[k] = int16_t(p.y[k] + dy);
            if (p.kind == MenuPrim::kSprite || p.kind == MenuPrim::kTile) { // cut to the area (the texel rows / columns kept)
                const int cut = std::max(0, top - int(p.y[0]));
                const int h = std::min(int(p.h) - cut, bottom + 1 - std::max(top, int(p.y[0])));
                if (h <= 0) continue;
                p.y[0] = int16_t(p.y[0] + cut);
                if (p.kind == MenuPrim::kSprite) p.v = uint8_t(p.v + cut);
                p.h = int16_t(h);
                const int cutX = std::max(0, left - int(p.x[0]));
                const int w = std::min(int(p.w) - cutX, right + 1 - std::max(left, int(p.x[0])));
                if (w <= 0) continue;
                p.x[0] = int16_t(p.x[0] + cutX);
                if (p.kind == MenuPrim::kSprite) p.u = uint8_t(p.u + cutX);
                p.w = int16_t(w);
            } else if (p.clipX1 < p.clipX0 && (left > 0 || top > 0 || right < RaceMenuAssets::kScreenWidth - 1 || bottom < RaceMenuAssets::kScreenHeight - 1)) {
                // polygons / lines: the area as their clip rectangle (those with a drawing area of their own keep it)
                p.clipX0 = int16_t(left), p.clipY0 = int16_t(top), p.clipX1 = int16_t(right), p.clipY1 = int16_t(bottom);
            }
            prims.push_back(p);
        }
    };
    const int c = transition_;
    // 0x800477C4: forward (M+0x212 = 0) / back (1) offsets of the previous (env M+0xA0) and the current view (env M+0x90);
    // the sideways slides 2 / 3 of Replace (the divisions truncate toward zero).
    if (slide_ == kFromRight) {
        if (c > 0 && previous_) add(*previous_, c * 8, false, (100 * (c - 16)) / 16, 0);
        add(*current, 128 - c * 8, true, (200 * c) / 16, 0);
    } else if (slide_ == kFromLeft) {
        if (c > 0 && previous_) add(*previous_, c * 8, false, (100 * (16 - c)) / 16, 0);
        add(*current, 128 - c * 8, true, -((200 * c) / 16), 0);
    } else {
        if (c > 0 && previous_) add(*previous_, c * 8, false, 0, back_ ? (16 - c) * 5 : (c - 16) * 5);
        add(*current, 128 - c * 8, true, 0, back_ ? -((c * 200) >> 4) : (c * 200) >> 4);
    }
    model = current->Model();
    if (!model && c > 0 && previous_) model = previous_->Model();
    if (!model) return prims;
    const std::vector<MenuPrim> floor = BuildPostRaceModelFloor(*model);
    prims.insert(prims.begin() + std::ptrdiff_t(clear), floor.begin(), floor.end());
    modelAt = clear + floor.size();
    return prims;
}

} // namespace gt2::screens
