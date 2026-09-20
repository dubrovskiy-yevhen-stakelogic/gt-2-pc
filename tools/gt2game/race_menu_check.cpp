// gt2game --race-menu-check: see race_menu_check.h.
#include "race_menu_check.h"
#include "race_result_check.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2export/png_writer.h"
#include "gt2formats/license_data.h"
#include "gt2formats/race_menu_assets.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/race_menus.h"
#include "gt2view/race_record_screens.h"
#include "gt2view/race_result_screens.h"
#include "gt2view/race_menu_views.h"
#include "gt2view/race_card_screens.h"
#include "gt2view/race_session_screens.h"
#include "gt2formats/replay_card.h"
#include "gt2formats/title_assets.h"
#include "game/shell/title_replay.h"

using namespace gt2;

namespace {

std::vector<uint8_t> ReadFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + path);
    std::vector<uint8_t> bytes;
    uint8_t buffer[65536];
    size_t n;
    while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0) bytes.insert(bytes.end(), buffer, buffer + n);
    std::fclose(f);
    return bytes;
}

// A 2 MB RAM dump of gt2run (KSEG0 addresses).
struct Ram {
    std::vector<uint8_t> bytes;
    template <typename T> T Get(uint32_t address) const {
        const size_t o = address & 0x1FFFFF;
        if (o + sizeof(T) > bytes.size()) throw std::runtime_error("ram: read outside the dump");
        T v;
        std::memcpy(&v, &bytes[o], sizeof(T));
        return v;
    }
    std::string Text(uint32_t address) const {
        std::string s;
        for (size_t o = address & 0x1FFFFF; o < bytes.size() && bytes[o] != 0; o++) s.push_back(char(bytes[o]));
        return s;
    }
};

screens::TextObject TextAt(const Ram& r, uint32_t a) { // the EXE's text object (0x28 bytes)
    screens::TextObject t;
    t.revealDivisor = r.Get<uint8_t>(a), t.waveDivisor = r.Get<uint8_t>(a + 1);
    t.steps = r.Get<int16_t>(a + 2), t.glowSpread = r.Get<int16_t>(a + 4), t.period = r.Get<int16_t>(a + 6), t.fadeSteps = r.Get<int16_t>(a + 8);
    t.extra = r.Get<int8_t>(a + 0xA), t.height = r.Get<uint8_t>(a + 0xB), t.flags = r.Get<uint16_t>(a + 0xC);
    t.font = r.Get<uint32_t>(a + 0x10), t.c0 = r.Get<uint32_t>(a + 0x14), t.c1 = r.Get<uint32_t>(a + 0x18);
    t.text = r.Text(r.Get<uint32_t>(a + 0x1C));
    t.alpha = r.Get<uint8_t>(a + 0x20);
    t.anim = r.Get<int16_t>(a + 0x22), t.settle = r.Get<int16_t>(a + 0x24), t.length = r.Get<int16_t>(a + 0x26);
    return t;
}

// The EXE's two-button bar (0x8006E1CC layout) of the dump; null while hidden (anim -1: 0x8006E5B8 draws nothing).
std::shared_ptr<const screens::ResultBar> BarAt(const Ram& r, uint32_t a, bool evenHidden = false) {
    auto b = std::make_shared<screens::ResultBar>();
    b->x = r.Get<int16_t>(a), b->y = r.Get<int16_t>(a + 2);
    b->title = TextAt(r, a + 4), b->label0 = TextAt(r, a + 0x2C), b->label1 = TextAt(r, a + 0x54);
    b->sound = r.Get<int8_t>(a + 0x7C), b->cursor = r.Get<int8_t>(a + 0x7D);
    b->w = r.Get<int16_t>(a + 0x7E), b->h = r.Get<int16_t>(a + 0x80), b->anim = r.Get<int16_t>(a + 0x82), b->slide = r.Get<int16_t>(a + 0x84);
    b->flags = r.Get<uint16_t>(a + 0x86);
    b->fill = r.Get<uint32_t>(a + 0x8C), b->gradient = r.Get<uint32_t>(a + 0x90);
    if (b->anim == -1 && !evenHidden) return nullptr;
    return b;
}

// `testOverride` >= 0: that test's texts / car / times (the dump's car table holds the licence's ten cars) instead of block + 0x4B8's.
screens::LicenceMenuState LicenceState(const RaceMenuAssets& a, const GtfsVolume& vol, const Ram* ram, int testOverride = -1) {
    int licence = 5, test = 0;
    if (ram) {
        licence = ram->Get<uint8_t>(0x801D5867u);
        test = ram->Get<int16_t>(ram->Get<uint32_t>(0x801C90A0u) + 0x4B8);
    }
    if (testOverride >= 0) test = testOverride;
    screens::LicenceMenuState s = screens::LicenceMenuDefaults(a, licence, test);
    // 0x8004CA90: the test's record of carparam/usa_license_data.dat by the name 0x8005B1F0[licence] % test.
    char name[16];
    std::snprintf(name, sizeof(name), a.Text(a.ovl0.Get<uint32_t>(0x8005B1F0u + uint32_t(licence) * 4)).c_str(), test);
    const LicenseTest t = LicenseData::Load(vol).Test(name);
    s.launchSpeed = t.settings[0];
    for (uint32_t m = 0; m < 3; m++) s.medalTimes[m] = t.MedalTime(m + 1);
    if (ram) {
        const uint32_t block = ram->Get<uint32_t>(0x801C90A0u);
        const uint32_t car = 0x801DA4B8u + uint32_t(test) * 0x44 + uint32_t(licence) * 0x2A8;
        s.carName = ram->Text(car);
        s.carPower = ram->Get<int16_t>(car + 0x40);
        s.carDrive = ram->Get<int16_t>(car + 0x42);
        for (uint32_t i = 0; i < 10; i++) s.medals[i] = ram->Get<uint8_t>(0x801CACF8u + uint32_t(licence) * 0x668 + i * 0xA4 + 1);
        for (uint32_t r = 0; r < screens::kLicenceRows; r++) s.rowEnabled[r] = ram->Get<int8_t>(0x8005B368u + r * 8) != 0;
        s.selectedRow = ram->Get<int16_t>(0x8005B39Cu + 6);
        const uint32_t text = block + uint32_t(s.selectedRow) * 0x28 + 0x1F8;
        s.flashPhase = ram->Get<int16_t>(text + 0x22) - ram->Get<int16_t>(text + 0x24);
        const uint32_t view = ram->Get<uint32_t>(ram->Get<uint32_t>(0x801C90A4u) + 0x1C8);
        s.arrowPhase = ram->Get<int16_t>(view + 0x16);
        s.dimDescription = ram->Get<int16_t>(view + 0x1A) == 1;
        s.carInfoFade = ram->Get<int16_t>(block + 0x51C);
        s.transmissionBar = BarAt(*ram, block + 0xEC); // 0x8004ED00 / 0x8004F474
        if (s.transmissionBar)
            std::printf("race-menu-check: TRANSMISSION bar anim %d cursor %d slide %d flags 0x%X\n", s.transmissionBar->anim, s.transmissionBar->cursor,
                        s.transmissionBar->slide, s.transmissionBar->flags);
        // The title / description the running game shows (block +0x4D8 / +0x4DC..) must be what the file gives.
        const RaceMenuAssets::LicenceInfo info = a.Licence(licence, test);
        if (ram->Text(ram->Get<uint32_t>(block + 0x4D8)) != info.title || ram->Get<int16_t>(block + 0x4FC) != int16_t(info.lines.size()))
            std::printf("race-menu-check: WARNING: the dump's licence title / line count differ from license_info_us\n");
    }
    return s;
}

