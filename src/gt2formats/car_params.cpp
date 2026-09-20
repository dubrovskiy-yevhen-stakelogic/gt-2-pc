#include "gt2formats/car_params.h"

#include <cstring>
#include <stdexcept>

#include "gt2vfs/gtfs.h"
#include "gt2formats/car_info.h"

// The original builder (0x80077214) runs a list of (part kind, row, mapping table) entries through a small
// interpreter (0x80077634) whose entries are { dst offset, src offset, element size x count, op } with the ops
// copy, add (u8 / u16 / u32 wrap), add-repeat (one source byte added to `count` destination bytes), multiply by
// src / 100 and multiply by src / 1000 (signed for 1- and 4-byte elements, unsigned for 2-byte ones, C
// truncating division). The functions below reproduce those entries field by field, in the original's order,
// because later entries overwrite earlier ones.
namespace gt2 {
namespace {

uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24); }
uint16_t U16(std::span<const uint8_t> b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }

// op 0 / 1 on one byte: (s8 * s8) / div, stored back as a byte (0x800778E4, `lb` loads).
void MulPercent(uint8_t& dst, uint8_t src, int32_t div) { dst = uint8_t((int32_t(int8_t(dst)) * int32_t(int8_t(src))) / div); }
// op 0 / 1 on a half-word: (u16 * u16) / div with the product wrapped to 32 bits (`lhu` loads, `mult`, `div`).
void MulPercent(int16_t& dst, uint16_t src, int32_t div) { dst = int16_t(int32_t(uint32_t(uint16_t(dst)) * uint32_t(src)) / div); }
// op 3 on one byte.
void Add(uint8_t& dst, uint8_t src) { dst = uint8_t(dst + src); }

// 0x800779C4: value at `position` (1-based) between `first` and `last` over `levels` positions.
uint8_t Interpolate(uint8_t first, uint8_t last, uint8_t levels, uint8_t position) {
    if (levels < 2) return first;
    return uint8_t(int32_t(first) + ((int32_t(last) - int32_t(first)) * (int32_t(position) - 1)) / (int32_t(levels) - 1));
}

// The rows the builder resolves from the configuration (0x800763E8): the working struct's row pointers.
struct ResolvedRows {
    const BrakeRow* brakes;
    const BrakeControllerRow* brakeController;
    const SteeringRow* steering;
    const ChassisRow* chassis;
    const EngineRow* engine;
    const DrivetrainRow* drivetrain;
    const GearboxRow* gearbox;
    const SuspensionRow* suspension;
    const LsdRow* lsd;
    const TyreSizeRow* tyreSizeFront;
    const TyreCompoundRow* compoundFront;
    const SurfaceGripRow* surfaceGrip;
    const TyreSizeRow* tyreSizeRear;
    const TyreCompoundRow* compoundRear;
    const LightweightRow* lightweight;
    const RacingModifyRow* racingModify;
    const PowerPartRow* portPolish;
    const EngineTuneRow* engineBalance;
    const PowerPartRow* displacement;
    const PowerPartRow* computer;
    const EngineTuneRow* naTune;
    const TurboKitRow* turboKit;
    const FlywheelRow* flywheel;
    const ClutchRow* clutch;
    const PropellerShaftRow* propellerShaft;
    const PowerPartRow* muffler;
    const PowerPartRow* intercooler;
    const AsmRow* asmRow;
    const TcsRow* tcs;
};

