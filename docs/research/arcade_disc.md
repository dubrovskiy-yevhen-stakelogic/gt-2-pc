# Arcade disc (US Arcade v1.1, SCUS-94455): identification, code map against Sim v1.2, address profile, arcade races

Status 2026-09-19 (second pass: plan steps A, B, C, G done - sections 11..14; third pass: steps D / E in part - the arcade
title and the Single Player / Road Race menus with the race build ported and verified, section 16; fourth pass: the race end,
RESULTS / post-race menu, the career's course win flags, the menu music, the Rally / Time Trial menus and build, section 17). Target: US Arcade v1.1, EXE `SCUS_944.55` SHA-1 **231f9dba7191b9ef915621662afdc40a7c66df95**.
Reference: US Simulation v1.2, EXE `SCUS_944.88` SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a ("Sim"). Addresses
are marked "Sim" or "Arcade". Evidence = bytes of both discs, our tools (`gt2tool exe-map`, `param-scan`, `vol-index`),
objdump, and scripted runs of the original in our interpreter (`gt2run session` / `calltrace`, outputs in
`work\re\arcade_*`, gitignored). Facts keyed by the Arcade EXE hash: `db\arcade_us11_symbols.yaml`.

## 0. Summary

- The Arcade disc is the same code base as the Sim disc, **built from nearly the same sources three weeks earlier**
  (overlay gzip mtimes 1999-12-08 vs 1999-12-29). Of the functions our port uses (src\game\sim + src\game\camera,
  327 code addresses) **48 are at the same address, 274 moved, 5 flagged changed** - and only two of those five are
  real logic changes (race frame pause input; a Sim-only tail of the wheel-effect pass). Physics, tyres, drivetrain,
  AI, catch-up AI, car setup and all race cameras are instruction-identical modulo relocations.
- Data moved by small constant deltas (race-overlay tables -0xE0, EXE .data -0x2BC/-0x308, race state and car array
  -0x310, BSS globals -0x5A0/-0x5D0). The race tuning bytes and the EXE trig/gear tables are **byte-identical**.
- Our readers work on the Arcade disc unchanged: VOL (10618 files), 1110 car models, 125/125 tracks,
  `carparam/usa_arcade_data.dat` -> 63/64 native parameter records; the player's record in a real arcade race equals
  the native record byte for byte.
- The Arcade disc **boots in our interpreter** (intro movie is black - no MDEC; Start skips it) through title, the
  arcade menus (Single Player -> Road Race -> level -> car -> course) to a 6-car race on Tahiti Road; RAM dump
  `work\re\arcade_race\ram.bin`.
- `gt2verify` on that dump with call translation (`GT2_VERIFY_SIM_DISC`, new): 66 rows pass with real cases (all
  fixed-point / trig-free math, drive shafts, drivetrain, tyre curves, setup stages...), 27 fail and ~12 row groups
  cannot run - every failure is a Sim data address used by the harness (tables, globals, car array). What the
  harness needs: a data-address layer (section 7).
- ~~`gt2game <arcade.bin>` stops at once~~ (first pass). Second pass: every table / global the port and the harness name
  by their Sim address goes through a per-EXE **address profile** (section 11); `gt2game <arcade.bin> --race` and the
  arcade race `--arcade` run; the whole gt2verify suite runs on the arcade dump (223 rows ok, 0 FAIL) and the native
  arcade race equals the original's frame by frame (6184 frames, 0 byte differences; section 14). The arcade shell's car /
  AI / level rules come from `carparam/usa_arcade_data.dat` tables 30..33 (section 12, `docs/formats/arcade_data.md`).

## 1. Identification

| | Arcade v1.1 | Sim v1.2 |
|---|---|---|
| Boot EXE | `SCUS_944.55` 231f9dba7191b9ef915621662afdc40a7c66df95 | `SCUS_944.88` 3030aa27... |
| pc0 / t_addr / t_size | 0x8005D570 / 0x80010000 / 0x99000 | 0x8005D600 / 0x80010000 / 0x99000 |
| GT2.OVL | e2a4d8e905224d6534c54887d08f0076e32decfa, 6 gzip members | 6 members |
| GT2.VOL | 972c7a65e0cf3c43cce84b219a84da88d98cbdfd, 10618 files | 11578 files |
| Other root files | MUSIC.DAT (XA), STREAM.DAT (FMV, Arcade only) | MUSIC.DAT |

GT2.OVL members (inflated SHA-1; all load at 0x80010000; entry table Arcade 0x80090EB8 / Sim 0x80091174):

| # | SHA-1 (Arcade) | Size | Entry | Role, seen in the Arcade run |
|---|---|---|---|---|
| 0 | 7360263f7c37eeac154e14d17d923287fe46b28f | 316,776 | 0x80012254 (race uses 0x80011F64) | race overlay (entered from ovl3 via 0x8005DA48(0, 0x80011F64)) |
| 1 | 20bb63ff66247bafc19cf1e9d939c90aa0871e2d | 245,144 | 0x800112D0 | title ("ARCADE MODE DISC", Start Game / Replay Theater) |
| 2 | 304ee2b35f0b3dc430394cbfbe14f670a3353cf3 | 275,312 | 0x80011750 | arcade menus (`10ArcadeMenu`, `16ArcadeRacingMenu`) |
| 3 | d31f01582a5c37f0a661fa378dfb62526d11ea23 | 11,500 | 0x80012C00 | race launcher between the arcade menus and ovl0 |
| 4 | 5339c790770cd9c93ee4ca4cc8b468cb19b49fe3 | 272,888 | 0x80013628 | GT-mode menus (present, not used by the arcade flow) |
| 5 | c40ec257e8e9b163394827a87599046aa6e3fc7c | 8,416 | 0x800114B8 | movie player (intro FMV from STREAM.DAT) |

Boot flow (session `work\re\arcade_nav`, calls of 0x8005D9AC overlay_select / 0x8005DA48 overlay_load):
f476 ovl5 (intro movie; unlike the Sim disc, whose main starts ovl1) -> Start at f600 -> f606 ovl1 title -> f1321
ovl2 arcade menu -> screens "ARCADE MODE" (Single Player / 2 player Battle / Bonus Items / Load Guest Garage), "GAME
SELECTION" (Road Race / Rally / Time Trial), "LEVEL SELECTION" (Easy / Normal / Difficult), "CAR SELECTION" (with AT /
MT), "COURSE SELECTION" -> f4267 ovl3 -> f4329 ovl0 at 0x80011F64 -> race start ~f5160 (2 laps, 6 cars). The input
script that reaches the race: `600:start,700:cross,900:start,1100:start,1300:cross,2000:cross,2400:cross,2700:cross,
3000:cross,3300:cross,3600:cross,3900:cross,4200:cross,4500:cross,5200:cross:2000` (defaults everywhere).

## 2. VOL layout

`gt2vfs` reads the Arcade GT2.VOL unchanged (`gt2tool ls`: 10618 files). By path the Arcade VOL is a strict subset of
the Sim VOL (0 Arcade-only paths, 960 Sim-only: carlogo 638, license 180, dirt 81, gtmenu 22, carparam 17, arcade 17,
...); 40 common paths differ in size (content differences: `scout_vol.md` section 7). File **numbers** differ between the
discs (fewer files): the game code carries them as immediates (section 5.3) - our code addresses files by path, so
this does not matter to gt2game. New: `GtfsEntry::index` (the VOL offset-table entry = the argument of the game's loader
Arcade 0x8005D7B8 / Sim 0x8005D848) and `gt2tool vol-index <disc> [hex numbers]`.

## 3. Data the arcade mode reads (observed)

All 42 calls of the VOL loader 0x8005D7B8 from boot to the race start (`work\re\arcade_files`, resolved with
`gt2tool vol-index`):

- boot: `.crsinfo`; title: `arcade/arc_key_config.tim`, `arc_font.tim`, `arc_fontinfo`, `.carinfoa`,
  `arcade/topmenu_panels_us.tim`, `title_item.tim`, `title_arcade_us.tim`;
- arcade menus: `arcade/arc_panels_us.tim`, `arc_maker.tim`, `arc_other.tim`, `arc_goodies_us.tim`,
  `course_mapinfo`, **`carparam/usa_arcade_data.dat`** (the arcade car tables), `.carinfoa`, the selected car's
  `carobj/<id>.cdo/.cdp` (menu model), `sound/arcade.seq` (menu music; SEQG like the GT menus);
- race: `.text/data-race.txd`, six cars `carobj/{ccrcn,a28sn,io19n,n24vn,k2zzn,m2g6n}.cdo/.cdp` (player = ccrcn,
  Corvette Coupe), `crsmap/tahiti_t.tim`, `crsobj/tahiti_t.trp/.tro`, `bgsobj/t_sky.bsp/.bso`, `crstim.arc`,
  `.crstims.tsd`, `arcade/game_status_files`. Race music is XA from MUSIC.DAT as on the Sim disc.

Strings of the arcade menus are in `data-arcade.txd` inside ovl2 (scout_exe.md section 4).

## 4. What already works on this disc

| Check | Result |
|---|---|
| `gt2tool ls / vol-index` | 10618 files, file numbers resolve |
| `gt2tool car-scan` | 1110 `.carinfoe` entries, 1110 cars found, 2220 models scanned ("bad names" 28, Sim 22) |
| `gt2tool track-scan` | 125 tracks, 125 parse (Sim: 126) |
| `gt2tool param-scan <arcade.bin>` | stock scan: 63 of 64 chassis rows (`tcegn` has no stock racing-modify row); arcade rules (section 12): 63 x 2 player rows + 38 opponents -> 164 records, 0 failures; `tcegn` = player row 55 `tcerr` with its own racing-modify row 71 |
| `... --compare carparam/usa_gtmode_data.dat` | all 63 differ from the GT-mode tables (tyre curves and grip % mostly, a few torque / spring / downforce / drive-type fields) |
| `... --dump work\re\arcade_race\ram.bin 801DE31A` | player ccrcn and AI k2zzn: native record == race record, 0 bytes; a28sn / n24vn / m2g6n: only `engine.torqueMultiplier1000` (table 1040 / 1160 / 1110, race 1000); io19n: tyre curves (another tyre row chosen by the shell) |
| `gt2game <arcade.bin> --race --track tahiti_t --car ccrcn` | runs (address profile; GT-mode car tables of this disc); screenshot `work\play\arcade\race_tahiti_ccrcn.png` |
| `gt2game <arcade.bin>` / `--arcade [--arcade-level N] [--arcade-class C] [--opponents ..]` | the arcade race of the menus (section 13); `work\play\arcade\arcade_a0a_start.png`, `arcade_a0a_race.png` |

## 5. Code map Sim v1.2 -> Arcade v1.1

### 5.1 Method (`gt2tool exe-map`, `src\gt2formats\exe_map.*`)

1. Per module (EXE and the six members): k-grams (12 words) of relocation-masked words unique in both builds anchor
   the images; the longest increasing chain gives runs of constant delta, extended word by word.
2. A function of Sim is compared word by word with its aligned counterpart: a difference is a **relocation** when it
   is a jal/j whose Sim target maps to the Arcade target, a lui of a RAM address, or the low half of a lui-built
   address (straight-line tracking, `addu` index propagation, and a last-lui fallback accepted only when the address
   pair agrees with the data map). Everything else is a **real difference** (constants, registers, branch offsets,
   opcodes, length). Classes: `same` (no real difference, same address), `shifted` (other address), `changed`,
   `unmapped`.
3. Data map: every lui-built address pair of aligned code (per overlay scope; EXE/BSS global).

Command: `build\gt2tool.exe exe-map "<Sim.bin>" "<Arcade.bin>" --db db\sim_us12_symbols.yaml --src src\game\sim=ovl0
--src src\game\camera=ovl0 --out map.tsv --data-out refs.tsv --yaml-out facts.yaml --all` (0.3 s).
Function boundaries are heuristic (jal targets, "jr ra" followed by a prologue, the queried addresses); a function
that ends in a data island is flagged at the island (tyre_smoke_draw at 0x8002EECC is such a case).

### 5.2 Counts

All heuristic functions (>= 8 instructions, containing a `jr ra`):

| Module | same | shifted | changed | unmapped |
|---|---|---|---|---|
| EXE | 19 | 847 | 17 | 1 |
| ovl0 race | 79 | 556 | 27 | 1 |
| ovl1 title | 13 | 224 | 21 | 8 |
| ovl2 arcade menus | 12 | 275 | 9 | 1 |
| ovl3 | 16 | 0 | 0 | 0 |
| ovl4 GT menus | 222 | 44 | 9 | 0 |
| ovl5 movie | 44 | 0 | 1 | 0 |

Ported code (every 0x80xxxxxx literal in the sources, comments included; `map_all.tsv`):

| Source tree | code addresses: same / shifted / changed |
|---|---|
| src\game\sim + src\game\camera (priority) | 48 / 274 / 5 (327) |
| src\gt2formats (EXE builders, tables) | 4 / 162 / 6 |
| src\game\audio | 5 / 50 / 1 |
| src\game\shell (title) | 3 / 77 / 2 |
| src\game\menu, src\game\career (GT mode, ovl4) | 173 / 108 / 3 |
| db\sim_us12_symbols.yaml (named) | 100 / 212 / 15 (+1 unmapped) |

### 5.3 The "changed" functions of the priority set, explained

| Sim | Arcade | Function | Difference | Port impact |
|---|---|---|---|---|
| 0x80015B64 | 0x80015B64 | race_frame (race_shell / race_sim) | 178 words: the Sim version tests a second pad slot (race object + 0x140) and a flag table 0x801D98E0 + 0xBF7C for the pause / quit input; the Arcade version reads pad 1 only | arcade variant of the pause check (small) |
| 0x800426F0 | 0x8004269C | UpdateWheelEffects (ground.cpp) | the Arcade function ends after the effect levels; the Sim tail (contactType == 2 && 0x800A951C == 0 && (0x801C9995 == 0 or 0x800AF232 == 0) -> clear skid / smoke / dust and body + 0x757..0x759, `ground.cpp` line ~769) does not exist | flag off for the Arcade build |
| 0x80012CD4 | 0x80012CD4 | race_car_entry_setup | 1 word: `sb zero, -0xB50(v0)` -> `-0xBA4`: Sim 0x8002F4B0 -> Arcade 0x8002F45C = the ovl0 data delta -0x54 (lui on another path) | none (relocation) |
| 0x8002E390 | 0x8002E33C | start-signal part of StartRace | `li a0, 0x26` -> `0x1F`: VOL slot number for loader 0x8005D8D4 (Arcade 0x8005D844) | none (we read by path) |
| 0x80046BAC | 0x80046ACC | (data island after the code block) | not a function | none |

The other one-word `changed` entries outside the priority set are the same kind (VOL file / slot numbers as
immediates: car_envmap_load 0x6D -> 0x5A, race_menu_uploads 0x0E -> 0x0C, gt_load_used_car_period 0x0B -> 0x09,
options_page_callback, change_parts_page_draw...). Genuinely different code: race_load_strings 0x80028CC0 (unmapped;
the race strings loader), race_overlay_entry (calls it), title_entry (ovl1, 181 words), hud_start_digits_and_licence_prize
(3 words), UpdateWheelEffects, race_frame.

Key addresses (all "shifted", 0 real differences): physics core 0x80039FC8 -> 0x80039F74, step driver 0x80034480 ->
0x8003442C, frame driver 0x8003EBF0 -> 0x8003EB9C, car setup 0x800319A8 -> 0x80031954, AI 0x80037834 -> 0x800377E0,
catch-up 0x80041E4C / 0x80042230 -> 0x80041DF8 / 0x800421DC, race tuning 0x8003BA64 / 0x8003B7B8 -> 0x8003BA10 /
0x8003B764, race cameras 0x80010000..0x800109FC same addresses, param builder 0x800771AC -> 0x800770BC, inflate
0x80082FAC -> 0x80082EBC. Full list: `db\arcade_us11_symbols.yaml` (generated block) and `work\re\arcade_map\map_all.tsv`.

### 5.4 Data deltas (lui-built references of aligned code, >= 10 references per run)

| Range (Sim) | Delta | Contents |
|---|---|---|
| EXE resident code | -0x90 (loaders 0x8005Dxxx), -0xF0 from ~0x80060000 | e.g. 0x80060840 -> 0x80060750, 0x80082FAC -> 0x80082EBC |
| EXE .data 0x8008F870..0x8009118C | -0x2BC | rodata / tables |
| EXE .data 0x80091570..0x800A94C1 | -0x308 | trig tables 0x80093150 -> 0x80092E48, atan 0x800A4AC8 -> 0x800A47C0, gear table 0x800923E2 -> 0x800920DA |
| 0x800A94D0..0x800AF232 | -0x310 | race object 0x800A9500 -> 0x800A91F0, car array 0x800A9688 -> 0x800A9378, course / car count 0x800AF230 -> 0x800AEF20 |
| BSS 0x801C8690..0x801E2EF0 | -0x5A0 | race tuning records 0x801C8690, race settings 0x801C98A0 -> 0x801C9300, shell bytes 0x801D5860 -> 0x801D52C0, param records 0x801DE8BA -> 0x801DE31A |
| 0x801C8568..0x801C8670 | -0x5A8 | frame constants 0x801C856C / 0x801C8570 -> 0x801C7FC4 / 0x801C7FC8 |
| 0x801EF5F0.. / 0x801F0230.. | -0x5D0 / -0x610 | overlay loader state 0x801EF610 -> 0x801EF040, VOL offset table ptr 0x801E35F0 -> 0x801E3020 |
| ovl0 0x8002EE98..0x8002F924 | -0x54 | camera tables 0x8002F350.. |
| ovl0 0x80046BAC..0x800505AC | -0xE0 | race tuning tables 0x80046C94..0x80046FC8 (raw bytes 0x80046E20 -> 0x80046D40 identical) |
| ovl0 0x80052D84..0x8005D5D4 | -0x90 | race-menu data |

Table contents compared byte for byte (Sim vs Arcade images): race tuning raw bytes (168) identical; gear auto table,
sin/cos, atan identical; camera onboard table: 8 bytes differ (0x8002F4BC..0x8002F4C9, a y/z pair of four entries).

## 6. Runtime bring-up (interpreter)

- `gt2run session <arcade.bin> 4000 ...`: the Arcade main enters **ovl5** (intro FMV) first; our Machine has no MDEC
  (0x1F801824 reads "idle"), the movie loop runs and the screen stays black; Start skips it. Then title, arcade menus
  and race run without any change to the runtime (`work\re\arcade_boot`, `arcade_nav`: screenshots of every screen).
- Race dump: `gt2run calltrace <arcade.bin> 6000 60 work\re\arcade_race "<script above>"` -> `ram.bin`, 380 functions
  called (183 in ovl0). The calltrace summary line ("course index 99 Tahiti Road, 6 cars") happens to read the right
  bytes only because it uses Sim addresses for course names that coincide; its game-mode byte is from the Sim address.
- Arcade race state in the dump: shell race mode (Sim 0x801D5866 -> 0x801D52C6) = 4; race settings +0x0C.. = `50 50 50
  50 64 64 64 64 64 00 00 19 0A 04 0A 1E 08`; the catch-up constants (Arcade 0x80046E8C..) = 0x400, 0xA0000, 0x280000,
  409, 0x1E0000, 0x500000, 0x1000 - exactly `catchup_tuning` of those bytes: the ported catch-up AI is active in this
  race. Level chosen: the default cursor of "LEVEL SELECTION" (not established which level that is).

## 7. gt2verify on the Arcade dump

Unchanged harness: `build_arcade\gt2verify.exe work\re\arcade_race\ram.bin <arcade.bin> tahiti_t` - every guest call
goes to a Sim address: the first row already fails and the run dies at row 9 (unmapped read).

New, additive: `GT2_VERIFY_SIM_DISC=<Sim disc>` -> `tools\gt2verify` builds the cross-build map at start and
`Guest::Call / CallWithBios / CallUntil` translate Sim function addresses (race dumps: ovl0 below 0x8005D5F8, EXE
above). Data addresses are not translated. Without the variable nothing changes (Sim suite: section 9).
Results per group (`GT2_VERIFY_SKIP` = all other groups; logs `work\re\arcade_race\verify_g_*.txt`):

- **pass with real cases (66 rows)**: Mul12/16/8 (+Wide/Floor/Sh*), ApproxLen, Falloff, Div64, Div12Shift, Interp,
  UpdateFootprint, PadInput, Displace; Core: ClearNbr, SgClutch, SgRpm, SgSelect, Length3, Neighbour, CarPair, Push;
  DriveShafts: all 15 (WrapAngle .. Drivetrain 0x80046B58, BoostSum, EngineStep...); Drivetrain: EngineSpd, EngineRpm,
  SelectGear, UpdateGear, Traction; Setup: MapWeight, DriveClass, WheelRad, Damper, Engine, SlipRatio, SlipAngle,
  LoadCamber, SuspTravel, ResetView; Tyres: SquareRoot, InterpS16, InterpPair, SymCurve, WheelSlip, SlipRatios; Career:
  RaceStats, PrizeTot, SetResult; License: ResultInit. 8 more rows print ok with 0 cases (the guest trapped).
