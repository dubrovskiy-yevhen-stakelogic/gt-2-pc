#pragma once
// Garage and money rules of the GT-mode career, ported from the original's routines (US Simulation v1.2, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; GT-mode menus = GT2.OVL member 4 at 0x80010000, "ovl4"). Every routine
// works on the original's layouts (career_state.h, TuneSheet below) so that gt2verify runs it on a RAM image next to
// the original (tools/gt2verify/verify_career.cpp). Rules and evidence: docs/research/menus_gtmode.md section 5.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "game/career/career_state.h"
#include "game/sim/car_setup.h"
#include "gt2formats/car_params.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/overlay_data.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2::career {

// The disc data the rules read: the GT-mode car tables (carparam/usa_gtmode_data.dat; the original's pointers
// 0x8009286C / 0x8009287C), the car directory (.carinfoa, 0x801C93D8: paint lists, region mask, names), the event
// tables (carparam/usa_gtmode_race.dat, 0x80092870 / 0x80092E70) and the list of part kinds that race cars come with
// (ovl4 0x80050B68, s16 list ended by a negative value).
struct CareerData {
    CarParamTables tables;
    CarInfoDirectory cars;
    GtModeRaceData race;
    std::vector<int16_t> raceCarKinds;
    GuestImage exe;  // SCUS_944.88: the mapping tables of 0x80077634 (0x800928xx)
    GuestImage ovl4; // GT2.OVL member 4 (the GT-mode menus): their lists (0x80050B68, 0x80050D1C, ...)
    // Branches whose exact result is not established throw std::logic_error when strict (gt2verify counts them as
    // not ported); the game sets it false and gets the native approximation named at the branch.
    bool strict = true;
    // Runtime state the menus' gear generator reads (0x80074B38 -> 0x80076070): the dirt flag of the .crsinfo entry
    // of the course index 0x800AF230, i.e. of the last race run (false in a fresh session).
    bool menuDirtCourse = false;

    static CareerData Load(const DiscImage& disc, const GtfsVolume& vol);
};

// ---------------------------------------------------------------- money and slots (EXE / ovl4, no table access)

// 0x8005E7B0(garage, amount): money += amount, clamped to 0..99,999,999.
void AddMoney(GarageBlock& g, int32_t amount);
// 0x80017914(-, price, player): 1 when money >= price and the garage is not full, -1 too little money, -2 full.
int32_t CanBuy(const GarageBlock& g, int32_t price);
// 0x8005E7F0(garage, slot): appends a prepared car (count <= 99); 1 = added, 0 = garage full.
int32_t AddPreparedCar(GarageBlock& g, const GarageCar& car);
// 0x8001EDAC(garage, index): removes a car, keeps the current-car index pointing at the same car (-1 when removed).
void RemoveCar(GarageBlock& g, int32_t index);
// 0x8001EF10(garage, from, to): moves a car, the others shift; the current-car index follows.
void MoveCar(GarageBlock& g, int32_t from, int32_t to);
// "select current car": ovl4 writes the index into the garage's current-car field.
void SelectCar(GarageBlock& g, int16_t index);

// ---------------------------------------------------------------- parts owned (bit set at +0x9A)

bool PartOwned(const GarageCar& car, int32_t kind);                  // 0x8005E874(car, kind)
void SetPartBit(uint8_t* bits, int32_t kind);                        // 0x8005E900(kind, bits)
void AddPart(GarageCar& car, int32_t kind, int32_t price);           // 0x8005E8B0(car, kind, price): bit + value += price

// ---------------------------------------------------------------- table lookups (EXE 0x800763xx..0x80077xxx)