screens::EventMenuState EventState(const Ram* ram) {
    screens::EventMenuState s;
    if (!ram) return s;
    const uint32_t block = ram->Get<uint32_t>(0x801C90A0u);
    const uint32_t view = ram->Get<uint32_t>(ram->Get<uint32_t>(0x801C90A4u) + 0x1C8);
    s.title = ram->Text(ram->Get<uint32_t>(view + 0x10));
    s.titleColour = ram->Get<uint32_t>(view + 0x0C);
    s.course = ram->Text(ram->Get<uint32_t>(block + 0x440 + 0x1C + 0x1C));
    const uint8_t mode = ram->Get<uint8_t>(0x801D5866u);
    s.machineTest = mode > 6 && mode < 10;
    s.ghostOptions = mode == 10;
    const uint32_t table = s.machineTest ? 0x8005D2B8u : 0x8005D244u;
    for (uint32_t r = 0; r < screens::kEventRows; r++) s.rowEnabled[r] = ram->Get<int8_t>(table + r * 8 + 4) != 0;
    s.selectedRow = ram->Get<int16_t>(0x8005D284u + 6);
    const uint32_t text = block + uint32_t(s.selectedRow) * 0x28;
    s.flashPhase = ram->Get<int16_t>(text + 0x22) - ram->Get<int16_t>(text + 0x24);
    s.transmissionBar = BarAt(*ram, block + 0x484); // 0x80057EAC / 0x800585C0
    s.exitBar = BarAt(*ram, block + 0x518);
    for (const auto& [name, bar] : {std::pair{"TRANSMISSION", s.transmissionBar}, std::pair{"Exit?", s.exitBar}})
        if (bar) std::printf("race-menu-check: %s bar anim %d cursor %d slide %d flags 0x%X\n", name, bar->anim, bar->cursor, bar->slide, bar->flags);
    return s;
}

screens::PartsPageState PartsState(const Ram* ram) {
    screens::PartsPageState s;
    if (!ram) return s;
    const uint32_t page = ram->Get<uint32_t>(0x801C90F0u);
    s.group = ram->Get<int8_t>(page + 5);
    s.groupCount = ram->Get<int16_t>(page + 8);
    s.groupTable = s.groupCount == 7 ? 2 : 0; // 0x80052D40 / 0x80052CB4 (0x8005C364 vs 0x8005C384 differ in group 4 only)
    s.arrowPhase = ram->Get<int16_t>(page + 0xC);
    s.previousColour = ram->Get<uint32_t>(page + 0x30);
    for (uint32_t k = 0; k < s.stages.size(); k++) s.stages[k] = ram->Get<int16_t>(0x8016E894u + 0x17B0 + k * 2);
    s.partsSetting = ram->Get<uint32_t>(0x801C90D0u) != 0;
    return s;
}

screens::MachineSettingsState MachineState(const Ram* ram) {
    screens::MachineSettingsState s;
    if (!ram) return s;
    const uint32_t page = ram->Get<uint32_t>(0x801C90F0u) + 0x2A7C;
    const int count = ram->Get<int16_t>(page + 8);
    for (int g = 0; g < count && g < 8; g++) s.groups.push_back(ram->Get<uint32_t>(0x8005D140u + uint32_t(g) * 4));
    s.group = ram->Get<int8_t>(page + 5);
    if (s.group >= 0 && s.group < int(s.groups.size()) && s.groups[size_t(s.group)] == 0x8005D070u) // "Others": rows built by 0x80055B14
        for (uint32_t i = 0; i < 16 && ram->Get<uint32_t>(0x8005D080u + i * 8) != 0; i++)
            s.rows.push_back({ram->Get<uint32_t>(0x8005D080u + i * 8), ram->Get<int16_t>(0x8005D084u + i * 8)});
    for (uint32_t i = 0; i < 16; i++) s.adjustable[i] = ram->Get<int8_t>(page + 0x92 + i);
    for (uint32_t i = 0; i < 0x80; i++) s.sheet[i] = ram->Get<uint8_t>(0x8016E894u + i);
    s.previousColour = ram->Get<uint32_t>(page + 0x240);
    s.arrowPhase = ram->Get<int16_t>(page + 0xE);
    s.changeParts = ram->Get<uint32_t>(0x801C90E4u) != 0;
    return s;
}

// The PARTS SETTING page of a RAM dump: page P = [0x801C90F0] + 0x2A7C, the widget 0x8005D154, the popup band 0x8005D188,
// the globals 0x801C90DC..0x801C90EC, the group list 0x8005D140 (+ the "Others" rows 0x8005D080) and the settings sheet
// 0x8016E894. Prints whether our 0x80055B14 gives the dump's group list.
void MachinePage(const Ram& ram, const RaceMenuAssets& a, const career::CareerData& data, screens::MachineSettingsPage& p, career::TuneSheet& sheet) {
    GuestImage image;
    image.base = 0x80000000u;
    image.bytes = ram.bytes;
    std::memcpy(&sheet, image.At(career::kSettingsSheetAddress, sizeof(career::TuneSheet)), sizeof(career::TuneSheet));
    const uint32_t page = ram.Get<uint32_t>(0x801C90F0u) + 0x2A7C;
    p.x = ram.Get<int16_t>(page + 0), p.y = ram.Get<int16_t>(page + 2);
    p.state = ram.Get<int8_t>(page + 4), p.group = ram.Get<int8_t>(page + 5), p.row = ram.Get<int8_t>(page + 6);
    p.groupCount = ram.Get<int16_t>(page + 8);
    p.opening = ram.Get<int16_t>(page + 0xA);
    p.scroll = ram.Get<int16_t>(page + 0xC);
    p.arrowPhase = ram.Get<int16_t>(page + 0xE);
    p.flash = ram.Get<int16_t>(page + 0x10);
    for (uint32_t i = 0; i < p.entries.size(); i++) p.entries[i] = image.Get<career::SettingValue>(page + 0x12 + i * 8);
    for (uint32_t i = 0; i < p.counts.size(); i++) p.counts[i] = ram.Get<int8_t>(page + 0x92 + i);
    for (uint32_t i = 0; i < p.sliders.size(); i++) {
        const uint32_t s = page + 0xA4 + i * 0x18;
        screens::MachineSettingsPage::Slider& sl = p.sliders[i];
        sl.label = ram.Get<uint32_t>(s), sl.unit = ram.Get<uint32_t>(s + 4), sl.format = ram.Get<uint8_t>(s + 8);
        sl.value = ram.Get<int16_t>(s + 0xA), sl.min = ram.Get<int16_t>(s + 0xC), sl.max = ram.Get<int16_t>(s + 0xE), sl.field = ram.Get<int16_t>(s + 0x10);
        sl.active = ram.Get<int16_t>(s + 0x12), sl.phase = ram.Get<int16_t>(s + 0x14);
    }
    p.band = shell::Band::Read(image, page + 0x224);
    p.previousColour = ram.Get<uint32_t>(page + 0x240), p.colour = ram.Get<uint32_t>(page + 0x244);
    p.widget = MenuListWidget::Read(image, screens::MachineSettingsPage::kWidget);
    p.popupBand = shell::Band::Read(image, screens::MachineSettingsPage::kPopupBand);
    p.entryGroup = ram.Get<int16_t>(0x801C90DCu);
    p.entryCount = ram.Get<int16_t>(0x801C90E0u);
    p.input = ram.Get<uint32_t>(0x801C90E4u) != 0;
    p.groupDescription = ram.Get<uint32_t>(0x801C90E8u);
    p.description = ram.Get<uint32_t>(0x801C90ECu);
    p.groups = {};
    for (int g = 0; g < p.groupCount && g < 8; g++) p.groups.groups.push_back(ram.Get<uint32_t>(0x8005D140u + uint32_t(g) * 4));
    for (uint32_t i = 0; i < 16 && ram.Get<uint32_t>(0x8005D080u + i * 8) != 0; i++)
        p.groups.otherRows.push_back({ram.Get<uint32_t>(0x8005D080u + i * 8), ram.Get<int16_t>(0x8005D084u + i * 8)});
    const screens::MachineSettingsGroups ours = screens::BuildMachineSettingsGroups(a.ovl0, sheet, data);
    const bool others = std::find(ours.groups.begin(), ours.groups.end(), screens::MachineSettingsPage::kOthersGroup) != ours.groups.end();
    const bool same = ours.groups == p.groups.groups && (!others || ours.otherRows == p.groups.otherRows);
    std::printf("race-menu-check: group list 0x80055B14 (ours) %s the dump's 0x8005D140:", same ? "EQUALS" : "DIFFERS FROM");
    for (uint32_t g : ours.groups) std::printf(" %08X", g);
    if (others) {
        std::printf(" | others");
        for (const auto& r : ours.otherRows) std::printf(" %08X/%d", r.first, r.second);
    }
    std::printf("\n");
    if (!same) {
        std::printf("race-menu-check:   dump:");
        for (uint32_t g : p.groups.groups) std::printf(" %08X", g);
        std::printf("\n");
    }
}

