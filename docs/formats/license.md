# Licence tests: `carparam/usa_license_data.dat`

Status 2026-09-19. Derived from the bytes of US Simulation v1.2 (SCUS_944.88, EXE SHA-1
3030aa271c0a4022fc69ce09d76a6bc75e69a32a; race overlay = GT2.OVL member 0), from our disassembly / Ghidra export of
the RAM dump of licence test B-1 (`work/re/license_race/ram.bin`), and checked by `gt2verify` (rows LicName, LicFind, LicMedal, LicSpec, LicBuild, LicTest, LicCar in
`tools/gt2verify/verify_license.cpp`). Parser: `src/gt2formats/license_data.*`; used by `gt2game --license`.
No external reference was used.

## 1. Where the test parameters come from

The race does not read the licence file itself for the rules: the menu copies the test's record into the shell's
race settings block `0x801C98A0` (0x40 bytes) and names the test at `0x801D586C` ("LJB00" for B-1). The race
overlay reads the file only for the medal times: the HUD (`0x8002D308`) and the results (`0x8002B170`) call
`0x8007830C(*0x80092E6C, 0x801D586C)` and take the medal pairs of the record + 0x44 with `0x8003D7B8`.

`*0x80092E6C` = the relocated `carparam/usa_license_data.dat` (GTDT, 36,432 bytes inflated; 0x80169894 in the dump).
Its bytes from file offset 0x208 equal the RAM image from 0x80169894 + 0x208 (all tables and extras). The files
`license_data.dat` and `eng_license_data.dat` have the same name pool but other table-30 rows; the US game uses
`usa_`.

## 2. Container

GTDT (`docs/formats/car_params.md` section 2) with 0x40 directory entries: 32 tables + their 32 extras. The loaded
header holds {pointer, u16 row size, u16 row count} per table.

| Table | Rows | Row size | Contents |
|---|---|---|---|
| 0..29 | | as `usa_gtmode_data.dat` (12, 16, 24, 20, ...) | car parameter tables of the ~50 licence cars (same layouts, own row indices) |
| 30 | 60 | 0x9C | the tests (below) |
| 31 | 53 | 0x60 | the licence cars: +0 packed car id, +0x5E u16 car number (= row + 1 in the file); the rest is not decoded |
| extra 62 (of table 30) | | | the name pool |

The EXE carries the row-size list 12, 16, 24, ..., 8, 0x9C, 0x60 at 0x8009249C (the carparam list 0x80092414 has
0x48 in position 30); which list the licence loader passes was not traced - the relocated header of the dump has
0x9C / 0x60.

Name pool: `u16 count; count x { u8 length; char name[length]; u8 0 }`. 105 names: the 60 test names ("LIA00" ..
"LJB09") interleaved with course file names ("TC_lisence", "circle80", ...).

## 3. Lookup (race overlay / EXE)

- `0x80076C74(pool, name)`: index of the first pool entry equal to `name` (strcmp 0x8008CF00), -1 if none.
- `0x800781E0(object, name)`: pool = `*(object + (u16(object + 6) >> 1) * 8 + 0xF8)` (the extra of table 30; with
  the global 0x80092878 == 2 the object at *0x80092870 + 8 is used instead - 3 in the dump); binary search of table 30
  for the row whose u16 at +0 equals the name's index: `mid = (lo + hi) >> 1`, equal -> mid, `lo == hi` -> -1, row
  below -> `lo = mid`, else `hi = mid`. It never terminates for a pool name that is not a test (e.g. a course name).
- `0x80078038(table, i)` = table pointer + u16 row size * i; `0x8007830C(object, name)` = the row pointer or 0.

Verified: the original's lookups of all 105 pool names (+ 5 absent) and all 60 test names (+ 2 absent) equal the
native ones, and each returned record equals our row byte for byte (rows LicName, LicFind).

## 4. Test row (table 30, 0x9C bytes)

| Offset | Field |
|---|---|
| +0x00 | u16 name index of the test ("LJB00") |
| +0x02 | u16 name index of the course file (`crsobj/<name>.tro`; B-1: "TC_lisence") |
| +0x04 | 16 car slots, u32 = (paint code << 26) + licence car number (the event-slot layout of usa_gtmode_race.dat; B-1: 0x14000034 = code 5 = paint `4`, car 0x34). Slot 0 is the test car. Slot 1 is filled in 6 tests (LIA02, LIB03, LJA02, LJA04, LJB06, LJB07: 0x0C / 0x28, 0x33 / 0x35, 0x0E / 0x27) but NO code of the US v1.2 build reads it (section 5): those tests run one car |
| +0x44 | the race settings block (0x40 bytes) - equal to the dump's `0x801C98A0..0x801C98DF` for B-1 (row LicTest) |
| +0x78 / +0x84 / +0x98 | the event-row prize fields: +0x84 u32 x 4 packed prize car ids - non-zero only in the first test of each licence = the licence's all-gold prize car (section 7) |
| +0x94 | u8: 2, 0x13 or 0x17 (unknown) |

