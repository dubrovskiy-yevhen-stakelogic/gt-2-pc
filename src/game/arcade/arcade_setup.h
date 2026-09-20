#pragma once
// The arcade menus' race build (GT2.OVL member 2 of US Arcade v1.1, SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95;
// all addresses in this file are ARCADE v1.1 addresses): the selection the menus fill (RAM 0x801C3010, 0x2D4 bytes) becomes
// the race block (0x801D52BC, 0x58C bytes), the settings block (0x801C9300, 0x40 bytes) and the six car records (0x801DE31A
// + i x 0x1C0) through 0x80010C84, which calls the event race builder 0x80010554 (instruction-identical to the GT-mode
// builder, Simulation ovl4 0x80010A30) with its opponent draw 0x80010238 (= Simulation 0x80010714), the racing-modification
// repaint 0x800104A8 (= 0x80010984) and the saturation test 0x800100EC (= 0x800105C8). docs/research/arcade_disc.md
// section 16. Verified by gt2verify row ArcadeBuild (tools/gt2verify/verify_arcade_menu.cpp) on the arcade menu dump.
//
// Also the menus' availability rules computed at menu start: the course counts (0x8001D210 / 0x8001D120 with the tier test
// 0x8002357C) and the per-car flags of class lists 0 (S) and 6 (bonus) (0x8001D418).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/career/garage.h"
#include "game/sim/car_setup.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/arcade_menu_data.h"
#include "gt2formats/course_data.h"
#include "gt2formats/gtmode_tables.h"