// NEW RECORD (view 0x8005B494): the keyboard object at the race work block + 0x180 (race_record_screens.h).
void NewRecordFromRam(const Ram& ram, screens::NameEntry& kb) {
    GuestImage image;
    image.base = 0x80000000u;
    image.bytes = ram.bytes;
    const uint32_t k = ram.Get<uint32_t>(0x801C90A0u) + 0x180;
    if (ram.Get<uint32_t>(k) != kb.font || ram.Get<int16_t>(k + 0xE) != kb.centreX || ram.Get<int16_t>(k + 0x10) != kb.nameY)
        std::printf("race-menu-check: WARNING: the dump's keyboard descriptor differs from ovl0 0x8005B2B0\n");
    kb.anim = ram.Get<int16_t>(k + 0x14);
    kb.pulse = ram.Get<int16_t>(k + 0x16);
    kb.caretPhase = ram.Get<int16_t>(k + 0x18);
    kb.row = ram.Get<int8_t>(k + 0x1A);
    kb.column = ram.Get<int8_t>(k + 0x1B);
    kb.caret = ram.Get<int16_t>(k + 0x1C);
    kb.maxLength = ram.Get<int16_t>(k + 0x1E);
    kb.widthLimit = ram.Get<int16_t>(k + 0x20);
    kb.name = ram.Text(ram.Get<uint32_t>(k + 0x24));
    kb.band = shell::Band::Read(image, k + 0x28);
    MenuListCallback callback = kb.list.callback;
    kb.list = MenuListWidget::Read(image, k + 0x44);
    kb.list.callback = callback;
}

// RECORD (view 0x8005B4DC): the work block's +0x520 band, +0x5DC count, +0x5DE test, +0x684 slide, the view's +0x16 / +0x18
// and the career's licence record.
screens::RecordsState RecordsFromRam(const RaceMenuAssets& a, const Ram& ram) {
    GuestImage image;
    image.base = 0x80000000u;
    image.bytes = ram.bytes;
    screens::RecordsState s;
    const uint32_t block = ram.Get<uint32_t>(0x801C90A0u);
    const uint32_t view = ram.Get<uint32_t>(ram.Get<uint32_t>(0x801C90A4u) + 0x1C8);
    if (view != screens::RecordsView::kView) std::printf("race-menu-check: WARNING: the dump's view is %08X, not 0x8005B4DC\n", view);
    s.licence = ram.Get<uint8_t>(0x801D5867u);
    s.test = ram.Get<int16_t>(block + 0x5DE);
    s.title = a.Licence(s.licence, s.test).title;
    if (ram.Text(ram.Get<uint32_t>(block + 0x4D8)) != s.title) std::printf("race-menu-check: WARNING: the dump's test title differs from license_info_us\n");
    career::LicenceTestRecord record;
    const uint32_t at = 0x801CACF8u + uint32_t(s.licence) * 0x668 + uint32_t(s.test) * 0xA4;
    std::memcpy(&record, image.At(at, sizeof(record)), sizeof(record));
    screens::LoadRecordRows(s, record);
    if (ram.Get<int16_t>(block + 0x5DC) != s.count) std::printf("race-menu-check: WARNING: the dump's row count %d differs from the record's %d\n", ram.Get<int16_t>(block + 0x5DC), s.count);
    s.count = ram.Get<int16_t>(block + 0x5DC);
    s.slide = ram.Get<int16_t>(block + 0x684);
    s.band = shell::Band::Read(image, block + 0x520);
    s.arrowPhase = ram.Get<int16_t>(view + 0x16);
    s.active = ram.Get<int16_t>(view + 0x18) != 0;
    return s;
}

// The logical pad bits (gt_menu_list.h menu_list_pad) a gt2play script ("field:button[:fields],...") presses at `field`
// (a press listed at field N is read by the view update of field N + 1).
uint32_t ScriptPressed(const std::string& script, int field) {
    static const std::pair<const char*, uint32_t> kButtons[] = {{"up", 0x1}, {"down", 0x2}, {"left", 0x4}, {"right", 0x8}, {"l1", 0x10}, {"l2", 0x20},
                                                                {"triangle", 0x100}, {"cross", 0x200}, {"square", 0x400}, {"circle", 0x800}, {"r1", 0x1000},
                                                                {"r2", 0x2000}, {"start", 0x10000}, {"select", 0x20000}};
    uint32_t bits = 0;
    size_t at = 0;
    while (at < script.size()) {
        size_t end = script.find(',', at);
        if (end == std::string::npos) end = script.size();
        const std::string item = script.substr(at, end - at);
        at = end + 1;
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos || std::atoi(item.c_str()) != field) continue;
        const size_t c2 = item.find(':', c1 + 1);
        const std::string name = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        for (const auto& [n, bit] : kButtons)
            if (name == n) bits |= bit;
    }
    return bits;
}

// Optional logic run of the NEW RECORD / RECORD checks: from=<ram.bin> fromfield=N tofield=M script=<gt2play script>:
// the state of the dump `from` (taken after the update of field N) updated field by field to M with the script's
// presses by our ports (0x80073720 / 0x8004FE40); the frame is then built from that state (not from ram=), and the
// state is compared with ram= (the dump after field M).
struct LogicRun {
    std::string from, script;
    int fromField = 0, toField = 0;
    bool Enabled() const { return !from.empty(); }
    static LogicRun Parse(int argc, char** argv, int first) {
        LogicRun r;
        for (int i = first; i < argc; i++) {
            const std::string a = argv[i];
            if (a.rfind("from=", 0) == 0) r.from = a.substr(5);
            else if (a.rfind("script=", 0) == 0) r.script = a.substr(7);
            else if (a.rfind("fromfield=", 0) == 0) r.fromField = std::atoi(a.c_str() + 10);
            else if (a.rfind("tofield=", 0) == 0) r.toField = std::atoi(a.c_str() + 8);
        }
        return r;
    }
};

// Counts the fields of our state that differ from the dump's (printed).
int CompareNewRecord(const screens::NameEntry& ours, const screens::NameEntry& dump) {
    int bad = 0;
    auto check = [&](const char* what, int a, int b) {
        if (a != b) std::printf("race-menu-check: logic: %s ours %d, original %d\n", what, a, b), bad++;
    };
    check("anim", ours.anim, dump.anim), check("pulse", ours.pulse, dump.pulse), check("caret phase", ours.caretPhase, dump.caretPhase);
    check("row", ours.row, dump.row), check("column", ours.column, dump.column), check("caret", ours.caret, dump.caret);
    check("band anim", ours.band.anim, dump.band.anim), check("list selection", ours.list.selection, dump.list.selection);
    check("list scroll", ours.list.scroll, dump.list.scroll), check("list blink", ours.list.blink, dump.list.blink);
    check("list fade", ours.list.fade, dump.list.fade), check("list state", ours.list.state, dump.list.state), check("list active", ours.list.active, dump.list.active);
    check("list revealed", ours.list.revealed, dump.list.revealed), check("list reveal delay", ours.list.revealDelay, dump.list.revealDelay);
    if (ours.name != dump.name) std::printf("race-menu-check: logic: name ours \"%s\", original \"%s\"\n", ours.name.c_str(), dump.name.c_str()), bad++;
    std::printf("race-menu-check: logic: %d state field(s) differ from the dump\n", bad);
    return bad;
}

