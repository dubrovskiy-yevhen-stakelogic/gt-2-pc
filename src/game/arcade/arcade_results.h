#pragma once
// What an arcade race leaves in the career (US Arcade v1.1, EXE SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95; all
// addresses ARCADE v1.1). Evidence: gt2run sessions of the original with the original's AI driving the player's car (the dev
// capture aid tools/gt2run/ai_player_arcade.h; work/re/arcade_results/r1..r5: event A0A, Tahiti Road, the player 1st) with a
// write watch over the whole career block 0x801C9340..+0x7C9C from the race load to the return to the menus, and our
// disassembly of the race overlay (GT2.OVL member 0) in the RAM snapshots of those runs. docs/research/arcade_disc.md section 17.
//
// The only career write of a Single Player race is made by the RESULTS view's setup (member 0 0x80050EF0, the view
// 0x8005B710 the arcade race loop 0x80016CC0 starts after the replay): with race block + 0x09 == 0 and game mode + 0x0A == 4
// (Road Race), the player 1st (results record 0x801D58E8 s16 +0 == 1) and a course record number block + 0x57E >= 0, the
// EXE's 0x8005DC64(career + 0xB8, block + 0x57E, block + 0x57F) sets the level's bit in the course's flag byte (+0x57F =
// 0x800272DC[level]: Easy 1, Normal 2, Difficult 4). Those bytes open class S cars (0x8001D418: bit 1 or 2 of the car's
// course) and the reverse courses (0x8001D120: bit 2) at the next menu start (arcade_setup.h). Nothing else of the career
// changes (no best times, no autosave); the title's Save writes the block to the card. In 2 player Battle (+0x0A == 0) the
// same setup counts the winner instead: career + 0xB8 + 0x44 (player 1) or + 0x46 (player 2) += 1 (ApplyBattleResult).
#include <cstdint>
#include <span>
#include <string>

namespace gt2::arcade {

// EXE 0x8005DC64(flags, record, bits): record in 0..31: flags[record] |= bits; then, while bit 2 is still clear and bits == 2
// (a Normal win), the Normal-win counter in bits 3..5 counts up; the fifth sets bits 1 and 2 as well (the Difficult bit: the
// reverse course opens after five Normal wins). Out-of-range records change nothing.
void SetCourseWinFlags(std::span<uint8_t> flags, int32_t record, int32_t bits);

// The RESULTS setup's career part for one player (member 0 0x80050EF0 .. 0x800510C4): `career` = the career block
// (0x7C9C bytes), `raceBlock` = the race block the race ran with (0x58C bytes), `place` = the player's finishing place
// (results record +0; <= 0 = not finished). Returns true when a flag byte changed.
bool ApplyArcadeRaceResult(std::span<uint8_t> career, std::span<const uint8_t> raceBlock, int32_t place);

// The 2 player Battle (race block + 0x0A == 0) part of the RESULTS setup (0x80050F78 .. 0x80050FF4, docs/research/arcade_disc.md
// section 19): the winner by the two results records' places (0x801D58E8 / 0x801D9E00, s16 + 0; <= 0 = not finished): both
// finished - player 1 when his place is lower; else player 1 when he finished, player 2 otherwise (also when neither did). The
// winner's u16 counter career + 0xFC (player 1) / + 0xFE (player 2) += 1 (the RESULTS view's title is the winner's string,
// 0x801C74F4 + 24 * winner). Returns the winner (0 / 1).
constexpr size_t kBattleWins = 0xFC; // career + 0xB8 + 0x44: u16 player 1's wins, + 2 player 2's
int ApplyBattleResult(std::span<uint8_t> career, int32_t place1, int32_t place2);

// ---- the course records of Time Trial / Rally (game mode 6; Simulation v1.2 addresses marked "Sim", the code is the same)
// career + 0x218 + course * 0x24 (course = the .crsinfo index of the race block's course file id + 0x40: EXE Sim 0x8005E764 /
// Arcade 0x8005E674): +0 the record lap as the results record keeps a lap (0x14 bytes: u32 time 1/1000 s, 3 split words, s16 max
// speed, s16; an empty record = 0x8005DD68's -1 / 0 pattern), +0x14 u32 the car id of entry 0, +0x18 the name (12 bytes).
// The race overlay points *(0x800A9524) at the course's record at the race load and the shell's lap line of mode 6 (Sim
// 0x80013824) copies a faster valid lap into it (sim::RaceShellOptions::courseRecordTarget); the mode 6 post-race path (Sim
// 0x8004AA5C) then stores + 0x14 = race block + 0x5C and + 0x18 = the career's name string + 0x7C8F (strcpy), and race block
// + 0x53C = entry 0's car name.
constexpr size_t kCourseRecords = 0x218, kCourseRecordSize = 0x24, kCourseRecordLap = 0x14;
constexpr size_t kCareerRecordName = 0x7C8F;
// The record of course `courseIndex` (copied out; throws outside the career).
std::span<const uint8_t> CourseRecord(std::span<const uint8_t> career, size_t courseIndex);
// A new record of `courseIndex`: the lap (0x14 bytes), `carId` at + 0x14, the career's name string at + 0x18.
void SetCourseRecord(std::span<uint8_t> career, size_t courseIndex, std::span<const uint8_t, kCourseRecordLap> lap, uint32_t carId);
// The two halves of it as the original makes them (docs/research/arcade_disc.md 17.10): the lap is copied into the record by the
// race shell's lap line (Sim 0x80013824 through *(0x800A9524)) during the race; the car id and the name only when the ENTER YOUR
// NAME view is confirmed (Sim 0x8004A990: strcpy(record + 0x18, career + 0x7C8F), record + 0x14 = race block + 0x5C).
void SetCourseRecordLap(std::span<uint8_t> career, size_t courseIndex, std::span<const uint8_t, kCourseRecordLap> lap);
void SetCourseRecordOwner(std::span<uint8_t> career, size_t courseIndex, uint32_t carId);
// The career's name buffer (+ 0x7C8F, 12 bytes; the keyboard edits it in place: the NUL after the text, the bytes behind a
// shorter name kept - the caller passes the keyboard's final text, the bytes after its NUL are cleared).
void SetCareerName(std::span<uint8_t> career, const std::string& name);
std::string CareerName(std::span<const uint8_t> career);

} // namespace gt2::arcade