// 0x80077E80 + 0x80076818: the first row of `table` belonging to `carId`, -1 when none (binary search on the
// u32 at +0 as the original does it, then back to the first equal row).
int32_t FirstRowOfCar(const CarParamTables& t, size_t table, uint32_t carId);
// 0x80076748(table, stage, stageOffset, carId): the first row of the car whose byte at `stageOffset` equals `stage`
// and whose byte +4 is not 0; -1 when none.
int32_t RowWithStage(const CarParamTables& t, size_t table, uint8_t stage, size_t stageOffset, uint32_t carId);
// 0x80076570(kind, carId, &price): the row of part `kind` (the menus' numbering 0..0x31, table / stage in the
// switch of 0x80076570) for the car, falling back to the generic car "00000"; -1 when none. `price` gets the row's
// u32 +4 (0 when none).
int32_t PartRow(const CarParamTables& t, int32_t kind, uint32_t carId, uint32_t* price = nullptr);
// 0x80076500(carId, stage, &price): the racing-modification row (table 5) with stage byte +0E == stage.
int32_t RacingModifyStageRow(const CarParamTables& t, uint32_t carId, uint8_t stage, uint32_t* price = nullptr);
// 0x800177D4(carId): catalogue price (table 30 +0x44), 0 when the car is not in the catalogue.
int32_t CataloguePrice(const CarParamTables& t, uint32_t carId);
// 0x80017B04(carId, kind): the part's price, -1 when the car has no such part.
int32_t PartPrice(const CarParamTables& t, uint32_t carId, int32_t kind);
// 0x80076240(table, row, config): the settings a part row brings into the configuration (mapping tables
// 0x800928A0.. through 0x80077634); tables other than 1, 5, 12, 13, 17, 18, 21, 27, 28 bring none.
void ApplyPartRowSettings(const GuestImage& exe, size_t table, std::span<const uint8_t> row, CarConfig& config);

// ---------------------------------------------------------------- the tune sheet (RAM 0x801DA4B8)

// The setting-screen object of the tune shop / purchase: the car's configuration and, for every part kind, the rows
// the car can have per stage (its own row at the row's stage, the upgrades of 0x80076570 at their slots) with their
// row indices, and the current stage of each kind at +0x17B0. Filled by 0x80015404 / 0x80015428 / 0x80016C5C.
#pragma pack(push, 1)
struct TuneSheet {
    CarConfig config;                       // +0000
    uint8_t unknown0084[0x988 - 0x84];      // (table 29 slots at +0x688 used by 0x8005EAC0 kind 5; not filled here)
    SuspensionRow suspension[4];            // +0988
    uint32_t suspensionRow[4];              // +0AB8
    BrakeRow brakes[2];                     // +0AC8
    uint32_t brakesRow[2];                  // +0AE0
    BrakeControllerRow brakeController[2];  // +0AE8
    uint32_t brakeControllerRow[2];         // +0B08
    TyresFrontRow tyresFront[10];           // +0B10 (slots 8 / 9 = copies of the car's own row)
    uint32_t tyresFrontRow[10];             // +0BB0
    TyreSizeRow tyreSizeFront[10];          // +0BD8
    TyreCompoundRow compoundFront[10];      // +0C00 (slots 8 / 9 not written)
    SurfaceGripRow gripFront[10];           // +0E80
    TyresRearRow tyresRear[10];             // +0ED0
    uint32_t tyresRearRow[10];              // +0F48
    TyreSizeRow tyreSizeRear[10];           // +0F70
    TyreCompoundRow compoundRear[10];       // +0F98
    GearboxRow gearbox[4];                  // +1218
    uint32_t gearboxRow[4];                 // +12A8
    ClutchRow clutch[4];                    // +12B8
    uint32_t clutchRow[4];                  // +12F8
    FlywheelRow flywheel[4];                // +1308
    uint32_t flywheelRow[4];                // +1338
    PropellerShaftRow propellerShaft[2];    // +1348
    uint32_t propellerShaftRow[2];          // +1360
    PowerPartRow computer[2];               // +1368
    uint32_t computerRow[2];                // +1380
    PowerPartRow portPolish[2];             // +1388
    uint32_t portPolishRow[2];              // +13A0
    EngineTuneRow engineBalance[2];         // +13A8
    uint32_t engineBalanceRow[2];           // +13C0
    EngineTuneRow naTune[4];                // +13C8
    uint32_t naTuneRow[4];                  // +13F8
    PowerPartRow displacement[2];           // +1408
    uint32_t displacementRow[2];            // +1420
    TurboKitRow turboKit[5];                // +1428
    uint32_t turboKitRow[5];                // +148C
    PowerPartRow intercooler[3];            // +14A0
    uint32_t intercoolerRow[3];             // +14C4
    PowerPartRow muffler[4];                // +14D0
    uint32_t mufflerRow[4];                 // +1500
    LightweightRow lightweight[4];          // +1510
    uint32_t lightweightRow[4];             // +1540
    RacingModifyRow racingModify[5];        // +1550
    uint32_t racingModifyRow[5];            // +15DC
    AsmRow activeStability[2];              // +15F0
    uint32_t activeStabilityRow[2];         // +1610
    TcsRow tractionControl[2];              // +1618
    uint32_t tractionControlRow[2];         // +1638
    LsdRow lsd[6];                          // +1640
    uint32_t lsdRow[6];                     // +1700
    SteeringRow steering;                   // +1718
    uint32_t steeringRow;                   // +1730
    ChassisRow chassis;                     // +1734
    uint32_t chassisRow;                    // +1748
    EngineRow engine;                       // +174C
    uint32_t engineRow;                     // +1798
    DrivetrainRow drivetrain;               // +179C
    uint32_t drivetrainRow;                 // +17AC
    int16_t stage[27];                      // +17B0 current stage per kind (TuneKind)
    uint16_t pad17E6;
};
#pragma pack(pop)
static_assert(sizeof(TuneSheet) == 0x17E8);
static_assert(offsetof(TuneSheet, suspension) == 0x988 && offsetof(TuneSheet, tyresFront) == 0xB10 && offsetof(TuneSheet, compoundRear) == 0xF98);
static_assert(offsetof(TuneSheet, gearbox) == 0x1218 && offsetof(TuneSheet, turboKit) == 0x1428 && offsetof(TuneSheet, racingModify) == 0x1550);
static_assert(offsetof(TuneSheet, lsd) == 0x1640 && offsetof(TuneSheet, drivetrainRow) == 0x17AC && offsetof(TuneSheet, stage) == 0x17B0);
constexpr uint32_t kTuneSheetAddress = 0x801DA4B8u;

