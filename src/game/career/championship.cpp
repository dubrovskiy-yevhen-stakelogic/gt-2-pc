#include "game/career/championship.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace gt2::career {

std::string SeriesBase(const std::string& name) { // 0x80018970
    const int32_t n = int32_t(name.size()) - 2;
    return n > 0 ? name.substr(0, size_t(n)) : std::string();
}

int32_t SeriesRaceCount(const std::string& base) { // 0x80018A00 (digit 0x80018954: non-digits count 0)
    const int32_t len = int32_t(base.size());
    if (len < 2) return 0;
    int32_t value = 0;
    for (int32_t i = len - 2; i < len; i++) {
        const int32_t c = int32_t(int8_t(base[size_t(i)]));
        const int32_t digit = uint32_t(c - 0x30) < 10 ? c - 0x30 : 0;
        value = value * 10 + digit;
    }
    return value;
}

std::string SeriesEventName(const std::string& base, int32_t race) { // 0x800189D0: sprintf "%s%02d" (ovl4 0x80050C14)
    char digits[16];
    std::snprintf(digits, sizeof(digits), "%02d", race);
    return base + digits;
}

std::string SeriesPrizeEvent(const std::string& name) { // 0x80018C8C (events; licences use their own names)
    const std::string base = SeriesBase(name);
    return SeriesRaceCount(base) < 2 ? name : SeriesEventName(base, 1);
}

void BuildSeries(SeriesBlock& b, const std::string& name, const CareerData& d, const EventMenuData& menu, uint32_t vsync) { // 0x80018A84
    std::memset(&b, 0, sizeof(b)); // 0x8005E658
    uint32_t seed = vsync;         // 0x8007D23C(0)
    const std::string base = SeriesBase(name);
    const int32_t count = SeriesRaceCount(base);
    b.count = int16_t(count);
    for (int32_t i = 0; i < count; i++) {
        const std::string race = count < 2 ? name : SeriesEventName(base, i + 1);
        const int32_t row = d.race.FindEvent(race); // 0x8007830C
        if (row < 0) throw std::runtime_error("series " + name + ": race " + race + " is not in the race file (the original reads through a null row)");
        const RaceEvent e = d.race.EventAt(size_t(row));
        std::string course = e.course;
        if (course == "none") { // 0x8001907C with the series' generator
            if (menu.randomCourses.empty()) throw std::runtime_error("event course list (ovl4 0x80050C4C) is empty");
            course = menu.randomCourses[NextRandom(seed) % uint32_t(menu.randomCourses.size())];
        }
        if (i >= 6) throw std::logic_error("series with more than 6 races (the original writes past its block)");
        b.laps[i] = e.bytes.at(0x45);
        b.courseId[i] = CourseFileId(course);
        if (race.size() >= 16) throw std::logic_error("series race name longer than its 16-byte field");
        std::memcpy(b.names[i], race.c_str(), race.size() + 1);
    }
}

void AddRacePoints(SeriesBlock& b) { // 0x8005E67C
    for (size_t i = 0; i < 6; i++) b.pointsTotal[i] = int8_t(b.pointsTotal[i] + b.pointsRace[i]);
}

void SetRacePoints(SeriesBlock& b, std::span<const int32_t> positions, const std::array<uint8_t, 6>& pointsByPosition) { // ovl0 0x80013824
    int16_t byPosition[6] = {-1, -1, -1, -1, -1, -1};
    for (size_t car = 0; car < 6; car++) b.pointsRace[car] = 0;
    for (size_t car = 0; car < positions.size() && car < 6; car++) {
        const uint32_t p = uint32_t(positions[car] - 1);
        if (p < 6) byPosition[p] = int16_t(car);
    }
    for (size_t i = 0; i < 6; i++)
        if (byPosition[i] >= 0) b.pointsRace[size_t(byPosition[i])] = int8_t(pointsByPosition[i]);
}

void ChampionshipOrder(const SeriesBlock& b, std::array<int8_t, 6>& order) { // 0x8005E6B0
    for (int32_t car = 0; car < 6; car++) {
        int32_t at = car;
        for (int32_t k = 0; k < car; k++)
            if (b.pointsTotal[order[size_t(k)]] < b.pointsTotal[car]) {
                for (int32_t j = car; j > k; j--) order[size_t(j)] = order[size_t(j - 1)];
                at = k;
                break;
            }
        order[size_t(at)] = int8_t(car);
    }
}

ChampionshipEnd FinishChampionship(CareerState& s, const SeriesBlock& b, const PrizeBlock& prizes, int32_t resultIndex, bool gtwFlag, uint32_t vsyncCounter) {
    ChampionshipEnd end;
    std::array<int8_t, 6> order{};
    ChampionshipOrder(b, order);
    int32_t place = 6;
    for (int32_t k = 0; k < 6; k++)
        if (order[size_t(k)] == 0) place = k;
    SetResult(s.record, resultIndex, place + 1); // 0x8005DBC0(0x801C9998, 0x801D5DD8, place + 1)
    end.place = place + 1;
    if (place == 0) {
        end.champion = true;
        end.prize = ApplyChampionshipEnd(s, s.garage, prizes, gtwFlag, vsyncCounter); // the view 0x8005D510 -> 0x80059800
    }
    return end;
}

// ---------------------------------------------------------------- race block name / course fields

namespace {
void StrCpy(std::span<uint8_t> block, size_t at, const std::string& text) {
    if (at + text.size() + 1 > block.size()) throw std::out_of_range("race block: string beyond the block");
    std::memcpy(block.data() + at, text.c_str(), text.size() + 1);
}
} // namespace

void SetRaceEventName(std::span<uint8_t> raceBlock, const std::string& name) { // 0x8005E548
    std::memset(raceBlock.data() + 0x10, 0, 0x10);
    StrCpy(raceBlock, 0x10, name);
}

uint32_t CourseIndexOfId(const CourseInfoTable& info, uint32_t id) { // 0x80060EB4
    for (uint32_t i = 0; i < info.entries.size(); i++)
        if (info.entries[i].fileId == id) return i;
    return 0;
}

void SetRaceCourse(std::span<uint8_t> raceBlock, uint32_t courseId, const CourseInfoTable& info) { // 0x8005E590
    std::memcpy(raceBlock.data() + 0x40, &courseId, 4);
    const uint32_t index = CourseIndexOfId(info, courseId);
    std::memset(raceBlock.data() + 0x20, 0, 0x20);
    StrCpy(raceBlock, 0x20, info.entries.at(index).name); // *0x80060E94(index): the entry's name pointer
}

} // namespace gt2::career
