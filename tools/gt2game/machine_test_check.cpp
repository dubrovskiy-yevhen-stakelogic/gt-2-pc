// The machine-test views from a RAM dump for the post-race views' check (see machine_test_check.h).
#include "machine_test_check.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "game/career/career_state.h"
#include "game/career/machine_test.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/race_menu_assets.h"
#include "gt2view/machine_test_views.h"

using namespace gt2;

namespace {

std::string TextAtAddress(const GuestImage& r, uint32_t a) {
    std::string s;
    if (a < r.base || a >= r.End()) return s;
    for (uint32_t o = a - r.base; o < r.bytes.size() && r.bytes[o] != 0; o++) s.push_back(char(r.bytes[o]));
    return s;
}

screens::TextObject TextAt(const GuestImage& r, uint32_t a) { // the EXE's text object (0x28 bytes)
    screens::TextObject t;
    t.revealDivisor = r.Get<uint8_t>(a), t.waveDivisor = r.Get<uint8_t>(a + 1);
    t.steps = r.Get<int16_t>(a + 2), t.glowSpread = r.Get<int16_t>(a + 4), t.period = r.Get<int16_t>(a + 6), t.fadeSteps = r.Get<int16_t>(a + 8);
    t.extra = r.Get<int8_t>(a + 0xA), t.height = r.Get<uint8_t>(a + 0xB), t.flags = r.Get<uint16_t>(a + 0xC);
    t.font = r.Get<uint32_t>(a + 0x10), t.c0 = r.Get<uint32_t>(a + 0x14), t.c1 = r.Get<uint32_t>(a + 0x18);
    t.text = TextAtAddress(r, r.Get<uint32_t>(a + 0x1C));
    t.alpha = r.Get<uint8_t>(a + 0x20);
    t.anim = r.Get<int16_t>(a + 0x22), t.settle = r.Get<int16_t>(a + 0x24), t.length = r.Get<int16_t>(a + 0x26);
    return t;
}

shell::Band BandAt(const GuestImage& r, uint32_t a) {
    shell::Band b = shell::Band::Read(r, a);
    b.anim = r.Get<int16_t>(a + 0x18);
    return b;
}

screens::ResultLabel LabelAt(const GuestImage& r, uint32_t a) { // 0x80048D14 layout (0x50 bytes)
    screens::ResultLabel l;
    l.band = BandAt(r, a);
    l.text = TextAt(r, a + 0x1C);
    l.flags = r.Get<uint16_t>(a + 0x4E);
    const uint32_t v = r.Get<uint32_t>(a + 0x44);
    if ((l.flags & 3) == 1) l.time = v;
    else l.value = TextAtAddress(r, v);
    l.valueColour = r.Get<uint32_t>(a + 0x48);
    l.fade = r.Get<int16_t>(a + 0x4C);
    return l;
}

screens::ResultBar BarAt(const GuestImage& r, uint32_t a) { // the EXE's two-button bar (0x8006E1CC layout)
    screens::ResultBar b;
    b.x = r.Get<int16_t>(a), b.y = r.Get<int16_t>(a + 2);
    b.title = TextAt(r, a + 4), b.label0 = TextAt(r, a + 0x2C), b.label1 = TextAt(r, a + 0x54);
    b.sound = r.Get<int8_t>(a + 0x7C), b.cursor = r.Get<int8_t>(a + 0x7D);
    b.w = r.Get<int16_t>(a + 0x7E), b.h = r.Get<int16_t>(a + 0x80), b.anim = r.Get<int16_t>(a + 0x82), b.slide = r.Get<int16_t>(a + 0x84);
    b.flags = r.Get<uint16_t>(a + 0x86);
    b.fill = r.Get<uint32_t>(a + 0x8C), b.gradient = r.Get<uint32_t>(a + 0x90);
    return b;
}

menu::OverlayModelCamera CameraAt(const GuestImage& r, uint32_t a) { // the camera object of 0x80048754 (0xDC bytes)
    menu::OverlayModelCamera c;
    for (int i = 0; i < 3; i++) c.position[size_t(i)] = r.Get<int32_t>(a + 0xA0 + uint32_t(i) * 4);
    c.pitch = r.Get<int16_t>(a + 0xAC), c.yaw = r.Get<int16_t>(a + 0xAE), c.roll = r.Get<int16_t>(a + 0xB0);
    c.x = r.Get<int16_t>(a + 0xC0), c.y = r.Get<int16_t>(a + 0xC2), c.w = r.Get<int16_t>(a + 0xC4), c.h = r.Get<int16_t>(a + 0xC6);
    c.left = r.Get<int16_t>(a + 0xC8), c.right = r.Get<int16_t>(a + 0xCA), c.top = r.Get<int16_t>(a + 0xCC), c.bottom = r.Get<int16_t>(a + 0xCE);
    c.distance = r.Get<int16_t>(a + 0xD0), c.farZ = r.Get<int16_t>(a + 0xD2);
    c.floor = r.Get<uint8_t>(a + 0xD4) != 0, c.floorSemi = r.Get<uint8_t>(a + 0xD5) != 0;
    c.floorColour = r.Get<uint32_t>(a + 0xD8);
    return c;
}

} // namespace

