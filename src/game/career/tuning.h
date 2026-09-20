#pragma once
// Fitting parts and the machine settings of a garage car, ported from the original (US Simulation v1.2, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): the GT-mode overlay (GT2.OVL member 4, "ovl4") fits bought parts through
// the menus' tune sheet of the current car (0x800B4490, loaded by 0x800173E8) and writes it back into the garage slot
// (0x80016F10); the settings screens of the race overlay (member 0, "ovl0": 0x80056194 on its sheet 0x8016E894) read
// and write the settings through the EXE's settings library (0x8005FC9C get, 0x8005F9DC set, 0x80060410 default)
// and move a value with the slider 0x80054D10. Evidence and the rules: docs/research/menus_gtmode.md section 8.
#include <array>
#include <cstdint>
#include <string>

#include "game/career/career_state.h"
#include "game/career/garage.h"

namespace gt2::career {

constexpr uint32_t kCarSheetAddress = 0x800B4490u;   // the menus' tune sheet of the current car (0x800173E8)
constexpr uint32_t kRacingBodyAddress = 0x800B5C78u; // s16: the racing-modification body chosen on the fit page (0x800174F4 / 0x80017530)
constexpr uint32_t kSettingsSheetAddress = 0x8016E894u; // the race overlay's sheet of the settings screens

// ---------------------------------------------------------------- lookups

// 0x80077E80(tables, table, id): binary search on the u32 at +0 (unsigned compares); the row found (not necessarily
// the first one of the id), -1 when none.
int32_t SearchRowById(const CarParamTables& t, size_t table, uint32_t id);

// ---------------------------------------------------------------- the record and figures of a tune sheet

// The configuration whose rows are the rows the sheet has selected (the index word of every kind's slot at its
// current stage; tyres at `frontTyreStage` / `rearTyreStage`): building it gives the record 0x8005F410 + 0x80077214
// build from the sheet's slots. Throws std::logic_error when a selected slot is empty (the original would read
// the 0xFF fill of 0x80015404) or a single-row kind (steering, chassis, engine, drivetrain) has a stage != 0 (the
// original then reads past the slot).
CarConfig SheetRowsConfig(const TuneSheet& s, int32_t frontTyreStage, int32_t rearTyreStage);
// 0x8005F410 + 0x80077214 on the sheet: the record of its selected rows and its configuration's settings (no model
// dimensions: lengths 0, tracks of the racing-modification row).
sim::CarParams SheetRecord(const TuneSheet& s, const CarParamTables& t);

struct CarFigures {
    uint16_t weight = 0; // record +0x5A (kg)
    uint16_t power = 0;  // 0x80075930 +0 (PS)
    uint16_t torque = 0; // 0x80075930 +4 (kgm x 10)
    uint8_t drive = 0;   // record +0x8A
};
// 0x8005F958(sheet, out): the sheet's record on the scratchpad (0x1F800000) and its figures (0x1F8001C0).
CarFigures SheetFigures(const TuneSheet& s, const CarParamTables& t, BuildScratch scratch);

// 0x8005E93C(sheet, out): reverse..top gear generated from the sheet's record at gearAutoFinal x 10 (0x80074B38 with
// the auto-set cleared). Only out[0..gearCount] are written; the original's caller then copies all eight entries
// of its stack buffer, so the entries above the top gear are whatever that stack held (docs: the gearbox residue;
// the caller passes the values it wants there, the native choice is "unchanged"). Returns the gear count.
int32_t SheetGearRatios(const TuneSheet& s, const CareerData& d, int16_t out[8]);
// 0x8005E99C(sheet, out, lo, hi, ...): the same from the ratios' auto-set snapshot of the configuration (+0x7B top speed,
// +0x7C final drive, +0x7E / +0x80 tyre stages), then 0x80074E04: the ranges of the gears (lo / hi: 7/8 and 9/8 of
// reverse and first gear, else a third of the way to the neighbouring gears). Returns the gear count.
int32_t SheetGearRanges(const TuneSheet& s, const CareerData& d, int16_t out[8], int16_t lo[8], int16_t hi[8]);
// 0x80074E04(count, ratios, lo, hi).
void GearRanges(int32_t count, const int16_t* ratios, int16_t* lo, int16_t* hi);

// ---------------------------------------------------------------- garage car <-> sheet

// 0x800173E8(index, player): 0x80015404 + 0x80015428 (the car's rows) + 0x80016C5C (its configuration and stages).
void LoadCarSheet(TuneSheet& s, const GarageCar& car, const CarParamTables& t);
// 0x80016F10(slot, sheet): the sheet's configuration back into the garage slot with the figures of 0x8005F958
// (power / torque / weight / drive, bit 15 = racing modification fitted).
void StoreCarSheet(GarageCar& car, const TuneSheet& s, const CarParamTables& t, BuildScratch scratch);

// 0x8001706C(partKind, out): the sheet kind (TuneKind) and stage a bought part (0x80076570 numbering) selects.
struct TuneStage {
    int16_t kind = 0;
    int16_t stage = 0;
};
TuneStage PartTuneStage(int32_t partKind);
// The part kind that selects `stage` of `tuneKind` (the inverse of 0x8001706C; tyres: 0x27 + stage - 1, racing
// modification: 0x22); -1 when no part does (stage 0 = the car's own part).
int32_t PartKindOfStage(int32_t tuneKind, int32_t stage);

// 0x8005F858(sheet): racing-modification bodies available (consecutive filled slots 1..4).
int32_t RacingBodyCount(const TuneSheet& s);
// 0x800174F4: body 1 chosen (`racingBody` = 0x800B5C78), returns its model id (slot +8).
uint32_t FirstRacingBody(const TuneSheet& s, int16_t& racingBody);
// 0x80017530: the next body (wraps to 1 after the last), returns its model id.
uint32_t NextRacingBody(const TuneSheet& s, int16_t& racingBody);

// 0x80017D6C(index, partKind, player, paint): fits an owned part to garage car `index` through `sheet`, which must
// hold that car (0x800173E8 before, as the menus do): -4 the part is not owned, -8 the car has no such part, else
// the sheet stage (tyres 0x27..0x2D: front and rear; racing modification 0x22: body `racingBody` with `paint`, the
// slot's model id becomes the body's) and 0x80016F10; returns 1.
int32_t FitPart(GarageBlock& g, int32_t index, int32_t partKind, uint32_t paint, TuneSheet& sheet, int16_t racingBody, const CareerData& d,
                BuildScratch scratch);
// Convenience: 0x800173E8 then 0x80017D6C (what the tune shop does for the current car).
int32_t FitPartToCar(GarageBlock& g, int32_t index, int32_t partKind, uint32_t paint, int16_t racingBody, const CareerData& d, BuildScratch scratch);
// 0x80018004(index, player, dirtEvent): before an event race (0x80013108): the car's sheet is loaded; dirt tyres
// (part 0x2D) are fitted when the event is a dirt event (0x80019538) and the car owns them, else fitted dirt tyres
// (front or rear stage 7) go back to stage 0 and the slot is written (0x80016F10).
void PrepareRaceTyres(GarageBlock& g, int32_t index, bool dirtEvent, TuneSheet& sheet, const CareerData& d, BuildScratch scratch);
// 0x80021B38(sheet, row): wheels = table 29 row `row` (CarConfig +0x38; +0x00 = row u32 | (row[7] & 0x1F) << 8, or
// row[6] when the row's word is 0 and a racing modification is fitted).
void SetWheelRow(TuneSheet& s, int32_t row, const CarParamTables& t);
// 0x80018100(index, wheelId, price, player): the wheel shop: 1 bought (0x80021B0C finds the table 29 row of the id,
// 0x80021B38, 0x80016F10, money -= price without clamp), -1 too little money (0x800181D0). `sheet` must hold the car.
int32_t BuyWheels(GarageBlock& g, int32_t index, uint32_t wheelId, int32_t price, TuneSheet& sheet, const CareerData& d, BuildScratch scratch);

// Part changes of the settings screens (ovl0 0x80052D84 case 8 offers stage 0 always and a higher stage when its
// part is owned): the stage selected on the car's sheet (0x8005EAC0) and the slot written (0x80016F10). Returns 1,
// -4 when the stage's part is not owned, -8 when the car has no row at that stage.
int32_t ChangePartStage(GarageBlock& g, int32_t index, int32_t tuneKind, int32_t stage, const CareerData& d, BuildScratch scratch);
// RemovePart(index, partKind): ChangePartStage to stage 0 of the part's sheet kind (the car's own part; tyres: both).
int32_t RemovePart(GarageBlock& g, int32_t index, int32_t partKind, const CareerData& d, BuildScratch scratch);

// 0x80013A28(code): the wheel id of a wheel code of the wheel shop's items (8 characters "mmNNN-kc": maker pair mm
// looked up in ovl4 0x80050904 (index << 12), NNN decimal, k = character 6 ('4' 1, '5' 2, '6' 3, else 0), c =
// character 7): ((maker << 12 | NNN) << 3 | k) << 13 | c.
uint32_t WheelIdOfCode(const CareerData& d, const std::string& code);
// 0x80021BEC(wheelId, racingModified): the wheel row's colour byte (+7, +6 when the car is racing modified).
uint8_t WheelColour(const CareerData& d, uint32_t wheelId, bool racingModified);

// 0x8005F044(sheet, work, kind, stage): the record the sheet would give with `kind` at `stage` (the parts page's
// preview; kind -1 = the sheet as it is). Its quirks are kept: a gearbox with the auto-set gets the ratios generated
// from the sheet's CURRENT selection, the turbo preview puts the NA tune back to stage 0 and applies the settings of
// the LSD's stage-0 row. Tyre kinds only move the tyre sub-rows.
sim::CarParams PreviewRecord(const TuneSheet& s, int32_t kind, int32_t stage, const CareerData& d);
// 0x8001DB90(transaction, partKind): the parts page's figures of the current car (its sheet 0x800B4490 loaded):
// price (0x80017B04, -1 = no such part), owned (0x8005E874), power before (record 0x800771AC of the sheet's
// configuration) and after (0x80017F18 -> 0x8005F044 with the part's stage; tyres unchanged; -1 without a part).
struct PartPreview {
    int32_t price = -1;       // transaction +0x30
    int32_t owned = 0;        // +0x24
    int32_t powerBefore = 0;  // +0x1C
    int32_t powerAfter = -1;  // +0x20
};
PartPreview PreviewPart(const GarageCar& car, const TuneSheet& sheet, int32_t partKind, const CareerData& d);

// ---------------------------------------------------------------- settings (EXE 0x8005FC9C / 0x8005F9DC / 0x80060410)

enum SettingKind : int32_t {
    kSettingSprings = 0,     // +0x60 / +0x61 (suspension stage >= 3)
    kSettingRideHeight = 1,  // +0x5C / +0x5D (stage >= 2)
    kSettingDamperBump = 2,  // +0x64.. (stage >= 1; with stage != 3 the rebound follows)
    kSettingDamperRebound = 3, // +0x66.. (stage == 3)
    kSettingCamber = 4,      // +0x5A / +0x5B (stage >= 1)
    kSettingToe = 5,         // +0x5E / +0x5F - 0x80 (stage >= 3)
    kSettingAntiRoll = 6,    // +0x6C / +0x6D (stage >= 3)
    kSettingBrakeBalance = 7, // +0x50 / +0x51 (brake controller stage >= 1)
    kSettingGears = 8,       // gear ratios 1..n and the final drive (gearbox stage >= 3)
    kSettingGearAuto = 9,    // +0x4E auto-set top speed (gearbox stage >= 3; regenerates the ratios)
    kSettingLsdInitial = 10, kSettingLsdAccel = 11, kSettingLsdDecel = 12, // LSD rows (entries with min < max)
    kSettingLsdRearInitial = 13, // +0x6F (LSD stage >= 5)
    kSettingAsm = 14,        // +0x74 (ASM stage >= 1)
    kSettingTcs = 15,        // +0x75 (TCS stage >= 1)
    kSettingDownforce = 16,  // +0x52 / +0x53 (racing modification with a range)
    kSettingCount = 17,
};

#pragma pack(push, 1)
// One entry of the settings screen's array (8 bytes; 0x8005FC9C writes value / min / max, the LSD settings the
// configuration field index in `field`).
struct SettingValue {
    int16_t value;
    int16_t min;
    int16_t max;
    int16_t field;
};
#pragma pack(pop)
static_assert(sizeof(SettingValue) == 8);

// 0x8005FC9C(sheet, setting, out): the entries of a setting (front / rear, or gears 1..n + final drive) into out[0..8];
// returns their count, -1 when the car's parts do not allow the setting. Members the original does not write keep
// their values.
int32_t GetSetting(const TuneSheet& s, int32_t setting, SettingValue* out, const CareerData& d);
// 0x8005F9DC(sheet, setting, values, count): writes the entries' values (low bytes, gears / final drive as u16)
// into the sheet's configuration. The gear auto-set (9) regenerates the ratios (0x8005E93C; the entries above the
// top gear keep their values here, the original's are stack residue).
void SetSetting(TuneSheet& s, int32_t setting, const SettingValue* values, int32_t count, const CareerData& d);
// 0x80060410(sheet, setting): the setting back to the defaults of the selected rows.
void DefaultSetting(TuneSheet& s, int32_t setting, const CareerData& d);
// 0x80054D10: the slider of an entry moved by `delta` (the pad's +-1 steps, x10 with L/R): the result is clamped to
// the end of the range it moves towards (min when decreasing, max when increasing); 0 keeps the value.
int16_t SliderStep(int16_t value, int16_t min, int16_t max, int32_t delta);
// 0x80054D10's pad decoding: delta from the pad words (held +0, pressed +4, repeat +0xC of the pad record).
int32_t SliderDelta(uint32_t held, uint32_t pressed, uint32_t repeat);

// The settings screen as one call: the car's sheet (0x800173E8), the setting's entries (0x8005FC9C), entry `entry`
// moved by `delta` (0x80054D10), written back (0x8005F9DC) and stored into the slot with the record's write-backs
// and figures (0x80056FF0's garage part). Returns the entry's new value, or -1 when the setting is not available.
int32_t AdjustSetting(GarageBlock& g, int32_t index, int32_t setting, int32_t entry, int32_t delta, const CareerData& d, BuildScratch scratch);
// Resets a setting (0x80060410) and stores the slot like AdjustSetting.
void ResetSetting(GarageBlock& g, int32_t index, int32_t setting, const CareerData& d, BuildScratch scratch);
// 0x80056FF0's garage part: the sheet's configuration with the record builder's write-backs (engine word, exhaust
// byte, flags; 0x80077188) and the figures of 0x8005F958 into the slot.
void StoreSettings(GarageCar& car, TuneSheet& s, const CarParamTables& t, BuildScratch scratch);

// 0x80056FF0 as a whole: the race overlay's commit of its settings screens (sheet 0x8016E894). In the original's order:
// the race record of the car rebuilt from the sheet's selected rows (0x8005F410 + 0x80077214; RAM 0x801DE8BA), the
// builder's write-backs into the sheet's configuration (0x80077188: engine word, exhaust byte, flags), the
// configuration copied into the race slot (race block + 0x64), then - when the race block names a garage (+0x582:
// 0 the player's, 1 the guest's; slot index +0x584) - the figures of 0x8005F958 (record on the scratchpad) and the
// configuration into that garage slot (power / torque / weight / drive; bits 14..15 of the power word and 13..15
// of the weight word kept), and in game mode 1 (race block + 0x0A) the whole sheet copied to the purchase sheet
// 0x801DA4B8. Targets left null are not written (the caller decides which exist).
struct SettingsCommit {
    sim::CarParams* raceRecord = nullptr;  // 0x801DE8BA (0x14FDA into the career block)
    CarConfig* raceSlotConfig = nullptr;   // 0x801D58C0 (race block 0x801D585C + 0x5C + 8)
    GarageCar* garageCar = nullptr;        // garage (+0x582) slot (+0x584), null = none
    TuneSheet* purchaseSheet = nullptr;    // 0x801DA4B8 (game mode 1 only)
};
constexpr uint32_t kSettingsRecordAddress = 0x801DE8BAu;
constexpr uint32_t kRaceSlotConfigAddress = 0x801D58C0u;
void CommitSettings(TuneSheet& s, const CarParamTables& t, BuildScratch scratch, const SettingsCommit& out);

} // namespace gt2::career
