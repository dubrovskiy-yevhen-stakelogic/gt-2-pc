# carparam/usa_arcade_data.dat - the arcade car and race tables

Disc: US Arcade v1.1 (SCUS-94455, EXE `SCUS_944.55` SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95). Evidence: the bytes of
the file (VOL path `carparam/usa_arcade_data.dat`), our disassembly of GT2.OVL member 3 (the race launcher, ovl3
SHA-1 d31f01582a5c37f0a661fa378dfb62526d11ea23) and the arcade race dump `work\re\arcade_race\ram.bin` (event A0A, Tahiti
Road, six cars). Parser: `src/gt2formats/arcade_data.*` (`ArcadeData`); checks: `gt2tool arcade-entries`, `gt2tool
param-scan <arcade disc>`, gt2verify rows `ArcadeEvent` / `ArcadeCars`.

## 1. Layout

GTDT (`car_params.md`) with 68 entries = 34 tables + 34 extras.

| Table | Rows | Content |
|---|---|---|
| 0..29 | as `usa_gtmode_data.dat` | the car part tables of the record builder (`car_params.h`: brakes .. TCS, table 29 = the profile rows the specs' + 0x38 names) |
| 30 | 15 x 0x9C | the arcade race events, the `RaceEvent` layout of `gtmode_tables.h` (`+00` name, `+02` course "roma", `+04` 16 opponent slots, `+44` the 0x40-byte race settings block, `+94` tag "0"); names in extra 30 (17 names) |
| 31 | 38 x 0x60 | opponent cars, `OpponentCarRow` (`CarSpec` + settings + `+5C` torque multiplier x100, `+5E` number = row + 1) |
| 32 | 63 x 60 | the player's cars: `CarSpec` (0x3A bytes: packed car id and every part row, `+38` the table-29 profile row) + 2 bytes |
| 33 | 63 x 60 | the same 63 cars in the same order; differs from table 32 only in the tyre rows (`+30` / `+32`) of 37 cars |

Events (table 30): `A<level><class>` for level 0 / 1 / 2 (the menu's Easy / Normal / Difficult) and class A / B / C / S
(`A0A` .. `A2S`, 12 events), `A2P` (no opponent slots), `ADT` and `ATT` (no opponents, settings + 0 = 80: a rolling
start at 80 km/h; `ADT` has settings + 0x31 = 1, the dirt-tyre flag of `RaceEvent`). The opponent slots depend only on
the class (A: 5 10 15 16 20 21 29 31; B: 1 3 4 9 11 13 17 24 32; C: 6 7 18 19 22 23 26 30 36 38; S: 2 8 12 14 25 27 28 33
34 35; paint code 0 in every slot). The level changes only the settings block:

| Level | corner grip % (settings + 0x0C..0x0F) A / B / C / S | catch-up bytes + 0x17..0x1C (`CatchUpTuning`) |
|---|---|---|
| 0 Easy | 80 / 85 / 95 / 75 | 19 0A 04 0A 1E 08 |
| 1 Normal | 90 / 90 / 98 / 90 | 0A 14 06 0A 1E 06 |
| 2 Difficult | 100 / 100 / 100 / 100 | 05 32 0A 05 1E 06 |

Every other settings byte is equal in the 12 events (AI grip 100 %, speed scale 100 %, tyre wear off, settings + 1 = 1).
The AI's engine power does not depend on the level: every opponent row has torque multiplier 100 (x10 = 1000 in the
record, `CarConfig::flags` bit 0).

## 2. What the race launcher builds (ovl3 0x800121DC)

(2026-09-19: the Single Player race of the menus is built by the identical copy 0x80010554 in member 2, called from 0x80010C84,
which then overwrites + 0x0F with the laps option, the course and + 0x57E / + 0x57F: `docs/research/arcade_disc.md` section 16.3.
The rules below hold for both copies.)

For each of the six race entries (race block 0x801D585C + 0x5C + i x 0xD0, Simulation addresses; the Arcade build keeps
the block at 0x801D52BC):

- player (entry kind 3): `CarConfig` = 0x80076ED0 (Simulation 0x80076FC0, `ConfigFromCarSpec`) of the car's row of table
  32, without the GT-mode flag (byte79 = 255, flags bit 6 clear, word00 = 0x100 and word38 from the table-29 row);
- AI (kind 1): 0x80076E6C (Simulation 0x80076F5C, `OpponentCarConfig`) of an opponent row drawn from the event's slots by
  0x80011EC0 (random slot, availability, duplicate rejection like the GT-mode picker 0x80010714; seed = the VSync counter
  0x8007D14C(0) at the build) - the opponent's own tyre rows (io19n: tyre row 61, the stock row is 60), flags bit 0 and
  torque multiplier 100;
- grid slot = 5 - i (the player last), transmission AT; then 0x800770BC (Simulation 0x800771AC, the record builder) for
  every entry.

The arcade race dump confirms every part: `gt2tool arcade-entries <arcade disc> work\re\arcade_race\ram.bin` - slot 0 ccrcn =
player car row 10 of table 32; slots 1..5 = opponents 16 (a28sn), 15 (io19n), 21 (n24vn), 31 (k2zzn), 20 (m2g6n), all
slots of A0A; the native builder's record of each configuration equals the dump's record (0 bytes, the setup-patched bytes
excepted); the settings block 0x801C98A0 equals A0A + 0x44 (gt2verify `ArcadeEvent`).

`tcegn` (chassis row 56, "no stock row in table 5"): the arcade shell never searches a stock row. Its only user is player
car row 55 `tcerr`, whose spec names racing-modification row 71 itself. `gt2tool param-scan <arcade disc>`: 63 x 2 player
rows + 38 opponent rows -> 164 records built, 0 failures; only chassis row 22 (fv30n) is selected by no row.

## 3. Open

- (Answered 2026-09-19, `docs/research/arcade_disc.md` section 16.3) Table 33 = the car selection's SETTINGS bar "Drift"
  (Racing = table 32); the laps = career + 3 (the title's arcade Race Laps option, new game 2), copied over the event's
  settings + 1 by the race build 0x80010C84 of member 2; the opponent draw of the menus' race is member 2's 0x80010238 (the
  same code as 0x80011EC0 of member 3), verified against the original (gt2verify ArcadeBuild) and end to end (three menu runs,
  race block and settings byte for byte).
- `A2P` / `ADT` / `ATT` (2-player battle, dirt / time trials): the build's mode 0 names the race "A2P" with the settings of
  "ATT", mode 6 (Rally and Time Trial both) uses "ATT" - `ADT` is used by no menu path (arcade_disc.md 17.4: Rally = "ATT" on a
  dirt course with the rally cars' own tyre rows). The mode-6 build is ported and verified (gt2verify ArcadeBuild mode-6 cases,
  the original's Rally / Time Trial blocks byte for byte); the mode-0 build is not.
