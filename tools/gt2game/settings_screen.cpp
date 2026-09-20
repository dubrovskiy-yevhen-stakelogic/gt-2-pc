// Machine settings before a race: the race overlay's PARTS SETTING page (see settings_screen.h).
#include "settings_screen.h"

#include "platform/os/keys.h"
#include "platform/os/paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "game_window.h"
#include "gt2formats/gt_menu_list.h"
#include "game/shell/title_draw.h"
#include "gt2view/change_parts.h"
#include "gt2view/race_menus.h"
#include "gt2view/race_result_screens.h"
#include "panel.h"

using namespace gt2;
using namespace gt2::career;

namespace gt2game {

namespace {

const char* SettingName(int32_t k) {
    static const char* const kNames[kSettingCount] = {"Springs", "Ride height", "Dampers (bound)", "Dampers (rebound)", "Camber", "Toe", "Stabilizers",
                                                      "Brake balance", "Gear ratio", "Gear auto-set", "LSD initial", "LSD accel", "LSD decel",
                                                      "LSD rear initial", "Active stability", "Traction control", "Downforce"};
    return (k >= 0 && k < kSettingCount) ? kNames[k] : "?";
}

std::string EntryName(int32_t setting, int32_t entry, int32_t count) {
    if (setting == kSettingGears) return entry == count - 1 ? "final" : std::to_string(entry + 1) + (entry == 0 ? "st" : entry == 1 ? "nd" : entry == 2 ? "rd" : "th");
    if (count == 2) return entry == 0 ? "front" : "rear";
    if (count > 2) return "#" + std::to_string(entry + 1);
    return "";
}

// Keyboard -> the page's pad bits (gt2formats/gt_menu_list.h menu_list_pad).
struct KeyBit {
    int key;
    uint32_t bit;
};
constexpr KeyBit kKeys[] = {
    {gt2::keys::kUp, menu_list_pad::kUp},       {gt2::keys::kDown, menu_list_pad::kDown},    {gt2::keys::kLeft, menu_list_pad::kLeft},  {gt2::keys::kRight, menu_list_pad::kRight},
    {gt2::keys::kReturn, menu_list_pad::kCross}, {gt2::keys::kBack, menu_list_pad::kTriangle}, {gt2::keys::kEscape, menu_list_pad::kTriangle},
    {'Q', menu_list_pad::kL1},          {gt2::keys::kPageUp, menu_list_pad::kL1},      {'W', menu_list_pad::kR1},        {gt2::keys::kPageDown, menu_list_pad::kR1},
    {gt2::keys::kHome, menu_list_pad::kStart},
    // The names gt2game's scripts use elsewhere (game_window.cpp): circle = Space, square = Delete, start = S.
    {gt2::keys::kSpace, menu_list_pad::kCircle}, {gt2::keys::kDelete, menu_list_pad::kSquare}, {'S', menu_list_pad::kStart},
};
// Observed auto-repeat of the original's pad (run2 of work/re/spec_msettings: right held 40 fields gave steps at the
// press, 28 fields later and 8 fields after that).
constexpr int kRepeatFirst = 28, kRepeatPeriod = 8;
constexpr uint32_t kRepeatBits = menu_list_pad::kUp | menu_list_pad::kDown | menu_list_pad::kLeft | menu_list_pad::kRight;

int BitIndex(uint32_t bit) {
    int i = 0;
    while (i < 32 && !(bit & (1u << i))) i++;
    return i;
}

} // namespace

SettingsScreen::SettingsScreen(career::GarageCar& car, const CareerData& data)
    : car_(car), data_(data), sheet_(std::make_unique<TuneSheet>()), page_(std::make_unique<screens::MachineSettingsPage>()) {}
SettingsScreen::~SettingsScreen() = default;

void SettingsScreen::Open() {
    LoadCarSheet(*sheet_, car_, data_.tables); // 0x800173E8
    changed_ = false;
    entered_ = false;
    countdown_ = 0;
    heldFor_.fill(-1);
    page_ = std::make_unique<screens::MachineSettingsPage>();
    parts_.reset();
    partsFirst_ = true; // the event menu's "Settings ..." opens CHANGE PARTS (view 0x8005D1C0); its L1 PARTS SETTING
    Reload();
}

void SettingsScreen::OpenParts() { // 0x80048374(M, 0x8005D1C0) -> 0x800572C4
    screens::ChangePartsContext c;
    c.sheet = sheet_.get();
    c.data = &data_;
    c.garageCar = &car_;
    c.powerLimit = powerLimit_;
    c.restrictions = raceFlags_;
    parts_ = std::make_unique<screens::ChangePartsView>(*assets_, c);
}

void SettingsScreen::Reload() {
    rows_.clear();
    values_.assign(size_t(kSettingCount), {});
    for (int32_t k = 0; k < kSettingCount; k++) {
        SettingValue v[9]{};
        const int32_t n = GetSetting(*sheet_, k, v, data_); // 0x8005FC9C: -1 = the parts do not allow it
        if (n < 1) continue;
        values_[size_t(k)].assign(v, v + n);
        for (int32_t e = 0; e < n; e++) rows_.push_back({k, e});
    }
}

MenuListPad SettingsScreen::ReadPad(GameWindow& window) {
    MenuListPad pad;
    for (const KeyBit& k : kKeys) {
        if (window.Pressed(k.key)) pad.pressed |= k.bit;
        if (window.Held(k.key) || window.Pressed(k.key)) pad.held |= k.bit; // held at some time since the last read
    }
    for (uint32_t bit = 1; bit != 0 && bit <= menu_list_pad::kSelect; bit <<= 1) {
        int& n = heldFor_[size_t(BitIndex(bit))];
        if (pad.pressed & bit) n = 0;
        else if (pad.held & bit) n = n >= 0 ? n + 1 : 0;
        else n = -1;
        if ((bit & kRepeatBits) && n >= kRepeatFirst && (n - kRepeatFirst) % kRepeatPeriod == 0) pad.repeat |= bit;
    }
    return pad;
}

bool SettingsScreen::Update(GameWindow& window) {
    const MenuListPad pad = ReadPad(window);
    if (!assets_) return true; // the page's image comes with the panels (first Draw)
    if (partsFirst_) {
        partsFirst_ = false;
        OpenParts();
    }
    if (parts_) { // CHANGE PARTS (0x8005731C)
        const int r = parts_->Update(&pad, true);
        sounds = parts_->sounds;
        if (parts_->changes > 0) changed_ = true;
        if (r == 0) return true;
        const bool leave = parts_->exit == screens::ChangePartsView::kLeave;
        std::printf("settings: CHANGE PARTS left (%d stage(s) selected)%s\n", parts_->changes, leave ? "" : ", L1: PARTS SETTING");
        parts_.reset();
        Reload();
        if (leave) { // -4: 0x80056FF0, the manager goes back
            Commit();
            return false;
        }
        Commit(); // -8: 0x80056FF0, then 0x80048374(M, 0x8005D1E4): PARTS SETTING entered again
        page_ = std::make_unique<screens::MachineSettingsPage>();
        entered_ = false;
        countdown_ = 0;
        return true;
    }
    if (!entered_) { // 0x8005747C: view +0x14 = 12, 0x80055E90(P, 0, 100)
        page_->Init(assets_->ovl0, *sheet_, data_);
        countdown_ = 12;
        entered_ = true;
    }
    // 0x800574C0.
    if (countdown_ > 0 && --countdown_ == 0) page_->StartOpen(); // 0x80055FD0
    std::vector<uint8_t> before(sizeof(TuneSheet));
    std::memcpy(before.data(), sheet_.get(), before.size());
    const int code = page_->Update(&pad, true, *sheet_, data_);
    sounds = page_->sounds;                                           // the slider's 0x80060840(8)
    if (const int s = screens::MachineSettingsPage::Sound(code); s >= 0) sounds.push_back(s); // 0x800574C0
    if (std::memcmp(before.data(), sheet_.get(), before.size()) != 0) { // 0x8005F9DC (cross in the popup) / 0x80060410 (start)
        changed_ = true;
        Reload();
    }
    if (code == screens::MachineSettingsPage::kChangeParts) { // 0x800574C0: -8 -> 0x80056FF0, 0x80048374(M, 0x8005D1C0)
        page_->StartClose();
        Commit();
        OpenParts();
        std::printf("settings: R1 = CHANGE PARTS (view 0x8005D1C0)\n");
    } else if (code == screens::MachineSettingsPage::kLeave) { // 0x80056FF0 + 0x80055FE0
        page_->StartClose();
        Commit();
        return false;
    }
    return true;
}

void SettingsScreen::Commit() {
    if (!changed_) return;
    SettingsCommit out; // 0x80056FF0: the garage slot (the race car's configuration is rebuilt by the caller from it)
    out.garageCar = &car_;
    CommitSettings(*sheet_, data_.tables, BuildScratch{scratch_.data()}, out);
    std::printf("settings: committed to the garage car (0x80056FF0)\n");
    for (const std::string& line : Describe()) std::printf("  %s\n", line.c_str());
}

std::vector<std::string> SettingsScreen::Describe() const {
    std::vector<std::string> out;
    for (const Row& r : rows_) {
        const std::vector<SettingValue>& v = values_[size_t(r.setting)];
        const SettingValue& e = v[size_t(r.entry)];
        char text[128];
        std::snprintf(text, sizeof text, "%s %s: %d (%d..%d)", SettingName(r.setting), EntryName(r.setting, r.entry, int32_t(v.size())).c_str(), e.value, e.min, e.max);
        out.push_back(text);
    }
    return out;
}

void SettingsScreen::Draw(Panels& p) const {
    assets_ = &p.MenuAssets(Panels::Screen::kSettings);
    if (partsFirst_) { // the first field: CHANGE PARTS is created by the next Update (its assets come with this Draw)
        p.FullScreen(shell::TitleFrameStart(), Panels::Screen::kSettings);
        return;
    }
    if (parts_) { // 0x800479AC: the view's header and 0x80053CA8
        std::vector<MenuPrim> prims = shell::TitleFrameStart();
        const std::vector<MenuPrim> part = screens::BuildPostRaceViewPart(*assets_, *parts_, 128, true);
        prims.insert(prims.end(), part.begin(), part.end());
        p.FullScreen(prims, Panels::Screen::kSettings);
        return;
    }
    // Before the page is entered (the first field) the page object is empty: its band is closed and only the view
    // header is drawn, as in the original's fields before 0x80055FD0.
    p.FullScreen(page_->Frame(*assets_, *sheet_, data_), Panels::Screen::kSettings);
}

} // namespace gt2game
