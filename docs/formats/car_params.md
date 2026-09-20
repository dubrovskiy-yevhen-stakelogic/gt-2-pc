# Car parameter tables (carparam GTDT) and the race record (CarParams)

Status 2026-09-18. US Simulation v1.2 (SCUS-94488, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a).
Everything below comes from the bytes of `carparam/usa_gtmode_data.dat` and `arcade/demofile_us.gmr` on
that disc and from our own disassembly / trace of the executable (work/re/race_load: boot-to-race trace of the
attract race, `gt2run watch` on the six records, objdump of the writers). Community documents (pez2k, SUBMANIAC)
were used only as naming hints; every field named here is named from what the code does with it.
Parser and builder: `src/gt2formats/car_params.*`. Verification: `tools/gt2verify/verify_params.cpp`.

The physics setup (`src/game/sim/car_setup.h`, `sim::CarParams`, 0x1C0 bytes, all fields documented there) does
not read the disc. The game shell builds one `CarParams` record per race slot from (a) the car's rows in the
parameter tables, (b) the per-car configuration (which parts are fitted and the menu settings) and (c) the
car's body model. This document describes (a), (b) and the builder.

## 1. Where the record comes from (the original)

Attract race of the dump, six records at 0x801DE8BA + slot * 0x1C0 (0x801C98E0 + 0x14FDA):