ResolvedRows Resolve(const CarParamTables& t, const CarConfig& c) {
    ResolvedRows r{};
    r.brakes = &t.RowAs<BrakeRow>(kTableBrakes, c.brakes);
    r.brakeController = &t.RowAs<BrakeControllerRow>(kTableBrakeController, c.brakeController);
    r.steering = &t.RowAs<SteeringRow>(kTableSteering, c.steering);
    r.chassis = &t.RowAs<ChassisRow>(kTableChassis, c.chassis);
    r.engine = &t.RowAs<EngineRow>(kTableEngine, c.engine);
    r.drivetrain = &t.RowAs<DrivetrainRow>(kTableDrivetrain, c.drivetrain);
    r.gearbox = &t.RowAs<GearboxRow>(kTableGearbox, c.gearbox);
    r.suspension = &t.RowAs<SuspensionRow>(kTableSuspension, c.suspension);
    r.lsd = &t.RowAs<LsdRow>(kTableLsd, c.lsd);
    r.lightweight = &t.RowAs<LightweightRow>(kTableLightweight, c.lightweight);
    r.racingModify = &t.RowAs<RacingModifyRow>(kTableRacingModify, c.racingModify);
    r.portPolish = &t.RowAs<PowerPartRow>(kTablePortPolish, c.portPolish);
    r.engineBalance = &t.RowAs<EngineTuneRow>(kTableEngineBalance, c.engineBalance);
    r.displacement = &t.RowAs<PowerPartRow>(kTableDisplacement, c.displacement);
    r.computer = &t.RowAs<PowerPartRow>(kTableComputer, c.computer);
    r.naTune = &t.RowAs<EngineTuneRow>(kTableNaTune, c.naTune);
    r.turboKit = &t.RowAs<TurboKitRow>(kTableTurboKit, c.turboKit);
    r.flywheel = &t.RowAs<FlywheelRow>(kTableFlywheel, c.flywheel);
    r.clutch = &t.RowAs<ClutchRow>(kTableClutch, c.clutch);
    r.propellerShaft = &t.RowAs<PropellerShaftRow>(kTablePropellerShaft, c.propellerShaft);
    r.muffler = &t.RowAs<PowerPartRow>(kTableMuffler, c.muffler);
    r.intercooler = &t.RowAs<PowerPartRow>(kTableIntercooler, c.intercooler);
    r.asmRow = &t.RowAs<AsmRow>(kTableAsm, c.activeStability);
    r.tcs = &t.RowAs<TcsRow>(kTableTcs, c.tractionControl);
    // Tyres: the front / rear tyre rows only point at the size, compound and surface-grip rows.
    const TyresFrontRow& front = t.RowAs<TyresFrontRow>(kTableTyresFront, c.tyresFront);
    r.tyreSizeFront = &t.RowAs<TyreSizeRow>(kTableTyreSize, front.tyreSizeRow);
    r.compoundFront = &t.RowAs<TyreCompoundRow>(kTableTyreCompound, front.compoundRow);
    r.surfaceGrip = &t.RowAs<SurfaceGripRow>(kTableSurfaceGrip, front.surfaceGripRow);
    const TyresRearRow& rear = t.RowAs<TyresRearRow>(kTableTyresRear, c.tyresRear);
    r.tyreSizeRear = &t.RowAs<TyreSizeRow>(kTableTyreSize, rear.tyreSizeRow);
    r.compoundRear = &t.RowAs<TyreCompoundRow>(kTableTyreCompound, rear.compoundRow);
    return r;
}

// Power multiplier step of the part loop: multiplier = multiplier * (gain + 100) / 100, gain 255 -> factor 5.19.
void ApplyPowerGain(int32_t& multiplier, uint8_t gainPercent) {
    const int32_t factor = gainPercent == 0xFF ? 0x207 : int32_t(gainPercent) + 100;
    multiplier = (multiplier * factor) / 100;
}

void CopyCompound(sim::CarParams& p, const TyreCompoundRow& c, int axle) {
    if (axle == 0) {
        p.tyreGripPercent[0] = c.gripPercent;
        std::memcpy(p.loadGripXs, c.loadGripXs, 4);
        std::memcpy(p.loadGripYs, c.loadGripYs, 4);
        std::memcpy(p.slipAngleXs, c.slipAngleXs, 8);
        std::memcpy(p.slipAngleYs, c.slipAngleYs, 8);
        std::memcpy(p.slipRatioNegXs, c.slipRatioNegXs, 6);
        std::memcpy(p.slipRatioNegYs, c.slipRatioNegYs, 6);
        std::memcpy(p.slipRatioNegYs2, c.slipRatioNegYs2, 6);
        std::memcpy(p.slipRatioPosXs, c.slipRatioPosXs, 6);
        std::memcpy(p.slipRatioPosYs, c.slipRatioPosYs, 6);
        std::memcpy(p.slipRatioPosYs2, c.slipRatioPosYs2, 6);
    } else {
        p.tyreGripPercent[1] = c.gripPercent;
        std::memcpy(p.loadGripXsRear, c.loadGripXs, 4);
        std::memcpy(p.loadGripYsRear, c.loadGripYs, 4);
        std::memcpy(p.slipAngleXsRear, c.slipAngleXs, 8);
        std::memcpy(p.slipAngleYsRear, c.slipAngleYs, 8);
        std::memcpy(p.slipRatioNegXsRear, c.slipRatioNegXs, 6);
        std::memcpy(p.slipRatioNegYsRear, c.slipRatioNegYs, 6);
        std::memcpy(p.slipRatioNegYs2Rear, c.slipRatioNegYs2, 6);
        std::memcpy(p.slipRatioPosXsRear, c.slipRatioPosXs, 6);
        std::memcpy(p.slipRatioPosYsRear, c.slipRatioPosYs, 6);
        std::memcpy(p.slipRatioPosYs2Rear, c.slipRatioPosYs2, 6);
    }
}

// 0x80017E18: track width (mm) from the wheel's lateral position (model units, 1/4096 m) and the rim code.
int16_t TrackFromWheel(uint8_t rimCode, int16_t wheelLateral) {
    int32_t lateral = wheelLateral;
    if (lateral < 0) lateral = -lateral;
    const int32_t halfRim = ((int32_t(rimCode) * 0xA000 + 0x5000) / 1000) >> 1;
    return int16_t((((lateral - halfRim) * 1000) >> 12) << 1);
}

} // namespace

