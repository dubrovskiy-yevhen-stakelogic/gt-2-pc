#include "game/career/garage.h"
#include "game/career/tuning.h"

#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "game/sim/car_body.h"
#include "game/sim/fixed.h"
#include "gt2formats/car_info.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2::career {

namespace {

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }
void Put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }

uint8_t* Bytes(CarConfig& c) { return reinterpret_cast<uint8_t*>(&c); }
uint8_t* Bytes(TuneSheet& s) { return reinterpret_cast<uint8_t*>(&s); }

// 0x80075E90(a, b, 0): (a << 12) / b in 64 bits, low word.
int32_t Div12(int32_t a, int32_t b) { return int32_t(uint64_t(sim::Div64(int64_t(a) * 4096, int64_t(b)))); }

// The catalogue configuration as 0x80076954 builds it (row 0 when the car is not in table 30).
CarConfig CatalogueConfigOrRow0(const CarParamTables& t, uint32_t carId) {
    const std::optional<CarConfig> c = CatalogueCarConfig(t, carId);
    if (c) return *c;
    return ConfigFromCarSpec(t, t.Row(kCarCatalogueTable, 0), true);
}

// Copies row `index` of `table` into the sheet at `at` (the row's size) and stores the index at `indexAt`.
void PutRow(TuneSheet& s, size_t at, size_t indexAt, const CarParamTables& t, size_t table, uint32_t index) {
    const std::span<const uint8_t> row = t.Row(table, index);
    if (at + row.size() > sizeof(TuneSheet) || indexAt + 4 > sizeof(TuneSheet)) throw std::out_of_range("tune sheet: slot outside the object");
    std::memcpy(Bytes(s) + at, row.data(), row.size());
    Put32(Bytes(s) + indexAt, index);
}
void PutBytes(TuneSheet& s, size_t at, std::span<const uint8_t> bytes, size_t count) {
    if (at + count > sizeof(TuneSheet) || count > bytes.size()) throw std::out_of_range("tune sheet: slot outside the object");
    std::memcpy(Bytes(s) + at, bytes.data(), count);
}

// One part kind of the sheet: the car's own row goes to the slot named by its stage byte, the upgrades of the part
// kinds firstKind.. to the slots 1..upgrades (0x80015428).
struct SheetGroup {
    size_t table;
    size_t configOffset;
    size_t stageOffset;
    size_t rowsAt, rowSize, indicesAt;
    int32_t firstKind, upgrades;
};

void LoadGroup(TuneSheet& s, const SheetGroup& g, const CarConfig& stock, uint32_t carId, const CarParamTables& t) {
    const uint16_t own = Get16(reinterpret_cast<const uint8_t*>(&stock) + g.configOffset);
    const uint8_t stage = t.Row(g.table, own)[g.stageOffset];
    PutRow(s, g.rowsAt + stage * g.rowSize, g.indicesAt + stage * 4u, t, g.table, own);
    for (int32_t k = 0; k < g.upgrades; k++) {
        const int32_t row = PartRow(t, g.firstKind + k, carId);
        if (row >= 0) PutRow(s, g.rowsAt + size_t(k + 1) * g.rowSize, g.indicesAt + size_t(k + 1) * 4u, t, g.table, uint32_t(row));
    }
}

} // namespace

// ---------------------------------------------------------------- data

CareerData CareerData::Load(const DiscImage& disc, const GtfsVolume& vol) {
    CareerData d{CarParamTables::Load(vol), CarInfoDirectory::Load(vol), GtModeRaceData::Load(vol), {}, LoadExeImage(disc), UiLayout(LoadOverlayImage(disc, 4))};
    const GuestImage& ovl4 = d.ovl4;
    for (uint32_t a = 0x80050B68u;; a += 2) { // s16 list ended by a negative value
        const int16_t k = ovl4.Get<int16_t>(a);
        if (k < 0) break;
        d.raceCarKinds.push_back(k);
        if (d.raceCarKinds.size() > 64) throw std::runtime_error("career data: race car kind list not terminated");
    }
    return d;
}

// ---------------------------------------------------------------- money and slots

void AddMoney(GarageBlock& g, int32_t amount) { // 0x8005E7B0
    g.money = int32_t(uint32_t(g.money) + uint32_t(amount));
    if (g.money > kMoneyLimit) g.money = kMoneyLimit;
    if (g.money < 0) g.money = 0;
}

int32_t CanBuy(const GarageBlock& g, int32_t price) { // 0x80017914
    if (g.money < price) return -1;
    if (g.count == kGarageCapacity) return -2;
    return 1;
}

int32_t AddPreparedCar(GarageBlock& g, const GarageCar& car) { // 0x8005E7F0
    if (g.count > 99) return 0;
    std::memcpy(&g.cars[g.count], &car, sizeof(GarageCar));
    g.count = int16_t(g.count + 1);
    return 1;
}