// Index of TuneSheet::stage (the kind argument of 0x8005EAC0).
enum TuneKind : int {
    kTuneSuspension = 0, kTuneBrakes = 1, kTuneBrakeController = 2, kTuneTyresFront = 3, kTuneTyresRear = 4, kTuneProfile = 5,
    kTuneGearbox = 6, kTuneClutch = 7, kTuneFlywheel = 8, kTunePropellerShaft = 9, kTuneComputer = 10, kTunePortPolish = 11,
    kTuneEngineBalance = 12, kTuneNaTune = 13, kTuneDisplacement = 14, kTuneTurbo = 15, kTuneIntercooler = 16, kTuneMuffler = 17,
    kTuneLightweight = 18, kTuneRacingModify = 19, kTuneAsm = 20, kTuneTcs = 21, kTuneLsd = 22, kTuneSteering = 23,
    kTuneChassis = 24, kTuneEngine = 25, kTuneDrivetrain = 26,
};

void ClearTuneSheet(TuneSheet& s);                                            // 0x80015404: memset 0xFF
void LoadTuneSheet(TuneSheet& s, uint32_t carId, const CarParamTables& t);   // 0x80015428
void SetTuneConfig(TuneSheet& s, const CarConfig& config, const CarParamTables& t); // 0x80016C5C
// 0x8005EAC0(sheet, kind, stage): selects the stage's row of `kind` into the sheet's configuration (+ the row's
// settings). Kind 6 with a row whose gearAutoSet (+0x20) is set regenerates the gear ratios (0x8005E93C: the part
// loop's record, 0x80074B38 at gearAutoFinal x 10 km/h into an 8-entry stack buffer of which only reverse..top gear
// are written - the rest is whatever the stack held). Strict: throws std::logic_error; else reverse..top gear from
// sim::GenerateGearRatios on the record, the other entries keep the row's values.
void SetTuneStage(TuneSheet& s, int32_t kind, int32_t stage, const CareerData& d);
// 0x80016FEC(sheet, config): the stock gearbox stage again, flags |= 0xC0, the configuration copied back.
void FinishTuneConfig(TuneSheet& s, CarConfig& config, const CareerData& d);
// 0x80017750(carId, config): the purchase's pass over the configuration; returns the racing-modification stage of
// the car's own row > 0 (0x801DBC8E, the slot's bit 15).
bool AnalysePurchase(TuneSheet& s, uint32_t carId, CarConfig& config, const CareerData& d);