// ---------------------------------------------------------------- GTDT

GtdtFile::GtdtFile(std::vector<uint8_t> data) : data_(std::move(data)) {
    if (data_.size() < 8 || std::memcmp(data_.data(), "GTDT", 4) != 0) throw std::runtime_error("GTDT: bad magic");
    const size_t count = U16(data_, 6);
    if (8 + count * 8 > data_.size()) throw std::runtime_error("GTDT: directory out of bounds");
    for (size_t i = 0; i < count; i++) {
        GtdtEntry e{U32(data_, 8 + i * 8), U32(data_, 12 + i * 8)};
        if (uint64_t(e.offset) + e.size > data_.size()) throw std::runtime_error("GTDT: entry out of bounds");
        entries_.push_back(e);
    }
}

std::span<const uint8_t> GtdtFile::Bytes(size_t i) const {
    const GtdtEntry& e = entries_.at(i);
    return std::span<const uint8_t>(data_).subspan(e.offset, e.size);
}

// ---------------------------------------------------------------- tables

CarParamTables::CarParamTables(std::vector<uint8_t> gtdt) : file_(std::move(gtdt)) {
    if (file_.EntryCount() / 2 < kCarParamTableCount) throw std::runtime_error("car params: not a car parameter file (too few tables)");
}

CarParamTables CarParamTables::Load(const GtfsVolume& vol, const std::string& path) { return CarParamTables(vol.Read(path)); }

size_t CarParamTables::RowCount(size_t table) const {
    if (table >= kCarParamTableCount) throw std::out_of_range("car params: table index");
    return file_.Entry(table).size / kCarParamRowSizes[table]; // the loader's integer division (0x80076B74)
}

std::span<const uint8_t> CarParamTables::Row(size_t table, size_t index) const {
    if (index >= RowCount(table)) throw std::out_of_range("car params: row " + std::to_string(index) + " of table " + std::to_string(table));
    const size_t size = kCarParamRowSizes[table];
    return file_.Bytes(table).subspan(index * size, size);
}

std::vector<uint16_t> CarParamTables::RowsOfCar(size_t table, uint32_t carId) const {
    std::vector<uint16_t> rows;
    const size_t count = RowCount(table);
    for (size_t i = 0; i < count; i++)
        if (U32(Row(table, i), 0) == carId) rows.push_back(uint16_t(i));
    return rows;
}

// ---------------------------------------------------------------- configuration

ReplayCar ReplayCarAt(std::span<const uint8_t> replay, size_t slot) {
    if (slot >= kReplayCarSlotCount) throw std::out_of_range("replay car slot");
    const size_t base = kReplayCarSlotOffset + slot * kReplayCarSlotStride;
    if (base + kReplayCarSlotStride > replay.size()) throw std::runtime_error("replay: car slot out of bounds");
    ReplayCar car;
    car.carId = U32(replay, base);
    std::memcpy(&car.config, replay.data() + base + kReplayCarConfigOffset, sizeof(CarConfig));
    for (size_t i = base + 0x90; i < base + kReplayCarSlotStride && replay[i] != 0; i++) car.name.push_back(char(replay[i]));
    return car;
}

