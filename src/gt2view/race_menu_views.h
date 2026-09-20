#pragma once
// The race overlay's GT-mode pre-race menus as views of the view manager (0x800474F4 / 0x800479AC / 0x800483A4 / 0x800483D8):
// the licence test menu (view 0x8005B470) and the event menu "SINGLE RACE" (view 0x8005D36C) with their objects, so that their
// open / close animations and the manager's slide transitions (16 fields, both views drawn shifted) are those of the original.
// The settled frames of race_menus.h (BuildLicenceMenuFrame / BuildEventMenuFrame) are these views with settled objects.
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); code addresses in GT2.OVL
// member 0 (ovl0, at 0x80010000) unless marked EXE; evidence: our disassembly / Ghidra pseudo-C of work/re/rs_licmenu and the
// GP0 captures work/play/atmt/cap (gt2play --prims; docs/formats/race_screens.md 5.6).
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "gt2view/race_menus.h"
#include "gt2view/race_result_screens.h"

namespace gt2::screens {

// View 0x8005B470: setup 0x8004ED00, update 0x8004EEB0, draw 0x8004F474 (W = [0x801C90A0]).
class LicenceMenuView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B470u;
    explicit LicenceMenuView(const RaceMenuAssets& a);
    LicenceMenuView(const LicenceMenuView&) = delete;
    LicenceMenuView& operator=(const LicenceMenuView&) = delete;

    // 0x8004ED00(again): view + 0x14 = 12 (the objects open when it runs out), + 0x1A = 0; Replay / Save Replay by `replay`
    // (M + 0x240); a first setup (again false) resets the list on Start (row 2); the labels, the bands (opening from 0), the
    // TRANSMISSION bar (0x8005AC20 at (0xB0, 0x19A), hidden), block + 0x51C = 8. `info` = the menu of the race's test (block
    // + 0x4B8 = race block + 0xC: info.test).
    void Setup(const LicenceMenuState& info, bool replay, bool again);
    // 0x8004EEB0: one update (`input` = the manager's argument: the current view). Returns 1 when the view is left.
    int Update(const MenuListPad* pad, bool input = true) override;
    void Draw(MenuOt& ot) const override; // 0x8004F474
    std::string Title() const override;   // view + 0x10 (none)
    uint32_t Colour() const override;     // view + 0x0C (0xF07800)
    void AttachList();

    // Once Update returned 1: the chosen row's code - 1 Start (after the TRANSMISSION bar: `transmission`), 0 Replay, 2 Exit
    // (M + 0x7C, view 0x8005B428 follows), -2 Save Replay ... (view 0x8005B51C), -4 Records ... (0x8005B4DC), -5 Demonstration
    // ... (0x8005B44C).
    int Action() const { return action_; }
    int transmission = 0;        // the bar's choice of the Start: 0 AT, 1 MT (0x801D156E = 0x801D5947)
    uint8_t lastTransmission = 0; // 0x801D156E: the bar opens on it (W + 0x169 = (0x801D156E != 0))
    bool testChanged = false;    // the last update moved the selector: the caller puts that test's menu into `info` (0x8004CCF8)
    // Closes the objects as a leaving row does (0x8004EEB0's end: the list 0x8006CED8, the bands, the labels).
    void Close();

    LicenceMenuState info;       // texts / car / times / medals of info.test (block + 0x4B8)
    int16_t counter = 12;        // view + 0x14
    int16_t arrowPhase = 0;      // view + 0x16 (0..45)
    bool input = false;          // view + 0x18: the last update's pad argument (the title and description are drawn only then)
    int16_t dialog = 0;          // view + 0x1A: 1 = the TRANSMISSION bar is up
    int16_t carInfoFade = 8;     // block + 0x51C
    std::array<bool, kLicenceRows> rowEnabled{true, true, true, true, true, true, true}; // 0x8005B368 + row * 8
    MenuListWidget list;         // 0x8005B39C (row callback 0x8004D474)
    std::array<TextObject, kLicenceRows> rowText;   // W + 0x1F8 + row * 0x28
    std::array<shell::Band, kLicenceRows> rowBand;  // W + 0x338 + row * 0x1C
    ResultLabel carLabel, licenceLabel;             // W + 0x418 "CAR INFO" / W + 0x468 "LICENSE INFO"
    shell::Band band, descBand;                     // W + 0x4BC (template 0x8005B238) / W + 0x500 (0x8005B294)
    ResultBar bar;                                  // W + 0xEC

private:
    int32_t ListCallback(int command, int row, const MenuListRowDraw* d); // 0x8004D474
    const RaceMenuAssets& a_;
    mutable MenuOt* drawOt_ = nullptr;
    int action_ = 0;
};

