#include "game/career/results.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>

#include "game/career/garage.h"
#include "game/sim/race_shell.h"

namespace gt2::career {

void AddRaceStats(CareerRecord& r, int32_t position) { // 0x8005DC64
    r.positionSum = int32_t(uint32_t(r.positionSum) + uint32_t(position));
    r.races = int32_t(uint32_t(r.races) + 1u);
    if (position == 1) r.wins = int32_t(uint32_t(r.wins) + 1u);
}

void AddPrizeTotal(CareerRecord& r, int32_t prize) { // 0x8005DC9C
    r.prizeTotal = r.prizeTotal + uint32_t(prize);
    if (r.prizeTotal > 100000000u) {
        r.prizeCarry = int32_t(uint32_t(r.prizeCarry) + 1u);
        if (r.prizeCarry < 0) r.prizeCarry = 0x7FFFFFFF;
        r.prizeTotal = r.prizeTotal - 100000000u;
    }
}

int32_t ResultAt(const CareerRecord& r, int32_t index) { // 0x8005DB90
    const uint8_t b = r.results[index / 2];
    return (uint32_t(index) & 1) == 0 ? (b & 0xF) : (b >> 4);
}

int32_t SetResult(CareerRecord& r, int32_t index, int32_t value) { // 0x8005DBC0
    if (index < 0 || value == 0) return 0;
    const int32_t current = ResultAt(r, index);
    if (!(current == 0 || value < current)) return 0;
    uint8_t& b = r.results[index / 2];
    if ((index & 1) == 0) b = uint8_t((b & 0xF0) | uint8_t(value));
    else b = uint8_t((b & 0x0F) | uint8_t(value << 4));
    return 1;
}

uint32_t NextRandom(uint32_t& seed) { // 0x80083AE0
    seed = seed * 17u + 17u;
    return seed ^ ((seed << 16) | (seed >> 16));
}

uint32_t RandomIndex(uint32_t vsyncCounter, uint32_t count) { // 0x800597C4
    uint32_t seed = vsyncCounter;
    const uint32_t r = NextRandom(seed);
    return count == 0 ? r : r % count; // `divu` by zero leaves the quotient registers undefined; never called with 0
}

RaceOutcome ApplyRaceResult(CareerRecord& r, GarageBlock& g, const PrizeBlock& prizes, int32_t position, int32_t resultIndex, uint32_t vsyncCounter) { // 0x80059A7C
    if (position < 1 || position > 6) throw std::out_of_range("race result: position outside 1..6 (the original indexes the prize table with it)");
    RaceOutcome o;
    o.position = position;
    o.prize = prizes.prize[position - 1];
    AddRaceStats(r, position);
    AddPrizeTotal(r, o.prize);
    SetResult(r, resultIndex, position);
    AddMoney(g, o.prize);
    if (prizes.prizeCarCount != 0 && position - 1 == 0) {
        o.prizeCar = int32_t(RandomIndex(vsyncCounter, uint32_t(int32_t(prizes.prizeCarCount))));
        o.prizeCarAdded = AddPreparedCar(g, prizes.prizeCars[o.prizeCar]) != 0;
    }
    return o;
}

RaceOutcome ApplyChampionshipRace(CareerRecord& r, GarageBlock& g, const PrizeBlock& prizes, int32_t position) { // 0x80059704
    RaceOutcome o;
    o.position = position;
    o.prize = prizes.prize[position - 1];
    AddRaceStats(r, position);
    AddPrizeTotal(r, o.prize);
    AddMoney(g, o.prize);
    return o;
}

RaceOutcome ApplyChampionshipEnd(CareerState& s, GarageBlock& g, const PrizeBlock& prizes, bool championshipFlag, uint32_t vsyncCounter) { // 0x80059800
    RaceOutcome o;
    o.prize = prizes.bonus;
    AddMoney(g, prizes.bonus);
    if (prizes.prizeCarCount != 0) {
        o.prizeCar = int32_t(RandomIndex(vsyncCounter, uint32_t(int32_t(prizes.prizeCarCount))));
        o.prizeCarAdded = AddPreparedCar(g, prizes.prizeCars[o.prizeCar]) != 0;
    }
    if (championshipFlag) reinterpret_cast<uint8_t*>(&s)[0x215] = 1; // 0x801C9AF5
    return o;
}

bool LicenceHeld(const CareerState& s, int32_t licence) { // 0x8001915C
    bool held = true;
    for (int32_t test = 0; test < int32_t(kLicenceTests); test++)
        if (s.licences[licence][test].passed == 0) held = false;
    return held;
}

int32_t LicenceLevel(const CareerState& s) { // 0x800191C4
    for (int32_t l = 0; l < int32_t(kLicenceCount); l++)
        if (LicenceHeld(s, l)) return l;
    return int32_t(kLicenceCount);
}

void AdvanceDay(CareerState& s) { s.record.days = s.record.days + 1u; } // ovl4 0x80013628: DAT_801c99d8 += 1

bool LicenceTestOfName(const std::string& name, int32_t& test) { // ovl4 0x80010078: (name[3] - '0') * 10 + name[4] - '0'
    if (name.size() < 5 || name[3] < '0' || name[3] > '9' || name[4] < '0' || name[4] > '9') return false;
    test = (name[3] - '0') * 10 + (name[4] - '0');
    return test >= 0 && test < int32_t(kLicenceTests);
}

int32_t RecordLicenceResult(CareerState& s, int32_t licence, int32_t test, uint32_t time, const uint8_t* settings) { // ovl0 0x8004DD80
    if (licence < 0 || licence >= int32_t(kLicenceCount) || test < 0 || test >= int32_t(kLicenceTests))
        throw std::logic_error("licence record outside the career block (the original writes there)");
    LicenceTestRecord& r = s.licences[licence][test];
    // The prize by the medal times of 0x8003D7B8 (unsigned compares), from the fourth up to gold.
    int32_t prize = time < sim::LicenseTargetTime(settings, 4) ? 1 : 0;
    if (time < sim::LicenseTargetTime(settings, 3)) prize = 2;
    if (time < sim::LicenseTargetTime(settings, 2)) prize = 3;
    if (time < sim::LicenseTargetTime(settings, 1)) prize = 4;
    if (int8_t(r.passed) == 0 && prize == 1) {
        if (r.byte2 < 255) r.byte2 = uint8_t(r.byte2 + 1);
        if (r.byte2 >= settings[0x30]) {
            r.passed = 1;
            return prize;
        }
        prize = 0;
    }
    if (int8_t(r.passed) < prize) r.passed = uint8_t(prize);
    return prize;
}

bool LicenceAllGold(const CareerState& s, int32_t licence) { // ovl0 0x8004DD14
    bool gold = true;
    for (int32_t test = 0; test < int32_t(kLicenceTests); test++)
        if (int8_t(s.licences[licence][test].passed) < 4) gold = false;
    return gold;
}

// ---------------------------------------------------------------- licence test best times

namespace {

// strcpy into a 12-byte entry (the names are at most 11 characters; a longer source is cut at the entry's end).
void CopyName(uint8_t* dst, const uint8_t* src) {
    for (size_t i = 0; i < 0x0C; i++) {
        dst[i] = src[i];
        if (src[i] == 0) return;
    }
}

} // namespace

int32_t LicenceRecordRank(const LicenceTestRecord& r, const TimeRecord& time) { // 0x8005DE8C
    if (time.time[0] == -1) return -1;
    for (int32_t i = 0; i < 5; i++) {
        if (r.times[i].time[0] == -1) return i;
        if (uint32_t(time.time[0]) < uint32_t(r.times[i].time[0])) return i;
    }
    return -1;
}

void StoreLicenceRecord(LicenceTestRecord& r, const TimeRecord& time, const std::string& name) { // 0x8005DEFC
    const int32_t rank = LicenceRecordRank(r, time);
    if (rank < 0) throw std::logic_error("StoreLicenceRecord: the time does not rank (0x8005DEFC is called for a ranked time only)");
    for (int32_t s = 3; s >= rank; s--) {
        CopyName(r.entries[s + 1], r.entries[s]);
        r.times[s + 1] = r.times[s];
    }
    r.times[rank] = time;
    uint8_t text[0x0C] = {};
    for (size_t i = 0; i < name.size() && i < 0x0B; i++) text[i] = uint8_t(name[i]);
    CopyName(r.entries[rank], text);
}

int32_t TimeRecordSector(const TimeRecord& r, int32_t i) { // 0x8005DD94
    int32_t end = r.time[0];
    if (i < 3) end = r.time[i + 1];
    int32_t start = 0;
    if (i - 1 >= 0) start = r.time[i];
    if (end == -1 && start != -1) end = r.time[0];
    if (end == start) end = -1;
    if (end == -1 || start == -1) return -1;
    return int32_t(uint32_t(end) - uint32_t(start));
}

std::string EnteredName(const CareerState& s) {
    const uint8_t* buffer = reinterpret_cast<const uint8_t*>(&s.garage) + offsetof(GarageBlock, byte401B); // + tail: 13 bytes to the block's end
    std::string name;
    for (size_t i = 0; i < 1 + sizeof(s.garage.tail) && buffer[i] != 0; i++) name.push_back(char(buffer[i]));
    return name;
}

void SetEnteredName(CareerState& s, const std::string& name) {
    uint8_t* buffer = reinterpret_cast<uint8_t*>(&s.garage) + offsetof(GarageBlock, byte401B);
    const size_t size = 1 + sizeof(s.garage.tail);
    const size_t previous = EnteredName(s).size();
    const size_t length = std::min(name.size(), size - 1);
    for (size_t i = 0; i < length; i++) buffer[i] = uint8_t(name[i]);
    for (size_t i = length; i <= previous && i < size; i++) buffer[i] = 0;
}

} // namespace gt2::career