// Ours (canvas) against the capture's drawing area (0, 0) 352 x 480, 5-bit channels; side.png = ours | original | diff.
size_t Compare(const MenuCanvas& ours, const std::vector<uint16_t>& vram, const std::string& sidePath) {
    const int W = RaceMenuAssets::kScreenWidth, H = RaceMenuAssets::kScreenHeight;
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    size_t diff = 0;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint16_t o = ours.At(x, y), c = vram[size_t(y) * 1024 + size_t(x)];
            const bool d = (o & 0x7FFF) != (c & 0x7FFF);
            if (d) {
                diff++;
                minX = std::min(minX, x), minY = std::min(minY, y), maxX = std::max(maxX, x), maxY = std::max(maxY, y);
            }
            auto put = [&](int column, uint16_t v) {
                uint8_t* p = &side[(size_t(y) * W * 3 + size_t(column * W + x)) * 4];
                p[0] = uint8_t((v & 31) << 3), p[1] = uint8_t(((v >> 5) & 31) << 3), p[2] = uint8_t(((v >> 10) & 31) << 3);
            };
            put(0, o);
            put(1, c);
            uint8_t* e = &side[(size_t(y) * W * 3 + size_t(2 * W + x)) * 4];
            const uint8_t grey = uint8_t((((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) << 3) / 12);
            e[0] = d ? 255 : grey, e[1] = d ? 0 : grey, e[2] = d ? 0 : grey;
        }
    std::printf("race-menu-check: %zu differing pixels", diff);
    if (diff) std::printf(" (box %d,%d - %d,%d)", minX, minY, maxX, maxY);
    std::printf(" -> %s\n", sidePath.c_str());
    std::filesystem::create_directories(std::filesystem::path(sidePath).parent_path());
    WritePngRgba(sidePath, W * 3, H, side);
    return diff;
}

size_t CompareVramRegion(const MenuVram& ours, const std::vector<uint16_t>& vram, int x0, int y0, int w, int h) {
    size_t diff = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (ours.Word(x, y) != vram[size_t(y) * 1024 + size_t(x)]) diff++;
    return diff;
}

// msettings-route <script> <start.ram.bin> <start field> <ram.bin>@<field> ...: the PARTS SETTING page of the first dump
// driven by our 0x80056194 with the pad of a gt2play / gt2run script ("field:button[:fields]", held 6 fields unless
// given), compared with the page, its objects and the settings sheet of every later dump. Pad model (observed in the
// dumps of work/re/spec_msettings/run1 / run2): a key held in field F is seen by the update of field F + 1 (held; pressed
// on its first field); directions auto-repeat 28 fields after the press, then every 8.
int RunMachineRoute(const DiscImage& disc, const GtfsVolume& vol, const std::vector<std::string>& args) {
    if (args.size() < 4) {
        std::puts("usage: gt2game <disc> --race-menu-check msettings-route <script> <start.ram.bin> <start field> <ram.bin>@<field> ...");
        return 2;
    }
    struct Press { int at, length; uint32_t bit; };
    static const std::pair<const char*, uint32_t> kButtons[] = {
        {"up", menu_list_pad::kUp}, {"down", menu_list_pad::kDown}, {"left", menu_list_pad::kLeft}, {"right", menu_list_pad::kRight},
        {"l1", menu_list_pad::kL1}, {"l2", menu_list_pad::kL2}, {"r1", menu_list_pad::kR1}, {"r2", menu_list_pad::kR2},
        {"triangle", menu_list_pad::kTriangle}, {"cross", menu_list_pad::kCross}, {"square", menu_list_pad::kSquare}, {"circle", menu_list_pad::kCircle},
        {"start", menu_list_pad::kStart}, {"select", menu_list_pad::kSelect}};
    std::vector<Press> presses;
    for (size_t pos = 0; pos < args[0].size();) {
        size_t comma = args[0].find(',', pos);
        if (comma == std::string::npos) comma = args[0].size();
        const std::string item = args[0].substr(pos, comma - pos);
        const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
        if (c1 == std::string::npos) throw std::runtime_error("bad script item " + item);
        const std::string name = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1);
        uint32_t bit = 0;
        for (const auto& [n, b] : kButtons)
            if (name == n) bit = b;
        if (!bit) throw std::runtime_error("bad script button " + name);
        presses.push_back({std::atoi(item.c_str()), c2 == std::string::npos ? 6 : std::atoi(item.c_str() + c2 + 1), bit});
        pos = comma + 1;
    }
    auto padOf = [&](int update) {
        MenuListPad pad;
        const int f = update - 1;
        for (const Press& p : presses) {
            if (f < p.at || f >= p.at + p.length) continue;
            pad.held |= p.bit;
            const int k = f - p.at;
            if (k == 0) pad.pressed |= p.bit;
            if ((p.bit & 0xF) && k >= 28 && (k - 28) % 8 == 0) pad.repeat |= p.bit;
        }
        return pad;
    };

    const RaceMenuAssets assets = RaceMenuAssets::Load(disc, vol, RaceMenuAssets::Pictures::kSettings);
    career::CareerData data = career::CareerData::Load(disc, vol);
    data.strict = false; // as the game (the gear auto-set's residue entries: tuning.h)
    Ram startRam;
    startRam.bytes = ReadFile(args[1]);
    screens::MachineSettingsPage page;
    auto sheet = std::make_unique<career::TuneSheet>();
    MachinePage(startRam, assets, data, page, *sheet);
    page.Bind(assets.ovl0);
    int field = std::atoi(args[2].c_str());
    int failures = 0;
    std::vector<int> codes;
    for (size_t t = 3; t < args.size(); t++) {
        const size_t atSign = args[t].rfind('@');
        if (atSign == std::string::npos) throw std::runtime_error("bad target " + args[t]);
        const int target = std::atoi(args[t].c_str() + atSign + 1);
        codes.clear();
        for (; field < target; field++) {
            const MenuListPad pad = padOf(field + 1);
            const int code = page.Update(&pad, true, *sheet, data);
            if (code != screens::MachineSettingsPage::kNothing) codes.push_back(code);
        }
        Ram ram;
        ram.bytes = ReadFile(args[t].substr(0, atSign));
        screens::MachineSettingsPage want;
        auto wantSheet = std::make_unique<career::TuneSheet>();
        MachinePage(ram, assets, data, want, *wantSheet);
        std::vector<std::string> diffs;
        auto check = [&](const char* what, long ours, long theirs) {
            if (ours != theirs) diffs.push_back(std::string(what) + " " + std::to_string(ours) + " != " + std::to_string(theirs));
        };
        check("state", page.state, want.state);
        check("group", page.group, want.group);
        check("row", page.row, want.row);
        check("scroll", page.scroll, want.scroll);
        check("arrow", page.arrowPhase, want.arrowPhase);
        check("flash", page.flash, want.flash);
        check("entryGroup", page.entryGroup, want.entryGroup);
        check("entryCount", page.entryCount, want.entryCount);
        check("description", long(page.description), long(want.description));
        check("band", page.band.anim, want.band.anim);
        check("popupBand", page.popupBand.anim, want.popupBand.anim);
        check("previousColour", long(page.previousColour), long(want.previousColour));
        check("colour", long(page.colour), long(want.colour));
        check("widget.count", page.widget.count, want.widget.count);
        check("widget.selection", page.widget.selection, want.widget.selection);
        check("widget.state", page.widget.state, want.widget.state);
        check("widget.fade", page.widget.fade, want.widget.fade);
        check("widget.scroll", page.widget.scroll, want.widget.scroll);
        check("widget.blink", page.widget.blink, want.widget.blink);
        const int rows = page.RowCount(assets.ovl0, page.GroupAt(page.group));
        for (int i = 0; i < rows; i++) check(("counts[" + std::to_string(i) + "]").c_str(), page.counts[size_t(i)], want.counts[size_t(i)]);
        for (int i = 0; i < want.entryCount && i < 16; i++) {
            const auto& a = page.sliders[size_t(i)];
            const auto& b = want.sliders[size_t(i)];
            const std::string s = "slider[" + std::to_string(i) + "].";
            check((s + "label").c_str(), long(a.label), long(b.label));
            check((s + "unit").c_str(), long(a.unit), long(b.unit));
            check((s + "format").c_str(), a.format, b.format);
            check((s + "value").c_str(), a.value, b.value);
            check((s + "min").c_str(), a.min, b.min);
            check((s + "max").c_str(), a.max, b.max);
            check((s + "field").c_str(), a.field, b.field);
            check((s + "active").c_str(), a.active, b.active);
            check((s + "phase").c_str(), a.phase, b.phase);
        }
        const uint8_t* ours = reinterpret_cast<const uint8_t*>(sheet.get());
        const uint8_t* theirs = reinterpret_cast<const uint8_t*>(wantSheet.get());
        for (size_t i = 0; i < sizeof(career::TuneSheet); i++)
            if (ours[i] != theirs[i]) {
                char text[96];
                std::snprintf(text, sizeof text, "sheet +0x%zX %02X != %02X", i, ours[i], theirs[i]);
                diffs.push_back(text);
            }
        std::printf("race-menu-check: route field %d: state %d group %d row %d, popup sel %d; codes", target, page.state, page.group, page.row, page.widget.selection);
        for (int c : codes) std::printf(" %d", c);
        std::printf(" -> %s\n", diffs.empty() ? "EQUAL" : "DIFFERENT");
        for (const std::string& d : diffs) std::printf("race-menu-check:   %s\n", d.c_str());
        if (!diffs.empty()) failures++;
    }
    std::printf("race-menu-check: route: %d of %zu targets differ\n", failures, args.size() - 3);
    return failures ? 1 : 0;
}