namespace gt2::arcade {

// Arcade v1.1 RAM.
constexpr uint32_t kMenuRegionAddress = 0x801C2EB0u; // the race context 0x80010C84 writes (0x801C2EB0..0x801C3010) + the selection
constexpr uint32_t kSelectionAddress = 0x801C3010u;  // the selection (0x2D4 bytes; ovl2 0x80011868 passes it to 0x80010C84)
constexpr size_t kSelectionSize = 0x2D4;
constexpr size_t kMenuRegionSize = (kSelectionAddress - kMenuRegionAddress) + kSelectionSize;
constexpr uint32_t kRaceBlockAddress = 0x801D52BCu;   // Simulation 0x801D585C
constexpr size_t kRaceBlockSize = 0x58C;
constexpr uint32_t kSettingsAddress = 0x801C9300u;    // Simulation 0x801C98A0
constexpr uint32_t kRecordsAddress = 0x801DE31Au;     // Simulation 0x801DE8BA
constexpr uint32_t kCareerAddress = 0x801C9340u;      // Simulation 0x801C98E0 (+0 language, +1..+8 the race option bytes)
constexpr uint32_t kVsyncCounterAddress = 0x801F0070u; // the u32 0x8007D14C(0) returns

// Field offsets of the selection (established by watching its writers in the arcade menu run, section 16.3).
struct Sel {
    static constexpr size_t kLevel = 0x00;        // s8: LEVEL SELECTION row (0x8001DE14); 0 Easy .. 2 Difficult
    static constexpr size_t kClass = 0x01;        // s8: 0 S, 1 A, 2 B, 3 C (0x8001E094; the event name table's column)
    static constexpr size_t kMode = 0x02;         // s8: game mode (0x8004F8EC[GAME SELECTION row]; 4 Road Race, 6 Rally / Time Trial)
    static constexpr size_t kLaps = 0x04;         // s16: career + 3 (arcade Race Laps option) for Single Player, + 6 for 2P (0x8001D6C8)
    static constexpr size_t kGarage = 0x06;       // s16: -1 = a car of the class lists; 0 / 1 = home / guest garage
    static constexpr size_t kGarageSlot = 0x08;   // s16
    static constexpr size_t kCarShown = 0x0C;     // u32 car id (car selection)
    static constexpr size_t kCar = 0x10;          // u32 car id
    static constexpr size_t kTyres = 0x14;        // s8: SETTINGS bar, 0 Racing = player car table 32, 1 Drift = table 33 (0x80010000)
    static constexpr size_t kColour = 0x16;       // s16: the paint index of the car's .carinfoa list (0x8001003C)
    static constexpr size_t kTransmission = 0x18; // s8: 0x8004FBD0[TRANSMISSION bar] (AT 0, MT 1) -> entry + 0x8F
    static constexpr size_t kConfigA = 0x1C, kConfigB = 0x1C0, kConfigC = 0x244; // the player's configuration after the build (0x84 each)
    static constexpr size_t kCourseName = 0xB8;   // char[]: the course's display name
    static constexpr size_t kCourseId = 0x1B8;    // u32: the course id -> race block + 0x40 (0x8005E4A0)
    static constexpr size_t kCourseRecord = 0x1BC; // s16: the course record number -> race block + 0x57E
    static constexpr size_t kGarageFlag = 0x2C8;  // u32: set by the build for a garage car (ovl3 then rebuilds the entries)
    static constexpr size_t kWord2CC = 0x2CC;     // u32: 0 from GAME SELECTION (0x8001DABC); block + 0x588 bit 0 = (value != 1)
    static constexpr size_t kWord2D0 = 0x2D0;     // u32: the course selection's view pointer (0x80022D98)
};

// The second player's half of the selection, written by the 2PLAYER BATTLE view (0x800522E0: init 0x80020700, update 0x8002229C with
// the per-player state machine 0x80020D88, draw 0x80022658) and read by 0x80010C84 in mode 0 (docs/research/arcade_disc.md section 19).
// Arrays of two, indexed by player (0 / 1).
struct Sel2P {
    static constexpr size_t kGarage = 0xA0;       // s16[2]: -1 = a car of the class lists; 0 / 1 = home / guest garage (0x80020D88 states 0 / 1)
    static constexpr size_t kGarageSlot = 0xA4;   // s16[2]: the garage car's index (state 1)
    static constexpr size_t kCar = 0xA8;          // u32[2]: car id (state 5; a garage car: its model id + 0x8C)
    static constexpr size_t kTyres = 0xB0;        // s8[2]: SETTINGS bar 0x8004FBD0[bar] (state 7): 0 table 32, 1 table 33
    static constexpr size_t kColour = 0xB2;       // s16[2]: the paint index (the car page + 0x0D)
    static constexpr size_t kTransmission = 0xB6; // s8[2]: TRANSMISSION bar 0x8004FBD0[bar] (states 3 / 6)
};

// The two garage blocks (0x4028 bytes each; empty = none): [0] home = career + 0x3C74 (RAM 0x801CCFB4), [1] guest = RAM 0x801D0FDC.
using GarageBlocks = std::array<std::span<const uint8_t>, 2>;

struct ArcadeSetupData {
    const ArcadeData& data;          // carparam/usa_arcade_data.dat (the tables *0x80092B64 points to)
    const ArcadeMenuData& menu;      // member 2's tables
    const CarInfoDirectory& cars;    // .carinfoa
    const CourseInfoTable& courses;  // .crsinfo
};

struct ArcadeRaceSetup {
    std::array<uint8_t, kRaceBlockSize> raceBlock{};
    std::array<uint8_t, 0x40> settings{};
    std::vector<sim::CarParams> records;                 // one per entry (race block + 0x5A)
    std::array<uint8_t, kMenuRegionSize> menuRegion{};   // RAM 0x801C2EB0.. after the build (the selection included)
};

// 0x80010C84(p1, p2, 0x801C3010) for the single-player Road Race (selection + 2 = 4) and Rally / Time Trial (mode 6: event
// "ATT" of table 30, one entry, 100 laps, no countdown - the event's rolling start, block + 0x588 bit 0 = not Rally, + 0x53C =
// the name of the car of the career's course record 0x8005E674) with a car of the class lists (selection + 6 < 0).
// `menuRegion` = RAM 0x801C2EB0.. before the build (the selection filled by the menus); `career` = the career block (at least
// its first 9 bytes for mode 4: +0 language, +1..+8 copied to race block + 0..+7; mode 6 also reads the course records
// + 0x218 + course x 0x24); `vsync` = the VSync counter (the draw's seed); p1 / p2 = the two numbers the menu entry draws
// before the build (0x800839F0 twice on the VSync counter; stored at 0x801C2EB0). Throws std::logic_error for inputs the original
// cannot handle (a car outside the player tables, a colour beyond its paints). Mode 0 (2 player Battle) needs the overload with both
// garage blocks.
// A garage car (selection + 6 = garage >= 0, + 8 = its index): `garageBlock` = that garage's block (0x4028 bytes, arcade_car_page.h);
// the builder draws all six entries (mode 4) / none (mode 6) and marks the selection's garage flag (+ 0x2C8); the car's entry
// is made at the race load by RebuildGarageEntry (ovl3).
ArcadeRaceSetup BuildArcadeRace(const ArcadeSetupData& d, std::span<const uint8_t> menuRegion, std::span<const uint8_t> career, uint32_t p1, uint32_t p2,
                                uint32_t vsync, std::span<const uint8_t> garageBlock = {});
// The same with both garage blocks: modes 4 / 6 use the selected garage's (selection + 6), mode 0 (2 player Battle, 0x80010D1C..) each
// player's (Sel2P::kGarage). Mode 0: event "A2P" (0x800267B4) with the settings block of "ATT" (0x800267B0), race block + 0..+7 =
// career + 1..+8 (+ 5 the 2P laps option career + 6, + 6 Handicap Start career + 7, + 7 Slow Car Boost career + 8), + 8 = 2, + 0x0A = 0,
// + 0x0D = 1 (countdown), + 0x0F = selection + 4 (the 2P laps copied by ARCADE MODE), two entries of 0x80010A34 (entry i on grid slot
// i, kind 3 / 4 = pad slots 2 / 3) with records 0 / 1; a garage car's entry is left empty (ovl3 makes it, RebuildGarageEntries). The
// selection's three configuration copies take player 1's; the race context names both players' cars (a garage car by its model).
ArcadeRaceSetup BuildArcadeRace(const ArcadeSetupData& d, std::span<const uint8_t> menuRegion, std::span<const uint8_t> career, uint32_t p1, uint32_t p2,
                                uint32_t vsync, const GarageBlocks& garages);

// GT2.OVL member 3 (the race launcher, SHA-1 d31f01582a5c37f0a661fa378dfb62526d11ea23; ARCADE addresses of that member)
// 0x80012C00 for a selection with the garage flag: the GT-mode car tables are loaded (0x80076C84 -> *0x80092B64) and 0x8001290C
// rebuilds the garage car's entry (modes 4 and 6; mode 0: RebuildGarageEntries below):
//   0x80011BE8(garage, index, rally): the car's tune sheet (0x80010000 / 0x80010024 / 0x80011858 = Simulation ovl4 0x800173E8's
//     0x80015404 / 0x80015428 / 0x80016C5C); Rally (selection + 0x2CC != 0): tyres front / rear to stage 7 (0x8005E9D0 =
//     Simulation 0x8005EAC0), else fitted dirt tyres (stage 7) back to stage 0; then 0x80011B0C = 0x80016F10: the sheet back into
//     the GARAGE CAR (its configuration and figures - the career's garage changes, the title's Save keeps it);
//   0x800127AC(block, entry 0, grid (mode 6: 0 with the slot cleared; mode 4: -1 = the drawn entry's slot kept), kind 3,
//     transmission (selection + 0x18), car, garage, index): slot + 0 model id, + 4 paint, + 8 the car's configuration with
//     +0x7A |= 0x40, + 0x90 the model's name, block + 0x582 / + 0x584 = garage / index, record 0 = 0x800770BC with the GT tables.
// `gt` = the GT-mode data of the disc (tables of carparam/usa_gtmode_data.dat, .carinfoa, usa_gtmode_race.dat).
void RebuildGarageEntry(ArcadeRaceSetup& setup, std::span<uint8_t> garageBlock, const career::CareerData& gt);
// 0x8001290C for every mode: modes 4 / 6 as RebuildGarageEntry on the selected garage; mode 0 for each player i with a garage car
// (Sel2P::kGarage + 2i >= 0; the same garage may serve both): 0x80011BE8(garage, index, rally) and 0x800127AC(block, entry i, grid i, kind
// i + 3, transmission Sel2P + i, car, -1, -1) - entry i cleared, record i, block + 0x582 / + 0x584 = -1.
void RebuildGarageEntries(ArcadeRaceSetup& setup, std::array<std::span<uint8_t>, 2> garages, const career::CareerData& gt);

// The menu entry's two numbers (ovl2 0x80011868: seed = VSync counter, 0x800839F0 twice).
std::array<uint32_t, 2> MenuEntryNumbers(uint32_t vsync);

// ---------------------------------------------------------------- availability at menu start (0x8001D210, 0x8001D418)

// 0x8002357C(tier): tier < 0, or every one of the ten 0xA4-byte records of the tier (career + 0x1418 + tier x 0x668, byte +1)
// is set. `career` = the career block (at least 0x1418 + 6 x 0x668 bytes).
bool TierOpen(std::span<const uint8_t> career, int32_t tier);
// 0x8001D120(list, reverse): the rows of a course list that are open (tier), for a reverse list only those whose course record
// flag (career + 0xB8 + record, bit 2) is set; returns the per-row flags (the image's +0x1C bytes the original rewrites).
std::vector<uint8_t> CourseAvailability(const std::vector<ArcadeCourse>& list, bool reverse, std::span<const uint8_t> career);
// 0x8001D418: the per-car flags of class list 0 (S) and 6 (bonus); `classS` = the Class-S row of CLASS SELECTION (0x800F365C).
struct ClassUnlocks {
    std::vector<uint8_t> classS, bonus;
    bool classSOpen = false;
};
ClassUnlocks ComputeClassUnlocks(const ArcadeMenuData& menu, std::span<const uint8_t> career);

} // namespace gt2::arcade