- **fail (27)**: SinTable, CosTable, AtanTable, Atan2, StepTime, Aero, WallTest, BrakeAssist, Corners, Proximity,
  PairSweep, CarContact, DriveLayout, Suspension, GearAuto, Drivetrain (0x800347C4), PeakSlip, TopSpeed, SetupCar,
  ResetDyn, ResetWear, RandIdx, GridArgs, ExeImage, OvlImage, CrsInfo, CourseId - each reads a table or global at its
  Sim address (0x80093150, 0x801C856C, 0x800A9520, 0x80046Fxx...) or compares the Sim images.
- **cannot run**: Ai / Shell (race lines looked up at Sim addresses), Camera (camera object 0x801FF97C), Params
  (expects the attract replay), Ground / Race (course chunk count via Sim globals), MoveBody / MovePass / CourseSweep /
  Mirror (unmapped reads), Music (Sim music table), Sound (process ends silently), Data (tuning tables).

What the harness needs for a full Arcade run (estimate 16-24 h): one address function `verify::A(simAddress)` (or
the same `gCodeAddressMap` idea for data) used for every data literal in `tools\gt2verify\*.cpp` (~1000 literals, mostly
mechanical), backed by the exe-map data map + explicit entries for addresses without a code reference; the same map
for the native side where the port reads guest globals (`sim::Field` / `GroundGlobals` constructors in the harness);
per-EXE expectations (row `ExeImage` / `OvlImage` against the dump's own disc). Rows whose code really differs
(race_frame, UpdateWheelEffects) need an Arcade variant of the native function.

## 8. Plan: the Arcade disc in gt2game

| Step | Content | Estimate |
|---|---|---|
| A. Build profile | `ExeProfile` keyed by EXE SHA-1 (Sim v1.2, Arcade v1.1): the ~60 table / data addresses gt2game reads from the disc images (disc_data.cpp 16, hud_assets 13, race_audio 6, race_menu_assets, title_assets, gt_menu_* ...). Generated by `gt2tool exe-map` into a data table (facts only, regenerated, never hand-edited); unknown EXE -> clear error. Then `--race` / `--selftest` on the Arcade disc. | 8-12 h |
| B. Arcade car data | `usa_arcade_data.dat` + `.carinfoa` for arcade races; the shell's AI rules seen in the dump (torque multiplier forced to 1000, tyre row choice), `tcegn` (no stock racing-modify row); course list and names from ovl2 tables / `.crsinfo` | 8-16 h |
| C. Arcade race rules | shell race mode 4 (catch-up on - already ported), lap counts, difficulty -> race settings bytes (+0x0C..+0x1C) from ovl2/ovl3, Road Race / Rally / Time Trial differences, race_frame pause variant, UpdateWheelEffects without the Sim tail, arcade HUD strings (`data-race.txd` of this disc), results screen of ArcadeRaceLoop | 24-40 h |
| D. Arcade menus (ovl2, 378 functions, ~92 KB code) | the five screens seen + 2P battle setup, bonus items, guest garage; assets `arcade/*`, `data-arcade.txd`, menu music `sound/arcade.seq` (our SEQG sequencer) | 60-100 h |
| E. Arcade title (ovl1: 224 shifted, 21 changed) | title variant, `title_arcade_us.tim`, options | 8-16 h |
| F. Intro FMV | STREAM.DAT demux + MDEC/iki (PLAN M9); independent | 40-60 h |
| G. Verification | step 7's data-address layer in gt2verify; arcade race dumps per mode; `--race-capture` frame comparison of an arcade race | 16-24 h |
| H. 2P battle | split screen, second pad | later |

Mode selection: gt2game picks the profile from the disc's EXE (`SCUS_944.55` -> arcade flow: title -> arcade menus;
`SCUS_944.88` -> as today). A Sim-disc-only user can already race the arcade courses with GT-mode cars; true arcade
behaviour (car tables, AI power, rules) needs steps A-C (~40-70 h); the full arcade product A-E ~110-180 h.

## 9. Sim results kept

`build_arcade\gt2verify.exe work\re\race_demo\ram.bin "<Sim disc>" seattle` - all rows ok, exit 0 (log
`work\re\arcade_race\sim_regression.txt`); `build_arcade\gt2game.exe "<Sim disc>" --selftest --cars 6 --no-sound` -
0 failures. Changes are additive: `GtfsEntry::index`, `gt2formats\exe_map.*`, gt2tool commands `exe-map`,
`vol-index`, `param-scan`, the optional call translation in gt2verify.

## 10. Open questions (first pass; see section 15 for the state after the second pass)

- Which level the default cursor of LEVEL SELECTION is, and how level -> race settings bytes / AI cars / torque
  multiplier (ovl2 / ovl3 data).
- `tcegn` in `usa_arcade_data.dat` has no stock racing-modify row: which row the arcade shell uses.
- ovl3's exact role (race launcher: loads ovl0 directly with entry 0x80011F64; on the Sim disc never loaded).
- race_load_strings 0x80028CC0 (Sim) has no aligned counterpart: the Arcade version of the race string loader.
- The 8 differing bytes of the onboard camera table (0x8002F4BC..; Arcade 0x8002F468..).
- Whether the Arcade EXE's "Japan area" region marker has any effect (scout_disc.md).

## 11. Address profile (plan step A)

`src/gt2formats/exe_profile.*`: `ExeProfile` per build, selected by the SHA-1 of the disc's executable (`ProfileOf(disc)`,
`GuestImage::profile` set by `LoadExeImage` / `LoadOverlayImage`; unknown EXE -> a clear error). Sim v1.2 is the reference
(identity); US Arcade v1.1 has 189 ranges in `src/gt2formats/exe_profiles.inc`, GENERATED from `db\arcade_us11_symbols.yaml`
by `gt2tool gen-profile --build ArcadeUs11=db\arcade_us11_symbols.yaml --out src\gt2formats\exe_profiles.inc`:

- `kind: data` (4 hand-established, verified facts): race state + car array 0x800A94D0..0x800AF240 (-0x310), course start
  lines 0x800B4A34.. (-0x310), the race task's view / camera object 0x801FF8B8.. (**+8**: the race task's stack frame),
  the race heap records 0x80169894.. (-0x310: licence database, settings sheet);
- `kind: data-range` (148 runs, GENERATED by `gt2tool exe-map <Sim> <Arcade> --ranges-yaml`): runs of lui-built data
  references of aligned code with one delta, [first, last reference]; dropped (28): runs whose references the image's
  word alignment places at another delta (constants such as lui 0x8009 / addiu -1) and isolated runs of < 3 references
  between two runs of another common delta;
- `kind: aligned-range` (37 runs of the word alignment of the EXE and the six members): code and static data.

Lookup order fact > reference run > aligned run; scope = the module (`GuestImage::module`, race map: member 0 below
0x8005D5F8, else the EXE / RAM). Every port address goes through it: `GuestImage::Sim(sim)` for the disc images (disc_data,
race cameras, HUD tables / fonts / race text copy, race audio, particles, sponsor slots, music table, shell tables, career
EXE tables), `RaceAddress()` / gt2verify's `D()` for guest RAM. `gt2tool profile-check <arcade.bin> <dirs>`: of the 1648
0x80xxxxxx literals of src\game, src\gt2formats, src\gt2view, tools\gt2game and tools\gt2verify 1612 resolve; the other
36 are harness-owned free RAM, comments, title / GT-menu addresses (member 1 / 4, not part of the arcade flow) and the
corresponding race-screen files.

Logic differences selected by the profile: `wheelEffectsTail` (UpdateWheelEffects: the Arcade function 0x8004269C ends at
0x80042FF4 right after the engine visuals, the Sim tail 0x8004304C..0x800430D4 does not exist; `GroundGlobals`,
`SimConstants`) and `raceFramePadSlot2` (race_frame 0x80015B64: the Sim build also polls pad slot 2 / the flag table for
pause; gt2game pauses from the keyboard = pad 1 in both builds, so the flag has no effect until a second pad is ported).
Harness layout adapter: the contact state's corner tables sit 8 bytes further from its head block in the Arcade build
(head 0x801C8608 -0x5A8, corners 0x801C8740 -0x5A0); `SimContactLayout` (tools/gt2verify/guest.h) shows the native side the
Sim layout. Race text: the race's copy of `.text/data-race.txd` is at 0x801C6940 (Sim 0x801C6C50), the lap-time caption
token 0x801C6954 (`ShellGlobals::lapTimeLabel`).

## 12. Arcade car tables and AI rules (plan step B)

`docs/formats/arcade_data.md`: `usa_arcade_data.dat` tables 30 (15 events `A<level><class>`, `A2P`, `ADT`, `ATT`), 31 (38
opponent rows), 32 / 33 (63 player cars, two tyre choices). The launcher ovl3 0x800121DC: player = `ConfigFromCarSpec`
(table 32 row, no GT-mode flag), AI = `OpponentCarConfig` of drawn opponent rows (0x80011EC0), grid 5 - i. Level = the
event's settings block only: AI corner grip 80..100 % by level and class, the catch-up tuning bytes; the AI's power is not
level-dependent (torque multiplier 100 in every opponent row: the dump's 1000 against the tables' 1040 / 1160 / 1110). The
io19n tyre difference = its opponent row's tyre row 61. `tcegn`: only via player row 55 `tcerr` (racing-modify row 71 named
by the row). The dump's race = event A0A (Easy, class A): the default cursor of LEVEL SELECTION is Easy.

## 13. Arcade race in gt2game (plan step C)

`tools/gt2game/arcade_race.*`: `--arcade` (default on the arcade disc without flags: its title / menus are not ported)
builds the race like the launcher - event `A<level><class>` (`--arcade-level 0..2`, `--arcade-class A|B|C|S`), the player's
car row (`--car`, `--arcade-tyres 32|33`), opponents drawn from the event (`--arcade-seed`) or given (`--opponents
16,15,21,31,20`), game mode 4 with the dump's race block bytes (0x801D585D / 0x801D5860 / 0x801D5865 = 0, frame-rate mode
2, countdown for a standing start), the event's settings block (catch-up AI on), grid 5 - i, HUD layout 2. Headless with
the dump's line-up and `--ai-player`: all six cars finish 2 laps (player 2:56.312). Not done: the arcade menus / title
(step D / E), results screen of the arcade shell, Rally / Time Trial / 2P modes, the lap count per course.

## 14. Verification on the Arcade disc (plan step G)

- `build_arcade2\gt2verify.exe work\re\arcade_race\ram.bin "<Arcade disc>" tahiti_t` (no environment variable: the
  profile comes from the disc): **223 rows ok, 0 FAIL, exit 0** (log `work\re\arcade_race\verify_arcade_final.txt`), including
  the new rows `ArcadeEvent` (settings block = A0A) and `ArcadeCars` (6 entries = the arcade rules, records 0 bytes). Rows
  skipped because the dump does not hold their subject: BuildParams (attract replay), License, Career garage / Menu / Title
  (members 4 / 1 not loaded).
