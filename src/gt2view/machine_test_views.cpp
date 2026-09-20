#include "gt2view/machine_test_views.h"

#include <algorithm>
#include <cstdio>

namespace gt2::screens {

using shell::Band;

namespace {

// ovl0 addresses (US v1.2).
constexpr uint32_t kMtRowTable = 0x8005D2B8u, kMtRowOffsets = 0x8005D2E8u, kMtWidget = 0x8005D2F0u;
constexpr uint32_t kRowText = 0x8005D20Cu, kRowBand = 0x8005D228u;             // the event menu's row templates (0x80057A70)
constexpr uint32_t kTransmissionBar = 0x8005AC20u;
constexpr uint32_t kStrStart = 0x801C7097u, kStrTryAgain = 0x801C6E5Eu;
// RESULTS of a machine test (0x80052170)
constexpr uint32_t kResultsLabel = 0x8005B60Cu, kPlaceText = 0x8005B62Cu, kPlaceBand = 0x8005B648u, kSpeedStyle = 0x8005B68Cu,
                   kRecordLabel = 0x8005B6A0u, kTimeFade = 0x8005B6C0u, kRecordBand = 0x8005B6C4u, kBar = 0x8005B5ACu;
constexpr uint32_t kPlaceStrings = 0x8005AB5Cu, kPlaceColours = 0x8005AB84u;
constexpr uint32_t kStrOutOfRanking = 0x801C7A0Eu, kStrRecord = 0x801C700Eu;
constexpr uint32_t kOutOfRankingColour = 0x1652C4u;
constexpr uint32_t kTimeBase = 0x8005AB58u;   // 0x02000000
constexpr uint32_t kStrSpeedUnit = 0x801C6C87u;
// RECORD (0x80058E28)
constexpr uint32_t kRankFormat = 0x8005AB48u; // "%d."
constexpr uint32_t kRowBase = 0x8005D208u, kRowRank = 0x8005D338u, kRowValue = 0x8005D33Cu, kRowName = 0x8005D340u, kRowBar = 0x8005D344u;
constexpr uint32_t kStrNoRecords = 0x801C7016u;
constexpr uint32_t kMediumFont = 0x801C9150u, kSmallFont = 0x801C9120u;

uint32_t Grey(uint32_t v) { return v | v << 8 | v << 16; }

// The glyphs of the EXE text engine (0x8007DD3C) with the draw mode page | mode << 5 (race_result_screens.cpp's rule).
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

void CloseBand(Band& b) { b.anim = int16_t(~b.steps); }

const MenuListPad kNoPad{};

} // namespace

// ---------------------------------------------------------------- the menu (view 0x8005D390)

MachineTestMenuView::MachineTestMenuView(const RaceMenuAssets& a) : a_(a) {
    list = MenuListWidget::Read(a.ovl0, kMtWidget);
    AttachList();
    transmissionBar.anim = -1;
    for (int r = 0; r < kMtRows; r++) rowStrings[size_t(r)] = a.Text(a.ovl0.Get<uint32_t>(kMtRowTable + uint32_t(r) * 8));
}

std::string MachineTestMenuView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t MachineTestMenuView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

void MachineTestMenuView::AttachList() {
    list.callback = [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); };
}

void MachineTestMenuView::Setup(int mode, bool replay, bool withCar, bool again) { // 0x800587BC
    subMode = mode;
    counter = 16;
    carShown = -1;
    dialog = 0;
    transmissionBar = ResultBar::Init(a_, kTransmissionBar, 0);
    transmissionBar.x = 0xB0, transmissionBar.y = 0x19A;
    if (!again) settingsChosen = false;
    rowEnabled[kMtReplay] = !settingsChosen && replay;      // 0x8005D2BC
    rowStrings[kMtStart] = a_.Text(replay ? kStrTryAgain : kStrStart); // 0x8005D2C0
    rowEnabled[kMtSaveReplay] = rowEnabled[kMtReplay];      // 0x8005D2DC
    const int16_t keep = list.selection;
    list = MenuListWidget::Read(a_.ovl0, kMtWidget);
    MenuListReset(list, [this](int command, const MenuListWidget&, int row, const MenuListRowDraw* d) { return ListCallback(command, row, d); });
    if (again) list.selection = keep;
    car = withCar;
    if (car) carCamera = menu::ModelViewCamera(200, 200); // 0x80049780(W + 0x364, 200, 200)
    course = CourseTitle::Init(a_, a_.Text(career::MachineTestCourseTitle(mode))); // 0x80048BD8(W + 0x440, ...)
    course.Open(); // W + 0x458 = 0, 0x8006C4B0(W + 0x45C, -1)
    action_ = 0;
}

