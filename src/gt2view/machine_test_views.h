#pragma once
// The race overlay's views of the GT-mode MACHINE TEST (race sub-modes 7 / 8 / 9: 0-400 m, 0-1000 m, Max Speed) in the view
// manager (0x800474F4 / 0x800479AC, race_session_screens.h SessionViewStack / race_result_screens.h PostRaceFlow):
//   the menu      view 0x8005D390 "MACHINE TEST": setup 0x800587BC, update 0x800589BC, draw 0x80058D20 (pushed by the view
//                 0x8005D348 after 20 fields: 0x80057D24 / 0x80057D74)
//   NEW RECORD    view 0x8005B7E8 "ENTER YOUR NAME": setup 0x80051F54, update 0x80051FC4, draw 0x80052144 (the keyboard)
//   RESULTS       view 0x8005B80C "RESULTS": setup 0x80052170, update 0x8005232C, draw 0x80052638 (the rank, the record)
//   RECORD        view 0x8005D3B4 "RECORD": setup 0x8005916C, update 0x8005918C, draw 0x80059240 (rows 0x80058E28)
// After the race of a test and its replay (the loop's state 6, 0x80017200 for sub-modes 7..9): the wait view 0x8005B7A0 (24
// fields; 0x80050EE4 writes the record, 0x80050D78 = career::WriteMachineTestRecord) -> NEW RECORD when the entry ranked (music
// 0x800481C8(8)) -> RESULTS -> "Save" SAVE GAME 0x8005B588 / "Next" the leave view 0x8005AE30 -> 0x8005D348 -> the menu.
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); code addresses in GT2.OVL member 0
// (ovl0, at 0x80010000) unless marked EXE. Evidence: our disassembly and Ghidra pseudo-C of the race overlay (work/re/mtest/ghidra,
// dump work/re/mtest/s7/ram_007500.bin), GP0 captures of the original's 0-400 m test (gt2play --prims, work/play/mtest/cap;
// docs/formats/race_screens.md section 5.7). W = the work block *0x801C90A0, M = the view manager *0x801C90A4.
#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "game/career/career_state.h"
#include "game/career/machine_test.h"
#include "gt2view/race_menu_views.h"
#include "gt2view/race_record_screens.h"
#include "gt2view/race_result_screens.h"

namespace gt2::screens {

// Rows of the machine-test menu (widget 0x8005D2F0, row table 0x8005D2B8 + row * 8 {string, s8 enabled, s8 code}, y offsets
// 0x8005D2E8): Replay (0), Start / Try Again (1), Settings ... (-3), Records ... (-4), Save Replay ... (-2), Exit (2).
enum MachineTestRow : int { kMtReplay = 0, kMtStart, kMtSettings, kMtRecords, kMtSaveReplay, kMtExit, kMtRows };

// View 0x8005D390 (colour 0xF4BE73, title "MACHINE TEST" 0x801C7A25).
class MachineTestMenuView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005D390u;
    explicit MachineTestMenuView(const RaceMenuAssets& a);
    MachineTestMenuView(const MachineTestMenuView&) = delete;
    MachineTestMenuView& operator=(const MachineTestMenuView&) = delete;

    // 0x800587BC(again): view + 0x14 = 16, + 0x16 = -1, + 0x18 = 0; TRANSMISSION (0x8005AC20) at (0xB0, 0x19A); a first setup
    // (again false) clears 0x801C90F4 (settingsChosen); Replay / Save Replay enabled by 0x801C90F4 == 0 && M + 0x240 (`replay`);
    // row 1 "Start" (0x801C7097), "Try Again" (0x801C6E5E) when a replay exists; the list reset (a second setup keeps the
    // selection); the car (M + 0x241: camera 0x80049780(200, 200)); the course title 0x80048BD8 of the sub-mode's test name
    // (career::MachineTestCourseTitle) opened.
    void Setup(int subMode, bool replay, bool car, bool again);
    int Update(const MenuListPad* pad, bool input = true) override; // 0x800589BC
    void Draw(MenuOt& ot) const override;                          // 0x80058D20 (without the car: Model())
    std::optional<PostRaceModel> Model() const override;           // view + 0x16 >= 0 and M + 0x241: (0x7C, 0xA0, 200, 200)
    std::string Title() const override;
    uint32_t Colour() const override;
    void AttachList();

    // Once Update returned 1: the row's code (M + 0x7C) - 0 Replay, 1 Start / Try Again (after the TRANSMISSION bar unless
    // `noTransmission`), 2 Exit (no dialog), -2 Save Replay (view 0x8005B51C), -3 Settings (0x8005D1C0), -4 Records (0x8005D3B4).
    int Action() const { return action_; }
    int transmission = 0;          // the bar's choice (0x801D156E = 0x801D5947)
    uint8_t lastTransmission = 0;  // 0x801D156E: the bar opens on it (W + 0x501)
    bool noTransmission = false;   // race block + 0x588 bit 3 (0x801D5DE4)
    bool settingsChosen = false;   // 0x801C90F4
    void Close();

    int subMode = career::kMachineTest400;
    int16_t counter = 16;          // view + 0x14
    int16_t carShown = -1;         // view + 0x16
    int16_t dialog = 0;            // view + 0x18: 1 TRANSMISSION
    bool car = false;              // M + 0x241
    std::array<bool, kMtRows> rowEnabled{false, true, true, true, false, true}; // 0x8005D2BC + row * 8
    std::array<std::string, kMtRows> rowStrings; // the table's strings (row 1 patched by the setup)
    MenuListWidget list;           // 0x8005D2F0 (row callback 0x80057A70 with the machine-test table)
    std::array<TextObject, kMtRows> rowText;     // W + row * 0x28
    std::array<shell::Band, kMtRows> rowBand;    // W + 0x140 + row * 0x1C
    CourseTitle course;            // W + 0x440
    ResultBar transmissionBar;     // W + 0x484
    menu::OverlayModelCamera carCamera; // W + 0x364
    int32_t frameLength = 256;     // M + 0x234

private:
    int32_t ListCallback(int command, int row, const MenuListRowDraw* d); // 0x80057A70
    const RaceMenuAssets& a_;
    mutable MenuOt* drawOt_ = nullptr;
    int action_ = 0;
};