Settings block (0x801C98A0 + offset; names from the readers in the race overlay):

| Offset | Field |
|---|---|
| +0x00 | u8 start speed in km/h: 0x80033384 calls 0x8003311C(body, v) when it is non-zero and 0x801D5869 == 0 - forward speed = v * 1138 (1138 = 1 km/h in 1/4096 m/s), rpm / gear / wheel speeds to match (a rolling start; ported 2026-09-19: sim::RollingStart, gt2verify RollStart; docs/research/menus_gtmode.md 8.1). 0 in 8 tests (LIA00, LJB00..05, LJB08), 50 / 100 / 150 / 200 in the others. The menu's race-block builder 0x80010078 writes race block +0x0D (0x801D5869) = 1 only for a zero start speed, so the others start rolling without the start hold (gt2game: LoadDiscData) |
| +0x01 | u8 target lap (type 5; 0xFF otherwise) |
| +0x02 | u8 test type: 2 stop in the box (LJA00, LJA01, LJB00..02), 3 reach the last sector line (42 tests), 5 complete the target lap (LIA00, LIS00..09, LJB03, LJB04; target lap 2 or 1); 1 would force control class 1 on car 0 (0x80033384) - no test uses it |
| +0x04..+0x0B | AI grip percent [class] front / [class + 4] rear |
| +0x0C..+0x0F / +0x10..+0x13 | corner grip / speed scale percent per class |
| +0x1D..+0x23 | tyre wear block (disc_data.h RaceSettings) |
| +0x24 | u8 box start in 10 m of course distance (type 2) |
| +0x25 | u8 box length in m (type 2) |
| +0x26 + 2k | medal pairs k = 0..4: {u8 minutes * 100 + seconds, u8 1/100 s} -> `0x8003D7B8` = (b0 / 100) * 60000 + (b0 % 100) * 1000 + b1 * 10 ms. k = 1 gold, 2 silver, 3 bronze, 4 the fourth prize; k = 0 is 0 in the file |
| +0x30 | u8 (10 for B-1): compared by 0x8002B170 with a save-record byte + 1 for the fourth prize |

B-1 (`LJB00`): course TC_lisence, licence car 34 = t2vzn (Toyota Vitz), type 2, box 1000 m + 26 m, medals
0:38.650 / 0:38.900 / 0:39.800, fourth 0:41.000.

Label -> test name: B-1 = LJB00 (the dump). By the pool's naming the others are B -> LJB, A -> LJA, IC -> LIC,
IB -> LIB, IA -> LIA, S -> LIS with the test number - 1 (only B-1 is confirmed by a dump).

## 5. The licence car