- `gt2verify --race-capture "<Arcade disc>" 17000 work\re\arcade_capture\race.bin "<the script of section 1>"` (hooks
  through the profile; writes `race.bin.setup` = the race block + settings block) and `gt2game "<Arcade disc>" --race
  --track tahiti_t --frames-compare work\re\arcade_capture\race.bin` (the capture's race from the setup file):
  **6184 frames (fields 4614..16999), 0 differ** - six car records and player 1's replay stream byte for byte.

## 15. Open questions (state after the second pass)

- Table 33 (second tyre choice): which menu option selects it.
- Lap count per course (ovl2 course table), Rally / Time Trial / 2P modes (events `ADT`, `ATT`, `A2P`, other game modes).
- 0x80011EC0's draw is ported with the structure of the GT-mode picker, not compared against a run.
- The 8 differing bytes of the onboard camera table (0x8002F4BC..; Arcade 0x8002F468..): read through the profile, their
  meaning not established.
- race_load_strings 0x80028CC0 (no counterpart), ovl3's role beyond the launcher, the "Japan area" marker.

(2026-09-19: table 33, the laps and the opponent draw are answered in section 16.)

## 16. Arcade title and menus (plan steps D and E, 2026-09-19)

All addresses in this section are ARCADE v1.1 addresses (EXE 231f9dba..., member 1 SHA-1 20bb63ff..., member 2 304ee2b3...).
Evidence: our disassembly of members 1..3 (`work\re\arcade_menu\ovl*.s`), Ghidra pseudo-C of the RAM at the race build
(`work\re\arcade_menu\ram.bin` = gt2run session snapshot f4226, project `work\ghidra\gt2_arcade_menu`, 735 functions in
`work\re\arcade_menu\decomp`), session runs with watched writes / call logs (`work\re\arcade_menu\s_calls`, `s_trace`, `s_watch`,
`s_drift`, `cmp1..3`) and GP0 captures (`gt2play --prims`: `work\play\arcade_menu\cap`, `cap2`). Facts: `db\arcade_us11_symbols.yaml`
(section "arcade menus"). Port: `src\game\arcade\*` (library gt2arcade), `src\gt2formats\arcade_menu_data.*`,
`tools\gt2game\arcade_mode.*`, rows `tools\gt2verify\verify_arcade_menu.cpp`.

### 16.1 Title (member 1)

The arcade title is the Simulation title's code (every title function "shifted" in section 5.2 except the entry): the same list
widget and rows (0x8004B0D8 results {-1, 0..5, -1}, sprite index 0x8004B0E8, widget 0x8004B0FC = Simulation 0x8004BC04 / BC14 /
BC28 - 0xB2C, the profile's reference run 0x8004BA00..0x8004C8A9). Its background is `arcade/title_arcade_us.tim`; Start Game
(result 0) loads member 2 (0x8005D9AC(2) from 0x80011434). The texts `data-title.txd` / `data-global.txd` of this member are other
builds (24514 / 13321 bytes, other block layout): the options / card screens are not mapped. Native: `shell::TitleMenu` now reads its
tables through `GuestImage::Sim()` (identity on the Simulation disc), `TitleAssets::LoadArcade`.

### 16.2 Menu structure (member 2)

Entry 0x80011750: 0x80011954 (0x80013BC8: uploads - panels_us -> page 6, arc_maker -> 0x0B, arc_font -> 0x1E, arc_other -> 0x0F,
goodies_us -> 0x1C (file ids via the arcade file table 0x801E2950), `usa_arcade_data.dat` -> *0x80092B64, data-arcade.txd block
language x 0x2E0 -> 0x800F81E0), then the view loop (update 0x8001419C, draw 0x80014544), then 0x801EF024 (Simulation 0x801EF5F4):
0 -> member 1, 1 -> race (0x80011868), 2 / 3 -> member 5. Views {init, update, draw, colour, title} (0x54 bytes):

| View | Update | Rows / behaviour |
|---|---|---|
| root 0x80051FEC | 0x8001D5DC | first init (0x8001D54C): garages, course counts 0x8001D210, class unlocks 0x8001D418, list kind 5; after 1 field -> ARCADE MODE; re-entered -> after 24 fields result 4 (title) |
| ARCADE MODE 0x80052040 | 0x8001D6C8 | Single Player / 2 player Battle / Bonus Items / Load Guest Garage (list 0x8004F8BC); selection + 4 = career + 3 (Single Player) / + 6 (2P); cursor 0x801D5004 |
| GAME SELECTION 0x80052094 | 0x8001D8F0 | Road Race / Rally / Time Trial (0x8004F938): selection + 2 = 0x8004F8EC[row] = 4 / 6 / 6, + 0x2CC = 1 for Rally, list kind 0 / 2 / 1; -> LEVEL / CLASS 0x800521E4 / CLASS 0x80052190 |
| LEVEL SELECTION 0x8005213C | 0x8001DE14 | Easy / Normal / Difficult (0x8004F9FC; a fourth row when 0x80023574 != 0 - it returns 0 in this build) -> selection + 0 |
| CLASS SELECTION 0x80052190 | 0x8001E094 | rows 0x8004FA3C (A, B, C, Home Garage, Guest Garage) or 0x8004FA28 (+ S when 0x800F365C); garage rows disabled while empty; selection + 1 = row result (0 S, 1 A, 2 B, 3 C), + 6 = -1 |
| CAR SELECTION 0x80052238 | 0x8001EAA8 | class list 0x80051F14[class] (S 10, A 8, B 9, C 9, rally 4, bonus 24 cars); left / right = car (paint = index % paints, 0x80016530), up / down = colour (0x80015E9C); cross -> TRANSMISSION AT / MT (+ 0x18 = 0x8004FBD0[bar]) -> SETTINGS Racing / Drift (+ 0x14 = bar) |
| COURSE SELECTION 0x80052334 | 0x80022D98 | course list by kind (0x800228BC: road 0x80050730 / reverse 0x800509F0, time trial 0x80050CB0 / 0x80050FB0, rally 0x800512B0, 2P 0x800513F0 / 0x800516B0; 0x20-byte rows {file, name, map info, flags, id, tier, record, open}); cross -> + 0xB8 name, + 0x1B8 id, + 0x1BC record, + 0x2D0 -> final view |
| (final) 0x80052388 | 0x80023440 | after 24 fields result 3 -> race |
| 2PLAYER BATTLE 0x800522E0, HOME / GUEST GARAGE 0x8005228C, LOAD GUEST GARAGE 0x800523DC, BONUS ITEMS 0x800525C8 | | not ported |

Transitions: a push calls the new view's init and runs 16 fields in which the old view is still updated without input
(0x8001419C); both are drawn, the old one with alpha t x 128 / 16 and draw offset y (t - 16) x 5, the new one with 128 - that and
offset t x 200 / 16 (pop: mirrored, 0x80014470). Header 0x80013828: title in the cell-12 font, spacing 0x22 - alpha / 4, text colour
(alpha x {0xF5, 0x5C, 0x19}) >> 8 at y 0x4C, shadow at +3, +3, underline TILE (x + 1, 0x4E) >> 7, gradient POLY_G4 y 0x2D -+ alpha
x 0x2D >> 7 from the view colour. Panel list 0x8001B6C0..0x8001BB6C: rows reveal every 6 fields after a 24-field delay (ghost
sprites spreading in), highlight sweep 0x8006B724 with period 61, arrows blinking with period 46, scroll of 8 fields, close = the
other rows shrink at once and the chosen one 7 fields later.

### 16.3 The selection and the race build (rules)

The selection (RAM 0x801C3010, 0x2D4 bytes; writers watched: `s_watch`) and the build 0x80010C84(p1, p2, 0x801C3010) are in
`db\arcade_us11_symbols.yaml` and `src\game\arcade\arcade_setup.h`. The builder 0x80010554 is member 2's instruction-identical copy of
Simulation ovl4 0x80010A30 (and of ovl3 0x800121DC); ovl3 rebuilds the entries only for a garage car (+ 0x2C8 != 0) - section 12's
"ovl3 0x800121DC" is the same code but NOT the one the Single Player race runs (`s_calls`: every config / record call at f4228 comes
from member 2). Answers of section 15:

- **Laps** = selection + 4 = career + 3, the title's arcade "Race Laps" option (new game 2; 2P: career + 6), written at ARCADE MODE
  (pc 0x8001D7DC) and copied to race block + 0x0F by 0x80010C84 over the event's settings + 1.
- **Table 33** = the SETTINGS bar of the car selection: Racing = 0 -> table 32, Drift = 1 -> table 33 (0x80010000: 1 selects the
  table-33 offset +0x110 of the GTDT; run `s_drift`: + 0x14 = 1, entry + 0x8F = 1 with MT).
- **Opponents** = member 2's 0x80010238 (= Simulation ovl4 0x80010714), seeded with the VSync counter *0x801F0070 at the build;
  the menu entry's p1 / p2 = 0x800839F0 twice on the same counter.
- Race block + 0x57E = the course record number, + 0x57F = 0x800272DC[level] (01 02 04 01); block + 0x588 bit 0 = (+ 0x2CC != 1).

Verification (`gt2verify work\re\arcade_menu\ram.bin "<Arcade disc>" x`): **ArcadeBuild** 300 random selections (level, class, any
player-table car, tyres, colour, transmission, laps, course, career option bytes, VSync, p1 / p2) - the original 0x80010C84 against
`BuildArcadeRace`, ALL of RAM compared (guest stack excepted): 0 mismatches (a wrong seed fails every case); **ArcadeAvail** 200
random careers - 0x8001D120 on the seven course lists + 0x8001D418 against `CourseAvailability` / `ComputeClassUnlocks`: 0.
Availability: a course tier t opens when the ten tests of GT-mode licence t are passed (career + 0x1418 + t x 0x668, byte +1 of
each 0xA4 record: the shared save); a new game has only the tier -1 rows (road 3, time trial 3, rally 1, 2P 3 + 1).

### 16.4 End-to-end race blocks

Three runs of the original through the menus (`gt2run session ... calls=80010C84 watch=801F0070:4 snap=4335`, `cmp1..3`) and the
same choices in gt2game (`--arcade-vsync` = the original's counter recovered from p1, `--arcade-setup-out`, `--arcade-no-race`):

| Run | Choices | VSync | Race block (0x58C) | Settings (0x40) |
|---|---|---|---|---|
| cmp1 | Easy, class A, Corvette (ccrcn) colour 0, AT, Racing, Tahiti Road | 0x1079 | 0 bytes differ | 0 |
| cmp2 | Normal, class B, gattn (car 2) colour 2, MT, Drift, Midfield Raceway | 0x10A1 | 0 | 0 |
| cmp3 | Difficult, class C, umcon (car 3) colour 2, AT, Racing, High Speed Ring | 0x10B5 | 0 | 0 |

(cmp2 / cmp3 first showed 1 byte, the player's paint: the car change's paint = index % paints rule of 0x80016530, now ported.)

### 16.5 Presentation

Frames of `ArcadeMenus::Frame()` on the software canvas with the interpreter GPU's rules against the captures' VRAM (352 x 480;
`gt2game "<Arcade disc>" --window 352x480 --arcade-square --script ... --arcade-shot N out.png --arcade-compare cap.vram.bin`; the
phases computed from the capture RAM's list counters; images `work\play\arcade_menu\native`):

| Screen | Capture | Native field | Differing pixels |
|---|---|---|---|
| Title (Start Game) | cap\title_1200 | 60 | 0 |
| ARCADE MODE | cap\mode_1900 | 371 | 0 |
| GAME SELECTION | cap\game_2300 | 385 | 0 |
| LEVEL SELECTION | cap\level_2600 | 315 | 0 |
| CLASS SELECTION | cap\class_2900 | 345 | 0 |
| transition ARCADE MODE -> GAME SELECTION (two frames) | cap2\f2006, f2014 | 91, 99 | 0, 0 |
| CLASS SELECTION after down (pulse), 3 phases | cap2\f2853, f2861, f2890 | 206, 214, 243 | 0, 0, 0 |
| CLASS SELECTION scrolled to Class-C, 3 phases | cap2\f2903, f2911, f2950 | 256, 264, 303 | 0, 0, 0 |

The Vulkan frames differ from the captures in the gradient polygons only (header gradient, highlight sweep: float gouraud and the
dither of the E1 0x220 mode, as the Simulation title's frames): 16-21 k pixels in the top 200 rows, the sprites equal.
CAR SELECTION and COURSE SELECTION are drawn in OUR layout from the original's data (car name, colour chips, the three bars of
0x80051F84, the two bars' labels, course number / name, "FASTEST LAP - No Records -"); their widgets (car and maker logos of
arc_carlogo / arc_maker, spec box and power graph, course map of course_map / course_mapinfo, record list) are not decoded. The
3D car of the car selection is the GT-mode menus' car view (`game/menu/menu_car.*`, `gt2view MenuCarView`) in our viewport; the
arcade overlay's own car camera (0x80016218 / 0x80016624) is not ported. (Superseded 2026-09-19: section 18 ports both pages and the
camera.)

### 16.6 gt2game on the arcade disc

`gt2game "<Arcade disc>"` (no race / mode flag; also `--title`) = `tools\gt2game\arcade_mode.cpp`: arcade title -> Start Game ->
ARCADE MODE -> Single Player -> Road Race -> level -> class -> car (+ transmission, tyres) -> course -> the race of the ported build
(`LoadRaceBlock` in arcade_race.cpp, the existing arcade race: frame-exact vs the original, section 14) in the same window -> Esc
after the race -> back to the menus (cursors kept) -> Back from ARCADE MODE -> title. The other title rows, 2 player Battle, Bonus
Items, Load Guest Garage, the garage classes, Rally and Time Trial show a "not available yet" page (the title's Options / Save / Load:
section 17.8). The career block is the new game
of 0x800104A0 (laps 2; only the always-open courses). `--arcade` and its flags still run the quick arcade race. New flags:
`--arcade-square`, `--arcade-shot N png`, `--arcade-compare cap.vram.bin`, `--arcade-frames N`, `--arcade-vsync N`,
`--arcade-setup-out file`, `--arcade-no-race`. Sound: the menu effects (the EXE's 0x80060750 = Simulation 0x80060840); the menu
music `sound/arcade.seq` is not played.

### 16.7 Open (after this pass; the fourth pass answers the result screen, the career writes, Rally / Time Trial's build and the
menu music - section 17)

- The arcade result screen after the race (the race overlay's arcade loop 0x80016F88 -> member 2) and the records / unlock writes
  it makes (career + 0xB8 + record flags; which prize records fill career + 0x1418..).
- Rally (mode 6 with 0x800F0298 = 1: does the build use "ATT" or "ADT"?), Time Trial (event "ATT", block + 0x0F = 100 laps,
  rolling start, 0x8005E674's ghost record), 2 player Battle (mode 0, "A2P", two entries), garages, Bonus Items.
- The car / course selection widgets (logos, graphs, course map, record list), the arcade car camera, the menu music.
- The title's options / save / load on this disc (other txd layouts).

## 17. Race end, results, career writes, menu music, Rally / Time Trial (2026-09-19)

All addresses ARCADE v1.1 (EXE 231f9dba..., member 0 7360263f..., member 2 304ee2b3...) unless marked "Sim". Evidence: gt2run
sessions of the original with the dev capture aid `tools/gt2run/ai_player_arcade.h` (the original's AI drives the player's car:
the car start `jal 0x80033330` at 0x800130DC gets control class 2; tools only), `work\re\arcade_results\r1..r5` (event A0A, Tahiti
Road, the player 1st in 2:56.312 - the same time as gt2game's `--ai-player`), a write watch over the whole career block
0x801C9340..+0x7C9C from the race load to the return to the menus, objdump of the RAM snapshots, gt2play `--prims` captures
(`work\play\arcade_results\cap`) and `--spu-log` (`work\re\arcade_music\spu.txt`). Facts: `db\arcade_us11_symbols.yaml` (section
"arcade race end ...").

### 17.1 The race end in the arcade loop

Member 0's arcade loop 0x80016F14 (Sim 0x80016F88) runs the race state machine 0x80015ED4 for modes 0 / 3 / 4 / 6 and returns 2
(member 2) afterwards. Observed order (Single Player road race):

1. "Finish", the badges and the results table of the race-end display (0x8002B11C = Sim 0x8002B170, sub-mode 4 = the default
   path with the table), held by the X wait (0x8002A6AC = Sim 0x8002A700).
2. X -> the race's **replay starts at once** (the race overlay reloads the race, "REPLAY" + "Replay <car>" captions, trackside
   cameras); Start -> pause -> Exit leaves it (r3: f18600 / 18700 / 18800).
3. 0x80016CC0 (mode 4): when the player's results record 0x801D58E8 +0 > 0 (or player 2's 0x801D9E00): 0x800471F4(M, 0x8005B710)
   = wait view (24 fields) + RESULTS (course, place, TOTAL TIME, FASTEST LAP, lap list, the car, dialog "Next"); then a second
   view-manager run 0x800471F4(M, 0x8005AD7C) = wait + the post-race menu "SINGLE RACE": Replay / Try Again / Save Replay ... /
   Exit (+ RESULTS / TOTAL TIME / FASTEST LAP and the turning car). Exit -> member 2 at once (r5: f20496, no confirmation), the
   ARCADE MODE view with the cursors kept. Mode 6 goes to 0x8004A638 / 0x8004A658, mode 3 to 0x8004DE54.

### 17.2 What is written to the career

The watch saw exactly one career write in the whole race (r4, f18901, pc 0x8005DC90, ra 0x800510C8): career + 0xC2 = 1. It is the
RESULTS setup 0x80050EF0 (Sim 0x80050FD0): with race block + 0x09 == 0, + 0x0A == 4, the player 1st and the course record number
+ 0x57E >= 0 it calls the EXE's **0x8005DC64(career + 0xB8, + 0x57E, + 0x57F)**: flags[record] |= the level's bit (+0x57F =
0x800272DC[level]: Easy 1, Normal 2, Difficult 4); while bit 2 is clear a Normal win counts in bits 3..5 and the fifth sets bits 1
and 2. These bytes are the unlocks of 0x8001D418 (class S / bonus cars: bit 1 or 2 of their course) and of 0x8001D120 (the reverse
courses: bit 2) - so class S opens with a Normal or Difficult win on the right course, a reverse course with a Difficult win or
five Normal wins. In 2 player Battle (+0x0A == 0) the same setup counts the winner: career + 0xB8 + 0x44 / + 0x46 += 1. No best
lap or time is stored by a road race (no write to the course records career + 0x218 + course x 0x24), nothing is saved to the
card (the title's Save does that). Port: `src/game/arcade/arcade_results.*` (`SetCourseWinFlags`, `ApplyArcadeRaceResult`);
gt2verify row **ArcadeWin** (0x8005DC64, 400 random cases, all RAM compared, 0 mismatches; `verify_arcade_results.cpp`).

The course records (+0x218, 0x24 bytes per .crsinfo course; +0x14 = the record car) are read by 0x8005E674 (course record of the
race's course) for the Rally / Time Trial ghost name (17.4) and by the COURSE SELECTION's "FASTEST LAP" table; which code writes
them (Time Trial / Rally, the race shell's mode-6 course record path of 0x80013824) is not established.

### 17.3 gt2game after the race

`gt2game <arcade.bin>` now runs the original's order after an arcade race (`tools/gt2game/arcade_mode.cpp RunArcadeRaceSession`,
`arcade_post_race.*`): the race (the race-end X ends it) -> the career flags above on the menus' career (the unlocks follow at the
next menu start, as in the original) -> the automatic replay of the race just driven (the recorded stream, Esc leaves it) ->
RESULTS -> the post-race menu: Replay (the replay, back to the menu), Try Again (the same race block again), Save Replay ... (drawn
disabled: the replay card manager is not ported), Exit (-> ARCADE MODE). The views are the Simulation port's
(`gt2view/race_result_screens.*`: the race overlay's code is the same in both builds) on assets laid out at Simulation addresses:
`LoadArcadeRaceMenuAssets` copies the arcade member 0 / EXE images to their Simulation places through the profile, translates the
pointers inside them back (image addresses by the inverse of the copy, RAM addresses by the profile's reference runs) and places
the arcade race text (member 0 0x80028C4C: `.text/data-race.txd` block language x 0x167F -> 0x801C6940, 7 blocks in the file) at
the Simulation string addresses of the reference runs. Comparison with the captures (`GT2_ARCADE_POST_TEST=1
GT2_ARCADE_POST_COMPARE="field:cap.vram.bin:side.png,..." gt2game <arcade.bin>`, the capture run's result as input, canvas with the
interpreter GPU's rules, pixels inside the 3D model's rectangle counted apart; `work\play\arcade_results\cmp_*.png`):

| Screen | Capture (gt2play `--ai-player`, the r4 script) | Native field | Differing pixels outside the model rectangle |
|---|---|---|---|
| post-race menu, Replay selected | `cap\menu_20250` | 1352 | 0 |
| post-race menu, Try Again (after down) | `cap\menu_20330` | 1438 (down at f1402) | 0 |
| RESULTS, lap list open, "Next" (t 99 / 499 after the setup) | `cap\results_19000`, `results_19400` | 129 / 529 | 0 / 0 |

The fields are ours (RESULTS set up at f24, Next pressed at f623); gt2play's scripted presses reach the game a few fields after the
script field, and the colour wave of "1st" (TextObject flag 8) shows any phase error (88 pixels one field off). Inside the model
rectangle the 3D car (not a 2D primitive) and what is drawn over it differ, as in the Simulation checks (race_screens.md 5.4).

Known gaps (race_view.cpp / panel.cpp / race_overlay_screens.cpp are the Simulation race screens' files, not changed here; the first
gap is closed by 17.7): the race runs with a RaceFlow and **no Panels** - Panels reads the race overlay's assets at Simulation addresses and cannot load on this disc
(the global text gzip 0x80022D80 of member 1), so the pause menu (0x80029E80) and the race-end display (0x8002B170) are not drawn on
the arcade disc (the pause works unseen: Esc pauses / continues; the flow's own pre-race and result panels are passed by a scripted
Enter in the field they appear). The original's RESULTS -> menu passage takes two view-manager runs (the screen closes to black in
25 fields, the menu's header opens with its spacing animation); ours switches with the one-run 16-field transition.

### 17.4 Rally and Time Trial

GAME SELECTION (0x8001D8F0): Rally -> 0x800F0298 = 1 (selection + 0x2CC), list kind 2, the class view 0x800521E4 (0x8001E284 /
0x8001E330, list 0x8004FB5C: "Rally Car" result 6, Home / Guest Garage 4 / 5): Rally Car = class list **6** (the four rally cars
and the 20 bonus cars behind them, opened by 0x8001D418), CAR SELECTION colour 0x2084B6; Time Trial -> CLASS SELECTION (the road
classes), list kind 1; both then CAR (TRANSMISSION, SETTINGS) and COURSE (rally list 0x800512B0 / time trial list 0x80050CB0). Both
build **mode 6** (0x80011038): event **"ATT"** for both (0x800267B0; `ADT` is not used by the menus - Rally's dirt comes from the
course and the rally cars' own tyre rows, not from the event's dirt flag), 100 laps, no countdown (the event's rolling start at
80 km/h), one entry (0x80010A34: grid 0, kind 3), + 0x588 bit 0 = not Rally, + 0x53C = the name of the car of the career's record
of the course (0x8005E674 + 0x14; "" without a record). At the race load the overlay adds entry 1 of kind 2 (the ghost) with the
player's car (rally r1 snapshot f7000); the HUD shows "RALLY" and sector splits. The ghost record / playback and the lap / split /
record handling are the race shell's mode-6 branches of 0x8003C70C (0x80012378 / 0x800122C4 / 0x8001286C / 0x8003FB70;
`src/game/sim/race_shell.cpp` throws there) - not ported (outside this pass' files).

Port: the build (`arcade_setup.cpp`, mode 6) and the menus (`arcade_menus.cpp`: the rally class view, class list 6 with the bonus
flags, the Time Trial path, the car view colour 0x8004FAC0[class]). gt2verify **ArcadeBuild** now draws a third of its 300 cases
as mode 6 (rally cars / class cars, the time trial and rally course lists, a random record car per course): 0 mismatches. End to end:
the original's Rally (Tahiti Dirt Route 3, t2cxr) and Time Trial (Tahiti Road, ccrcn) builds (`gt2run session ... snap=4035`,
`work\re\arcade_rally\cmp_rally`, `cmp_tt`) against our menus with the same presses (`--arcade-setup-out`): race block and settings
0 bytes differ in both. gt2game shows a "RALLY" / "TIME TRIAL" notice instead of the race (the shell's mode 6).

### 17.5 Menu music

Member 2's music object 0x800F3718: 0x800257CC (from the menus' entry 0x80014064) loads arcade file ids 216 / 215 (the arcade file
table 0x801E2950 -> VOL 0x296F `sound/arcseq.ins`, 0x296E `sound/arcade.seq`); the root view's first init (0x8001D584) starts
sequence 0 at once (0x800256EC: master = career + 0xB3 * 0x4000 / 255). The bank sits behind sys.ins at SPU 0x9630 like gtmseq.ins
(the captured voices' start addresses). Port: `MenuAudio::LoadArcade` / `StartArcadeMusic` on the existing SEQG sequencer
(`docs/formats/sound.md` section 9). Check: `gt2play <arcade.bin> --script "600:start,700:cross,900:start,1100:start,1300:cross"
--spu-log spu.txt` against `gt2game <arcade.bin> --script "70:cross" --arcade-record-audio menu.wav` (its events file): **1076 of
1076 music key-ons equal** (start address, pitch, volume L / R, ADSR 1 / 2, field; 990 fields from the first note), and the title's
accept effect (2 voices) equal. The previous pass read MenuAudio's EXE tables (pitch / pan / velocity / effect pairs) at their
Simulation addresses on this disc; they now go through the profile (identity on the Simulation disc).

### 17.6 Not available yet (what they need)

- **2 player Battle** (mode 0, event name "A2P", settings of "ATT"; build path 0x80010C84 mode 0 with two 0x80010A34 entries of
  kinds 3 / 4 from selection + 0xA0.. / + 0xA8.. / + 0xB0..): the second player's selection screens, a second pad, split-screen
  rendering (two cameras / views, the race frame's pad slot 2), the race shell's mode-0 branches (ShowGap mode 0, the "PLAYER 1 / 2
  WINS" race end, the 2P win counts career + 0xFC / + 0xFE).
- (Done in 18.4 / 18.6: Home / Guest garage cars - class rows 4 / 5, view 0x8001F8CC, LOAD GUEST GARAGE 0x800523DC, ovl3's
  rebuild 0x80012C00 / 0x8001290C. CORRECTION: the garage blocks are 0x4028 bytes with cars of 0xA4 bytes, not 0x2014 / 0x52.)
- (Done in 18.5: Bonus Items 0x800525C8 and ENDING CREDITS 0x8005261C; the credits movie itself is MDEC and not ported.)
- (Done in 17.8: the arcade title's Options / Save / Load and the first-boot auto-load; the career is saved on the card.)
- Rally / Time Trial races: the race shell's mode 6 (17.4).
- (Done in 18.2 / 18.3: CAR / COURSE SELECTION widgets and the arcade car camera 0x80016218 / 0x80016624.)

### 17.7 Pause and finish on the Arcade disc

Addresses ARCADE v1.1 (member 0 7360263f...) unless marked "Sim". Facts: `db\arcade_us11_symbols.yaml` (section "pause menu and
race-end display over the arcade race"); checks: `docs/formats/race_screens.md` section 7.

- **Code.** The pause menu (0x80029D18 input, 0x80029E2C draw) and the race-end display (0x8002B11C with 0x8002AB0C "Finish",
  0x8002A38C / 0x8002A5DC badges, 0x8002AC8C result rows) are the Sim functions 0x80029D6C / 0x80029E80 / 0x8002B170 ... modulo
  relocations: objdump of 0x80029D18..0x8002BC9C against Sim 0x80029D6C..0x8002BCF0 (2024 lines each) differs only in relocated
  calls, jumps and lui / addiu data halves; the three "other differences" of 0x8002B11C are the licence prize record offset
  (+0x158C, Sim +0x17E8) and the 2P / "YOU WIN" string offsets - not on the arcade race's path (sub-mode 4, the default path with
  the results table).
- **Data.** ovl0 tables at -0x54 (pause labels 0x8002F58C = {"Continue" 0x801C6ADB, "Exit" 0x801C6AE5}, ease / colour words
  0x8002F594.., badges 0x8002F7A4 / 0x8002F7B0: bytes equal to Sim's), EXE fonts at -0x308 (0x80092E28 / 0x80092E34), the race
  text block 0x167F bytes at 0x801C6940 (0x80028C4C: language x 5759) with "Finish" 0x801C698B, "Results" 0x801C6992, "Running"
  0x801C699D, "1Lap" 0x801C69A7, "%dLaps" 0x801C69B0, the table's names = the race slots of the race block (+0xEC + car x 0xD0,
  0x801D53A8). Every address lies in an accepted reference / aligned run of the profile with that delta: no new ranges.
- **What was wrong.** Panels built `RaceOverlayAssets` from the raw images at Simulation addresses (fonts, colours, badges, the
  race text with the Simulation block size 0x1915) and the licence / settings menu assets with `RaceMenuAssets::Load` (member 1's
  global text gzip), which throws on this disc, so the arcade race ran without Panels.
- **Port.** `RaceOverlayAssets::Load` reads every table through the image's profile (`GuestImage::Sim`, strings through the
  profile's reference runs, the block size `kArcadeRaceTextBlockSize`; identity on the Simulation disc); Panels on the arcade disc
  takes the menu assets of `LoadArcadeRaceMenuAssets`, has no GT-mode menu font (our own pre-race / result panels draw nothing: the
  arcade flow has none) and plays the arcade EXE's effects; `RunArcadeRaceSession` creates Panels (`LoadArcadeRacePanels`) and
  names the rows from the race block (`ArcadeRaceSlotNames`). Esc / Start = the pause (Continue / Exit); the finish = "Finish",
  the badges, the darkening and the table, held until X, then the replay and the post-race views of 17.3.
- **Checks.** The frame builders on the arcade disc against 15 captures (race-end timers 39..315, the pause with Continue settled
  and with Exit flashing): equal primitive sequences, 0 differing pixels (race_screens.md 7). In the game
  (`GT2_RACE_OVERLAY_LOG=<file> gt2game <arcade.bin> --fast --ai-player --script "<menus>,1300:esc,1340:down,1400:esc"`): the
  logged pause frames equal the captures' (78 / 78 lines, Continue settled and Exit flashing), and so do the race-end frames that
  depend on the timer alone and that the --fast run reaches (4 race frames per presented frame): "Finish" (t 39), the fading
  copies (t 109 / 129), the header (t 157), the OFFICIAL TIMER badge (t 189). The rows depend on our race's results (the capture
  run's opponent draw is not reproduced: its VSync counter was not recovered); screenshots show the table with the slot names.
  After the X the replay (Esc leaves it) and RESULTS follow as before.
- **Open.** Pause -> Exit abandons the race to our (here empty) pre-race panel, Esc + Enter leaves it; what the arcade loop does
  after a pause Exit in the race (0x80016CC0 with the results record +0 == 0: the post-race menu without RESULTS?) is not
  established, and race_view.cpp has no hook for it. The pause was captured over the replay (18600:start), not during the race (the
  same function).

### 17.8 Arcade title Options / Save / Load

Addresses ARCADE v1.1 (EXE 231f9dba..., member 1 20bb63ff...) unless marked "Sim". Evidence: objdump of `work\ovl\arcade_us11\ovl1.bin`
and SCUS_944.55 against the Simulation functions of menus_gtmode.md section 10; `gt2play --prims` captures of the original arcade
(`work\play\arcade_title\cap`: options pages, save / load screens, the first-boot view, with RAM images); gt2verify rows on the
options capture's RAM. Facts: `db\arcade_us11_symbols.yaml` (sections "title text facts" and "arcade title").

- **Flow.** The entry 0x800112D0 is the Simulation entry's flow: first boot (0x801EF020 == 0) -> the first-boot view 0x8004AE28
  (Sim 0x8004B900: "BASCUS-94455GAME" from card 1, "Loading Save Data..." / "Auto Loading Complete"; captured at f800 / f1000 with a
  save on card 1), then the title view 0x8004B14C and the jump table 0x80020CB8 on 0x801EF023: 0 Start Game (member 2), 1 Replay
  Theater, 2 Options (views 0x8004B4A8 -> 0x8004B550, CD track 6 via 0x80012380 -> EXE 0x80080E34), 3 Save Game (0x8004AA38 ->
  0x8004AA8C: EXE 0x8007275C + 0x80072EAC, track 7), 4 Load Game (0x8004AAE0 -> 0x8004AB34: 0x80072F20, track 7), 5 Data
  Transfer, 6 the attract demo. Every options / card function is the Simulation's shifted (member 1 -0x94 .. -0xB2C, EXE -0xF0).
- **Options.** The same five pages (GLOBAL OPTIONS, RACE OPTIONS, KEY CONFIGURATION, 1P / 2P ANALOG SETTINGS) with the same rows:
  rows 0x8004B210 (8) / 0x8004B35C (7) = Sim - 0xB2C, relocation-masked equal (aligned run), getters / setters / stepper 0x800179E0 /
  0x80017AD4 / 0x80017B98 on the career 0x801C9340 with the Simulation byte offsets. RACE OPTIONS row 0 "Race Laps" = career + 3
  (the arcade Single Player laps, 16.3), row 2 = + 6 (2P laps), Car Damage + 2 / + 5, Tire Damage + 4, Handicap Start + 7 (drawn
  "%dm": the arcade text has no minus sign), Slow Car Boost + 8; GLOBAL OPTIONS + 0xAE..+ 0xB4 and the vibration bytes + 0x36 / + 0x88.
- **Card.** The same manager, states and save file: header 0x80069F48 (the same name / title / icon data as the Simulation EXE),
  block = career 0x801C9340, 0x7C9C bytes, CRC over 0x7E9C (0x8006A124 / 0x8006A224 / 0x8006A188). One file for both discs.
- **Texts.** The arcade data-title / data-global are other builds (blocks 0xDAE / 0x76F at 0x801B9330 / 0x801EF0E0, docs/formats/
  title.md section 8). The port names its strings by Simulation address; 16 strings that no accepted reference run maps (built from a
  base register + offset, e.g. "SOUND" = "VIEW SETTINGS" + 18 in the Simulation and + 17 in the arcade code, or read from a view table)
  are `kind: data` facts, each with the Simulation and arcade instruction that loads it; `gt2tool gen-profile` regenerated
  `exe_profiles.inc` (20 facts).
- **Port.** `TitleAssets::LoadArcade` (arcade blocks, `Text()` through the profile), `TitleAssets::SimLayoutScreens()` (the arcade
  member 1 / EXE at the Simulation addresses for OptionsScreen / CardManager / the key and analog pages; string pointers become
  build-text tokens), `shell::TitleBootLoad` (`src\game\shell\arcade_title.*`: the first-boot view), `tools\gt2game\arcade_title.*`
  (the screens on the session's career; `arcade_mode.cpp` opens them from the title, gives the career to the menus at Start Game and
  takes it back at their exit, passes the career's options / pad block to the races: `RaceOptions`). The PC SETTINGS page of the
  Simulation port is shown as a sixth page on this disc too.
- **Verification.** gt2verify on `work\play\arcade_title\cap\opt_1450.txt.ram.bin` (`build\gt2verify.exe <that> "<Arcade disc>"`):
  ArcTText 143 cases (both blocks = the RAM copies; every string pointer of the rows / page titles / view titles / bar templates and
  every Simulation string constant of the screens = the string the original's RAM holds there), ArcOptGet 2000, ArcOptSet 2000,
  ArcOptStep 3000, ArcSavePack / Check / Unpack 60, ArcSaveHead 1: 0 mismatches. Frames (canvas, interpreter rules; native script
  -> shot field vs capture):

| Screen | Native script, shot | Capture (gt2play script) | Differing pixels |
|---|---|---|---|
| GLOBAL OPTIONS | `20:down,40:down,60:cross`, 122 | `600:start,1200:down,1260:down,1320:cross`, 1450 | 0 |
| GLOBAL OPTIONS, Chase View edited | + `220:cross,260:down`, 296..304 | + `1480:cross,1520:down`, 1560 | 0 |
| RACE OPTIONS | + `240:right`, 310 / 312 | + `1500:right`, 1550 | 0 |
| KEY CONFIGURATION (`--fake-pad "0:type=digital"`) | + `340:right`, 410 | + `1600:right`, 1650 | 0 (476 with an analog pad in port 1: its key table) |
| 1P / 2P ANALOG SETTINGS | + `440:right` / `540:right`, 506..514 / 606..614 | + `1700:right` / `1800:right`, 1750 / 1850 | 0 / 0 |
| title, Save Game selected | `20:down,40:down,60:down`, 110 | `...,1320:down`, 1370 | 0 |
| SAVE GAME, Select a Slot | + `80:cross`, 220 | + `1380:cross`, 1500 | 0 |
| SAVE GAME, Start Saving? | + `150:cross`, 295 | + `1560:cross`, 1700 | 0 |
| SAVE GAME, Saving Complete | + `200:left,220:cross`, 330..450 | + `1800:left,1860:cross`, 2400 | 5813 (reveal animation, as Sim) |
| first boot, Auto Loading Complete (save on card 1) | none, 60 | `600:start`, 1000 | 0 |
| LOAD GAME, Select a Slot (after the boot view) | `239:down,...,299:down,319:cross`, 459 | `600:start,1400:down,...,1580:down,1640:cross`, 1760 | 0 |
| LOAD GAME, Start Loading? | + `389:cross`, 539..559 | + `1800:cross`, 1960 | 0 |
| LOAD GAME, Loading Complete | + `589:left,609:cross`, 700 / 740 | + `2000:left,2060:cross`, 2500 | 165 ("OK" reveal, as Sim) |

  Round trip: the native Save Game of the arcade new game on a formatted empty card (`work\play\title\card1_backup.mcd` copy) = the
  card the original wrote through the same presses, all 131072 bytes. A native save with RACE OPTIONS laps 5 / 2P laps 4
  (`work\memcards\arcade_title\custom_native.mcd`): the original's first-boot view loads it (RAM career 0x801C9340 = the saved block,
  0 bytes differ; its Save / Load captures ran on copies, the cards unchanged afterwards); gt2game loads it by the boot view and by
  Load Game from slot 2, and the next race the menus build has 5 laps (`--arcade-setup-out`, race block + 0x0F).
- **Not done.** The progress bars run at our card rate (8 sectors per field; the original's first-boot read takes ~200 fields),
  so the boot / saving frames with a partly lit bar are not compared. Replay Theater / Data Transfer: ported 2026-09-19
  (docs/formats/title.md section 12: the same member 1 code, one arcade difference in Copy Replay's message line).

### 17.9 Rally / Time Trial races: the race shell's game mode 6 and the ghost (2026-09-19)

Addresses of this subsection are US SIMULATION v1.2 (the race overlay's mode 6 code is the same in the Arcade build, "shifted";
its data reached through the profile, e.g. the ghost block 0x800A8D70 -> Arcade 0x800A8A68, the lap ring 0x801D5F84 -> 0x801D59E4).
Evidence: objdump and Ghidra pseudo-C of the race overlay (`work\re\mode6\decomp`, project `gt2_mode6` on `work\re\race_demo\ram.bin`),
frame captures of the original with the dev capture aid (`GT2_CAPTURE_AI_PLAYER=1 gt2verify --race-capture ...`: the original's AI
drives the player's car; new side file `<capture>.ghost` = the ghost block, the reference lap buffer and the overlay's ghost flags
per frame, `gt2formats/race_capture.h RaceCaptureGhost`), RAM snapshots of both modes (`gt2run session ... aiplayer snap=`:
`work\re\arcade_tt\s1\ram_011000.bin` Time Trial Tahiti Road ccrcn lap 2, `work\re\arcade_rally\s1\ram_012000.bin` Rally
`tahiti_d_new` (Tahiti Dirt Route 3) t2cxr). Port: `src/game/sim/race_shell.*` (GhostSession and the Ghost* routines, LapCheck's
mode 6 branches), `race_sim.*` (the frame driver's mode 6 parts), facts in `db\sim_us12_symbols.yaml` (section "game mode 6").

**Rules.** Mode 6 races two cars: 0x8001503C copies the player's entry (and parameter record) into entry 1 unless that entry holds a
lap of a previous race (its byte + 0x8C), kind 2 = the ghost: pad slot 1, control class 0, contact type 2 (0x80012CD4; no car-to-car
contact), both cars on grid slot 2 of a circuit (rolling start at 80 km/h, no countdown, the clock stands until car 0's lap 1). The
ghost replays the session's best lap:
- Player 1's input stream (0x80013EF0 / 0x80013C90) goes into a ring of four lap buffers at 0x801D5F84 (s16 laps kept, s16 current,
  4 x 0x10FC over the normal 0x4400 stream): head 0xE0 (+0 s16 lap-line fraction of the frame in 1/4096, +2 ms of the frame after the
  line, +3 (clock + frame) % 3, +4 lap time, +8 the car state of 0x800350FC: body + 0x600..0x668 and the first 0x1C bytes of each
  wheel), stream object at +0xE0 (capacity 0x1000). The reference (best) lap is the buffer 0x801DA4A0; the ghost's stream is its +0xE0.
- At a timed lap line of player 1 (0x8003C70C -> 0x8001286C): the lap's stream ends; a lap without the invalid flag that beats the
  reference (or the first lap) becomes it (head, stream, the player snapshot 0x800A8D70 -> ghost snapshot 0x800A90B2 = body + 0x45C..
  0x798 one frame after the lap start, the player's car constants body[0..0x45C) -> car 1 + pointer relink 0x80031440, entry / params /
  car sound / render block of car 1); then the ghost restarts on the reference (0x8003F724: 0x8003519C car state, the snapshot, its
  stream from the start, its clock offset (ref + 3) - clock % 3 so that its line times match the reference lap's), deferred by one
  frame when the reference crossed the line later in its frame; the ring advances and the next lap records.
- The ghost's time is its own: its lap / split times are (clock at the line + offset) / 3 - its last line time.
- The ghost is held (body + 0x718 |= 0x8003FAEC) while it has no lap (2), while it waits at the lap start for player 1 (1: a Try Again
  race placed it there, 0x8003EF40 in the car start) and after its stream ran out (2: the player is slower than the reference).
- Its displayed pose (and its course distance for the race order) is the blend of its last two physics poses by the difference of the
  two laps' line fractions (0x8003F2F0 before the render transform and 0x80042568, 0x8003F548 restores the physics pose after it).
- The course record: the race object + 0x24 (0x800A9524) points at the career's record of the course (0x8005E764: career + 0x218 +
  course index of race block + 0x40 * 0x24). The HUD compares every lap / split with it (0x80013824 / 0x8001374C, hudCompare) and a
  faster valid lap is copied into it (0x80013824 mode 6, newRecord 0x801D5DE9) - this is the writer of the course records that 17.2
  left open; the mode 6 post-race path (0x8004AA5C) adds + 0x14 = entry 0's car id and + 0x18 = the career's name string (+ 0x7C8F).
- The race end (0x800153B8 -> 0x80012570 / 0x800125BC) puts the ring in recording order and, after a new reference, hands it to the
  next race of the session (entry 1 = entry 0 with + 0x8C = 1); 0x80012410 clears the reference only once per overlay load.
- Drawing (0x800140A4, not ported): the ghost (pad slot 1) is drawn only while 0x800AF232 != 0 (toggled by the pad bit 0x20000 in
  race_frame 0x80015B64) and by the option career + 0xB5 (1 always; 2 / 3 by distance; 0 never in mode 6), semi-transparent (mode 3).
  Both bytes are 1 in the captured runs.

**Verification.** `gt2game "<Arcade disc>" --race --track <course> --ai-player --frames-compare <capture>` (a mode 6 capture goes to
`FramesCompareGhost`, `tools/gt2game/arcade_race.*`: the two car records like FramesCompare, plus every frame the ring (counters, heads,
stream headers and coded bytes), the reference lap, the snapshots / playback and the flags):

| Capture (GT2_CAPTURE_AI_PLAYER, to field 28000) | Frames | Laps (new references) | Differ |
|---|---|---|---|
| Time Trial, Tahiti Road, ccrcn (`work\re\arcade_tt\race.bin`) | 11807 (fields 4388..27999) | 5 (laps 1, 2, 4) | 0 |
| the same with pad presses during the race (`race_pad.bin`: the recorded stream is not idle, the ghost drives on it) | 11807 | 5 | 0 |
| Rally, tahiti_d_new, t2cxr (`work\re\arcade_rally\race.bin`) | 11799 (fields 4404..27999) | 4 | 0 |

gt2verify on the two mode 6 dumps (`build_arcade4\gt2verify.exe work\re\arcade_tt\s1\ram_011000.bin "<Arcade disc>" tahiti_t`, `...
arcade_rally\s1\ram_012000.bin ... tahiti_d_new`): new rows CarSave / CarSave1 (0x800350FC), CarRestore (0x8003519C), GhostInit
(0x80012410), GhostHold (0x8003FAEC), GhostLap (0x8001286C, 1200 cases; the entry / params / sound / render part of a new reference
is the harness's hook, ShellHooks::ghostReference), GhostStart (0x8003F724), GhostCheck (0x8003FB70), GhostBlend (0x8003F2F0),
GhostPose (0x8003F548), LapCheck6 (0x8003C70C in mode 6 with both cars); StartCar now covers 0x8003EF40 and GridArgs / GridDisc the
ghost and the player entries of mode 6 (contact type byte). Harness fix found on the rally dump: the WallResp rows now take the
dump's dirt flag (the first dirt-course dump; the wall response differs on dirt).

**gt2game.** Rally and Time Trial of the arcade menus now race (`arcade_mode.cpp RunArcadeGhostSession`): the ghost session of
race_shell.h, the course record of the menus' career in (HUD Record) and out (`game/arcade/arcade_results.h SetCourseRecord`: the
lap, the car id, the career's name string; the title's Save writes it to the card). A scripted Time Trial through the menus with
`--ai-player` gives the capture's lap times (1:20.286 race time at lap 1, 1:18.416 lap 2, ...). The ghost car is not drawn while it
has no lap to drive (held with 2); otherwise it is drawn like the other cars (opaque). Not done: the mode 6 post-race views
(0x8004A638 / 0x8004A658, the NEW RECORD name entry: Pause -> Exit returns to the menus), Try Again with the kept reference
(GhostRaceEnd is ported but not wired: the session ends at Exit), the ghost's translucency and display toggle, replays of mode 6
races (0x8003F990, the demo paths of 0x80013EF0 / 0x80013244), 2P.

(2026-09-19, later: the ghost's look and toggle, Try Again with the kept reference, the post-race views and the replay are done - 17.10.)

### 17.10 Mode 6: the ghost's look, the arcade loop after the race, Try Again, the post-race views (2026-09-19)

Addresses: the ghost rule and the race frame are US SIMULATION v1.2 and the same in the Arcade build (0x800140A4, the load of
0x800AF232 at 0x80014384 in both); the arcade loop is ARCADE (member 0 7360263f...); the views are named by their Simulation
addresses, the Arcade copies sit 0xE0 lower (code: 0x8004A638 ..) with their data 0x90 lower (views 0x8005B00C ..). Evidence: our
objdump / Ghidra pseudo-C of the race overlay (`work\re\mode6_post\decomp`, project `gt2_mode6post` on `work\re\race_demo\ram.bin`;
arcade `work\ovl\arcade_us11\ovl0.bin`), gt2run sessions of the original with the dev capture aid (`work\re\mode6_post\s1..s5`: the
Time Trial of 17.9, pause -> Exit, the replay's pause -> Exit, the name entry, SESSION RESULTS, the menu, Try Again; calls and write
watches), gt2play `--prims` captures (`work\play\mode6\tt_*`: the race around laps 2 / 3; `work\play\mode6\cap\c1_*`, `c2_*`: the
views). Facts: `db\sim_us12_symbols.yaml` / `db\arcade_us11_symbols.yaml` (sections "game mode 6: the ghost's look ..." /
"mode 6 arcade loop ...").

**The ghost's look (0x800140A4).** The car pass 0x8001545C calls 0x800140A4(car, view, mirror) per car. It measures the car
(car + 0x830.. - camera view + 0xB8.., max + mid / 2 + min / 4 of the absolute components, 16.16 m -> car + 0x804), links it into
the draw list 0x800ADA08 (main view) and hands a record to the EXE's car renderer 0x80067444: +0 LOD (0 by distance, n = LOD n - 1),
+1 the palette group (added << 24 to the CLUT word: CLUT rows + 4 * group; 0..2 = the ground class most wheels are on, car + 0x4A1
+ wheel * 0x68, also the wheel texture set), +3 = 1. Not drawn: the mirror's pass beyond 0x63FFFF, car + 0x0F != 0, the followed
car in the driver view. The ghost (pad slot 1) in game mode 6 (0x80012378) - or in another mode once the start hold is over outside
the attract race - is not drawn while the display toggle 0x800AF232 is 0 (the race frame 0x80015B64 flips it on the generic pad bit
0x20000 = Select; 1 at the race start) and, by the career's ghost option + 0xB5: 0 "No Ghost" not drawn in mode 6; 1 "Type1" always
the ghost look; 2 "Type2" / 3 "Type3" the ghost look within 0x2FFFF / 0x4FFFF (3 / 5 m) of the camera and the normal look farther;
other values normal. The ghost look is LOD 3 - 1 = 2 with group 3: 0x80067444 draws no wheels for group 3, and the CLUT rows 492..495
are empty in VRAM (all-zero texels are transparent), so only the reflection pass (the additive environment map of the body's bit-15
polygons, 0x26 / 0x2E, tpage 9 | 0x20, colour ((0x1000 - min(dirt << 12 / 600000, 0x1000)) * 3) >> 7) and the shadow (0x80068004,
drawn at LOD 0 / 1 or with group 3) show: a glass car. The mirror's pass draws every car with LOD 2 and its normal look (the ghost
too). Captures of the original: the ghost's polygons are the raw-textured 0x25 / 0x2D with CLUT (656, 492), 3 + 3 per frame at
173 m (`tt_13930`), about 340 at 0.9 m (`tt_13990`: the close-range subdivision of 0x80063EF4); in the rear-view mirror the same car
with CLUT row 480 and wheels. car + 0x0F: 0x80012CD4 sets it for the ghost entry, 0x800133F0 clears it every frame while car + 0x0E
(the ghost has a lap) is set, 0x8001286C sets + 0x0E / clears + 0x0F (s5 watch: pc 0x8001342C every frame of the Try Again race).
Port: `race_shell.h CarDrawRule / ApproxDistance / GhostDisplayToggle`; gt2verify row **CarDraw** (0x800140A4 run up to its call of
0x80067444, the record's +0 / +1 and car + 0x804 against the port, or "not drawn"; 3000 random cases per dump: pad slot, + 0x0F,
positions, wheel classes, the view's followed car / driver view, mode, hold, attract flag, toggle, option, the mirror's pass): 0
mismatches on all four dumps. gt2game (`race_view.cpp`): the rule per frame for car 1 (main view and mirror), the ghost look = its
own car slot (`SceneAssets::UseCar(id, "ghost:" + id)`) with the LOD 2 reflection pass and the shadow only; Select (pad) / G
(keyboard) flips the toggle; the option comes from the menus' career (`RaceViewConfig::ghostOption`). The toggle also feeds the
Simulation build's tail of UpdateWheelEffects (ground.cpp, not in the Arcade build); RaceSim keeps the constant (mode 6 exists on the
Arcade disc only).

**The arcade loop after a mode 6 race.** 0x80016F14 runs the state machine 0x80015ED4 (object on its stack: +0xC the vtable
0x8002EFAC, +8 the default code 3): each state's handler returns a code; 0 = the stored default, 2 / 3 = go to the state's "back" /
"next" state (s1 / s0 of the dispatcher, the stored default = 3), 1 = leave, 4..13 = that state (jump table 0x8002EECC). States:
4 0x80016BCC (mode 3 load) -> 5 -> 8 0x80016C58 (the race-run flag + 0x5D0 = 1; when + 0x5D1 (a ghost loaded from a card): race
block entry 1 + 0x8C = 1, + 0x8D = 0, + 0x8E = 2) -> 9 0x8001622C (the race, vtable + 0x44(0)) -> 10 -> 11 -> 12 0x80016274 (the
replay, vtable + 0x44(1)) -> 13 -> 6 0x80016CBC (the post-race function; the previous notes' 0x80016CC0 is its second
instruction) -> 5 (Try Again: the race again, no load) / 11 (Replay) / 7 (Exit -> leave -> member 2) by M + 0x7C (M = the view
manager at loop + 0x38C). A mode 6 race never finishes: pause -> Exit ends it (0x800153B8: the ring put in order, the reference handed
to entry 1), then the replay of the ring's laps plays at once (s1: f9747 0x80016274 after the Exit at f9740), its pause -> Exit leads
to 0x80016CBC: 0x8004A638(race-run flag) (Sim 0x8004A718: 0x8005B088 = flag, 0x801D55AA = 0), the manager on the wait view 0x8005B00C
(Sim 0x8005B09C), 0x8004A658 (Sim 0x8004A738: 0x801D55AA != 0 -> loop + 0x5D1). 0x801D55AA is set only by the ghost card manager's
Load (Sim 0x80050304: + 0x8002F4B1 = 1).

**The views** (Simulation addresses; `src/gt2view/race_session_screens.*`, the framework of race_result_screens.h):
- wait 0x8005B09C (0x8004A754 / 0x8004A7B4): 20 fields, then CD-DA track 8 (0x800481C8 -> EXE 0x80080F24(8, 1)) and, with the
  race-run flag, W+0 = 1 and ENTER YOUR NAME when 0x801D5DE9 == 1 else SESSION RESULTS; without it (after a replay) the menu.
- ENTER YOUR NAME 0x8005B0E4 (0x8004A920 / 0x8004A990 / 0x8004AAD8): the EXE keyboard (race_record_screens.h NameEntry) with the
  descriptor 0x8005AF68 on the career's name buffer + 0x7C8F (11 characters, 256 pixels), opened after 24 fields with the caret at
  the end; CANCEL = sound 0; OK = sound 3, strcpy(course record + 0x18, the buffer), record + 0x14 = race block + 0x5C (car 0's id),
  strcpy(race block + 0x53C, + 0xEC), the keyboard closes, SESSION RESULTS follows.
- SESSION RESULTS 0x8005B108 (0x8004AE08 / 0x8004B19C / 0x8004B530): the course title; the kept laps (the list 0x8005AE84 with the
  row callback 0x8004AB04, rows 0x800495A8 / 0x800495E4: lap label and four sector times 0x8005DD94 and the total every 60 pixels);
  the colours of 0x8004AD40 (a sector at or below the session's best of that sector 0x8004AC20 and the best lap's total in
  0x02013060, else 0x025A5A5A); the session record (result record + 0xD0) and the course record (0x8005E764: total 0x02011660, "Max
  Speed", the record car = race block + 0x53C, the name = record + 0x18); bands 0x8005AECC, labels 0x8005AEE8 / 0x8005AF08 / 0x8005AF28
  / 0x8005AF48 (the CAR / NAME bands as wide as the wider word + 4, W+0x394 = + 0x20), the rule 0x8005AEB8 (0x8006BCB8 / 0x8006BD08);
  the draw's text context keeps the last row colour of 0x800495E4 for "Max Speed". X: on to the menu (W+0 != 0) or back to it
  (W+0 = 0, entered from the menu's "Records ...": manager code 2); triangle is refused (sound 0) with W+0 != 0.
- TIME TRIAL 0x8005B12C (0x8004C034 / 0x8004C1EC / 0x8004C5C8), also after Rally: rows 0x8005B01C (12 bytes {colour, label, s8
  enabled, u8 action}): Replay 0 / Try Again 1 / Settings ... 0xFB / Records ... 0xFC / Save Ghost ... 0xFA / Load Ghost ... 0xFD /
  Save Replay ... 0xFE / Ghost Options ... 0xF9 / Exit 2; the setup enables Settings for a garage car (0x801D5DDE / 0x801D5DE0),
  Replay and Save Replay while 0x801C90B4 == 0, Save Ghost = 0x8002F4B1 && !0x801D55AA; the list 0x8005AF7C (callback 0x8004BA40:
  text template 0x8005AFE4, band 0x8005B000 as wide as the widest label + 16), car 0 turning at (0x8A, 0xF0, 200, 200)
  (0x80048754, camera 0x80049780(200, 200), 0x80049874), the course picture as a 188 x 200 sprite (0x94, 0x6C) of page 0x37, CLUT
  0x401C, uv (0, 1), grey = the list's fade | 0x02000000 into ot[2]; Ghost Options opens the list 0x8005AFB0 (callback 0x8004BD1C:
  labels 0x8005B08C, boxes, the selection's fill pulsing with 0x801C90BC) on the career's + 0xB5 and writes the chosen row there;
  rows 0 / 1 / 2 store the action in M + 0x7C and push the leave view; 0xFC pushes SESSION RESULTS; 0xFA / 0xFD / 0xFE / 0xFB push
  the card managers 0x8005B540 / 0x8005B564 / 0x8005B51C and the settings view 0x8005D1C0.
- leave 0x8005B0C0 (0x8004A8B8 / 0x8004A8E8): 21 fields, 0x800481E8 (the CD music stops), manager code 4.
- The manager 0x800474F4 keeps a stack (M + 0x1D0, index M + 0x210): code 1 = the pushed view enters (init 0, forward
  transition), 2 = back to the view below (0x800483D8, init 1, transition 1), 3 / 4 = leave. 0x800477C4 offsets: forward = the new
  view from (0, c * 200 / 16) and the old one to (0, (c - 16) * 5); back = the new one from (0, -c * 200 / 16) and the old one to
  (0, (16 - c) * 5) (c = 16 .. 1).
- The course picture: `arcade/course_mapinfo` (u32 count 78, u32 0, 16-byte entries {packed size, sector, name offset, flags 0x10
  reverse / 0x08 2P}, the course file names) and `arcade/course_map` (per entry at sector * 2048 "@(#)GT-ZIP" + u32 unpacked size +
  the LZ stream of the EXE's 0x80083C1C: LSB-first flag bytes, literal / match {length - 3, distance - 1 in 7 or 15 bits}) = a 4-bit
  TIM 188 x 200 + CLUT; member 2's COURSE SELECTION inflates it (0x80017AA8, gt2run watch of the TIM: writers 0x80083C4C / 0x80083CF0)
  and uploads it to page (384, 256); at the race it is at page (448, 256) (CLUT row 256, image rows 257..456; all 200 rows and the
  CLUT of the capture's VRAM = the inflated picture). Port: `src/gt2formats/course_map.*` (also usable by the arcade menus'
  course selection).
- Text: the keyboard's OK / CANCEL are data-global strings (Sim 0x801EFBE4 / 0x801EFBE7); `LoadArcadeRaceMenuAssets` now places the
  arcade title's data-global block at the Simulation addresses of the profile's reference runs (like the race text).

**Try Again with the kept reference.** The Try Again race of the original (s4 / s5) runs without a load: state 8 then the race;
entry 1 keeps + 0x8C = 1 and kind 2 (0x800125BC at the race end), 0x80012CD4 keeps the ghost's stream, 0x8002F4B4 keeps the
reference (0x80012410 clears only the playback), + 0x0F is cleared by 0x800133F0 in the first frame: the ghost waits at the
reference's lap start and drives when the player crosses the line (RAM f11700: car 1 + 0x0E 1, + 0x0F 0). gt2game
(`arcade_mode.cpp RunArcadeGhostSession`): one GhostSession for the visit, the pause's Exit ends the race (`RaceFlow::pauseExitEnds`
-> `RaceSim::EndGhostRace`), the course record's lap is written during the race (the pointer) and its car / name at the name entry
(`arcade_results.h SetCourseRecordLap / SetCourseRecordOwner / SetCareerName`), then the views (`arcade_post_race.h
RunArcadeSessionViews`); Try Again loops with the same session, Exit returns to the menus; the ghost option goes back to the career.
End to end (`GT2_ARCADE_RACE_SETUP=work\re\arcade_tt\race.bin.setup gt2game <arcade.bin> --ai-player --fast --script ...`): lap 1
1:20.276 (the capture's), pause Exit, ENTER YOUR NAME, SESSION RESULTS, TIME TRIAL, Try Again -> race 2 "ghost with the kept lap"
(the glass car ahead from the start), pause Exit, SESSION RESULTS (no new record), Exit. With `--ai-player` the recorded stream is
the idle pad (the AI drives the car, as in the capture): the ghost coasts from the lap line, as in the original's capture run.

**Checks (frames).** `GT2_ARCADE_SESSION_TEST=<capture ram> [GT2_ARCADE_SESSION_COMPARE="field:cap.vram.bin:side.png,..."]
[GT2_ARCADE_SESSION_FRAMES=N] gt2game <arcade.bin> --arcade-square --no-sound --script "<presses>"`: the views on the inputs read from
the capture's RAM (result record, new-record flag, course record *(0x800A9524), names, option), the interpreter GPU's rules; our field
= capture field - 10289 (the views started at 10296; the capture's VRAM dump is the frame 6 fields after the named one); pixels inside
the 3D car's rectangle (0x8A, 0xF0, 200, 200) counted apart (the car is drawn by the renderer, not a primitive):

| Screen | Capture | Differing pixels |
|---|---|---|
| ENTER YOUR NAME, keyboard open, empty name | `c1_10380` | 0 |
| ENTER YOUR NAME, "A" typed, cursor on OK (accent page) | `c1_10720` | 0 |
| SESSION RESULTS (lap, session record, course record, CAR / NAME) | `c1_11000`, `c1_11250` | 0 / 0 |
| TIME TRIAL, Replay / Try Again selected (course picture, rows) | `c1_11400`, `c1_11550` | 0 / 0 (outside the car) |
| TIME TRIAL, Ghost Options selected; option list open (Type1); Type2 selected; list closed; after five up presses | `c2_12150` .. `c2_13050` | 0 in all five (outside the car) |

gt2verify (build `build_mode6b`, 2026-09-19): race_demo 252 ok, license_race 236 ok, arcade_tt 221 ok, arcade_rally 207 ok, each
with the new CarDraw row (3000 cases, 0 mismatches); the only FAIL of the run, RepUnpack 0x80069AC4 on the two Arcade dumps, is the
replay-file row added the same day (verify_title_replay.cpp, in progress), not a row of this work. `gt2game <Sim disc>
--selftest --cars 6`: the reference line (step 300: 137.8 km/h, rpm 6126, gear 3; 0 failures). The mode 6 frames-compare runs of 17.9:
11807 / 11807 / 11799 frames, 0 differ (the race simulation is unchanged).

**Pause -> Exit of the Road Race (17.7 "Open").** The same state machine: the race state (9) ends, states 10 / 11 lead to the replay
(12), its pause -> Exit to 0x80016CBC; mode 4 with the results record's place 0 skips RESULTS and runs the post-race menu at once
(gt2run `work\re\mode6_post\road1`: pause Exit f8140, 0x80016274 f8148, the replay's Exit f9140, 0x80016CBC f9148, 0x800471F4(M,
0x8005AD7C) f9217; the menu shows "Retire" and "--:--:---"). gt2game: `RunArcadeRaceSession` sets `RaceFlow::pauseExitEnds`, plays
the replay of the recorded stream after a pause Exit as after the finish, then the menu with place 0.

**The replay of a mode 6 race (states 10 .. 12, 0x80016274).** The race overlay runs the race again with 0x800A951C set and
the race block's one entry (0x8001503C): 1 car, demo flag 1, player 1 on pad slot 2 plays the ring's laps from lap buffer 0.
0x80013EF0 (0x800A951C set) takes the lap step car + 0x21 (cleared), clamps car + 0x20 + step to the ring, reads a frame of the
lap's stream (0x80013C90 playback); at the stream's end it goes to the next lap: 0x8002F4B8 = 1 and 0x800132D0(car, lap) re-inits
that lap's stream for playback, or 0x800A8D68 = 1 (the replay is over) past the last lap. 0x80015B64 runs 0x8003F990 on car 0
while 0x8002F4B8 is set: the playback block's pending / lap / blend / index zeroed, the lap's start state restored (0x8003519C with
contact type, car index and control class kept), both poses saved, the ghost snapshot's byte 0 = 2, 0x800124B0, the clock 0 on
lap 0 else 180000 with the clock offset head[3] and car + 0x14 = (180000 + offset - head[2]) / 3, snapshot byte 1 = 2.
0x800124B0 re-inits *(car 1 + 0x1C): in a replay car 1 is not set up again, and at a race end with a new best 0x800125BC copied the
whole car record 0 over car 1 (0x800A9688..0x800AA1C8 -> 0x800AA1C8, with 0x800A9508 = 0x800A9504, race block entry 0 -> entry 1,
0x801D5A16 = 2, parameters 0x801DE8BA -> 0x801DEA7A), so car 1 + 0x1C is player 1's last stream (a ring lap buffer), not the
reference's 0x801DA580 (0x80012CD4 sets that for the ghost entry of the next live race). Watch on the capture: the replay's
0x800124B0 (pc 0x80012744, ra 0x80015440, f9747) writes the stream header of lap buffer 2 (0x801D7CC0). Port: race_shell.h
GhostReplayLapStart (0x8003F990), GhostReplayInput (0x80013EF0 demo + 0x800132D0), GhostSession::ghostStreamLap (car 1 + 0x1C;
set in GhostRaceEnd with a new best, reset by GhostSetupGhostCar), RaceSim::ReplayLapStartIfPending; gt2game: arcade_race.h
ReplayRaceData, RaceViewConfig::ghostReplay (the view leaves at 0x800A8D68 or on Esc / Start), RunArcadeGhostSession plays it
after the pause Exit and for the menu's Replay row (then the views again without the race-run flag, as state 11).
The race start 0x8001584C clears 0x800A8D68 (0x80015AB0; StartRace clears GhostSession::replayEnded), so the menu's Replay plays
the ring again. End to end (GT2_ARCADE_RACE_SETUP=work\re\mode6_replay\race.bin.setup, --ai-player, work\re\mode6_replay\e2e
log4.txt): pause Exit -> the replay to its end (0x800A8D68) -> ENTER YOUR NAME -> SESSION RESULTS -> TIME TRIAL -> Replay (plays;
Esc leaves) -> TIME TRIAL at once -> Try Again (race 2 with the kept lap and the new course record).

Verification: gt2verify --race-capture of the Time Trial with pause Exit and the replay (`work\re\mode6_replay\race.bin`, 5216
frames, race 2 = the replay from frame 2608): `gt2game <Arcade disc> --race --track tahiti_t --ai-player --frames-compare
work\re\mode6_replay\race.bin` -> 5216 compared (2608 of the replay), 0 differ (car record, lap ring, reference lap, snapshots /
playback block, flags, and the replay's clock). Before the car record copy was modelled, every replay frame differed in 8 bytes
of lap buffer 2's stream header (the port re-initialised the reference's stream). With the replay in place: gt2verify race_demo
253 ok, license_race 237 ok (0 FAIL), arcade_tt 221 ok, arcade_rally 207 ok (all Ghost* rows and CarDraw 0 mismatches; their
only FAIL / guest exception is the RepUnpack / RepPack row of verify_title_replay.cpp's work in progress); selftest
reference line and 0 failures; the three 17.9 frames-compare runs 11807 / 11807 / 11799 frames, 0 differ.

**Open.** The ghost / replay card managers (Save Ghost 0x8005B540, Load Ghost 0x8005B564 -> 0x801D55AA, Save Replay 0x8005B51C) and
the settings view 0x8005D1C0: rows drawn disabled. The replay's pause menu (Start in the replay): gt2game leaves the replay at once
(as its Exit). The replay's lap stepping controls (car + 0x21) are not wired to keys. The views' CD-DA track 8 is not played.
Frames of the back transition (Records -> the menu) and of the leave view are not compared (no capture); the replay's rendered
frames are not compared with the capture (the simulation is).

### 17.11 The TIME TRIAL menu's card rows, the views' music, the replay pause and lap steps (2026-09-19)

Addresses: Simulation v1.2 (the race overlay's views and the EXE card manager; the Arcade copies are the Simulation's shifted:
member 0 code -0xE0 / data -0x90, EXE -0xF0). Evidence: our disassembly (views 0x8005B51C / 0x8005B540 / 0x8005B564, their code
0x8005019C..0x800503B4, the manager's setup 0x80050494 with the object 0x8005B500; the card manager's states: replay.md 9.5),
gt2run sessions of the original arcade with the dev capture aid (`work/re/cards/sg1`: the Time Trial route of 17.10 + Save Ghost;
`lg1`: + Load Ghost of the ghost gt2game saved; RAM snaps), gt2play captures (`work/play/cards/sgcap`). Port:
`src/gt2view/race_card_screens.*` (CardView over shell::CardManager, RaceCardAssets), `tools/gt2game/arcade_post_race.*`
(ArcadeCardContext), `tools/gt2game/arcade_mode.cpp` (the payloads, the loaded ghost for Try Again), `src/game/audio/menu_music.*`.

- **Save Ghost ...** (0xFA -> view 0x8005B540, header 0xC0): 0x80072EB4, mode 0 with the ghost file (race block entry 1, its
  parameter record, the reference lap): "Select a Slot", the replay file's list with "- New File -" (or "Select Number of Blocks"
  without a file), the name entry, "Saving..." and "Saving Complete"; the manager's exit -> back to TIME TRIAL (sound 4).
  gt2game builds the payload from the session: entry 1 = entry 0 with + 0x8C = 1, + 0x8E = 2 (0x800125BC at the race end), the
  player's parameter record (0x801DE8BA -> 0x801DEA7A), GhostSession::reference.
- **Load Ghost ...** (0xFD -> 0x8005B564, 0x6F0000): 0x80072F54, mode 3: the ghosts of the race's course (+ 0x42 == 1, + 0x44 ==
  0x801D589C); "Loading Complete". Update 0x80050304: 0x80072F8C -> 0x801D55AA = 1, 0x8002F4B1 = 1; the views' 0x8004A658 -> loop
  + 0x5D1 -> state 8 (0x80016C58: entry 1 + 0x8C = 1, + 0x8D = 0, + 0x8E = 2): the Try Again race drives the loaded ghost (its
  entry's car). gt2game: raceBlock entry 1 = the loaded entry with those bytes, GhostSession::reference = the loaded head and
  stream, savedBest = 1, entryHasLap; a later new best gives entry 1 back to the player's car (0x800125BC).
- **Save Replay ...** (0xFE -> 0x8005B51C, 0x6F): 0x80072E7C, mode 0 with 0x80069948's mode 6 record (the race block as RAM holds
  it, the results record, the ring's laps); the theater plays such records (replay.md 9.6).
- The card views draw 0x80072B78 into the view's OT slots 4 / 5 / 6 under the race view manager's header; fonts 0x801C9110 /
  9120 / 9150 (page 6). The strings are data-global.txd's: six more `kind: data` facts in db/arcade_us11_symbols.yaml (the kind
  texts "Ghost" / "Race", "need %d sector", "%d/%d sector free", "No Replay Files Found", "Failed to Create Replay File"), the
  profile regenerated (`gt2tool gen-profile`, 26 facts).
- **Settings ...** (0xFB -> view 0x8005D1C0 "CHANGE PARTS", enabled for a garage car only): not ported here (the row stays
  disabled; the Simulation port of CHANGE PARTS / PARTS SETTING, race_screens.md, would need the garage car's configuration and its
  write-back to the career).
  Status 2026-09-19 (still not ported; what is known): the original's route to it with a HOME GARAGE car in the Time
  Trial is captured - `gt2play <Arcade disc> --original --ai-player --card <copy of work/re/arcade_garage/card_s1.mcd> --script
  <work/play/change_parts/cap1/script.txt>` (ARCADE MODE -> Single Player -> Time Trial -> Home Garage -> the Protege GT-X ->
  Tahiti Road, one AI lap, pause Exit, the replay's Exit, ENTER YOUR NAME "A", SESSION RESULTS, TIME TRIAL row 2 "Settings ..." at
  f11900; CHANGE PARTS on screen from f11950; `gt2run session` of the same route with snaps: work/re/change_parts/r3, ram_012300);
  captures c_11990 .. c_12500 (state 0, state 1 "Suspension Kit", state 2 the stage list Normal / Sports / Semi-racing / Full
  Customization, the description line). Findings: the arcade member 0 holds the Simulation page unchanged (code -0xE0, data -0x90:
  the three group tables at 0x8005C2D4 / 2F4 / 314 list the same eight groups and parts as the Simulation's 0x8005C364 / 384 / 3A4);
  a stage is selectable only when the garage car owns it (row callback 0x80052D84 command 8: 0x8005E874 on the garage slot of
  0x801D5DDE / 0x801D5DE0, Arcade career 0x801C9340 + 0x3C74 home / RAM 0x801D0FDC guest), and when 0x801D5DE2 > 0 ("Power
  Restricted to %dhp") a power part may not raise the preview's power above it (0x80057A28); card_s1's car owns no upgrade, so
  every other stage is disabled and nothing can be changed on it. Leaving (triangle / square, code -4) runs 0x80056FF0 (Arcade
  0x80056F10): the sheet 0x8016E894 into the race record, race block entry 0 and the garage car (0x8005F958 figures) - the career
  change (`career::CommitSettings` covers it). What a port needs: CHANGE PARTS states 1 / 2 (0x800536A4 update, 0x80053CA8 draw,
  0x800529C0 part rows, 0x80052D84 popup rows; only state 0 is built today, race_menus.cpp BuildPartsPageFrame), the per-stage
  preview records and power curves (0x800576FC: 0x8005F044 + 0x80077214 + 0x80075930 + 0x8007489C, one stage per field) and the
  EXE's power / torque graph widget (0x80073CE4 .. 0x800747D0, ~880 instructions, not ported anywhere), the TIME TRIAL menu's
  hook (arcade_post_race.cpp) with the garage car's sheet (arcade_setup TuneGarageCar / LoadCarSheet), and a capture with a garage
  car that owns parts (e.g. a card holding work/re/spec_msettings/card_tuned.mcd's tuned car) to verify a change and the career bytes.
  Ported 2026-09-19: states 0 / 1 / 2, the preview queue and the executable's graph widget, the row in the TIME TRIAL
  menu (enabled by the garage rule 0x8004C0B0; Replay / Save Replay disabled after it, 0x801C90B4) - docs/formats/race_screens.md
  section 8: 20 captures 0 differing pixels, the career after the change = the original's.
  PARTS SETTING ported 2026-09-19: L1 on CHANGE PARTS replaces the view with PARTS SETTING (Arcade view 0x8005D154 =
  Sim 0x8005D1E4, replace 0x80048294 = Sim 0x80048374, commit 0x80056F60 = Sim 0x80056FF0, the manager's sideways slides 3 / 2),
  its R1 goes back, triangle / square commit and return to TIME TRIAL - docs/formats/race_screens.md section 8.1: 34 captures of
  the original's L1 / R1 route (`work/play/change_parts/cap3`) 0 differing pixels outside the 3D car in gt2game's own session,
  the career after it = the original's (all 0x7C9C bytes).
- **The views' music.** 0x800481C8(track) = EXE 0x80080F24(track, 1): an XA track of MUSIC.DAT, not CD-DA (both US images hold a
  single MODE2/2352 data track: the ISO volume size = the .bin's sector count; the Arcade .bin has no cue sheet). Requests: the
  post-race menu's wait view 0x8005AE0C (init 0x80049C68: track 8; after RESULTS or at once), 0x8005B7A0 (0x80050EE4: track 8 only
  in game modes 7..9: not after an arcade race), the session wait view 0x8005B09C (0x8004A7B4: track 8); stop 0x800481E8 =
  0x8007C570 (the leave views 0x8005AE30 / 0x8005B0C0). gt2game: `audio::MenuMusic` (MusicPlayer + its own device), track 8 at
  those points with the career's music volume (+ 0xB3), stopped when the views end. Not compared with the original's CD log.
- **The replay pause.** In the arcade loop's replays (after a race and the menu's Replay row) Start opens the race's pause menu
  0x80029D6C / 0x80029E80 (the s5 route: 10100:start, 10160:down, 10220:cross = Exit of the replay); Exit leaves the replay (the
  views follow). gt2game: RunRaceView opens the pause over any replay that has Panels (the arcade sessions pass them); the title's
  replays (argument 1 of the race overlay, 0x800A9500, title.md 11) keep Start = leave, as the original.
- **Lap steps.** 0x800109FC (replay camera) in game mode 6 with the hold 0 writes car[target] + 0x21 = -1 / + 1 on the generic pad's
  L1 / R1 (watch of 0x800A96A9 during the theater's Demo 02: pc 0x80010BBC, value FF after L1, 01 after R1); 0x80013EF0 reads and
  clears it (the ring's previous / next lap). gt2game: GameCamera gives the camera's car + 0x21 (CameraCar::ghostByte) to
  GhostSession::replayLapStep; the replay controls read the generic pad (circle / square / cross, up / down, triangle, L1 / R1 =
  0x800109FC's bits) and the keys C / O / I, Up / Down, T, Page Up / Page Down; the pad's view button no longer toggles the replay
  camera in replays (the original's 0x800109FC does not read it).

Verification:
- A ghost saved by gt2game loads in the original: the arcade Time Trial route with Load Ghost on the card gt2game wrote
  (`work/re/cards/lg1`, snap f12400): 0x801D55AA = 1 (arcade 0x801D500A) and race block entry 1, the parameter record, the reference
  lap's head and stream in RAM = the payload of our entry, byte for byte.
- The original's saved ghost loads in gt2game (`work/play/cards/lgn`: "loaded ghost 1 (920 bytes)", race 2 "ghost with the kept
  lap" on the loaded entry).
- The two saves of the same menu presses differ only where the races differ (the original's run had other laps) and in the
  description's residue bytes after the title / car name (replay.md 9.5): race block entry 1 (0xD0) and the parameter record (0x1C0)
  of both saves are equal.
- A Time Trial record gt2game saved (Save Replay ..., `work/play/cards/srn/card.mcd` entry 1, 2700 bytes, 4 laps) plays in the
  original's theater (the Simulation disc, `GT2_CAPTURE_CARD` + gt2verify --race-capture, Load Replay, `work/re/theater6/srn.bin`)
  and gt2game's playback of the same entry equals it: `gt2game <Sim disc> --replay work/play/cards/srn/card.mcd#1 --frames-compare
  work/re/theater6/srn.bin` 4936 frames (to the capture's end), 0 differ.
- gt2verify (title dump): RepGhostUnpack 0x80069D58 300 / 0, RepGhostPack 0x80069CC0 300 / 0, RepTitle 0x80069028 300 / 0.
- Frames (the session views' canvas with the interpreter's rules against `work/play/cards/sgcap`; GT2_ARCADE_SESSION_COMPARE on the
  GT2_ARCADE_RACE_SETUP run `work/re/arcade_tt/race.bin.setup`, the same presses 9250 fields earlier; `work/play/cards/sgcmp2`):

| SAVE GHOST screen | Capture | Native | Differing pixels |
|---|---|---|---|
| Select a Slot | c_11640 | 2355 | 954: the bar title's shine (not ported, title.md 9) |
| the list ("Demo 01", "- New File -", sector bar, "total 1 replay", "need 8 sector 114/149 sector free") | c_11800 / c_11900 | 2574 / 2617 | 0 / 0 |
| the keyboard / after a character | c_12000 / c_12130 | 2754 / 2886 | 0 / 0 |
| "Saving Complete" | c_12400 | 3125 | 0 |
| back in TIME TRIAL | c_12640 | 3396 | 0 (outside the car) |

  Before the string facts the list frames differed by 20230 / 23422 pixels (no "Race" / "need ..." / "... sector free": those
  strings had no arcade mapping).

## 18. Arcade menus: CAR / COURSE SELECTION widgets, garages, Bonus Items, Load Guest Garage (2026-09-19)

ARCADE v1.1 addresses (EXE SCUS_944.55 231f9dba..., member 2 304ee2b3..., member 3 d31f0158...). Evidence: objdump of the RAM
with member 2 loaded (`work\re\arcade_menu\ram.bin`), gt2run session `work\re\arcade_garage\s1` (a garage car's race build),
gt2play `--prims` captures (`work\play\arcade_menu\cap3` .. `cap8`, `work\play\arcade_garage\cap1`, `cap2`), for the EXE widgets
objdump of the Simulation dump `work\re\race_demo\ram.bin`. Facts: `db\arcade_us11_symbols.yaml` (block "CAR / COURSE SELECTION
widgets, garages, Bonus Items, Load Guest Garage"). Port: `src\game\arcade\arcade_widgets.*`, `arcade_car_page.*`,
`arcade_course_page.*`, `arcade_bonus.*`, `arcade_card.*`, `arcade_setup.*` (garage build); rows in
`tools\gt2verify\verify_arcade_menu.cpp`; dev aids `tools\gt2game\arcade_menu_check.*` (GT2_ARCADE_PRIMS listing),
GT2_ARCADE_COMPARE_MASK, GT2_ARCADE_CAREER_POKE.

### 18.1 Drawing model

A view draws into the menu's ordering table (8 slots, `ViewOt`): 0x80013828 puts the header gradient into slot 6 and its text
into slot 4; the widgets use slot 2 (their glyphs slot 3), bars slots 0 / 1; the GPU walks slots 7 .. 0 and every packet added
to a slot is drawn before the ones added earlier (`MenuOtSlot`). Text goes through the EXE text context (view + 0x1C4: font
descriptor, page word with the semi-transparency mode in bits 21..22, target slot, colour with bit 25 = semi; 0x8006AB78(ctx,
page) also clears +0x1C, the thick-underline byte). Fonts: 0x801294F0 cell 12, 0x801294E0 cell 7, 0x801234D0 cell 5, 0x80129500
cell 3 (arc_fontinfo), EXE 0x80092E1C. Two text objects exist: member 2's (0x2C bytes, 0x80019FC8 .., template height at +0xE)
and the executable's (0x28 bytes, height at +0xB; the dialog / bar templates EXE 0x80091BA0 / 0x80091BC0 = Simulation 0x80091EA8 /
0x80091EC8). The EXE dialog (0x8006DBC8 ..), button bar (0x8006E0DC ..), growing band (0x8006BD74 ..) and progress bar
(0x8006BF5C ..) are instruction-identical to Simulation + 0xF0 except for their data addresses (normalised objdump diff), so the
port reuses `screens::TextObject` with the arcade EXE image.

### 18.2 CAR SELECTION (view 0x80052238: 0x8001E890 / 0x8001EAA8 / 0x8001F124)

Carousel 0x800F04B0 (template 0x8004FB7C; 0x8001C790 .. 0x8001CBF4) over the car's widgets object 0x800F05C0 (two copies so the
old car's widgets close while the new ones open): drive badge + power text, power / torque graph (template 0x8004F488,
0x800121C4 .. 0x80013034; curve = 0x80075840 of the record 0x800770BC built from the car's player table 32 row), bars Max Speed /
Handling / Acceleration (0x8004F4A8; 0x80051F84), colour chips, spec box Weight / Max Power / Max Torque (0x8004F4C4 / 0x8004F4E0),
maker logo (sprites 0x8004F5F0, arc_maker), name logo (arc_carlogo container entry of the model number; 0x80016ECC / 0x80016CA4 /
0x8001713C), rule 0x8004FB8C, TRANSMISSION / SETTINGS bars 0x8004FBA0 / 0x8004FBB8 (0x8001AA98 .. 0x8001ADA8). Camera object
0x8001E520 (0x80016218, turned by 0x80016624 after the views): position 0x94CCC, pitch 0x5E, yaw 0x1500, screen 256 x 240,
window -128 / 128 / 90 / -50, H 400; the car is drawn into (48, 180) 256 x 200 on the menu car floor (semi colour 0xA2A2A2).

### 18.3 COURSE SELECTION (view 0x80052334: 0x80022C34 / 0x80022D98 / 0x800231AC)

Carousel 0x8004FF90 with the page callback 0x800229E0 (number 0x80027264 + name, a reverse course marked 0x7F, drawn subtractive),
normal / reverse groups; picture of arcade/course_map (0x80017C44 / 0x80017EF0, tpage 0x16); lengths 0x80017488 ("%dft" of the
picture record +0 / +4); record table 0x800176B8 (career + 0x218 + course x 0x24, three sectors and the lap, "- No Records -" or
car / name); label boxes 0x8004FFA0 / BC / D8 (EXE 0x8006BD74 / BDC4 / BE04); bands 0x8004FFF4 / 0x80050008 (0x80011EA0 ..).
Deviations: the original reads the picture from the CD over ~19 fields (ours: at once, so the transition frames right after a
course change differ in the picture area); the course movie (0x80013ADC / 0x80013B34, a 112 x 96 sprite at (24, 254) twelve
fields after a change, STREAM.DAT MDEC) is not ported - the interpreter has no MDEC either, the captures show the empty sprite.

### 18.4 Home / Guest garage

Blocks: home = career + 0x3C74 (RAM 0x801CCFB4, saved), guest = RAM 0x801D0FDC (behind the 0x7C9C-byte career, filled only by
LOAD GUEST GARAGE); 0x4028 bytes: +0 s16 count, +4 100 cars of 0xA4 (career::GarageCar). The class views enable the rows 4 / 5
while the block's count is > 0 (Rally: only cars with dirt tyres). Summary 0x80019F44 (home 0x800EFAB8, guest 0x800EFEA8; all 100
entries): 0x80019E88 per car (+1 part 0x2D owned, +2 class by 0x80019E44 = weight * 300000 / power^2 with a 32-bit wrap, 0x88B9
without power: < 0xE10 S, < 0x170C A, < 0x2AE5 B, else C; +3 drive, +4 power, +6 torque, +8 weight). HOME / GUEST GARAGE (view
0x8005228C, title 0x8004FC08[garage]; 0x8001F8CC / 0x8001FA8C): list widget 0x8004FBD4 (EXE list), rows 0x8001F50C -> 0x8001F21C
(name, chip EXE 0x8006BA18, "%dhp" 0x800F8295, class letter 0x8004FADC, banner 0x800453FC), the chosen car's page 0x8001F754
(specs with the "unknown" bars 0x80051FBC, no curve), name logo = VOL file carlogo/ at the model's index + 1 (checked for all
1110 models).

Race: 0x80010C84 with selection + 6 = garage, + 8 = index builds without a player entry (0x80010554(0, 0, event, 0, ...): six
drawn opponents in mode 4, none in mode 6), sets selection + 0x2C8 and context car 0 = the garage car's model id / paint. Member 3
at the race load (0x80012C00): the GT-mode tables, 0x8001290C -> 0x80011BE8 (tune sheet; rally stage 7 with dirt tyres 3 / 4 or
7 -> 0; stored back into the garage car = the career changes) and 0x800127AC (entry 0: model id, paint, configuration with
+0x7A | 0x40, name, block + 0x582 / + 0x584, record 0x800770BC with the GT tables). Port: BuildArcadeRace(garage block),
RebuildGarageEntry, ArcadeMenus::BuildRace (writes the home garage back into the career); gt2game's race loader takes GT tables
for entries with +0x7A bit 6.

### 18.5 BONUS ITEMS (0x800525C8) and ENDING CREDITS (0x8005261C)

BONUS ITEMS (0x80023CA8 / 0x80023D58 / 0x80023EB8, colour 0x0A16C0, title 0x800F8469): the rows of the table 0x80051790 (16 bytes
{name, s8 road, s8 dirt banner, s8 car mark, s32 tier, s32 flag byte n of career + 0xB8}) up to the first closed tier
(0x8002357C), in the list widget 0x80052524 (clamped 5 .. count - 5 every field; rows 0x80023BC8 -> 0x80023674: name, tier box
0x80052508[tier] (no tier 0x80052520), mark 0x800524A8 + 12 x (bit 2 -> 2, bit 1 -> 1, bit 0 -> 0), road mark 0x800524CC,
car sprite 0x800524D8 in 0x80052500 or 0x80052504 when bit 1 | 2, banner 0x80052454 + 12 k by road / flag), the legend sprites
0x80052484 / 0x80052490, the message 0x800F8490 and the dialog 0x80052558 ("Next"): its cross opens ENDING CREDITS, back leaves.
ENDING CREDITS (0x800240BC / 0x8002416C / 0x800242FC): panel list 0x800525A8 with the rows 0x80052580; Arcade Mode is open when
0x800235F4 (every table row with a flag byte has bit 1 or 2), Simulation Mode when career + 0x215 != 0 (0x80023664); logo
0x80052430 at (176, 150) with alpha |view + 0x18| x 128 / 12. A chosen row -> view 0x80052670, which after 24 fields returns 5 / 6
(0x800F36AE = row) and the top level plays the ending movie of STREAM.DAT (MDEC): not ported, gt2game shows a notice page.

### 18.6 LOAD GUEST GARAGE (0x800523DC)

View 0x80023478 / 0x800234C8 / 0x80023550 over member 2's own card manager (object *0x800F36E0, set-up 0x80025178 with
0x80051FD0: fonts copied to 0x800F3708 / 36E8 / 36F8 from 0x80129500 / 0x801234D0 / 0x801294E0, page 0x1E; open 0x8002521C ->
0x80025030: header band 0x800526C4, bars 0x800526E0 + 0x30 k (k = 0 error, 1 slot (cursor 1), 2 no card, 3 loaded, 4 load?),
progress 0x800527D0; per field 0x80025258: band tick, the five bars' updates, the state's step, chained enters; 10 = exit, 8 =
loaded (the view then rebuilds the guest summary); draw 0x80025390 into slot 4: the lines at (176, 214 / 250) in cell 7 colour
0x025C5248, "Memory Card N" (0x80052808) at (28, 126) over the band at (24, 110), the bars, the state's draw). The states
(table 0x80052810) are copies of the executable's card manager restricted to loading: 0 wait 24, 1 error (text +0x24 in 0x6F6F6F
at (176, 250), bar 0), 2 select slot, 3 check (status 1 wait, 2 no card, 4 error, 0 + "BASCUS-94455GAME" -> 6, else no file), 4 no
card (bar 2), 5 no file (bar 0), 6 "Start Loading?" (bar 4), 7 read (0x8006A0C4 of the 0x7EA0-byte file, CRC 0x8006A224, then
0x8006A1C4: the file's home garage, file + 0x200 + 0x3C74, 0x4028 bytes -> 0x801D0FDC), 8 "Loading Complete" / "No Cars Found"
(0x800F8475) with bar 3. The strings are data-global.txd's (block at 0x801EF0E0; member 1 0x8001D674). In the US language (career
+ 0 != 0) the helpers 0x80024440 / 0x800244A8 / 0x800244E8 (the Japanese text path) do nothing. Ours: the slots are the .mcd images
of gt2game `--card` / `--card2`, the transfer runs over 8 sectors per field (the original's rate is the card's). Checked: the guest
RAM 0x801D0FDC after the capture's load equals the card file's bytes 0x3E74.. (Python over the .mcd chain).

### 18.7 Verification

Frames: `gt2game "<Arcade disc>" [--card <copy of work\memcards\gt2_save_1car.mcd>] --window 352x480 --arcade-square --no-sound
--script ... --arcade-shot N out.png --arcade-compare <capture>.vram.bin` (software canvas, interpreter rules), scanned over a few
native fields around the capture's field:

| Screen | Captures | Result |
|---|---|---|
| CAR SELECTION (car / transmission / tyres, 21 transition frames) | cap3, cap4 | 0 pixels outside the 3D car's area 48,180 - 303,379 |
| COURSE SELECTION steady / transitions | cap3 course_4150, cap6 | 0 (transitions: the picture-load delay frames differ, 18.3) |
| HOME GARAGE list / car page | arcade_garage\cap1 f3050 / f3200 | 0 / 0 outside the car's area |
| BONUS ITEMS, ENDING CREDITS, transitions (new career) | cap7 f2126 .. f3000 (21) | 0 |
| the same with career pokes (tiers 3..5 open, flag bytes 0..7, career + 0x215) | cap8 (13) | 0 |
| LOAD GUEST GARAGE without a card | cap7 f3203 .. f3500 (5) | 0 |
| no card / Select a Slot / Start Loading? / Loading Complete / back | arcade_garage\cap2 (11) | 0 |
| loading (progress) | arcade_garage\cap2 f2950 | the progress segments only (timing) |

cap8 is taken with `gt2play --poke 1900 <0x801C9340 + offset> <word>` and the native side with GT2_ARCADE_CAREER_POKE =
"offset=word,..." (the same words; the scripts in `work\play\arcade_menu\cap8`). gt2verify on `work\re\arcade_menu\ram.bin`:
ArcadeGarageBuild (0x80010C84 with random garage blocks, modes 4 / 6, all RAM) 200 cases 0 mismatches, ArcadeGarageInfo
(0x80019F44 header and all 100 entries) 200 / 0, with ArcadeBuild 300 / 0, ArcadeAvail 200 / 0. End to end (garage car, gt2run
session `work\re\arcade_garage\s1` ram_003900, vsync 0xE85): race block, settings, record 0 and the home garage after the
tune-sheet store 0 bytes differ; gt2game races the garage car (`--ai-player` smoke run).

### 18.8 Not done

The course movie and the ending movies (MDEC; now section 20); 2 player Battle and the garage rebuild of mode 0 (now section 19).
The card screen's transfer timing and the course picture's CD delay are ours (18.3, 18.6). ENDING CREDITS rows chosen and the
loading error paths (corrupt file, card error) are ported but not compared with captures.

## 19. 2 player Battle (game mode 0, 2026-09-19)

US Arcade v1.1 (EXE SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95, GT2.OVL members ovl0 7360263f..., ovl2
304ee2b3..., ovl3 d31f0158...); ARCADE addresses unless marked "Sim" (US Simulation v1.2, EXE 3030aa27...; the race overlay's
routines are the same code, translated by the profile). Evidence: our objdump / Ghidra pseudo-C of member 2 (work\re\arcade_menu
decomp 80020700.c, 8002229C.c, 80020D88.c, 80022058.c, 80022658.c, 80020208.c, 8001DB10.c ..), of the race overlay in RAM
(work\re\arcade_race\ram.bin, objdump at 0x80010000), gt2play --prims captures (work\play\arcade_2p\menu1 / menu2 / cap0..cap2 /
p2win) and gt2run sessions (work\re\arcade_2p\s0..s4, results_s1..s3). Facts in db\arcade_us11_symbols.yaml ("2 player Battle").

### 19.1 Menus

- ARCADE MODE row 1 (career + 6 = the 2P laps -> selection + 4) pushes **2P GAME SELECTION** (view 0x800520E8: init 0x8001DB10,
  update 0x8001DB74, draw 0x8001DCFC; the panel list 0x8004F980 Road Race / Rally; cursor 0x801D5009, > 1 -> 0). The choice r sets
  0x800F364C (COURSE SELECTION's list kind) = 4 for Rally else 3, 0x800F0298 = (r == 1) -> selection + 0x2CC, selection + 2 (game
  mode) = 0 and pushes 2PLAYER BATTLE; after it COURSE SELECTION shows the 2P course lists (kind 3 -> list 5 *_2p roads, kind 4 ->
  list 6 dirt).
- **2PLAYER BATTLE** (view 0x800522E0: init 0x80020700, update 0x8002229C, draw 0x80022658; title "2PLAYER BATTLE", colour
  0x0010A0FF): both players choose at once, each with the state machine 0x80020D88(view, input, player, state) on its own pad
  (view + 0x1A4 + player * 0x10: +4 pressed, +0xC repeat) over its own copy of the single-player widgets:
  - player state (6 bytes at 0x800F3660 / 0x800F3666): +0 state (0 class list, 1 garage list, 2 garage car, 3 its TRANSMISSION,
    4 done, 5 class car, 6 its TRANSMISSION, 7 its SETTINGS, 8 done, 9 left), +1 class row, +2 car index, +3 spec refresh, +4 s16
    timer (list open 12 / 24 fields, carousel 12 / 20);
  - class lists 0x8004FDDC / 0x8004FDFC (x 0x60 / 0x100, y 0x84 / 0x128): rows 0x8004FC88 / 0x8004FD3C (A, B, C, Home, Guest),
    0x8004FC74 / 0x8004FD28 with S (0x8001D53C), Rally 0x8004FCEC / 0x8004FDA0 (Rally Car = class list 6, Home, Guest); a garage
    row is disabled (kind | 0x80) while its garage is empty (0x801CCFB4 / 0x801D0FDC count 0). Their panel items have scale
    0x40: 0x8001C218 then draws POLY_FT4 (0x80011C2C; 0x80011D58 semi with the page | mode) of half extents w * 0x40 >> 8, sizes
    * scale >> 7 (ported: arcade_menus.cpp PanelItemDraw, gt2formats MenuPrim::kPolyFT4);
  - garage lists (EXE list widgets 0x8004FEC0 + garage * 0x34 + player * 0x68, row callbacks 0x80020580 / 0x800205E0 / 0x80020640 /
    0x800206A0 -> 0x8001F50C with the order table 0x800F3440 + player * 200, the summaries 0x800EFAB8 / 0x800EFEA8);
  - carousels 0x800F16D0 / 0x800F16FC (templates 0x8004FE38 / 0x8004FE48; page callbacks 0x80020518 / 0x8002054C -> 0x80020208 with
    the layouts 0x8004FE58 / 0x8004FE6C: widgets 0 / 2 / 3 of the spec object at x + L0, L1 / x + L2, L3 / x + L6, L7, the bars at
    L8, L9 - drawn inside the page callback), specs 0x800F18E0 / 0x800F2560, TRANSMISSION bars 0x800F31E0 / 0x800F3278 (defs
    0x8004FBA0 / 0x8004FE80 = 0x8004FE98[player], flag bit 1 = slide from the other side), SETTINGS 0x800F3310 / 0x800F33A8 (0x8004FBB8
    / 0x8004FEA0 = 0x8004FEB8[player]); the maker logo scaled 0x40 at (x, y - h / 4 - 2) (0x80011C2C, not for bmmgn / brmcn), the
    name logo 0x8001713C with scale 0x40 (POLY_FT4; logo objects view + 0x23C / + 0x268 on texture pages 0x16 / 0x17);
  - labels "PLAYER 1" / "PLAYER 2" (texts 0x800F1470 / 0x800F149C, template 0x8004FC4C, P2 flags | 0x100) at (0x14, 0x6C) /
    (0x14C, 0x1C8), their bands 0x800F1438 / 0x800F1454 (templates 0x8004FC24 / 0x8004FC38) at (0, 0x58) / (0x160, 0x1B4), class labels
    0x800F1678 / 0x800F16A4 (template 0x8004FE1C, colour 0x8004FAC0[row], name 0x8004FAF8[row]) at (0x9C, 0x6C) / (0xC4, 0x1C8), the
    grow line 0x8004FC10;
  - the menu models view + 0x228 / + 0x22C (0x80016530 class car: paint = index % paints; 0x8001634C garage car) with the cameras
    0x800F1728 / 0x800F1804 (0x80020170: z 0xB0000, pitch 0x5E, yaw 0x1500, 0xD2 x 0x90, window (-0x69, 0x69) x (0x43, -0x11), H 400,
    opaque floor 0x404040; yaw += 16 per field while loaded) drawn in the environments view + 0xBC / + 0xCC with the drawing areas
    (0x8A, 0x5E, 0xD2, 0x90) / (4, 0x12E, 0xD2, 0x90) while view + 0x18 == 0 (24 fields after the entry);
  - the choices: selection + 0xA0 s16[2] garage (-1 = class car), + 0xA4 s16[2] garage index, + 0xA8 u32[2] car (a garage car's
    model id), + 0xB0 s8[2] SETTINGS 0x8004FBD0[bar] (tyre table), + 0xB2 s16[2] paint, + 0xB6 s8[2] TRANSMISSION 0x8004FBD0[bar];
  - 0x8002229C: a player's Back from the class list (return 0) closes the other player's widgets (0x80022058) and leaves the page;
    both players in state 4 / 8 -> COURSE SELECTION (sound 3); back from it: states 7 / 3, the bars and carousels reopen after 32
    fields (view + 0x1C), the cars after 24.
- The race build 0x80010C84 in mode 0 (BuildArcadeRace, section 16): event name "A2P" (member 2 0x800267B4) with the settings of
  "ATT" (0x800267B0), race block + 0..+7 = career + 1..+8, + 8 = 2, + 0x0A = 0, + 0x0D = 1, + 0x0F = selection + 4, two entries
  (entry i on grid slot i, kind i + 3) with records 0 / 1; a garage car's entry is left empty and made by member 3 (0x8001290C mode
  0: RebuildGarageEntries for each player with a garage car).

Native: game/arcade/arcade_battle.{h,cpp} (ArcadeBattlePage), ArcadeView::kGame2P / kBattle in arcade_menus.*, the second pad
(ArcadeMenus::Update(pad, pad2)), the two 3D cars (gt2view MenuCarView::UsePair / AppendPair) in tools/gt2game/arcade_mode.cpp.

### 19.2 Race

- Controls: entry kinds 3 / 4 = pad slots 2 / 3 (the shell's pad objects 0x800A9528 / 0x800A95D8 of ports 1 / 2). gt2game: player 1
  = the controller in use + the race keys; player 2 = the second controller (port 2, platform/input InputSystem::Port2) merged with
  player 2's keys (game_window.h kPlayer2Keys: I / K / J / L D-pad, U Cross, O Square, P Circle, Y Triangle, 9 / 8 R2 / L2, 0 / 7
  R1 / L1, T Start; a digital pad), also in the 2PLAYER BATTLE page. `--fake-pad2 <script>` scripts port 2; gt2run / gt2verify
  scripts accept `p2.<button>` (the interpreter's SIO port 2).
- Race load (Sim 0x8003C12C; the addresses of this bullet are Sim, as the gt2verify rows) in mode 0: 0x8003B73C(race block + 7, Slow Car Boost) writes settings + 0x1A..+0x1C and returns the
  catch-up enable; 0x8003B69C(block + 3, Tire Damage) the wear bytes + 0x1D..+0x23 (options 0 {0,0,100,0,0,0,0}, 1 {20,25,13,2,10,16,
  15}, 2 {10,25,13,1,10,8,15} with + 0x20 = 1); other modes (not 1 / 2 / 4 / 12) call 0x8003B69C(0) (race_sim.h RaceLoadSettings).
- Frame driver mode 0: Sim 0x80042038(body 0, body 1), 0x80041F68 per car (the catch-up of the car behind, race_sim.h CatchUpBattle).
- **Handicap Start** (race block + 6 = career + 7): the per-car setup 0x80012CD4 (the same address in both builds) passes Arcade 0x80033330 (Sim 0x80033384) a 13th argument -s1 with s1 =
  10h (kind 3, h > 0) / 1 (kind 3, h < 0) / -10h (kind 4, h < 0) / 1 (kind 4, h > 0), 0 for h = 0 and every mode but 0. Non-zero,
  it calls Sim 0x80039040 (Arcade 0x80038FEC): the start position `offset` metres along the main line (PickLine(6)):
  offset << 16 clamped (circuit +-(length - 20 m); point-to-point [last record's distance - length, last sector line - 20 m]),
  wrapped into [0, length), section = the first record at or beyond it (its s16 + 0x10 = the chunk hint), point = Sim 0x80035D68 at the
  distance, heading vector = the point 10 m further minus it; a negative offset starts the car in sector 0 of lap 0
  (ai_driver.h LineStartPlacement, car_setup.cpp StartCar, race_sim.h HandicapGridOffset).
- Player 1's dirt level: race block + 0x58 (Arcade 0x801D5314, Sim 0x801D58B4) -> body + 0x658 (0x80012CD4), now also set by
  arcade_race.cpp LoadRaceBlock.
- The standing start of the 2P block (+ 0x0D = 1) is kept even with a settings start speed (race_common.cpp LoadCourseData).

### 19.3 Split screen (tools/gt2game/split_race.*)

The race overlay addresses of 19.3 are Simulation v1.2 ones except where marked Arcade (the same code; the profile maps them).

- Sim 0x800297F4 draws both players' views: the rectangles 0x8002F1FC = (0, 0, 320, 120) / (0, 120, 320, 120), view + 0x2EA = split;
  camera objects view + 0xC4 + player * 0x110 with + 0x103 = 1 (0x80010088: half-height window (-160, 160) x (66, -66), the split
  chase offsets 0x8002F360); no rear-view mirror while split (0x800294D4).
- The 2P HUD Sim 0x8002E908 per car (car 0 also the start display Sim 0x8002A19C and the race end): TILE (0, 119) 320 x 2 0xC6C6C6; warning
  (160, 80 / 192), messages (160, 48 / 156), record (308, 16 / 124), needles and tachometer (276, 68 / 180), turbo (224, 92 / 204),
  tyres (296, 28 / 140), lap block (12, 16 / 124), no course map; the dial face slot = car + 0x880 (0x8002BD84: face k uploaded to
  slot k) (gt2view/hud.* Hud::Build2P).
- Race end Arcade 0x8002B11C (Sim 0x8002B170) sub-mode 0: the rows (0x8002AC8C, Sim 0x8002ACE0, with last argument 1: no player highlight) from timer 0xBE, one per 16; from
  0xEE the winner's text Arcade 0x801C74F4 (Sim 0x801C7814) + 24 * (row 0's car != 0) ("PLAYER 1 WINS !!" / "PLAYER 2 WINS !!"), colour Arcade 0x8006B458(0x8002F5B0,
  0x144080, min(t, 8), 8) (Sim 0x8006B548(0x8002F604, ..)), centred at (0xA0, 0xB4); the end timer restarts at each finish. (The P2 string is 24 bytes after the P1
  one; the port first used 23 and drew nothing for a P2 win - found with the p2win captures.)

### 19.4 Results

- RESULTS setup 0x80050EF0 (member 0) in mode 0: the winner = player 1 when both finished and p1 < p2, else player 1 when he
  finished, else player 2; u16 career + 0xFC (P1) / + 0xFE (P2) += 1; the place text of RESULTS = 0x801C74F4 + 24 * winner
  (arcade_results.h ApplyBattleResult; 19.7). Observed: gt2run session work\re\arcade_2p\results_s2 (the p2win race, X at the race end, pause Exit of the
  replay): f16899 call 0x80050EF0, write career + 0xFE = 1 (pc 0x80050FF4).
- After it the original shows RESULTS with both players' columns (total time, fastest lap, lap times) and the post-race menu
  "2PLAYER BATTLE" (Replay / Try Again / Save Replay ... / Exit) with "PLAYER 1 win n  PLAYER 2 win m" (shots results_s2 f17200,
  results_s3 f17900). Ported since: 19.7 (the views), 19.8 (the replays).

### 19.5 Verification

| Check | Command / input | Result |
|---|---|---|
| Race build mode 0 | gt2verify work\re\arcade_menu\ram.bin: ArcadeBuild (81 of 300 cases mode 0), ArcadeGarageBuild (73 of 200 mode 0, garage combinations) | 0 mismatches |
| Catch-up, race load settings, handicap placement | gt2verify rows CatchBattle 0x80042038 (3000), BoostSet 0x8003B73C (400), WearSet 0x8003B69C (400), LineStart 0x80039040 (600) on race_demo, license_race, arcade_race, arcade_tt s1, arcade_2p\race | 0 mismatches on every dump |
| The 2P dump | gt2verify work\re\arcade_2p\race\ram.bin "<Arcade>" tahiti_t_2p (ArcadeEvent compares the ATT row after RaceLoadSettings) | 243 rows ok, 0 FAIL |
| Regression | gt2verify race_demo (Sim, seattle), license_race (Sim, TC_lisence), arcade_race, arcade_tt s1 (tahiti_t); gt2game <Sim> --selftest --cars 6; the frames-compares of 14 / 17.9 / 17.10 | 260 / 244 / 247 / 253 rows ok, 0 FAIL; step 300 137.8 km/h rpm 6126 gear 3, 0 failures; 6184 / 11807 / 11807 / 11799 / 5216 frames, 0 differ |
| End to end | gt2game <Arcade> --fast --ai-player --script (the menu1 route, the course, Enter at the race end) | A2P tahiti_t_2p ccrcn / ia54n, finish 2:44.986 / 2:52.560 (the capture's times), career + 0xFC = 1, back to the menus |
| Race frames | GT2_CAPTURE_AI_PLAYER=1 gt2verify --race-capture (script work\re\arcade_2p\script1.txt; GT2_CAPTURE_POKE="2000:801C9347=03" for the handicap), gt2game --race --track tahiti_t_2p --ai-player --frames-compare | race.bin 6539 / hc3 (Handicap 3) 6539 / p2win 7039 frames, 0 differ |
| Cameras | GT2_CAPTURE_AI_PLAYER=1 gt2verify --camera-capture "<Arcade>" 3900 8000 300 <script1> | 4076 / 4076 camera objects |
| 2P HUD | gt2play --prims (# hud2p-ours) cap2 (12 captures) and p2win f14400 / f14700 / f15100 | every list equal in sequence |
| Race end | gt2game --race-screen-check race-end <capture> out.png [timer=N] on cap1 (P1 wins) and p2win f15250 .. f15700 (P2 wins) | 0 pixels at the frame's timer |
| Menus | gt2game --window 352x480 --arcade-square --script ... --arcade-shot N --arcade-compare <capture>.vram.bin, GT2_ARCADE_COMPARE_MASK = the two car areas "138,94,347,237;4,302,213,445" (menu1: class cars, work\play\arcade_2p\menu1; menu2: with work\memcards\gt2_save_1car.mcd, both players' home garage car) | 0 pixels outside the car areas: 2P GAME SELECTION, class lists, class car pages / TRANSMISSION / SETTINGS (f2600 .. f3000), garage lists and garage car pages (f2150 .. f2540), the transition to COURSE SELECTION (f2600); COURSE SELECTION differs in the movie preview only (compare runs have no movies) |
| Winner count | gt2run session results_s2 (the original) vs ApplyBattleResult | career + 0xFE += 1 in both |

The native field of an original field F: the menus' display is at F - 1224 (menu1) / F - 1201 (menu2) with the inputs 6 fields
earlier (the original applies a pad press later than gt2game); player 2's presses need one field more (menu2: + 1). The 3D cars
are not compared (the original loads the model from the CD about 35 fields after the choice: its yaw is ahead of ours by that).

### 19.6 Not done

- (Done 2026-09-19, 19.7 - 19.9: the mode-0 post-race views, the 2P replays and Save Replay, the display frame rate and the sound
  of the split screen; 19.10: the start phase of the split sound, the full-view replay HUD with the 2P course map, the
  HUD frames compared with captures.) Open: see 19.10 (the scene pixels of the halves, the replay cameras' HUD inputs).
- The model load delay of the menus (the original's CD read) is not modelled (as the car page, section 18).
- `--no-movies` is not in gt2game's list of title-mode flags (main.cpp kTitleFlags): with it the arcade disc starts the default race
  instead of the title (seen during this work; the movie-exit flag).

### 19.7 After the race: RESULTS on two columns, the "2PLAYER BATTLE" menu (2026-09-19)

Addresses: the views are named by their Simulation addresses (the race overlay's code is the same; Arcade = Sim - 0xE0 for the
code, the data 0x90 lower, as 17.10); Arcade addresses marked. Evidence: our objdump / Ghidra pseudo-C of the Sim race overlay
(`work/re/2p_post`: decomp of 0x80049D90, 0x8004A0BC, 0x8004A55C, 0x80050FD0, 0x8005162C, 0x80051BF4, the view manager 0x800472D4
/ 0x800472E0 / 0x800474F4 / 0x800477C4 / 0x800479AC / 0x800483A4, the wait / leave views 0x80049C68 / 0x80049D28), objdump of the
arcade loop 0x80016CBC (Arcade), gt2play `--prims` captures of the original (`work/play/arcade_2p/post1`: the p2win route with
Next / down presses, P2 wins; `post_p1`: the race.bin route (script1), P1 wins, captures by `--prims-on-call 80050EF0`; `post_rev`:
the lap reveal of RESULTS). Facts: `db/arcade_us11_symbols.yaml` ("2 player Battle", race text facts 0x801C782B / 0x801C6E3D).

- **The arcade loop** (0x80016CBC, Arcade) treats mode 0 like mode 4: RESULTS (0x800471F4(M, 0x8005B710)) when the race was run and
  either player's record has a place (+0 > 0: 0x80016D6C), then a second run of the view manager with the menu's wait view
  0x8005AD7C; M + 0x7C: 0 Replay (state 11), 1 Try Again (state 5), else Exit.
- **RESULTS setup 0x80050FD0 in mode 0** (W+0 = 1): the place text is the winner's string (0x801C7814 + 23 * winner; Arcade
  0x801C74F4 + 24 * winner - the arcade text pads its strings to 24 bytes) in place 1's colour 0x8005AB84[0]; W+8 / W+0xC = the
  two totals (records + 0xF8), W+0x10 / W+0x14 the fastest laps (+ 0xD0); the lap rows (W+2 = count) are those of the player with
  more kept laps (+4; a tie: player 1), from lap (+2 - +4) of that player, each player's time of the lap by 0x8005E378 (k = lap -
  (+2 - +4) in [0, +4): the kept lap's time, else 0xFFFFFFFF); W+0x68 the lap numbers. 0x80050B4C(winner) makes the RESULTS model
  the winner's car (*(0x800A9F00 + winner * 0xB40)); CD track 8 (1P: 8 for 1st, else 19 - the setup's 0x800481C8, missing in the
  earlier arcade port).
- **RESULTS draw 0x80051BF4 / list callback 0x800505AC**: with W+0 the times are right-aligned at x 210 and player 2's column at
  300 (1P: 250): total, fastest lap, and per lap row player 1's time with its "Lap n" label and player 2's time without a label.
  The reveal's ghost copies of the second column repeat player 1's row (0x800508C0 passes the same row object) - the overlapping
  "Lap 1" / times of the reveal fields are the original's (captures post_rev, 0 pixels).
- **The post-race menu 0x80049D90 in mode 0**: title "2PLAYER BATTLE" (0x801C6E3D = 0x801C78FC - 2751; Arcade 0x801C75DE - 2734 =
  0x801C6B30), the rows 0x8005AD58 (Replay / Try Again / Save Replay ... / Exit, as SINGLE RACE); instead of the 1P labels two
  labels W+0x574 / W+0x5C4 of the descriptors 0x8005ACB0 / 0x8005ACD0 ("PLAYER 1" / "PLAYER 2", red / blue bands) whose values are
  sprintf(0x801C7842 "win %d", career + 0xFC / + 0xFE) (the counts after the setup's +1); the update opens / ticks / closes these two
  (0x8004A0BC with s8 = mode 0), the draw puts them at (0x20, 0x186) / (0xD0, 0x186). The menu's model is car 0.
- **The view manager's runs** (both modes): 0x800471F4 (Sim 0x800472D4) only sets the stack; the task 0x800472E0 sets M+0x211 = 0
  and calls the first view's init; RESULTS' Next pushes the leave view 0x8005AE30 (init 0x80049D28: V+14 = 20, update 0x80049D58
  returns 4 at the 21st update: the task ends) with the 16-field forward transition; the second run starts settled with the wait
  view 0x8005AE0C (init 0x80049C68: V+14 = 20 - the earlier port had 16 -, track 8) which pushes the menu; a row pushes the leave
  view again. The header's spacing animation and the course band's ghost copies of the capture f17440 are the menu's forward
  transition.
- Port: `gt2view/race_result_screens.* ResultsInput::battle / winner / totalTime2 / fastestLap2 / laps2, PostMenuInput::wins`,
  `tools/gt2game/arcade_post_race.* RunArcadePostRace` (now on `screens::SessionViewStack` with the runs above and the card view of
  Save Replay; `BattleLapRows`, `BattlePostRaceInput`; one MenuCarView with UsePair for the winner's car and car 0 - two views would
  share the renderer's car slots: the user saw the RESULTS car missing / black with the first version), `arcade_mode.cpp
  RunArcadeBattleSession` (race -> replay -> RESULTS -> 2PLAYER BATTLE; Replay / Try Again / Save Replay / Exit).
- Frames (`GT2_ARCADE_BATTLE_POST_TEST=<capture ram.bin> GT2_ARCADE_POST_COMPARE="field:cap.vram.bin:side.png,..." gt2game
  <arcade.bin> --arcade-square --no-sound --script "<Next / down / enter>"`: the views on the inputs read from the capture's RAM, the
  interpreter GPU's rules; our field = capture field + 6 - (the setup's call field - 24); pixels inside the 3D model's rectangle
  counted apart):

| Capture (winner) | Frames | Differing pixels outside the car |
|---|---|---|
| post1 (P2): RESULTS opening / labels / the lap list / Next (16920 .. 17300), the passage (17420 black, 17440 the menu's transition), the menu 17500 .. 18160 (Replay, Try Again, Save Replay ..., Exit selected; presses at native 525 / 1083 / 1163 / 1243) | 14 | 0 in all |
| post_rev (P2): the lap reveal (17051 .. 17131) | 9 | 0 in all |
| post_p1 (P1): RESULTS 16620 .. 17049, the passage 17119, the menu 17144 .. 17449 | 14 | 0 in all (the menu one field earlier than our timeline: the second run's start is a phase of the original's task switch, 0 in post1 and the 1P captures) |
| 1P regression (work/play/arcade_results/cap, the documented presses with the menu 25 fields later: 1377 / 1463, down 1427) | 4 | 0 in all |

### 19.8 2-player replays (2026-09-19)

- **Recording**: 0x80013EF0 (pad slot 2, player 1's stream 0x801D5F84) and 0x80014030 (pad slot 3, player 2's stream 0x801DA49C, the
  pad object 0x800A95D8) -> 0x80013C90; both stop at 300 fields after the first finish (0x800A9522 is the shell's), so the replay
  ends ~150 frames after the winner's finish. `split_race.cpp SplitPads` records both; the result carries both stream objects
  flushed (0x800167D0).
- **Playback**: 0x800A951C set; both streams played; either stream's end sets 0x800A8D68 (0x80014030 as 0x80013EF0) and the replay
  ends. Both camera objects run the replay cameras (0x800109FC); Triangle of player 1's pad (T key) toggles camera 1's + 0x103 and
  0x800292A0 (19.9) turns the split off (player 1's full view, trackside camera following the leader, the 1P replay HUD) or on again.
  Start: the pause menu in the arcade loop's replay, leave in the title's (argument 1).
- **The record** (0x80069948 mode 0): the race block (0x58C), then per player the results record (0xFC) and the stream object (no
  parameter record); `replay.h ReplayFile::stream2`, `replay_card PayloadOfReplay(.., results2)` / `ToReplayFile`. Save Replay of the
  2PLAYER BATTLE menu pushes the card manager's view 0x8005B51C (mode 0, 0x80072E7C) - `ArcadeCardContext::replayPayload` of the
  battle session. The title's Replay Theater plays a record with a kind-4 slot in the split screen (title_mode.cpp; no longer "not
  available").
- **A replay's race load** (0x8003C12C): mode 0 applies Tire Damage (race block + 3) and Slow Car Boost (+ 7) to the event row's
  settings also in a replay; `LoadReplayRace` did not (the theater's replay of our record had the tyre wear bytes of the Tire Damage
  option in the original's cars and not in ours: 1500 of 1500 frames differed before, 0 after).
- Verification (`gt2verify --race-capture <Sim disc> 7200 <out> "1200:down,1260:cross,1400:cross,1550:cross,1800:cross"` with
  `GT2_CAPTURE_CARD=<card copy>`: the Sim title's theater, Load Replay, entry 0; then `gt2game <Sim disc> --replay <card>#0
  --frames-compare <out>`: both cars' bodies byte for byte and player 1's stream object, every frame):

| Record | Frames | Differ |
|---|---|---|
| gt2game's (GT2_ARCADE_RACE_SETUP=work/re/arcade_2p/race.bin.setup, player 1 on keys, player 2 on `--fake-pad2`, pause Exit, the replay, Save Replay ...: `work/re/theater2p/ours_card.mcd`) played by the original: `ours.bin` | 1500 | 0 |
| the original's (gt2play, both players on pad presses, pause Exit, the replay's pause Exit, Save Replay ...: `work/re/theater2p/orig_card.mcd`) played by the original: `orig.bin` against gt2game's playback | 1290 | 0 |

  gt2game's title theater plays the original's record (`--title --card <copy of orig_card.mcd>`, Load Replay): "2 player Battle",
  1290 steps, back to the theater. End to end (`GT2_ARCADE_RACE_SETUP=work/re/arcade_2p/race.bin.setup gt2game <arcade.bin>
  --ai-player --fast`): race (2:44.986 / 2:52.560), the replay (5367 frames), RESULTS, 2PLAYER BATTLE, Save Replay ... (entry 0
  stored), Exit.

### 19.9 Split-screen presentation and sound (2026-09-19)

- **Sim 0x800292A0** (Arcade 0x8002924C): the view's split flag (view + 0x2EA) follows the camera objects' + 0x103 - off and either
  split: both split, camera 1 on car 0 / camera 2 on car 1, replay mode 0, on; on and not both split: both unsplit with replay mode 2,
  off; the frame draws 0x800297F4 (mask 0 / 2) when it was on and 0x8002972C (the full view) when it is off - the frame that turns it
  on draws no view (`split_race.cpp SplitViewFrame`). The HUD: split - the 2P HUD (0x8002E908 per car); full - player 1's replay
  HUD ("REPLAY", the course map): captures `work/play/arcade_2p/replay1` 16250 (split), 16440 / 16520 (after Triangle: full),
  16640 (split again). Our full-view HUD is the 1P replay HUD of race_view (Replay Info layout of the options; the original's
  frame shows the large "REPLAY"; the 2P course's map TIM does not load: "TIM block does not fit", hud assets).
- **Display frame rate** (modern_graphics.md 2): the split race uses the race view's scheme - the steps on the paced field clock
  (stepAnchor), frames at the display's blanks / the cap, each view drawn between the last two steps: both camera objects
  (InterpolatedProjection, CameraCut), the cars and each car's four wheel transforms (steer / travel / rolling angle), the smoke,
  the HUD gauges and clock of both players. `--frame-cap 120 --vsync 0`: 120.0 fps, frame ms median 8.33 / p99 8.69, alpha step
  error median 0.0037 / p99 0.012, 30.0 steps/s; scripted / shot runs stay frame-locked (--interp-alpha for a fixed state).
- **Sound, Sim 0x80015DF4**: split (view + 0x2EA) -> 0x80014ED0: 0x800146D8(car i, rank 0, camera i) for the two player cars;
  else 0x80014E6C (every car against camera 1). car + 0x80C / + 0x808 (the vector / distance volume 0x800146D8 reads) come from the
  render's 0x800140A4 of the last view drawn, but 0x800297F4 saves and restores car 0's around player 2's view (Sim 0x80029888 ..
  0x800298DC): each car is heard against its own camera. 0x800146D8 takes the camera's own speed / direction (+ 0xFC / + 0xE4) for
  the doppler. Port: `RaceAudio::StepSplit` / `StepCar`, `Listener::cameraMotion`. Oracle (`GT2_SOUND_COMPARE=1 gt2game <Arcade>
  --race --track tahiti_t_2p --ai-player --frames-compare work/re/arcade_2p/race.bin`: the race capture's car records hold the
  sound objects car + 0xAA4; the logic fields of both cars compared, not the voice handles / SPU addresses): frames 309 .. 6538
  (the whole race after the start) = the original for both cars; frames 1 .. 308 (the start hold and 19 frames after it) differed
  in the doppler and one slot volume - found and fixed in 19.10 (all frames equal now).
  Both players' sounds go to one stereo output (the original's single SPU mix).

### 19.10 The start of the split sound, the full-view replay HUD, the HUD frames against captures (2026-09-19)

Addresses Sim v1.2 unless marked (Arcade = the same code: 0x800133F0 / 0x800146D8 / 0x80014ED0 at the same addresses, the HUD
routines 0x54 lower). Evidence: objdump of the race overlay in work/re/race_demo/ram.bin (Sim) and work/re/arcade_race/ram.bin
(Arcade), of the EXE's 0x80081164 / 0x80081500 / 0x8007BCA0; the race captures work/re/arcade_2p/race.bin, hc3, p2win (the car
records hold car + 0x808 .. + 0x870 and the sound objects), the gt2play --prims captures work/play/arcade_2p/replay1, cap1,
cap2, p2win (draw lists, RAM, VRAM). Facts: db/arcade_us11_symbols.yaml (sound_pass_split, car_render_transform,
race_hud_dispatch, race_hud_replay_caption, race_hud_tick, race_hud_frame_counter).

**The split sound's start phase** (19.9 left frames 1 .. 308 different). Three causes, each seen in the capture's car records:
- **The order of the frame.** The frame routine calls the physics tick 0x8003EBF0, the camera updates 0x800100F4, the HUD tick
  0x8002E550, then the sound pass (0x80015DF4 -> 0x80014ED0 / 0x80014E6C), and only later (0x80015EE0) the view's draw 0x800292A0,
  whose 0x800133F0 / 0x800140A4 write the car vector car + 0x80C, the distance car + 0x804, the distance volume car + 0x808 and
  car + 0x868. So the sound of frame k mixes with the view values of frame k - 1's draw (and its split flag) and the camera
  objects' own + 0xFC / + 0xE4 / + 0xEC / + 0xF4 of frame k. Capture: the sound object's master volume of frame k = car + 0x808 of
  frame k - 1 (3799, 3826, 3826, 3855 ..); frame 1 mixes the zero vector of no draw (doppler 0x1000, master 0, pan 0).
- **car + 0x868 is not the motion.** 0x800133F0: car + 0x85C = the last car + 0x830; 0x8001336C writes the new matrix and position;
  car + 0x868 = position - car + 0x85C; only then car + 0x830 += matrix * (0, 0, car + 0x5A) >> 8 (the CG offset). The velocity
  the doppler takes is thus the motion minus the offset: a standing car has the negated offset ((7999, 38, 22) for car 0 on the
  tahiti_t_2p grid), which is what moves the start hold's doppler (0x1215 against 0x11B4 before).
- **The GTE arithmetic**: 0x80081164 (Normalise: 0x80082C58 / 0x80082CE0, ported as camera::VectorLength) and 0x80081500 (MVMVA
  sf 1, IR1 saturated) instead of the float normalisation: pan / side / doppler were +-1 .. 3 off.
Port: `RaceAudio::StepTwoPlayer` / `PrimeTwoPlayer` (the view values of the previous draw kept per car: `CarView`, `ViewCar` =
0x800140A4, `CarVelocity` = 0x800133F0's car + 0x868, `MixCar` = 0x800146D8), `Listener::cameraPosition` / `cameraAxisSide` /
`cameraAxisRight` (the camera object's exact fields), the single-player `RaceAudio::Step` with the same velocity and GTE
arithmetic. Oracle: `GT2_SOUND_COMPARE=1 gt2game <Arcade> --race --track tahiti_t_2p --ai-player --frames-compare <capture>`:
race.bin 6539 / 6539, hc3 6539 / 6539, p2win 7039 / 7039 frames with both cars' sound objects = the original (0 differ).

**The full-view HUD of a 2P replay** (split off by Triangle). The HUD dispatch 0x800293D4: game mode != 0 -> 0x8002E818 (the
replay caption "Replay" + the followed car's name at (16, 210) / (16, 222)) and 0x8002E63C for car 0; **mode 0**: split ->
0x8002E908 per car with its camera, else **0x8002E63C(view, the car camera 1 follows, camera 1)** - no caption. In a replay
0x8002E63C draws by camera + 0x107 (Replay Info: 0 only the lap block, 1 the full HUD, 2 + the two panels 0x8002DA3C /
0x8002DBB0); in the split replay each half uses its own camera's (capture replay1: camera 1 = 1, camera 2 = 0 - player 1 had
toggled it, the option career + 0xAE was 0). The large "REPLAY" of the captures is the start display 0x8002A19C (timer 121 ..
240), drawn by 0x8002E63C in both views. The earlier port drew the 1P replay HUD of race_view (the caption, the options' Replay
Info, car 0, game mode 2 with the record block) - now `Hud::Build2PFull` (0x8002E63C with both cars' dial faces as the race start
uploaded them: slot = car + 0x880) from `split_race.cpp SplitHudOf`.
- **The 2P course maps**: tahiti_t_2p.tim and tahiti_d_new_2p.tim - with 31 other maps on the Arcade disc (33 of 119: the
  reverse Tahiti courses, laguna2, circle30 / 80, the test_* / l_* licence maps ..; 34 of 120 on the Sim disc) - have an image
  block whose length word says 0x240C for a 0x120C-byte block (96 x 96 4-bit, the data ending at the file's end). The EXE's
  TIM upload 0x8007BCA0 -> 0x8007BC1C / 0x8007BBD4 uses the length only to step from the CLUT block to the image and loads w x h words, so the original
  shows the map; our parser required length = 12 + w h 2 ("TIM block does not fit"). `LoadCourseMap` now reads the last block
  as the EXE does. The capture's VRAM (576, 144) / CLUT (368, 508) = tahiti_t_2p.tim's (white outline CLUT 0000, FFFF x 15).

**The HUD frames against the captures.**
- `gt2play <Arcade> --hud2p-check <capture>.ram.bin <out.txt> <capture.txt>` (dev aid): our HUD from the original's RAM of the
  snapshot (`TwoPlayerHudFromRam`, view + 0x2EA / camera + 0x107 / + 0x10C read too), its primitive list against the capture's
  draw list (equal in sequence), and that run rasterised with the interpreter GPU's rules twice (the capture's VRAM / our HUD
  VRAM copy `Hud::Vram`): **0 differing pixels** in every capture: replay1 split 16060 / 16100 / 16250 / 16380 / 16620 / 16640 /
  16700 (255 .. 267 primitives: both halves, the divider TILE, top half full / bottom half lap block only) and full view 16420 /
  16440 / 16520 (174 / 174 / 162 primitives: lap block, needles, dial, course map with the two dots, "REPLAY"); cap1 (13), cap2
  (12), p2win (19 race-end frames) - 278 .. 365 primitives each. (Before: replay frames 197 of 198 and 1 of 210 in sequence.)
- The game's own inputs: `GT2_SOUND_COMPARE=1 GT2_HUD2P_RAMS="<snapshot ram.bin>;.." gt2game ... --frames-compare <race capture>`
  finds the capture frame of each snapshot (both car bodies equal) and compares our HudFrames (`SplitHudOf`, what the split race
  draws) with `TwoPlayerHudFromRam` field by field: race.bin with cap1 + cap2 25 / 25, p2win 19 / 19 snapshots equal. This needed
  the counter 0x8002F864 (0x8002E550: +1 per frame while the race clock runs, the lap block's (n & 15) * 1000 / 900 ms): the split
  race kept our display frame counter before; now `SplitHudCounter`.
- The cameras of both halves: the camera objects are bit-exact (19.5 camera capture, player 2's camera included); each half maps
  its camera's projection into its rectangle (0x8002F1FC). The 3D scene of the halves is the native renderer's (not rasterised
  like the PS1), so its pixels are not compared.

Still open: the replay cameras' HUD inputs (camera 1's target / + 0x107 in a replay) are checked through the prims captures only,
not against a native replay run (the replay1 captures are not of a recorded race capture); the single-player race_view keeps its
own frame counter for 0x8002F864 and the float listener (its sound is not compared per frame).

## 20. Movies (2026-09-19, update of 16-18)

The statements above that the intro, the course movies and the ending movies are black / not ported (MDEC stub) are superseded: STREAM.DAT (27 movies, table 0x80092088: 0..23 course previews 112 x 96, 24 intro 320 x 192, 25 / 26 endings 640 x 216 / 640 x 224) is decoded natively (src/gt2formats/str_video.*) and by the interpreter's MDEC (src/machine/mdec.*); every picture the original displays equals the native decode (intro 4563, endings 3861 / 4863, course previews 1021 + 799 pictures, XA audio sample-exact). gt2game plays the intro (boot and every 4th attract cycle, Start skips), the endings (ENDING CREDITS rows 0 / 1, not skippable) and the course previews (silent). Details: docs/formats/str_video.md. Not ported: the Arcade title's attract race (3 of 4 attract cycles show a notice).