// ---- the licence menu as a view (race_menu_views.h): every object from the dump; the view manager's run of a script

GuestImage RamImage(const Ram& r) {
    GuestImage g;
    g.base = 0x80000000u;
    g.bytes = r.bytes;
    g.module = GuestImage::kRaceRam;
    return g;
}

shell::Band BandAtRam(const GuestImage& g, uint32_t a) {
    shell::Band b = shell::Band::Read(g, a);
    b.anim = g.Get<int16_t>(a + 0x18);
    return b;
}

screens::ResultLabel LabelAtRam(const Ram& r, const GuestImage& g, uint32_t a) { // 0x80048D14 layout (0x50 bytes)
    screens::ResultLabel l;
    l.band = BandAtRam(g, a);
    l.text = TextAt(r, a + 0x1C);
    l.flags = r.Get<uint16_t>(a + 0x4E);
    const uint32_t v = r.Get<uint32_t>(a + 0x44);
    if ((l.flags & 3) == 1) l.time = v;
    else l.value = r.Text(v);
    l.valueColour = r.Get<uint32_t>(a + 0x48);
    l.fade = r.Get<int16_t>(a + 0x4C);
    return l;
}

// The view object of `view` (M + 0x1C8 current, M + 0x1CC previous during a switch); 0 when neither.
uint32_t ViewObject(const Ram& r, uint32_t view) {
    const uint32_t m = r.Get<uint32_t>(0x801C90A4u);
    for (uint32_t o : {0x1C8u, 0x1CCu}) {
        const uint32_t v = r.Get<uint32_t>(m + o);
        if (v == view) return v;
    }
    return 0;
}

std::unique_ptr<screens::LicenceMenuView> LicenceViewFromRam(const RaceMenuAssets& a, const GtfsVolume& vol, const Ram& r) {
    auto v = std::make_unique<screens::LicenceMenuView>(a);
    const GuestImage g = RamImage(r);
    const screens::LicenceMenuState s = LicenceState(a, vol, &r);
    const uint32_t w = r.Get<uint32_t>(0x801C90A0u);
    const uint32_t view = screens::LicenceMenuView::kView;
    v->info = s;
    v->counter = r.Get<int16_t>(view + 0x14);
    v->arrowPhase = r.Get<int16_t>(view + 0x16);
    v->input = r.Get<int16_t>(view + 0x18) != 0;
    v->dialog = r.Get<int16_t>(view + 0x1A);
    v->carInfoFade = r.Get<int16_t>(w + 0x51C);
    v->rowEnabled = s.rowEnabled;
    v->list = MenuListWidget::Read(g, 0x8005B39Cu);
    v->AttachList();
    for (uint32_t row = 0; row < uint32_t(screens::kLicenceRows); row++) {
        v->rowText[row] = TextAt(r, w + 0x1F8 + row * 0x28);
        v->rowBand[row] = BandAtRam(g, w + 0x338 + row * 0x1C);
    }
    v->carLabel = LabelAtRam(r, g, w + 0x418);
    v->licenceLabel = LabelAtRam(r, g, w + 0x468);
    v->band = BandAtRam(g, w + 0x4BC);
    v->descBand = BandAtRam(g, w + 0x500);
    v->bar = *BarAt(r, w + 0xEC, true); // set up by 0x8004ED00 even while hidden
    v->lastTransmission = r.Get<uint8_t>(0x801D156Eu);
    return v;
}

// The fields of two licence views that the frames depend on (the dump's against ours after a run).
int CompareLicenceViews(const screens::LicenceMenuView& ours, const screens::LicenceMenuView& dump) {
    int bad = 0;
    auto check = [&](const char* what, int x, int y) {
        if (x != y) std::printf("race-menu-check: logic: %s ours %d, original %d\n", what, x, y), bad++;
    };
    check("counter", ours.counter, dump.counter), check("arrow phase", ours.arrowPhase, dump.arrowPhase), check("input", ours.input, dump.input);
    check("dialog", ours.dialog, dump.dialog), check("car info fade", ours.carInfoFade, dump.carInfoFade), check("test", ours.info.test, dump.info.test);
    check("list selection", ours.list.selection, dump.list.selection), check("list state", ours.list.state, dump.list.state);
    check("list fade", ours.list.fade, dump.list.fade), check("list scroll", ours.list.scroll, dump.list.scroll), check("list blink", ours.list.blink, dump.list.blink);
    check("list revealed", ours.list.revealed, dump.list.revealed);
    for (size_t k = 0; k < ours.rowText.size(); k++) {
        check("row text anim", ours.rowText[k].anim, dump.rowText[k].anim), check("row band anim", ours.rowBand[k].anim, dump.rowBand[k].anim);
        check("row text colour", int(ours.rowText[k].c0), int(dump.rowText[k].c0));
    }
    check("car label fade", ours.carLabel.fade, dump.carLabel.fade), check("car label band", ours.carLabel.band.anim, dump.carLabel.band.anim);
    check("car label text", ours.carLabel.text.anim, dump.carLabel.text.anim);
    check("licence label fade", ours.licenceLabel.fade, dump.licenceLabel.fade), check("licence label band", ours.licenceLabel.band.anim, dump.licenceLabel.band.anim);
    check("band anim", ours.band.anim, dump.band.anim), check("description band anim", ours.descBand.anim, dump.descBand.anim);
    check("bar anim", ours.bar.anim, dump.bar.anim), check("bar cursor", ours.bar.cursor, dump.bar.cursor);
    return bad;
}