Updated 2026-09-19 (second pass). The race builders set up the shell's car slot 0 (`0x801D58B8`, 0x90 bytes per slot)
from slot 0 of the test row; there are two copies of the same code: GT-mode ovl4 `0x80010078` (a test started from
the licence page) and race overlay `0x8004C7A0` (a test chosen / retried on the race overlay's licence menu):

- `u = row + 4`, number = u & 0x3FFFFFF, paint code = u >> 26;
- `0x800768C0(number)` = `0x80077D5C(object, 31, number - 1)`: the table 31 row number - 1 (the licence database is
  the selected one: global `0x80092878` = 3);
- slot +0 = row +0 (packed car id), slot +4 = EXE `0x80091620[code]` ("-0123456789abc...", the paint character),
  +0x8C = 1, +0x8D = 0 (grid slot), +0x8E = 3 (player 1), +0x8F = 0;
- `0x80076FC0(row, slot + 8)`: the configuration from the row with the mapping `0x80092BB4` and the part rows'
  default settings (`gt2::ConfigFromCarSpec`, gtMode = false: no flag 0x40, byte +0x79 = 255);
- config +0x1C (racing modification row) != 0 (lhu + blez): slot +0 = table 5 row + 8 (the modification's body);
- then the settings block copy (row + 0x44, 0x40 bytes -> `0x801C98A0`) and `0x800771AC(slot + 8, 0x801DE8BA)`.

The earlier rule (the stock rows of the id, `gt2::StockCarConfig`) was checked on B-1 only and is not the original's:
20 of the 60 tests use cars without a price-0 row set in the licence tables (all ten S tests, eight IA, two IB; e.g.
IB-1 = LIB00 s2irr, S-1 = LIS00 brm1r) and made `gt2game --license` throw ("no stock row"), and other tests got other
configurations than the original's (A-1 now passes at 0:19.652 with the headless driver, was 0:20.254; B-1 unchanged).
Port: `LicenseData::RaceCar` (`license_data.h`).

**Slot 1 is never read.** All callers of the record lookup `0x8007830C` in the executable and GT2.OVL members 0..5
were checked (ovl0 0x8002B288 results, 0x8002D494 HUD, 0x8004C7E0 builder, 0x8004CAD8 menu info, 0x8004DE00 licence
record; ovl4 0x800100B8 builder, 0x800103FC licence page; the others read other databases), as well as the table-row
accessors `0x80078038` / `0x800781E0`: every reader of a test row takes +0x00, +0x02, +0x04 (slot 0), +0x44.., +0x94;
none reads +0x08. Confirmed on the original: B-7 (LJB06, slot 1 = car 0x28) run in the interpreter
(`gt2run session`, route `1400:cross,2000:right,2060:cross,2400:cross,2560:down x6 (40 fields apart),2900:cross,
3700:cross,4500:cross:1300`, dump `work/re/lic2_b7/ram_005900.bin`): car count `0x800AF231` = 1, slot 1 (`0x801D5948`)
all zero, slot 0 = h2irn paint '1'. The six "second car" tests are ordinary one-car tests.

Verification (gt2verify on `work/re/license_race/ram.bin`):
- LicSpec (`0x80076FC0`, 53 cases): the original's `0x800768C0(n)` row equals `LicenseData::CarRowAt(n)` and its
  configuration equals `ConfigFromCarSpec(row, false)` byte for byte, for every licence car;
- LicBuild (`0x8004C7A0`, 60 cases): the original builder run for EVERY test name: car slot 0 (id, paint, the whole
  configuration after the record builder's write-backs), the record `0x801DE8BA` (except the lengths / tracks that
  `0x80017E74` sets at the race start), the settings block, the entry bytes, slot 1 empty, `0x801D5869` = (settings
  +0 == 0), mode 3 and the lap byte - all equal to the native `RaceCar` + `BuildCarParams`;
- LicTest / LicCar: the dump's test (car count 1, the whole configuration and the patched record).
- Frames: `gt2verify --race-capture` of B-7 with the route above (574 race frames, fields 4655..5800, a rolling start
  at 50 km/h) and `gt2game --license B-7 --frames-compare work/lic2/cap/b7.bin`: 0 frames differ.

## 6. The race

The dump's shell state for B-1: game mode `0x801D5866` = 3, lap count `0x801D586B` = 255, `0x801D5867` = 5 /
`0x801D5868` = 0 (the save record index 0x801CACFC + 5 * 0x668 + 0 * 0xA4), countdown on, one car, entry kind 3
(player 1: pad slot 2, control class 0 - `0x80012CD4`), grid slot 0, automatic. The shell's mode 3 rules
(`race_shell.h`) decide pass / fail: `0x801D5DEC` = 1 pass, 3 past the box, 4 off the course / loose surface,
5 wall; `0x801D5DF0` = the time. The results screen `0x8002B170` gives a prize only for code 1: time < medal 1 /
2 / 3 (unsigned) = gold / silver / bronze; medal 4 only with the save-record condition above.

`gt2game <disc> --license B-1 [--headless N] [--license-decel A]` loads the test's course and car and runs it in
mode 3 with the test's settings block; the console prints the result and the prize. In `--headless` a test driver
(ours, not the original's) holds full throttle and brakes when the stopping distance at A m/s^2 (default 10) reaches
the middle of the box; it does not steer. All 60 tests start and run headless without an error (2026-09-19, 300 s
each): 4 pass (LJA00, LJB00..02), the others end with the shell's own fail codes (15 wall, 36 off the course) or run
out of time (5) - the driver, not the port, fails them. The licence car's paint is the slot's paint character
(`.cdp` paint index; `--paint N` overrides).

## 7. The prize block of a licence test

Before a test the GT-mode menus (ovl4 `0x80013864`, after the day + 1 and the load of the licence file) run
`0x80018C8C(0x801D55AC, name)`, the event prize builder: for a licence name (`0x80018608`) `0x80018C14` finds the
licence of the three-letter prefix (ovl4 list `0x80050C34`) and the format of ovl4 `0x80050C1C` ("LIS%02d" ...) with 0
names the licence's FIRST test; that row (the event-row layout: +0x78 prizes, +0x84 prize cars, +0x98 bonus) builds the
block like an event's (`career::PreparePrizes`, seeded with the VSync counter). Prize cars (+0x84 of the first tests):
S 0x1701E65C, IA 0x1200F71C, IB 0x1745E59C, IC 0x1E0D129C, A 0x0E35A498, B 0x120DD758 (h2ssn). When a licence becomes
all gold, the race overlay's `0x8004E104` runs `0x8004DF04` = `0x8005E7F0(garage, 0x801D55CC)`: prize car 0 into the
garage (unconditional; nothing when the garage is full). Ported in `tools/gt2game/career_race.cpp` (licence path of
`RunMenuRace`); not compared with the original (no dump of an all-gold licence).
