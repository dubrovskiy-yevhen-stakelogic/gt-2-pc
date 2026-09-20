#pragma once
// The race overlay's post-race views of game mode 6 (the Arcade disc's Time Trial / Rally) as GPU primitives in draw order,
// with their per-field state machines (the views of race_result_screens.h's framework):
//   wait        (view 0x8005B09C: init 0x8004A754, update 0x8004A7B4)  20 fields, then ENTER YOUR NAME after a new course
//               record (0x801D5DE9 == 1), else SESSION RESULTS; a later visit (the race-run flag 0x8005B088 clear: after a
//               replay) goes to the menu at once. Both start CD-DA track 8 (0x800481C8 -> EXE 0x80080F24(8, 1)).
//   ENTER YOUR NAME (view 0x8005B0E4: 0x8004A920 / 0x8004A990 / 0x8004AAD8) the EXE keyboard (race_record_screens.h
//               NameEntry, descriptor 0x8005AF68) on the career's name buffer + 0x7C8F; OK: the course record + 0x18 = the
//               name, + 0x14 = car 0's id (race block + 0x5C), race block + 0x53C = the slot's car name (+ 0xEC).
//   SESSION RESULTS (view 0x8005B108: 0x8004AE08 / 0x8004B19C / 0x8004B530) the session's laps with their sector times
//               (the best sector of the session and the best lap in the highlight colour), the session record (the best
//               entry of the result record 0x801D5F58) and the course record (0x8005E764) with its car and name.
//   TIME TRIAL  (view 0x8005B12C: 0x8004C034 / 0x8004C1EC / 0x8004C5C8) the menu: Replay / Try Again / Settings ... /
//               Records ... / Save Ghost ... / Load Ghost ... / Save Replay ... / Ghost Options ... / Exit (rows 0x8005B01C,
//               12 bytes {colour, label, s8 enabled, u8 action}), the course picture (a 188 x 200 sprite of page 0x37,
//               gt2formats/course_map.h), car 0 turning (0x80048754), the ghost option list (0x8005AFB0: the career's
//               + 0xB5, labels 0x8005B08C).
//   leave       (view 0x8005B0C0: 0x8004A8B8 / 0x8004A8E8) 21 fields, stops the CD music; the manager returns 4 and the
//               arcade loop reads the chosen action (M + 0x7C: 0 Replay, 1 Try Again, 2 Exit).
// The view manager 0x800474F4: update 1 = the view pushed by 0x800483A4 enters (init 0, forward transition M + 0x212 = 0),
// 2 = back to the view below (0x800483D8, init 1, transition 1), 3 / 4 = leave. Transitions (0x800477C4): forward = the new
// view from (0, c * 200 / 16), the old one to (0, (c - 16) * 5); back = the new view from (0, -c * 200 / 16), the old one
// to (0, (16 - c) * 5); header alphas 128 - c * 8 / c * 8.
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); the US Arcade v1.1 build
// has the same code 0xE0 lower (0x8004A638 ..; data 0x90 lower: views 0x8005B00C ..), reached through the Simulation layout of
// gt2game's arcade_post_race.h. Evidence: Ghidra pseudo-C and objdump of the race overlay (work/re/mode6_post/decomp), gt2run
// sessions of the original (work/re/mode6_post/s1..s5: the views' order, calls and writes) and gt2play --prims captures
// (work/play/mode6/cap/c1_*, c2_*).
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/sim/race_shell.h"
#include "gt2view/race_record_screens.h"
#include "gt2view/race_result_screens.h"

namespace gt2::screens {

// What the mode 6 views read of the race and the career.
struct SessionInput {
    sim::PlayerResults results{};     // 0x801D5E88: the session's laps (lap number + 2, count + 4, best lap number + 6, laps + 8,
                                      // the best entry + 0xD0 = the session record)
    uint8_t newRecord = 0;            // 0x801D5DE9: the session set a new course record
    sim::LapEntry courseRecord{};     // 0x8005E764: the career's course record (time -1 = none) ...
    std::string recordName;           // ... + 0x18 (the name entered)
    std::string recordCar;            // race block + 0x53C (the record car's name)
    std::string slotCar;              // race block + 0xEC (the name of car 0's slot: the record car after a new record)
    std::string course;               // 0x801D587C (race block + 0x20)
    uint8_t ghostOption = 1;          // career + 0xB5
    std::string enteredName;          // the career's name buffer + 0x7C8F
    bool savedBest = false;           // 0x8002F4B1: the race end handed a new reference to the next race ("Save Ghost ..." row)
    bool ghostLoaded = false;         // 0x801D55AA: a ghost was loaded from a card (0x80050304)
    bool garageCar = false;           // 0x801D5DDE / 0x801D5DE0: a garage car ("Settings ..." row)
    bool replayAvailable = true;      // 0x801C90B4 == 0 ("Replay" / "Save Replay ..." rows)
};

// 0x8005B09C / 0x8005B0C0: the views without drawing.
class SessionWaitView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B09Cu;
    SessionWaitView(const RaceMenuAssets& a, bool raceRun, bool newRecord) : a_(a), raceRun_(raceRun), newRecord_(newRecord) {}
    int Update(const MenuListPad* pad, bool input = true) override; // 0x8004A7B4
    void Draw(MenuOt&) const override {}
    std::string Title() const override { return ""; }
    uint32_t Colour() const override { return 0; }
    enum Next { kNone, kEnterName, kResults, kMenu };
    Next next = kNone;
    int16_t counter = 20;
    std::vector<int> cdTracks; // 0x800481C8 requests of the last update (CD-DA track 8)
private:
    [[maybe_unused]] const RaceMenuAssets& a_;
    bool raceRun_, newRecord_;
};