void RemoveCar(GarageBlock& g, int32_t index) { // 0x8001EDAC
    if (g.currentCar == index) g.currentCar = -1;
    if (index < g.currentCar) g.currentCar = int16_t(g.currentCar - 1);
    for (int32_t i = index; i < g.count - 1; i++) std::memmove(&g.cars[i], &g.cars[i + 1], sizeof(GarageCar));
    g.count = int16_t(g.count - 1);
}

void MoveCar(GarageBlock& g, int32_t from, int32_t to) { // 0x8001EF10
    GarageCar moving;
    std::memcpy(&moving, &g.cars[from], sizeof(GarageCar));
    if (to < from) {
        for (int32_t i = from - 1; i >= to; i--) std::memcpy(&g.cars[i + 1], &g.cars[i], sizeof(GarageCar));
    } else if (from < to) {
        for (int32_t i = from; i < to; i++) std::memcpy(&g.cars[i], &g.cars[i + 1], sizeof(GarageCar));
    }
    std::memcpy(&g.cars[to], &moving, sizeof(GarageCar));
    const int32_t current = g.currentCar;
    if (current >= 0) {
        if (current < from && to <= current) g.currentCar = int16_t(g.currentCar + 1);
        else if (current == from) g.currentCar = int16_t(to);
    }
}

void SelectCar(GarageBlock& g, int16_t index) { g.currentCar = index; } // ovl4 0x8001DFEC case 6 / 0x8001DDAC

// ---------------------------------------------------------------- parts owned

bool PartOwned(const GarageCar& car, int32_t kind) { // 0x8005E874
    return (car.partsOwned[kind >> 3] & (1u << ((kind - ((kind < 0 ? kind + 7 : kind) >> 3) * 8) & 31))) != 0;
}

void SetPartBit(uint8_t* bits, int32_t kind) { // 0x8005E900
    bits[kind >> 3] = uint8_t(bits[kind >> 3] | (1u << ((kind - ((kind < 0 ? kind + 7 : kind) >> 3) * 8) & 31)));
}

void AddPart(GarageCar& car, int32_t kind, int32_t price) { // 0x8005E8B0
    SetPartBit(car.partsOwned, kind);
    car.value = int32_t(uint32_t(car.value) + uint32_t(price));
}

// ---------------------------------------------------------------- table lookups

int32_t FirstRowOfCar(const CarParamTables& t, size_t table, uint32_t carId) {
    // 0x80077E80: lo = -1, hi = count; probe (lo + hi) >> 1 (unsigned compares of the ids) until lo + 1 == hi.
    const uint32_t count = uint32_t(t.RowCount(table));
    uint32_t lo = 0xFFFFFFFFu, hi = count;
    int32_t probe = int32_t(count) - 1;
    int32_t found = -1;
    do {
        const uint32_t mid = uint32_t(probe >> 1);
        const uint32_t id = Get32(t.Row(table, mid).data());
        if (id == carId) {
            found = int32_t(mid);
            break;
        }
        uint32_t next = mid;
        if (id < carId) {
            next = hi;
            lo = mid;
        }
        probe = int32_t(lo + next);
        hi = next;
    } while (lo + 1 != hi);
    if (found < 0) return -1;
    // 0x80076818: back to the first row of the car.
    int32_t row = found;
    while (row >= 1 && Get32(t.Row(table, uint32_t(row - 1)).data()) == carId) row--;
    return row;
}

int32_t RowWithStage(const CarParamTables& t, size_t table, uint8_t stage, size_t stageOffset, uint32_t carId) { // 0x80076748
    int32_t row = FirstRowOfCar(t, table, carId);
    if (row == -1) return -1;
    for (; size_t(row) < t.RowCount(table); row++) {
        const std::span<const uint8_t> r = t.Row(table, uint32_t(row));
        if (Get32(r.data()) != carId) break;
        if (r[stageOffset] == stage && r[4] != 0) return row;
    }
    return -1;
}

int32_t PartRow(const CarParamTables& t, int32_t kind, uint32_t carId, uint32_t* price) { // 0x80076570
    size_t table = 0, stageOffset = 8;
    int32_t stage = 1;
    switch (kind) {
    case 0: table = 0x1B; break;
    case 1: table = 0; break;
    case 2: table = 1; break;
    case 3: case 4: case 5: table = 0x0F; stage = kind - 2; break;
    case 6: table = 10; break;
    case 7: case 8: table = 9; stage = kind - 6; break;
    case 9: table = 8; break;
    case 10: case 11: case 12: table = 0x0E; stage = kind - 9; break;
    case 13: case 14: case 15: table = 0x11; stage = kind - 12; break;
    case 16: case 17: table = 0x13; stage = kind - 15; break;
    case 0x12: case 0x13: case 0x14: table = 4; stage = kind - 0x11; stageOffset = 0x0B; break;
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: table = 0x15; stage = kind - 0x14; break;
    case 0x1A: case 0x1B: case 0x1C: table = 0x14; stage = kind - 0x19; break;
    case 0x1D: case 0x1E: case 0x1F: table = 0x0B; stage = kind - 0x1C; break;
    case 0x20: table = 7; break;
    case 0x21: table = 0x10; break;
    case 0x22: table = 5; stage = 1; stageOffset = 0x0E; break;
    case 0x23: case 0x24: case 0x25: table = 0x12; stage = kind - 0x22; break;
    case 0x26: table = 0x1C; break;
    case 0x27: case 0x28: case 0x29: case 0x2A: case 0x2B: case 0x2C: case 0x2D: table = 0x16; stage = kind - 0x26; break;
    case 0x2E: case 0x2F: case 0x30: case 0x31: table = 0x0C; stage = kind - 0x2D; break;
    default: // the original then searches "table -1" (outside the file); no such part here
        if (price) *price = 0;
        return -1;
    }
    int32_t row = RowWithStage(t, table, uint8_t(stage), stageOffset, carId);
    if (row == -1) row = RowWithStage(t, table, uint8_t(stage), stageOffset, kNoPartId); // "00000" (0x80060924)
    if (price) *price = row == -1 ? 0 : Get32(t.Row(table, uint32_t(row)).data() + 4);
    return row;
}

