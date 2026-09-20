#pragma once
// The GT-mode MACHINE TEST (the map's "Machine Test" page: 0-400 m, 0-1000 m, Max Speed; race sub-modes 7 / 8 / 9 of the
// race block + 0x0A): the menus' race setup and the career records the race overlay keeps of them.
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); code addresses in GT2.OVL
// member 4 (ovl4, the GT-mode menus) / member 0 (ovl0, the race overlay) as marked, EXE = the resident executable. Evidence:
// our disassembly (work/ovl/sim_us12/*.asm), Ghidra pseudo-C of the race overlay (work/re/mtest/ghidra) and gt2run sessions of a
// 0-400 m run from the menus (work/re/mtest/s7: the 0-400 m test of work/memcards/gt2_save_1car.mcd, docs/formats/race_screens.md
// section 5.7).
//
// Career records (career + 0x3A88 / + 0x3B2C / + 0x3BD0, one 0xA4-byte record per test, sub-mode 7 / 8 / 9):
//   +0 u8 count (0..8; > 8 is treated as 0 by the insert), +4 eight entries of 0x14 bytes {u32 car id (race slot 0 + 0 = the
//   garage car's model id), u32 value (0-400 m / 0-1000 m: the time in ms, smaller is better; Max Speed: the speed readout,
//   larger is better; -1 = none on a new game, EXE 0x8005E07C), char name[12] (the name entered in NEW RECORD)}.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "gt2formats/course_data.h"