// ---------------------------------------------------------------- building a garage car

// 0x80075328 + 0x800756BC (0x80075930): the power / torque figures of a record, written to `out` (0x6C bytes: +0 max
// power, +2 its rpm, +4 max torque (kgm x 10), +6 its rpm, +8 rev limit, +0x0A sample count, +0x0C rpm list, +0x2C
// power list, +0x4C torque list); patches the record's last torque sample like the original. Returns out +0.
uint16_t CarPowerFigures(sim::CarParams& record, uint8_t* out);
// 0x800771AC(config, record): the record as the menus build it (without the race start's model dimensions
// 0x80017E74); writes back engineWord / exhaustByte / flags bit 1 into the configuration.
void BuildMenuRecord(const CarParamTables& t, CarConfig& config, sim::CarParams& record);
// 0x800178E4(carId): the catalogue gearbox row's byte +9 (gear count) < 3.
bool GearboxFlag(const CarParamTables& t, uint32_t carId);
// 0x8001781C(carId, bits): race cars (catalogue +0x40 != 0) come with the part kinds of `raceCarKinds` that exist for
// them; returns false (the original's null pointer) for other cars.
bool FittedParts(const CareerData& d, uint32_t carId, std::array<uint8_t, 7>& bits);

// Work area of the record build: the original uses the scratchpad (record at 0x1F800000, figures at 0x1F8001C0).
struct BuildScratch {
    uint8_t* bytes; // >= 0x230 bytes
};
// 0x8001EC0C(slot, carId, modelId, paint, config, value, flag15, flag14, parts)
void BuildGarageCar(GarageCar& slot, uint32_t carId, uint32_t modelId, uint32_t paint, CarConfig config, int32_t value, bool flag15, bool flag14,
                    const uint8_t* parts, const CarParamTables& t, BuildScratch scratch);
// 0x8001EE78(garage, carId, paint, config, value, flag15, flag14, parts): appends (modelId = carId) when count < 100.
bool AppendGarageCar(GarageBlock& g, uint32_t carId, uint32_t paint, const CarConfig& config, int32_t value, bool flag15, bool flag14, const uint8_t* parts,
                     const CarParamTables& t, BuildScratch scratch);

// ---------------------------------------------------------------- menu actions

// 0x8001796C(carId, paint, price, player): buys a new or used car: 1 bought, -1 too little money, -2 garage full.
int32_t BuyCar(GarageBlock& g, uint32_t carId, uint32_t paint, int32_t price, const CareerData& d, TuneSheet& sheet, BuildScratch scratch);
// 0x80017A70(index, player): sells a car for a quarter of its catalogue price (signed division toward zero).
void SellCar(GarageBlock& g, int32_t index, const CarParamTables& t);
// 0x80017B40(index, kind, player): 1 when the part may be bought; -8 no such part, -1 too little money, -3 owned,
// -5 / -6 / -7 the prerequisite kind 0x12 / 0x13 / 0x14 of kind 0x13 / 0x14 / 0x22 is missing.
int32_t PartPurchaseCheck(const GarageBlock& g, int32_t index, int32_t kind, const CarParamTables& t);

// 0x80017C98(index, kind, player): buys a part: the check, then the bit and the price (0x8005E8B0) and money -=
// price (no clamp). Returns the check's code.
int32_t BuyPart(GarageBlock& g, int32_t index, int32_t kind, const CarParamTables& t);

} // namespace gt2::career