int32_t RacingModifyStageRow(const CarParamTables& t, uint32_t carId, uint8_t stage, uint32_t* price) { // 0x80076500
    const int32_t row = RowWithStage(t, kTableRacingModify, stage, 0x0E, carId);
    if (price) *price = row == -1 ? 0 : Get32(t.Row(kTableRacingModify, uint32_t(row)).data() + 4);
    return row;
}

int32_t CataloguePrice(const CarParamTables& t, uint32_t carId) { // 0x800177D4
    const std::optional<size_t> row = FindCatalogueRow(t, carId);
    return row ? int32_t(Get32(t.Row(kCarCatalogueTable, *row).data() + 0x44)) : 0;
}

int32_t PartPrice(const CarParamTables& t, uint32_t carId, int32_t kind) { // 0x80017B04
    uint32_t price = 0;
    return PartRow(t, kind, carId, &price) < 0 ? -1 : int32_t(price);
}

void ApplyPartRowSettings(const GuestImage& exe, size_t table, std::span<const uint8_t> row, CarConfig& config) { // 0x80076240
    uint32_t map = 0;
    switch (table) {
    case 1: map = 0x800928CCu; break;
    case 5: map = 0x800928D8u; break;
    case 0x0C: map = 0x800928E4u; break;
    case 0x0D: map = 0x80092888u; break;
    case 0x11: map = 0x800928A0u; break;
    case 0x12:
        map = 0x80092900u;
        config.toeCode[0] = 0x80;
        config.toeCode[1] = 0x80;
        break;
    case 0x15: map = 0x8009294Cu; break;
    case 0x1B: map = 0x80092890u; break;
    case 0x1C: map = 0x80092898u; break;
    default: return;
    }
    // 0x80077634: u32 entries until 0: bits 0..9 destination (config), 10..19 source (row), 20..23 size code, 24..31
    // operation. Only operation 4 (copy of size x count bytes, 0x8008DFC4) occurs in these tables.
    static constexpr uint8_t kElement[9] = {1, 2, 4, 1, 1, 1, 1, 2, 2}, kCount[9] = {1, 1, 1, 4, 6, 8, 16, 8, 16};
    for (uint32_t a = exe.Sim(map);; a += 4) { // the table of the executable's build (gt2formats/exe_profile.h)
        const uint32_t e = exe.Get<uint32_t>(a);
        if (e == 0) break;
        const uint32_t dst = e & 0x3FF, src = (e >> 10) & 0x3FF, code = (e >> 20) & 0xF, op = e >> 24;
        if (code > 8) throw std::logic_error("mapping table: unknown size code");
        const size_t n = size_t(kElement[code]) * kCount[code];
        if (op != 4) throw std::logic_error("mapping table: operation " + std::to_string(op) + " not ported (0x80077634)");
        if (src + n > row.size() || dst + n > sizeof(CarConfig)) throw std::out_of_range("mapping table: copy outside the row / configuration");
        std::memmove(Bytes(config) + dst, row.data() + src, n);
    }
}

// ---------------------------------------------------------------- tune sheet

void ClearTuneSheet(TuneSheet& s) { std::memset(&s, 0xFF, sizeof(s)); } // 0x80015404

