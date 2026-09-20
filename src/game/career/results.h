#pragma once
// Race results applied to the GT-mode career: statistics, prize money, the result nibbles (medals / best places),
// the random prize car, the championship end, the licence level and the day counter. Ported from the race overlay
// (GT2.OVL member 0 at 0x80010000: 0x80059A7C / 0x80059704 / 0x80059800 / 0x800597C4), the EXE (0x8005DC64,
// 0x8005DC9C, 0x8005DB90, 0x8005DBC0, 0x80083AE0) and the GT-mode overlay (0x8001915C, 0x800191C4, 0x80013628);
// US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a. docs/research/menus_gtmode.md section 5.4.
#include <cstddef>
#include <cstdint>
#include <string>

#include "game/career/career_state.h"

namespace gt2::career {

#pragma pack(push, 1)
// The prize block the GT-mode overlay prepares before a race (0x80018C8C; RAM 0x801D55AC) and the race overlay
// pays out: credits are the event row's u16 values x 100.
struct PrizeBlock {
    int32_t bonus;               // +00 championship bonus (event row +0x98 x 100)
    int32_t prize[6];            // +04 prize by finishing position (row +0x78..)
    int8_t prizeCarCount;        // +1C
    uint8_t pad1D[3];
    GarageCar prizeCars[4];      // +20 garage slots built like purchases (random paint, value = catalogue price)
};
#pragma pack(pop)
static_assert(sizeof(PrizeBlock) == 0x2B0 && offsetof(PrizeBlock, prizeCars) == 0x20);
constexpr uint32_t kPrizeBlockAddress = 0x801D55ACu;
constexpr uint32_t kRacePositionAddress = 0x801D5DE8u;  // s8 finishing position of the player (1-based)
constexpr uint32_t kResultIndexAddress = 0x801D5DD8u;   // s16 the event's result entry (0x800188B0), -1 none
constexpr uint32_t kChampionshipFlagAddress = 0x801D5DDCu; // u8 set when the championship was won (0x80059800)
constexpr uint32_t kVsyncCounterAddress = 0x801F0680u;  // u32 the counter 0x8007D23C(0) returns (the random seed)

// 0x8005DC64(record, position): races + 1, position sum + position, wins + 1 when first.
void AddRaceStats(CareerRecord& r, int32_t position);
// 0x8005DC9C(record, prize): the prize total, carried into +0x58 above 100,000,000 (unsigned compare).
void AddPrizeTotal(CareerRecord& r, int32_t prize);
// 0x8005DB90(record, index): the 4-bit result entry.
int32_t ResultAt(const CareerRecord& r, int32_t index);
// 0x8005DBC0(record, index, value): stores value when index >= 0, value != 0 and the entry is empty or worse;
// returns 1 when stored.
int32_t SetResult(CareerRecord& r, int32_t index, int32_t value);

// 0x80083AE0(&seed): seed = seed * 17 + 17, returns seed ^ (seed rotated by 16).
uint32_t NextRandom(uint32_t& seed);
// 0x800597C4(count): a random index below count from a generator seeded with the VSync counter (0x8007D23C(0)).
uint32_t RandomIndex(uint32_t vsyncCounter, uint32_t count);

// What a results routine did (for the caller's display; the original shows it with 0x800595E0).
struct RaceOutcome {
    int32_t position = 0;
    int32_t prize = 0;
    int32_t prizeCar = -1;       // index into PrizeBlock::prizeCars, -1 none
    bool prizeCarAdded = false;  // 0x8005E7F0's result (false when the garage was full)
};

// 0x80059A7C: a single race of an event finished at `position` (1..6): statistics, prize total, result entry,
// money, and for a win the random prize car. `guestGarage` unused (player 0 only, as the original).
RaceOutcome ApplyRaceResult(CareerRecord& r, GarageBlock& g, const PrizeBlock& prizes, int32_t position, int32_t resultIndex, uint32_t vsyncCounter);
// 0x80059704: the per-race variant without the result entry and the prize car (races of a championship).
RaceOutcome ApplyChampionshipRace(CareerRecord& r, GarageBlock& g, const PrizeBlock& prizes, int32_t position);
// 0x80059800 (rules only; the prize screen it then loads is UI): money += the bonus, the random prize car without a
// position condition, the byte 0x801C9AF5 (career state + 0x215) = 1 when the championship flag is set.
RaceOutcome ApplyChampionshipEnd(CareerState& s, GarageBlock& g, const PrizeBlock& prizes, bool championshipFlag, uint32_t vsyncCounter);

// 0x8001915C(licence): all 10 test records of licence L (0 S .. 5 B) have byte +1 != 0.
bool LicenceHeld(const CareerState& s, int32_t licence);
// 0x800191C4: the first licence held in the order S, IA, IB, IC, A, B (0..5), 6 = none. An event requiring r
// (1 B .. 6 S) admits the player when 6 - LicenceLevel() >= r.
int32_t LicenceLevel(const CareerState& s);

// ovl4 0x80013628: every event (and licence test) race started from the GT-mode menus advances the day by one.
void AdvanceDay(CareerState& s);

// ---------------------------------------------------------------- licence test results (race overlay)

// The race block bytes that name the licence test of a race (written by ovl4 0x80010078 from the test name "LJBnn":
// +0x0B = 0x80010000(name) = the licence 0 S .. 5 B, +0x0C = the two digits nn = the test 0..9).
constexpr uint32_t kRaceLicenceAddress = 0x801D5867u, kRaceLicenceTestAddress = 0x801D5868u;
constexpr uint32_t kLicenceTimeAddress = 0x801D5DF0u; // u32 the licence result time (1/1000 s, 0x800156EC)
// The test index of a licence test name ("LJB00" -> 0; the licence comes from the prefix, 0x80010000 = events.h
// LicenceOfTest): the two digits after the three-character prefix; false when they are missing or >= 10.
bool LicenceTestOfName(const std::string& name, int32_t& test);

// Race overlay 0x8004DD80: records a PASSED licence test (the caller 0x8004E104 runs it when the result code
// 0x801D5DEC is 1 and the pass flag 0x8005B3D0 is set) into the career's test record (state + 0x1418 + licence x
// 0x668 + test x 0xA4): the prize of `time` from the test's settings block (`settings` = its database record + 0x44,
// 0x8003D7B8): 4 gold (time < medal 1), 3 silver (< 2), 2 bronze (< 3), 1 the fourth prize (< 4), 0 none. The fourth
// prize of a never-passed test counts in byte +2 (saturating at 255) and marks the test passed (+1 = 1) once the count
// reaches settings + 0x30; any better prize is stored in +1 when it beats the stored one (signed compare). Returns
// the prize computed (the value the routine keeps in s1 at its end: 0 when the fourth prize did not reach the count).
int32_t RecordLicenceResult(CareerState& s, int32_t licence, int32_t test, uint32_t time, const uint8_t* settings);
// 0x8004DD14(licence): every test of the licence has prize >= 4 (gold). 0x8004DCAC(licence) is LicenceHeld.
bool LicenceAllGold(const CareerState& s, int32_t licence);

// ---------------------------------------------------------------- licence test best times (EXE)

// The player's lap entry the race shell fills (0x801D5E90 = results[0].laps[0], sim::LapEntry: time, three splits, max
// speed readout): after a passed licence test the time is lap 1 (0x8005E3C4); 0x8004E104 ranks it, 0x8004E658 stores it.
constexpr uint32_t kLicenceLapAddress = 0x801D5E90u;
// 0x8005DE8C(record, time): the slot (0..4) `time` takes among the record's five best times - the first one that is
// empty (time[0] == -1) or slower (time.time[0] < slot time[0], unsigned: a tie does not rank); -1 when time.time[0] is
// -1 or the time is not among the five.
int32_t LicenceRecordRank(const LicenceTestRecord& r, const TimeRecord& time);
// 0x8005DEFC(record, time, name): the slots rank..3 move one down (strcpy of the 12-byte names, the 20-byte times; the
// fifth is dropped), then `time` (20 bytes) and `name` (strcpy) go into the slot of 0x8005DE8C. The caller stores a
// ranked time only (the original's loop is not bounded for -1): throws std::logic_error for an unranked one.
void StoreLicenceRecord(LicenceTestRecord& r, const TimeRecord& time, const std::string& name);
// 0x8005DD94(record, i): the time of sector i (0..3) from the cumulative splits time[1..3] and the total time[0]:
// split[i] - split[i - 1] (split[-1] = 0, split[3] = the total); a missing split (-1) with a known previous one is the
// total; equal ends or a missing one give -1.
int32_t TimeRecordSector(const TimeRecord& r, int32_t i);
// The name entered last on the NEW RECORD keyboard (0x801D156F = garage + 0x401B: up to 11 characters and a NUL,
// cleared by a new game), the keyboard's initial text (ovl0 0x8004E5E8 / 0x8004E658). SetEnteredName writes the text and
// its NUL and clears the bytes of a longer previous name (the keyboard edits the buffer in place and its deletions
// leave NULs behind).
std::string EnteredName(const CareerState& s);
void SetEnteredName(CareerState& s, const std::string& name);

} // namespace gt2::career
