#include "game/arcade/arcade_setup.h"
#include "game/pc_features.h"

#include <algorithm>
#include <array>
#include <memory>
#include <cstring>
#include <stdexcept>

#include "game/career/championship.h"
#include "game/career/garage.h"
#include "game/career/tuning.h"
#include "game/career/results.h"

namespace gt2::arcade {

namespace {

template <typename T> T Rd(std::span<const uint8_t> b, size_t o) {
    if (o + sizeof(T) > b.size()) throw std::out_of_range("arcade setup: read past a buffer");
    T v;
    std::memcpy(&v, b.data() + o, sizeof(T));
    return v;
}
template <typename T> void Wr(std::span<uint8_t> b, size_t o, T v) {
    if (o + sizeof(T) > b.size()) throw std::out_of_range("arcade setup: write past a buffer");
    std::memcpy(b.data() + o, &v, sizeof(T));
}
// strcpy: the bytes up to and including the NUL (nothing after it is touched).
void StrCpy(std::span<uint8_t> b, size_t o, const std::string& s) {
    if (o + s.size() + 1 > b.size()) throw std::out_of_range("arcade setup: string past a buffer");
    std::memcpy(b.data() + o, s.c_str(), s.size() + 1);
}
std::string CString(std::span<const uint8_t> b, size_t o) {
    std::string s;
    while (o < b.size() && b[o] != 0) s.push_back(char(b[o++]));
    if (o >= b.size()) throw std::out_of_range("arcade setup: unterminated string");
    return s;
}

// The .carinfoa record the binary search 0x80060934 (Simulation 0x80060A24) lands on (record 0 when the id is absent).
const CarInfoRecord& CarInfoOf(const CarInfoDirectory& cars, uint32_t carId) {
    const int32_t index = cars.IndexOf(carId);
    return cars.At(index < 0 ? 0 : size_t(index));
}
// 0x80060BA0: a paint's index (letters case-insensitive), 0 when absent.
size_t PaintIndexOf(const CarInfoRecord& r, int32_t paint) {
    auto lower = [](int32_t c) { return uint32_t(c - 0x41) < 0x1A ? c + 0x20 : c; };
    const int32_t wanted = lower(paint);
    for (size_t i = 0; i < r.PaintCount(); i++)
        if (lower(int32_t(int8_t(r.paintIds.at(i)))) == wanted) return i;
    return 0;
}
// 0x80060C38(carId, paint): the paint's chip colour.
uint16_t ChipColour(const CarInfoDirectory& cars, uint32_t carId, int32_t paint) {
    const CarInfoRecord& r = CarInfoOf(cars, carId);
    return r.chipColors.at(PaintIndexOf(r, paint));
}
// 0x800100EC: ((max - min) * 32) / max over the three 5-bit channels.
int32_t Saturation(uint16_t colour) {
    const int32_t r = colour & 0x1F, g = (colour >> 5) & 0x1F, b = (colour >> 10) & 0x1F;
    const int32_t hi = std::max(r, std::max(g, b)), lo = std::min(r, std::min(g, b));
    return hi == 0 ? 0 : ((hi - lo) * 0x20) / hi;
}
constexpr char kPaintCharset[] = "-0123456789abcdefghijklmnopqrstuvwxyz"; // EXE (Simulation 0x80091620): index = 6-bit code
char PaintOfCode(uint32_t code) {
    if (code >= sizeof(kPaintCharset) - 1) throw std::logic_error("arcade setup: paint code beyond the character set (not ported)");
    return kPaintCharset[code];
}

// The builder's 12-byte working entry: car id, transmission, paint character, colour, opponent number.
struct Entry {
    uint32_t carId = 0;
    int8_t transmission = 0; // +4
    char paint = 0;          // +5
    uint16_t colour = 0;     // +6
    uint16_t opponent = 0;   // +8
};

constexpr uint32_t kTimeTrialEvent = 0x800267B0u, kBattleEvent = 0x800267B4u, kEmptyString = 0x800267B8u; // member 2 strings: "ATT", "A2P", ""
constexpr size_t kGarageCarsAt = 4, kGarageCarBytes = 0xA4;                         // a garage block: +0 count, +4 the cars

uint32_t OpponentCarId(const ArcadeData& ad, uint32_t number) { return ad.Opponent(number).spec.carId; }

// 0x80010238(entry, tables, event, &seed, entries, count): a random used slot whose car the language shows, redrawn with
// probability 29/32 while an earlier entry has the same opponent or car (64 tries), then the paint. Returns the slot's paint code.
uint32_t PickOpponent(Entry& e, const ArcadeSetupData& d, const RaceEvent& event, uint8_t language, uint32_t& seed, const std::vector<Entry>& earlier) {
    const uint32_t used = uint32_t(event.OpponentCount());
    if (used < 1) return 0;
    bool anyAvailable = false; // the original loops forever when no slot's car exists in the language
    for (uint32_t k = 0; k < used; k++)
        anyAvailable = anyAvailable || CarInfoOf(d.cars, OpponentCarId(d.data, RaceEvent::SlotOpponent(event.slots[k]) & 0xFFFF)).AvailableIn(language);
    if (!anyAvailable) throw std::logic_error("arcade event " + event.name + ": no opponent car of the language (the original never returns)");
    int32_t budget = 0x40;
    uint32_t slot = 0, number = 0;
    for (;;) {
        do {
            slot = event.slots[career::NextRandom(seed) % used];
            number = slot & 0xFFFF;
        } while (!CarInfoOf(d.cars, OpponentCarId(d.data, number)).AvailableIn(language));
        bool reject = false;
        for (const Entry& prior : earlier) {
            if ((prior.opponent == number || prior.carId == OpponentCarId(d.data, number)) && (career::NextRandom(seed) & 0x1F) < 0x1D && budget > 0) {
                reject = true;
                break;
            }
        }
        if (!reject) break;
        budget--;
    }
    const uint32_t code = slot >> 26;
    const uint32_t carId = OpponentCarId(d.data, number);
    const CarInfoRecord& info = CarInfoOf(d.cars, carId);
    if (code != 0) {
        e.paint = PaintOfCode(code);
        e.colour = info.chipColors.at(PaintIndexOf(info, e.paint));
    } else {
        const uint32_t count = uint32_t(info.PaintCount());
        uint32_t r = 0;
        for (int32_t tries = 0; tries < 2; tries++) { // at most two draws; a saturated colour (> 5) ends it
            r = career::NextRandom(seed);
            if (Saturation(info.chipColors.at(r % count)) > 5) break;
        }
        e.paint = char(info.paintIds.at(r % count));
        e.colour = info.chipColors.at(r % count);
    }
    e.carId = carId;
    e.opponent = uint16_t(slot);
    return code;
}

// 0x800104A8(entry, model, code, &seed): the paint of the racing-modification body.
void RepaintForModel(Entry& e, uint32_t model, uint32_t code, const ArcadeSetupData& d, uint32_t& seed) {
    const CarInfoRecord& info = CarInfoOf(d.cars, model);
    if (code == 0) {
        const uint32_t r = career::NextRandom(seed);
        e.colour = info.chipColors.at(r % uint32_t(info.PaintCount()));
        e.paint = char(info.paintIds.at(r % uint32_t(info.PaintCount())));
    } else {
        const char paint = PaintOfCode(code & 0x3F);
        e.colour = info.chipColors.at(PaintIndexOf(info, paint));
        e.paint = paint;
    }
}

// 0x80010000(carId, tyres): the car's row of player car table 32 (tyres != 1) or 33 (0x80078008: the row with the id).
std::span<const uint8_t> PlayerSpec(const ArcadeData& ad, uint32_t carId, int32_t tyres) {
    const size_t table = tyres == 1 ? kArcadePlayerCarTableAlt : kArcadePlayerCarTable;
    for (size_t row = 0; row < ad.PlayerCarCount(); row++) {
        const std::span<const uint8_t> b = ad.PlayerCarBytes(table, row);
        if (Rd<uint32_t>(b, 0) == carId) return b;
    }
    throw std::logic_error("arcade setup: car not in player car table " + std::to_string(table) + " (the original then reads a null spec)");
}

CarConfig ConfigOfSlot(std::span<const uint8_t> block, size_t slot) {
    CarConfig c;
    std::memcpy(&c, block.data() + slot + 8, sizeof(c));
    return c;
}

// The common end of 0x80010C84 (0x80011510..): the player's configuration into the selection (three copies), the garage
// flag, the race context 0x801C2EB0 (course name, 0x2CC, mode, entries, the two numbers, each grid slot's car and chip
// colour) and the view pointer 0x801C3000.
// `garageCars[i]` = the garage car of entry i (0x80010C84's local_34 / local_30: entry 0 in modes 4 / 6, entries 0 / 1 in mode 0),
// empty = the entry's own car.
void FinishBuild(const ArcadeSetupData& d, std::span<uint8_t> region, std::span<uint8_t> sel, std::span<const uint8_t> block, const CarConfig& local, uint32_t p1,
                 uint32_t p2, std::array<std::span<const uint8_t>, 2> garageCars, bool garageFlag) {
    for (const size_t at : {Sel::kConfigA, Sel::kConfigB, Sel::kConfigC}) std::memcpy(sel.data() + at, &local, sizeof(local));
    Wr<uint32_t>(sel, Sel::kGarageFlag, garageFlag ? 1u : 0u);
    const size_t ctx = 0; // region offset of 0x801C2EB0
    StrCpy(region, ctx + 0x0E, CString(sel, Sel::kCourseName));
    Wr<uint16_t>(region, ctx + 0x08, Rd<uint16_t>(sel, Sel::kWord2CC));
    Wr<uint16_t>(region, ctx + 0x0A, uint16_t(int16_t(int8_t(sel[Sel::kMode]))));
    Wr<uint16_t>(region, ctx + 0x0C, block[0x5A]);
    Wr<uint32_t>(region, ctx + 0x00, p1);
    Wr<uint32_t>(region, ctx + 0x04, p2);
    for (size_t i = 0; i < block[0x5A]; i++) {
        const size_t slot = 0x5C + i * 0xD0;
        const size_t grid = block[slot + 0x8D];
        uint32_t carId = Rd<uint32_t>(block, slot);
        int32_t paint = Rd<int32_t>(block, slot + 4);
        if (i < 2 && !garageCars[i].empty()) { // the garage car: its model id (+0x8C) and paint (+4); its slot of the block may be empty
            carId = Rd<uint32_t>(garageCars[i], 0x8C);
            paint = Rd<int32_t>(garageCars[i], 4);
        }
        Wr<uint32_t>(region, ctx + 0x50 + grid * 8, carId);
        Wr<uint16_t>(region, ctx + 0x54 + grid * 8, ChipColour(d.cars, carId, paint));
    }
    Wr<uint32_t>(region, ctx + 0x150, Rd<uint32_t>(sel, Sel::kWord2D0)); // 0x801C3000
}

} // namespace

std::array<uint32_t, 2> MenuEntryNumbers(uint32_t vsync) {
    uint32_t seed = vsync;
    const uint32_t a = career::NextRandom(seed);
    const uint32_t b = career::NextRandom(seed);
    return {a, b};
}

ArcadeRaceSetup BuildArcadeRace(const ArcadeSetupData& d, std::span<const uint8_t> menuRegionIn, std::span<const uint8_t> career, uint32_t p1, uint32_t p2,
                                uint32_t vsync, std::span<const uint8_t> garageBlock) {
    if (menuRegionIn.size() != kMenuRegionSize) throw std::invalid_argument("arcade setup: menu region size");
    const std::span<const uint8_t> sel0 = menuRegionIn.subspan(kSelectionAddress - kMenuRegionAddress, kSelectionSize);
    if (int8_t(sel0[Sel::kMode]) == 0 && !garageBlock.empty())
        throw std::invalid_argument("arcade setup: a 2 player Battle build takes both garage blocks (BuildArcadeRace with GarageBlocks)");
    int16_t g = 0;
    std::memcpy(&g, sel0.data() + Sel::kGarage, 2);
    GarageBlocks garages;
    if (g >= 0 && g < 2) garages[size_t(g)] = garageBlock;
    return BuildArcadeRace(d, menuRegionIn, career, p1, p2, vsync, garages);
}

namespace {

// 0x80010C84 with selection + 2 = 0 (0x80010D1C..0x80010F98): 2 player Battle.
ArcadeRaceSetup BuildBattle(const ArcadeSetupData& d, ArcadeRaceSetup out, std::span<const uint8_t> career, uint32_t p1, uint32_t p2, const GarageBlocks& garages) {
    const std::span<uint8_t> region(out.menuRegion);
    const std::span<uint8_t> sel = region.subspan(kSelectionAddress - kMenuRegionAddress, kSelectionSize);
    const std::span<uint8_t> block(out.raceBlock);
    // The settings of event "ATT" (0x800267B0), the name "A2P" (0x800267B4).
    const std::string settingsName = d.menu.ImageString(kTimeTrialEvent);
    const int32_t row = d.data.FindEvent(settingsName);
    if (row < 0) throw std::logic_error("arcade setup: no event " + settingsName + " (the original reads a null row)");
    const RaceEvent event = d.data.EventAt(size_t(row));
    std::fill(block.begin(), block.end(), uint8_t(0));
    for (size_t k = 0; k < 8; k++) block[k] = career[1 + k]; // career + 6 = 2P laps -> + 5, + 7 handicap start -> + 6, + 8 slow car boost -> + 7
    block[0x08] = 2;
    block[0x09] = 0;
    block[0x0A] = 0;
    block[0x0D] = 1;
    block[0x0E] = 0;
    block[0x0F] = uint8_t(Rd<int16_t>(sel, Sel::kLaps)); // lh / sb
    career::SetRaceEventName(block, d.menu.ImageString(kBattleEvent)); // 0x8005E458
    career::SetRaceCourse(block, Rd<uint32_t>(sel, Sel::kCourseId), d.courses); // 0x8005E4A0
    StrCpy(block, 0x44, event.tag);                                              // 0x8007807C(row + 0x94)
    block[0x5A] = 2;
    Wr<int16_t>(block, 0x582, -1);
    Wr<int16_t>(block, 0x584, -1);
    Wr<int16_t>(block, 0x586, 0);
    block[0x580] = 0;
    Wr<uint32_t>(block, 0x588, ((Rd<uint32_t>(block, 0x588) & ~1u) | (Rd<uint32_t>(sel, Sel::kWord2CC) != 1 ? 1u : 0u)) & ~6u);
    std::array<CarConfig, 2> local{};
    std::array<std::span<const uint8_t>, 2> garageCars{};
    bool garageFlag = false;
    out.records.resize(2);
    for (size_t i = 0; i < 2; i++) {
        const int16_t g = Rd<int16_t>(sel, Sel2P::kGarage + 2 * i);
        if (g < 0) { // 0x80010A34(block, i, i, i + 3, transmission, car, 0x8001003C(car, colour), spec)
            const uint32_t car = Rd<uint32_t>(sel, Sel2P::kCar + 4 * i);
            const std::span<const uint8_t> spec = PlayerSpec(d.data, car, int8_t(sel[Sel2P::kTyres + i]));
            const CarInfoRecord& info = CarInfoOf(d.cars, car);
            const int16_t colour = Rd<int16_t>(sel, Sel2P::kColour + 2 * i);
            if (colour < 0 || size_t(colour) >= info.paintIds.size()) throw std::logic_error("arcade setup: colour index beyond the car's paints");
            const size_t slot = 0x5C + i * 0xD0;
            std::fill(block.begin() + std::ptrdiff_t(slot), block.begin() + std::ptrdiff_t(slot + 0xD0), uint8_t(0));
            block[slot + 0x8C] = 1;
            block[slot + 0x8D] = uint8_t(i);
            block[slot + 0x8E] = uint8_t(i + 3);
            Wr<uint32_t>(block, slot, car);
            block[slot + 0x8F] = sel[Sel2P::kTransmission + i];
            Wr<int32_t>(block, slot + 4, int32_t(int8_t(info.paintIds[size_t(colour)])));
            StrCpy(block, slot + 0x90, info.rawName);
            CarConfig config = ConfigFromCarSpec(d.data.Tables(), spec, false); // 0x80076ED0
            career::BuildMenuRecord(d.data.Tables(), config, out.records[i]);  // 0x800770BC -> 0x801DE31A + i * 0x1C0
            std::memcpy(block.data() + slot + 8, &config, sizeof(config));
            local[i] = ConfigFromCarSpec(d.data.Tables(), spec, false);
        } else { // a garage car: its configuration; the entry is made at the race load (ovl3 0x8001290C, RebuildGarageEntries)
            if (g > 1 || garages[size_t(g)].size() < kGarageCarsAt + 100 * kGarageCarBytes) throw std::invalid_argument("arcade setup: a garage car needs its garage block");
            const int16_t index = Rd<int16_t>(sel, Sel2P::kGarageSlot + 2 * i);
            if (index < 0 || index >= 100) throw std::invalid_argument("arcade setup: garage index");
            garageCars[i] = garages[size_t(g)].subspan(kGarageCarsAt + size_t(index) * kGarageCarBytes, kGarageCarBytes);
            std::memcpy(&local[i], garageCars[i].data() + 8, sizeof(CarConfig));
            garageFlag = true;
        }
    }
    std::copy(event.settings.begin(), event.settings.end(), out.settings.begin()); // "ATT" row + 0x44 -> 0x801C9300
    FinishBuild(d, region, sel, block, local[0], p1, p2, garageCars, garageFlag);
    return out;
}

} // namespace

ArcadeRaceSetup BuildArcadeRace(const ArcadeSetupData& d, std::span<const uint8_t> menuRegionIn, std::span<const uint8_t> career, uint32_t p1, uint32_t p2,
                                uint32_t vsync, const GarageBlocks& garages) {
    if (menuRegionIn.size() != kMenuRegionSize) throw std::invalid_argument("arcade setup: menu region size");
    if (career.size() < 9) throw std::invalid_argument("arcade setup: career bytes");
    ArcadeRaceSetup out;
    std::copy(menuRegionIn.begin(), menuRegionIn.end(), out.menuRegion.begin());
    const std::span<uint8_t> region(out.menuRegion);
    const std::span<uint8_t> sel = region.subspan(kSelectionAddress - kMenuRegionAddress, kSelectionSize);
    const std::span<uint8_t> block(out.raceBlock);
    const int8_t mode = int8_t(sel[Sel::kMode]);
    if (mode == 0) return BuildBattle(d, std::move(out), career, p1, p2, garages);
    if (mode != 4 && mode != 6) throw std::logic_error("arcade setup: game mode " + std::to_string(mode) + " (the menus build 0, 4 and 6)");
    std::span<const uint8_t> garageBlock;
    {
        const int16_t g = Rd<int16_t>(sel, Sel::kGarage);
        if (g >= 0 && g < 2) garageBlock = garages[size_t(g)];
    }
    const bool fromGarage = Rd<int16_t>(sel, Sel::kGarage) >= 0;
    std::span<const uint8_t> garageCar;
    if (fromGarage) {
        const int16_t index = Rd<int16_t>(sel, Sel::kGarageSlot);
        if (garageBlock.size() < kGarageCarsAt + 100 * kGarageCarBytes || index < 0 || index >= 100)
            throw std::invalid_argument("arcade setup: a garage car needs its garage block");
        garageCar = garageBlock.subspan(kGarageCarsAt + size_t(index) * kGarageCarBytes, kGarageCarBytes);
    }
    const uint8_t language = career[0];

    if (mode == 6) { // 0x80011038..0x8001126C: Rally and Time Trial (selection + 0x2CC = 1 for Rally)
        const std::string name = d.menu.ImageString(kTimeTrialEvent); // "ATT"
        const int32_t row = d.data.FindEvent(name);
        if (row < 0) throw std::logic_error("arcade setup: no event " + name + " (the original reads a null row)");
        const RaceEvent event = d.data.EventAt(size_t(row));
        std::copy(event.settings.begin(), event.settings.end(), out.settings.begin()); // row + 0x44 -> 0x801C9300
        std::fill(block.begin(), block.end(), uint8_t(0));
        for (size_t k = 0; k < 8; k++) block[k] = career[1 + k];
        block[0x08] = 2;
        block[0x0A] = 6;
        block[0x09] = 0, block[0x0D] = 0, block[0x0E] = 0; // no countdown: the rolling start of the event (settings + 0 = 80 km/h)
        block[0x0F] = 100;                                  // laps
        career::SetRaceEventName(block, event.name);        // 0x8005E458
        career::SetRaceCourse(block, Rd<uint32_t>(sel, Sel::kCourseId), d.courses); // 0x8005E4A0
        StrCpy(block, 0x44, event.tag);
        block[0x5A] = 1;
        Wr<int16_t>(block, 0x582, -1);
        Wr<int16_t>(block, 0x584, -1);
        Wr<int16_t>(block, 0x586, 0);
        block[0x580] = 0;
        Wr<uint32_t>(block, 0x588, (Rd<uint32_t>(block, 0x588) & ~7u) | (Rd<uint32_t>(sel, Sel::kWord2CC) != 1 ? 1u : 0u));
        // 0x8005E674: the course's record of the career (career + 0x218 + course index * 0x24); its +0x14 = the record car,
        // whose name goes to + 0x53C (the ghost's name; "" = 0x800267B8 without a record).
        const uint32_t courseIndex = career::CourseIndexOfId(d.courses, Rd<uint32_t>(block, 0x40)); // 0x80060DC4
        const size_t recordAt = 0x218 + size_t(courseIndex) * 0x24;
        if (career.size() < recordAt + 0x18) throw std::invalid_argument("arcade setup: the time trial build reads the career's course record");
        const uint32_t recordCar = Rd<uint32_t>(career, recordAt + 0x14);
        StrCpy(block, 0x53C, d.menu.ImageString(kEmptyString));
        if (recordCar != 0) StrCpy(block, 0x53C, CarInfoOf(d.cars, recordCar).rawName); // 0x800609F8
        if (fromGarage) { // no entry here: ovl3 makes it (RebuildGarageEntry); the local configuration = the garage car's
            CarConfig local;
            std::memcpy(&local, garageCar.data() + 8, sizeof(local));
            FinishBuild(d, region, sel, block, local, p1, p2, {garageCar, {}}, true);
            return out;
        }
        // 0x80010A34(block, 0, 0, 3, transmission, car, 0x8001003C(car, colour), spec): the player's entry and record.
        const uint32_t car = Rd<uint32_t>(sel, Sel::kCar);
        const std::span<const uint8_t> spec = PlayerSpec(d.data, car, int8_t(sel[Sel::kTyres]));
        const CarInfoRecord& info = CarInfoOf(d.cars, car);
        const int16_t colour = Rd<int16_t>(sel, Sel::kColour);
        if (colour < 0 || size_t(colour) >= info.paintIds.size()) throw std::logic_error("arcade setup: colour index beyond the car's paints");
        const size_t slot = 0x5C;
        block[slot + 0x8C] = 1;
        block[slot + 0x8D] = 0;
        block[slot + 0x8E] = 3;
        Wr<uint32_t>(block, slot, car);
        block[slot + 0x8F] = sel[Sel::kTransmission];
        Wr<uint32_t>(block, slot + 4, uint32_t(uint8_t(info.paintIds[size_t(colour)])));
        StrCpy(block, slot + 0x90, info.rawName);
        CarConfig config = ConfigFromCarSpec(d.data.Tables(), spec, false); // 0x80076ED0
        out.records.resize(1);
        career::BuildMenuRecord(d.data.Tables(), config, out.records[0]); // 0x800770BC -> 0x801DE31A
        std::memcpy(block.data() + slot + 8, &config, sizeof(config));
        const CarConfig local = ConfigFromCarSpec(d.data.Tables(), spec, false);
        FinishBuild(d, region, sel, block, local, p1, p2, {}, false);
        return out;
    }

    // 0x80010C84, mode 4: the event of the level and class, the player's entry.
    const int8_t level = int8_t(sel[Sel::kLevel]), cls = int8_t(sel[Sel::kClass]);
    if (level < 0 || level > 3 || cls < 0 || cls > 3) throw std::logic_error("arcade setup: level / class outside the event name table");
    const int32_t eventRow = d.data.FindEvent(d.menu.eventNames[size_t(level)][size_t(cls)]);
    if (eventRow < 0) throw std::logic_error("arcade setup: no event " + d.menu.eventNames[size_t(level)][size_t(cls)] + " (the original reads a null row)");
    const RaceEvent event = d.data.EventAt(size_t(eventRow));
    Entry player;
    std::span<const uint8_t> spec;
    if (!fromGarage) {
        const uint32_t playerCar = Rd<uint32_t>(sel, Sel::kCar);
        const CarInfoRecord& playerInfo = CarInfoOf(d.cars, playerCar);
        const int16_t colour = Rd<int16_t>(sel, Sel::kColour);
        if (colour < 0 || size_t(colour) >= playerInfo.paintIds.size()) throw std::logic_error("arcade setup: colour index beyond the car's paints");
        player.carId = playerCar;
        player.paint = char(playerInfo.paintIds[size_t(colour)]); // 0x8001003C
        player.transmission = int8_t(sel[Sel::kTransmission]);
        spec = PlayerSpec(d.data, playerCar, int8_t(sel[Sel::kTyres]));
    }

    // 0x80010554(0, 0, event, spec, &player, 0, 0); a garage car: 0x80010554(0, 0, event, 0, 0, 0, 0) - six drawn opponents.
    std::fill(block.begin(), block.end(), uint8_t(0));
    for (size_t k = 0; k < 8; k++) block[k] = career[1 + k];
    block[0x08] = 2;
    block[0x09] = 2;
    block[0x0A] = 4;
    block[0x0B] = 5;
    block[0x0C] = 0;
    block[0x0E] = 0;
    block[0x0D] = event.settings[0] == 0 ? 1 : 0;
    block[0x0F] = event.settings[1];
    career::SetRaceEventName(block, event.name);                                   // 0x8005E458
    career::SetRaceCourse(block, CourseFileId(event.course), d.courses);           // 0x8005E500: by the course's name
    StrCpy(block, 0x44, event.tag);                                                // strcpy(block + 0x44, tag)
    Wr<int16_t>(block, 0x57C, -1);
    Wr<int16_t>(block, 0x582, -1);
    Wr<int16_t>(block, 0x584, -1);
    Wr<int16_t>(block, 0x586, 0);
    block[0x580] = 0;
    Wr<uint32_t>(block, 0x588, (Rd<uint32_t>(block, 0x588) | 1u) & ~6u);
    const size_t count = 6;
    block[0x5A] = uint8_t(count);
    uint32_t seed = vsync;
    std::vector<Entry> entries;
    if (!fromGarage) {
        entries.push_back(player);
        entries[0].opponent = 0; // the copy's +8 is cleared
    }
    for (size_t i = 0; i < count; i++) {
        const size_t slot = 0x5C + i * 0xD0;
        const uint8_t kind = (i == 0 && !fromGarage) ? 3 : 1;
        uint32_t code = 0;
        if (i >= entries.size()) {
            Entry e;
            code = PickOpponent(e, d, event, language, seed, entries);
            e.transmission = 0;
            entries.push_back(e);
        }
        Entry& e = entries[i];
        Wr<uint32_t>(block, slot, e.carId);
        Wr<int32_t>(block, slot + 4, int32_t(int8_t(e.paint)));
        block[slot + 0x8D] = uint8_t(count - (i + 1));
        block[slot + 0x8C] = 1;
        CarConfig config;
        if (kind == 3) {
            config = ConfigFromCarSpec(d.data.Tables(), spec, false); // 0x80076ED0
        } else {
            config = d.data.OpponentConfig(e.opponent);             // 0x800767D0 + 0x80076E6C
            const std::span<const uint8_t> rm = d.data.Tables().Row(kTableRacingModify, config.racingModify); // 0x80076E3C(5, +0x24)
            if (rm[0x0E] != 0) {
                const uint32_t body = Rd<uint32_t>(rm, 8);
                Wr<uint32_t>(block, slot, body);
                RepaintForModel(e, body, code, d, seed);
                Wr<int32_t>(block, slot + 4, int32_t(int8_t(e.paint)));
            }
        }
        std::memcpy(block.data() + slot + 8, &config, sizeof(config));
        block[slot + 0x8E] = kind;
        block[slot + 0x8F] = uint8_t(e.transmission);
        StrCpy(block, slot + 0x90, CarInfoOf(d.cars, Rd<uint32_t>(block, slot)).rawName); // 0x800609F8
    }
    // The settings block of the event the block names (0x8007821C(tables, block + 0x10)), then every entry's record (0x800770BC).
    const int32_t namedRow = d.data.FindEvent(CString(block, 0x10));
    if (namedRow < 0) throw std::logic_error("arcade setup: the race block's event is not in table 30");
    const RaceEvent named = d.data.EventAt(size_t(namedRow));
    std::copy(named.settings.begin(), named.settings.end(), out.settings.begin());
    out.records.resize(count);
    for (size_t i = 0; i < count; i++) {
        CarConfig config = ConfigOfSlot(block, 0x5C + i * 0xD0);
        career::BuildMenuRecord(d.data.Tables(), config, out.records[i]);
        std::memcpy(block.data() + 0x5C + i * 0xD0 + 8, &config, sizeof(config));
    }

    // Back in 0x80010C84: the player's configuration, the mode's bytes, laps, course, record number, level byte.
    CarConfig local;
    if (fromGarage) std::memcpy(&local, garageCar.data() + 8, sizeof(local));
    else local = ConfigFromCarSpec(d.data.Tables(), spec, false);
    block[0x09] = 0;
    block[0x0A] = sel[Sel::kMode];
    block[0x5A] = 6;
    block[0x0F] = sel[Sel::kLaps];
    career::SetRaceCourse(block, Rd<uint32_t>(sel, Sel::kCourseId), d.courses); // 0x8005E4A0
    block[0x57E] = sel[Sel::kCourseRecord];
    block[0x57F] = uint8_t(d.menu.levelBlockByte[size_t(level)]);
    FinishBuild(d, region, sel, block, local, p1, p2, {garageCar, {}}, !garageCar.empty());
    return out;
}

namespace {

// 0x80011BE8(garage, index, rally): the car's tune sheet, the rally tyre rule, the sheet stored back into the garage car.
void TuneGarageCar(career::GarageCar& car, uint32_t word2cc, const career::CareerData& gt) {
    auto sheet = std::make_unique<career::TuneSheet>();
    career::LoadCarSheet(*sheet, car, gt.tables);
    if (word2cc != 0) {
        career::SetTuneStage(*sheet, career::kTuneTyresFront, 7, gt);
        career::SetTuneStage(*sheet, career::kTuneTyresRear, 7, gt);
    } else if (sheet->stage[career::kTuneTyresFront] == 7 || sheet->stage[career::kTuneTyresRear] == 7) {
        career::SetTuneStage(*sheet, career::kTuneTyresFront, 0, gt);
        career::SetTuneStage(*sheet, career::kTuneTyresRear, 0, gt);
    }
    std::array<uint8_t, 0x230> scratch{};
    career::StoreCarSheet(car, *sheet, gt.tables, career::BuildScratch{scratch.data()}); // 0x80011B0C
}

// 0x800127AC(block, entry, grid, kind, transmission, car, garage, index) -> record `entry` (0x801DE31A + entry * 0x1C0).
void GarageEntry(ArcadeRaceSetup& setup, size_t entry, int32_t grid, uint8_t kind, uint8_t transmission, const career::GarageCar& car, int16_t garage,
                 int16_t index, const career::CareerData& gt) {
    const std::span<uint8_t> block(setup.raceBlock);
    const size_t slot = 0x5C + entry * 0xD0;
    if (grid >= 0) {
        std::fill(block.begin() + std::ptrdiff_t(slot), block.begin() + std::ptrdiff_t(slot + 0xD0), uint8_t(0));
        block[slot + 0x8C] = 1;
        block[slot + 0x8D] = uint8_t(grid);
    }
    block[slot + 0x8E] = kind;
    block[slot + 0x8F] = transmission;
    Wr<uint32_t>(block, slot, car.modelId);
    Wr<uint32_t>(block, slot + 4, car.paint);
    CarConfig config = car.config;
    uint8_t* const cfg = reinterpret_cast<uint8_t*>(&config);
    cfg[0x7A] = uint8_t(cfg[0x7A] | 0x40); // the GT-mode flag
    StrCpy(block, slot + 0x90, CarInfoOf(gt.cars, car.modelId).rawName);
    Wr<int16_t>(block, 0x582, garage);
    Wr<int16_t>(block, 0x584, index);
    if (setup.records.size() <= entry) setup.records.resize(entry + 1);
    career::BuildMenuRecord(gt.tables, config, setup.records[entry]); // 0x800770BC with the GT tables
    std::memcpy(block.data() + slot + 8, &config, sizeof(config));
}

career::GarageCar& GarageCarAt(std::span<uint8_t> garageBlock, int16_t index) {
    if (garageBlock.size() < kGarageCarsAt + 100 * kGarageCarBytes || index < 0 || index >= 100) throw std::invalid_argument("arcade setup: garage block / index");
    return *reinterpret_cast<career::GarageCar*>(garageBlock.data() + kGarageCarsAt + size_t(index) * kGarageCarBytes);
}

} // namespace

void RebuildGarageEntries(ArcadeRaceSetup& setup, std::array<std::span<uint8_t>, 2> garages, const career::CareerData& gt) { // ovl3 0x8001290C
    const std::span<uint8_t> region(setup.menuRegion);
    const std::span<uint8_t> sel = region.subspan(kSelectionAddress - kMenuRegionAddress, kSelectionSize);
    if (Rd<uint32_t>(sel, Sel::kGarageFlag) == 0) return;
    if (int8_t(sel[Sel::kMode]) != 0) {
        const int16_t g = Rd<int16_t>(sel, Sel::kGarage);
        if (g >= 0 && g < 2) RebuildGarageEntry(setup, garages[size_t(g)], gt);
        return;
    }
    for (size_t i = 0; i < 2; i++) { // 0x8001298C..0x80012A28
        const int16_t g = Rd<int16_t>(sel, Sel2P::kGarage + 2 * i);
        if (g < 0) continue;
        if (g > 1) throw std::invalid_argument("arcade setup: garage number");
        const int16_t index = Rd<int16_t>(sel, Sel2P::kGarageSlot + 2 * i);
        career::GarageCar& car = GarageCarAt(garages[size_t(g)], index);
        TuneGarageCar(car, Rd<uint32_t>(sel, Sel::kWord2CC), gt);
        GarageEntry(setup, i, int32_t(i), uint8_t(i + 3), sel[Sel2P::kTransmission + i], car, -1, -1, gt);
    }
}

void RebuildGarageEntry(ArcadeRaceSetup& setup, std::span<uint8_t> garageBlock, const career::CareerData& gt) { // ovl3 0x80012C00 -> 0x8001290C
    const std::span<uint8_t> region(setup.menuRegion);
    const std::span<uint8_t> sel = region.subspan(kSelectionAddress - kMenuRegionAddress, kSelectionSize);
    if (Rd<uint32_t>(sel, Sel::kGarageFlag) == 0) return;
    const int8_t mode = int8_t(sel[Sel::kMode]);
    if (mode == 0) throw std::invalid_argument("arcade setup: the 2 player Battle garage rebuild takes both garages (RebuildGarageEntries)");
    if (mode != 4 && mode != 6) return;
    const int16_t g = Rd<int16_t>(sel, Sel::kGarage), index = Rd<int16_t>(sel, Sel::kGarageSlot);
    if (g < 0) return;
    if (garageBlock.size() < kGarageCarsAt + 100 * kGarageCarBytes || index < 0 || index >= 100) throw std::invalid_argument("arcade setup: garage block / index");
    career::GarageCar& car = *reinterpret_cast<career::GarageCar*>(garageBlock.data() + kGarageCarsAt + size_t(index) * kGarageCarBytes);
    // 0x80011BE8(garage, index, rally)
    auto sheet = std::make_unique<career::TuneSheet>();
    career::LoadCarSheet(*sheet, car, gt.tables);
    if (Rd<uint32_t>(sel, Sel::kWord2CC) != 0) {
        career::SetTuneStage(*sheet, career::kTuneTyresFront, 7, gt);
        career::SetTuneStage(*sheet, career::kTuneTyresRear, 7, gt);
    } else if (sheet->stage[career::kTuneTyresFront] == 7 || sheet->stage[career::kTuneTyresRear] == 7) {
        career::SetTuneStage(*sheet, career::kTuneTyresFront, 0, gt);
        career::SetTuneStage(*sheet, career::kTuneTyresRear, 0, gt);
    }
    std::array<uint8_t, 0x230> scratch{};
    career::StoreCarSheet(car, *sheet, gt.tables, career::BuildScratch{scratch.data()}); // 0x80011B0C
    // 0x800127AC(block, 0, grid, 3, transmission, car, garage, index)
    const std::span<uint8_t> block(setup.raceBlock);
    const size_t slot = 0x5C;
    if (mode == 6) {
        std::fill(block.begin() + std::ptrdiff_t(slot), block.begin() + std::ptrdiff_t(slot + 0xD0), uint8_t(0));
        block[slot + 0x8C] = 1;
        block[slot + 0x8D] = 0;
    }
    block[slot + 0x8E] = 3;
    block[slot + 0x8F] = sel[Sel::kTransmission];
    Wr<uint32_t>(block, slot, car.modelId);
    Wr<uint32_t>(block, slot + 4, car.paint);
    CarConfig config = car.config;
    uint8_t* const cfg = reinterpret_cast<uint8_t*>(&config);
    cfg[0x7A] = uint8_t(cfg[0x7A] | 0x40); // the GT-mode flag
    StrCpy(block, slot + 0x90, CarInfoOf(gt.cars, car.modelId).rawName);
    Wr<int16_t>(block, 0x582, g);
    Wr<int16_t>(block, 0x584, index);
    if (setup.records.empty()) setup.records.resize(1);
    career::BuildMenuRecord(gt.tables, config, setup.records[0]); // 0x800770BC with the GT tables
    std::memcpy(block.data() + slot + 8, &config, sizeof(config));
}

bool TierOpen(std::span<const uint8_t> career, int32_t tier) { // 0x8002357C
    if (pc::unlockCourses) return true;
    if (tier < 0) return true;
    bool open = true;
    const size_t base = 0x1418 + size_t(tier) * 0x668;
    for (size_t k = 0; k < 10; k++)
        if (Rd<uint8_t>(career, base + k * 0xA4 + 1) == 0) open = false;
    return open;
}

std::vector<uint8_t> CourseAvailability(const std::vector<ArcadeCourse>& list, bool reverse, std::span<const uint8_t> career) { // 0x8001D120
    std::vector<uint8_t> flags;
    for (const ArcadeCourse& c : list) {
        if (pc::unlockCourses) { flags.push_back(1); continue; }
        uint8_t f = 0;
        if (TierOpen(career, int16_t(uint16_t(c.tier)))) {
            f = 1;
            if (c.record != -1 && reverse) f = uint8_t((Rd<uint8_t>(career, 0xB8 + size_t(c.record)) >> 2) & 1);
        }
        flags.push_back(f);
    }
    return flags;
}

ClassUnlocks ComputeClassUnlocks(const ArcadeMenuData& menu, std::span<const uint8_t> career) { // 0x8001D418
    ClassUnlocks u;
    if (pc::unlockCars) {
        u.classSOpen = true;
        u.classS.assign(menu.classes[0].cars.size(), 1);
        u.bonus.assign(menu.classes[6].cars.size(), 1);
        return u;
    }
    auto won = [&](int8_t record) { const uint8_t b = Rd<uint8_t>(career, 0xB8 + size_t(uint8_t(record))); return ((b >> 1) & 1) != 0 || ((b >> 2) & 1) != 0; };
    for (size_t i = 0; i < menu.classes[0].cars.size(); i++) {
        const int8_t r = menu.classSUnlock.at(i);
        uint8_t f = 1;
        if (r >= 0) {
            f = won(r) ? 1 : 0;
            if (f) u.classSOpen = true;
        }
        u.classS.push_back(f);
    }
    for (size_t i = 0; i < menu.classes[6].cars.size(); i++) {
        const int8_t r = menu.bonusUnlock.at(i);
        u.bonus.push_back(r < 0 || won(r) ? 1 : 0);
    }
    return u;
}

} // namespace gt2::arcade