void LoadTuneSheet(TuneSheet& s, uint32_t carId, const CarParamTables& t) { // 0x80015428
    const CarConfig stock = CatalogueConfigOrRow0(t, carId);
    const uint8_t* cfg = reinterpret_cast<const uint8_t*>(&stock);
    LoadGroup(s, {kTableSuspension, 0x12, 8, 0x988, 0x4C, 0xAB8, 0x23, 3}, stock, carId, t);
    LoadGroup(s, {kTableBrakes, 0x04, 8, 0xAC8, 0x0C, 0xAE0, 1, 1}, stock, carId, t);
    LoadGroup(s, {kTableBrakeController, 0x06, 8, 0xAE8, 0x10, 0xB08, 2, 1}, stock, carId, t);
    { // front tyres: the own row in slots 0, 8 and 9 with its size / compound / grip rows
        const uint16_t own = Get16(cfg + 0x16);
        const std::span<const uint8_t> r = t.Row(kTableTyresFront, own);
        for (size_t slot : {size_t(0), size_t(8), size_t(9)}) PutRow(s, 0xB10 + slot * 0x10, 0xBB0 + slot * 4, t, kTableTyresFront, own);
        const std::span<const uint8_t> size = t.Row(24, Get16(r.data() + 0x0A));
        for (size_t slot : {size_t(0), size_t(8), size_t(9)}) PutBytes(s, 0xBD8 + slot * 4, size, 4);
        PutBytes(s, 0xC00, t.Row(25, Get16(r.data() + 0x0C)), 0x40);
        const std::span<const uint8_t> grip = t.Row(26, Get16(r.data() + 0x0E));
        for (size_t slot : {size_t(0), size_t(8), size_t(9)}) PutBytes(s, 0xE80 + slot * 8, grip, 8);
        for (int32_t k = 0; k < 7; k++) {
            const int32_t row = PartRow(t, 0x27 + k, carId);
            if (row < 0) continue;
            const size_t slot = size_t(k + 1);
            PutRow(s, 0xB10 + slot * 0x10, 0xBB0 + slot * 4, t, kTableTyresFront, uint32_t(row));
            const std::span<const uint8_t> u = t.Row(kTableTyresFront, uint32_t(row));
            PutBytes(s, 0xBD8 + slot * 4, t.Row(24, Get16(u.data() + 0x0A)), 4);
            PutBytes(s, 0xC00 + slot * 0x40, t.Row(25, Get16(u.data() + 0x0C)), 0x40);
            PutBytes(s, 0xE80 + slot * 8, t.Row(26, Get16(u.data() + 0x0E)), 8);
        }
    }
    { // rear tyres: slots 0, 8, 9; the upgrades use the FRONT table's row index for table 23 as well
        const uint16_t own = Get16(cfg + 0x18);
        const std::span<const uint8_t> r = t.Row(kTableTyresRear, own);
        for (size_t slot : {size_t(0), size_t(8), size_t(9)}) PutRow(s, 0xED0 + slot * 0x0C, 0xF48 + slot * 4, t, kTableTyresRear, own);
        const std::span<const uint8_t> size = t.Row(24, Get16(r.data() + 6));
        for (size_t slot : {size_t(0), size_t(8), size_t(9)}) PutBytes(s, 0xF70 + slot * 4, size, 4);
        const std::span<const uint8_t> compound = t.Row(25, Get16(r.data() + 8));
        for (size_t slot : {size_t(0), size_t(8), size_t(9)}) PutBytes(s, 0xF98 + slot * 0x40, compound, 0x40);
        for (int32_t k = 0; k < 7; k++) {
            const int32_t row = PartRow(t, 0x27 + k, carId);
            if (row < 0) continue;
            const size_t slot = size_t(k + 1);
            PutRow(s, 0xED0 + slot * 0x0C, 0xF48 + slot * 4, t, kTableTyresRear, uint32_t(row));
            const std::span<const uint8_t> u = t.Row(kTableTyresRear, uint32_t(row));
            PutBytes(s, 0xF70 + slot * 4, t.Row(24, Get16(u.data() + 6)), 4);
            PutBytes(s, 0xF98 + slot * 0x40, t.Row(25, Get16(u.data() + 8)), 0x40);
        }
    }
    LoadGroup(s, {kTableGearbox, 0x10, 8, 0x1218, 0x24, 0x12A8, 0x0D, 3}, stock, carId, t);
    LoadGroup(s, {kTableClutch, 0x2C, 8, 0x12B8, 0x10, 0x12F8, 3, 3}, stock, carId, t);
    LoadGroup(s, {kTableFlywheel, 0x2A, 8, 0x1308, 0x0C, 0x1338, 10, 3}, stock, carId, t);
    LoadGroup(s, {kTablePropellerShaft, 0x2E, 8, 0x1348, 0x0C, 0x1360, 0x21, 1}, stock, carId, t);
    LoadGroup(s, {kTableComputer, 0x24, 8, 0x1368, 0x0C, 0x1380, 6, 1}, stock, carId, t);
    LoadGroup(s, {kTablePortPolish, 0x1E, 8, 0x1388, 0x0C, 0x13A0, 0x20, 1}, stock, carId, t);
    LoadGroup(s, {kTableEngineBalance, 0x20, 8, 0x13A8, 0x0C, 0x13C0, 9, 1}, stock, carId, t);
    LoadGroup(s, {kTableNaTune, 0x26, 8, 0x13C8, 0x0C, 0x13F8, 0x1D, 3}, stock, carId, t);
    LoadGroup(s, {kTableDisplacement, 0x22, 8, 0x1408, 0x0C, 0x1420, 7, 1}, stock, carId, t);
    LoadGroup(s, {kTableTurboKit, 0x28, 8, 0x1428, 0x14, 0x148C, 0x2E, 4}, stock, carId, t);
    LoadGroup(s, {kTableIntercooler, 0x32, 8, 0x14A0, 0x0C, 0x14C4, 0x10, 2}, stock, carId, t);
    LoadGroup(s, {kTableMuffler, 0x30, 8, 0x14D0, 0x0C, 0x1500, 0x1A, 3}, stock, carId, t);
    LoadGroup(s, {kTableLightweight, 0x1A, 0x0B, 0x1510, 0x0C, 0x1540, 0x12, 3}, stock, carId, t);
    { // racing modification: the own row at its stage (+0E), stages 1..4 from 0x80076500
        const uint16_t own = Get16(cfg + 0x1C);
        const uint8_t stage = t.Row(kTableRacingModify, own)[0x0E];
        PutRow(s, 0x1550 + stage * 0x1Cu, 0x15DC + stage * 4u, t, kTableRacingModify, own);
        for (uint32_t u = 1; u <= 4; u++) {
            const int32_t row = RacingModifyStageRow(t, carId, uint8_t(u));
            if (row >= 0) PutRow(s, 0x1550 + u * 0x1C, 0x15DC + u * 4, t, kTableRacingModify, uint32_t(row));
        }
    }
    LoadGroup(s, {kTableAsm, 0x34, 8, 0x15F0, 0x10, 0x1610, 0, 1}, stock, carId, t);
    LoadGroup(s, {kTableTcs, 0x36, 8, 0x1618, 0x10, 0x1638, 0x26, 1}, stock, carId, t);
    LoadGroup(s, {kTableLsd, 0x14, 8, 0x1640, 0x20, 0x1700, 0x15, 5}, stock, carId, t);
    PutRow(s, 0x1718, 0x1730, t, kTableSteering, Get16(cfg + 0x08));
    PutRow(s, 0x1734, 0x1748, t, kTableChassis, Get16(cfg + 0x0A));
    PutRow(s, 0x174C, 0x1798, t, kTableEngine, Get16(cfg + 0x0C));
    PutRow(s, 0x179C, 0x17AC, t, kTableDrivetrain, Get16(cfg + 0x0E));
}