| Function | Role |
|---|---|
| 0x80076D74 | loads the car parameter file (file index from the EXE table 0x800925A4 + language * 10, column 0; language byte 0x801C98E0 = 1 -> index 103 = `carparam/usa_gtmode_data.dat.gz`, identified by content: its engine row 585 is `us36n` with the dump's torque curve) into a buffer (0x800E15C0 in the trace; the buffer is reused before the race starts, so the dump no longer holds it) and stores the pointer at 0x8009287C |
| 0x80076AE8 / 0x80076B74 | converts the file's directory in place: entry i (8 bytes) becomes `{ u32 pointer, u16 rowSize, u16 rowCount = size / rowSize }` with the row sizes from the EXE table 0x80092414 (see section 2). 0x80076CF8 does the same for `usa_gtmode_race.dat` (index 104, row sizes 0x80092490: 156, 96, 128, ...) - not used by the record builder |
| 0x80077D5C / 0x80077F38 | row lookup: `pointer + rowSize * index` (plain 0-based row index) |
| 0x800771AC (config, record) | the builder entry: clears a 0xC4-byte working struct (0x80076A54), resolves rows (0x800763E8), fills the record (0x80077214), writes three bytes back into the config (+0x76 u16, +0x78, +0x7A) |
| 0x800763E8 (work, config) | copies the config's settings into the working struct (mapping table 0x80092A48) and resolves one row pointer per part table from the config's u16 row indices (triples `{ workOffset, configOffset, table }` at 0x80092B1C, 24 entries) plus the five tyre rows (section 4) |
| 0x80077214 (work, record) | `memset(record, 0, 0x1C0)`; for every entry of the part list 0x80092CA4 `{ u16 kind, u16 workOffset, u32 mappingTable }` (24 entries, kind = table index) runs the mapping table on (row, record) with 0x80077634 and accumulates the power multiplier; then the post-loop steps of section 5 |
| 0x80077634 (mapping, src, dst) | mapping interpreter: entries are u32 `dst offset (bits 0..9) | src offset (10..19) | element code (20..23) | op (24..31)`, 0 terminates. Element codes: 0 = u8 x1, 1 = u16 x1, 2 = u32 x1, 3 = u8 x4, 4 = u8 x6, 5 = u8 x8, 6 = u8 x16, 7 = u16 x8, 8 = u16 x16. Ops: 0 = dst = dst * src / 100, 1 = dst = dst * src / 1000 (0x800778E4: `lb` signed for bytes, `lhu` unsigned for half-words, `lw` for words, 32-bit product, C truncating `div`), 2 = add-repeat (0x80077880: one source element added to each of `count` destination elements), 3 = add (0x80077800, wrapping), 4 = copy (memcpy of size x count) |
| 0x800779C4 (first, last, levels, position) | `levels < 2 ? first : first + (last - first) * (position - 1) / (levels - 1)` (signed `div`); all arguments are bytes loaded with `lbu` |
| 0x80017E74 (car, record) | at race start, before 0x80033384: overwrites the record's frontLength / rearLength (+8, +A) and frontTrack / rearTrack (+14, +16) from the car's body model (section 6) |
| 0x8001523C -> 0x80012CD4 | per slot: 0x80017E74, then 0x80033384 (the setup) with the record |

In the attract race the six configurations come from the replay file: `arcade/demofile_us.gmr` is loaded at
0x801D42DC and its six 0xD0-byte car slots (file offset 0x15DC, RAM 0x801D58B8) carry the configurations; the
builder is called with `slot + 8`. A saved replay has the same layout (the file is a memory-card image, "SC"
magic, Shift-JIS title).

## 2. GTDT container and the car parameter file

```
0x00  "GTDT"
0x04  u16 0x6C
0x06  u16 entryCount              (0x3E = 62 for usa_gtmode_data.dat, 0x44 = 68 for usa_arcade_data.dat)
0x08  entryCount x { u32 offset, u32 size }
```
The first entryCount / 2 entries are row tables; the second half are per-table extras (mostly empty; the
loader relocates them too). Row sizes assigned by the loader (EXE 0x80092414, first 31 used for the gtmode
file):

| # | rows (usa_gtmode_data) | row size | our name | rows per car |
|---|---|---|---|---|
| 0 | 1143 | 12 | brakes | stock + upgrade stages |
| 1 | 531 | 16 | brake controller | upgrade only (row 0 = none) |
| 2 | 1 | 24 | steering | one row for all cars |
| 3 | 618 | 20 | chassis | one |
| 4 | 1597 | 12 | lightweight | upgrade only |
| 5 | 1050 | 28 | racing modification | stock + RM |
| 6 | 618 | 76 | engine | one |
| 7 | 526 | 12 | port polish | upgrade only |
| 8 | 526 | 12 | engine balance | upgrade only |
| 9 | 29 | 12 | displacement | upgrade only |
| 10 | 526 | 12 | computer | upgrade only |
| 11 | 612 | 12 | NA tune | upgrade only |
| 12 | 1158 | 20 | turbo kit | upgrade only |
| 13 | 618 | 16 | drivetrain | one |
| 14 | 1570 | 12 | flywheel | upgrade only |
| 15 | 2187 | 16 | clutch | stock + stages |
| 16 | 306 | 12 | propeller shaft | upgrade only |
| 17 | 2205 | 36 | gearbox | stock + stages |
| 18 | 2199 | 76 | suspension | stock + stages |
| 19 | 393 | 12 | intercooler | upgrade only |
| 20 | 1570 | 12 | muffler | upgrade only |
| 21 | 2331 | 32 | limited-slip differential | stock + stages |
| 22 | 4696 | 16 | tyres front | stock + 6 grades |
| 23 | 4696 | 12 | tyres rear | stock + 6 grades (same indices as 22 in the six configs) |
| 24 | 217 | 4 | tyre size | referenced by 22 / 23 |
| 25 | 20 | 64 | tyre compound (grip curves) | referenced by 22 / 23 |
| 26 | 1 | 8 | surface grip | referenced by 22 |
| 27 | 3 | 16 | ASM | upgrade only |
| 28 | 3 | 16 | TCS | upgrade only |
| 29, 30 | | 8, 72 | not read by the builder | |

Every row of tables 0..23 and 27..28 starts with the packed car id (`car_info.h`, 6 bits per character). Ids
are sorted within a table. Upgrade tables have a first row with id "00000" (0x01041041) meaning "no part";
their other rows have `u32 price` at +4 and a stage byte at +8 (table 23: stage at +4). A car's stock part is
its row with price 0 (checked on the six attract cars for tables 0, 5, 15, 17, 18, 21, 22, 23).

`usa_arcade_data.dat` has 34 tables with the same first 31 row sizes; whether the arcade mode builds records
from it with the same code path was not traced.

## 3. The per-car configuration (`CarConfig`, 0x84 bytes)

Offsets from the pointer the builder receives (replay slot + 8). Row indices are u16, 0-based.

| Offset | Field | Use |
|---|---|---|
| 0x00 | u32 | copied to the working struct, not used (0x300 player / 0x100 AI in the replay) |
| 0x04..0x36 | u16 row of tables 0, 1, 2, 3, 6, 13, 17, 18, 21, 22, 23, 4, 5, 7, 8, 9, 10, 11, 12, 14, 15, 16, 20, 19, 27, 28 (in that order) | row lookups |
| 0x38 | u16 | not read by the builder (10 for the AI cars, 0 for the player) |
| 0x3A | u16 torqueMultiplier100 | replaces the engine row's multiplier when flags bit 0 is set (AI cars: 100..136) |
| 0x3C | s16 gearRatio[8], 0x4C s16 finalDrive, 0x4E u8 gearAutoFinal | copied over the gearbox row's values |
| 0x4F | u8 | 0xFF in the replay (the gearbox row's byte +22) |
| 0x50 | u8 brakeBalance[2] | positions into the brake controller's ABS gain range |
| 0x52 | u8 downforce[2] | record +88 |
| 0x54..0x59 | turbo: boost, spool rpm/100, spool rate, second boost, second spool rpm, second spool rate | record +179, +178, +17A, +17C, +17B, +17D |
| 0x5A | camber[2], 0x5C rideHeight[2], 0x5E toe[2], 0x60 spring[2], 0x62 damperScaleDivisor[2] | record +6C, +116, +18E, +6E / +7A, +18C |
| 0x64 | u8 damperLevel[8]: bump low / bump high / rebound low / rebound high, front then rear | positions into the suspension row's ranges |
| 0x6C | u8 antiRollLevel[2] | positions |
| 0x6E | diffInitial[2], 0x70 diffAccel[2], 0x72 diffDecel[2] | record +186.. |
| 0x74 | asmLevel, 0x75 tcsLevel | positions |
| 0x76 | u16 | written by the builder: engine row +0A |
| 0x78 | u8 | written by the builder: muffler row +8, plus 4 when a turbo kit is fitted |
| 0x79 | u8 | 29 for the player, 50 for the AI cars (meaning unknown) |
| 0x7A | flags | bit 0: torqueMultiplier100 applies; bit 1: set by the builder when the turbo row's +9 is non-zero; bit 7: the record's gearAutoSet is cleared |
| 0x7C..0x81 | 3 x u16 | copied to the working struct, not used |

The stock configuration reproduced by `StockCarConfig`: the price-0 rows of tables 0, 5, 15, 17, 18, 21, 22,
23, the single rows of 3, 6, 13, row 0 elsewhere, and the settings at the rows' defaults: brake balance =
brake controller row +C, downforce = racing-modify row +12 / +15, turbo = turbo row +A..+F, camber / ride
height / spring / damper divisors = suspension row +B, +E, +15, +18, +1F, +22, +23, +24, toe 128, all positions
1, differential = LSD row +D, +17, +10, +1A, +13, +1D, gears from the gearbox row. This equals the six attract
configurations except that the AI cars carry the stage-1 tyre rows (rows +1..+6 of the car's tyre block).

## 4. Row layouts (fields the builder reads)

`->` = copied into the record (offsets of `sim::CarParams`), `*=` = multiplied (op 0 / 1), `+=` = added.

- **Brakes (0)**: +9 -> brakeFront (+60), +A -> brakeRear (+61), +B -> handbrake (+63).
- **Brake controller (1)**: +9 levels F, +A / +B ABS gain at the first / last position (F), +C -> absGain[0]
  (+2E, overwritten by the position value), +D levels R, +E / +F first / last (R).
- **Steering (2)**: +A u8[6] -> steerLimitXs (+45), +10 u8[6] -> steerLimitYs (+4B), +16 -> steerRateDeg (+5F),
  +17 -> steerLockDeg (+5E).
- **Chassis (3)**: +4 -> frontWeightPercent (+12), +6 u8[2] -> tyreGripModifier (+17E), +A s16 -> height (+E),
  +C s16 -> wheelbase (+10), +E s16 -> weightKg (+5A), +10 -> yawInertiaCode (+57), +11 -> pitchInertiaCode (+58),
  +12 -> rollInertiaCode (+59), +13 -> +5C (not read by the setup).
- **Lightweight (4)**: +8 u16 permille: weightKg *= v / 1000 (u16 product), yawInertiaCode *= v / 1000 (after the
  loop, u8 x u16 product); +A: rollInertiaCode *= v / 100.
- **Racing modification (5)**: +8 u32 model id (the stock row carries the car's own id), +C: yawInertiaCode *= v / 100
  and, after the loop, weightKg = weightKg * v / 100 (s16, truncating); +D: rollInertiaCode *= v / 100; +F -> Cd
  (+62); +12 -> downforce F (+88); +15 -> downforce R (+89); +16 s16 -> frontTrack (+14); +18 s16 -> rearTrack (+16);
  +1A s16 -> width (+C).
- **Engine (6)**: +A u16 -> config +76; +C u16[16] -> torque (+13E); +36 -> torqueMultiplier1000 low byte (+1AE);
  +37 -> downshiftFloorRpm10 (+5D); +38 -> idleRpm10 (+13); +39 -> revLimitRpm100 (+32); +3A -> upshiftRpm100 (+31);
  +3B u8[16] -> torqueRpm100 (+34); +4B -> torquePointCount (+56).
- **Port polish (7), displacement (9), computer (10), intercooler (19), muffler (20)**: +9 power gain %; muffler +8
  -> config +78.
- **Engine balance (8)**: +9 += to all 16 torqueRpm100; +A += revLimitRpm100; +B power gain.
- **NA tune (11)**: as 8, and +A += upshiftRpm100 as well (mapping table 0x80092DEC).
- **Turbo kit (12)**: +9 -> turboBoostCap10 (+2D); +A -> turboBoost10 (+179); +B -> turboSpoolRpm100 (+178); +C ->
  turboSpoolRate10 (+17A); +D -> +17C; +E -> +17B; +F -> +17D; +10 += revLimitRpm100; +11 += upshiftRpm100; +12 power
  gain; +13 -> powerPercent (+1AC).
- **Drivetrain (13)**: +7 -> centreSplitPercent (+119), +8 -> driveType (+8A), +9 -> fourWheelType (+51), +A ->
  engineBrake (+2C), +B / +C -> wheelInertia (+64 / +65), +D -> engineInertia (+2B), +E / +F -> axleInertiaCode (+182 / +183).
- **Flywheel (14)**: +9: engineBrake *= %, +A: engineInertia *= %, +B: wheelInertia[0] *= % and wheelInertia[1] *= %
  (0x80092DF4).
- **Clutch (15)**: +9 engineBrake, +A engineInertia, +B wheelInertia[0], +C wheelInertia[1] (all *= %), +D -> +33.
- **Propeller shaft (16)**: +9 engineBrake, +A wheelInertia[0], +B wheelInertia[1] (*= %); +A also axleInertiaCode[0]
  and [1] *= % (0x80092DFC).
- **Gearbox (17)**: +9 -> gearCount (+2A), +A s16[8] -> gearRatio (+18), +1A s16 -> finalDrive (+28), +20 -> gearAutoSet
  (+1AA), +21 -> gearAutoFinal (+1AB). Ratios / final / auto-final are then overwritten from the config.
- **Suspension (18)**: +B -> camberFront10 (+6C), +E -> camberRear10 (+6D), +15 / +18 -> rideHeightMm (+116 / +117),
  +19 u8[2] -> bumpTravelMm (+19E), +1B u8[2] -> droopTravelMm (+1A0), +1F / +22 -> spring codes (+6E / +7A), +23 / +24
  -> damperScaleDivisor (+18C / +18D), +25 / +26 -> bump-stop codes (+70 / +7C). Damper forces and anti-roll bars are
  `{ levels, first, last, default }` groups: bump F: levels +27, low +28/+29 (default +2A -> +73), high +2B/+2C
  (+2D -> +75); rebound F: levels +2E, low +2F/+30 (+31 -> +77), high +32/+33 (+34 -> +79); bump R: +35, +36/+37
  (+38 -> +7F), +39/+3A (+3B -> +81); rebound R: +3C, +3D/+3E (+3F -> +83), +40/+41 (+42 -> +85); anti-roll F: +43,
  +44/+45 (+46 -> +6F); anti-roll R: +47, +48/+49 (+4A -> +7B). The defaults are copied first and then replaced by
  the value at the config's position (0x800779C4).
- **LSD (21)**: +C -> diffTypeCode[0] (+184), +D -> diffInitialTorque[0] (+186), +10 -> diffAccel[0] (+188), +13 ->
  diffDecel[0] (+18A), +16 -> diffTypeCode[1] (+185), +17 -> +187, +1A -> +189, +1D -> +18B (the six torque bytes are
  then overwritten from the config).
- **Tyres front (22)**: +A u16 row of table 24, +C u16 row of table 25, +E u16 row of table 26. **Tyres rear (23)**:
  +4 stage, +6 u16 row of table 24, +8 u16 row of table 25.
- **Tyre size (24)**: +0 -> tyreWidthCode, +1 -> rimCode, +2 -> tyreAspectCode (+66 / +67, +68 / +69, +6A / +6B).
- **Tyre compound (25)**: +0 -> tyreGripPercent (+86 / +87), +4 u8[4] -> loadGripXs, +8 -> loadGripYs, +C u8[8] ->
  slipAngleXs, +14 -> slipAngleYs, +1C u8[6] -> slipRatioNegXs, +22 -> slipRatioNegYs, +28 -> slipRatioNegYs2, +2E ->
  slipRatioPosXs, +34 -> slipRatioPosYs, +3A -> slipRatioPosYs2 (front block at +11B/+11F/+8F/+97/+B1..+D0, rear at
  +124/+128/+A0/+A8/+D7..+F6).
- **Surface grip (26)**: +0 u8[7] -> surfaceGripPercent (+192..+198).
- **ASM (27)**: +9 levels, +A -> asmYawGain100 (+1A5), +B default -> asmYawThreshold100 (+1A6), +C / +D first / last
  (position value replaces the default).
- **TCS (28)**: +A levels, +B -> tcsGain10 (+1A4), +C default -> tcsFalloffGain10 (+1A2), +D / +E first / last, +F ->
  tcsSteerGain100 (+1A3).

## 5. Builder order (0x80077214) and the derived fields

1. `memset(record, 0)`; power multiplier = 1000.
2. Part loop in the order 0, 1, 2, 3, 6, 13, 17, 18, 21, 27, 28, 4, 5, 7, 8, 9, 10, 11, 12, 14, 15, 16, 20, 19: run the
   part's mapping table; then `multiplier = multiplier * (gain + 100) / 100` with gain = row +9 for kinds 7, 9, 10,
   19, 20, row +B for 8 and 11, row +12 for 12, 0 otherwise (gain 255 -> factor 519).
3. yawInertiaCode = yawInertiaCode * lightweight +8 / 1000; weightKg = weightKg * racing-modify +C / 100.
4. Tyre mapping tables (size F, compound F, surface grip, size R, compound R).
5. 0x80077AF4: config camber, ride height, toe, spring, damper divisors; damper forces and anti-roll bars at the
   config positions. 0x80077AC4: config turbo. 0x80077AAC: config downforce. 0x80077A6C: config gears, final,
   auto-final. 0x800779FC: absGain F/R at the brake balance positions. 0x80077D2C: config differential torques.
   0x80077CE8: tcsFalloffGain10 at tcsLevel. 0x80077CA4: asmYawThreshold100 at asmLevel.
6. flags bit 0 -> torqueMultiplier1000 = config +3A. config +76 = engine +A; config +78 = muffler +8 (+4 and flags
   bit 1 when turbo +9 != 0). powerPercentTop (+1B0) = multiplier. torqueMultiplier1000 *= 10. flags bit 7 ->
   gearAutoSet = 0.
7. Constants: unsprung mass 40 (+71, +7D), damper knees 30 / 70 (+72, +74, +76, +78 and the rear four), curve
   counts 6 (+44, +B0, +C3, +D6, +E9), 8 (+8E, +9F), 4 (+11A, +123, +12C, +135), camber grip curves
   xs {0, 0x82, 0xA4, 0xFF} ys {0xC8, 0xAC, 0xA6, 0x94} (+12D.., +136..), steerMaxRateCode 9 (+118).

Record fields not written by any step stay 0: +0..+7, the track / length words until race start, +8B (clutchCode) .. +8D,
+FC..+115, +15E..+177, +180, +181, +190, +191, +199..+19B, +1A7..+1A9, +1AD, +1B2..+1BF.

## 6. Model-derived fields (0x80017E74, race start)

From the car object (car + 0x878 -> the loaded `.cdo`; its LOD 0 block at +0x870, see car_cdo_cdp.md):
`s = LOD0.scale - 16`, `scaled(v) = s >= 0 ? v << s : v >> -s` (model units, 1/4096 m after scaling):

- frontLength (+8) = `(u32)(-scaled(bbox[2]) * 125) >> 9` (bbox[2] = min z, the front, negative) - mm
- rearLength (+A) = `(u32)(scaled(bbox[6]) * 125) >> 9`
- frontTrack (+14) = `T(rimCode[0], cdo+0x20)`, rearTrack (+16) = `T(rimCode[1], cdo+0x30)` where cdo+0x20 / +0x30
  is the first s16 of the front-left / rear-left wheel entry (its lateral position) and
  `T(rim, w) = (((|w| - (((rim * 0xA000 + 0x5000) / 1000) >> 1)) * 1000) >> 12) << 1` (0x80017E18).

Also car + 0x87C = `max(|bbox[0]|, bbox[4]) scaled << 4` (half width, not part of the record).

## 7. Verification (gt2verify, `build\gt2verify.exe work\re\race_load\ram.bin <disc> seattle`)

For each of the six attract cars the record is built natively from the replay's configuration, the tables of
`carparam/usa_gtmode_data.dat` and `carobj/<model>.cdo`, then compared with the dump's record (a) raw, skipping
the bytes the setup patches (first samples of the tyre curves, a zero last torque point, auto-set gears), and
(b) exactly after handing it to the original setup 0x800319A8 on the guest. Result 2026-09-18: `BuildParams 6
cases, 0 mismatches`, `BuildParams+Setup 6 cases, 0 mismatches`, `StockConfig 6 cases, 0 mismatches` (the stock
configuration equals the replay's, tyre rows of the AI cars excepted).

## 8. Open questions

- Unnamed row bytes (e.g. brake controller +4.., chassis +5 / +8, engine +4..+9 and +2C..+35, suspension +9..+1E
  groups other than the ones read, LSD +9..+B) and the second half of the GTDT directory.
- Config bytes +0, +38, +4F, +79, +7B, +7C..+81 and flags bit 6; how the shell chooses AI tyres / power
  multipliers (the attract replay has stage-1 tyres and 100..136 % torque for the AI cars).
- Whether the arcade disc / arcade mode builds records from `usa_arcade_data.dat` through the same code.
- The stage byte (+8; +4 in table 23) of the stock brake / clutch / gearbox / suspension / LSD / tyre rows is 0 and
  their price is 0; which of the two the shell uses as the stock criterion (both agree on the six cars).
