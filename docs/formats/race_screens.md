# Race overlay screens over the race: pause menu, race-end display, licence prize

Status 2026-09-19. US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Code addresses are
in GT2.OVL member 0 ("ovl0", the race overlay, loaded at 0x80010000) unless marked EXE. Evidence: our disassembly and Ghidra
pseudo-C of RAM dumps taken with gt2run sessions (`work\re\rs_pause2` at the open pause menu, `work\re\rs_bronze` at the
licence B-1 prize, `work\re\rs_licmenu` at the licence menu; pseudo-C only under `work\re\*\decomp*`), and GP0 captures
of the original (`gt2play --prims`, `work\play\racescreens\cap\*.txt` + VRAM dumps). Port: `src/gt2view/race_overlay_screens.*`
(frames), `src/gt2view/panel_view.*` (Vulkan), `tools/gt2game/panel.*` (layers, the `--race-screen-check` comparison),
`tools/gt2game/race_view.cpp` (flow). The full-screen menus of the race overlay (licence test menu, event pre-race menu,
settings) are in the second part of this file.

## 1. Frame, VRAM, strings

- These screens are drawn into the race's 320 x 240 drawing area (double buffered at y 0 / 240) in the HUD's ordering-table
  slot, after the 3D scene: every packet is inserted at the head of the slot, so the GPU draws them in the reverse order of
  generation (a glyph = one packet {E1 page | mode << 5, SPRT}).
- VRAM they sample (captured VRAM equals the files word for word): `font/racefont.dat` at (384, 0) (tpages 6 / 7, text CLUT
  0x001A in its row 0), the HUD sheet `arcade/game_status_files` member 0 at (512, 0) (tpage 8: badges), and the image
  block of `arcade/license_tim.tim` (4-bit, 128 x 170 words, no CLUT block, CLUT rows inside the image) at (384, 256)
  (tpages 0x16 / 0x17: the licence prize pictures).
- Strings: 0x80028CC0 reads file id 8 (`.text/data-race.txd`) and copies the block of the career's language (career + 0,
  1 = USA) at file offset `language * 0x1915` (6421 bytes per language) to RAM 0x801C6C50; the code refers to the strings
  by those addresses (e.g. 0x801C6C99 "Finish", 0x801C6CA0 "Results", 0x801C6DE8 "Continue", 0x801C6DF2 "Exit",
  0x801C703C "BRONZE PRIZE!", 0x801C708B "FAIL", 0x801C8488 "KIDS PRIZE ACQUIRED!", 0x801C78D7 "Points", 0x801C78C6
  "Total Points", 0x801C78BD "%dpts"). Block 0 of the file is NOT the US block: `LoadHudStrings(vol)` (the HUD's
  captions, equal in both blocks) reads the whole file from block 0; the race screens use `LoadHudStrings(vol, language)`
  (hud_assets.h, the 0x1915-byte block the overlay copies; `RaceOverlayAssets::strings`).
- Fonts (EXE descriptors, `gt2::HudFont`): 0x80093130 (large digits, cell 12: pause buttons, times, result rows) and
  0x8009313C (the START / Finish / prize font).
- Colour interpolation 0x8006B548(from, to, t, n): per channel `from + (to - from) * t / n`, t clamped to 0..n, the top
  byte (bit 25 = semi-transparent) of `from`.

## 2. Pause menu (0x80029D6C input, 0x80029E80 draw)

State bytes 0x800A94C0 (s8 selection: 0 Continue, 1 Exit) and 0x800A94C1 (s8 counter: -1 closed; +1 per field, wraps
from 31 to 0; reset to 0 by a move). Input 0x80029D6C(view, pad): up (bit 0) / down (bit 1) move and clamp to 0..1, the
choose bits 0x10A00 close the menu (counter -1): Continue returns 0, Exit sets 0x800A8D68 = 2 and returns 1.

Draw 0x80029E80 (called from 0x800293D4 after the HUD, returns while the counter is negative): context with font
0x80093130, colour 0x02475B5B (semi-transparent, additive: context mode 1). With t = min(counter, 14) the selected button's
colour is `(0xF2 - 22 t / 14, 0xF2 - 82 t / 14, 0xF2 - 132 t / 14) | semi` (a white flash settling to 0x6EA0DC), the other
one 0x02244290. For i = 0, 1 (labels ovl0 0x8002F5E0 = {"Continue", "Exit"}): centre (0x9C + 4 i, 0x6D + 22 i); the
label centred at y + 7 (0x8006ADB4, spacing 0), the rounded button (colour), a second rounded button 0x02181818 (drawn
first by the GPU: a 50 % darkening under it), then E1 mode 0 (0x8007DA44(ot, 0)).

Rounded button EXE 0x800683FC(ot, {u32 colour, s16 x, y, w, h}, 32): TILE (x - w / 2, y - h / 2, w, h) and on each side
six flat quads (POLY_F4) from the end's centre (x +- w / 2, y) through the 13 points {s16 px, py} of EXE 0x80091A78 (a
half circle: px scaled by `h * 32 >> 17`, py by `h >> 12`); right side v0 / v1 / v3 = points i / i + 1 / i + 2, v2 the
centre; left side mirrored (v3 / v1 / v0 = points i / i + 1 / i + 2).

## 3. Race-end display (0x8002B170(timer, ot), from the HUD 0x8002E63C)

Timer = s16 0x800AF226 (race_shell.h `endTimer`: -1 while racing, 0 at the player's finish, +1 per race frame, held by
0x8002A700 while it waits for X); its companion 0x800AF228 (`endTimerAux`) starts at timer 0x9E. Context: font 0x8009313C,
mode 1 (additive for semi-transparent colours).

- "Finish" 0x8002AB60(ot, timer): while timer < 0x96, centred at (160, 0x82) spacing 0 in 0x0A376E; from 0x50 colour
  0x8006B548(0x8002F608, 0x8002F60C, timer - 0x50, 12); from 0x5C two more copies at 160 -+ (k << 3) / 12 with all three
  grey (0x90 - 12 k) | semi, k = clamp(timer - 0x5C, 0, 12) (they fade out additively).
- Licence test (sub-mode 0x801D5866 == 3):
  - result code 0x801D5DEC != 1: only "FAIL" centred at (160, 0xA0) in 0x00006E (spacing 0).
  - pass: "Finish"; from timer 0x78 + 1: the prize from the time 0x801D5DF0 against 0x8003D7B8(licence record + 0x44,
    k): gold (k 1) 0x801C7065 / colour 0x8002F620 / picture 0x8005B18C, silver (2) 0x801C7051 / 0x8002F624 / 0x8005B198,
    bronze (3) 0x801C703C / 0x8002F628 / 0x8005B1A4, and below the fourth time (4) "KIDS PRIZE ACQUIRED!" 0x801C8488 /
    0x8002F628 / picture 0x8005B1B0 only when the test record +1 == 0 and record +2 + 1 == licence record + 0x74 (the
    fourth-prize count reaches the settings' +0x30); otherwise "FAIL" in 0x8002F62C without a picture. With t = min(timer
    - 0x78, 12): colour 0x8006B548(0x8002F61C, prize colour, t, 12) (not semi-transparent); the prize text centred at
    (160, 0xCE) spacing -1 (font 0x8009313C); the time (0x80068734) centred in fixed cells at (160, 0xB2) (0x8006B49C:
    advance 8, narrow 7, sign flag 1, font 0x80093130); the picture (sprite descriptor {u8 u, v, u16 clut, w, h, tpage}) as
    a POLY_FT4 (0x8007E864) centred at (160, 0x6E): with k = (t << 4) / 12, half height `ease[16 - k] * (h / 2) >> 7`, the
    top edge narrowed and the bottom widened by `ease[k] * (h / 2) >> 8` (a tilt), colour 0x8006B548(0x8002F604,
    0x8002F5FC, k, 16) and 0x808080 (0x8002F600) at k = 16; texture page | 0x20. `ease` = ovl0 0x8002F5E8, 17 bytes
    128 .. 0. Sound 3 at t == 1.
  - from timer 0x5A + 1: TILE (0, 0, 320, 240) of (4 c, 4 c, 4 c) | semi, c = min(timer - 0x5A, 16), under E1 0x40
    (subtractive): the frame darkens by up to 0x40.
- Other sub-modes: "Finish"; 0x8002A630: badge 0x8002F804 with the aux timer, then badge 0x8002F7F8 with min(aux - 0x50,
  16), both at (160, 0xD8) (0x8002A3E0: a POLY_FT4 of the sheet sprite that grows in over 16 frames - half height `ease[16
  - t] * (h / 2) >> 7`, taper `ease[t] * (h / 2) >> 8`, colour 0x8002F604 -> 0x8002F5FC -, stays in 0x006680 until 64 and
  shrinks away over 16 more frames with the colours reversed); then per sub-mode:
  - 1 (GT-mode event), 6..10: no table; from timer 0x9E + 1 the whole frame darkens: TILE (0, 0, 320, 240) of 2 c per
    channel with c = 3 min(timer - 0x9E, 16) (up to 0x60), subtractive. The two badges of the HUD sheet are "OFFICIAL
    TIMER" (0x8002F804) and "SEIKO" (0x8002F7F8).
  - default (arcade 4 / 5 ...) and 2 (championship race, while timer < 0x13C, when the series has more than one race
    0x801D5DF6 > 1): the results table, one row per 16 frames from 0xBE: 0x8002ACE0(ot, t, row, car, leader laps,
    leader time, ctx, flag) - font 0x80093130, the position digit (row + 1) at (4 row + 0x12, 24 row + 0x51) spacing 1 in
    0x02004670, the car name (race slot 0x801D5948 + car * 0xD0) at (4 row + 0x1F, ...) spacing -1 in 0x606060
    (0x70543A for car 0 unless the flag), the time right-aligned at (4 row + 0x118, ...) in fixed cells (0x8006B3F4: 7, 6,
    1, 0): the winner's total time, else "Running" / the difference 0x80068B04 when finished / "%dLaps" "1Lap" behind;
    a rounded bar (0x800683FC) at (4 row + 0xAC, 24 row + 0x4A) 0x130 x 0x16 in 0x8006B548(0x8002F614, 0x8002F618, 8 - t,
    8), then E1 0.
  - 2 from 0x13C: "Points" rows (0x8002AF8C: "%dpts" of the race points 0x801D5E82[car], right-aligned 0x8006AE28),
    from 0x1BA "Total Points" rows (0x801D5E7C[car] in the order of 0x8005E6B0, bar colour 0x8002F610).
  - 0 (2 players): table with the flag set + "PLAYER 1 / 2 WINS !!"; 0xB: "YOU WIN!" / "YOU LOSE".
  - with a table: from 0x9E + 1 the band TILE (0, 0x32, 320, 0xBE) darkens by up to 0x40, from 0x96 a red TILE (0, 0x30,
    320, 2) 0x000080, the title ("Results" / "Points" / "Total Points") centred at (160, 0x2E) in 0x0A376F and a black TILE
    (0, 0, 320, 0x30).