void SetTuneConfig(TuneSheet& s, const CarConfig& config, const CarParamTables& t) { // 0x80016C5C
    std::memcpy(&s.config, &config, sizeof(CarConfig));
    const uint8_t* c = reinterpret_cast<const uint8_t*>(&config);
    auto stageOf = [&](size_t table, size_t configOffset, size_t byte) { return int16_t(t.Row(table, Get16(c + configOffset))[byte]); };
    s.stage[kTuneSuspension] = stageOf(kTableSuspension, 0x12, 8);
    s.stage[kTuneBrakes] = stageOf(kTableBrakes, 0x04, 8);
    s.stage[kTuneBrakeController] = stageOf(kTableBrakeController, 0x06, 8);
    s.stage[kTuneTyresFront] = stageOf(kTableTyresFront, 0x16, 8);
    s.stage[kTuneTyresRear] = stageOf(kTableTyresRear, 0x18, 4);
    s.stage[kTuneGearbox] = stageOf(kTableGearbox, 0x10, 8);
    s.stage[kTuneClutch] = stageOf(kTableClutch, 0x2C, 8);
    s.stage[kTuneFlywheel] = stageOf(kTableFlywheel, 0x2A, 8);
    s.stage[kTunePropellerShaft] = stageOf(kTablePropellerShaft, 0x2E, 8);
    s.stage[kTuneComputer] = stageOf(kTableComputer, 0x24, 8);
    s.stage[kTunePortPolish] = stageOf(kTablePortPolish, 0x1E, 8);
    s.stage[kTuneEngineBalance] = stageOf(kTableEngineBalance, 0x20, 8);
    s.stage[kTuneNaTune] = stageOf(kTableNaTune, 0x26, 8);
    s.stage[kTuneDisplacement] = stageOf(kTableDisplacement, 0x22, 8);
    s.stage[kTuneTurbo] = stageOf(kTableTurboKit, 0x28, 8);
    s.stage[kTuneIntercooler] = stageOf(kTableIntercooler, 0x32, 8);
    s.stage[kTuneMuffler] = stageOf(kTableMuffler, 0x30, 8);
    s.stage[kTuneLightweight] = stageOf(kTableLightweight, 0x1A, 0x0B);
    s.stage[kTuneRacingModify] = stageOf(kTableRacingModify, 0x1C, 0x0E);
    s.stage[kTuneAsm] = stageOf(kTableAsm, 0x34, 8);
    s.stage[kTuneTcs] = stageOf(kTableTcs, 0x36, 8);
    s.stage[kTuneLsd] = stageOf(kTableLsd, 0x14, 8);
    const int16_t steering = stageOf(kTableSteering, 0x08, 8);
    s.stage[kTuneChassis] = 0;
    s.stage[kTuneEngine] = 0;
    s.stage[kTuneSteering] = steering;
    s.stage[kTuneDrivetrain] = stageOf(kTableDrivetrain, 0x0E, 4);
}

