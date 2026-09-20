#pragma once
// GT-mode championships (series of events): the series block the GT-mode overlay builds before the race
// (0x80018A84 into 0x801D5DF4: the races' names, courses and laps), the race block's name / course fields (EXE
// 0x8005E548 / 0x8005E590 / 0x80060EB4), the points (0x8005E67C, points table of the race overlay 0x8002F4CC) and
// the championship end of the race overlay (0x80017A28: standings 0x8005E6B0, result entry, the champion's bonus and
// prize car 0x80059800). US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a.
// docs/research/menus_gtmode.md section 8.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/career/events.h"
#include "game/career/results.h"
#include "gt2formats/course_data.h"

namespace gt2::career {

#pragma pack(push, 1)
// The series block (RAM 0x801D5DF4, 0x94 bytes; cleared by 0x8005E658).
struct SeriesBlock {
    int16_t race;               // +00 race in progress, 0-based (0x80017A28 advances it after each race)
    int16_t count;              // +02 races of the series (0x80018A00: the two digits before the race number)
    uint32_t courseId[6];       // +04 0x80083004 of each race's course file name
    uint16_t laps[6];           // +1C event row +0x45 (settings block +1)
    char names[6][16];          // +28 the races' event names
    int8_t pointsTotal[6];      // +88 points per car slot (0x8005E67C adds +8E after each race)
    int8_t pointsRace[6];       // +8E points of the last race per car slot (race overlay 0x80013824)
};
#pragma pack(pop)
static_assert(sizeof(SeriesBlock) == 0x94);
constexpr uint32_t kSeriesAddress = 0x801D5DF4u;
constexpr uint32_t kRaceBlockAddress = 0x801D585Cu;   // the race block (0x58C bytes)
constexpr size_t kRaceBlockSize = 0x58C;

// 0x80018970(out, name): the name without its last two characters (the race number).
std::string SeriesBase(const std::string& name);
// 0x80018A00(base): the base's last two characters as a decimal number (non-digits count 0): the race count.
int32_t SeriesRaceCount(const std::string& base);
// 0x800189D0(out, base, n): "%s%02d".
std::string SeriesEventName(const std::string& base, int32_t race);
// The event whose row 0x80018C8C reads the prizes from: the name itself for single races, else race 1 of the series.
std::string SeriesPrizeEvent(const std::string& name);
// 0x80018A84(block, name): the series block of an event (single events: count 0 or 1 and the name itself); a race
// whose course is "none" gets a random one (0x8001907C) from a generator seeded with the VSync counter `vsync`.
// Throws when a race of the series is not in the race file.
void BuildSeries(SeriesBlock& b, const std::string& name, const CareerData& d, const EventMenuData& menu, uint32_t vsync);

// 0x8005E67C(block): pointsTotal += pointsRace per car slot.
void AddRacePoints(SeriesBlock& b);
// The race overlay's points of a finished race (0x80013824, modes 2 / 11): pointsRace[car] = table[position - 1]
// (0x8002F4CC) for the cars with a position 1..6, 0 for the others.
void SetRacePoints(SeriesBlock& b, std::span<const int32_t> positions, const std::array<uint8_t, 6>& pointsByPosition);
// 0x8005E6B0(block, order): the car slots sorted by pointsTotal (descending; a car goes before an earlier one only
// with strictly more points).
void ChampionshipOrder(const SeriesBlock& b, std::array<int8_t, 6>& order);

// The end of a championship as 0x80017A28 applies it (count > 1, after the last race): the player's (slot 0) place
// in the standings goes into the result entry `resultIndex` (0x8005DBC0, place + 1); a champion gets 0x80059800
// (bonus, random prize car, career +0x215 when `gtwFlag` = 0x801D5DDC, the "GTW" series).
struct ChampionshipEnd {
    int32_t place = 0;      // 1-based
    bool champion = false;
    RaceOutcome prize;      // 0x80059800's outcome (champions only)
};
ChampionshipEnd FinishChampionship(CareerState& s, const SeriesBlock& b, const PrizeBlock& prizes, int32_t resultIndex, bool gtwFlag, uint32_t vsyncCounter);

// ---------------------------------------------------------------- race block name / course fields

// 0x8005E548(block, name): +0x10 = the event name (16 bytes cleared, then strcpy).
void SetRaceEventName(std::span<uint8_t> raceBlock, const std::string& name);
// 0x80060EB4(id): the .crsinfo entry with the course file id, 0 when none.
uint32_t CourseIndexOfId(const CourseInfoTable& info, uint32_t id);
// 0x8005E590(block, id): +0x40 = id, +0x20 = the display name of the .crsinfo entry (32 bytes cleared, then strcpy).
void SetRaceCourse(std::span<uint8_t> raceBlock, uint32_t courseId, const CourseInfoTable& info);

} // namespace gt2::career
