#pragma once
#include <array>
#include <cstdint>
#include <span>

#include "game/sim/race_sim.h"
#include "gt2formats/course_data.h"
#include "gt2formats/overlay_data.h"

// The simulation's game data built from the disc: the tuning constants of the race overlay / executable
// (SimConstants) and the course's race data (RaceCourseData). Both are what the original derives at race load;
// the derivations are ports of the routines named below (US Simulation v1.2, SCUS_944.88 SHA-1 3030aa27...).
//
// SimConstants: the race overlay (GT2.OVL member 0 at 0x80010000) holds raw byte tables at 0x80046E20..0x80046EA4
// and 0x80046ED4..0x80046ED9; at race load 0x8003C12C calls 0x8003BA64 -> 0x8003B7B8, which expand them into the
// working constants at 0x80046DB0.., 0x80046E00.., 0x80046EF0.., 0x80046F3C.., 0x80046F88.. and into the BSS
// objects 0x801C8730 (rolling-resistance curve) and 0x801C8690 (drive-class tuning), mixing in the shell's race
// settings block 0x801C98A0.. and the course's dirt flag. Everything else the simulation reads is a plain table
// of the overlay (0x80046C94.., 0x80046D7C.., 0x80046DC8.., 0x80046DD4, 0x80046EAC..) or of the executable
// (0x800923E2). The frame constants 0x801C856C / 0x801C8570 are set by 0x8001523C from the shell's frame-rate mode.
//
// RaceCourseData: the course's .tro carries the start lines and the race lists (gt2formats/course_data.h);
// 0x80038DA0 fills the loader-computed fields of every record from the course geometry.
namespace gt2::sim {

// The shell's race settings block (0x801C98A0.., 0x40 bytes) as far as the simulation reads it. The menu writes
// it before the race overlay loads (the title overlay's attract race: a 16-byte-stride copy at 0x80010A70 of
// GT2.OVL member 1); it is runtime state, so it is an input here. Defaults = the attract race's values:
// every AI grip and scale at 100 %, tyre wear off.
struct RaceSettings {
    uint32_t controlWord = 0x05000200;                  // 0x801C98A0  byte 2 == 1 forces control class 1 for car 0 (0x80033384)
    std::array<uint8_t, 8> aiGripPercent{100, 100, 100, 100, 100, 100, 100, 100}; // 0x801C98A4  [class] front, [class + 4] rear
    std::array<uint8_t, 4> cornerGripPercent{100, 100, 100, 100}; // 0x801C98AC  -> class tuning + 0x26 (0 = 100, capped at 100)
    std::array<uint8_t, 4> speedScalePercent{100, 100, 100, 100}; // 0x801C98B0  divides class tuning + 0 (0 = 100, capped at 100)
    // Tyre wear (0x8003BA64): enabled only when wearLimit != 0, the course is not dirt, wearKnee < wearLimit,
    // kneeGripLossPercent < wornGripLossPercent and pitGripPercent < wornGripLossPercent.
    uint8_t wearLimit = 0;              // 0x801C98BD  x 10000
    uint8_t wornGripLossPercent = 0;    // 0x801C98BE
    uint8_t pitGripPercent = 100;       // 0x801C98BF
    uint8_t coldLimit = 0;              // 0x801C98C0  x -10000
    uint8_t coldGripLossPercent = 0;    // 0x801C98C1
    uint8_t wearKnee = 0;               // 0x801C98C2  x 10000
    uint8_t kneeGripLossPercent = 0;    // 0x801C98C3
};

// Shell globals the simulation reads that are not data files: set by the menus / the race shell at run time.
// Defaults = a single-player GT-mode race; the RAM dump of the attract race (work/re/race_load) has gameMode 2,
// flag800A951C 1 and control class 0 (see dev_dump_constants.cpp).
struct ShellState {
    // 0x801D5866, the race mode of the shell's 16-byte mode block 0x801D585C.. (copied there by the menu through
    // 0x800698D0). What the race overlay makes of the values: 0 = the GT-mode race (per-event tuning from
    // 0x801D585F / 0x801D5863 in 0x8003C12C, no AI catch-up, extra HUD in 0x8001584C); 2 / 4 / 0xC = the arcade
    // races (fixed tuning 0x80041E4C(1), AI catch-up 0x80042230 in 0x8003EBF0; 2 = the attract race of the dump);
    // 3 = licence test (0x8003D1E4, hold 120 fields); 6 = two-player battle (0x80012378); 7 / 8 / 9 = time-attack
    // style modes of 0x8003C70C. The physics port only distinguishes 3 and 6 (stuck-car reset, off-road flag).
    uint8_t gameMode = 0;
    // Inputs of 0x800418E8 (the shell's control class, 2 = player race car with wall damage and pit flags):
    // mode 0: class 2 when modeFlag60 == 1 and the course is not dirt; modes 2 / 4 / 0xC: class 2 when
    // modeFlag65 != 1 and modeFlag5D == 1 and not dirt; mode 3: class 1; other modes: 0. The dump's block has
    // 0x801D585D = 0, 0x801D5860 = 1, 0x801D5865 = 1.
    uint8_t modeFlag5D = 0, modeFlag60 = 1, modeFlag65 = 1;
    uint8_t frameRateMode = 2;          // 0x801D5864: 1 = 60 Hz (rate 60, frame 0x444), 0 / 2 = 30 Hz (30, 0x888) (0x8001523C)
    uint8_t viewMode = 0;               // 0x801C9990  (the race view block; 0 in the dump)
    uint8_t flag800A951C = 0;           // 0x800A951C  1 = attract / demo race (0x8001584C: the loop's byte + 0x2E8); 0 for a player
    uint8_t flag801C9995 = 1;           // 0x801C9995  (view block byte + 5, 1 in the dump)
    uint8_t flag800AF232 = 1;           // 0x800AF232  set to 1 by the race start 0x8001584C
    uint8_t byte801D5869 = 1;           // 0x801D5869  0 disables the start countdown (hold counter 0x800A951E)
    // 0x80046F64: the shell's race clock, zeroed by the race-state init 0x8003C3F4 and advanced by 0x8003D168 every
    // frame (+100 at 30 Hz, +50 at 60 Hz). The AI holds its steering on straights while it is below 9000
    // (ai_driver.h). RaceSim does not advance it (the race clock is shell logic), so the default is a value of a
    // running race - the dump's, 46900 - rather than the start value 0.
    uint32_t raceClock = 46900;
};

// 0x800418E8 for the shell state and the course.
int32_t ShellControlClass(const ShellState& shell, bool dirtCourse);

// Ports of 0x8003B7B8 + 0x8003BA64 (+ 0x8001523C for the frame constants) on the pristine overlay and
// executable images. Throws std::runtime_error / std::out_of_range on an image that does not hold the tables.
SimConstants LoadSimConstants(const GuestImage& raceOverlay, const GuestImage& exe, const RaceSettings& settings, const ShellState& shell,
                              bool dirtCourse);

// The original's arc-tangent table (0x800A4AC8, 4097 s16) from the executable image, for trig.h's
// AtanTableOverride (the generated table differs from it by one unit in a few entries).
std::span<const int16_t> AtanTableOf(const GuestImage& exe);

// Port of 0x80038DA0 (with 0x800386F4, 0x800389E0, 0x80038C88, 0x800358E0, 0x800287DC) on the parsed course:
// fills the loader-computed fields of the race lists and assembles the RaceCourseData. `raceOverlay` supplies
// the list order table 0x80046DF8 and the race-state table 0x80046DD4.
RaceCourseData BuildRaceCourseData(const Track& track, const CourseExtras& extras, const TrackRaceData& file, const GuestImage& raceOverlay,
                                   bool dirtCourse);

} // namespace gt2::sim