class SessionLeaveView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B0C0u;
    int Update(const MenuListPad* pad, bool input = true) override; // 0x8004A8E8: returns 4 after 21 updates
    void Draw(MenuOt&) const override {}
    std::string Title() const override { return ""; }
    uint32_t Colour() const override { return 0; }
    int16_t counter = 20; // 0x8004A8B8 (+ 0x800481E8: the CD music stops)
};

// ENTER YOUR NAME (0x8005B0E4).
class SessionNameView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B0E4u, kDescriptor = 0x8005AF68u;
    SessionNameView(const RaceMenuAssets& a, const std::string& name);  // 0x8004A920
    int Update(const MenuListPad* pad, bool input = true) override;     // 0x8004A990: 1 = OK (keyboard.name is the name)
    void Draw(MenuOt& ot) const override { keyboard.Draw(ot[0]); }      // 0x8004AAD8
    std::string Title() const override;
    uint32_t Colour() const override;
    int16_t delay = 24;
    NameEntry keyboard;
private:
    [[maybe_unused]] const RaceMenuAssets& a_;
};

// One row of times (0x800495A8 init / 0x800495E4 draw, 0x20 bytes): +0 s16 lap label (> 0: drawn with "%d"), +4 the lap entry,
// +8 six colours (the label, sectors 1..4, the total).
struct TimeRow {
    int16_t lap = -1;
    sim::LapEntry entry{};
    std::array<uint32_t, 6> colours{};
};
// 0x800495E4(row, ot, ctx, x, y, alpha): the label right-aligned at x, then the four sector times (0x8005DD94) and the total
// right-aligned every 60 pixels, colours from 0x8005AB58 towards the row's by alpha / 128 (small font 0x801C9120, mode 1).
void AddTimeRow(const RaceMenuAssets& a, MenuOtSlot& ot, const TimeRow& row, int x, int y, int alpha);

// SESSION RESULTS (0x8005B108).
class SessionResultsView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B108u;
    SessionResultsView(const RaceMenuAssets& a, const SessionInput& in, bool fromName);   // 0x8004AE08 (*W = fromName)
    SessionResultsView(const SessionResultsView&) = delete;
    SessionResultsView& operator=(const SessionResultsView&) = delete;
    int Update(const MenuListPad* pad, bool input = true) override; // 0x8004B19C: 1 = on to the menu, 2 = back to it
    void Draw(MenuOt& ot) const override;                           // 0x8004B530
    std::string Title() const override;
    uint32_t Colour() const override;

    int16_t counter = 24;             // V+14
    bool fromName = true;             // *W: entered from the wait / name views (back is refused)
    CourseTitle course;               // W+4
    std::vector<TimeRow> laps;        // W+0x13C + i * 0x20 (W+0x138 count)
    TimeRow sessionRow, recordRow;    // W+0x27C / W+0x308
    shell::Band sessionBand, recordBand; // W+0x29C / W+0x328 (0x8005AECC)
    ResultLabel sessionLabel, recordLabel, carLabel, nameLabel; // W+0x2B8 / 0x344 / 0x398 / 0x3E8
    int16_t labelWidth = 0;           // W+0x394
    struct Rule { uint32_t c0 = 0, c1 = 0; int16_t x = 0, y = 0, w = 0, h = 0, steps = 1, anim = -1; } rule; // W+0x438 (0x8005AEB8; 0x8006BCB8 / 0x8006BD08)
    MenuListWidget list;              // 0x8005AE84
    SessionInput in;
private:
    int32_t ListCallback(int command, int row, const MenuListRowDraw* d);
    [[maybe_unused]] const RaceMenuAssets& a_;
    mutable MenuOt* drawOt_ = nullptr;
};