// View 0x8005B7E8 (colour 0x4066DD, title 0x801C6FCF): the EXE keyboard at W + 0x5C8 (descriptor 0x8005B5F8, the name buffer
// 0x801D156F, 11 characters, 256 pixels). Setup: view + 0x14 = 24; after 24 updates the keyboard opens with the caret at the end
// of the name. CANCEL: sound 0, stays; OK: sound 3, 0x8005E03C(record, W + 2, name) (the caller: career::StoreMachineTestName),
// the keyboard closes, RESULTS (0x8005B80C) follows.
class MachineNewRecordView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B7E8u, kDescriptor = 0x8005B5F8u;
    explicit MachineNewRecordView(const RaceMenuAssets& a);
    void Setup(const std::string& lastName); // 0x80051F54
    int Update(const MenuListPad* pad, bool input = true) override; // 0x80051FC4 (1 = OK: the name is keyboard.name)
    void Draw(MenuOt& ot) const override;                          // 0x80052144
    std::string Title() const override;
    uint32_t Colour() const override;
    int16_t delay = 0; // view + 0x14
    NameEntry keyboard;

private:
    const RaceMenuAssets& a_;
};

// View 0x8005B80C (colour 0xF4BE73, title "RESULTS" 0x801C6EE7).
struct MachineResultsInput {
    int subMode = career::kMachineTest400;
    int16_t rank = -1;          // W + 2 (-1 "OUT OF RANKING" 0x801C7A0E)
    uint32_t time = 0;          // W + 8
    uint32_t maxSpeed = 0;      // W + 0xC
    uint32_t vsync = 0;         // the VSync counter at the setup (0x80050BC4: the car camera's pose)
};
class MachineResultsView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B80Cu;
    explicit MachineResultsView(const RaceMenuAssets& a);
    MachineResultsView(const MachineResultsView&) = delete;
    MachineResultsView& operator=(const MachineResultsView&) = delete;
    void Setup(const MachineResultsInput& in);                        // 0x80052170
    int Update(const MenuListPad* pad, bool input = true) override;   // 0x8005232C
    void Draw(MenuOt& ot) const override;                             // 0x80052638
    std::optional<PostRaceModel> Model() const override;             // car 0 in (0, 0x96, 0x160, 300) as RESULTS
    std::string Title() const override;
    uint32_t Colour() const override;
    bool SaveChosen() const { return save_; } // left with "Save" (SAVE GAME 0x8005B588), else "Next" (0x8005AE30)

    int subMode = career::kMachineTest400;
    int16_t t = 0, carShown = -1, done = 0;    // view + 0x14 / + 0x16 / + 0x18
    uint32_t time = 0, maxSpeed = 0;           // W + 8 / W + 0xC
    ResultLabel resultsLabel, recordLabel;     // W + 0x2F8 (0x8005B60C) / W + 0x38C (0x8005B6A0 with "RECORD" 0x801C700E)
    TextObject place;                          // W + 0x348 (template 0x8005B62C)
    shell::Band placeBand, recordBand;         // W + 0x370 (0x8005B648) / W + 0x3DC (0x8005B6C4)
    FadePair timeFade;                         // 0x8005B6C0
    ResultBar bar;                             // W + 0x4F8 (template 0x8005B5AC "Save Game?" Save / Next)
    menu::OverlayModelCamera carCamera;        // W + 0x1D8 (0x80050BC4, turned by 12: 0x80050CC4)
    int32_t frameLength = 256;                 // M + 0x234

private:
    const RaceMenuAssets& a_;
    bool save_ = false;
};

// View 0x8005D3B4 (colour 0xF07800, title "RECORD" 0x801C700E): the eight entries of the sub-mode's career record, sliding in
// row by row (view + 0x14 counts to 0x38) and out (from 0xC); a face button (after 0x10 updates) leaves (sound 3, update
// returns 2: the manager pops back to the menu, whose setup(1) runs).
class MachineRecordsView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005D3B4u;
    MachineRecordsView(const RaceMenuAssets& a, const career::MachineTestRecord& record, const career::MachineTestCarNames& names, int subMode);
    void Setup();                                                    // 0x8005916C
    int Update(const MenuListPad* pad, bool input = true) override;  // 0x8005918C (2 = leave)
    void Draw(MenuOt& ot) const override;                            // 0x80059240
    std::string Title() const override;
    uint32_t Colour() const override;
    int16_t t = 0, closing = 0; // view + 0x14 / + 0x16

private:
    const RaceMenuAssets& a_;
    career::MachineTestRecord record_;   // copies: what the view shows while it is up
    career::MachineTestCarNames names_;
    int subMode_;
};

// The value display of RESULTS (0x800492C4 with isNumber 1: Max Speed): 0x80068CA0 + the unit (0x801C6C87), right-aligned
// numbers (0x8006B184, digit shift -3; the two ghost copies at x -/+ k / 2 with shift -2), `centre`: x += width / 2 (0x8006B044).
void AddSpeedDisplay(const RaceMenuAssets& a, MenuOtSlot& ot, const FadePair& fade, uint32_t value, int x, int y, int mode, const TimeStyle& style,
                     bool centre);

} // namespace gt2::screens