bool IsMachineTestScreen(const std::string& s) { return s == "mtmenu" || s == "mtnewrecord" || s == "mtresults" || s == "mtrecords"; }

std::unique_ptr<screens::PostRaceView> MachineTestViewFromRam(const RaceMenuAssets& a, const GuestImage& r, const std::string& screen, uint32_t& viewAddress) {
    const uint32_t W = r.Get<uint32_t>(0x801C90A0u), M = r.Get<uint32_t>(0x801C90A4u);
    const uint32_t current = r.Get<uint32_t>(M + 0x1C8), previous = r.Get<uint32_t>(M + 0x1CC);
    const int transition = r.Get<int8_t>(M + 0x211);
    const uint32_t want = screen == "mtmenu"      ? screens::MachineTestMenuView::kView
                          : screen == "mtnewrecord" ? screens::MachineNewRecordView::kView
                          : screen == "mtresults"   ? screens::MachineResultsView::kView
                                                    : screens::MachineRecordsView::kView;
    viewAddress = current == want ? current : (transition > 0 && previous == want) ? previous : 0;
    if (!viewAddress) return nullptr;
    const int subMode = r.Get<uint8_t>(0x801D5866u);
    if (screen == "mtmenu") {
        auto v = std::make_unique<screens::MachineTestMenuView>(a);
        v->subMode = subMode;
        v->counter = r.Get<int16_t>(viewAddress + 0x14);
        v->carShown = r.Get<int16_t>(viewAddress + 0x16);
        v->dialog = r.Get<int16_t>(viewAddress + 0x18);
        for (uint32_t row = 0; row < uint32_t(screens::kMtRows); row++) {
            v->rowEnabled[row] = r.Get<int8_t>(0x8005D2BCu + row * 8) != 0;
            v->rowStrings[row] = TextAtAddress(r, r.Get<uint32_t>(0x8005D2B8u + row * 8));
            v->rowText[row] = TextAt(r, W + row * 0x28);
            v->rowBand[row] = BandAt(r, W + 0x140 + row * 0x1C);
        }
        v->settingsChosen = r.Get<uint8_t>(0x801C90F4u) != 0;
        v->noTransmission = ((r.Get<uint32_t>(0x801D5DE4u) >> 3) & 1) != 0;
        v->lastTransmission = r.Get<uint8_t>(0x801D156Eu);
        v->list = MenuListWidget::Read(r, 0x8005D2F0u);
        v->AttachList();
        v->course.band = BandAt(r, W + 0x440);
        v->course.text = TextAt(r, W + 0x45C);
        v->transmissionBar = BarAt(r, W + 0x484);
        v->car = r.Get<uint8_t>(M + 0x241) != 0;
        v->carCamera = CameraAt(r, W + 0x364);
        v->frameLength = r.Get<int32_t>(M + 0x234);
        std::printf("race-menu-check: MACHINE TEST menu (sub-mode %d): counter %d car %d dialog %d, list sel %d state %d fade %d, bar %d\n", subMode, v->counter,
                    v->carShown, v->dialog, v->list.selection, v->list.state, v->list.fade, v->transmissionBar.anim);
        return v;
    }
    if (screen == "mtnewrecord") {
        auto v = std::make_unique<screens::MachineNewRecordView>(a);
        screens::NameEntry& kb = v->keyboard;
        const uint32_t k = W + 0x5C8;
        v->delay = r.Get<int16_t>(viewAddress + 0x14);
        kb.anim = r.Get<int16_t>(k + 0x14), kb.pulse = r.Get<int16_t>(k + 0x16), kb.caretPhase = r.Get<int16_t>(k + 0x18);
        kb.row = r.Get<int8_t>(k + 0x1A), kb.column = r.Get<int8_t>(k + 0x1B), kb.caret = r.Get<int16_t>(k + 0x1C);
        kb.maxLength = r.Get<int16_t>(k + 0x1E), kb.widthLimit = r.Get<int16_t>(k + 0x20);
        kb.name = TextAtAddress(r, r.Get<uint32_t>(k + 0x24));
        kb.band = BandAt(r, k + 0x28);
        const MenuListCallback callback = kb.list.callback;
        kb.list = MenuListWidget::Read(r, k + 0x44);
        kb.list.callback = callback;
        std::printf("race-menu-check: machine NEW RECORD delay %d, \"%s\" caret %d, anim %d, list sel %d state %d fade %d\n", v->delay, kb.name.c_str(), kb.caret, kb.anim,
                    kb.list.selection, kb.list.state, kb.list.fade);
        return v;
    }
    if (screen == "mtresults") {
        auto v = std::make_unique<screens::MachineResultsView>(a);
        v->subMode = subMode;
        v->t = r.Get<int16_t>(viewAddress + 0x14), v->carShown = r.Get<int16_t>(viewAddress + 0x16), v->done = r.Get<int16_t>(viewAddress + 0x18);
        v->time = r.Get<uint32_t>(W + 8), v->maxSpeed = r.Get<uint32_t>(W + 0xC);
        v->resultsLabel = LabelAt(r, W + 0x2F8);
        v->place = TextAt(r, W + 0x348);
        v->placeBand = BandAt(r, W + 0x370);
        v->recordLabel = LabelAt(r, W + 0x38C);
        v->timeFade = {r.Get<int16_t>(0x8005B6C0u), r.Get<int16_t>(0x8005B6C2u)};
        v->recordBand = BandAt(r, W + 0x3DC);
        v->bar = BarAt(r, W + 0x4F8);
        v->carCamera = CameraAt(r, W + 0x1D8);
        v->frameLength = r.Get<int32_t>(M + 0x234);
        std::printf("race-menu-check: machine RESULTS t %d car %d done %d, rank W+2 %d, time %u speed %u, bar %d\n", v->t, v->carShown, v->done, r.Get<int16_t>(W + 2), v->time,
                    v->maxSpeed, v->bar.anim);
        return v;
    }
    // mtrecords: the career's record of the sub-mode and the car names the race overlay's init copied to 0x80169894.
    career::MachineTestRecord record{};
    const uint32_t at = career::kStateAddress + 0x3A88u + uint32_t(std::max(0, career::MachineTestIndex(subMode))) * 0xA4u;
    std::memcpy(record.bytes, r.At(at, sizeof(record.bytes)), sizeof(record.bytes));
    career::MachineTestCarNames names;
    names.bytes.assign(r.At(0x80169894u, 0x364), r.At(0x80169894u, 0x364) + 0x364);
    names.written.assign(names.bytes.size(), 1);
    auto v = std::make_unique<screens::MachineRecordsView>(a, record, names, subMode);
    v->t = r.Get<int16_t>(viewAddress + 0x14);
    v->closing = r.Get<int16_t>(viewAddress + 0x16);
    std::printf("race-menu-check: machine RECORD (sub-mode %d): t %d closing %d, %d entries\n", subMode, v->t, v->closing, int(int8_t(record.bytes[0])));
    return v;
}