CarConfig StockCarConfig(const CarParamTables& tables, uint32_t carId) {
    // The car's own row: the one with its id; among several (upgrade stages) the one with price 0.
    auto stockRow = [&](size_t table, size_t priceOffset, bool byStageByte) -> uint16_t {
        for (uint16_t row : tables.RowsOfCar(table, carId)) {
            const std::span<const uint8_t> bytes = tables.Row(table, row);
            const bool stock = byStageByte ? bytes[priceOffset] == 0 : U32(bytes, priceOffset) == 0;
            if (stock) return row;
        }
        throw std::runtime_error("car params: no stock row for car " + UnpackCarId(carId) + " in table " + std::to_string(table));
    };
    CarConfig c{};
    c.brakes = stockRow(kTableBrakes, 4, false);
    c.brakeController = 0;
    c.steering = 0;
    // Tables 3, 6 and 13 have exactly one row per car.
    auto onlyRow = [&](size_t table) -> uint16_t {
        const std::vector<uint16_t> rows = tables.RowsOfCar(table, carId);
        if (rows.empty()) throw std::runtime_error("car params: car " + UnpackCarId(carId) + " not in table " + std::to_string(table));
        return rows[0];
    };
    c.chassis = onlyRow(kTableChassis);
    c.engine = onlyRow(kTableEngine);
    c.drivetrain = onlyRow(kTableDrivetrain);
    c.gearbox = stockRow(kTableGearbox, 4, false);
    c.suspension = stockRow(kTableSuspension, 4, false);
    c.lsd = stockRow(kTableLsd, 4, false);
    c.tyresFront = stockRow(kTableTyresFront, 4, false);
    c.tyresRear = stockRow(kTableTyresRear, 4, true);
    c.racingModify = stockRow(kTableRacingModify, 4, false);
    c.clutch = stockRow(kTableClutch, 4, false);
    // Upgrade-only tables: row 0 is "no part".
    c.lightweight = c.portPolish = c.engineBalance = c.displacement = c.computer = c.naTune = c.turboKit = 0;
    c.flywheel = c.propellerShaft = c.muffler = c.intercooler = c.activeStability = c.tractionControl = 0;
    // Settings at the rows' defaults (what the six attract-race cars carry; a hypothesis for the level fields).
    const BrakeControllerRow& bc = tables.RowAs<BrakeControllerRow>(kTableBrakeController, c.brakeController);
    c.brakeBalance[0] = c.brakeBalance[1] = bc.absDefault;
    const RacingModifyRow& rm = tables.RowAs<RacingModifyRow>(kTableRacingModify, c.racingModify);
    c.downforce[0] = rm.downforceFront;
    c.downforce[1] = rm.downforceRear;
    const TurboKitRow& tk = tables.RowAs<TurboKitRow>(kTableTurboKit, c.turboKit);
    c.turboBoost10 = tk.turboBoost10;
    c.turboSpoolRpm100 = tk.turboSpoolRpm100;
    c.turboSpoolRate10 = tk.turboSpoolRate10;
    c.turboBoost10Second = tk.turboBoost10Second;
    c.turboSpoolRpm100Second = tk.turboSpoolRpm100Second;
    c.turboSpoolRate10Second = tk.turboSpoolRate10Second;
    const SuspensionRow& s = tables.RowAs<SuspensionRow>(kTableSuspension, c.suspension);
    c.camber10[0] = s.camberFront10;
    c.camber10[1] = s.camberRear10;
    c.rideHeightMm[0] = s.rideHeightFront;
    c.rideHeightMm[1] = s.rideHeightRear;
    c.toeCode[0] = c.toeCode[1] = 128;
    c.springCode[0] = s.springFront;
    c.springCode[1] = s.springRear;
    c.damperScaleDivisor[0] = s.damperScaleDivisor[0];
    c.damperScaleDivisor[1] = s.damperScaleDivisor[1];
    for (uint8_t& level : c.damperLevel) level = 1;
    c.antiRollLevel[0] = c.antiRollLevel[1] = 1;
    const LsdRow& lsd = tables.RowAs<LsdRow>(kTableLsd, c.lsd);
    c.diffInitial[0] = lsd.diffInitialFront[0];
    c.diffInitial[1] = lsd.diffInitialRear[0];
    c.diffAccel[0] = lsd.diffAccelFront[0];
    c.diffAccel[1] = lsd.diffAccelRear[0];
    c.diffDecel[0] = lsd.diffDecelFront[0];
    c.diffDecel[1] = lsd.diffDecelRear[0];
    c.asmLevel = c.tcsLevel = 1;
    const GearboxRow& g = tables.RowAs<GearboxRow>(kTableGearbox, c.gearbox);
    std::memcpy(c.gearRatio, g.gearRatio, sizeof(c.gearRatio));
    c.finalDrive = g.finalDrive;
    c.gearAutoFinal = g.gearAutoFinal;
    c.byte4F = g.raw22[0]; // 0xFF in the replay's configurations, like the byte after gearAutoFinal in their gear rows
    return c;
}

// ---------------------------------------------------------------- builder

CarBodyDimensions BodyDimensionsOf(const CarModel& model) {
    if (model.lods.empty()) throw std::runtime_error("car params: model without LODs");
    CarBodyDimensions d;
    d.bboxFront = model.lods[0].bbox[2];
    d.bboxRear = model.lods[0].bbox[6];
    d.scaleShift = model.lods[0].scale;
    d.wheelLateralFront = model.wheels[0].w;
    d.wheelLateralRear = model.wheels[2].w;
    return d;
}