void SetTuneStage(TuneSheet& s, int32_t kind, int32_t stage, const CareerData& d) { // 0x8005EAC0
    uint8_t* b = Bytes(s);
    uint8_t* c = Bytes(s.config);
    s.stage[kind] = int16_t(stage);
    auto index = [&](size_t at) { return Get16(b + at + size_t(stage) * 4); }; // low half of the row index word
    auto rowAt = [&](size_t at, size_t size) { return std::span<const uint8_t>(b + at + size_t(stage) * size, size); };
    switch (kind) {
    case 0:
        Put16(c + 0x12, index(0xAB8));
        ApplyPartRowSettings(d.exe, 0x12, rowAt(0x988, 0x4C), s.config);
        break;
    case 1: Put16(c + 0x04, index(0xAE0)); break;
    case 2:
        Put16(c + 0x06, index(0xB08));
        ApplyPartRowSettings(d.exe, 1, rowAt(0xAE8, 0x10), s.config);
        break;
    case 3: Put16(c + 0x16, index(0xBB0)); break;
    case 4: Put16(c + 0x18, index(0xF48)); break;
    case 5: Put16(c + 0x38, index(0x688)); break;
    case 6:
        Put16(c + 0x10, index(0x12A8));
        ApplyPartRowSettings(d.exe, 0x11, rowAt(0x1218, 0x24), s.config);
        if (b[0x1238 + size_t(stage) * 0x24] != 0) { // 0x8005EAA4: gearAutoSet of the selected gearbox row
            // 0x8005E93C into an 8-entry stack buffer, all eight copied: reverse..top gear are generated from the
            // sheet's record; the entries above the top gear are the stack's residue in the original (left by
            // earlier, deeper calls of the menus; not reproducible, and read by nothing but the save / replay bytes:
            // docs/research/menus_gtmode.md section 8). Here they keep the values the gearbox row brought.
            int16_t ratios[8];
            std::memcpy(ratios, c + 0x3C, sizeof(ratios));
            SheetGearRatios(s, d, ratios);
            std::memcpy(c + 0x3C, ratios, sizeof(ratios));
            c[0x7B] = c[0x4E];
            Put16(c + 0x7C, Get16(c + 0x4C));
            Put16(c + 0x7E, uint16_t(s.stage[kTuneTyresFront]));
            Put16(c + 0x80, uint16_t(s.stage[kTuneTyresRear]));
        }
        break;
    case 7: Put16(c + 0x2C, index(0x12F8)); break;
    case 8: Put16(c + 0x2A, index(0x1338)); break;
    case 9: Put16(c + 0x2E, index(0x1360)); break;
    case 10: Put16(c + 0x24, index(0x1380)); break;
    case 11: Put16(c + 0x1E, index(0x13A0)); break;
    case 12: Put16(c + 0x20, index(0x13C0)); break;
    case 13: // NA tune: the turbo and the intercooler go back to stage 0
        Put16(c + 0x28, Get16(b + 0x148C));
        ApplyPartRowSettings(d.exe, 0x0C, std::span<const uint8_t>(b + 0x1428, 0x14), s.config);
        s.stage[kTuneTurbo] = 0;
        s.stage[kTuneIntercooler] = 0;
        Put16(c + 0x32, Get16(b + 0x14C4));
        Put16(c + 0x26, index(0x13F8));
        break;
    case 14: Put16(c + 0x22, index(0x1420)); break;
    case 15: // turbo: the NA tune goes back to stage 0
        s.stage[kTuneNaTune] = 0;
        Put16(c + 0x26, Get16(b + 0x13F8));
        Put16(c + 0x28, index(0x148C));
        ApplyPartRowSettings(d.exe, 0x0C, rowAt(0x1428, 0x14), s.config);
        break;
    case 16: Put16(c + 0x32, index(0x14C4)); break;
    case 17: Put16(c + 0x30, index(0x1500)); break;
    case 18: Put16(c + 0x1A, index(0x1540)); break;
    case 19:
        Put16(c + 0x1C, index(0x15DC));
        ApplyPartRowSettings(d.exe, 5, rowAt(0x1550, 0x1C), s.config);
        break;
    case 20:
        Put16(c + 0x34, index(0x1610));
        ApplyPartRowSettings(d.exe, 0x1B, rowAt(0x15F0, 0x10), s.config);
        break;
    case 21:
        Put16(c + 0x36, index(0x1638));
        ApplyPartRowSettings(d.exe, 0x1C, rowAt(0x1618, 0x10), s.config);
        break;
    case 22:
        Put16(c + 0x14, index(0x1700));
        ApplyPartRowSettings(d.exe, 0x15, rowAt(0x1640, 0x20), s.config);
        break;
    default: break;
    }
}

