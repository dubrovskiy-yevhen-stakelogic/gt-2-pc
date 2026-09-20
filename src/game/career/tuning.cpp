#include "game/career/tuning.h"

#include <cstring>
#include <span>
#include <memory>
#include <stdexcept>
#include <string>

namespace gt2::career {

namespace {

uint16_t Get16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
uint32_t Get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void Put16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

const uint8_t* Bytes(const TuneSheet& s) { return reinterpret_cast<const uint8_t*>(&s); }
uint8_t* Bytes(TuneSheet& s) { return reinterpret_cast<uint8_t*>(&s); }
uint8_t* Cfg(TuneSheet& s) { return reinterpret_cast<uint8_t*>(&s.config); }
const uint8_t* Cfg(const TuneSheet& s) { return reinterpret_cast<const uint8_t*>(&s.config); }
uint8_t* Bytes(CarConfig& c) { return reinterpret_cast<uint8_t*>(&c); }

// The slot row of `kind` at its current stage (0x8005F410's pointers).
const uint8_t* SlotRow(const TuneSheet& s, size_t rowsAt, size_t rowSize, int32_t stage) {
    const size_t at = rowsAt + size_t(int64_t(stage) * int64_t(rowSize));
    if (stage < 0 || at + rowSize > sizeof(TuneSheet)) throw std::logic_error("tune sheet: stage outside the slots");
    return Bytes(s) + at;
}
const uint8_t* Suspension(const TuneSheet& s) { return SlotRow(s, 0x988, 0x4C, s.stage[kTuneSuspension]); }

// One kind of the sheet: configuration field, index words of the slots, stage index.
struct SheetKind {
    size_t configOffset, indicesAt;
    int kind;
    int32_t slots;
};
constexpr SheetKind kSheetKinds[] = {
    {0x12, 0xAB8, kTuneSuspension, 4}, {0x04, 0xAE0, kTuneBrakes, 2}, {0x06, 0xB08, kTuneBrakeController, 2}, {0x10, 0x12A8, kTuneGearbox, 4},
    {0x2C, 0x12F8, kTuneClutch, 4}, {0x2A, 0x1338, kTuneFlywheel, 4}, {0x2E, 0x1360, kTunePropellerShaft, 2}, {0x24, 0x1380, kTuneComputer, 2},
    {0x1E, 0x13A0, kTunePortPolish, 2}, {0x20, 0x13C0, kTuneEngineBalance, 2}, {0x26, 0x13F8, kTuneNaTune, 4}, {0x22, 0x1420, kTuneDisplacement, 2},
    {0x28, 0x148C, kTuneTurbo, 5}, {0x32, 0x14C4, kTuneIntercooler, 3}, {0x30, 0x1500, kTuneMuffler, 4}, {0x1A, 0x1540, kTuneLightweight, 4},
    {0x1C, 0x15DC, kTuneRacingModify, 5}, {0x34, 0x1610, kTuneAsm, 2}, {0x36, 0x1638, kTuneTcs, 2}, {0x14, 0x1700, kTuneLsd, 6},
    {0x08, 0x1730, kTuneSteering, 1}, {0x0A, 0x1748, kTuneChassis, 1}, {0x0C, 0x1798, kTuneEngine, 1}, {0x0E, 0x17AC, kTuneDrivetrain, 1},
};

uint16_t SlotIndex(const TuneSheet& s, size_t indicesAt, int32_t stage, int32_t slots) {
    const size_t at = indicesAt + size_t(int64_t(stage) * 4);
    if (stage < 0 || stage >= slots) throw std::logic_error("tune sheet: stage " + std::to_string(stage) + " outside the kind's slots (the original reads the next field)");
    const uint32_t word = Get32(Bytes(s) + at);
    if (word == 0xFFFFFFFFu) throw std::logic_error("tune sheet: the selected slot is empty (the original reads the 0xFF fill of 0x80015404)");
    return uint16_t(word);
}

sim::SetupConstants GearConstants(const CareerData& d) {
    sim::SetupConstants c;
    c.gearAutoTable = d.exe.At(d.exe.Sim(0x800923E2u), 6 * 8);
    return c;
}

// Writes the figures of the sheet into a garage slot the way 0x80016F10 / 0x80056FF0 do (weight / drive / torque /
// power bits 0..13 with the power's high bits ORed into the flags like the original's `& 0xC000 | power`).
void PutFigures(GarageCar& car, const CarFigures& f) {
    car.powerFlags = uint16_t((car.powerFlags & 0xC000) | f.power);
    car.torqueFigure = f.torque;
    car.weightDrive = uint16_t((car.weightDrive & 0xE000) | f.weight);
    car.weightDrive = uint16_t((f.weight & 0x1FFF) | uint16_t(f.drive << 13));
}

} // namespace

// ---------------------------------------------------------------- lookups

int32_t SearchRowById(const CarParamTables& t, size_t table, uint32_t id) { // 0x80077E80
    const uint32_t count = uint32_t(t.RowCount(table));
    uint32_t lo = 0xFFFFFFFFu, hi = count;
    int32_t probe = int32_t(count) - 1;
    do {
        const uint32_t mid = uint32_t(probe >> 1);
        const uint32_t rowId = Get32(t.Row(table, mid).data());
        if (rowId == id) return int32_t(mid);
        uint32_t next = mid;
        if (rowId < id) {
            next = hi;
            lo = mid;
        }
        probe = int32_t(lo + next);
        hi = next;
    } while (lo + 1 != hi);
    return -1;
}

// ---------------------------------------------------------------- record and figures of a sheet

CarConfig SheetRowsConfig(const TuneSheet& s, int32_t frontTyreStage, int32_t rearTyreStage) {
    CarConfig c = s.config;
    uint8_t* b = Bytes(c);
    for (const SheetKind& k : kSheetKinds) {
        const int32_t stage = s.stage[k.kind];
        if (k.kind >= kTuneSteering && stage != 0)
            throw std::logic_error("tune sheet: stage " + std::to_string(stage) + " of the single-row kind " + std::to_string(k.kind) + " (0x8005F410 reads past the slot)");
        Put16(b + k.configOffset, SlotIndex(s, k.indicesAt, stage, k.slots));
    }
    Put16(b + 0x16, SlotIndex(s, 0xBB0, frontTyreStage, 10));
    Put16(b + 0x18, SlotIndex(s, 0xF48, rearTyreStage, 10));
    return c;
}

sim::CarParams SheetRecord(const TuneSheet& s, const CarParamTables& t) { // 0x8005F410 + 0x80077214
    CarConfig c = SheetRowsConfig(s, s.stage[kTuneTyresFront], s.stage[kTuneTyresRear]);
    sim::CarParams record;
    BuildMenuRecord(t, c, record); // the write-backs go to the copy (0x80077214 writes them into its working struct)
    return record;
}

CarFigures SheetFigures(const TuneSheet& s, const CarParamTables& t, BuildScratch scratch) { // 0x8005F958
    const sim::CarParams record = SheetRecord(s, t);
    std::memcpy(scratch.bytes, &record, sizeof(record)); // 0x80077214(work, 0x1F800000)
    CarPowerFigures(*reinterpret_cast<sim::CarParams*>(scratch.bytes), scratch.bytes + 0x1C0); // 0x80075930(0x1F800000, 0x1F8001C0)
    CarFigures f;
    f.power = Get16(scratch.bytes + 0x1C0);
    f.torque = Get16(scratch.bytes + 0x1C4);
    f.weight = Get16(scratch.bytes + 0x5A);
    f.drive = scratch.bytes[0x8A];
    return f;
}

int32_t SheetGearRatios(const TuneSheet& s, const CareerData& d, int16_t out[8]) { // 0x8005E93C
    sim::CarParams record = SheetRecord(s, d.tables);
    record.gearAutoSet = 0;
    sim::GenerateGearRatios(record, int32_t(record.gearAutoFinal) * 10, d.menuDirtCourse, GearConstants(d)); // 0x80074B38
    const int32_t gears = record.gearCount;
    for (int32_t gear = 0; gear <= gears && gear < 8; gear++) out[gear] = record.gearRatio[gear];
    return gears;
}

void GearRanges(int32_t count, const int16_t* r, int16_t* lo, int16_t* hi) { // 0x80074E04
    auto eighths = [](int32_t v, int32_t k) { v *= k; if (v < 0) v += 7; return int16_t(v >> 3); };
    auto third = [](int32_t v) { return int16_t(int32_t((int64_t(v) * 0x55555556LL) >> 32) - (v >> 31)); };
    hi[1] = eighths(r[1], 9);
    for (int32_t i = 1; i < count; i++) {
        lo[i] = third(r[i] * 2 + r[i + 1]);
        hi[i + 1] = third(r[i + 1] * 2 + r[i]);
    }
    lo[count] = eighths(r[count], 7);
    lo[0] = eighths(r[0], 7);
    hi[0] = eighths(r[0], 9);
}

int32_t SheetGearRanges(const TuneSheet& s, const CareerData& d, int16_t out[8], int16_t lo[8], int16_t hi[8]) { // 0x8005E99C
    const uint8_t* c = Cfg(s);
    CarConfig rows = SheetRowsConfig(s, Get16(c + 0x7E), Get16(c + 0x80));
    rows.finalDrive = int16_t(Get16(c + 0x7C));
    rows.gearAutoFinal = c[0x7B];
    sim::CarParams record;
    BuildMenuRecord(d.tables, rows, record);
    record.gearAutoSet = 0;
    sim::GenerateGearRatios(record, int32_t(record.gearAutoFinal) * 10, d.menuDirtCourse, GearConstants(d)); // 0x80074B38
    const int32_t gears = record.gearCount;
    if (gears > 7) throw std::logic_error("0x8005E99C: more than 7 gears (the original writes past its buffers)");
    for (int32_t gear = 0; gear <= gears; gear++) out[gear] = record.gearRatio[gear];
    GearRanges(gears, out, lo, hi); // 0x80074E04(record gear count, out, lo, hi)
    return gears;
}

// ---------------------------------------------------------------- garage car <-> sheet

void LoadCarSheet(TuneSheet& s, const GarageCar& car, const CarParamTables& t) { // 0x800173E8
    ClearTuneSheet(s);               // 0x80015404
    LoadTuneSheet(s, car.carId, t);  // 0x80015428
    SetTuneConfig(s, car.config, t); // 0x80016C5C
}

void StoreCarSheet(GarageCar& car, const TuneSheet& s, const CarParamTables& t, BuildScratch scratch) { // 0x80016F10
    std::memcpy(&car.config, &s.config, sizeof(CarConfig));
    PutFigures(car, SheetFigures(s, t, scratch));
    car.powerFlags = uint16_t((car.powerFlags & 0x7FFF) | (s.stage[kTuneRacingModify] > 0 ? 0x8000 : 0));
}

TuneStage PartTuneStage(int32_t partKind) { // 0x8001706C
    int16_t base = 0, kind = 0;
    switch (partKind) {
    case 0: base = 0; kind = kTuneAsm; break;
    case 1: base = 1; kind = kTuneBrakes; break;
    case 2: base = 2; kind = kTuneBrakeController; break;
    case 3: case 4: case 5: base = 3; kind = kTuneClutch; break;
    case 6: base = 6; kind = kTuneComputer; break;
    case 7: case 8: base = 7; kind = kTuneDisplacement; break;
    case 9: base = 9; kind = kTuneEngineBalance; break;
    case 10: case 11: case 12: base = 10; kind = kTuneFlywheel; break;
    case 13: case 14: case 15: base = 13; kind = kTuneGearbox; break;
    case 16: case 17: base = 16; kind = kTuneIntercooler; break;
    case 0x12: case 0x13: case 0x14: base = 0x12; kind = kTuneLightweight; break;
    case 0x15: case 0x16: case 0x17: case 0x18: case 0x19: base = 0x15; kind = kTuneLsd; break;
    case 0x1A: case 0x1B: case 0x1C: base = 0x1A; kind = kTuneMuffler; break;
    case 0x1D: case 0x1E: case 0x1F: base = 0x1D; kind = kTuneNaTune; break;
    case 0x20: base = 0x20; kind = kTunePortPolish; break;
    case 0x21: base = 0x21; kind = kTunePropellerShaft; break;
    case 0x22: base = 0x22; kind = kTuneRacingModify; break;
    case 0x23: case 0x24: case 0x25: base = 0x23; kind = kTuneSuspension; break;
    case 0x26: base = 0x26; kind = kTuneTcs; break;
    case 0x2E: case 0x2F: case 0x30: case 0x31: base = 0x2E; kind = kTuneTurbo; break;
    default: break; // 0x27..0x2D (tyres) and others: kind 0 (suspension), stage partKind + 1
    }
    TuneStage r;
    r.kind = kind;
    r.stage = int16_t(int16_t(partKind) - base + 1);
    return r;
}

int32_t PartKindOfStage(int32_t tuneKind, int32_t stage) {
    if (stage < 1) return -1;
    if (tuneKind == kTuneTyresFront || tuneKind == kTuneTyresRear) return stage <= 7 ? 0x26 + stage : -1;
    for (int32_t part = 0; part < 0x32; part++) {
        if (part >= 0x27 && part <= 0x2D) continue;
        const TuneStage t = PartTuneStage(part);
        if (t.kind == tuneKind && t.stage == stage) return part;
    }
    return -1;
}

int32_t RacingBodyCount(const TuneSheet& s) { // 0x8005F858
    int32_t n = 0;
    for (int32_t k = 1; k < 5; k++) {
        if (int32_t(s.racingModifyRow[k]) < 0) return n;
        n++;
    }
    return n;
}

uint32_t FirstRacingBody(const TuneSheet& s, int16_t& racingBody) { // 0x800174F4
    racingBody = 1;
    return s.racingModify[1].modelId;
}

uint32_t NextRacingBody(const TuneSheet& s, int16_t& racingBody) { // 0x80017530
    const int32_t count = RacingBodyCount(s);
    racingBody = int16_t(racingBody + 1);
    if (count < racingBody) racingBody = 1;
    return Get32(Bytes(s) + 0x1558 + size_t(racingBody) * 0x1C);
}

int32_t FitPart(GarageBlock& g, int32_t index, int32_t partKind, uint32_t paint, TuneSheet& sheet, int16_t racingBody, const CareerData& d,
                BuildScratch scratch) { // 0x80017D6C
    GarageCar& car = g.cars[index];
    if (!PartOwned(car, partKind)) return -4;
    if (PartRow(d.tables, partKind, car.carId) < 0) return -8;
    if (partKind == 0x22) {
        SetTuneStage(sheet, kTuneRacingModify, racingBody, d);
        const uint32_t model = Get32(Bytes(sheet) + 0x1558 + size_t(int64_t(racingBody) * 0x1C));
        car.paint = paint;
        car.modelId = model;
    } else if (partKind < 0x22 || partKind > 0x2D || partKind < 0x27) {
        const TuneStage t = PartTuneStage(partKind);
        SetTuneStage(sheet, t.kind, t.stage, d);
    } else { // tyres 0x27..0x2D: front and rear
        SetTuneStage(sheet, kTuneTyresFront, partKind - 0x26, d);
        SetTuneStage(sheet, kTuneTyresRear, partKind - 0x26, d);
    }
    StoreCarSheet(car, sheet, d.tables, scratch);
    return 1;
}

int32_t FitPartToCar(GarageBlock& g, int32_t index, int32_t partKind, uint32_t paint, int16_t racingBody, const CareerData& d, BuildScratch scratch) {
    auto sheet = std::make_unique<TuneSheet>();
    LoadCarSheet(*sheet, g.cars[index], d.tables);
    return FitPart(g, index, partKind, paint, *sheet, racingBody, d, scratch);
}

void PrepareRaceTyres(GarageBlock& g, int32_t index, bool dirtEvent, TuneSheet& sheet, const CareerData& d, BuildScratch scratch) { // 0x80018004
    LoadCarSheet(sheet, g.cars[index], d.tables);
    if (dirtEvent && PartOwned(g.cars[index], 0x2D)) {
        FitPart(g, index, 0x2D, 0, sheet, 0, d, scratch); // 0x80017D6C(index, 45, 0, 0): paint / body unused for tyres
        return;
    }
    if (sheet.stage[kTuneTyresFront] == 7 || sheet.stage[kTuneTyresRear] == 7) {
        SetTuneStage(sheet, kTuneTyresFront, 0, d);
        SetTuneStage(sheet, kTuneTyresRear, 0, d);
        StoreCarSheet(g.cars[index], sheet, d.tables, scratch);
    }
}

void SetWheelRow(TuneSheet& s, int32_t row, const CarParamTables& t) { // 0x80021B38
    const std::span<const uint8_t> r = t.Row(kCarProfileTable, uint32_t(row)); // 0x80077D5C(tables, 29, row)
    const uint32_t word = Get32(r.data());
    uint32_t colour = r[7] & 0x1Fu;
    if (word == 0 && t.Row(kTableRacingModify, s.config.racingModify)[0x0E] != 0) colour = r[6] & 0x1Fu;
    s.config.word00 = word | colour << 8;
    s.config.word38 = uint16_t(row);
}

int32_t BuyWheels(GarageBlock& g, int32_t index, uint32_t wheelId, int32_t price, TuneSheet& sheet, const CareerData& d, BuildScratch scratch) { // 0x80018100
    if (g.money < price) return -1; // 0x800181D0
    const int32_t row = SearchRowById(d.tables, kCarProfileTable, wheelId); // 0x80021B0C
    if (row < 0) throw std::logic_error("0x80018100: wheel id not in table 29 (the original indexes the table with -1)");
    SetWheelRow(sheet, row, d.tables);
    StoreCarSheet(g.cars[index], sheet, d.tables, scratch);
    g.money = int32_t(uint32_t(g.money) - uint32_t(price));
    return 1;
}

int32_t ChangePartStage(GarageBlock& g, int32_t index, int32_t tuneKind, int32_t stage, const CareerData& d, BuildScratch scratch) {
    GarageCar& car = g.cars[index];
    if (stage > 0) {
        const int32_t part = PartKindOfStage(tuneKind, stage);
        if (part < 0 || !PartOwned(car, part)) return -4;
    }
    auto sheet = std::make_unique<TuneSheet>();
    LoadCarSheet(*sheet, car, d.tables);
    const size_t indices[] = {0xAB8, 0xAE0, 0xB08, 0xBB0, 0xF48, 0x688, 0x12A8, 0x12F8, 0x1338, 0x1360, 0x1380, 0x13A0, 0x13C0, 0x13F8,
                              0x1420, 0x148C, 0x14C4, 0x1500, 0x1540, 0x15DC, 0x1610, 0x1638, 0x1700};
    if (tuneKind < 0 || tuneKind > kTuneLsd || tuneKind == kTuneProfile) return -8;
    if (Get32(Bytes(*sheet) + indices[tuneKind] + size_t(stage) * 4) == 0xFFFFFFFFu) return -8;
    SetTuneStage(*sheet, tuneKind, stage, d);
    if (tuneKind == kTuneTyresFront) SetTuneStage(*sheet, kTuneTyresRear, stage, d);
    StoreCarSheet(car, *sheet, d.tables, scratch);
    return 1;
}

int32_t RemovePart(GarageBlock& g, int32_t index, int32_t partKind, const CareerData& d, BuildScratch scratch) {
    const int32_t kind = (partKind >= 0x27 && partKind <= 0x2D) ? int32_t(kTuneTyresFront) : int32_t(PartTuneStage(partKind).kind);
    return ChangePartStage(g, index, kind, 0, d, scratch);
}

uint32_t WheelIdOfCode(const CareerData& d, const std::string& code) { // 0x80013A28
    if (code.size() < 8) throw std::invalid_argument("wheel code shorter than 8 characters");
    auto c = [&](size_t i) { return int32_t(int8_t(code[i])); };
    uint32_t maker = 0;
    for (uint32_t a = 0x80050904u;; a += 2, maker++) { // u16 pairs, 0-terminated
        const uint32_t pair = d.ovl4.Get<uint16_t>(a);
        if (pair == 0 || int32_t(pair) == (c(0) | c(1) << 8)) break;
        if (maker > 64) throw std::runtime_error("wheel maker list (ovl4 0x80050904) not terminated");
    }
    uint32_t kind = 0;
    if (c(6) == '5') kind = 2;
    else if (c(6) == '4') kind = 1;
    else if (c(6) == '6') kind = 3;
    const int32_t number = (c(2) - 0x30) * 100 + (c(3) - 0x30) * 10 - 0x30 + c(4);
    return ((uint32_t(int32_t(maker << 12) | number) << 3 | kind) << 13) | uint32_t(c(7));
}

uint8_t WheelColour(const CareerData& d, uint32_t wheelId, bool racingModified) { // 0x80021BEC
    const int32_t row = SearchRowById(d.tables, kCarProfileTable, wheelId); // 0x80021B0C
    if (row < 0) throw std::logic_error("0x80021BEC: wheel id not in table 29 (the original reads row -1)");
    const std::span<const uint8_t> r = d.tables.Row(kCarProfileTable, uint32_t(row));
    return racingModified ? r[6] : r[7];
}

sim::CarParams PreviewRecord(const TuneSheet& s, int32_t kind, int32_t stage, const CareerData& d) { // 0x8005F044
    auto copy = std::make_unique<TuneSheet>(s);
    TuneSheet& w = *copy;
    auto map = [&](size_t table, size_t rowsAt, size_t rowSize, int32_t st) { ApplyPartRowSettings(d.exe, table, std::span<const uint8_t>(SlotRow(w, rowsAt, rowSize, st), rowSize), w.config); };
    if (kind >= 0 && kind <= kTuneLsd && kind != kTuneProfile) w.stage[kind] = int16_t(stage);
    switch (kind) {
    case 0: map(0x12, 0x988, 0x4C, stage); break;
    case 2: map(1, 0xAE8, 0x10, stage); break;
    case 6:
        map(0x11, 0x1218, 0x24, stage);
        if (Bytes(s)[0x1238 + size_t(int64_t(stage) * 0x24)] != 0) { // 0x8005EAA4 on the gearbox row of `stage`
            int16_t ratios[8];
            std::memcpy(ratios, Cfg(w) + 0x3C, sizeof(ratios)); // above the top gear: stack residue in the original
            SheetGearRatios(s, d, ratios);                        // 0x8005E93C on the sheet as it is
            std::memcpy(Cfg(w) + 0x3C, ratios, sizeof(ratios));
        }
        break;
    case 13: // NA tune: turbo and intercooler at stage 0
        w.stage[kTuneTurbo] = 0;
        map(0x0C, 0x1428, 0x14, 0);
        w.stage[kTuneIntercooler] = 0;
        break;
    case 15: // turbo: NA tune at stage 0 and the LSD's stage-0 row settings (the original's table choice)
        map(0x0C, 0x1428, 0x14, stage);
        w.stage[kTuneNaTune] = 0;
        map(0x15, 0x1640, 0x20, 0);
        break;
    case 19: map(5, 0x1550, 0x1C, stage); break;
    case 20: map(0x1B, 0x15F0, 0x10, stage); break;
    case 21: map(0x1C, 0x1618, 0x10, stage); break;
    case 22: map(0x15, 0x1640, 0x20, stage); break;
    default: break;
    }
    return SheetRecord(w, d.tables);
}

PartPreview PreviewPart(const GarageCar& car, const TuneSheet& sheet, int32_t partKind, const CareerData& d) { // 0x8001DB90
    PartPreview p;
    p.price = PartPrice(d.tables, car.carId, partKind);
    p.owned = PartOwned(car, partKind) ? 1 : 0;
    uint8_t figures[0x6C];
    CarConfig config = sheet.config;
    sim::CarParams record;
    BuildMenuRecord(d.tables, config, record); // 0x800771AC on a copy of the sheet's configuration
    p.powerBefore = CarPowerFigures(record, figures);
    p.powerAfter = -1;
    if (p.price >= 0) { // 0x80017F18(index, kind, 0, work)
        TuneStage t{-1, 0};
        if (PartRow(d.tables, partKind, car.carId) >= 0 && (partKind > 0x2D || partKind < 0x27)) t = PartTuneStage(partKind);
        sim::CarParams after = PreviewRecord(sheet, t.kind, t.stage, d);
        p.powerAfter = CarPowerFigures(after, figures);
    }
    return p;
}

// ---------------------------------------------------------------- settings

int32_t GetSetting(const TuneSheet& s, int32_t setting, SettingValue* out, const CareerData& d) { // 0x8005FC9C
    const uint8_t* c = Cfg(s);
    const int16_t suspensionStage = s.stage[kTuneSuspension];
    auto pair = [&](size_t front, size_t rear, const uint8_t* row, size_t minF, size_t maxF, size_t minR, size_t maxR) {
        out[0].value = c[front];
        out[0].min = row[minF];
        out[0].max = row[maxF];
        out[1].value = c[rear];
        out[1].min = row[minR];
        out[1].max = row[maxR];
        return 2;
    };
    auto levels = [&](size_t front, size_t rear, const uint8_t* row, size_t maxF, size_t maxR) {
        out[0].value = c[front];
        out[0].min = 1;
        out[0].max = row[maxF];
        out[1].value = c[rear];
        out[1].min = 1;
        out[1].max = row[maxR];
        return 2;
    };
    switch (setting) {
    case kSettingSprings:
        if (suspensionStage < 3) return -1;
        return pair(0x60, 0x61, Suspension(s), 0x1D, 0x1E, 0x20, 0x21);
    case kSettingRideHeight:
        if (suspensionStage < 2) return -1;
        return pair(0x5C, 0x5D, Suspension(s), 0x13, 0x14, 0x16, 0x17);
    case kSettingDamperBump:
        if (suspensionStage < 1) return -1;
        return levels(0x64, 0x68, Suspension(s), 0x27, 0x35);
    case kSettingDamperRebound:
        if (suspensionStage != 3) return -1; // 0x8005F800
        return levels(0x66, 0x6A, Suspension(s), 0x2E, 0x3C);
    case kSettingCamber:
        if (suspensionStage < 1) return -1;
        return pair(0x5A, 0x5B, Suspension(s), 0x09, 0x0A, 0x0C, 0x0D);
    case kSettingToe: {
        if (suspensionStage <= 2) return -1;
        const uint8_t* row = Suspension(s);
        out[0].value = int16_t(c[0x5E] - 0x80);
        out[0].max = int16_t(row[0x10] - 0x80);
        out[0].min = int16_t(row[0x0F] - 0x80);
        out[1].value = int16_t(c[0x5F] - 0x80);
        out[1].max = int16_t(row[0x12] - 0x80);
        out[1].min = int16_t(row[0x11] - 0x80);
        return 2;
    }
    case kSettingAntiRoll:
        if (suspensionStage < 3) return -1;
        return levels(0x6C, 0x6D, Suspension(s), 0x43, 0x47);
    case kSettingBrakeBalance:
        if (s.stage[kTuneBrakeController] < 1) return -1;
        return levels(0x50, 0x51, SlotRow(s, 0xAE8, 0x10, s.stage[kTuneBrakeController]), 0x09, 0x0D);
    case kSettingGears: {
        if (s.stage[kTuneGearbox] <= 2) return -1;
        const uint8_t* row = SlotRow(s, 0x1218, 0x24, s.stage[kTuneGearbox]);
        int16_t generated[8] = {}, lo[8] = {}, hi[8] = {};
        SheetGearRanges(s, d, generated, lo, hi);
        const int32_t gears = row[9]; // 0x8005F834
        if (gears > 7) throw std::logic_error("0x8005FC9C: more than 7 gears (the original reads past its buffers)");
        SettingValue* e = out;
        for (int32_t i = 1; i <= gears; i++, e++) {
            e->value = int16_t(Get16(c + 0x3C + size_t(i) * 2));
            e->max = hi[i];
            e->min = lo[i];
            if (e->max < e->value) e->max = e->value;
            if (e->value < e->min) e->min = e->value;
        }
        e->value = int16_t(Get16(c + 0x4C));
        e->max = int16_t(Get16(row + 0x1C));
        e->min = int16_t(Get16(row + 0x1E));
        return gears + 1;
    }
    case kSettingGearAuto: {
        if (s.stage[kTuneGearbox] < 3) return -1;
        const uint8_t* row = SlotRow(s, 0x1218, 0x24, s.stage[kTuneGearbox]);
        out[0].value = c[0x4E];
        out[0].max = row[0x23];
        out[0].min = row[0x22];
        return 1;
    }
    case kSettingLsdInitial: case kSettingLsdAccel: case kSettingLsdDecel: {
        const uint8_t* row = SlotRow(s, 0x1640, 0x20, s.stage[kTuneLsd]);
        int32_t n = 0;
        SettingValue* e = out;
        for (uint32_t axle = 0; axle < 2; axle++) {
            const uint32_t field = axle + uint32_t(setting - 10) * 2;
            e->value = c[d.exe.Get<int32_t>(d.exe.Sim(0x800915A0u) + field * 4)];
            e->max = row[d.exe.Get<int32_t>(d.exe.Sim(0x800915C0u) + field * 12)];
            e->min = row[d.exe.Get<int32_t>(d.exe.Sim(0x800915BCu) + field * 12)];
            e->field = int16_t(d.exe.Get<uint16_t>(d.exe.Sim(0x80091600u) + field * 2));
            if (e->min < e->max) {
                e++;
                n++;
            }
        }
        return n < 1 ? -1 : n;
    }
    case kSettingLsdRearInitial: {
        if (s.stage[kTuneLsd] <= 4) return -1;
        const uint8_t* row = SlotRow(s, 0x1640, 0x20, s.stage[kTuneLsd]);
        out[0].value = c[0x6F];
        out[0].max = row[0x19];
        out[0].min = row[0x18];
        out[0].field = 1;
        return 1;
    }
    case kSettingAsm: {
        if (s.stage[kTuneAsm] < 1) return -1;
        const uint8_t* row = SlotRow(s, 0x15F0, 0x10, s.stage[kTuneAsm]);
        out[0].value = c[0x74];
        out[0].max = row[0x0D];
        out[0].min = row[0x0C];
        return 1;
    }
    case kSettingTcs: {
        if (s.stage[kTuneTcs] < 1) return -1;
        const uint8_t* row = SlotRow(s, 0x1618, 0x10, s.stage[kTuneTcs]);
        out[0].value = c[0x75];
        out[0].max = row[0x0E];
        out[0].min = row[0x0D];
        return 1;
    }
    case kSettingDownforce: {
        if (s.stage[kTuneRacingModify] <= 0) return -1;
        const uint8_t* row = SlotRow(s, 0x1550, 0x1C, s.stage[kTuneRacingModify]);
        out[0].value = c[0x52];
        out[0].max = row[0x11];
        out[0].min = row[0x10];
        out[1].value = c[0x53];
        out[1].max = row[0x14];
        out[1].min = row[0x13];
        return out[0].min < out[0].max ? 2 : -1;
    }
    default: return -1;
    }
}

void SetSetting(TuneSheet& s, int32_t setting, const SettingValue* v, int32_t count, const CareerData& d) { // 0x8005F9DC
    uint8_t* c = Cfg(s);
    const uint8_t v0 = uint8_t(v[0].value), v1 = uint8_t(v[1].value); // the entries' low bytes (param + 0 / + 8)
    switch (setting) {
    case kSettingSprings: c[0x60] = v0; c[0x61] = v1; break;
    case kSettingRideHeight: c[0x5C] = v0; c[0x5D] = v1; break;
    case kSettingDamperBump:
        if (s.stage[kTuneSuspension] != 3) { // 0x8005F800: without separate rebound settings the rebound follows
            c[0x66] = v0; c[0x6A] = v1; c[0x67] = v0; c[0x6B] = v1;
        }
        c[0x64] = v0; c[0x68] = v1; c[0x65] = v0; c[0x69] = v1;
        break;
    case kSettingDamperRebound: c[0x66] = v0; c[0x6A] = v1; c[0x67] = v0; c[0x6B] = v1; break;
    case kSettingCamber: c[0x5A] = v0; c[0x5B] = v1; break;
    case kSettingToe: c[0x5E] = uint8_t(v0 + 0x80); c[0x5F] = uint8_t(v1 + 0x80); break;
    case kSettingAntiRoll: c[0x6C] = v0; c[0x6D] = v1; break;
    case kSettingBrakeBalance: c[0x50] = v0; c[0x51] = v1; break;
    case kSettingGears: {
        const int32_t gears = SlotRow(s, 0x1218, 0x24, s.stage[kTuneGearbox])[9]; // 0x8005F834
        const SettingValue* e = v;
        for (int32_t i = 1; i <= gears; i++, e++) Put16(c + 0x3C + size_t(i) * 2, uint16_t(e->value));
        Put16(c + 0x4C, uint16_t(e->value));
        break;
    }
    case kSettingGearAuto:
        c[0x4E] = v0;
        if (Bytes(s)[0x1238 + size_t(int64_t(s.stage[kTuneGearbox]) * 0x24)] != 0) { // 0x8005EAA4
            int16_t ratios[8];
            std::memcpy(ratios, c + 0x3C, sizeof(ratios)); // the entries above the top gear: see SheetGearRatios
            SheetGearRatios(s, d, ratios);
            std::memcpy(c + 0x3C, ratios, sizeof(ratios));
            c[0x7B] = c[0x4E];
            Put16(c + 0x7C, Get16(c + 0x4C));
            Put16(c + 0x7E, uint16_t(s.stage[kTuneTyresFront]));
            Put16(c + 0x80, uint16_t(s.stage[kTuneTyresRear]));
        }
        break;
    case kSettingLsdInitial: case kSettingLsdAccel: case kSettingLsdDecel: case kSettingLsdRearInitial:
        for (int32_t i = 0; i < count; i++) c[d.exe.Get<int32_t>(d.exe.Sim(0x800915A0u) + uint32_t(int32_t(v[i].field)) * 4)] = uint8_t(v[i].value);
        break;
    case kSettingAsm: c[0x74] = v0; break;
    case kSettingTcs: c[0x75] = v0; break;
    case kSettingDownforce: c[0x52] = v0; c[0x53] = v1; break;
    default: break;
    }
}

void DefaultSetting(TuneSheet& s, int32_t setting, const CareerData& d) { // 0x80060410
    uint8_t* c = Cfg(s);
    switch (setting) {
    case kSettingSprings: { const uint8_t* r = Suspension(s); c[0x60] = r[0x1F]; c[0x61] = r[0x22]; break; }
    case kSettingRideHeight: { const uint8_t* r = Suspension(s); c[0x5C] = r[0x15]; c[0x5D] = r[0x18]; break; }
    case kSettingDamperBump: {
        const uint8_t* r = Suspension(s);
        c[0x64] = r[0x2A]; c[0x65] = r[0x2D]; c[0x68] = r[0x38]; c[0x69] = r[0x3B];
        if (s.stage[kTuneSuspension] != 3) { c[0x66] = r[0x31]; c[0x67] = r[0x34]; c[0x6A] = r[0x3F]; c[0x6B] = r[0x42]; }
        break;
    }
    case kSettingDamperRebound: { const uint8_t* r = Suspension(s); c[0x66] = r[0x31]; c[0x67] = r[0x34]; c[0x6A] = r[0x3F]; c[0x6B] = r[0x42]; break; }
    case kSettingCamber: { const uint8_t* r = Suspension(s); c[0x5A] = r[0x0B]; c[0x5B] = r[0x0E]; break; }
    case kSettingToe: c[0x5E] = 0x80; c[0x5F] = 0x80; break;
    case kSettingAntiRoll: { const uint8_t* r = Suspension(s); c[0x6C] = r[0x46]; c[0x6D] = r[0x4A]; break; }
    case kSettingBrakeBalance: {
        const uint8_t* r = SlotRow(s, 0xAE8, 0x10, s.stage[kTuneBrakeController]);
        c[0x50] = r[0x0C];
        c[0x51] = r[0x0C];
        break;
    }
    case kSettingGears: {
        const uint8_t* r = SlotRow(s, 0x1218, 0x24, s.stage[kTuneGearbox]);
        Put16(c + 0x4C, Get16(r + 0x1A));
        const int32_t gears = r[9]; // 0x8005F834
        for (int32_t i = 0; i < gears; i++) Put16(c + 0x3C + size_t(i + 1) * 2, Get16(r + 0x0C + size_t(i) * 2));
        if (r[0x20] != 0) { // 0x8005EAA4
            int16_t ratios[8], lo[8], hi[8];
            std::memcpy(ratios, c + 0x3C, sizeof(ratios)); // the entries above the top gear: see SheetGearRatios
            SheetGearRanges(s, d, ratios, lo, hi);
            std::memcpy(c + 0x3C, ratios, sizeof(ratios));
        }
        break;
    }
    case kSettingGearAuto: c[0x4E] = SlotRow(s, 0x1218, 0x24, s.stage[kTuneGearbox])[0x21]; break;
    case kSettingLsdInitial: case kSettingLsdRearInitial: {
        const uint8_t* r = SlotRow(s, 0x1640, 0x20, s.stage[kTuneLsd]);
        c[0x6E] = r[0x0D]; c[0x6F] = r[0x17];
        break;
    }
    case kSettingLsdAccel: { const uint8_t* r = SlotRow(s, 0x1640, 0x20, s.stage[kTuneLsd]); c[0x70] = r[0x10]; c[0x71] = r[0x1A]; break; }
    case kSettingLsdDecel: { const uint8_t* r = SlotRow(s, 0x1640, 0x20, s.stage[kTuneLsd]); c[0x72] = r[0x13]; c[0x73] = r[0x1D]; break; }
    case kSettingAsm: c[0x74] = SlotRow(s, 0x15F0, 0x10, s.stage[kTuneAsm])[0x0B]; break;
    case kSettingTcs: c[0x75] = SlotRow(s, 0x1618, 0x10, s.stage[kTuneTcs])[0x0C]; break;
    case kSettingDownforce: { const uint8_t* r = SlotRow(s, 0x1550, 0x1C, s.stage[kTuneRacingModify]); c[0x52] = r[0x12]; c[0x53] = r[0x15]; break; }
    default: break;
    }
}

int16_t SliderStep(int16_t value, int16_t min, int16_t max, int32_t delta) { // 0x80054D10
    const int32_t v = value;
    int32_t next = v;
    if (delta < 0) next = (delta + v < min) ? min : delta + v;
    else if (delta > 0) next = (max < delta + v) ? max : delta + v;
    return int16_t(next);
}

int32_t SliderDelta(uint32_t held, uint32_t pressed, uint32_t repeat) { // 0x80054D10 (pad record +0, +4, +0xC)
    int32_t delta = 0;
    if (held & 0x10) delta = -1;
    if (held & 0x1000) delta++;
    if (held & 0xC) delta *= 10;
    if ((pressed | repeat) & 4) delta--;
    if ((pressed | repeat) & 8) delta++;
    return delta;
}

void StoreSettings(GarageCar& car, TuneSheet& s, const CarParamTables& t, BuildScratch scratch) { // 0x80056FF0 (garage part)
    CarConfig rows = SheetRowsConfig(s, s.stage[kTuneTyresFront], s.stage[kTuneTyresRear]);
    sim::CarParams record;
    BuildMenuRecord(t, rows, record);
    s.config.engineWord = rows.engineWord; // 0x80077188: the builder's write-backs into the sheet's configuration
    s.config.exhaustByte = rows.exhaustByte;
    s.config.flags = rows.flags;
    const CarFigures f = SheetFigures(s, t, scratch);
    std::memcpy(&car.config, &s.config, sizeof(CarConfig));
    PutFigures(car, f);
}

void CommitSettings(TuneSheet& s, const CarParamTables& t, BuildScratch scratch, const SettingsCommit& out) { // ovl0 0x80056FF0
    CarConfig rows = SheetRowsConfig(s, s.stage[kTuneTyresFront], s.stage[kTuneTyresRear]); // 0x8005F410
    sim::CarParams record;
    BuildMenuRecord(t, rows, record);                                                       // 0x80077214
    if (out.raceRecord) std::memcpy(out.raceRecord, &record, sizeof(record));
    s.config.engineWord = rows.engineWord; // 0x80077188
    s.config.exhaustByte = rows.exhaustByte;
    s.config.flags = rows.flags;
    if (out.raceSlotConfig) std::memcpy(out.raceSlotConfig, &s.config, sizeof(CarConfig));
    if (out.garageCar) {
        const CarFigures f = SheetFigures(s, t, scratch); // 0x8005F958
        std::memcpy(&out.garageCar->config, &s.config, sizeof(CarConfig));
        PutFigures(*out.garageCar, f);
    }
    if (out.purchaseSheet) std::memcpy(out.purchaseSheet, &s, sizeof(TuneSheet));
}

int32_t AdjustSetting(GarageBlock& g, int32_t index, int32_t setting, int32_t entry, int32_t delta, const CareerData& d, BuildScratch scratch) {
    auto sheet = std::make_unique<TuneSheet>();
    LoadCarSheet(*sheet, g.cars[index], d.tables);
    SettingValue values[9]{};
    const int32_t count = GetSetting(*sheet, setting, values, d);
    if (count < 1 || entry < 0 || entry >= count) return -1;
    values[entry].value = SliderStep(values[entry].value, values[entry].min, values[entry].max, delta);
    SetSetting(*sheet, setting, values, count, d);
    StoreSettings(g.cars[index], *sheet, d.tables, scratch);
    return values[entry].value;
}

void ResetSetting(GarageBlock& g, int32_t index, int32_t setting, const CareerData& d, BuildScratch scratch) {
    auto sheet = std::make_unique<TuneSheet>();
    LoadCarSheet(*sheet, g.cars[index], d.tables);
    DefaultSetting(*sheet, setting, d);
    StoreSettings(g.cars[index], *sheet, d.tables, scratch);
}

} // namespace gt2::career