// licence-flow: the licence menu of the dump from= (after field fromfield) run to tofield by our view manager (SessionViewStack)
// with the script's presses (pad of update f = the script's presses of field f - 1, as the records check): the menu's rows push
// SAVE REPLAY (screens::CardView on card= <mcd>, default work/memcards/card1.mcd) or the leave view 0x8005B428 (20 fields); the
// card view's exit pops back to the menu (0x8004ED00(1)). The frame of the run against the capture; with ram= (the dump of the
// capture) the licence view's state against ours when it is not the card view's turn.
int RunLicenceFlow(const DiscImage& disc, const GtfsVolume& vol, const RaceMenuAssets& assets, const LogicRun& run, const Ram* dump, const std::string& cardPath,
                   std::vector<MenuPrim>& prims, int& logicDiff) {
    Ram start;
    start.bytes = ReadFile(run.from);
    std::unique_ptr<screens::LicenceMenuView> first = LicenceViewFromRam(assets, vol, start);
    screens::LicenceMenuView* menu = first.get();
    const TitleAssets title = TitleAssets::Load(disc, vol);
    const TitleAssets cardAssets = screens::RaceCardAssets(assets, title);
    const shell::ReplayRowText rowText = shell::ReplayRowText::Load(vol, cardAssets);
    // A copy of the card: the manager may write it.
    const std::string card = (std::filesystem::temp_directory_path() / "gt2_licence_flow.mcd").string();
    std::filesystem::copy_file(cardPath, card, std::filesystem::copy_options::overwrite_existing);
    screens::SessionViewStack stack;
    stack.Start(std::move(first), false);
    screens::CardView* cardView = nullptr;
    for (int f = run.fromField + 1; f <= run.toField; f++) {
        MenuListPad pad;
        pad.pressed = ScriptPressed(run.script, f - 1);
        pad.held = pad.pressed;
        const int r = stack.Update(&pad);
        if (menu->testChanged) { // 0x8004CCF8: the selector's test (its texts, car and times)
            const screens::LicenceMenuState t = LicenceState(assets, vol, &start, menu->info.test);
            menu->info.title = t.title, menu->info.description = t.description, menu->info.carName = t.carName;
            menu->info.carPower = t.carPower, menu->info.carDrive = t.carDrive, menu->info.launchSpeed = t.launchSpeed, menu->info.medalTimes = t.medalTimes;
            std::printf("race-menu-check: logic: f%d the selector: test %d\n", f, menu->info.test + 1);
        }
        if (cardView && stack.Top() == cardView && r == 2) {
            std::printf("race-menu-check: logic: f%d SAVE REPLAY left: the menu again (0x8004ED00(1))\n", f);
            menu->Setup(menu->info, menu->rowEnabled[screens::kLicenceReplay], true);
            stack.Pop();
            cardView = nullptr;
        } else if (stack.Top() == menu && r == 1) {
            std::printf("race-menu-check: logic: f%d the menu is left, code %d\n", f, menu->Action());
            if (menu->Action() == -2) {
                auto v = std::make_unique<screens::CardView>(assets, cardAssets, rowText, screens::CardView::kSaveReplayView,
                                                             std::array<shell::CardSlot, 2>{shell::CardSlot{card}, shell::CardSlot{}});
                ReplayPayload payload; // the race of the start dump: 0x801D585C, 0x801DE8BA, 0x801D5E88, 0x801D5F84 (0x19 + its used bytes)
                std::memcpy(payload.race.data(), &start.bytes[0x1D585Cu], payload.race.size());
                ReplayPayload::Player p;
                p.params.assign(&start.bytes[0x1DE8BAu], &start.bytes[0x1DE8BAu] + 0x1C0);
                p.results.assign(&start.bytes[0x1D5E88u], &start.bytes[0x1D5E88u] + 0xFC);
                const uint32_t used = start.Get<uint32_t>(0x801D5F84u + 0x10);
                p.stream.assign(&start.bytes[0x1D5F84u], &start.bytes[0x1D5F84u] + std::min<size_t>(0x19 + used, 0x4400));
                payload.players.push_back(p);
                v->Manager().SetSaveData(PackReplayPayload(payload), ReplayDescription(payload));
                cardView = v.get();
                stack.Push(std::move(v));
            } else {
                stack.Push(std::make_unique<screens::WaitView>(assets, 0x8005B428u, 20));
            }
        }
    }
    std::printf("race-menu-check: logic: after the run: top %s, switch %d\n", stack.Top() == menu ? "the licence menu" : cardView ? "SAVE REPLAY" : "a wait view",
                stack.Transition());
    if (dump && ViewObject(*dump, screens::LicenceMenuView::kView)) {
        const std::unique_ptr<screens::LicenceMenuView> d = LicenceViewFromRam(assets, vol, *dump);
        logicDiff = CompareLicenceViews(*menu, *d);
        std::printf("race-menu-check: logic: %d licence view field(s) differ from the dump\n", logicDiff);
    }
    size_t modelAt = 0;
    std::optional<screens::PostRaceModel> model;
    prims = stack.Frame(assets, modelAt, model);
    std::filesystem::remove(card);
    return 0;
}


// The event menu view (race_menu_views.h) with every object of the dump (W = [0x801C90A0], view 0x8005D36C).
std::unique_ptr<screens::EventMenuView> EventViewFromRam(const RaceMenuAssets& a, const Ram& r) {
    auto v = std::make_unique<screens::EventMenuView>(a);
    const GuestImage g = RamImage(r);
    const screens::EventMenuState s = EventState(&r);
    const uint32_t w = r.Get<uint32_t>(0x801C90A0u);
    const uint32_t view = screens::EventMenuView::kView;
    v->title = s.title, v->titleColour = s.titleColour;
    v->counter = r.Get<int16_t>(view + 0x14);
    v->carShown = r.Get<int16_t>(view + 0x16);
    v->dialog = r.Get<int16_t>(view + 0x18);
    v->dialogCode = r.Get<int16_t>(view + 0x1A);
    v->machineTest = s.machineTest, v->ghostOptions = s.ghostOptions;
    v->rowEnabled = s.rowEnabled;
    v->settingsChosen = r.Get<uint8_t>(0x801C90F4u) != 0;
    v->noTransmission = ((r.Get<uint32_t>(0x801D5DE4u) >> 3) & 1) != 0;
    v->lastTransmission = r.Get<uint8_t>(0x801D156Eu);
    v->list = MenuListWidget::Read(g, 0x8005D284u);
    v->AttachList();
    for (uint32_t row = 0; row < uint32_t(screens::kEventRows); row++) {
        v->rowText[row] = TextAt(r, w + row * 0x28);
        v->rowBand[row] = BandAtRam(g, w + 0x140 + row * 0x1C);
    }
    v->course.band = BandAtRam(g, w + 0x440);
    v->course.text = TextAt(r, w + 0x45C);
    v->transmissionBar = *BarAt(r, w + 0x484, true);
    v->exitBar = *BarAt(r, w + 0x518, true);
    const uint32_t m = r.Get<uint32_t>(0x801C90A4u);
    v->car = r.Get<uint8_t>(m + 0x241) != 0;
    return v;
}

int CompareEventViews(const screens::EventMenuView& ours, const screens::EventMenuView& dump) {
    int bad = 0;
    auto check = [&](const char* what, int x, int y) {
        if (x != y) std::printf("race-menu-check: logic: %s ours %d, original %d\n", what, x, y), bad++;
    };
    check("counter", ours.counter, dump.counter), check("car shown", ours.carShown, dump.carShown), check("dialog", ours.dialog, dump.dialog);
    check("list selection", ours.list.selection, dump.list.selection), check("list state", ours.list.state, dump.list.state);
    check("list fade", ours.list.fade, dump.list.fade), check("list scroll", ours.list.scroll, dump.list.scroll), check("list blink", ours.list.blink, dump.list.blink);
    for (size_t k = 0; k < ours.rowText.size(); k++) {
        check("row text anim", ours.rowText[k].anim, dump.rowText[k].anim), check("row band anim", ours.rowBand[k].anim, dump.rowBand[k].anim);
        check("row text colour", int(ours.rowText[k].c0), int(dump.rowText[k].c0));
    }
    check("course band", ours.course.band.anim, dump.course.band.anim), check("course text", ours.course.text.anim, dump.course.text.anim);
    check("transmission bar anim", ours.transmissionBar.anim, dump.transmissionBar.anim), check("transmission bar cursor", ours.transmissionBar.cursor, dump.transmissionBar.cursor);
    check("exit bar anim", ours.exitBar.anim, dump.exitBar.anim), check("exit bar cursor", ours.exitBar.cursor, dump.exitBar.cursor);
    return bad;
}