sim::CarParams BuildCarParams(const CarParamTables& tables, CarConfig& config, const CarBodyDimensions& body) {
    const ResolvedRows r = Resolve(tables, config);
    sim::CarParams p;
    std::memset(&p, 0, sizeof(p));
    int32_t power = 1000;

    // ---- the part loop of 0x80077214 (mapping list 0x80092CA4), in its order
    // kind 0: brakes
    p.brakeFront = r.brakes->brakeFront;
    p.brakeRear = r.brakes->brakeRear;
    p.handbrake = r.brakes->handbrake;
    ApplyPowerGain(power, 0);
    // kind 1: brake controller
    p.absGain[0] = r.brakeController->absDefault;
    ApplyPowerGain(power, 0);
    // kind 2: steering
    std::memcpy(p.steerLimitXs, r.steering->steerLimitXs, 6);
    std::memcpy(p.steerLimitYs, r.steering->steerLimitYs, 6);
    p.steerRateDeg = r.steering->steerRateDeg;
    p.steerLockDeg = r.steering->steerLockDeg;
    ApplyPowerGain(power, 0);
    // kind 3: chassis
    p.frontWeightPercent = r.chassis->frontWeightPercent;
    p.tyreGripModifier[0] = r.chassis->tyreGripModifier[0];
    p.tyreGripModifier[1] = r.chassis->tyreGripModifier[1];
    p.height = r.chassis->height;
    p.wheelbase = r.chassis->wheelbase;
    p.weightKg = r.chassis->weightKg;
    p.yawInertiaCode = r.chassis->yawInertiaCode;
    p.pitchInertiaCode = r.chassis->pitchInertiaCode;
    p.rollInertiaCode = r.chassis->rollInertiaCode;
    p.reserved05C = r.chassis->byte13;
    ApplyPowerGain(power, 0);
    // kind 6: engine
    std::memcpy(p.torque, r.engine->torque, sizeof(p.torque));
    p.torqueMultiplier1000 = r.engine->torqueMultiplier100; // low byte only; scaled x10 below
    p.downshiftFloorRpm10 = r.engine->downshiftFloorRpm10;
    p.idleRpm10 = r.engine->idleRpm10;
    p.revLimitRpm100 = r.engine->revLimitRpm100;
    p.upshiftRpm100 = r.engine->upshiftRpm100;
    std::memcpy(p.torqueRpm100, r.engine->torqueRpm100, 16);
    p.torquePointCount = r.engine->torquePointCount;
    ApplyPowerGain(power, 0);
    // kind 13: drivetrain
    p.centreSplitPercent = r.drivetrain->centreSplitPercent;
    p.driveType = r.drivetrain->driveType;
    p.fourWheelType = r.drivetrain->fourWheelType;
    p.engineBrake = r.drivetrain->engineBrake;
    p.wheelInertia[0] = r.drivetrain->wheelInertia[0];
    p.wheelInertia[1] = r.drivetrain->wheelInertia[1];
    p.engineInertia = r.drivetrain->engineInertia;
    p.axleInertiaCode[0] = r.drivetrain->axleInertiaCode[0];
    p.axleInertiaCode[1] = r.drivetrain->axleInertiaCode[1];
    ApplyPowerGain(power, 0);
    // kind 17: gearbox
    p.gearCount = r.gearbox->gearCount;
    std::memcpy(p.gearRatio, r.gearbox->gearRatio, sizeof(p.gearRatio));
    p.finalDrive = r.gearbox->finalDrive;
    p.gearAutoSet = r.gearbox->gearAutoSet;
    p.gearAutoFinal = r.gearbox->gearAutoFinal;
    ApplyPowerGain(power, 0);
    // kind 18: suspension (defaults; the config's settings override most of them below)
    {
        const SuspensionRow& s = *r.suspension;
        p.camberFront10 = s.camberFront10;
        p.camberRear10 = s.camberRear10;
        p.rideHeightMm[0] = s.rideHeightFront;
        p.rideHeightMm[1] = s.rideHeightRear;
        p.bumpTravelMm[0] = s.bumpTravelMm[0];
        p.bumpTravelMm[1] = s.bumpTravelMm[1];
        p.droopTravelMm[0] = s.droopTravelMm[0];
        p.droopTravelMm[1] = s.droopTravelMm[1];
        p.suspension[0][0] = s.springFront;
        p.suspension[1][0] = s.springRear;
        p.damperScaleDivisor[0] = s.damperScaleDivisor[0];
        p.damperScaleDivisor[1] = s.damperScaleDivisor[1];
        p.suspension[0][2] = s.bumpStopFront;
        p.suspension[1][2] = s.bumpStopRear;
        p.suspension[0][5] = s.bumpLowFront[2];
        p.suspension[0][7] = s.bumpHighFront[2];
        p.suspension[0][9] = s.reboundLowFront[2];
        p.suspension[0][11] = s.reboundHighFront[2];
        p.suspension[1][5] = s.bumpLowRear[2];
        p.suspension[1][7] = s.bumpHighRear[2];
        p.suspension[1][9] = s.reboundLowRear[2];
        p.suspension[1][11] = s.reboundHighRear[2];
        p.suspension[0][1] = s.antiRollFront[2];
        p.suspension[1][1] = s.antiRollRear[2];
    }
    ApplyPowerGain(power, 0);
    // kind 21: limited-slip differential (defaults; the config overrides the torques below)
    p.diffTypeCode[0] = r.lsd->diffTypeFront;
    p.diffInitialTorque[0] = r.lsd->diffInitialFront[0];
    p.diffAccel[0] = r.lsd->diffAccelFront[0];
    p.diffDecel[0] = r.lsd->diffDecelFront[0];
    p.diffTypeCode[1] = r.lsd->diffTypeRear;
    p.diffInitialTorque[1] = r.lsd->diffInitialRear[0];
    p.diffAccel[1] = r.lsd->diffAccelRear[0];
    p.diffDecel[1] = r.lsd->diffDecelRear[0];
    ApplyPowerGain(power, 0);
    // kind 27: ASM
    p.asmYawGain100 = r.asmRow->asmYawGain100;
    p.asmYawThreshold100 = r.asmRow->asmYawThreshold[0];
    ApplyPowerGain(power, 0);
    // kind 28: TCS
    p.tcsGain10 = r.tcs->tcsGain10;
    p.tcsFalloffGain10 = r.tcs->tcsFalloffGain[0];
    p.tcsSteerGain100 = r.tcs->tcsSteerGain100;
    ApplyPowerGain(power, 0);
    // kind 4: lightweight
    MulPercent(p.weightKg, r.lightweight->weightPermille, 1000);
    MulPercent(p.rollInertiaCode, r.lightweight->rollInertiaPercent, 100);
    ApplyPowerGain(power, 0);
    // kind 5: racing modification
    MulPercent(p.yawInertiaCode, r.racingModify->yawInertiaPercent, 100);
    MulPercent(p.rollInertiaCode, r.racingModify->rollInertiaPercent, 100);
    p.dragCoefficient100 = r.racingModify->dragCoefficient100;
    p.downforce[0] = r.racingModify->downforceFront;
    p.downforce[1] = r.racingModify->downforceRear;
    p.frontTrack = r.racingModify->frontTrack;
    p.rearTrack = r.racingModify->rearTrack;
    p.width = r.racingModify->width;
    ApplyPowerGain(power, 0);
    // kind 7: port polish
    ApplyPowerGain(power, r.portPolish->powerPercent);
    // kind 8: engine balance
    for (uint8_t& rpm : p.torqueRpm100) Add(rpm, r.engineBalance->rpmShift100);
    Add(p.revLimitRpm100, r.engineBalance->revLimitShift100);
    ApplyPowerGain(power, r.engineBalance->powerPercent);
    // kind 9: displacement
    ApplyPowerGain(power, r.displacement->powerPercent);
    // kind 10: computer
    ApplyPowerGain(power, r.computer->powerPercent);
    // kind 11: NA tune
    for (uint8_t& rpm : p.torqueRpm100) Add(rpm, r.naTune->rpmShift100);
    Add(p.revLimitRpm100, r.naTune->revLimitShift100);
    Add(p.upshiftRpm100, r.naTune->revLimitShift100); // mapping table 0x80092DEC
    ApplyPowerGain(power, r.naTune->powerPercent);
    // kind 12: turbo kit
    p.turboBoostCap10 = r.turboKit->turboBoostCap10;
    p.turboBoost10 = r.turboKit->turboBoost10;
    p.turboSpoolRpm100 = r.turboKit->turboSpoolRpm100;
    p.turboSpoolRate10 = r.turboKit->turboSpoolRate10;
    p.turboBoost10Second = r.turboKit->turboBoost10Second;
    p.turboSpoolRpm100Second = r.turboKit->turboSpoolRpm100Second;
    p.turboSpoolRate10Second = r.turboKit->turboSpoolRate10Second;
    Add(p.revLimitRpm100, r.turboKit->revLimitShift100);
    Add(p.upshiftRpm100, r.turboKit->upshiftShift100);
    p.powerPercent = r.turboKit->powerPercentFirst;
    ApplyPowerGain(power, r.turboKit->powerPercent);
    // kind 14: flywheel
    MulPercent(p.engineBrake, r.flywheel->engineBrakePercent, 100);
    MulPercent(p.engineInertia, r.flywheel->engineInertiaPercent, 100);
    MulPercent(p.wheelInertia[0], r.flywheel->wheelInertiaPercent, 100);
    MulPercent(p.wheelInertia[1], r.flywheel->wheelInertiaPercent, 100); // mapping table 0x80092DF4
    ApplyPowerGain(power, 0);
    // kind 15: clutch
    MulPercent(p.engineBrake, r.clutch->engineBrakePercent, 100);
    MulPercent(p.engineInertia, r.clutch->engineInertiaPercent, 100);
    MulPercent(p.wheelInertia[0], r.clutch->wheelInertiaFrontPercent, 100);
    MulPercent(p.wheelInertia[1], r.clutch->wheelInertiaRearPercent, 100);
    p.reserved033 = r.clutch->byte0D;
    ApplyPowerGain(power, 0);
    // kind 16: propeller shaft
    MulPercent(p.engineBrake, r.propellerShaft->engineBrakePercent, 100);
    MulPercent(p.wheelInertia[0], r.propellerShaft->wheelInertiaFrontPercent, 100);
    MulPercent(p.wheelInertia[1], r.propellerShaft->wheelInertiaRearPercent, 100);
    MulPercent(p.axleInertiaCode[0], r.propellerShaft->wheelInertiaFrontPercent, 100); // mapping table 0x80092DFC
    MulPercent(p.axleInertiaCode[1], r.propellerShaft->wheelInertiaFrontPercent, 100);
    ApplyPowerGain(power, 0);
    // kind 20: muffler
    ApplyPowerGain(power, r.muffler->powerPercent);
    // kind 19: intercooler
    ApplyPowerGain(power, r.intercooler->powerPercent);

    // ---- after the loop (0x80077368..)
    p.yawInertiaCode = uint8_t((int32_t(p.yawInertiaCode) * int32_t(r.lightweight->weightPermille)) / 1000);
    p.weightKg = int16_t((int32_t(p.weightKg) * int32_t(r.racingModify->yawInertiaPercent)) / 100);
    // tyres (mapping tables 0x80092D6C, 0x80092D8C, 0x80092820, 0x80092D7C, 0x80092DBC)
    p.tyreWidthCode[0] = r.tyreSizeFront->tyreWidthCode;
    p.rimCode[0] = r.tyreSizeFront->rimCode;
    p.tyreAspectCode[0] = r.tyreSizeFront->tyreAspectCode;
    CopyCompound(p, *r.compoundFront, 0);
    std::memcpy(p.surfaceGripPercent, r.surfaceGrip->surfaceGripPercent, 7);
    p.tyreWidthCode[1] = r.tyreSizeRear->tyreWidthCode;
    p.rimCode[1] = r.tyreSizeRear->rimCode;
    p.tyreAspectCode[1] = r.tyreSizeRear->tyreAspectCode;
    CopyCompound(p, *r.compoundRear, 1);
    // 0x80077AF4: suspension settings of the configuration (mapping table 0x80092E24 + damper / anti-roll positions)
    {
        const SuspensionRow& s = *r.suspension;
        p.camberFront10 = config.camber10[0];
        p.camberRear10 = config.camber10[1];
        p.rideHeightMm[0] = config.rideHeightMm[0];
        p.rideHeightMm[1] = config.rideHeightMm[1];
        p.toeCode[0] = config.toeCode[0];
        p.toeCode[1] = config.toeCode[1];
        p.suspension[0][0] = config.springCode[0];
        p.suspension[1][0] = config.springCode[1];
        p.damperScaleDivisor[0] = config.damperScaleDivisor[0];
        p.damperScaleDivisor[1] = config.damperScaleDivisor[1];
        p.suspension[0][5] = Interpolate(s.bumpLowFront[0], s.bumpLowFront[1], s.bumpLevelsFront, config.damperLevel[0]);
        p.suspension[0][7] = Interpolate(s.bumpHighFront[0], s.bumpHighFront[1], s.bumpLevelsFront, config.damperLevel[1]);
        p.suspension[0][9] = Interpolate(s.reboundLowFront[0], s.reboundLowFront[1], s.reboundLevelsFront, config.damperLevel[2]);
        p.suspension[0][11] = Interpolate(s.reboundHighFront[0], s.reboundHighFront[1], s.reboundLevelsFront, config.damperLevel[3]);
        p.suspension[1][5] = Interpolate(s.bumpLowRear[0], s.bumpLowRear[1], s.bumpLevelsRear, config.damperLevel[4]);
        p.suspension[1][7] = Interpolate(s.bumpHighRear[0], s.bumpHighRear[1], s.bumpLevelsRear, config.damperLevel[5]);
        p.suspension[1][9] = Interpolate(s.reboundLowRear[0], s.reboundLowRear[1], s.reboundLevelsRear, config.damperLevel[6]);
        p.suspension[1][11] = Interpolate(s.reboundHighRear[0], s.reboundHighRear[1], s.reboundLevelsRear, config.damperLevel[7]);
        p.suspension[0][1] = Interpolate(s.antiRollFront[0], s.antiRollFront[1], s.antiRollLevelsFront, config.antiRollLevel[0]);
        p.suspension[1][1] = Interpolate(s.antiRollRear[0], s.antiRollRear[1], s.antiRollLevelsRear, config.antiRollLevel[1]);
    }
    // 0x80077AC4: turbo settings (mapping table 0x80092E08)
    p.turboBoost10 = config.turboBoost10;
    p.turboSpoolRpm100 = config.turboSpoolRpm100;
    p.turboSpoolRate10 = config.turboSpoolRate10;
    p.turboBoost10Second = config.turboBoost10Second;
    p.turboSpoolRpm100Second = config.turboSpoolRpm100Second;
    p.turboSpoolRate10Second = config.turboSpoolRate10Second;
    // 0x80077AAC: downforce
    p.downforce[0] = config.downforce[0];
    p.downforce[1] = config.downforce[1];
    // 0x80077A6C: gears
    std::memcpy(p.gearRatio, config.gearRatio, sizeof(p.gearRatio));
    p.finalDrive = config.finalDrive;
    p.gearAutoFinal = config.gearAutoFinal;
    // 0x800779FC: brake balance
    p.absGain[0] = Interpolate(r.brakeController->absLowFront, r.brakeController->absHighFront, r.brakeController->levelsFront, config.brakeBalance[0]);
    p.absGain[1] = Interpolate(r.brakeController->absLowRear, r.brakeController->absHighRear, r.brakeController->levelsRear, config.brakeBalance[1]);
    // 0x80077C9C: empty. 0x80077D2C: differential settings (mapping table 0x80092E50)
    p.diffInitialTorque[0] = config.diffInitial[0];
    p.diffInitialTorque[1] = config.diffInitial[1];
    p.diffAccel[0] = config.diffAccel[0];
    p.diffAccel[1] = config.diffAccel[1];
    p.diffDecel[0] = config.diffDecel[0];
    p.diffDecel[1] = config.diffDecel[1];
    // 0x80077CE8: TCS position, 0x80077CA4: ASM position
    p.tcsFalloffGain10 = Interpolate(r.tcs->tcsFalloffGain[1], r.tcs->tcsFalloffGain[2], r.tcs->levels, config.tcsLevel);
    p.asmYawThreshold100 = Interpolate(r.asmRow->asmYawThreshold[1], r.asmRow->asmYawThreshold[2], r.asmRow->levels, config.asmLevel);
    // power
    if (config.flags & 1) p.torqueMultiplier1000 = config.torqueMultiplier100;
    config.engineWord = r.engine->word0A;
    config.exhaustByte = r.muffler->byte08;
    if (r.turboKit->turboBoostCap10 != 0) {
        config.flags |= 2;
        config.exhaustByte = uint8_t(config.exhaustByte + 4);
    }
    p.powerPercentTop = uint16_t(power);
    p.torqueMultiplier1000 = uint16_t(p.torqueMultiplier1000 * 10);
    if (config.flags & 0x80) p.gearAutoSet = 0;
    // constants of the builder (0x80077534..)
    for (int axle = 0; axle < 2; axle++) {
        p.suspension[axle][3] = 0x28; // unsprung mass 40 kg per wheel
        p.suspension[axle][4] = 0x1E; // damper knees 30 % / 70 %
        p.suspension[axle][6] = 0x46;
        p.suspension[axle][8] = 0x1E;
        p.suspension[axle][10] = 0x46;
    }
    p.steerLimitCount = 6;
    p.slipRatioNegCount = p.slipRatioPosCount = p.slipRatioNegCountRear = p.slipRatioPosCountRear = 6;
    p.slipAngleCount = p.slipAngleCountRear = 8;
    p.loadGripCount = p.loadGripCountRear = 4;
    p.camberGripCount = p.camberGripCountRear = 4;
    static constexpr uint8_t kCamberGripXs[4] = {0, 0x82, 0xA4, 0xFF};
    static constexpr uint8_t kCamberGripYs[4] = {0xC8, 0xAC, 0xA6, 0x94};
    std::memcpy(p.camberGripXs, kCamberGripXs, 4);
    std::memcpy(p.camberGripYs, kCamberGripYs, 4);
    std::memcpy(p.camberGripXsRear, kCamberGripXs, 4);
    std::memcpy(p.camberGripYsRear, kCamberGripYs, 4);
    p.steerMaxRateCode = 9;

    // ---- 0x80017E74 at race start: lengths from the model's LOD 0 bounding box, tracks from the wheel positions
    {
        const int32_t shift = int32_t(body.scaleShift) - 16;
        auto scaled = [shift](int16_t v) { return shift >= 0 ? int32_t(v) << shift : int32_t(v) >> -shift; };
        p.frontLength = int16_t((uint32_t(-scaled(body.bboxFront)) * 125u) >> 9); // model units (1/4096 m) -> mm
        p.rearLength = int16_t((uint32_t(scaled(body.bboxRear)) * 125u) >> 9);
        p.frontTrack = TrackFromWheel(p.rimCode[0], body.wheelLateralFront);
        p.rearTrack = TrackFromWheel(p.rimCode[1], body.wheelLateralRear);
    }
    return p;
}

sim::CarParams BuildCarParams(const GtfsVolume& vol, const CarParamTables& tables, CarConfig& config) {
    const RacingModifyRow& rm = tables.RowAs<RacingModifyRow>(kTableRacingModify, config.racingModify);
    const CarModel model = ParseCarModel(vol.Read("carobj/" + UnpackCarId(rm.modelId) + ".cdo"));
    return BuildCarParams(tables, config, BodyDimensionsOf(model));
}

} // namespace gt2