int32_t MachineTestMenuView::ListCallback(int command, int row, const MenuListRowDraw* d) { // 0x80057A70 in sub-modes 7..9
    if (row < 0 || row >= kMtRows) return 0;
    TextObject& text = rowText[size_t(row)];
    Band& b = rowBand[size_t(row)];
    switch (command) {
    case kMenuListReset:
        text = TextObject::FromTemplate(a_.ovl0, kRowText, rowStrings[size_t(row)]);
        b = Band::Read(a_.ovl0, kRowBand);
        b.anim = -1;
        break;
    case kMenuListReveal: text.Open(-1), b.anim = 0; break;
    case kMenuListClose: text.Close(), b.anim = int16_t(~b.steps); break;
    case kMenuListTick: text.Tick(), b.Tick(); break;
    case kMenuListDraw:
        if (d && drawOt_) {
            TextObject t = text;
            t.alpha = uint8_t(d->alpha);
            const int y = d->y + a_.ovl0.Get<int8_t>(kMtRowOffsets + uint32_t(row));
            t.Draw(*drawOt_, 0, d->x, y + 0x18, a_.FontAt(t.font));
            b.Draw((*drawOt_)[1], d->x - 8, y - 2);
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

void MachineTestMenuView::Close() {
    MenuListClose(list); // 0x8006CED8
    course.Close();      // W + 0x458 = ~W + 0x444, 0x8006C548(W + 0x45C)
    carShown = -1;
}

int MachineTestMenuView::Update(const MenuListPad* pad, bool input) { // 0x800589BC
    sounds.clear();
    if (counter > 0 && --counter == 0) {
        MenuListOpen(list);   // 0x8006CE70
        list.revealPeriod = -1; // 0x8005D30C
        carShown = 0;
    }
    course.Tick();
    if (car) menu::TurnModelCamera(carCamera, 16, frameLength); // 0x80049874
    const MenuListPad* buttons = input ? (pad ? pad : &kNoPad) : nullptr;
    bool leave = false;
    if (dialog != 0) {
        if (dialog == 1) {
            MenuListUpdate(list, nullptr);
            const int r = transmissionBar.Update(buttons, &sounds);
            if (r == -1) {
                sounds.push_back(2);
                dialog = 0;
            } else if (r >= 0 || r <= -4) {
                transmission = r;
                lastTransmission = uint8_t(r); // 0x801D156E = 0x801D5947
                sounds.push_back(3);
                action_ = 1; // M + 0x7C = 1 (set when the bar opened)
                leave = true;
            }
        }
    } else {
        transmissionBar.Update(nullptr, &sounds);
        const int r = MenuListUpdate(list, buttons);
        if (r == -3) {
            sounds.push_back(6);
        } else if (r == -4 || r == -1) {
            sounds.push_back(0);
        } else if (r >= 0 && r < int(list.count)) {
            const int code = a_.ovl0.Get<int8_t>(kMtRowTable + uint32_t(r) * 8 + 5); // 0x8005D2BD + r * 8
            if (code == 1 && !noTransmission) {
                sounds.push_back(1);
                transmissionBar.cursor = int8_t(lastTransmission != 0 ? 1 : 0); // W + 0x501
                transmissionBar.Open(); // 0x8006E388
                dialog = 1;
            } else {
                if (code == -3) settingsChosen = true; // 0x801C90F4 = 1, the view 0x8005D1C0
                action_ = code;
                sounds.push_back(3);
                leave = true;
            }
        }
    }
    if (!leave) return 0;
    Close();
    return 1;
}

void MachineTestMenuView::Draw(MenuOt& ot) const { // 0x80058D20
    const RaceMenuAssets& a = a_;
    transmissionBar.Draw(a, ot);
    course.Draw(a, ot, 0xB0, 0x68);
    drawOt_ = &ot;
    MenuListDraw(list, ot[0]);
    drawOt_ = nullptr;
}

std::optional<PostRaceModel> MachineTestMenuView::Model() const { // 0x80048754(W + 0x220, M + 0xC0, M + 0xD0, W + 0x364, 0)
    if (carShown < 0 || !car) return std::nullopt;
    PostRaceModel m;
    m.camera = carCamera;
    m.envX = 0x7C, m.envY = 0xA0; // 0x8008034C(M + 0xC0, (0x7C, 0xA0, 200, 200))
    return m;
}

// ---------------------------------------------------------------- NEW RECORD (view 0x8005B7E8)

MachineNewRecordView::MachineNewRecordView(const RaceMenuAssets& a) : keyboard(a, kDescriptor), a_(a) {
    keyboard.sound = [this](int id) { sounds.push_back(id); };
}

std::string MachineNewRecordView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t MachineNewRecordView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

void MachineNewRecordView::Setup(const std::string& lastName) { // 0x80051F54
    delay = 24;
    keyboard.name = lastName.substr(0, size_t(std::max<int>(0, keyboard.maxLength))); // + 0x5EC = 0x801D156F, + 0x5E6 = 11, + 0x5E8 = 256
}

int MachineNewRecordView::Update(const MenuListPad* pad, bool) { // 0x80051FC4 (the pad block M + 0x1A8 whatever the argument)
    sounds.clear();
    if (delay > 0 && --delay == 0) {
        keyboard.Open(); // 0x8007364C
        keyboard.caret = int16_t(keyboard.name.size()); // + 0x5E4 = strlen
    }
    const int r = keyboard.Update(pad ? pad : &kNoPad); // 0x80073720
    if (r == NameEntry::kCancel) {
        sounds.push_back(0);
        return 0;
    }
    if (r != NameEntry::kOk) return 0;
    sounds.push_back(3);
    keyboard.Close(); // 0x800736E0 (after the caller's 0x8005E03C)
    return 1;
}

void MachineNewRecordView::Draw(MenuOt& ot) const { keyboard.Draw(ot[0]); } // 0x80073AFC(W + 0x5C8, ot)

// ---------------------------------------------------------------- RESULTS (view 0x8005B80C)

MachineResultsView::MachineResultsView(const RaceMenuAssets& a) : a_(a) {}

std::string MachineResultsView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t MachineResultsView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

void MachineResultsView::Setup(const MachineResultsInput& in) { // 0x80052170
    const GuestImage& o = a_.ovl0;
    subMode = in.subMode;
    time = in.time;
    maxSpeed = in.maxSpeed;
    t = 0, carShown = -1, done = 0;
    resultsLabel = ResultLabel::Init(a_, kResultsLabel);
    // The place text: 0x8005AB5C[rank] in 0x8005AB84[rank] with the template's flags byte (+ 0xC) 200 when ranked (< 10),
    // "OUT OF RANKING" in 0x1652C4 with 0xE1 else.
    const uint32_t rank = uint32_t(int32_t(in.rank));
    const bool ranked = rank < 10;
    place = TextObject::FromTemplate(o, kPlaceText, a_.Text(ranked ? o.Get<uint32_t>(kPlaceStrings + rank * 4) : kStrOutOfRanking));
    place.flags = uint16_t((place.flags & 0xFF00u) | (ranked ? 200u : 0xE1u));
    place.c0 = ranked ? o.Get<uint32_t>(kPlaceColours + rank * 4) : kOutOfRankingColour;
    placeBand = Band::Read(o, kPlaceBand);
    placeBand.anim = -1;
    recordLabel = ResultLabel::Init(a_, kRecordLabel); // 0x8005B6A8 patched to "RECORD" before the init
    recordLabel.text.text = a_.Text(kStrRecord);
    recordBand = Band::Read(o, kRecordBand);
    recordBand.anim = -1;
    timeFade = {FadePair::Read(o, kTimeFade).steps, -1}; // 0x8005B6C2 = -1
    bar = ResultBar::Init(a_, kBar, 0);
    carCamera = menu::ResultsModelCamera(in.vsync); // 0x80050BC4(W + 0x1D8)
    save_ = false;
}

std::optional<PostRaceModel> MachineResultsView::Model() const { // 0x80048754(W + 0x94, M + 0xC0, M + 0xD0, W + 0x1D8, 0)
    if (carShown < 0) return std::nullopt;
    PostRaceModel m;
    m.camera = carCamera;
    m.envX = 0, m.envY = 0x96; // 0x8008034C(M + 0xC0, (0, 0x96, 0x160, 300))
    return m;
}

int MachineResultsView::Update(const MenuListPad* pad, bool input) { // 0x8005232C
    sounds.clear();
    int s = t;
    if (done == 0) s++;
    switch (s) {
    case 0x18: carShown = 0; break;
    case 0x30: resultsLabel.Open(); break;
    case 0x3C: place.Open(-1), placeBand.anim = 0; break;
    case 0x48: recordLabel.Open(); break;
    case 0x54: timeFade.anim = 0, recordBand.anim = 0; break;
    case 0x73:
        done = 1;
        bar.Open();  // 0x8006E388
        s = 0x74;
        bar.cursor = 1; // W + 0x575 = 1 ("Next")
        break;
    default: break;
    }
    t = int16_t(s);
    resultsLabel.Tick();
    place.Tick();
    placeBand.Tick();
    recordLabel.Tick();
    timeFade.Tick();
    recordBand.Tick();
    const MenuListPad* buttons = (s < 0x73 || !input) ? nullptr : (pad ? pad : &kNoPad);
    const int r = bar.Update(buttons, &sounds);
    bool left = false;
    if (r == 0 || r == 1) {
        save_ = r == 0; // 0: SAVE GAME 0x8005B588, 1: the leave view 0x8005AE30
        bar.Close();    // 0x8006E3FC
        sounds.push_back(3);
        resultsLabel.Close();
        place.Close();
        CloseBand(placeBand);
        recordLabel.Close();
        timeFade.Close();
        CloseBand(recordBand);
        carShown = -1;
        left = true;
    } else if (r == -1) {
        sounds.push_back(0);
    }
    menu::TurnModelCamera(carCamera, 12, frameLength); // 0x80050CC4 (the end of every update)
    return left ? 1 : 0;
}

void MachineResultsView::Draw(MenuOt& ot) const { // 0x80052638 (the 3D car 0x80048754: Model())
    const RaceMenuAssets& a = a_;
    bar.Draw(a, ot);
    resultsLabel.Draw(a, ot, 2, 0x28, 0x88);
    place.Draw(ot, 0, 0xB0, 0xB8, a.FontAt(place.font));
    placeBand.Draw(ot[1], 0x60, 0x88);
    ot[1].DrawMode(0x220);
    recordLabel.Draw(a, ot, 2, 0x28, 0xF0);
    recordBand.Draw(ot[1], 0x74, 0x100);
    ot[1].DrawMode(0x220);
    const TimeStyle style = TimeStyle::Read(a.ovl0, kSpeedStyle);
    // 0x800492C4(0x8005B6C0, ot + 4, ctx, value, 0xB0, 0x110, 0, 0x8005B68C, isNumber, 1): the time (sub-modes 7 / 8, W + 8) or
    // the max speed (else, W + 0xC) centred on x.
    if (subMode == career::kMachineTest400 || subMode == career::kMachineTest1000) AddTimeDisplay(a, ot[1], timeFade, time, 0xB0, 0x110, 0, style, true);
    else AddSpeedDisplay(a, ot[1], timeFade, maxSpeed, 0xB0, 0x110, 0, style, true);
}

void AddSpeedDisplay(const RaceMenuAssets& a, MenuOtSlot& ot, const FadePair& fade, uint32_t value, int x, int y, int mode, const TimeStyle& style, bool centre) {
    if (fade.anim == -1) return;
    int u = fade.anim;
    bool ghosts = true;
    if (u < 0) {
        u = ~u;
        ghosts = false;
    }
    const HudFont& f = a.FontAt(style.font);
    const std::string s = FormatRaceSpeed(value) + a.Text(kStrSpeedUnit); // 0x80068CA0 + strcat 0x801C6C87
    if (centre) x += f.NumberWidth(s, style.digitShift, 0) >> 1;            // 0x8006B044(ctx, s, style + 0xE, 0)
    const int k = 128 - (u << 7) / fade.steps;
    std::vector<HudFontSprite> glyphs;
    if (fade.anim < fade.steps && ghosts) {
        const uint32_t g = Grey(uint32_t(k)) | 0x2000000u;
        f.NumberRight(s, x - (k >> 1), y, style.digitShift, -2, 0, glyphs);
        AddSprites(ot, glyphs, g, 1);
        glyphs.clear();
        f.NumberRight(s, x + (k >> 1), y, style.digitShift, -2, 0, glyphs);
        AddSprites(ot, glyphs, g, 1);
        glyphs.clear();
    }
    const uint32_t colour = MenuListLerp(style.colour, a.ovl0.Get<uint32_t>(kTimeBase), k, 128);
    f.NumberRight(s, x, y, style.digitShift, -3, 0, glyphs);
    AddSprites(ot, glyphs, colour, mode & 3);
}

// ---------------------------------------------------------------- RECORD (view 0x8005D3B4)

MachineRecordsView::MachineRecordsView(const RaceMenuAssets& a, const career::MachineTestRecord& record, const career::MachineTestCarNames& names, int subMode)
    : a_(a), record_(record), names_(names), subMode_(subMode) {}

std::string MachineRecordsView::Title() const { return a_.Text(a_.ovl0.Get<uint32_t>(kView + 0x10)); }
uint32_t MachineRecordsView::Colour() const { return a_.ovl0.Get<uint32_t>(kView + 0x0C); }

void MachineRecordsView::Setup() { // 0x8005916C
    t = 0;
    closing = 0;
}

int MachineRecordsView::Update(const MenuListPad* pad, bool input) { // 0x8005918C
    sounds.clear();
    int v;
    if (closing == 0) {
        v = t + 1;
        if (v > 0x38) v = 0x38;
    } else {
        v = t - 1;
        if (v < 0) v = 0;
    }
    t = int16_t(v);
    const bool early = v < 0x11;
    if (!early && input && pad && (pad->pressed & 0xF00u) != 0) { // M + 0x1AC & 0xF00: a face button
        sounds.push_back(3);
        t = 0xC;
        closing = 1;
        return 2;
    }
    return 0;
}

void MachineRecordsView::Draw(MenuOt& ot) const { // 0x80059240
    const RaceMenuAssets& a = a_;
    const GuestImage& o = a.ovl0;
    const bool speed = subMode_ != career::kMachineTest400 && subMode_ != career::kMachineTest1000;
    MenuOtSlot& s0 = ot[0];
    int tt = t;
    for (int i = 0, x = 0x20, y = 0x78; i < career::kMachineTestEntries; i++, x += 4, y += 0x2C) {
        int alpha;
        if (closing == 0) {
            alpha = tt - 0x10;
            if (alpha > 0) {
                int v = alpha * 0x80;
                if (alpha > 8) v = 0x400;
                alpha = v >> 3;
            }
            tt -= 4;
        } else {
            alpha = (tt << 7) / 0xC;
        }
        if (alpha <= 0) continue;
        // 0x80058E28(ot, i, entry, 0x80074AE4(0x80169894, car id), x, y, alpha, speed)
        const uint32_t value = career::MachineTestEntryValue(record_, i);
        const int rx = x + (0x80 - alpha) * 2;
        const uint32_t base = o.Get<uint32_t>(kRowBase);
        std::vector<HudFontSprite> glyphs;
        char rank[16];
        std::snprintf(rank, sizeof rank, a.Text(kRankFormat).c_str(), i + 1);
        a.FontAt(kMediumFont).Number(rank, rx - 2, y + 0xC, 1, -2, 0, glyphs);
        AddSprites(s0, glyphs, MenuListLerp(base, o.Get<uint32_t>(kRowRank), alpha, 0x80), 1);
        const HudFont& small = a.FontAt(kSmallFont);
        if (value == 0xFFFFFFFFu) {
            const std::string none = a.Text(kStrNoRecords);
            glyphs.clear();
            small.Text(none, rx + 0x70 - (small.TextWidth(none, 1) >> 1), y + 8, 1, glyphs); // 0x8006ADB4
            AddSprites(s0, glyphs, MenuListLerp(base, o.Get<uint32_t>(kRowName), alpha, 0x80), 1);
        } else {
            glyphs.clear();
            if (!speed) a.FontAt(kMediumFont).TimeRight(FormatRaceTime(value), rx + 0x60, y + 0xC, 8, 7, 0, 0, glyphs); // 0x80068734 + 0x8006B3F4
            else a.FontAt(kMediumFont).NumberRight(FormatRaceSpeed(value) + a.Text(kStrSpeedUnit), rx + 0x60, y + 0xC, 1, -3, 0, glyphs); // 0x80068CA0 + 0x8006B184
            AddSprites(s0, glyphs, MenuListLerp(base, o.Get<uint32_t>(kRowValue), alpha, 0x80), 1);
            glyphs.clear();
            small.Text(career::MachineTestEntryName(record_, i), rx + 0x70, y, 1, glyphs); // 0x8006AC90(entry + 8)
            AddSprites(s0, glyphs, MenuListLerp(base, o.Get<uint32_t>(kRowName), alpha, 0x80), 1);
            glyphs.clear();
            small.Text(names_.NameOf(career::MachineTestEntryCar(record_, i)), rx + 0x70, y + 0x12, 1, glyphs);
            AddSprites(s0, glyphs, MenuListLerp(base, o.Get<uint32_t>(kRowName), alpha, 0x80), 1);
        }
        AddRoundedBar(s0, a.exe, MenuListLerp(base, o.Get<uint32_t>(kRowBar), alpha, 0x80), rx + 200, y, 400, 0x28, 0x18); // 0x800683FC
        s0.DrawMode(0);
    }
}

} // namespace gt2::screens