// event-flow: as licence-flow for the event menu (the leave view 0x8005AE30; no card view: the row Save Replay is off before a
// race).
int RunEventFlow(const GtfsVolume& vol, const RaceMenuAssets& assets, const LogicRun& run, const Ram* dump, std::vector<MenuPrim>& prims, int& logicDiff) {
    (void)vol;
    Ram start;
    start.bytes = ReadFile(run.from);
    std::unique_ptr<screens::EventMenuView> first = EventViewFromRam(assets, start);
    screens::EventMenuView* menu = first.get();
    screens::SessionViewStack stack;
    stack.Start(std::move(first), false);
    for (int f = run.fromField + 1; f <= run.toField; f++) {
        MenuListPad pad;
        pad.pressed = ScriptPressed(run.script, f - 1);
        pad.held = pad.pressed;
        const int r = stack.Update(&pad);
        if (stack.Top() == menu && r == 1) {
            std::printf("race-menu-check: logic: f%d the menu is left, code %d (transmission %d)\n", f, menu->Action(), menu->transmission);
            stack.Push(std::make_unique<screens::WaitView>(assets, 0x8005AE30u, 20));
        }
    }
    if (dump && ViewObject(*dump, screens::EventMenuView::kView)) {
        const std::unique_ptr<screens::EventMenuView> d = EventViewFromRam(assets, *dump);
        logicDiff = CompareEventViews(*menu, *d);
        std::printf("race-menu-check: logic: %d event view field(s) differ from the dump\n", logicDiff);
    }
    size_t modelAt = 0;
    std::optional<screens::PostRaceModel> model;
    prims = stack.Frame(assets, modelAt, model);
    return 0;
}

} // namespace