void FinishTuneConfig(TuneSheet& s, CarConfig& config, const CareerData& d) { // 0x80016FEC
    SetTuneStage(s, kTuneGearbox, s.stage[kTuneGearbox], d);
    s.config.flags = uint8_t(s.config.flags | 0xC0);
    std::memcpy(&config, &s.config, sizeof(CarConfig));
}

bool AnalysePurchase(TuneSheet& s, uint32_t carId, CarConfig& config, const CareerData& d) { // 0x80017750
    ClearTuneSheet(s);
    LoadTuneSheet(s, carId, d.tables);
    SetTuneConfig(s, config, d.tables);
    FinishTuneConfig(s, config, d);
    return s.stage[kTuneRacingModify] > 0; // *(s16*)0x801DBC8E
}

// ---------------------------------------------------------------- building a garage car

uint16_t CarPowerFigures(sim::CarParams& record, uint8_t* out) { // 0x80075930
    // 0x80075328(record, block, 0): the engine block of the race setup, here in a scratch body.
    auto body = std::make_unique<sim::CarBody>();
    std::memset(body.get(), 0, sizeof(sim::CarBody));
    sim::SetupEngine(*body, record, 0);
    const sim::EngineBlock& e = *reinterpret_cast<const sim::EngineBlock*>(reinterpret_cast<const uint8_t*>(body.get()) + sim::kEngineBlockOffset);
    // 0x800756BC(block, out)
    auto put16 = [&](size_t at, int32_t v) { Put16(out + at, uint16_t(v)); };
    auto get16s = [&](size_t at) { return int32_t(int16_t(Get16(out + at))); };
    int32_t n = 0;
    for (int32_t i = 0; i < int16_t(e.count); i++) {
        int32_t v = int32_t(uint32_t(e.xs[i]) * 60u); // rev/s << 12 -> rpm << 12 (32-bit wrap like the original's shifts)
        if (v < 0) v += 4095;
        int32_t rpm = v >> 12;
        rpm = ((rpm + 50) / 100) * 100; // 0x51EB851F: signed division by 100
        if (n <= 0 || get16s(0x0C + (n - 1) * 2) < rpm) {
            put16(0x0C + n * 2, rpm);
            n++;
        }
    }
    put16(0x0A, n);
    put16(0x08, e.revLimitRpm);
    int32_t top = e.revLimitRpm;
    const int32_t lastListed = get16s(0x0C + (n - 1) * 2);
    if (top < lastListed) top = lastListed;
    int32_t maxPower = 0, maxTorque = 0, rpmAtTorque = 0, rpmAtPower = 0;
    size_t cursor = 0;
    for (int32_t rpm = get16s(0x0C); rpm <= top; rpm += 100) {
        const int32_t speed = (rpm << 12) / 60; // 0x88888889: signed division by 60
        const int32_t torque = sim::Interpolate(e.xs, e.ys, e.count, speed);
        const int32_t torqueKgm10 = int32_t(uint32_t(torque) * 10u) / 39; // 0xD20D20D3: signed division by 39
        const int32_t power = Div12(sim::Mul12Wide(Div12(torque, 0x9CCD), rpm), 2864);
        if (get16s(0x0C + cursor * 2) == rpm) {
            put16(0x2C + cursor * 2, power);
            put16(0x4C + cursor * 2, torqueKgm10);
            cursor++;
        }
        if (!(int32_t(Get16(out + 8)) < rpm)) {
            if (maxTorque < torqueKgm10) {
                maxTorque = torqueKgm10;
                rpmAtTorque = rpm;
            }
            if (maxPower < power) {
                maxPower = power;
                rpmAtPower = rpm;
            }
        }
    }
    put16(0x00, maxPower);
    put16(0x04, maxTorque);
    put16(0x06, rpmAtTorque);
    put16(0x02, rpmAtPower);
    return Get16(out);
}

void BuildMenuRecord(const CarParamTables& t, CarConfig& config, sim::CarParams& record) {
    // BuildCarParams = 0x800771AC + 0x80017E74 (the race start's model dimensions); undo the latter: the lengths
    // stay 0 and the tracks are the racing-modification row's.
    record = BuildCarParams(t, config, CarBodyDimensions{});
    const RacingModifyRow& rm = t.RowAs<RacingModifyRow>(kTableRacingModify, config.racingModify);
    record.frontLength = 0;
    record.rearLength = 0;
    record.frontTrack = rm.frontTrack;
    record.rearTrack = rm.rearTrack;
}

bool GearboxFlag(const CarParamTables& t, uint32_t carId) { // 0x800178E4
    const CarConfig c = CatalogueConfigOrRow0(t, carId);
    return t.Row(kTableGearbox, c.gearbox)[9] < 3;
}

bool FittedParts(const CareerData& d, uint32_t carId, std::array<uint8_t, 7>& bits) { // 0x8001781C
    const std::optional<size_t> row = FindCatalogueRow(d.tables, carId);
    if (!row) throw std::runtime_error("0x8001781C: car not in the catalogue (the original reads through a null row)");
    if (d.tables.Row(kCarCatalogueTable, *row)[0x40] == 0) return false;
    bits.fill(0);
    for (int16_t kind : d.raceCarKinds)
        if (PartRow(d.tables, kind, carId) >= 0) SetPartBit(bits.data(), kind);
    return true;
}

