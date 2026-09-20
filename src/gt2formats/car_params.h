#pragma once
// Car parameter tables (GTDT "carparam" files) and the native builder of the 0x1C0-byte race record
// (sim::CarParams) that the game shell assembles for every car of a race.
//
// Everything here was derived from the bytes of US Simulation v1.2 (EXE SHA-1 3030aa27...) and from our own
// disassembly of its record builder: 0x800771AC(config, record) -> 0x800763E8 (row lookup, EXE triples at
// 0x80092B1C) -> 0x80077214 (field mapping driven by the mapping tables at 0x80092CA4.., interpreted by
// 0x80077634), plus 0x80017E74 which derives the lengths / tracks from the car model at race start.
// See docs/formats/car_params.md for the layouts with provenance. Offsets in comments are hexadecimal.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "game/sim/car_setup.h"
#include "gt2formats/car_model.h"

namespace gt2 {

class GtfsVolume;

// ---------------------------------------------------------------- GTDT container

// "GTDT" file: u32 magic, u16 0x6C, u16 entryCount, entryCount x { u32 offset, u32 size }. The first
// entryCount / 2 entries are the row tables; the loader (0x80076AE8 / 0x80076B74) turns each entry into
// { pointer, u16 rowSize, u16 rowCount = size / rowSize } with the row sizes below (EXE table 0x80092414).
struct GtdtEntry {
    uint32_t offset = 0;
    uint32_t size = 0;
};

class GtdtFile {
public:
    explicit GtdtFile(std::vector<uint8_t> data);
    size_t EntryCount() const { return entries_.size(); }
    const GtdtEntry& Entry(size_t i) const { return entries_.at(i); }
    std::span<const uint8_t> Bytes(size_t i) const;

private:
    std::vector<uint8_t> data_;
    std::vector<GtdtEntry> entries_;
};

// ---------------------------------------------------------------- the car parameter tables

// Table indices of carparam/usa_gtmode_data.dat (and of arcade_data.dat, which shares the layout). The
// names are ours, from what the record builder does with the rows.
enum CarParamTable : size_t {
    kTableBrakes = 0,          // 12-byte rows, one per car and brake upgrade
    kTableBrakeController = 1, // 16
    kTableSteering = 2,        // 24 (one row)
    kTableChassis = 3,         // 20, one per car
    kTableLightweight = 4,     // 12
    kTableRacingModify = 5,    // 28, one per car plus the racing modification
    kTableEngine = 6,          // 76, one per car
    kTablePortPolish = 7,      // 12
    kTableEngineBalance = 8,   // 12
    kTableDisplacement = 9,    // 12
    kTableComputer = 10,       // 12
    kTableNaTune = 11,         // 12
    kTableTurboKit = 12,       // 20
    kTableDrivetrain = 13,     // 16, one per car
    kTableFlywheel = 14,       // 12
    kTableClutch = 15,         // 16
    kTablePropellerShaft = 16, // 12
    kTableGearbox = 17,        // 36
    kTableSuspension = 18,     // 76
    kTableIntercooler = 19,    // 12
    kTableMuffler = 20,        // 12
    kTableLsd = 21,            // 32
    kTableTyresFront = 22,     // 16
    kTableTyresRear = 23,      // 12
    kTableTyreSize = 24,       // 4
    kTableTyreCompound = 25,   // 64
    kTableSurfaceGrip = 26,    // 8 (one row)
    kTableAsm = 27,            // 16
    kTableTcs = 28,            // 16
    kCarParamTableCount = 31,
};

// Row sizes the loader assigns to the tables (EXE 0x80092414, first 31 entries).
constexpr std::array<uint16_t, kCarParamTableCount> kCarParamRowSizes = {12, 16, 24, 20, 12, 28, 76, 12, 12, 12, 12, 12, 20, 16, 12, 16,
                                                                          12, 36, 76, 12, 12, 32, 16, 12, 4,  64, 8,  16, 16, 8,  72};

// The packed id of the "no part" row (first row of the upgrade tables): "00000".
constexpr uint32_t kNoPartId = 0x01041041u;

// Rows of the tables the record builder reads. Only the fields it uses are named; "raw" bytes keep the
// layouts exact (static_assert). Every row starts with the packed car id (car_info.h) of the car it belongs
// to. Upgrade rows carry a u32 price at +04 and a stage byte at +08 (0 = the car's stock part).
#pragma pack(push, 1)
struct BrakeRow { // table 0
    uint32_t carId;
    uint32_t price;         // +04
    uint8_t stage;          // +08
    uint8_t brakeFront;     // +09  -> CarParams::brakeFront
    uint8_t brakeRear;      // +0A  -> brakeRear
    uint8_t handbrake;      // +0B  -> handbrake
};
struct BrakeControllerRow { // table 1
    uint32_t carId;
    uint32_t price;
    uint8_t stage;          // +08
    uint8_t levelsFront;    // +09  number of balance positions, front
    uint8_t absLowFront;    // +0A  absGain[0] at position 1
    uint8_t absHighFront;   // +0B  ... at the last position
    uint8_t absDefault;     // +0C  written to absGain[0] first, then overridden by the position
    uint8_t levelsRear;     // +0D
    uint8_t absLowRear;     // +0E
    uint8_t absHighRear;    // +0F
};
struct SteeringRow { // table 2
    uint32_t carId;
    uint8_t raw04[6];
    uint8_t steerLimitXs[6]; // +0A  -> steerLimitXs
    uint8_t steerLimitYs[6]; // +10  -> steerLimitYs
    uint8_t steerRateDeg;    // +16  -> steerRateDeg
    uint8_t steerLockDeg;    // +17  -> steerLockDeg
};
struct ChassisRow { // table 3
    uint32_t carId;
    uint8_t frontWeightPercent; // +04  -> frontWeightPercent
    uint8_t byte05;
    uint8_t tyreGripModifier[2]; // +06  -> tyreGripModifier
    uint8_t raw08[2];
    int16_t height;             // +0A  mm -> height
    int16_t wheelbase;          // +0C  mm -> wheelbase
    int16_t weightKg;           // +0E  -> weightKg (before the lightweight / racing-modify factors)
    uint8_t yawInertiaCode;     // +10  -> yawInertiaCode
    uint8_t pitchInertiaCode;   // +11  -> pitchInertiaCode
    uint8_t rollInertiaCode;    // +12  -> rollInertiaCode
    uint8_t byte13;             // +13  -> CarParams::reserved05C (not read by the setup)
};
struct LightweightRow { // table 4
    uint32_t carId;
    uint32_t price;
    uint16_t weightPermille;    // +08  weightKg and yawInertiaCode are multiplied by v / 1000 (1000 in row 0)
    uint8_t rollInertiaPercent; // +0A  rollInertiaCode *= v / 100
    uint8_t byte0B;
};
struct RacingModifyRow { // table 5
    uint32_t carId;
    uint32_t price;             // +04  0 for the car's stock row
    uint32_t modelId;           // +08  packed id of the body model (the car's own id in the stock row)
    uint8_t yawInertiaPercent;  // +0C  yawInertiaCode *= v / 100; weightKg *= v / 100 (after the loop)
    uint8_t rollInertiaPercent; // +0D  rollInertiaCode *= v / 100
    uint8_t byte0E;
    uint8_t dragCoefficient100; // +0F  -> dragCoefficient100
    uint8_t raw10[2];
    uint8_t downforceFront;     // +12  -> downforce[0] (then overridden by CarConfig::downforce)
    uint8_t raw13[2];
    uint8_t downforceRear;      // +15  -> downforce[1]
    int16_t frontTrack;         // +16  -> frontTrack (overridden from the model at race start)
    int16_t rearTrack;          // +18  -> rearTrack
    int16_t width;              // +1A  mm -> width
};
struct EngineRow { // table 6
    uint32_t carId;
    uint8_t raw04[6];
    uint16_t word0A;            // +0A  copied back into CarConfig::engineWord by the builder (not in the record)
    uint16_t torque[16];        // +0C  -> torque
    uint8_t raw2C[10];
    uint8_t torqueMultiplier100; // +36  -> torqueMultiplier1000 = v * 10 (unless CarConfig::flags & 1)
    uint8_t downshiftFloorRpm10; // +37  -> downshiftFloorRpm10
    uint8_t idleRpm10;          // +38  -> idleRpm10
    uint8_t revLimitRpm100;     // +39  -> revLimitRpm100
    uint8_t upshiftRpm100;      // +3A  -> upshiftRpm100
    uint8_t torqueRpm100[16];   // +3B  -> torqueRpm100
    uint8_t torquePointCount;   // +4B  -> torquePointCount
};
// Tables 7 (port polish), 9 (displacement), 10 (computer), 19 (intercooler), 20 (muffler): only the power
// gain is used; the muffler's byte08 is copied into CarConfig::exhaustByte.
struct PowerPartRow {
    uint32_t carId;
    uint32_t price;
    uint8_t byte08;             // +08  stage (muffler: also the exhaust sound byte of the config)
    uint8_t powerPercent;       // +09  power multiplier gain, % (255 = x5.19)
    uint8_t raw0A[2];
};
struct EngineTuneRow { // tables 8 (engine balance) and 11 (NA tune)
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t rpmShift100;        // +09  added to all 16 torqueRpm100 samples
    uint8_t revLimitShift100;   // +0A  added to revLimitRpm100 (NA tune: also to upshiftRpm100)
    uint8_t powerPercent;       // +0B  power multiplier gain, %
};
struct TurboKitRow { // table 12
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t turboBoostCap10;    // +09  -> turboBoostCap10; non-zero marks a turbo (CarConfig::flags |= 2)
    uint8_t turboBoost10;       // +0A  -> turboBoost10 (then overridden by CarConfig::turbo)
    uint8_t turboSpoolRpm100;   // +0B  -> turboSpoolRpm100
    uint8_t turboSpoolRate10;   // +0C  -> turboSpoolRate10
    uint8_t turboBoost10Second; // +0D  -> turboBoost10Second
    uint8_t turboSpoolRpm100Second; // +0E
    uint8_t turboSpoolRate10Second; // +0F
    uint8_t revLimitShift100;   // +10  added to revLimitRpm100
    uint8_t upshiftShift100;    // +11  added to upshiftRpm100
    uint8_t powerPercent;       // +12  power multiplier gain, %
    uint8_t powerPercentFirst;  // +13  -> powerPercent (power at the first rpm sample)
};
struct DrivetrainRow { // table 13
    uint32_t carId;
    uint8_t raw04[3];
    uint8_t centreSplitPercent; // +07  -> centreSplitPercent
    uint8_t driveType;          // +08  -> driveType
    uint8_t fourWheelType;      // +09  -> fourWheelType
    uint8_t engineBrake;        // +0A  -> engineBrake
    uint8_t wheelInertia[2];    // +0B  -> wheelInertia
    uint8_t engineInertia;      // +0D  -> engineInertia
    uint8_t axleInertiaCode[2]; // +0E  -> axleInertiaCode
};
struct FlywheelRow { // table 14
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t engineBrakePercent; // +09  engineBrake *= v / 100
    uint8_t engineInertiaPercent; // +0A  engineInertia *= v / 100
    uint8_t wheelInertiaPercent; // +0B  wheelInertia[0] and [1] *= v / 100
};
struct ClutchRow { // table 15
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t engineBrakePercent; // +09  engineBrake *= v / 100
    uint8_t engineInertiaPercent; // +0A  engineInertia *= v / 100
    uint8_t wheelInertiaFrontPercent; // +0B  wheelInertia[0] *= v / 100
    uint8_t wheelInertiaRearPercent; // +0C  wheelInertia[1] *= v / 100
    uint8_t byte0D;             // +0D  -> CarParams::reserved033 (not read by the setup)
    uint8_t raw0E[2];
};
struct PropellerShaftRow { // table 16
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t engineBrakePercent; // +09  engineBrake *= v / 100
    uint8_t wheelInertiaFrontPercent; // +0A  wheelInertia[0] *= v / 100; axleInertiaCode[0], [1] *= v / 100
    uint8_t wheelInertiaRearPercent; // +0B  wheelInertia[1] *= v / 100
};
struct GearboxRow { // table 17
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t gearCount;          // +09  -> gearCount
    int16_t gearRatio[8];       // +0A  -> gearRatio (then overridden by CarConfig::gearRatio)
    int16_t finalDrive;         // +1A  -> finalDrive (overridden by CarConfig::finalDrive)
    uint8_t raw1C[4];
    uint8_t gearAutoSet;        // +20  -> gearAutoSet (cleared when CarConfig::flags & 0x80)
    uint8_t gearAutoFinal;      // +21  -> gearAutoFinal (overridden by CarConfig::gearAutoFinal)
    uint8_t raw22[2];
};
// Suspension rows carry (position count, value at position 1, value at the last position, default) groups
// for the damper forces and the anti-roll bars; the record gets the value interpolated at the config's
// position (Interpolate below).
struct SuspensionRow { // table 18
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t raw09[2];
    uint8_t camberFront10;      // +0B  -> camberFront10 (overridden by CarConfig::camber10)
    uint8_t raw0C[2];
    uint8_t camberRear10;       // +0E  -> camberRear10
    uint8_t raw0F[6];
    uint8_t rideHeightFront;    // +15  -> rideHeightMm[0] (overridden by CarConfig::rideHeightMm)
    uint8_t raw16[2];
    uint8_t rideHeightRear;     // +18  -> rideHeightMm[1]
    uint8_t bumpTravelMm[2];    // +19  -> bumpTravelMm
    uint8_t droopTravelMm[2];   // +1B  -> droopTravelMm
    uint8_t raw1D[2];
    uint8_t springFront;        // +1F  -> suspension[0].springCode (overridden by CarConfig::springCode)
    uint8_t raw20[2];
    uint8_t springRear;         // +22  -> suspension[1].springCode
    uint8_t damperScaleDivisor[2]; // +23  -> damperScaleDivisor (overridden by the config)
    uint8_t bumpStopFront;      // +25  -> suspension[0].bumpStopCode
    uint8_t bumpStopRear;       // +26  -> suspension[1].bumpStopCode
    uint8_t bumpLevelsFront;    // +27  positions of the front bump dampers
    uint8_t bumpLowFront[3];    // +28  low-speed bump force: first, last, default -> suspension[0].bumpLowForce
    uint8_t bumpHighFront[3];   // +2B  high-speed bump force -> bumpHighForce
    uint8_t reboundLevelsFront; // +2E
    uint8_t reboundLowFront[3]; // +2F  -> reboundLowForce
    uint8_t reboundHighFront[3]; // +32  -> reboundHighForce
    uint8_t bumpLevelsRear;     // +35
    uint8_t bumpLowRear[3];     // +36
    uint8_t bumpHighRear[3];    // +39
    uint8_t reboundLevelsRear;  // +3C
    uint8_t reboundLowRear[3];  // +3D
    uint8_t reboundHighRear[3]; // +40
    uint8_t antiRollLevelsFront; // +43
    uint8_t antiRollFront[3];   // +44  -> suspension[0].antiRollCode
    uint8_t antiRollLevelsRear; // +47
    uint8_t antiRollRear[3];    // +48  -> suspension[1].antiRollCode
    uint8_t byte4B;
};
struct LsdRow { // table 21
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t raw09[3];
    int8_t diffTypeFront;       // +0C  -> diffTypeCode[0]
    uint8_t diffInitialFront[3]; // +0D  default, raw, raw -> diffInitialTorque[0] (overridden by the config)
    uint8_t diffAccelFront[3];  // +10  -> diffAccel[0]
    uint8_t diffDecelFront[3];  // +13  -> diffDecel[0]
    int8_t diffTypeRear;        // +16  -> diffTypeCode[1]
    uint8_t diffInitialRear[3]; // +17  -> diffInitialTorque[1]
    uint8_t diffAccelRear[3];   // +1A  -> diffAccel[1]
    uint8_t diffDecelRear[3];   // +1D  -> diffDecel[1]
};
struct TyresFrontRow { // table 22
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t byte09;
    uint16_t tyreSizeRow;       // +0A  row of table 24
    uint16_t compoundRow;       // +0C  row of table 25
    uint16_t surfaceGripRow;    // +0E  row of table 26
};
struct TyresRearRow { // table 23
    uint32_t carId;
    uint8_t stage;              // +04
    uint8_t byte05;
    uint16_t tyreSizeRow;       // +06  row of table 24
    uint16_t compoundRow;       // +08  row of table 25
    uint8_t raw0A[2];
};
struct TyreSizeRow { // table 24
    uint8_t tyreWidthCode;      // +00  -> tyreWidthCode[axle]
    uint8_t rimCode;            // +01  -> rimCode[axle]
    uint8_t tyreAspectCode;     // +02  -> tyreAspectCode[axle]
    uint8_t byte03;
};
struct TyreCompoundRow { // table 25
    uint8_t gripPercent;        // +00  -> tyreGripPercent[axle]
    uint8_t raw01[3];
    uint8_t loadGripXs[4];      // +04  -> loadGripXs
    uint8_t loadGripYs[4];      // +08  -> loadGripYs
    uint8_t slipAngleXs[8];     // +0C  -> slipAngleXs
    uint8_t slipAngleYs[8];     // +14  -> slipAngleYs
    uint8_t slipRatioNegXs[6];  // +1C  -> slipRatioNegXs
    uint8_t slipRatioNegYs[6];  // +22  -> slipRatioNegYs
    uint8_t slipRatioNegYs2[6]; // +28  -> slipRatioNegYs2
    uint8_t slipRatioPosXs[6];  // +2E  -> slipRatioPosXs
    uint8_t slipRatioPosYs[6];  // +34  -> slipRatioPosYs
    uint8_t slipRatioPosYs2[6]; // +3A  -> slipRatioPosYs2
};
struct SurfaceGripRow { // table 26
    uint8_t surfaceGripPercent[7]; // +00  -> surfaceGripPercent[0..6]
    uint8_t byte07;
};
struct AsmRow { // table 27
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t levels;             // +09  positions
    uint8_t asmYawGain100;      // +0A  -> asmYawGain100
    uint8_t asmYawThreshold[3]; // +0B  default, first, last -> asmYawThreshold100 (interpolated at the position)
    uint8_t raw0E[2];
};
struct TcsRow { // table 28
    uint32_t carId;
    uint32_t price;
    uint8_t stage;              // +08
    uint8_t byte09;
    uint8_t levels;             // +0A  positions
    uint8_t tcsGain10;          // +0B  -> tcsGain10
    uint8_t tcsFalloffGain[3];  // +0C  default, first, last -> tcsFalloffGain10 (interpolated at the position)
    uint8_t tcsSteerGain100;    // +0F  -> tcsSteerGain100
};
#pragma pack(pop)
static_assert(sizeof(BrakeRow) == 12 && sizeof(BrakeControllerRow) == 16 && sizeof(SteeringRow) == 24 && sizeof(ChassisRow) == 20);
static_assert(sizeof(LightweightRow) == 12 && sizeof(RacingModifyRow) == 28 && sizeof(EngineRow) == 76 && sizeof(PowerPartRow) == 12);
static_assert(sizeof(EngineTuneRow) == 12 && sizeof(TurboKitRow) == 20 && sizeof(DrivetrainRow) == 16 && sizeof(FlywheelRow) == 12);
static_assert(sizeof(ClutchRow) == 16 && sizeof(PropellerShaftRow) == 12 && sizeof(GearboxRow) == 36 && sizeof(SuspensionRow) == 76);
static_assert(sizeof(LsdRow) == 32 && sizeof(TyresFrontRow) == 16 && sizeof(TyresRearRow) == 12 && sizeof(TyreSizeRow) == 4);
static_assert(sizeof(TyreCompoundRow) == 64 && sizeof(SurfaceGripRow) == 8 && sizeof(AsmRow) == 16 && sizeof(TcsRow) == 16);
static_assert(offsetof(EngineRow, torqueRpm100) == 0x3B && offsetof(SuspensionRow, antiRollRear) == 0x48 && offsetof(LsdRow, diffDecelRear) == 0x1D);

class CarParamTables {
public:
    // `gtdt` = the decompressed carparam/usa_gtmode_data.dat (the file the record builder used in the attract
    // race: file index 103 of language 1 in the EXE's file table 0x800925A4).
    explicit CarParamTables(std::vector<uint8_t> gtdt);
    static CarParamTables Load(const GtfsVolume& vol, const std::string& path = "carparam/usa_gtmode_data.dat");