Sub-mode 2 is set by "Start Race" of the event menu for EVERY GT-mode event race, single or series (watch of
0x801D5866 on the CBM0001 route: ovl4 0x800134A8 writes 1 when the race block is built, then the race overlay's
0x800178B0 -> 0x8001710C(state, a1, 2) writes race block +0xA = 2 at the race start). A single event therefore shows the
results table of the default path (0x801D5DF6 < 2); the Points / Total Points pages need a series. gt2game uses 2 for
all event races (career_race.cpp).

## 4. Verification (`gt2game <disc> --race-screen-check ...`)

Our primitive list against the capture's (text format of gt2play --prims, compared line by line) and both rasterised with
the rules of our interpreter's GPU on black (ours with our composed VRAM, the capture's with its VRAM dump); side-by-side
PNGs (capture frame | original's primitives | ours | differences) in `work\play\racescreens\`:

| Screen | Capture (gt2play --script, --prims field) | State | Primitives | Differing pixels |
|---|---|---|---|---|
| Pause, Continue selected (settled) | licence B-1 route + `4300:cross:1300,6000:start`, 6060 | selection 0, counter >= 14 | 78 / 78 | 0 (`pause_side.png`) |
| Pause, Exit selected (flash phase) | + `6040:down`, 6045 | selection 1, counter 2 | 78 / 78 | 0 (`pause_exit_side.png`) |
| Licence pass, "Finish" | `4300:cross:2375,6675:square:700`, 7000 / 7040 | timer < 0x50 | 12 / 12 | 0 |
| Licence pass, Finish brightening + darkening | same, 7080 | timer 91 | 14 / 14 | 0 |
| Licence pass, BRONZE PRIZE! (the X wait) | same, 7400 | timer 146, 0:39.002 | 79 / 79 | 0 (`licence_bronze_side.png`) |
| Licence pass, three fading "Finish" copies | same, 7100 | timer 101 | 38 / 38 | 0 (`licence_bronze_7100_side.png`) |
| Licence pass, prize and picture scaling in (tilted POLY_FT4) | same, 7140 / 7150 | timer 121 / 126 | 79 / 79 | 0 (`licence_bronze_7140_side.png`, `_7150_`) |
| Licence fail ("FAIL") | `4300:cross:3200` (overshoot, result 3), 7300 | result 3 | 8 / 8 | 0 (`licence_fail_side.png`) |

Event race end (2026-09-19, the original's AI driving the player's car, section 6): `gt2play --card <copy of
gt2_save_1car.mcd> --ai-player --script "<route of work\re\ev_route.txt>" --prims F cap\evend_F.txt` (Clubman Cup Rome
Short CBM0001, 6 cars, the player 3rd; race end timer from the capture's RAM `.ram.bin`, `--race-screen-check race-end
cap\evend_F.txt out.png [timer=N]`, the state is read from the RAM dump: timers, sub-mode, series block, per car
position +0x184 / laps +0x3C / finished +0x130 / time +0x1B4 of 0xB40-byte cars from 0x800A9C80, names 0x801D5948):

| Capture field | Timer (aux) | Contents | Primitives | Differing pixels |
|---|---|---|---|---|
| 15880 / 15960 | 20 / 57 | "Finish" | 12 / 12 | 0 |
| 16030 | 88 | Finish brightening | 12 / 12 | 0 |
| 16060 / 16100 | 101 / 121 | three fading copies | 36 / 36 | 0 |
| 16180 | 155 | band darkening, header | 16 / 16 | 0 |
| 16200 / 16240 | 162 (4) / 181 (23) | OFFICIAL TIMER badge growing / held | 19 / 19 | 0 |
| 16290 / 16330 | 202 / 219 | SEIKO badge, result rows 1..2 / 1..3 appearing | 73 / 73, 141 / 141 | 0 |
| 16420 / 16500 | 256 / 289 | six rows (times, "+0.154" differences), the X wait | 305 / 305, 359 / 359 | 0 |

Championship (GT300, `work\re\champ_route.txt`, capture card with the Dodge Concept Car LM, the autopilot of section 6;
race 1 with the script `work\re\champ_race1_script.txt` + `26150:cross,26500:cross,26850:cross`, race 5 with the
autopilot's whole run `work\re\champ_full_script.txt`): sub-mode 2 with 0x801D5DF6 = 5, the Results / Points / Total
Points pages and their X waits at 0x13B / 0x1B9 / 0x237:

| Capture | Timer | Contents | Primitives | Differing pixels |
|---|---|---|---|---|
| champ1_25700 | 144 | race 1: Finish copies, badge | 36 / 36 | 0 |
| champ1_25900 / 26040 | 241 / 305 | Results rows appearing / all six | 303 / 303, 437 / 437 | 0 |
| champ1_26200 / 26380 | 346 / 427 | Points rows ("8pts" ...) | 153 / 153, 407 / 407 | 0 |
| champ1_26560 / 26740 | 474 / 562 | Total Points (0x8005E6B0 order) | 223 / 223, 417 / 417 | 0 |
| champ5_134040 / 134120 | 280 / 315 | race 5 Results, the wait | 437 / 437 | 0 |
| champ5_134300 / 134380 | 400 / 441 | race 5 Points, the wait | 407 / 407 | 0 |
| champ5_134560 / 134650 | 527 / 567 | final standings (totals 30 / 11 / 34 / 19 / 6 / 20: the player 2nd) | 427 / 427 | 0 |

The player won races 1 and 4 and finished 2nd overall, so the champion's end view (0x80059800) was not reached (it was
captured later with a stronger car: section 5.4).

The timer of the listed frame is the dump's timer minus 0..2 (the dump is taken 6 fields after the listing starts);
the matching timers are stored next to the captures (`cap\evend_F.timer`).

## 5. Full-screen menus of the race overlay (352 x 480)

Established and ported by the menu sub-task (code `src/gt2formats/race_menu_assets.*`, `src/gt2view/race_menus.*`
(library gt2screens), comparison `gt2game <disc> --race-menu-check <licence|event|parts|msettings> <cap.vram.bin>
<side.png> ram=<ram.bin>`; the frame state is read from a gt2run RAM dump of the captured field + 6).

- Frame: 0x800479AC clears 352 x 480 black (E1 0x200), then the view's ordering table, highest slot first; slot 4 =
  the view header 0x80047024 (gradient POLY_G4 (0, 45 -+ 45 a / 128), the title in the 12-cell font with spacing
  0x22 - a / 4, grey 0x66 a / 128 over a black copy at (+2, +3), red TILE underline (x + 1, 78, w, 2)).
- VRAM: 0x80047B40 loads file 0x0D `arcade/arc_font.tim`, 0x80047CAC puts its image block at (384, 0) (page 6, CLUTs
  inside the rows, base 0x18) - the same place as the race's `font/racefont.dat`; 0x80047EA0 state 2 loads file 0x38
  `arcade/license_tim.tim` for sub-mode 3 and file 0x39 `arcade/setting.tim` for sub-modes 1 / 6..10 through 0x8018EC00
  and 0x80046FB0 to page 0x16 (384, 256); state 0 loads file 0x0E `arcade/arc_fontinfo` to 0x801A8C00; state 1 fills
  the font descriptors 0x801C9130 (cell 12), 0x801C9150 (7), 0x801C9120 (5), 0x801C9110 (4); work block 0x801C90A0.
  gt2game re-uploads the panel rows when it switches between the menus and the race screens.
- `arcade/license_info_us`: u16 6, u16 10, u32 0x4B4, then 60 records of 20 bytes {u16 title, u16 line count, u16
  lines[8]} (record = licence * 10 + test, licences 0 S .. 5 B), loaded to 0x80173894 (0x8004CCF8).
- Text objects (EXE 0x8006C460 init / 0x8006C4B0 open / 0x8006C514 restart / 0x8006C548 close / 0x8006C580 tick /
  0x8006CD04 width / 0x8006C5DC draw, 0x28 bytes): the selected row c0 0x907040 + flag 8, others c0 0x707070; colour =
  lerp(c0 -> c1 0xD7D7FF) over a 45-field period; disabled rows at alpha 64.
- Licence test menu (view 0x8005B470 colour 0xF07800, draw 0x8004F474): "License Test" right-aligned to x 0x154 at
  y 0x38, the test title at (0x5E, 0x68), bands 0x8005B238 (0, 0x50) and 0x8005B294 (0, 300), list widget 0x8005B39C with
  row callback 0x8004D474 (row 0 = the invisible test selector with arrows 0x8006BA48 around "B-1"), "CAR INFO" /
  "LICENSE INFO" 0x8005B254 / 0x8005B274 at (0x78, 0x9E) / (0x78, 0xEC), car data 0x8004D7D0 ("%dhp" = power * 1000 /
  0x3F6, drive 0 FR / 2 4WD / 3 MR / 4 RR / else FF, "Launch Speed at %d mph" = speed * 10000 / 0x3EDD, medal times
  right-aligned with chips 0x8006BB08), ten test boxes (polyline 21 x 29 every 0x16 from (0x78, 0x72), medals SPRT of
  0x8005B150). gt2game fills the car power from the record's figures (0x80075328: 67hp for the B-1 Vitz, as captured).
- Event menu (view 0x8005D36C "SINGLE RACE" 0x801C6E29, colour 0xF4BE73, draw 0x800585C0): course name centred on 0xB0 at
  y 0x70 over a 0x100 x 12 band, widget 0x8005D284 (callback 0x80057A70, row offsets 0x8005D27C {0, 0, 0, 0, 8, 16}):
  Replay, Test Run, Settings ..., Save Replay ..., Start Race, Exit (0x80057EAC: 6 rows outside mode 10); Replay and Save
  Replay are enabled when a replay exists. The view image's title word is "SESSION %d" (0x801C78DE); gt2game shows it
  with the race number for championship races (not captured).
- "Settings ..." -> CHANGE PARTS (view 0x8005D1C0, 0x80053CA8, groups 0x8005C364 / 0x8005C384 / 0x8005C3A4, records of
  0x90 bytes, stage of a part = sheet 0x8016E894 + 0x17B0 + kind * 2); L1 -> PARTS SETTING (view 0x8005D1E4,
  0x80056810; groups by 0x80055B14: suspension 0x8005C6E0 / 0x8005C650 (separate damper rows), brakes 0x8005C770, gears
  0x8005D100[k] / 0x8005D120[k] (0x8005C800.. / 0x8005CBF0.. with Auto Setup), aerodynamics 0x8005CFE0, others built at
  0x8005D080). Row record {+0 name, +4 JP name, +8 unit, +0xC value kind (0x800551BC: the configuration bytes shown),
  +0xD flags: bits 0..3 number format of 0x80054B9C ("%d", "%d.%d", "%d.%02d", "%d.%03d", x5 as "%d.%02d"), bit 7 a
  front / rear pair}. US labels / units: Spring Rate lb/in, Ride Height in, Damper (Bound / Rebound) level, Camber
  (degree sign), Toe in, Stabilizer level, Brake Controller level, Downforce, gears 1st .. 7th / Final, Auto Setup.
- Verified (0 differing pixels, `work\play\racescreens\{licence,event,parts,msettings}_side.png`): licence menu
  (capture `licmenu_8100`), event menu (`event_4900`), CHANGE PARTS (`parts_5300`: event route + `4950:down,5000:down,
  5050:cross`), PARTS SETTING (`msettings_5600`: + `5300:l1`).
- In gt2game: the licence and event menus replace our pre-race panel; "Settings ..." opens the original's PARTS SETTING page
  (below). (2026-09-19, later: every row works as in the original - section 5.5.)

### 5.1 PARTS SETTING interactive (states 0 / 1 / 2) - 2026-09-19

Code `screens::MachineSettingsPage` / `BuildMachineSettingsGroups` (race_menus.*), used by `tools/gt2game/settings_screen.*`.
- Groups 0x80055B14 (db entry; predicates 0x8005F800 / 814 / 820 / 834 / 88C / 8F0 / 904 / 918 on the sheet's stages),
  page P = [0x801C90F0] + 0x2A7C, update 0x80056194 via 0x800574C0 (result codes -> sounds), row callback 0x80055D50.
- State 0 groups (up / down; cross / circle / right enter; R1 = CHANGE PARTS; triangle / square leave + commit
  0x80056FF0); state 1 rows (left / triangle / square back; start = default 0x80060410; cross / circle opens the popup
  list 0x8005D154 with one slider object per GetSetting entry, labels Front / Rear, 1st.. / Final); state 2 = the popup
  (MenuListUpdate; the selected row's slider 0x80054D10: +-1 per press / repeat, held L1 -1 / R1 +1 per field, x10
  with left / right held; cross writes back 0x8005F9DC, triangle / square cancels).
- Draw 0x80056810: in-list slider rows 0x80055328 (value kinds 8, 0x12..0x17: Sports / Wide, Soft / Hard), selected
  row sprite lerp(base, group colour, a, 0x180) flashing to 0x02A0A0A0, description 0x801C90EC at (0xB0, 0x1BC),
  popup rows 0x80054E20 sliding in from the right, slider 0x8005480C (captions, number, knob TILE, POLY_G4 bar,
  outline), popup band 0x8005D188 with the row title, rows at alpha a - 100 * fade / 8 in state 2, no "R1 - CHANGE
  PARTS" in state 2.
- Verified: 31 GP0 captures (`cap\ms1_*`, `cap\ms2_*`, card = copy of `work\re\spec_msettings\card_tuned.mcd` (a2bsn
  with suspension 3, brake controller, gearbox 3, LSD, ASM, TCS), scripts `cap\ms_run1.script` / `ms_run2.script`):
  row selection, popup opening, steps, auto-repeat, R1 / L1, Auto Setup, LSD / ASCC / TCSC popups, cancel, start - 0
  differing pixels each (`--race-menu-check msettings <cap.vram.bin> <side.png> ram=<cap.ram.bin>`). The update replays
  the scripts from an early dump (`--race-menu-check msettings-route`): run1 33 / 33 later dumps equal (page, widget,
  sliders, whole sheet), run2 48 / 60 - the rest differ only in the stack residue the original writes into the gear
  entries above the top gear on the Auto Setup commit (tuning.h, known).
- gt2game keys: arrows = d-pad, Enter = cross, Backspace / Esc = triangle, Q / PageUp = L1, W / PageDown = R1, Home =
  start; directions repeat 28 fields after the press, then every 8 (observed in the captures, the EXE pad driver's
  repeat). R1 (CHANGE PARTS) only logs: CHANGE PARTS is not interactive in the port. Not ported: sounds of the page, its
  close animation, the view manager's header animation, the JP branch 0x800498B0, the power graph 0x800745B0.

### 5.2 After an event race: RESULTS, BONUS (prize money), post-race menu - 2026-09-19

Code `src/gt2view/race_result_screens.*` (`ResultsView`, `BonusView`, `PostRaceMenuView`, `PostRaceFlow`, labels
0x80048D14.., dialog EXE 0x8006DCB8.., button bar EXE 0x8006E1CC.., time display 0x800492C4, lap row 0x800490F8,
money 0x8005A11C / EXE 0x80068D0C), check `tools/gt2game/race_result_check.*` (`--race-menu-check results | bonus |
postmenu`, state from the RAM dump, or `sim=N` from our own set-up, or `advance=N press=K:button` from an earlier dump).
- Chain (ovl0 0x800172A8 / 0x80017C3C): wait view 0x8005B7A0 -> RESULTS 0x8005B7C4 (0x80050FD0 / 0x8005162C /
  0x80051BF4: place text 0x8005AB5C[p] in 0x8005AB84[p], TOTAL TIME 0x801D5F80, FASTEST LAP 0x801D5F58, lap list
  0x8005B76C with callback 0x800505AC, dialog "Next") -> leave 0x8005AE30 -> BONUS: single race 0x8005D558 ->
  0x8005D57C (0x80059A7C, Save / Next bar), championship race 0x8005D4C8 -> 0x8005D4EC (0x80059704, dialog), champion
  0x8005D510 -> 0x8005D534 (0x80059800: header 0xD3DE90, count 45 frames later, 3D prize "gtprz") -> wait 0x8005AE0C
  -> post-race menu 0x8005AE54 (0x80049D90 / 0x8004A0BC / 0x8004A55C; rows 0x8005AD98 Replay / Save Replay ... /
  Continue, 0x8005AD78 Replay / Next Session / Save Replay ... / Exit between races, title "CHAMPIONSHIP" in sub-mode 2).
  View switches take 16 fields (0x800474F4 / 0x800479AC, both views drawn shifted).
- BONUS: 0x800595E0(place, prize, prize car): shown money = money - prize, the prize counts over at t = 0x90 with step
  ((speed / 3)^2) / 50 + 1 (x10 + 1 while X / O held), sounds 8 / 3; "New Car Acquired!!" band (0x8005D454) at 0x92
  when a prize car was added (single race, 1st place, event with prize cars; no 3D car there).
- Verified (0 differing pixels and equal 2D primitive sequences): 44 captures `cap\post_results_*`, `post_bonus_*`,
  `post_postmenu_*` (CBM0001 flow `work\re\ev_flow_script.txt`; GT300 race 1 and race 5 with `work\re\champ_full_script.txt`,
  incl. `champ5_135300` / `champ5_135600`), plus 29 frames from our own set-up without input (text reveal, money
  counting, lap list opening) and 8 with presses (bar cursor, list scrolling, view switches). The 3D car of RESULTS /
  the post-race menu (0x80048754, clip rectangles (0, 0x96, 0x160, 300) / (0x7C, 0xA0, 200, 200)) is checked separately
  (section 5.4): its primitives and the pixels inside its clip are excluded from the 2D comparison and counted.
- gt2game: after an event race `career_race.cpp RunPostRaceViews` shows RESULTS -> BONUS (the applied result: place,
  prize, money, prize car) -> the post-race menu (Continue / Next Session; Exit -> Yes ends a series); Replay / Save
  Replay work since 2026-09-19 (section 5.5) (2026-09-19, later: "Save" opens SAVE GAME, the lap list is filled, the 3D car is drawn -
  section 5.4). Before: "Save" acted as Next (SAVE GAME 0x8005B588 not ported). The lap list was empty in the game (the
  race view does not return lap times yet).

### 5.3 Licence records: NEW RECORD name entry and the RECORD table - 2026-09-19

Code `src/gt2view/race_record_screens.*` (`NameEntry` = EXE keyboard 0x80073548..0x80073CE0 with row callback
0x8007306C, `NewRecordView` 0x8005B494, `RecordsView` 0x8005B4DC, `MenuListSelect` 0x8006D400), rules
`career::LicenceRecordRank` 0x8005DE8C / `StoreLicenceRecord` 0x8005DEFC / `TimeRecordSector` 0x8005DD94 / name
buffer 0x801D156F (results.h). Keyboard: layout 0x8009226C (12 rows x 16 + CANCEL / OK), cell 16 x 38 centred on x 176,
list 13 rows wrapping, 5 visible; triangle / square delete (with repeat), cross / circle type (max 11 characters,
width < 256) or CANCEL / OK, start jumps to OK, L1 / R1 move the caret, left / right the column. The view is pushed by
the licence menu's state 0x8004E104 after a pass whose time ranks; OK stores the time and the name.
- Verified: 12 captures `cap\rec_*` (keyboard opening, settled, cursor moved, scrolled, "BC" typed, delete, OK / CANCEL
  selected, records table sliding in, table, B-2) - 0 differing pixels; the update replays the scripts from an earlier
  dump with 0 field differences (`--race-menu-check newrecord | records`).
- gt2game: a licence pass whose time ranks opens NEW RECORD (keys as the settings page, Delete = square, Space =
  circle); OK stores the record and the name into the career. The RECORD table (`RunLicenceRecordsScreen`) is ready but
  not reachable yet: the licence menu's rows are built in race_view.cpp, "Records" stays disabled.
  (2026-09-19, later: "Records ..." is enabled and opens the RECORD view - section 5.4.)

### 5.4 The 3D car / trophy (0x80048754), the championship end (0x80059800), SAVE GAME, sounds - 2026-09-19

**Model views.** 0x80048754(model instance, env M+0xC0, env M+0xD0, camera, trophy) is the race overlay's version of the
GT-mode menus' car view (docs/formats/gt_menu.md section 10) with a camera object of its own (0xDC bytes): position
+0xA0..+0xA8 (16.16 m), angles +0xAC pitch / +0xAE yaw / +0xB0 roll, rectangle +0xC0..+0xC6 (only w / h are read by
0x8007B374), window +0xC8..+0xD2 (left, right, top, bottom, H, far; 0x8007B320), floor +0xD4 / semi +0xD5 / colour +0xD8.
V = Ry(-yaw) Rx(-pitch) Rz(-roll) (0x8007B14C / 0x8007B0C4 / 0x8007B1D4 = [[c, -s, 0], [s, c, 0], [0, 0, 1]], each V = V R),
eye = V position; the model at (0, 0x80061544(model), 0) unrotated (car: EXE 0x80067444 with the instance's reflection
CLUT / page / colour; trophy: 0x80048528, no ground shadow); then the floor disc (0x8006C274 ignores its radius argument:
always 4 m) with V = Rx(-pitch) only. The GPU draws the model environment right after the frame's clear and before the
views (their texts cover the car).

| View | Model / camera | Set-up | Per update | Environment |
|---|---|---|---|---|
| RESULTS (V+0x16 >= 0 from t = 0x18) | car 0 W+0x94 {0x7FD7, 9, *0x800A9F00, 1, 0x40}, camera W+0x1D8 | 0x80050BC4: pitch (r4 & 0x7F) + 0x50, yaw r1 & 0xFFF (generator 0x80083AE0 seeded by the VSync counter), z 8 m, 0x160 x 300, window (-176, 176) x (133, -52), H 400, floor 0x3E3E3E | 0x80050CC4: yaw + 12 | (0, 0x96, 0x160, 300) |
| post-race menu (after 16 updates) | car 0 W+0x220, camera W+0x364 | 0x80049780(200, 200): pitch 0x5E, yaw 0x1500, z 11 m, window (-100, 100) x (99, -24), semi floor 0xA2A2A2 | 0x80049874: yaw + 16 | (0x7C, 0xA0, 200, 200) |
| championship end (W+0x20C > 0x17) | trophy 'gtprz' W+0x210 {0x6028, 0x1A, 0x8015F894, 0, 0x20}, camera W+0x354 | 0x80049780(0x160, 0x1E0), floor off (W+0x428 = 0) | 0x800593F4(t = W+0x20C - 0x18, at most 0x78): z 4 -> 12 m and roll -((0x5A - t) * 6 + 0x40) -> -0x40 over 90 updates, y 0.3 m, yaw + 8 | full frame |

(z += 0.5 m per update when M+0x234 = u32 0x801F0698 exceeds 0xFB90: 256 / 257 in every dump, never in play.) Port:
`menu::OverlayModelCamera` / `ResultsModelCamera` / `ModelViewCamera` / `TurnModelCamera` / `ChampionModelMotion` /
`OverlayModelProject` / `OverlayModelFloor` (src/game/menu/menu_car.*, the exact integer GTE math of the menus' port),
`screens::PostRaceModel` / `PostRaceView::Model()` / `PostRaceFlow::Frame(a, modelAt, model)` (race_result_screens.*).
gt2game draws it with `Panels::FullScreenModel`: a MenuView on the panel rows (the clear and the floor at the far depth,
the 3D layer, the views in front; polygons interpolated) + `MenuCarView` (the race renderer's car items in the 352 x 480
frame; reflection page 9 / CLUT 0x7FD7 = the course's map the race leaves in VRAM; the trophy without shadow and without
reflection pass - its own map, uploaded by 0x80059800, is not loaded).

Check (`gt2game <disc> --race-menu-check results|bonus|postmenu <cap.vram.bin> <side.png> [vulkan]`, the camera objects
read from the dump): the floor disc primitive by primitive, the silhouette of the captured model primitives against our
model's LOD 0 triangles (wheels, ground shadow) through our projection (IoU, boxes, centroids; `<side>_model.png`), with
`vulkan` the renderer's own pixels (one shot with and one without the model; the views' 2D pixels left out;
`<side>_vulkan.png`):

| Captures | Model | Floor | Silhouette IoU | Renderer (vulkan) |
|---|---|---|---|---|
| post_results_* (CBM0001 8, GT300 race 1 4) | a2bsn | 24 / 24 equal | 0.953 .. 0.963 | 0.906 (16000) |
| post_postmenu_* (11) | a2bsn | 24 / 24 equal | 0.945 .. 0.961 | 0.924 (16950) |
| champion champ_136111 .. 137071 (23 with the trophy drawn) | gtprz | off | 0.966 .. 0.994 | 0.966 (136871) |

`sim=N` (our set-up from the dump's results): the VSync counter dump - N gives exactly the dump's RESULTS pitch / yaw (4
dumps: 0x80050BC4 verified), and our BonusView from its own set-up gives the dump's trophy camera (champ_136161, 83
updates: 0x80049780 + 0x800593F4 verified).

**Championship end (captured).** The GT300 series was won by the original's AI in the Mitsubishi FTO LM Edition (m-tor,
557 PS / 930 kg; capture card = work/memcards/capture/champ_dphr.mcd + `gt2tool career-buy` + current car =
work/memcards/capture/champ_m-tor.mcd; `gt2play --card <copy> --ai-player --script <work/re/champ_route.txt> --auto 5150
<autoscript> --prims-on-call 80059800 <offsets> <prefix>`, work/play/champion/r2_m-tor; the Lotus Esprit GT1 and Honda S2000
GT1 runs did not win). Order: last race -> RESULTS -> BONUS (0x80059704, the race's prize) -> post-race menu (Replay /
Save Replay ... / Continue) -> "Continue" -> 0x80017A28 -> view 0x8005D510 (16) -> 0x8005D534 (0x80059800 at f136071:
bonus 100,000 + the random prize car, "New Car Acquired!!" at t 0x92, the trophy) -> X -> the leave view 0x8005AE30 ->
the GT-mode menus (no post-race menu after it). 2D: 26 captures (t -42 .. 207: labels, counting, the prize-car band, the
Save / Next bar) = 0 differing pixels and equal primitive sequences (`work/play/champion/side`); the first dump (f136079)
is inside the set-up's CD load (W not written yet) and is not comparable. gt2game runs this flow (our former
"Championship Result" panel is gone; rules: results.cpp ApplyChampionshipEnd).

**SAVE GAME** (view 0x8005B588 = the EXE card manager 0x80072F9C(0) / 0x800728F0 / 0x80072B78 under the race overlay's
header "SAVE GAME"): "Save" of a Save / Next bar (BONUS of a single race, the championship end) runs the title's port
(game/shell CardManager) on the cards of the options (--card / --card2, default saves\card1.mcd); the chain goes on as
after "Next". Drawn with the title's VRAM and header, not the race overlay's. Captured (not yet compared):
work/play/champion/save_run (the r2_m-tor run replayed with `137000:left,137040:cross,...`: "Save" of the champion's bar at
f137040 -> view 0x8005B588 from f137060; the race overlay's header "SAVE GAME" (red gradient, colour 0xF4) over the EXE
manager's body: "Select a Slot" + Memory Card Slot bar, "Game File Already Exists / Overwrite File?" + Yes / No bar; the
presses chose No, the card stayed unchanged).

**Prize car of the championship end (captured).** Dumps f136056 (before 0x80059800) and f137066: money 555,000 -> 655,000
(bonus 0x801D55AC = 100,000), garage 3 -> 4 cars, the new one t2mmr paint 'd' = prize block car 2 of {n2xsr, t2wsr, t2mmr,
t286r} (0x801D55C8 = 4), career +0x215 stays 0 (0x801D5DDC = 0, not a GTW series). 0x800597C4 = r % 4 of the generator
seeded by the VSync counter gives 2 for the counters 4k (136060 = the call's field 136071 - 11; the dumps show counter =
field - 10): consistent with a set-up one field before the call's counter, not an exact proof.

**Other wiring.** The licence menu's "Records ..." opens the RECORD view (RunLicenceRecordsScreen); RESULTS gets the lap
list of the results record (RaceViewResult::lapTimes / lapNumber); sounds (0x80060840 through game/audio MenuAudio, an
output of its own next to the race's while the race overlay's screens run): the post-race views' requests, PARTS SETTING
(0x800574C0 + the slider's 8), the licence / event menus (0x8004EEB0 / 0x80058108: list move 6, back 0, a leaving row 3).
Not done: the keyboard / RECORD screens' sounds (the views do not collect them yet), the pause menu's, the close
animations of the menus.

### 5.5 The GT loop's states; Replay, Save Replay, Demonstration, Test Run - 2026-09-19

Evidence: our disassembly of ovl0 (work/re/rs_licmenu/ram_007990.bin: 0x80015FF8, 0x80017098 .. 0x80017D14, the tables
0x8002EF20 / 0x8002F058 / 0x8002F088 / 0x8002F0B8 / 0x8002F110), Ghidra pseudo-C of the menus (0x8004EEB0, 0x80058108, 0x80057EAC,
0x800585C0, 0x8004E464 / 0x8004E494), gt2run sessions (work/re/lic_demo/s1: Demonstration; work/re/testrun/s1: Test Run, pause Exit,
the replay, the menu; work/re/menu_lic3: licence pause Exit), captures of the original (`gt2verify --race-capture`, `gt2play --prims`).
Facts: db/sim_us12_symbols.yaml (race_loop_run_states .. licence_menu_rows).

**The loop.** The race overlay runs a state machine (0x80015FF8; the GT-mode object's vtable 0x8002F110 "GranTurismoRaceLoop"):
4 init -> 5 -> 8 the pre-race menu (skipped when loop + 0x5D0 = 0) -> 9 the race -> 10 -> 11 -> 12 the replay -> 13 -> 6 the
post-race state -> 7 leave. The menus' rows write a code into M + 0x7C (0 Replay, 1 Start / Test Run, 2 Exit, 4 Start Race; the
negative codes push views: -2 SAVE REPLAY 0x8005B51C, -3 Settings, -4 Records 0x8005B4DC, -5 Demonstration 0x8005B44C).
- State 10 (0x80017964): a finish (0x800A8D68 < 2) sets M + 0x240 ("a replay exists") and M + 0x241 (the menu's car) and goes to
  the replay at once; the pause's Exit (2) does the same in a licence test and a Test Run, but leaves the overlay (state 7, no
  replay) in an event race (sub-mode 2).
- State 6 (0x80017A28): the licence: 0x8004DF34 and the licence view 0x8005B404 (0x8004E104: the record, NEW RECORD, then the menu);
  sub-mode 1 (the event before Start Race): the event menu again (code 1 races at once: loop + 0x5D0 was cleared; 0 the replay; 4
  0x8001710C); sub-mode 2: RESULTS -> BONUS -> the post-race menu, or (after the menu's Replay, loop + 0x5D1 = 0) the menu's wait
  view 0x8005AE0C directly; code 0 -> the replay again.
- Test Run: init 0x80017500 runs 0x80017098 for sub-mode 1 (race block + 0xF = 100 laps, + 0xD = 0 no countdown, + 0x5A = 1 car,
  the other entries + 0x8C = 0; the values kept in loop + 0x5D2..); "Start Race" = 0x8001710C(loop, 1, 2) restores them and sets
  sub-mode 2 (every event race runs in game mode 2: AI catch-up by the settings, the rear-view mirror, the HUD's Record / Best Lap).
  The HUD of a Test Run is sub-mode 1's ("Lap 1", Record / Best Lap).
- Demonstration (0x8004E494): the test's demo run from the disc (EXE 0x80069EF8: VOL number u16 0x801E30B2 + s8 0x80091C8C[licence]
  * 10 + test; 0x801E30B2 = the number of "/license/a_lia00.lgf.gz", EXE path list 0x8009118C entry 225, written by the boot
  0x80010228) read to 0x801D585C (a RAM image: race block, slots, player 1's stream at + 0x728, a parameter record at + 0x4C40 that
  0x80069F28 copies to 0x801DE8BA, race block + 9 = 1), M + 0x240 = 0 (the Replay row off), code 0 -> the replay state.
- Menu rows: licence 0x8005B364 {selector, Replay, Start, Records ..., Save Replay ..., Demonstration ..., Exit}; event 0x8005D244
  {Replay, Test Run, Settings ..., Save Replay ..., Start Race, (Ghost Options ...), Exit}: Replay / Save Replay enabled by M + 0x240
  (the event menu also needs 0x801C90F4 == 0, set to 1 by "Settings ..."); the event menu opens on row 0 (captured), the licence
  menu on Start; with M + 0x241 the event menu draws race slot 0's car (0x80048754 with the post-race menu's objects W + 0x220 /
  W + 0x364: camera 0x80049780(200, 200), turned 16 per update, drawn in (0x7C, 0xA0, 200, 200)).
- The GT events' grid (ovl4 0x80013108 six-car path): entry i on grid slot 5 - i (0x80012F7C), the player (entry 0) starts last.

**Port** (`tools/gt2game/race_view.cpp` RaceFlow, `career_race.cpp`): after a race of the menus the replay plays at once (Start in it
= the pause, Exit ends it); the licence result (records, NEW RECORD) is applied after the replay; Replay / Save Replay / Test Run /
Start Race / Demonstration / Records as above; SAVE REPLAY = `screens::CardView` (race_card_screens.h) in a view stack on the menu's
assets with the payload of the race (race block with the options' copies + 1..+7 and the race's name, slots, the player's parameter
record, results record, stream); the post-race menu's Replay plays the race again and comes back to the menu's wait view, its Save
Replay returns to the menu with the list kept. `gt2game --replay licence-demo:B-1` plays a demonstration (`LicenceDemoReplay`).
(2026-09-19, later: the transmission dialog, the licence menu's test selector and the view manager's slide transitions around the card
view are ported - section 5.6.)

**Verification.**
| What | Oracle | Result |
|---|---|---|
| Demonstration B-1 | the original's run (`gt2verify --race-capture`, licence route + `3700:down,3730:down,3760:down,3800:cross`) against `gt2game --replay licence-demo:B-1 --frames-compare work/re/lic_demo/demo_b1.bin` | 1246 frames, 0 differ |
| Test Run race (1 car, 100 laps, mode 1, grid slot 5) | the original's Test Run (`work/re/testrun/tr.bin`, script `work/re/testrun/script.txt`) with its race block, `--frames-compare` | 704 frames, 0 differ |
| gt2game's Test Run saved by its SAVE REPLAY (`work/play/gtmenus/tr2/card1.mcd`) | played by the original's theater (`work/re/testrun/ours2_theater.bin`) against gt2game's playback | 3400 frames, 0 differ |
| gt2game's event race (CBM0001, the player's entry AI-driven) saved from the post-race menu (`work/play/gtmenus/ev4/card1.mcd`) | the original's theater (`ours_ev_theater.bin`) | 4742 frames, 0 differ |
| gt2game's licence run (pause Exit, the replay) saved from the licence menu (`work/play/gtmenus/lic3/card1.mcd`) | the original's theater (`work/re/lic_demo/ours_lic3_theater.bin`) | 1303 frames, 0 differ |
| The event race in game mode 2 | the original's CBM0001 race (`work/re/testrun/ev_race.bin`) | 1630 frames, 0 differ |
| SINGLE RACE before a race / after the Test Run's replay with the car | `gt2play --prims` 4900 / 9300 / 9700 (`work/play/gtmenus/cap`), `--race-menu-check eventmenu` | 95 / 95 primitives, 0 differing pixels outside the car's area, floor 24 / 24, silhouette IoU 0.959 / 0.955 |

Card bytes against the original's own save of the same test (`work/play/theater/card_b1_replay.mcd`, another run): the parameter
record and slots 1..5 equal; the race block differs only in + 0x44 (the original's sponsor category of a licence is "0", ours "General01") and
the tail + 0x57C.. (FF FF .. 01, not in our replay); slot 0 equal (no name, as the original's licence slot); results and stream differ with the run. Against the captured Test Run block (`tr.bin.setup`) our saved block
differs in the stale + 0xB (5 in both captured event blocks: a licence byte of an earlier race) and the sponsor seed + 0x54 (the
VSync counter at the load); the opponents differ with the seed. The directory entry's residue bytes and the sectors' tails are the
original's buffer residue (replay.md 9.5): byte identity of whole cards is not reachable.

### 5.6 TRANSMISSION, the licence test selector, the menus as views, sponsor category - 2026-09-19

Evidence: our disassembly of ovl0 / ovl4 (`work/ovl/sim_us12/*.asm`; Ghidra pseudo-C `work/re/rs_licmenu/decomp` 0x8004EEB0, 0x8004ED00,
0x8004F474, 0x80058108, 0x80057EAC, 0x800585C0, 0x8004D474, 0x80057A70, 0x8004E320, 0x8004E494, EXE 0x8006E388 / 0x8006E43C) and GP0
captures with RAM dumps (`gt2play --prims`, `work/play/atmt/cap`: licence route `1400:cross,2000:right,2060:cross,2400:cross,2700:cross`
+ `3500:cross,3600:right,3700:cross` (lic_*) or `3400:up,3430:up,3460:right,3560:right,3660:left,3760:cross,3860:cross,3960:cross`
(sel_*), the save route of `work/play/theater/s7` + `10100:cross` (sr_*), the event route `work/re/ev_route.txt` on
`gt2_save_1car.mcd` + `4950:down,5000:cross,5100:right,5200:cross` (evt_*) or `4950:up,5000:cross,5100:left,5200:right,5300:triangle`
(evx_*, ex2_*)). Facts: db/sim_us12_symbols.yaml (licence_menu_update .. race_block_tail).

**TRANSMISSION bar.** The EXE two-button bar (0x8006E1CC, the post-race views' `ResultBar`) from ovl0 template 0x8005AC20 ("TRANSMISSION",
"AT", "MT", sound 5) at (0xB0, 0x19A): licence W+0xEC (0x8004ED00), event W+0x484 (0x80057EAC); the event menu also has "Exit?" (Yes /
No, 0x8005ACF0) at W+0x518. Both draws put the bars first (0x8006E5B8 before everything else of 0x8004F474 / 0x800585C0).
- Licence: Start (row 2) plays sound 1, sets W+0x169 = (0x801D156E != 0), opens the bar, view + 0x1A = 1 (the description at a third,
  the list gets pad 0); cancel (triangle / square) sound 2; a choice writes 0x801D156E (career + 0x7C8E = garage + 0x401A, saved with
  the career) and 0x801D5947 (race slot 0 + 0x8F), sound 3, code 1 (view 0x8005B428, 20 fields, then the race). Captured after MT:
  0x801D5947 = 1, 0x801D156E = 1.
- Event (sub-mode 1 / 10): Test Run (code 1) / Start Race (4) open the bar the same way (W+0x501, view + 0x18 = 2, + 0x1A = the code)
  unless race block + 0x588 bit 3 is set - the race overlay's entry 0x80011F64 copies the garage car's + 0x98 bit 14 (0x800178E4: the
  catalogue gearbox has fewer than 3 gears) there; then the code goes on at once with + 0x8F = 0 (ovl4 0x80011000's fifth argument).
  Exit (code 2) opens "Exit?" with W+0x595 = 1 (No): Yes -> sound 3, code 2 (0x8005AE30), No -> sound 1, back -> sound 2.
- Port: `screens::LicenceMenuView` / `EventMenuView` (gt2view/race_menu_views.h), `RaceFlow::transmission` (the career byte) and
  `transmissionDialog` (!bit 14); the race's `RaceOptions::manual` = the choice (race_view.cpp). gt2game's saved replays of a licence run
  with MT (`work/play/atmt/g5`, `g7`) hold the race block, the six slots (slot 0 + 0x8F = 1) and 0x53C..0x58B equal to the original's RAM
  after its MT choice (`cap/lic_3730.txt.ram.bin`): 0 differing bytes.

**Licence test selector (row 0).** 0x8004EEB0: the list's -2 on row 0 with left / right (pressed or repeated, pad + 4 / + 0xC): sound 5,
block + 0x4B8 -1 / +1 wrapping 0..9, block + 0x51C = 8, 0x8004CCF8 (the test's title / description); row 0 chosen -> sound 1 and
0x8006D400(widget, 2) (the selection to Start). 0x8004F474 draws the car / licence info (0x8004D7D0: the car of the menus' table
0x801DA4B8 + test * 0x44 + licence * 0x2A8, launch speed and medal times of the test's record 0x8004CA90) at alpha
max(0, 0x80 - fade * 0x80 / 8) * band alpha >> 7 (fade = block + 0x51C, 8 also at every setup), and the title / description only while
view + 0x18 (the update's input argument) != 0. The race is the selector's only at Start: view 0x8005B428's update (0x8004E340..)
rebuilds the race block with 0x8004C7A0 (= ovl4 0x80010078) when race block + 0xC != block + 0x4B8, then race block + 0xEB = career +
0x7C8E; the demonstration (0x8004E494) and RECORD (0x8004FDC4: + 0x5DE = block + 0x4B8) use the selector's test too; every setup
(0x8004ED00) puts the selector back on race block + 0xC. Port: the view's selector, `RaceFlow::licenceMenuOfTest`, RaceExit::kChangeTest
(career_race.cpp builds that test's race and starts it at once, `RaceFlow::startAtOnce`). gt2game's B-2 started from B-1's selector and
saved (`work/play/atmt/g4/card1.mcd`) = the original's B-2 race block after its selector Start (`cap/sel_4100.txt.ram.bin`) except slot
0 + 0x8F (MT chosen in ours, AT in the capture).

**The menus as views; the view manager's transitions.** The licence menu (0x8005B470) and the event menu (0x8005D36C) are views with
their objects: bands W+0x4BC / + 0x500 (opening from anim 0 at the setup), the labels W+0x418 / + 0x468 (0x80048D14, the post-race
views' `ResultLabel`), the list 0x8005B39C / 0x8005D284 with the row objects (text W+0x1F8 / W, band W+0x338 / W+0x140; callbacks
0x8004D474 / 0x80057A70), the course title W+0x440, the bars. Setup: view + 0x14 = 12 (licence) / 16 (event) updates until the list and
labels open (0x8006CE70, reveal period -1); a leaving row closes the list (0x8006CED8), the bands (anim ~steps), the labels / course
title and clears view + 0x18. The view manager (0x800474F4 / 0x800479AC, `screens::SessionViewStack`): the codes 0 / 1 / 2 / 4 push the
leave views 0x8005B428 (licence) / 0x8005AE30 (event), -5 0x8005B44C, all 20 fields; -2 pushes SAVE REPLAY (0x8005B51C) - the menu
slides out while it slides in (16 fields); the card manager's exit pops back (0x800483D8: the menu's setup(1) 0x8004ED00 / 0x80057EAC,
the card view sliding out). gt2game runs the same (race_view.cpp: the menu borrowed into the stack; career_race.cpp
RunSaveReplayScreen with the lent menu, also for the post-race menu 0x8005AE54). Not done this way: RECORD (-4) and Settings (-3) stay
modal (no slide; the menu's setup(1) afterwards).

**Verification** (`gt2game <disc> --race-menu-check licence|event <cap.vram.bin> <side.png> ram=<cap.ram.bin>` [`view`: every object
from the dump] [`from=<ram.bin> fromfield=N tofield=M script=<gt2play script>`: our views and view manager run field by field from an
earlier dump, the views' state compared with ram=; licence runs push our CardView on `card=` (default work/memcards/card1.mcd)]):

| Captures | Check | Result |
|---|---|---|
| lic_3480..3640 (8: bar opening, settled, MT moved), sel_3402..3862 (12: selector, test change fade, Start), evt_4990..5140 (7), evx_5004..5340 (10) | settled states from RAM | 0 differing pixels each |
| same licence frames with `view` | every object from RAM | 0 |
| lic_3603 / 3612 (from lic_3480), sel_3432..3862 (11, from sel_3402), lic_3702 / 3712 (the bar's choice, the menu closing, leave view; from lic_3640) | logic run | view state = dump, 0 differing pixels |
| evt_5004..5230 (9, incl. the leave transition after MT; from evt_4990), ex2_5004..5302 (4: Exit?, Yes / No, cancel; from ex2_4930) | logic run | view state = dump, 0 differing pixels |
| sr_8801 / 8809 / 8817 (Save Replay: the menu closing and sliding out, SAVE REPLAY sliding in; from sr_8790) | logic run with CardView | view state = dump, 0 differing pixels |
| sr_10101..10125 (the card view's exit: the menu's setup(1) and opening, the card view sliding out; from sr_8790 through the whole save) | logic run | licence view state = dump at every frame; 10117 / 10125 0 pixels; 10090 / 10101 / 10109 differ only in the card bar's flash phase (ours finishes the card write earlier: transfer timing, accepted) |

**Sponsor category** (sponsor_boards.md): race block + 0x44 = the name pool string of the licence / event row's u16 + 0x94 (licence
data: LJB00 "0", LJB06 "General01" ...; events: `gt2tool events` "tag"), + 9 = 1: the race's boards use it (none for "0" and the seed
+ 0x54 is not written then), the saved replays carry it; with the race block's tail (+ 0x57C.. result index, GTW flag, player, garage
slot, power limit, flags - db race_block_tail) gt2game's licence replay equals the original's own save (`work/play/theater/
card_b1_replay.mcd`) in the race block, all six slots and 0x53C..0x58B (0 differing bytes; was: + 0x44 and the tail).

### 5.7 The GT-mode MACHINE TEST (race sub-modes 7 / 8 / 9) - 2026-09-20

Evidence: our disassembly (`work/ovl/sim_us12/ovl0.asm`, `ovl4.asm`, `exe.asm`), Ghidra pseudo-C of ovl0 (`work/re/mtest/ghidra/decomp`,
dump `work/re/mtest/s7/ram_007500.bin`), gt2run sessions (`work/re/mtest/s1..s7`: 0-400 m of `work/memcards/gt2_save_1car.mcd`, route
`1400:cross,2100:left,2160:cross,2400:up,2450:left,2500:left,2550:left,2600:cross,3050:down,3100:cross,3450:down,3500:cross` - the map
from the loaded GTF page, down = Machine Test (GM page 929), down = 0-400m (right = 0-1000m, right right = Max Speed)), GP0 captures
`work/play/mtest/cap` (script `script.txt`). Facts: db/sim_us12_symbols.yaml (block "machine test").

- **Menus -> race.** The map page's items carry the names G400 / G1000 / GMAX (ovl4 0x80022F20.. , 0x8001861C -> sub-mode 7 / 8 / 9); the
  menus' result 2 runs ovl4 0x80012C6C(name, sub-mode) instead of an event setup and the day counter does not advance. 0x80012C6C: race
  block cleared; + 0..7 = career + 1..8, + 8 = 2, + 9 = 1, + 0xA = sub-mode, + 0xB = 5, + 0xD = 1, + 0xF = 1 lap, + 0x5A = 1 car; the
  name (0x8005E548 of row + 0), the course (row + 2: TC_lisence for G400 / G1000, maxspeed for GMAX; 0x80083004 -> 0x8005E590), + 0x44 =
  row + 0x94; + 0x57C = -1, + 0x582 = -1 -> 0, + 0x584 = the garage index, + 0x586 = 0, + 0x588 = 1; 0x801C98A0 = row + 0x44; the race
  tyres 0x80018004(index, 0, 0x80019538(name)); slot 0 = the garage car (0x80011000(block, 0, -1, 3, 0, car, 0, index): grid slot not
  written, + 0x8E = 3, + 0x8F = 0, the name 0x80060AE8, the record 0x800771AC); the car names of the RECORD view (0x8001F124 at
  0x801D5FA0: s16 count, 0x48-byte {car id, name} per record entry + the current car); the Settings page's sheet 0x801DA4B8 (0x80011160 /
  0x80011184 / 0x800129B8 = the code of 0x80015404 / 0x80015428 / 0x80016C5C). Port: `career::PrepareMachineTest` (row MtSetup).
- **The race.** The race shell's sub-modes 7 / 8: a line at 400 m / 1000 m of course distance counts as lap 1 (0x8003C70C,
  race_shell.cpp LapCheck), 1 lap = finish; 9: the max-speed readout is not reset at the lap line. The race overlay's loop
  (0x80017500 init copies 0x801D5FA0 -> 0x80169894 and 0x801DA4B8 -> 0x8016E894; 0x80017784 state 8; 0x80017A28 state 6; 0x80017200):
  the menu, the race, the replay at once (state 10: M + 0x5CC / + 0x5CD; also after the pause's Exit), then with M + 0x5D1 and the results
  record's position > 0 the record views, then the menu (0x8005D348 waits 20 fields and pushes it). Menu code 0 Replay -> the replay (no
  record views after it), 1 Start / Try Again -> the race (state 5), 2 Exit -> the GT-mode menus. Every race end (0x80017964) writes the
  player's dirt (0x801DE8B8 = (body + 0x658 << 12) / 600000, 0x800131AC) into the garage car + 0xA2; every race start (0x80017784) puts
  the car's + 0xA2 into race block + 0x58.
- **Menu** view 0x8005D390 "MACHINE TEST" (setup 0x800587BC, update 0x800589BC, draw 0x80058D20): the event menu's objects with the widget
  0x8005D2F0, the rows 0x8005D2B8 (Replay 0, Start / "Try Again" when a replay exists 1, Settings ... -3, Records ... -4, Save Replay ... -2,
  Exit 2; y offsets 0x8005D2E8; Replay / Save Replay enabled by M + 0x240 and 0x801C90F4 == 0), the course title "0-400m" / "0-1000m" /
  "Max Speed" (data-global 0x801EFE0E / 0x801EFE18 / 0x801EFE23) at (0xB0, 0x68), the TRANSMISSION bar for code 1 unless race block +
  0x588 bit 3, no "Exit?" bar; -4 pushes RECORD 0x8005D3B4, -3 the Settings page 0x8005D1C0, -2 SAVE REPLAY 0x8005B51C.
- **After the race** (0x80017200): the wait view 0x8005B7A0 (24 fields; 0x80050EE4 -> 0x80050D78: the entry {race slot 0 car id, the
  results record + 0xD0 time (7 / 8) or + 0xE0 max speed (9, larger is better), name ""} into career + 0x3A88 / 0x3B2C / 0x3BD0 by EXE
  0x8005E0D0; W + 2 = the rank, W + 8 = time, W + 0xC = speed) -> ranked: NEW RECORD 0x8005B7E8 (setup 0x80051F54 / update 0x80051FC4 /
  draw 0x80052144: the keyboard W + 0x5C8 with descriptor 0x8005B5F8, OK stores the name with EXE 0x8005E03C) -> RESULTS 0x8005B80C
  (0x80052170 / 0x8005232C / 0x80052638: "RESULTS" label 0x8005B60C at (0x28, 0x88), the rank text 0x8005AB5C[rank] in 0x8005AB84[rank]
  or "OUT OF RANKING" 0x801C7A0E in 0x1652C4 at (0xB0, 0xB8), the label 0x8005B6A0 patched to "RECORD" at (0x28, 0xF0), the value
  (0x800492C4: time, or in sub-mode 9 the speed + unit 0x801C6C87) centred at (0xB0, 0x110), the car as RESULTS, the "Save Game?"
  bar 0x8005B5AC from t 0x73 on "Next") -> the leave view 0x8005AE30 -> 0x8005D348 -> the menu.
- **RECORD** view 0x8005D3B4 (0x8005916C / 0x8005918C / 0x80059240): eight rows 0x80058E28 at (0x20 + 4 i, 0x78 + 0x2C i) sliding in
  (4 updates apart) and out; rank "%d.", the value (time / speed), the name, the car name of 0x80169894 (EXE 0x80074AE4), "-- No
  Records --" 0x801C7016, the rounded bar; a face button (after 0x10 updates) returns 2 (the manager pops back, the menu's setup(1)).
- **HUD** (0x8002D308 in sub-modes 7..9 -> 0x8002D20C): "Record" and the career record's entry 0 (a time at x + 3, or the speed +
  unit; "----" of 0x8002F2E8 when empty). Lap block 0x8002C76C as sub-mode 1 moved up 0x12.
- **Port.** `src/game/career/machine_test.*` (PrepareMachineTest, WriteMachineTestRecord, StoreMachineTestName, InsertMachineTestRecord),
  `src/gt2view/machine_test_views.*` (MachineTestMenuView, MachineNewRecordView, MachineResultsView, MachineRecordsView, AddSpeedDisplay),
  `src/gt2view/hud.cpp` (Hud::MachineRecord), `tools/gt2game/career_race.cpp` (RunMachineTest, RunMachineTestViews; `--career <save>
  --event G400 --headless`), `tools/gt2game/race_view.*` (RaceFlow::machineTest / recordsView / raceEnded, RaceViewResult::playerDirt,
  RaceViewConfig::machineRecord), checks `tools/gt2game/machine_test_check.*` (`--race-menu-check mtmenu | mtnewrecord | mtresults |
  mtrecords`), `tools/gt2verify/verify_machine_test.cpp`.

**Verification.**
| What | Oracle | Result |
|---|---|---|
| MtName EXE 0x8005E03C / MtRecord ovl0 0x80050D78 | gt2verify on `work/re/mtest/race/ram.bin` (TC_lisence) | 400 / 1200 cases, 0 mismatches (728 cases with the entry's 11-byte name tail masked: strcpy of "" leaves 0x80050D78's stack there) |
| MtSetup ovl4 0x80012C6C | gt2verify on `work/re/mtest/menu/ram.bin` (the machine test page) | 150 cases, whole RAM equal |
| 0-400 m race (pad: cross held) | `gt2verify --race-capture` `work/re/mtest/cap/m400.bin`, `gt2game --track TC_lisence --frames-compare` | 639 frames, 0 differ |
| 0-1000 m / Max Speed (GT2_CAPTURE_AI_PLAYER) | `m1000.bin` (TC_lisence) / `mmax.bin` (maxspeed), `--ai-player --frames-compare` | 1628 / 3825 frames, 0 differ |
| Menu, NEW RECORD, RESULTS, RECORD | 28 captures `work/play/mtest/cap/mt_*` (menu settled / TRANSMISSION bar / with the car, keyboard opening / settled, RESULTS reveal / bar / car, RECORD rows sliding in / settled, view switches) | 0 differing pixels outside the 3D car, equal primitive sequences; the car's floor 24 / 24, silhouette IoU 0.955 .. 0.966 |
| The views' logic | `advance=N press=K:button` runs from earlier dumps (3950 -> 4003 down, 4003 -> 4045 cross, 4045 -> 4100, 4100 -> 4203 cross, 7300 -> 7420 start, 7462 -> 7600 -> 7700, 7700 -> 7806 cross, 8130 -> 8250, 7880 -> 7950; press K = P - F - 5) | 0 differing pixels each |
| HUD in the race (Record 0x8002D20C, lap block, countdown) | `gt2play --prims` "# hud-ours" (fields 4450 .. 5570 of the capture script) | 6 / 6 frames: our list a contiguous run of the captured frame |
| Race end ("Finish" copies) | `--race-screen-check race-end` h_5570 | 36 / 36, 0 pixels |
| Career bytes after a test | the original with its AI driving (`gt2run session ... aiplayer`, `work/re/mtest/ai2`, RAM after the menu is back) vs gt2game `--menu --career ... --ai-player --auto-race --save-out` | 0:21.349 in both; the 0x7C9C career bytes equal except the 11 stack-residue bytes after the entry's name (career + 0x3A95..0x3A9F) |

Not verified by a capture: the sub-mode 9 display branches (RESULTS' speed value, RECORD speed rows, the HUD's speed record) - ported
from the disassembly; the pop transition back from RECORD (the checker draws forward switches only; SessionViewStack's back
transition is the one verified with SAVE REPLAY); the replay HUD's "Replay" label order / car name of gt2play's inputs (all our
primitives are in the capture, the label at another place of the list - the same for every sub-mode).

## 6. Oracle capture aid: the original's AI drives the player's car (tools only)

A pad script cannot drive a whole race, so the screens after an event race were never reached in the interpreter. The
dev tools can now let the ORIGINAL's AI drive the player's entry (`tools/gt2run/ai_player.h`; `gt2run session ...
aiplayer`, `gt2play ... --ai-player`). This changes guest state and is a capture aid of the RE tools only - the product
(gt2game) runs the native simulation and does not use this guest-state hook.

- Switch: the race overlay's entry setup 0x80012CD4 calls the car start 0x80033384 at 0x800130DC (`jal`, word
  0x0C00CCE1) with the control class as its 8th argument (stack word sp + 0x1C): 0 for an entry of kind 3 (player 1,
  pad slot car + 0x18 = 2), 2 for AI entries. The hook (Machine::cpu.onCall at that call) writes 2 for the car whose pad
  slot is 2. 0x80033384 stores it in body + 0x45D (0x80030D64) and sets the car up as an AI car (class tuning,
  automatic gearbox) - the state gt2game's `--ai-player` builds natively. The entry stays the player's: results record
  0x801D5E88, HUD, race-end sequence, prizes. CBM0001 (Clubman Cup Rome Short) with `work\memcards\gt2_save_1car.mcd`:
  the player's Mazda finishes 3rd by itself.
- Autopilot (`tools/gt2run/race_autopilot.h`, `gt2run session ... auto=<from>[:<to>]`): presses through the screens
  between races from guest RAM (race clock 0x80046F64, race-end timer 0x800AF226, replay flag 0x800A951C, event menu
  0x800585C0 / widget 0x8005D284, post-race menu 0x8004A55C / widget 0x8005ADC0: Start Race -> AT, the results waits,
  replay -> pause -> Exit, Continue / Next Session); its presses are written to `autoscript.txt` as a gt2play
  `--script` (the interpreter is deterministic).
- gt2play: `--card <mcd>` (a copy!), repeatable `--prims <field> <out.txt>` (ascending, 8+ fields apart), each also
  writes `<out.txt>.ram.bin` (RAM after field + 6, the comparisons' state); capture runs no longer present frames.
  Windows throttles background processes: raise the priority of long runs (PowerShell `PriorityClass = 'AboveNormal'`).
- Capture cards (under `work\memcards\capture`, never in the repo): `gt2tool career-buy` of a race car into a copy of
  the one-car save plus a scratch script marking the 60 licence records passed (+1) and selecting the car (CRC fixed;
  `gt2tool save-info` checks it) - needed for the championships (GT300 needs licence IB, <= 591 hp).
- Routes (fields at 60 Hz, from a cold boot with the card): event menu of CBM0001 `work\re\ev_route.txt`; the event
  flow with the autopilot's presses `work\re\ev_flow_script.txt` (RESULTS view set up at f15682, BONUS at f16141);
  GT300 championship `work\re\champ_route.txt` (Special Events, lineup 4 via R1 twice + cross, GT 300 Championship,
  GO via R1 twice; the pre-race menu "SESSION 1" opens with row 0 selected, up wraps to Exit, again to Start Race).

## 7. The US Arcade v1.1 disc (SCUS_944.55, EXE SHA-1 231f9dba...) - 2026-09-19

The arcade race overlay (member 0 7360263f...) holds the same pause menu (0x80029D18 input / 0x80029E2C draw) and race-end
display (0x8002B11C with 0x8002AB0C "Finish", 0x8002A38C / 0x8002A5DC badges, 0x8002AC8C rows) modulo relocations (objdump
of both: every differing word is a relocated call, jump or lui / addiu data half, except the licence-prize and 2P / "YOU WIN"
branches of 0x8002B11C, which the arcade race does not take). The arcade race is sub-mode 4 (race block + 0x0A): the default
path with the results table. `RaceOverlayAssets::Load` reads every table through the disc's address profile (identity on
the Simulation disc): ovl0 tables -0x54 (colours / ease 0x8002F594.., badges 0x8002F7A4 / 0x8002F7B0, pause labels
0x8002F58C), EXE fonts / cap table -0x308 (0x80092E28 / 0x80092E34), the race text block of 0x167F bytes at 0x801C6940
(member 0 0x80028C4C; hud_assets.h `kArcadeRaceTextBlockSize`) with the strings at the profile's reference runs ("Finish"
0x801C698B, "Results" 0x801C6992, "Running" 0x801C699D, "Continue" / "Exit" 0x801C6ADB / 0x801C6AE5). Row names = the race
slots of the race block (+ 0xEC + car x 0xD0, arcade 0x801D53A8). Facts: `db\arcade_us11_symbols.yaml` (section "pause menu
and race-end display over the arcade race"). `--race-screen-check` takes the arcade disc (state from the capture's RAM
through the profile). Captures: `gt2play <arcade.bin> --ai-player --script "<r4 script of work\re\arcade_results\r4 up to
18600:start,18700:down>" --prims F ...` (`work\play\arcade_results\cap2`; the pause is the replay's, 18600:start):

| Capture | Timer / state | Contents | Primitives | Differing pixels |
|---|---|---|---|---|
| cap2\end_16300 | 39 | "Finish" | 12 / 12 | 0 |
| cap2\end_16410 | 92 | Finish brightening | 36 / 36 | 0 |
| cap2\end_16440 / 16480 | 109 / 129 | three fading copies | 36 / 36 | 0 |
| cap2\end_16540 | 157 | band darkening, header | 16 / 16 | 0 |
| cap2\end_16570 / 16600 | 172 / 189 | OFFICIAL TIMER badge growing / held | 19 / 19 | 0 |
| cap2\end_16640 / 16680 / 16720 / 16760 | 206 / 225 / 242 / 260 | SEIKO, rows 1..6 appearing | 155, 219, 285, 359 | 0 |
| cap\end_16800 / 17700 | 280 / 315 | six rows, the X wait | 413 / 413 | 0 |
| cap2\end_18640 | pause, Continue, counter >= 14 | Continue / Exit | 78 / 78 | 0 |
| cap2\end_18704 | pause, Exit, counter 1 (flash) | | 78 / 78 | 0 |

gt2game's arcade race (`arcade_mode.cpp`, Panels on the arcade disc) draws them from its own race; the dev aid
`GT2_RACE_OVERLAY_LOG=<file>` (panel.h) writes the frames it draws, which were matched against the captures (docs/research/
arcade_disc.md 17.7).

## 8. CHANGE PARTS interactive and the power / torque graph - 2026-09-19

Both builds (US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; US Arcade v1.1, SCUS_944.55 SHA-1 231f9dba...:
member 0 code at -0x90 from Sim 0x80052100 on, data -0x90, the executable -0xF0). Evidence: our disassembly / Ghidra pseudo-C
(`work/re/change_parts/sim_decomp`, `sim_decomp2` of `work/re/menu_race/ram.bin`), gt2run sessions and gt2play captures of both
originals with a garage car that owns power parts. Code: `src/gt2view/change_parts.*` (ChangePartsView, PartsPreview,
SheetSlotRow), `src/gt2view/power_graph.*` (the EXE widget), `tools/gt2game/arcade_post_race.*` / `arcade_mode.cpp` (the arcade
TIME TRIAL row), `tools/gt2game/settings_screen.*` (the Simulation event menu's "Settings ..."), `tools/gt2game/change_parts_check.*`
(the dev check). Facts: `db/sim_us12_symbols.yaml` (block "CHANGE PARTS interactive ..."), `db/arcade_us11_symbols.yaml`.

- **Page (0x80053558 / 0x800536A4 / 0x80053CA8).** The view 0x8005D1C0 waits 12 fields (0x800572C4), then 0x80053674 opens the page
  band. State 0 the groups (tables 0x8005C364 turbo + NA rows / 0x8005C384 turbo rows / 0x8005C3A4 neither, 0x8005F7C8 / 0x8005F790;
  8 groups with turbo rows, else 7), state 1 the group's parts, state 2 the stage list (EXE list widget 0x8005C3DC, row callback
  0x80052D84, its band 0x8005C410 at (0, 0xD2)); codes and sounds as 0x8005731C (-2 moved 6, -5 entered 1, -6 back 2, -7 refused 0,
  -4 leave 4 with the commit 0x80056FF0, -8 L1 PARTS SETTING 3 with the commit). A part entry (record + 0x10 + p * 0x10) is {name,
  description, stage names (12-byte entries {name, description, s16 part kind}), s16 sheet kind, s8 picture, u8 flags}: flags bit 6
  = a sub-part (no stage list, -7), bit 7 = a power part (graph). The course restrictions (race block + 0x588 bits 1..2 = 1: no
  turbo kit / intercooler, 2: no NA tune-up) refuse the part (-7). A stage row is enabled when the sheet has its row
  (0x8005EE4C), the race block's garage car owns it (0x8005E874 with the stage entry's part kind; stage 0 always), a power limit
  (+ 0x586, "Power Restricted to %dhp") is not below the stage preview's power (0x80057A28), and for tyres the course's surface
  (bit 0 set: the dirt stage 7 disabled, clear: only it). Choosing selects the stage's row into the sheet (0x8005EAC0); the part
  rows then show it.
- **Preview (0x80057654 / 0x800576FC / 0x80057854).** Entering a power part queues the sheet as it is and each stage; one slot per
  field: 0x8005F410 or 0x8005F044 (career::SheetRecord / PreviewRecord), 0x80077214, the figures 0x80075930 and the samples
  0x8007489C; when all are done the graph opens with the slots' maximum last rpm / power / torque (0x80073CFC). The draw shows the
  car's curves at alpha 0x30 and, in state 2, the selected stage's at 0x80 with "%dhp/%drpm" and "%d.%dkgm/%drpm" (small font at
  (0x78, 0x186 / 0x19A); the torque string is a `kind: data` fact of the arcade db).
- **Graph widget (EXE 0x80073CE4 .. 0x800747D0, 0x8007489C, object ovl0 0x8005C42C {span 8, x 40, y 270, w 64, h 130, page 6, font
  0x801C9110, colours power 0x020A50A0, torque 0x02A0500A, axes 0x02AEAEAE}).** Scales (power step 20 / 50 above 300, torque step 2 /
  5 above 400, the rpm axis every 1000 rpm, samples every 250 rpm), a reveal of span fields across the ticks / samples (lerp from
  0x02DCDCDC over 8 steps), the anim cycling from span + 8 to span + 0x44. Only member 0 calls it (no other jal or function pointer in
  the EXE or any member of either disc). The arcade CAR SELECTION's graph is member 2's own widget (arcade_car_page.h ArcGraph, its
  scales are the same rules, its reveal a vertical wipe); the GT-mode menus (member 4) show the figures as text. So the port is one
  widget (power_graph.*) used by CHANGE PARTS on both discs.
- **Arcade.** TIME TRIAL's "Settings ..." (0xFB, 0x8004C4B0 = Sim; enabled when 0 <= race block + 0x584 < the count of the garage
  of + 0x582: 0x8004C0B0) pushes the page and sets 0x801C90B4 = 1: Replay / Save Replay are disabled afterwards. gt2game: the sheet
  = 0x800173E8 of the garage car (the original's 0x8016E584 equals it: configuration and stages), the commit into the race block's
  slot 0 (the next Try Again races the new car) and the garage car (the career). L1 commits and opens PARTS SETTING, its R1 CHANGE
  PARTS again (section 8.1). The arcade race assets' pointer translation (arcade_post_race.cpp
  TranslatePointers) took the part entries' + 0xC words for addresses (the muffler's 0x80060011): RestoreDataWords puts the arcade
  words back.
- **Simulation.** The event menu's "Settings ..." now opens CHANGE PARTS as the original (settings_screen.h), L1 PARTS SETTING, its
  R1 CHANGE PARTS again. The race block's power limit / flags come from SettingsScreen::SetRaceLimits (default 0 / 1, a tarmac race
  without a limit; the event's tail is not passed by career_race.cpp yet).

Verification (card `work/play/change_parts/card_parts.mcd`: `work/re/spec_msettings/card_tuned.mcd` + `gt2tool career-part` on car 0:
mufflers Sports / Semi-racing (Sports fitted), port & polish and turbo stage 1 owned but not fitted, computer and engine balance fitted):
- gt2verify (Sim race dump `work/re/race_demo/ram.bin`): PartsPrev 0x800576FC 297 cases (real garage sheets, every stage of random
  kinds, then 0x80057854), PgCurves 0x8007489C 3000, PgScale 0x80073DA4 (+ tick / close / reset) 3000 - 0 mismatches.
- Arcade: the original's Time Trial with the Home Garage car (gt2play --ai-player, `work/play/change_parts/cap2/script.txt`: the
  route of arcade_disc.md 17.11, then 6 downs to Muffler & Air Cleaner, enter, enter, down to Semi-racing, cross, triangle,
  triangle): 20 captures (states 0 / 1 / 2 with the graph's reveal and the previews, the stage change, back in TIME TRIAL) against
  gt2game's own session (GT2_ARCADE_RACE_SETUP of the route's race `work/re/change_parts/rc1/race.bin.setup`, the card's career by the
  first-boot rule, the same presses 8700 fields earlier; GT2_ARCADE_SESSION_COMPARE; the captures' VRAM is the one of field + 6):
  0 differing pixels each (TIME TRIAL: outside the 3D car); the career block after the session = the original's (all 0x7C9C bytes,
  GT2_ARCADE_CAREER_DUMP vs `c_12560.txt.ram.bin`). `gt2game <Arcade disc> --change-parts-check`: 15 captures 0, the commit (race
  slot configuration, race record 0x801DE8BA, garage block, sheet) = the dump after the page (`c_12600`).
- Simulation: the original's CBM0001 event menu route (`work/play/change_parts/simcap/script.txt`, the same card) with the same page
  presses: `gt2game <Sim disc> --change-parts-check simcap/c_5460.txt.ram.bin 5050 "<script>" <out> <captures> after=c_5760.txt.ram.bin`:
  10 captures 0 differing pixels, the commit equal. gt2game's own GT-mode flow (`--menu --menu-page 835`, GO on Rome Short, the
  event menu's Settings ...) shows the page and commits (`work/play/change_parts/simnat`).
- Not compared: the header's entry animation of the view transition in the Simulation event menu (settings_screen.h draws it
  settled; the arcade session and the check run the manager's transitions, section 8.1).

### 8.1 PARTS SETTING in the arcade Time Trial session (L1 / R1) - 2026-09-19

Addresses Simulation v1.2 unless marked (Arcade member 0 code / data -0x90 here, the view manager -0xE0). Evidence: our objdump
of both members 0 (`work/ovl/{sim_us12,arcade_us11}/ovl0.bin`), gt2play captures of the original arcade
(`work/play/change_parts/cap3`, `cap3b` for the draw lists of the slides). Facts: `db/arcade_us11_symbols.yaml`
(arcade_parts_setting_view_update); no new profile facts (the page's data is inside the member 0 reference run -0x90; `gt2tool
gen-profile` output unchanged).

- **Views.** CHANGE PARTS' update 0x8005731C on -8 (L1): 0x80048374(M, 0x8005D1E4) - the manager's current top is REPLACED
  (M + 0x1CC = the old top, stack[M + 0x210] = the new view; 0x800483A4 is the push, 0x800483D8 the pop), the commit 0x80056FF0,
  the page close 0x80053684, sound 3, returns 5. PARTS SETTING's update 0x800574C0 on -8 (R1): 0x80048374(M, 0x8005D1C0),
  0x80056FF0, 0x80055FE0, sound 3, returns 6; on -4 (triangle / square in the groups): 0x80056FF0, 0x80055FE0, sound 4, returns 2
  (the pop: back to TIME TRIAL, which re-runs 0x8004C034(1)). The manager 0x800474F4 switches on the result through the table
  0x8005A77C: 1 forward (new top's init(0), M + 0x211 = 16, M + 0x212 = 0), 2 back (0x800483D8, init(1), direction 1), 3 / 4 end,
  5 the new top's init(0) with direction 3, 6 with direction 2. PARTS SETTING's init 0x8005747C (view + 0x14 = 12, 0x80055E90(P,
  0, 100)) and CHANGE PARTS' 0x800572C4 (12, 0x80053558(page, 0, 100)) reset their pages: re-entering starts at group 0. The
  sheet 0x8016E894 stays (not reloaded): PARTS SETTING edits the sheet CHANGE PARTS selected stages into.
- **Slides (0x800477C4).** Offsets of the view environments (x, y) with c = M + 0x211 (16 .. 1), divisions truncating toward
  zero: direction 2 - the entering view (200 c / 16, 0), the leaving one (100 (c - 16) / 16, 0); direction 3 - the entering
  (-200 c / 16, 0), the leaving (100 (16 - c) / 16, 0) (directions 0 / 1: the vertical ones of section 5.2). 0x8008034C makes
  each view's drawing area the frame rectangle moved by its offset and clamped to the frame (E3 / E4 / E5 of the captured draw
  lists, e.g. `cap3b/c_12526.txt`: the leaving CHANGE PARTS E3 x 25 / E5 25, the entering PARTS SETTING E4 x 0xC9 / E5 -150):
  the leaving view is cut on the left at its offset, the entering one on the right at 351 + its offset (the header's spread
  letters of the entering title are cut there). gt2view `SessionViewStack::Replace` / `Frame` (race_session_screens.*): sprites
  and tiles are cut to the area, polygons get it as their clip rectangle.
- **Port.** `gt2view/change_parts.h PartsSettingView` (view 0x8005D1E4 over `MachineSettingsPage`, whose draw is now also
  `MachineSettingsPage::Draw(MenuOt&)` for a view's ordering table), `tools/gt2game/arcade_post_race.cpp RunArcadeSessionViews`:
  CHANGE PARTS -8 -> commit, Replace(PARTS SETTING, direction 3); PARTS SETTING -8 -> commit, Replace(CHANGE PARTS, direction 2);
  -4 of either -> commit, pop, TIME TRIAL Setup(1). The commit (arcade_mode.cpp `parts.commit`, career::CommitSettings) writes the
  race block's slot 0 configuration (the next Try Again's car) and the garage car (the career). `--change-parts-check` runs the
  pages in the same view stack (L1 / R1 followed, a commit per exit).
- **Verified.** The original's route (`cap3/script.txt`: section 8's arcade route with `12520:l1` instead of the last triangle,
  then `12600:cross,12650:cross,12700:right,12720:right,12740:right,12780:cross` (Spring Rate front +3 steps, written back),
  `12830:triangle,12870:down,12920:r1,13000:l1,13080:triangle`), 34 captures c_12500 .. c_13200 (both slides, PARTS SETTING's
  opening, the popup, the steps, the write-back, the group change, back in TIME TRIAL):
  - gt2game's own session (GT2_ARCADE_RACE_SETUP=`work/re/change_parts/rc1/race.bin.setup`, `--ai-player --fast --arcade-square
    --no-sound --card <copy of card_parts.mcd>`, the presses 8700 fields earlier with q / w for L1 / R1, then Exit; GT2_ARCADE_SESSION_COMPARE
    field = capture + 6 - 8700 +- 2; `work/play/change_parts/nat5`): all 34 captures 0 differing pixels outside the 3D car's
    rectangle, the slides at exactly capture + 6 - 8700. The career block after the session (GT2_ARCADE_CAREER_DUMP) = the
    original's RAM `cap3/c_13200.txt.ram.bin` at 0x801C9340 (Arcade): all 0x7C9C bytes equal; the race block equal except + 0x1B8
    (entry 1 + 0x8C, the ghost flag: 1 in the original, 0 in our dump - the ghost session sets it in its race copy, arcade_mode.cpp;
    the same difference as section 8's run, unrelated to the pages); slot 0's configuration (the next race's car) equal.
  - `gt2game <Arcade disc> --change-parts-check work/play/change_parts/cap2/c_11990.txt.ram.bin 11900 "<cap3 script>"
    <out> <cap3 captures c_12500 .. c_13060>@<field> after=work/play/change_parts/cap3/c_13200.txt.ram.bin`: 29 captures 0
    differing pixels; after the four exits the race slot configuration, race record, garage block and sheet = the dump. The
    section 8 runs (Arcade cap2 15 captures, Simulation simcap 10) stay 0 with the commit equal.
- Not compared: the pages' sounds (requested with the codes of 0x8005731C / 0x800574C0 and the slider's 8, no SPU log compared).
