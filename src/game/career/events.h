#pragma once
// GT-mode events: the per-event info records of the menus (ovl4 0x80019474 / 0x8001928C), the entry check
// (0x8001973C), the prize block (0x80018C8C) and the opponents of an event race (0x80010A30 / 0x80010714 /
// 0x80010984: random picks among the event's opponent slots, paint rule, racing-modification body). US Simulation
// v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; GT-mode menus = GT2.OVL member 4.
// docs/research/menus_gtmode.md sections 2.4 / 5.3 / 5.4, docs/formats/gtmode_tables.md section 5.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/career/garage.h"
#include "game/career/results.h"

namespace gt2::career {

// The menu's own event list (ovl4 0x80050D1C, 248 name pointers) and the name lists / prefixes of the two special
// checks of 0x8001973C, read from the GT-mode overlay on the disc.
struct EventMenuData {
    std::vector<std::string> events;           // 0x80050D1C: index = the result entry of the event (0x800188B0)
    std::vector<std::string> goldListA;        // 0x80050BB4 ("GJL0001"...): all must be won for the "PFL" / "EPL" events
    std::vector<std::string> goldListB;        // 0x80050BF8 ("PFL0001"...): all must be won for the "GTW" events
    std::array<std::string, 3> prefixes;       // 0x80022E64 "PFL", 0x80022E68 "EPL", 0x80022F1C "GTW"
    std::vector<std::string> randomCourses;    // 0x80050C4C ("autumn"...): courses of the events whose course is "none" (0x8001907C)
    std::array<std::string, 3> machineTests;   // 0x80022F20 / 0x80022F28 / 0x80022F30: the machine-test events (0x8001861C)
    std::vector<std::string> licencePrefixes;  // 0x80050D04: the six licence test name prefixes, licence order S, IA, IB, IC, A, B
    std::array<uint32_t, 6> licenceMessages{}; // 0x80051164: the menu message per licence (0x80019B88)
    static EventMenuData Load(const GuestImage& ovl4);
};

// 0x8001861C(name): race sub-mode 7 / 8 / 9 of the machine tests (set up by 0x80012C6C instead of an event race), -1
// for other names.
int32_t MachineTestMode(const EventMenuData& menu, const std::string& name);

// 0x800190E4(name): the licence (0 S .. 5 B) of a licence test name by its first three characters (0 when none).
int32_t LicenceOfTest(const EventMenuData& menu, const std::string& name);
// 0x80019B88(name, &message): the menus' entry check of a licence test (names starting with 'L', 0x80018608): the
// tests of licence L < 5 need licence L + 1 (0x8001915C): 1 admitted, -3 refused; `message` = 0x80051164[L].
int32_t LicenceEntryCheck(const CareerState& s, const EventMenuData& menu, const std::string& name, uint32_t& message);

#pragma pack(push, 1)
// Info record of one menu event (0x24 bytes at 0x800B5E88 + index * 0x24; 0x8001928C).
struct EventInfo {
    int32_t bonus;            // +00 row +0x98 x 100
    int32_t prize[6];         // +04 row +0x78.. x 100
    uint16_t rules;           // +1C bit 0 dirt tyres (row +0x75), 1..3 licence (+0x47), 4..6 drive (+0x77), 7..8 row +0x9A, 9..10 row +0x9B
    int16_t powerLimit;       // +1E row +0x96
    int16_t carListStart;     // +20 first entry of the event's car list in the pool (0x800B8168)
    uint8_t carListCount;     // +22
    uint8_t pad23;
};
#pragma pack(pop)
static_assert(sizeof(EventInfo) == 0x24);
constexpr uint32_t kEventInfoAddress = 0x800B5E88u, kCarListPoolAddress = 0x800B8168u;

struct EventInfoTable {
    std::vector<EventInfo> infos;     // one per menu event
    std::vector<uint32_t> carListPool; // 0x800B8168: the car ids of all car lists, in menu order
};
// 0x80019474: the info records of all menu events (0x8001928C per event; the car lists appended to the pool).
EventInfoTable BuildEventInfos(const CareerData& d, const EventMenuData& menu);

// 0x800188B0(name): the menu index of an event name, 0 when absent.
int32_t MenuEventIndex(const EventMenuData& menu, const std::string& name);

// State the entry check reads besides the career: the current car's tune sheet (RAM 0x800B4490, loaded for the
// current car by 0x800173E8): +0x54 of its configuration (turbo boost, 0 = naturally aspirated: 0x800175A0) and the
// stages (0x800175B0: racing modification > 0; 0x800175C0: every upgrade kind at stage < 1).
struct EntryCarState {
    bool naturallyAspirated = true;    // 0x800175A0: u8 0x800B44E4 == 0
    bool racingModified = false;       // 0x800175B0: s16 0x800B5C66 > 0
    bool stock = true;                 // 0x800175C0
};
EntryCarState EntryCarStateOf(const TuneSheet& sheet);

// 0x8001973C(name, &message): 1 when the current car may enter, else -1 no car, -9 / -10 the special series not
// won, -2 dirt tyres, -3 licence, -4 drive train, -5 aspiration, -6 racing modification / stock rule, -7 power,
// -8 not in the car list. `message` gets the menu message id (0x800000xx) the original stores.
int32_t EntryCheck(const CareerState& s, const GarageBlock& g, const EventMenuData& menu, const EventInfoTable& infos, const std::string& name,
                   const EntryCarState& car, uint32_t& message);

// 0x80018C8C for an event row (single race: the row of the event's own name): prizes, bonus and the prize cars built
// like purchases with a random paint (generator seeded with the VSync counter). The tune sheet at 0x801DA4B8 and the
// scratchpad are used like the original uses them.
void PreparePrizes(PrizeBlock& p, const RaceEvent& event, uint32_t vsyncCounter, const CareerData& d, TuneSheet& sheet, BuildScratch scratch);

// One car of the event race as 0x80010A30 builds it into the race block's car slots.
struct GridCar {
    uint32_t carId = 0;       // slot +0 (the racing-modification body when the opponent's row has one)
    char paint = 0;           // slot +4 (as int)
    uint16_t colour = 0;      // the paint's chip colour (entry +6)
    uint16_t opponent = 0;    // opponent number (race table 1 row + 1)
    uint8_t paintCode = 0;    // the slot's 6-bit paint code (0 = random paint)
    CarConfig config{};       // slot +8 (0x80076F5C: the opponent's configuration, flags |= 0x40)
    std::string name;         // slot +0x90 (.carinfoa name)
};
// 0x80013108 + 0x8001907C: the course of an event race; an event whose course is "none" gets a random one of the
// menu's list with a generator seeded with the VSync counter (the same value 0x80010A30 starts from).
std::string EventCourse(const EventMenuData& menu, const RaceEvent& event, uint32_t vsyncCounter);

// 0x80010A30(0, 1, event, 0...): the six opponents of an event (the player then replaces slot 0, 0x80011000):
// random picks among the event's used opponent slots (re-picked while the car is hidden in the language, and with
// probability 29/32 while it repeats an earlier pick, at most 64 times), random paint of the car's list unless
// the slot names one (re-drawn once when its saturation is <= 5), the racing-modification body. `seed` = the
// VSync counter the original reads (0x8007D23C(0)); it is advanced like the original's generator.
std::vector<GridCar> PickEventOpponents(const CareerData& d, const RaceEvent& event, uint8_t language, uint32_t& seed, size_t count = 6);

} // namespace gt2::career