int RunRaceMenuCheck(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv) {
    int at = 0;
    for (int i = 0; i < argc; i++)
        if (std::strcmp(argv[i], "--race-menu-check") == 0) at = i;
    if (at != 0 && at + 1 < argc && std::strcmp(argv[at + 1], "msettings-route") == 0)
        return RunMachineRoute(disc, vol, std::vector<std::string>(argv + at + 2, argv + argc));
    if (at != 0 && at + 1 < argc && IsRaceResultScreen(argv[at + 1])) // results / bonus / postmenu (race_result_check.h)
        return RunRaceResultCheck(disc, vol, std::vector<std::string>(argv + at + 1, argv + argc));
    if (at == 0 || at + 3 >= argc) {
        std::puts("usage: gt2game <disc> --race-menu-check <licence|event|parts|msettings> <cap.vram.bin> <side.png> [ram=<ram.bin>] [phase=N] [sel=N] [ps1] [msettings: flash=N slphase=N fade=N wstate=N band=N scroll=N]");
        return 2;
    }
    const std::string screen = argv[at + 1], capture = argv[at + 2], sidePath = argv[at + 3];
    std::string ramPath;
    int phase = -1000, selection = -1;
    bool ps1 = false;
    std::vector<std::pair<std::string, int>> overrides; // other key=value options (msettings)
    for (int i = at + 4; i < argc; i++) {
        const std::string a = argv[i];
        if (a.rfind("ram=", 0) == 0) ramPath = a.substr(4);
        else if (a.rfind("phase=", 0) == 0) phase = std::atoi(a.c_str() + 6);
        else if (a.rfind("sel=", 0) == 0) selection = std::atoi(a.c_str() + 4);
        else if (a == "ps1") ps1 = true;
        else if (const size_t eq = a.find('='); eq != std::string::npos && eq > 0) overrides.push_back({a.substr(0, eq), std::atoi(a.c_str() + eq + 1)});
    }
    Ram ram;
    if (!ramPath.empty()) ram.bytes = ReadFile(ramPath);
    const Ram* r = ramPath.empty() ? nullptr : &ram;

    std::vector<uint16_t> vram(1024 * 512);
    {
        const std::vector<uint8_t> bytes = ReadFile(capture);
        if (bytes.size() != vram.size() * 2) throw std::runtime_error(capture + ": not a 1024 x 512 VRAM dump");
        std::memcpy(vram.data(), bytes.data(), bytes.size());
    }

    std::vector<MenuPrim> prims;
    int logicDiff = 0; // newrecord / records with from=: state fields of our logic run that differ from the dump
    RaceMenuAssets::Pictures pictures = RaceMenuAssets::Pictures::kLicence;
    if (screen == "event" || screen == "parts" || screen == "msettings") pictures = RaceMenuAssets::Pictures::kSettings;
    else if (screen != "licence" && screen != "newrecord" && screen != "records") {
        std::printf("race-menu-check: unknown screen %s (licence, event, parts, msettings)\n", screen.c_str());
        return 2;
    }
    const RaceMenuAssets assets = RaceMenuAssets::Load(disc, vol, pictures);
    if (screen == "newrecord") { // needs ram=
        if (!r) throw std::runtime_error("newrecord: ram=<ram.bin> is required");
        screens::NameEntry kb(assets);
        NewRecordFromRam(ram, kb);
        const LogicRun run = LogicRun::Parse(argc, argv, at + 4);
        std::unique_ptr<screens::NameEntry> simulated;
        if (run.Enabled()) { // 0x8004E658 / 0x80073720 field by field from the earlier dump
            Ram start;
            start.bytes = ReadFile(run.from);
            simulated = std::make_unique<screens::NameEntry>(assets);
            NewRecordFromRam(start, *simulated);
            simulated->sound = [](int id) { std::printf("race-menu-check: logic: sound %d\n", id); };
            for (int f = run.fromField + 1; f <= run.toField; f++) {
                MenuListPad pad;
                pad.pressed = ScriptPressed(run.script, f - 1);
                const int result = simulated->Update(&pad);
                if (result == screens::NameEntry::kCancel) std::printf("race-menu-check: logic: f%d CANCEL (the view: sound 0, stays)\n", f);
                if (result == screens::NameEntry::kOk) std::printf("race-menu-check: logic: f%d OK (the view stores and closes)\n", f);
            }
            logicDiff = CompareNewRecord(*simulated, kb);
        }
        const screens::NameEntry& shown = simulated ? *simulated : kb;
        std::printf("race-menu-check: NEW RECORD \"%s\" caret %d, row %d column %d, anim %d pulse %d caret phase %d; list sel %d scroll %d fade %d state %d, band %d\n",
                    shown.name.c_str(), shown.caret, shown.row, shown.column, shown.anim, shown.pulse, shown.caretPhase, shown.list.selection, shown.list.scroll,
                    shown.list.fade, shown.list.state, shown.band.anim);
        prims = screens::BuildNewRecordFrame(assets, shown);
    } else if (screen == "records") { // needs ram=
        if (!r) throw std::runtime_error("records: ram=<ram.bin> is required");
        screens::RecordsState s = RecordsFromRam(assets, ram);
        if (const LogicRun run = LogicRun::Parse(argc, argv, at + 4); run.Enabled()) { // 0x8004FE40 field by field
            Ram start;
            start.bytes = ReadFile(run.from);
            const screens::RecordsState s0 = RecordsFromRam(assets, start);
            screens::RecordsView view(assets);
            view.licence = s0.licence, view.test = s0.test, view.slide = s0.slide, view.arrowPhase = s0.arrowPhase, view.active = s0.active, view.band = s0.band;
            view.delay = start.Get<int16_t>(start.Get<uint32_t>(start.Get<uint32_t>(0x801C90A4u) + 0x1C8) + 0x14);
            view.sound = [](int id) { std::printf("race-menu-check: logic: sound %d\n", id); };
            for (int f = run.fromField + 1; f <= run.toField; f++) {
                MenuListPad pad;
                pad.pressed = ScriptPressed(run.script, f - 1);
                if (view.Update(&pad, view.active)) std::printf("race-menu-check: logic: f%d the view is left\n", f);
            }
            const int dumpDelay = ram.Get<int16_t>(ram.Get<uint32_t>(ram.Get<uint32_t>(0x801C90A4u) + 0x1C8) + 0x14);
            int bad = 0;
            auto check = [&](const char* what, int a, int b) {
                if (a != b) std::printf("race-menu-check: logic: %s ours %d, original %d\n", what, a, b), bad++;
            };
            check("test", view.test, s.test), check("slide", view.slide, s.slide), check("arrow phase", view.arrowPhase, s.arrowPhase);
            check("active", view.active, s.active), check("band anim", view.band.anim, s.band.anim), check("delay", view.delay, dumpDelay);
            std::printf("race-menu-check: logic: %d state field(s) differ from the dump\n", bad);
            logicDiff = bad;
            // The frame of our state (the record rows as the dump's career holds them).
            s.test = view.test, s.slide = view.slide, s.arrowPhase = view.arrowPhase, s.active = view.active, s.band = view.band;
        }
        if (phase != -1000) s.arrowPhase = phase;
        std::printf("race-menu-check: RECORD licence %d test %d \"%s\", %d row(s), slide %d, arrow phase %d, active %d, band %d\n", s.licence, s.test, s.title.c_str(),
                    s.count, s.slide, s.arrowPhase, s.active ? 1 : 0, s.band.anim);
        prims = screens::BuildRecordsFrame(assets, s);
    } else if (screen == "event" && LogicRun::Parse(argc, argv, at + 4).Enabled()) { // event-flow (see RunEventFlow)
        RunEventFlow(vol, assets, LogicRun::Parse(argc, argv, at + 4), r, prims, logicDiff);
    } else if (screen == "licence" && LogicRun::Parse(argc, argv, at + 4).Enabled()) { // licence-flow (see RunLicenceFlow)
        std::string cardPath = "work/memcards/card1.mcd";
        for (int i = at + 4; i < argc; i++)
            if (std::strncmp(argv[i], "card=", 5) == 0) cardPath = argv[i] + 5;
        RunLicenceFlow(disc, vol, assets, LogicRun::Parse(argc, argv, at + 4), r, cardPath, prims, logicDiff);
    } else if (screen == "licence" && r && ViewObject(*r, screens::LicenceMenuView::kView) &&
               std::any_of(argv + at + 4, argv + argc, [](const char* x) { return std::strcmp(x, "view") == 0; })) {
        // Every object of the view from the dump (its animations; race_menu_views.h), drawn settled by the manager (no switch).
        const std::unique_ptr<screens::LicenceMenuView> v = LicenceViewFromRam(assets, vol, ram);
        std::printf("race-menu-check: licence view: counter %d input %d dialog %d, list state %d fade %d sel %d, band %d, labels %d / %d, bar %d\n", v->counter,
                    v->input ? 1 : 0, v->dialog, v->list.state, v->list.fade, v->list.selection, v->band.anim, v->carLabel.fade, v->licenceLabel.fade, v->bar.anim);
        prims = screens::BuildPostRaceFrame(assets, *v, 128);
    } else if (screen == "licence") {
        screens::LicenceMenuState s = LicenceState(assets, vol, r);
        if (phase != -1000) s.flashPhase = phase;
        if (selection >= 0) s.selectedRow = selection;
        std::printf("race-menu-check: licence %d test %d, row %d phase %d, car \"%s\"\n", s.licence, s.test, s.selectedRow, s.flashPhase, s.carName.c_str());
        prims = screens::BuildLicenceMenuFrame(assets, s);
    } else if (screen == "msettings" && r) {
        // The interactive page (states 0 / 1 / 2) from the dump; overrides: phase= arrow phase (+0xE), sel= group,
        // flash= row flash (+0x10), slphase= the selected popup row's slider phase, fade= widget fade (+0x24),
        // wstate= widget state (+0x26), band= popup band anim (0x8005D1A0), scroll= group scroll (+0xC).
        const career::CareerData data = career::CareerData::Load(disc, vol);
        screens::MachineSettingsPage page;
        auto sheet = std::make_unique<career::TuneSheet>();
        MachinePage(ram, assets, data, page, *sheet);
        if (phase != -1000) page.arrowPhase = int16_t(phase);
        if (selection >= 0) page.group = int8_t(selection);
        for (const auto& [key, value] : overrides) {
            if (key == "flash") page.flash = int16_t(value);
            else if (key == "slphase" && page.widget.selection >= 0 && page.widget.selection < 16) page.sliders[size_t(page.widget.selection)].phase = int16_t(value);
            else if (key == "fade") page.widget.fade = int16_t(value);
            else if (key == "wstate") page.widget.state = int16_t(value);
            else if (key == "band") page.popupBand.anim = int16_t(value);
            else if (key == "scroll") page.scroll = int16_t(value);
        }
        std::printf("race-menu-check: PARTS SETTING state %d group %d of %d row %d, scroll %d, arrow %d, flash %d, input %d; widget count %d sel %d "
                    "state %d fade %d scroll %d, popup band %d, page band %d\n",
                    page.state, page.group, page.groupCount, page.row, page.scroll, page.arrowPhase, page.flash, page.input ? 1 : 0, page.widget.count,
                    page.widget.selection, page.widget.state, page.widget.fade, page.widget.scroll, page.popupBand.anim, page.band.anim);
        for (int i = 0; i < page.entryCount && i < 16; i++) {
            const screens::MachineSettingsPage::Slider& s = page.sliders[size_t(i)];
            std::printf("race-menu-check:   slider %d \"%s\" format %d value %d (%d..%d) field %d phase %d\n", i, assets.Text(s.label).c_str(), s.format, s.value, s.min,
                        s.max, s.field, s.phase);
        }
        prims = page.Frame(assets, *sheet, data);
    } else if (screen == "msettings") {
        screens::MachineSettingsState s = MachineState(r);
        if (phase != -1000) s.arrowPhase = phase;
        if (selection >= 0) s.group = selection;
        std::printf("race-menu-check: machine settings group %d of %zu, arrow phase %d\n", s.group, s.groups.size(), s.arrowPhase);
        prims = screens::BuildMachineSettingsFrame(assets, s);
    } else if (screen == "parts") {
        screens::PartsPageState s = PartsState(r);
        if (phase != -1000) s.arrowPhase = phase;
        if (selection >= 0) s.group = selection;
        std::printf("race-menu-check: parts group %d of %d (table %d), arrow phase %d\n", s.group, s.groupCount, s.groupTable, s.arrowPhase);
        prims = screens::BuildPartsPageFrame(assets, s);
    } else {
        screens::EventMenuState s = EventState(r);
        if (phase != -1000) s.flashPhase = phase;
        if (selection >= 0) s.selectedRow = selection;
        std::printf("race-menu-check: event \"%s\" / \"%s\", row %d phase %d\n", s.title.c_str(), s.course.c_str(), s.selectedRow, s.flashPhase);
        prims = screens::BuildEventMenuFrame(assets, s);
    }
    const size_t fontDiff = CompareVramRegion(assets.vram, vram, 384, 0, 128, 256);
    const size_t picDiff = CompareVramRegion(assets.vram, vram, 384, 256, pictures == RaceMenuAssets::Pictures::kLicence ? 128 : 64,
                                             pictures == RaceMenuAssets::Pictures::kLicence ? 170 : 235);
    std::printf("race-menu-check: VRAM font pages 6/7: %zu differing words, pictures page 0x16: %zu\n", fontDiff, picDiff);
    std::printf("race-menu-check: %zu primitives\n", prims.size());
    const MenuCanvas canvas = screens::RenderRaceMenuFrame(assets, prims, !ps1);
    return Compare(canvas, vram, sidePath) == 0 && logicDiff == 0 ? 0 : 1;
}

gt2::screens::EventMenuState EventMenuStateOfRam(const std::vector<uint8_t>& bytes) { // race_result_check.cpp "eventmenu"
    Ram ram;
    ram.bytes = bytes;
    return EventState(&ram);
}