// View 0x8005D36C "SINGLE RACE": setup 0x80057EAC, update 0x80058108, draw 0x800585C0 (the rows of sub-mode 1).
class EventMenuView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005D36Cu;
    explicit EventMenuView(const RaceMenuAssets& a);
    EventMenuView(const EventMenuView&) = delete;
    EventMenuView& operator=(const EventMenuView&) = delete;

    // 0x80057EAC(again): view + 0x14 = 16, + 0x16 = -1 (no car), + 0x18 = 0; the bars "Exit?" (0x8005ACF0) and TRANSMISSION
    // (0x8005AC20) at (0xB0, 0x19A); Replay / Save Replay enabled by `replay` && !settingsChosen (0x801C90F4, cleared by a first
    // setup); the list reset on row 0 by a first setup (the selection kept by a second); the car camera when `car` (M + 0x241);
    // the course title (0x80048BD8, opened).
    void Setup(const EventMenuState& info, bool replay, bool car, bool again);
    int Update(const MenuListPad* pad, bool input = true) override; // 0x80058108
    void Draw(MenuOt& ot) const override;                          // 0x800585C0 (without the car: Model())
    std::optional<PostRaceModel> Model() const override;           // car shown (view + 0x16 >= 0 and M + 0x241): (0x7C, 0xA0, 200, 200)
    std::string Title() const override { return title; }
    uint32_t Colour() const override { return titleColour; }
    void AttachList();

    // Once Update returned 1: the row's code (M + 0x7C) - 0 Replay, 1 Test Run, 4 Start Race (both after the TRANSMISSION bar
    // unless `noTransmission`: race block + 0x588 bit 3), 2 Exit (after "Exit?" Yes), -2 Save Replay, -3 Settings.
    int Action() const { return action_; }
    int transmission = 0;
    uint8_t lastTransmission = 0;
    bool noTransmission = false; // race block + 0x588 bit 3 (0x80011F64: the garage car's gearbox has fewer than 3 gears)
    bool settingsChosen = false; // 0x801C90F4
    bool machineTest = false;    // race mode 7..9: the rows 0x8005D2B8 / offsets 0x8005D2E8 (drawn only; not run by gt2game)
    bool ghostOptions = false;   // race mode 10: seven rows (drawn only)
    void Close();

    std::string title;           // view + 0x10 ("SINGLE RACE" 0x801C6E29; a series: "SESSION %d")
    uint32_t titleColour = 0;    // view + 0x0C
    int16_t counter = 16;        // view + 0x14
    int16_t carShown = -1;       // view + 0x16
    int16_t dialog = 0;          // view + 0x18: 1 "Exit?", 2 TRANSMISSION
    int16_t dialogCode = 0;      // view + 0x1A: the row code that opened the TRANSMISSION bar
    bool car = false;            // M + 0x241
    std::array<bool, kEventRows> rowEnabled{false, true, true, false, true, true, true};
    MenuListWidget list;         // 0x8005D284 (row callback 0x80057A70)
    std::array<TextObject, kEventRows> rowText;     // W + row * 0x28
    std::array<shell::Band, kEventRows> rowBand;    // W + 0x140 + row * 0x1C
    CourseTitle course;          // W + 0x440
    ResultBar transmissionBar, exitBar;             // W + 0x484 / W + 0x518
    menu::OverlayModelCamera carCamera;             // W + 0x364 (0x80049780(200, 200), turned by 16 per update)
    int32_t frameLength = 256;   // M + 0x234

private:
    int32_t ListCallback(int command, int row, const MenuListRowDraw* d); // 0x80057A70
    const RaceMenuAssets& a_;
    mutable MenuOt* drawOt_ = nullptr;
    int action_ = 0;
};

// The settled views of the race_menus.h states (BuildLicenceMenuFrame / BuildEventMenuFrame draw them).
std::unique_ptr<LicenceMenuView> SettledLicenceMenuView(const RaceMenuAssets& a, const LicenceMenuState& st);
std::unique_ptr<EventMenuView> SettledEventMenuView(const RaceMenuAssets& a, const EventMenuState& st);

// A view drawn / updated through another owner's object (the menus lent to a modal screen's view manager, e.g. SAVE REPLAY).
class BorrowedView : public PostRaceView {
public:
    explicit BorrowedView(PostRaceView& v) : v_(v) {}
    std::optional<PostRaceModel> Model() const override { return v_.Model(); }
    int Update(const MenuListPad* pad, bool input = true) override {
        const int r = v_.Update(pad, input);
        sounds = v_.sounds;
        return r;
    }
    void Draw(MenuOt& ot) const override { v_.Draw(ot); }
    std::string Title() const override { return v_.Title(); }
    uint32_t Colour() const override { return v_.Colour(); }
    PostRaceView& Inner() const { return v_; }
private:
    PostRaceView& v_;
};

} // namespace gt2::screens