    size_t RowCount(size_t table) const;
    std::span<const uint8_t> Row(size_t table, size_t index) const; // throws std::out_of_range
    template <typename T>
    const T& RowAs(size_t table, size_t index) const {
        static_assert(sizeof(T) <= 76);
        const std::span<const uint8_t> row = Row(table, index);
        if (row.size() != sizeof(T)) throw std::out_of_range("car params: row size mismatch");
        return *reinterpret_cast<const T*>(row.data());
    }
    // Rows whose leading packed id equals `carId`, in table order.
    std::vector<uint16_t> RowsOfCar(size_t table, uint32_t carId) const;

private:
    GtdtFile file_;
};

// ---------------------------------------------------------------- the car configuration

// The per-car configuration the shell keeps for a garage / race car: the row of every part table plus the
// settings chosen in the menus (0x84 bytes). The builder 0x800771AC takes a pointer to it. In a replay file
// (arcade/demofile_us.gmr) the six race cars are 0xD0-byte slots at kReplayCarSlotOffset: { u32 carId; u32;
// CarConfig; u8[4]; char name[64] }.
#pragma pack(push, 1)
struct CarConfig {
    uint32_t word00;             // +00  copied into the builder's working struct, otherwise unused (0x300 / 0x100 in the replay)
    uint16_t brakes;             // +04  row of table 0
    uint16_t brakeController;    // +06  table 1
    uint16_t steering;           // +08  table 2
    uint16_t chassis;            // +0A  table 3
    uint16_t engine;             // +0C  table 6
    uint16_t drivetrain;         // +0E  table 13
    uint16_t gearbox;            // +10  table 17
    uint16_t suspension;         // +12  table 18
    uint16_t lsd;                // +14  table 21
    uint16_t tyresFront;         // +16  table 22
    uint16_t tyresRear;          // +18  table 23
    uint16_t lightweight;        // +1A  table 4
    uint16_t racingModify;       // +1C  table 5
    uint16_t portPolish;         // +1E  table 7
    uint16_t engineBalance;      // +20  table 8
    uint16_t displacement;       // +22  table 9
    uint16_t computer;           // +24  table 10
    uint16_t naTune;             // +26  table 11
    uint16_t turboKit;           // +28  table 12
    uint16_t flywheel;           // +2A  table 14
    uint16_t clutch;             // +2C  table 15
    uint16_t propellerShaft;     // +2E  table 16
    uint16_t muffler;            // +30  table 20
    uint16_t intercooler;        // +32  table 19
    uint16_t activeStability;    // +34  table 27 (ASM)
    uint16_t tractionControl;    // +36  table 28 (TCS)
    uint16_t word38;             // +38  not read by the builder (10 for the AI cars of the attract race, 0 for the player)
    uint16_t torqueMultiplier100; // +3A  replaces the engine row's multiplier when flags & 1 (AI cars: 100..136)
    int16_t gearRatio[8];        // +3C  -> gearRatio
    int16_t finalDrive;          // +4C  -> finalDrive
    uint8_t gearAutoFinal;       // +4E  -> gearAutoFinal
    uint8_t byte4F;
    uint8_t brakeBalance[2];     // +50  positions (front, rear) into the brake controller's abs range
    uint8_t downforce[2];        // +52  -> downforce
    uint8_t turboBoost10;        // +54  -> turboBoost10
    uint8_t turboSpoolRpm100;    // +55  -> turboSpoolRpm100
    uint8_t turboSpoolRate10;    // +56  -> turboSpoolRate10
    uint8_t turboBoost10Second;  // +57  -> turboBoost10Second
    uint8_t turboSpoolRpm100Second; // +58
    uint8_t turboSpoolRate10Second; // +59
    uint8_t camber10[2];         // +5A  -> camberFront10, camberRear10
    uint8_t rideHeightMm[2];     // +5C  -> rideHeightMm
    uint8_t toeCode[2];          // +5E  -> toeCode (128 = 0)
    uint8_t springCode[2];       // +60  -> suspension[axle].springCode
    uint8_t damperScaleDivisor[2]; // +62  -> damperScaleDivisor
    uint8_t damperLevel[8];      // +64  positions: bump low, bump high, rebound low, rebound high (front), the same (rear)
    uint8_t antiRollLevel[2];    // +6C  positions (front, rear)
    uint8_t diffInitial[2];      // +6E  -> diffInitialTorque (front, rear)
    uint8_t diffAccel[2];        // +70  -> diffAccel
    uint8_t diffDecel[2];        // +72  -> diffDecel
    uint8_t asmLevel;            // +74  position into the ASM row's threshold range
    uint8_t tcsLevel;            // +75  position into the TCS row's falloff range
    uint16_t engineWord;         // +76  written by the builder: EngineRow::word0A
    uint8_t exhaustByte;         // +78  written by the builder: muffler PowerPartRow::byte08, + 4 with a turbo kit
    uint8_t byte79;              // +79  (29 for the player, 50 for the AI cars of the attract race)
    uint8_t flags;               // +7A  bit 0: torqueMultiplier100 applies; bit 1: set by the builder when a turbo kit is fitted; bit 7: gearAutoSet is forced off
    uint8_t byte7B;
    uint16_t word7C, word7E, word80; // +7C..+81 copied into the builder's working struct, unused by it
    uint16_t pad82;
};
#pragma pack(pop)
static_assert(sizeof(CarConfig) == 0x84);
static_assert(offsetof(CarConfig, gearRatio) == 0x3C && offsetof(CarConfig, brakeBalance) == 0x50 && offsetof(CarConfig, flags) == 0x7A);

// Replay file (arcade/demofile_us.gmr, also the format of saved replays): the shell's race block image. The
// six car slots start at 0x15DC (RAM 0x801D58B8 when the file is loaded at 0x801D42DC), 0xD0 bytes each.
constexpr size_t kReplayCarSlotOffset = 0x15DC;
constexpr size_t kReplayCarSlotStride = 0xD0;
constexpr size_t kReplayCarSlotCount = 6;
constexpr size_t kReplayCarConfigOffset = 8; // CarConfig inside a slot (the packed car id is at +0)

struct ReplayCar {
    uint32_t carId = 0;
    CarConfig config{};
    std::string name;
};
ReplayCar ReplayCarAt(std::span<const uint8_t> replay, size_t slot);

// The car's stock configuration: the rows of its own parts (price 0) and no upgrades, settings at the rows'
// defaults (as the six attract-race cars have them). Throws when the car is not in the tables.
CarConfig StockCarConfig(const CarParamTables& tables, uint32_t carId);

// ---------------------------------------------------------------- the record builder

// Body-model inputs of 0x80017E74: the record's lengths and tracks come from the .cdo, not from the tables.
struct CarBodyDimensions {
    int16_t bboxFront = 0;       // LOD 0 bounding box min z (negative, model units)
    int16_t bboxRear = 0;        // LOD 0 bounding box max z
    int16_t scaleShift = 0;      // LOD 0 scale field; model units are shifted by (scale - 16)
    int16_t wheelLateralFront = 0; // first s16 of the front-left wheel entry (0x20 of the .cdo)
    int16_t wheelLateralRear = 0;  // first s16 of the rear-left wheel entry (0x30)
};
CarBodyDimensions BodyDimensionsOf(const CarModel& model);

// 0x800771AC + 0x80017E74: the record as the physics setup (sim::SetupCar) receives it. `config` gets the two
// bytes the builder writes back (engineWord, exhaustByte, flags bit 1).
sim::CarParams BuildCarParams(const CarParamTables& tables, CarConfig& config, const CarBodyDimensions& body);

// Convenience: reads carobj/<modelId>.cdo of the configured racing-modify row's model.
sim::CarParams BuildCarParams(const GtfsVolume& vol, const CarParamTables& tables, CarConfig& config);

} // namespace gt2
