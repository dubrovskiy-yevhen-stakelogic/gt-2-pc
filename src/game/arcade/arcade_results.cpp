#include "game/arcade/arcade_results.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gt2::arcade {

void SetCourseWinFlags(std::span<uint8_t> flags, int32_t record, int32_t bits) { // EXE 0x8005DC64
    if (record < 0 || record >= 32) return;
    if (size_t(record) >= flags.size()) throw std::out_of_range("arcade results: course flag byte outside the career");
    uint8_t& b = flags[size_t(record)];
    const uint8_t old = b;
    const uint8_t v = uint8_t(old | uint32_t(bits)); // sb in the delay slot: stored on both paths
    b = v;
    if ((v >> 2) & 1) return;
    if (bits != 2) return;
    uint32_t count = uint32_t((v & 0x38) >> 3) + 1;
    if (count >= 5) {
        count = 5;
        b = uint8_t(old | 6); // ori v0, a1, 6 (a1 = the byte before the |= bits)
    }
    b = uint8_t((b & 0xC7) | (count << 3));
}

bool ApplyArcadeRaceResult(std::span<uint8_t> career, std::span<const uint8_t> raceBlock, int32_t place) { // 0x80050EF0 (1P)
    if (career.size() < 0xB8 + 0x20 || raceBlock.size() < 0x58C) throw std::invalid_argument("arcade results: career / race block too short");
    const uint8_t mode = raceBlock[0x0A];
    if (mode == 0) throw std::invalid_argument("arcade results: a 2 player Battle result takes both places (ApplyBattleResult)");
    const bool roadRace = raceBlock[0x09] == 0 && mode == 4;
    const int32_t record = int8_t(raceBlock[0x57E]);
    if (!roadRace || place - 1 != 0 || record < 0) return false;
    const uint8_t before = career[0xB8 + size_t(record < 32 ? record : 0)];
    SetCourseWinFlags(career.subspan(0xB8), record, int8_t(raceBlock[0x57F]));
    return record < 32 && career[0xB8 + size_t(record)] != before;
}

int ApplyBattleResult(std::span<uint8_t> career, int32_t place1, int32_t place2) { // 0x80050EF0, game mode 0 (0x80050F78..0x80050FF4)
    if (career.size() < kBattleWins + 4) throw std::invalid_argument("arcade results: career too short for the battle counts");
    const int16_t p1 = int16_t(place1), p2 = int16_t(place2); // the results records' s16 + 0 (0x801D58E8 / 0x801D9E00)
    const bool first = (p1 > 0 && p2 > 0) ? p1 < p2 : p1 > 0;
    const size_t at = kBattleWins + (first ? 0 : 2);
    const uint16_t count = uint16_t(career[at] | career[at + 1] << 8) + 1; // lhu / addiu / sh
    career[at] = uint8_t(count & 0xFF);
    career[at + 1] = uint8_t(count >> 8);
    return first ? 0 : 1;
}

std::span<const uint8_t> CourseRecord(std::span<const uint8_t> career, size_t courseIndex) {
    const size_t at = kCourseRecords + courseIndex * kCourseRecordSize;
    if (at + kCourseRecordSize > career.size()) throw std::out_of_range("arcade results: course record outside the career");
    return career.subspan(at, kCourseRecordSize);
}

void SetCourseRecord(std::span<uint8_t> career, size_t courseIndex, std::span<const uint8_t, kCourseRecordLap> lap, uint32_t carId) {
    const size_t at = kCourseRecords + courseIndex * kCourseRecordSize;
    if (at + kCourseRecordSize > career.size() || kCareerRecordName + 1 > career.size()) throw std::out_of_range("arcade results: course record outside the career");
    std::memcpy(career.data() + at, lap.data(), kCourseRecordLap);
    std::memcpy(career.data() + at + 0x14, &carId, 4);
    // strcpy(record + 0x18, career + 0x7C8F) (Sim 0x8004AA6C -> 0x8008CEDC); the record keeps 12 bytes.
    size_t n = 0;
    while (kCareerRecordName + n < career.size() && career[kCareerRecordName + n] != 0 && n < 11) n++;
    std::memset(career.data() + at + 0x18, 0, 12);
    std::memcpy(career.data() + at + 0x18, career.data() + kCareerRecordName, n);
}

void SetCourseRecordLap(std::span<uint8_t> career, size_t courseIndex, std::span<const uint8_t, kCourseRecordLap> lap) {
    const size_t at = kCourseRecords + courseIndex * kCourseRecordSize;
    if (at + kCourseRecordSize > career.size()) throw std::out_of_range("arcade results: course record outside the career");
    std::memcpy(career.data() + at, lap.data(), kCourseRecordLap);
}

void SetCourseRecordOwner(std::span<uint8_t> career, size_t courseIndex, uint32_t carId) { // Sim 0x8004A990
    const size_t at = kCourseRecords + courseIndex * kCourseRecordSize;
    if (at + kCourseRecordSize > career.size() || kCareerRecordName + 12 > career.size()) throw std::out_of_range("arcade results: course record outside the career");
    // strcpy(record + 0x18, career + 0x7C8F) (0x8008CEDC): the text and its NUL; the record's bytes behind it are kept.
    size_t n = 0;
    while (n < 11 && career[kCareerRecordName + n] != 0) n++;
    std::memcpy(career.data() + at + 0x18, career.data() + kCareerRecordName, n);
    career[at + 0x18 + n] = 0;
    std::memcpy(career.data() + at + 0x14, &carId, 4);
}

void SetCareerName(std::span<uint8_t> career, const std::string& name) {
    if (kCareerRecordName + 12 > career.size()) throw std::out_of_range("arcade results: the name buffer lies outside the career");
    const size_t n = std::min<size_t>(name.size(), 11);
    std::memset(career.data() + kCareerRecordName, 0, 12);
    std::memcpy(career.data() + kCareerRecordName, name.data(), n);
}

std::string CareerName(std::span<const uint8_t> career) {
    std::string s;
    for (size_t k = 0; k < 11 && kCareerRecordName + k < career.size() && career[kCareerRecordName + k] != 0; k++) s.push_back(char(career[kCareerRecordName + k]));
    return s;
}

} // namespace gt2::arcade