namespace gt2::career {

struct CareerData;
struct EventMenuData;
struct EventInfoTable;
struct TuneSheet;

constexpr int kMachineTest400 = 7, kMachineTest1000 = 8, kMachineTestMaxSpeed = 9; // race block + 0x0A (0x801D5866)
constexpr int kMachineTestEntries = 8;
constexpr size_t kMachineTestEntrySize = 0x14;

// 7 / 8 / 9 -> 0 / 1 / 2, anything else -> -1.
int MachineTestIndex(int subMode);
// The career's record of the test (career + 0x3A88 + index * 0xA4), null for another sub-mode.
MachineTestRecord* MachineTestRecordOf(CareerState& s, int subMode);
const MachineTestRecord* MachineTestRecordOf(const CareerState& s, int subMode);
// Entry k of a record (+4 + k * 0x14).
inline uint8_t* MachineTestEntry(MachineTestRecord& r, int k) { return r.bytes + 4 + k * int(kMachineTestEntrySize); }
inline const uint8_t* MachineTestEntry(const MachineTestRecord& r, int k) { return r.bytes + 4 + k * int(kMachineTestEntrySize); }
uint32_t MachineTestEntryCar(const MachineTestRecord& r, int k);
uint32_t MachineTestEntryValue(const MachineTestRecord& r, int k);
std::string MachineTestEntryName(const MachineTestRecord& r, int k);

// EXE 0x8005E0D0(record, entry, higher): the entry ranked into the record (sorted, at most 8 entries; an entry of the same
// car is replaced only by a better value, else -1); returns its index or -1. (The title's Data Transfer uses the same routine:
// game/shell/title_transfer.h InsertMachineTestEntry.)
int32_t InsertMachineTestRecord(MachineTestRecord& record, const uint8_t* entry, bool higher);

// What the race overlay's machine-test views read of the result (work block W = *0x801C90A0): W + 2 the rank (s16, -1 = not
// ranked / not a machine test), W + 8 the time (u32 0x801D5F58), W + 0xC the max speed (u32 of u16 0x801D5F68).
struct MachineTestOutcome {
    int16_t rank = -1;
    uint32_t time = 0;
    uint32_t maxSpeed = 0;
};

// ovl0 0x80050D78 (run by the wait view 0x8005B7A0 after the race of a machine test, 0x80050EE4): the entry {race slot 0's car
// id (0x801D58B8), value, name ""} with value = the time of the player's results record + 0xD0 (u32 0x801D5F58; sub-modes 7 / 8)
// or its + 0xE0 (u16 0x801D5F68: the max speed; sub-mode 9, larger is better) ranked into the career's record of the sub-mode.
MachineTestOutcome WriteMachineTestRecord(CareerState& s, int subMode, uint32_t carId, uint32_t time, uint16_t maxSpeed);

// EXE 0x8005E03C(record, index, name): the name into entry `index` (+0x0C + index * 0x14, strcpy) when index < 8 (unsigned).
// ovl0 0x80051FC4 (the machine test's NEW RECORD, view 0x8005B7E8) stores the entered name (0x801D156F) with the rank W + 2.
void StoreMachineTestName(MachineTestRecord& record, int32_t index, const std::string& name);

// ---------------------------------------------------------------- the menus' race setup

// The names of the three tests in the GT-mode overlay (ovl4 0x80022F20 "G400" / 0x80022F28 "G1000" / 0x80022F30 "GMAX"; the
// menu page's items carry them) are EventMenuData::machineTests (events.h MachineTestMode 0x8001861C).

// ovl4 0x8001F124(table, record, model id): the car names the race overlay's RECORD view shows (0x801D5FA0 = career + 0xC6C0,
// copied to 0x80169894 by the race overlay's init 0x80017500 for sub-modes 7..9 and read through EXE 0x80074AE4): s16 count =
// entries + 1, then per entry of the record and last for `modelId` 0x48 bytes {u32 car id, the car's name (0x80060AE8: the
// .carinfoa name with its 0x7F padding, strcpy)}. The original writes only these bytes.
struct MachineTestCarNames {
    std::vector<uint8_t> bytes; // the table as RAM holds it from 0x801D5FA0 (only `written` bytes are defined)
    std::vector<uint8_t> written;
    // EXE 0x80074AE4(table, car id): the name of the first entry with the id; "" (0x8008FB3C) when none.
    std::string NameOf(uint32_t carId) const;
};

// Everything ovl4 0x80012C6C(name, sub-mode) prepares before the race overlay runs a machine test (0x80013628: result 2 with a
// machine-test name, 0x8001861C; the day counter does NOT advance):
//  - race block 0x801D585C (0x58C bytes, cleared): + 0 .. + 7 = career + 1 .. + 8, + 8 = 2, + 9 = 1, + 0xA = sub-mode, + 0xB = 5,
//    + 0xC = 0, + 0xD = 1 (countdown), + 0xE = 0, + 0xF = 1 lap, + 0x10 the event name (0x8005E548 of row + 0), + 0x20 / + 0x40 the
//    course of row + 2 (0x80083004 -> 0x8005E590), + 0x44 the tag of row + 0x94, + 0x5A = 1 car, slot 0 = the current garage car
//    (0x80011000(block, 0, -1, 3, 0, car, 0, index): + 0 model id, + 4 paint, + 8 configuration (flags | 0x40), + 0x8E = 3, + 0x8F
//    = 0, + 0x90 the car's name; + 0x8C / + 0x8D stay 0), + 0x57C = -1, + 0x580 = 0, + 0x582 = 0 (player), + 0x584 = the garage
//    index, + 0x586 = 0, + 0x588 = 1;
//  - the settings block 0x801C98A0 = event row + 0x44 (0x40 bytes);
//  - the garage car's race tyres (0x80018004(index, 0, 0x80019538(name))) and its parameter record (0x800771AC into
//    0x801C98E0 + 0x14FDA);
//  - the RECORD view's car names (0x8001F124, MachineTestCarNames).
//  - the tune sheet of the race overlay's Settings page (0x801DA4B8: 0x80011160 / 0x80011184 / 0x800129B8 = the same code as
//    0x80015404 / 0x80015428 / 0x80016C5C: cleared, the garage car's rows (its + 0 car id), its configuration).
struct MachineTestRace {
    std::string name;                        // "G400" / "G1000" / "GMAX"
    int subMode = kMachineTest400;
    std::array<uint8_t, 0x58C> raceBlock{};  // 0x801D585C
    std::array<uint8_t, 0x40> settings{};    // 0x801C98A0
    sim::CarParams params{};                 // 0x801C98E0 + 0x14FDA: slot 0's record
    MachineTestCarNames carNames;            // 0x801D5FA0
    std::string course;                      // the course file of row + 2 ("TC_lisence", "maxspeed")
    std::string tag;                         // row + 0x94
    bool dirt = false;                       // 0x80019538(name)
};
// Throws when `name` is not a machine test, the career has no current car or the event row is missing. Changes the career
// like the original: the garage car's race tyres (0x80018004).
// `sheet` = the menus' sheet 0x800B4490 that 0x80018004 loads, `raceSheet` (optional) = the Settings page's sheet 0x801DA4B8,
// `scratch` = the record build's work area (garage.h BuildScratch, >= 0x230 bytes).
MachineTestRace PrepareMachineTest(CareerState& s, const CareerData& d, const EventMenuData& menu, const EventInfoTable& infos,
                                   const CourseInfoTable& courseInfo, const std::string& name, TuneSheet& sheet, uint8_t* scratch,
                                   TuneSheet* raceSheet = nullptr);

// The course title of the race overlay's machine-test menu (ovl0 0x800587BC): data-global 0x801EFE0E "0-400m" (7), 0x801EFE18
// "0-1000m" (8), else 0x801EFE23 "Max Speed".
uint32_t MachineTestCourseTitle(int subMode);

} // namespace gt2::career