// TIME TRIAL (0x8005B12C).
class TimeTrialMenuView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B12Cu;
    enum Action : int8_t { kReplay = 0, kTryAgain = 1, kExit = 2, kGhostOptions = -7, kSaveGhost = -6, kSettings = -5, kRecords = -4, kLoadGhost = -3,
                           kSaveReplay = -2 };
    TimeTrialMenuView(const RaceMenuAssets& a, const SessionInput& in);
    TimeTrialMenuView(const TimeTrialMenuView&) = delete;
    TimeTrialMenuView& operator=(const TimeTrialMenuView&) = delete;
    void Setup(bool back);                                          // 0x8004C034 (arg 1 = re-entered from a view it pushed)
    int Update(const MenuListPad* pad, bool input = true) override; // 0x8004C1EC: 1 = a row that leaves (Chosen())
    void Draw(MenuOt& ot) const override;                           // 0x8004C5C8
    std::optional<PostRaceModel> Model() const override;            // car 0 in (0x8A, 0xF0, 200, 200) while V+16 >= 0
    std::string Title() const override;
    uint32_t Colour() const override;
    int8_t Chosen() const { return chosen_; } // the row's action byte (0xF9.. as int8_t) once Update returned 1
    uint8_t ghostOption = 1;          // career + 0xB5 (0x801C9995), written by the ghost option list
    // Rows the game cannot run yet, drawn disabled after the setup's own rules (bit = row): the replay of a mode 6 race and the
    // card managers of ghosts / replays are not ported (see docs/research/arcade_disc.md 17.10).
    uint16_t unsupported = 0;

    struct Row {
        std::string text;
        uint32_t colour = 0;
        bool enabled = true;          // row + 8 (patched by the setup)
        int8_t action = 0;            // row + 9
    };
    std::vector<Row> rows;            // 0x8005B01C
    int16_t counter = 16, car = -1, ghostOpen = 0; // V+14 / V+16 / V+18
    int16_t blink = 0;                // 0x801C90BC
    std::vector<TextObject> rowText;  // W + 0x728 + r * 0x28
    std::vector<shell::Band> rowBand; // W + 0x9A8 + r * 0x1C
    CourseTitle course;               // W+0x6E4
    MenuListWidget list, ghostList;   // 0x8005AF7C / 0x8005AFB0
    menu::OverlayModelCamera carCamera; // W+0x608 (0x80049780(200, 200), turned by 0x80049874)
    int32_t frameLength = 256;        // M+0x234
    SessionInput in;
private:
    int32_t ListCallback(int command, int row, const MenuListRowDraw* d);
    int32_t GhostCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* d);
    [[maybe_unused]] const RaceMenuAssets& a_;
    mutable MenuOt* drawOt_ = nullptr;
    int8_t chosen_ = 0;
};

// The view manager with its stack (0x800474F4 / 0x800479AC / 0x800483A4 / 0x800483D8) for these views.
class SessionViewStack {
public:
    // The first view: entering (transition 16, forward), or settled (`transition` false: a new run of the manager's task,
    // 0x800472E0 sets M+0x211 = 0 and calls the view's init).
    void Start(std::unique_ptr<PostRaceView> first, bool transition = true);
    void Push(std::unique_ptr<PostRaceView> next);                  // update returned 1: `next` enters (forward)
    bool Pop();                                                     // update returned 2: the view below enters (back); false = none
    // The view replaced the manager's top (0x80048374) and returned 5 / 6: `next` enters sliding sideways (M+0x212 = 3 / 2;
    // 0x800477C4: 2 = the new view from the right (x 200 c / 16, the leaving one 100 (c - 16) / 16), 3 = from the left
    // (x -200 c / 16, the leaving one 100 (16 - c) / 16); the drawing area starts at the view's x offset).
    enum Slide : int { kFromRight = 2, kFromLeft = 3 };
    void Replace(std::unique_ptr<PostRaceView> next, Slide slide);
    int Update(const MenuListPad* pad);                             // the transition, the leaving view's update(0), the top's update(1)
    std::vector<MenuPrim> Frame(const RaceMenuAssets& a, size_t& modelAt, std::optional<PostRaceModel>& model) const;
    PostRaceView* Top() const { return stack_.empty() ? nullptr : stack_.back().get(); }
    PostRaceView* Previous() const { return transition_ > 0 ? previous_ : nullptr; } // the view leaving during a transition
    int Transition() const { return transition_; }
private:
    std::vector<std::unique_ptr<PostRaceView>> stack_;
    std::unique_ptr<PostRaceView> popped_; // the view leaving by a back transition or a replacement
    PostRaceView* previous_ = nullptr;     // the view leaving (drawn / updated with arg 0 during the transition)
    int transition_ = 0;
    bool back_ = false;
    int slide_ = 0;                        // M+0x212 = 2 / 3 (Replace), else 0
};

} // namespace gt2::screens