void BuildGarageCar(GarageCar& slot, uint32_t carId, uint32_t modelId, uint32_t paint, CarConfig config, int32_t value, bool flag15, bool flag14,
                    const uint8_t* parts, const CarParamTables& t, BuildScratch scratch) { // 0x8001EC0C
    sim::CarParams record;
    BuildMenuRecord(t, config, record); // 0x800771AC(config, 0x1F800000): the config gets the write-backs
    std::memcpy(scratch.bytes, &record, sizeof(record));
    sim::CarParams& onScratch = *reinterpret_cast<sim::CarParams*>(scratch.bytes);
    CarPowerFigures(onScratch, scratch.bytes + 0x1C0);
    slot.carId = carId;
    slot.modelId = modelId;
    slot.paint = paint;
    std::memcpy(&slot.config, &config, sizeof(CarConfig));
    slot.value = value;
    std::memset(slot.partsOwned, 0, sizeof(slot.partsOwned));
    if (parts) std::memcpy(slot.partsOwned, parts, sizeof(slot.partsOwned));
    const uint16_t power = Get16(scratch.bytes + 0x1C0), torque = Get16(scratch.bytes + 0x1C4);
    const uint16_t weight = Get16(scratch.bytes + 0x5A);
    const uint8_t drive = scratch.bytes[0x8A];
    slot.powerFlags = uint16_t((slot.powerFlags & 0xC000) | power);
    slot.torqueFigure = torque;
    slot.wordA2 = 0;
    slot.weightDrive = uint16_t((weight & 0x1FFF) | (drive << 13));
    slot.powerFlags = uint16_t((slot.powerFlags & 0x3FFF) | (flag15 ? 0x8000 : 0) | (flag14 ? 0x4000 : 0));
}

bool AppendGarageCar(GarageBlock& g, uint32_t carId, uint32_t paint, const CarConfig& config, int32_t value, bool flag15, bool flag14, const uint8_t* parts,
                     const CarParamTables& t, BuildScratch scratch) { // 0x8001EE78
    const int32_t count = g.count;
    if (count >= kGarageCapacity) return false;
    BuildGarageCar(g.cars[count], carId, carId, paint, config, value, flag15, flag14, parts, t, scratch);
    g.count = int16_t(g.count + 1);
    return true;
}

// ---------------------------------------------------------------- menu actions

int32_t BuyCar(GarageBlock& g, uint32_t carId, uint32_t paint, int32_t price, const CareerData& d, TuneSheet& sheet, BuildScratch scratch) { // 0x8001796C
    CarConfig config = CatalogueConfigOrRow0(d.tables, carId);
    const bool flag15 = AnalysePurchase(sheet, carId, config, d);
    if (g.money < price) return -1;
    const bool flag14 = GearboxFlag(d.tables, carId);
    std::array<uint8_t, 7> fitted{};
    const bool haveParts = FittedParts(d, carId, fitted);
    if (!AppendGarageCar(g, carId, paint, config, price, flag15, flag14, haveParts ? fitted.data() : nullptr, d.tables, scratch)) return -2;
    g.money = int32_t(uint32_t(g.money) - uint32_t(price));
    return 1;
}

void SellCar(GarageBlock& g, int32_t index, const CarParamTables& t) { // 0x80017A70
    const int32_t price = CataloguePrice(t, g.cars[index].carId);
    RemoveCar(g, index);
    AddMoney(g, price / 4); // `if (v < 0) v += 3; v >> 2`
}

int32_t PartPurchaseCheck(const GarageBlock& g, int32_t index, int32_t kind, const CarParamTables& t) { // 0x80017B40
    const GarageCar& car = g.cars[index];
    const int32_t price = PartPrice(t, car.carId, kind);
    if (PartRow(t, kind, car.carId) < 0) return -8;
    if (!(price <= g.money)) return -1;
    if (PartOwned(car, kind)) return -3;
    int32_t prerequisite = 0, code = 0;
    if (kind == 0x14) { prerequisite = 0x13; code = -6; }
    else if (kind == 0x13) { prerequisite = 0x12; code = -5; }
    else if (kind == 0x22) { prerequisite = 0x14; code = -7; }
    else return 1;
    return PartOwned(car, prerequisite) ? 1 : code;
}

int32_t BuyPart(GarageBlock& g, int32_t index, int32_t kind, const CarParamTables& t) { // 0x80017C98
    const int32_t price = PartPrice(t, g.cars[index].carId, kind);
    const int32_t check = PartPurchaseCheck(g, index, kind, t);
    if (check == 1) {
        AddPart(g.cars[index], kind, price);
        g.money = int32_t(uint32_t(g.money) - uint32_t(price));
    }
    return check;
}

} // namespace gt2::career
