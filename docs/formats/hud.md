# In-race HUD: graphics, strings, routines

Status 2026-09-19 (second pass: START / countdown, warnings, tyre panel, medal lines). Everything below is derived from the bytes of US Simulation v1.2 (SCUS_944.88, EXE SHA-1
3030aa271c0a4022fc69ce09d76a6bc75e69a32a; race overlay = GT2.OVL member 0) and from our disassembly / Ghidra
export of the race overlay, checked against the original's GP0 traffic (`gt2play --prims`). Parsers:
`src/gt2formats/hud_assets.*`; renderer: `src/gt2view/hud.*`. No external reference was used.

## 1. Race font image: `font/racefont.dat`

A raw 4-bit image of 128 words x 256 rows (256 x 256 texels), no header, uploaded to VRAM (384, 0) unchanged (the
captured VRAM equals the file). Row 0 holds eight 16-entry CLUTs; the HUD text uses CLUT 2 = GPU word 0x001A
(416, 0): entry 0 transparent, 1 = 0x8000 (outline), 2..15 a grey ramp. Glyphs are 4-bit sprites (GP0 0x64) on
tpage 6 / 7.

## 2. The text engine (ported: `gt2::HudFont`)

0x8007DD3C draws one character, 0x8007DC78 is the advance to the next. A font descriptor of the executable is
{u32 glyph table, u32 kerning table, u8 cell, u8 gap}: 0x80093124 (the HUD font: 0x80095AC8 / 0x80096578, cell 8,
gap 1), 0x80093130 (large digits: 0x8009825C / 0x80098D10, cell 12, gap 2), 0x8009313C (the START / countdown font
of 0x8002A19C; `HudFont::kStartFont`). The glyph table has two words per character code:

- w0: bits 0-5 advance, 6-13 sprite index (0 = none: space), 14-16 accent sprite 1..7, 17-22 centring offset (added
  when the code carries flag 0x100), 23-30 left kerning class (bit 31 set: bits 23-28 = a fixed kerning value);
- w1: bits 0-5 x offset, 6-11 height above the baseline, 12-17 / 18-22 accent x / y, 23-30 right kerning class
  (bit 31 as for w0);
- at table + 0x7FC the accent sprites, at + 0x818 the sprites: u8 u, u8 v, bits 16-20 width / 2, 21-26 height,
  27-28 CLUT offset, 29-31 texture page offset (added to the context's page 6 and CLUT 0x18).
- kerning table: u32 {u8 row stride, u8 pad, u8 bits per entry}, then rows; kern(a, b) = the entry of row
  class(a), column class(b) (read with lwl / lwr: unaligned). advance(a, b) = gap + (w0(a) & 0x3F) - kern.

Layout helpers: 0x8006AC90 text at a baseline (returns the width), 0x8006AD3C width, 0x8006AE28 right-aligned,
0x8006ADB4 centred; times 0x8006B360 / 0x8006B218 / 0x8006B3F4 (right) / 0x8006B49C (centred): fixed cells
(`advance`, narrow cell next to ':' and '.', '.' shifted by `dotShift`, glyphs centred in the cell, pen starting
cell / 2 left); numbers 0x8006B044 / 0x8006AF40 / 0x8006B184 (digits in a cell of cell + extra, other characters
proportional). Formats: 0x80068734 time "M:SS.mmm" / "H:MM:SS.d" / "--:--:---" (-1); 0x80068CA0 speed "%d.%d" of
(v / 100, v % 100) - e.g. 5607 -> "56.7" (sic); 0x80068B04 split difference "+S.mmm" / "-S.mmm" / 0xB1 "0.000".
This replaces the kerning rule inferred earlier from 32 captured pairs (hud.md of 2026-09-18).

## 3. Gauge sheet, dial faces, course map

`arcade/game_status_files(.gz)`: `u32 count = 11`, `u32 offset[11]`; member 0 = the sheet (4-bit, 64 x 234 words,
uploaded to (512, 0); rows 224..233 are raw CLUT words: (512, 228) dial face, (528, 228) speed digits, (560, 228)
unit, (512, 229) strip, (528..560, 229 / 230) badges ...); members 1..10 = the dial faces for 6, 7, 8, 9, 10, 11,
12, 14, 16, 18 thousand rpm (4-bit 20 x 80 words + CLUT). The race uploads the car's face over slot 0 of the sheet
(uv (0, 0) 80 x 80, tpage 8) with its CLUT at (512, 228) - captured licence test: member 2 (7000) there.

`crsmap/<course>.tim(.gz)`: the course map, 4-bit 96 x 96 texels + CLUT, uploaded to (576, 144) / CLUT (368, 508)
whatever the file's destinations (Seattle and TC_lisence: the captured VRAM equals the file).

Tables of the race overlay (`gt2::HudTables`; descriptor = {u8 u, u8 v, u16 clut, u16 w, u16 h, u16 tpage, u16}):
0x8002F630 face slots, 0x8002F678 speed digits (uv (240, 16 d)), 0x8002F6F0 km/h / 0x8002F6FC mph (the US game
draws mph), 0x8002F708 strip "0123456789R/", 0x8002F798 position badges, 0x8002F7E0 / 0x8002F7EC turbo face /
plate, 0x8002F868 dial limits, 0x8002F874 gear -> strip glyph (0 -> 'R'), 0x8002F880 / 0x8002F898 tachometer
needle, 0x8002F8B0 / 0x8002F8C8 turbo needle ({colour 0, colour 1, radius 0, radius 1, half width 0, half width 1}
in 1/16 pixels), 0x8002F8E0 caption bar {x, y, w, h 5, 0x82, 0x50}.

## 4. Strings: `.text/data-race.txd`

NUL-terminated strings, one block per language, the US block first ("Lap", "Total Time", "Lap Time", "Record",
"Best Lap", "mph", "START", "Finish", "Time Invalid !", "Crash !", "Out of Course", "Sector1".. ); the race copies
the first block to 0x801C6C50 and the shell's caption tokens point into that copy.

## 5. The HUD routines (ported: `gt2view::Hud`)

0x8002E63C calls, per view and frame, in this order (every routine inserts its packets at the head of one ordering
table slot, so the GPU draws them in REVERSE; the port builds the packets in the same order and draws them
reversed with the draw-mode state):

| Routine | At (x, y) | What |
|---|---|---|
| 0x8002A19C | (160, 110) | countdown digits and "START" / "REPLAY" from the start timer (section 6) |
| 0x8002B170 | - | the race-end display ("Finish", licence prize / FAIL; `gt2view/race_overlay_screens.*`, race_screens.md) |
| 0x8002E204 | (160, 182) | the warning line of car + 0x790 (section 8) |
| 0x8002D664 | (160, 96) | message block: split difference at y - 16 (small font, green / red / grey by the comparison), caption (car + 0xA98) at y - 26, its time (large font, 0x1E5A78) at y, "Time Invalid !" at y + 16, "Crash !" / "Out of Course" at y + 32; fading out (additive, colour towards black) over the last 30 frames of each timer (car + 0xA8E / 0xA90 / 0xA92) |
| replay (0x800A951C != 0) | | view + 0x107 = 0: only the lap block; 2: + 0x8002DA3C / 0x8002DBB0 (not ported) |
| 0x8002D308 | (308, 16) | "Record" + course record (time, top speed + unit) and "Best Lap" + the race's best lap, right-aligned; mode 0 (GT race) draws nothing, mode 3 the licence record + the three medal lines 0x8002D12C (section 7) |
| 0x8002C1CC | (276, 180) | needle (angle = rpm * 2412 / (1000 ceil(limit / 1000)) + 1274 of 4096), gear glyph (dim 0x1C2644 while the clutch is not engaged) over a subtractive 14 x 16 box, unit, 3-digit speed (car + 0x6DA / 100) |
| 0x8002C00C | (276, 180) | dial face (additive) and the ring: 4 ceil(limit / 1000) flat quads between radii 641 / 516 (1/16 px), grey 0x505050 (mode 0) below the red line car + 0x3C2 / 250, red 0x0000F4 from there (0x8002BD84 precomputes the spokes) |
| 0x8002C584 | (224, 204) | turbo gauge, only when car + 0x154 != 0 |
| 0x8002DE8C | (296, 124) | tyre panel when 0x80046F48 (tyre wear) != 0 or the shell's control class (0x800418E8) is 2 (section 9) |
| 0x80029064 | (16, 144) | course map (not in licence tests): the 96 x 96 sprite, a black 4 x 4 and a 2 x 2 dot per car at map + 48 + (metres * 163 >> 12) (x, z from car + 0x832 / 0x83A); the player red, the others green |
| 0x8002C76C | (12, 16) | lap block: badge (modes 0 / 2 / 4 / 0xB), "Total Time" + race time, "Lap Time" + the last two recorded laps + the running lap (replay: the lap in brackets from the result record), "Lap" + lap counter strip; other layouts for modes 1 / 3 / 6 / 7..10 |

Rotations use the executable's sine table 0x80093150 (0x8002BCF0: x = ((r cos - w sin) >> 12 + 4) >> 4,
y = ((w cos + r sin) >> 12 - 8) >> 4) and the pivot is added to the packed 16-bit xy word (a negative x borrows
one from y).

Verification (`gt2play --prims`: builds our HUD from the original's inputs of the frame read from guest RAM and
lists it as "# hud-ours" lines next to the capture): licence test B-1 at field 5300 - all 68 of our primitives
equal captured ones, same order, same draw modes (lap block, record block, ring with the red line, face, needle,
gear, unit, speed); with the medal lines (2026-09-19) all 98 of 98 at fields 5200 / 5500. Attract replay at 3600: all
57 of ours (lap block with the bracketed lap, "Replay" + car name) are in the capture.

Checks of 2026-09-19 (`work/play/hudfx/`, comparison: our "# hud-ours" list must occur in the captured frame's draw
list as one contiguous run, E1 commands included): start timer 350..94 (fields 4448..4720 of the B-1 route, the digits
"2" / "1", START, both fades) - 20 of 20 frames; Wrong Way (the car turned round: `--script ...,4300:cross:2000,
4600:left:130`, fields 4660..4860) - 11 of 11; tyre panel (`gt2play --poke 4450 80046F48 0007A120`, wheel damage bytes
poked to 0xC0 / 0x40 / 0xFF) - 11 of 11 + 2. `gt2play --prims` fills the new inputs from guest RAM (start timer, car +
0x790, the medal pairs of the settings block 0x801C98A0, the message block car + 0xA8C.., the tyre panel flag = the
original called 0x80043108 from 0x8002DE8C in the captured frames).

## 6. Countdown and START / REPLAY (0x8002A19C, ported: `Hud::StartDisplay`)

Input: the start timer 0x800AF224 (s16): 0x8002A0C8 sets it to hold + 240 at the race start, 0x8002A0D4 subtracts the
fields per frame (0x801D5864) every frame down to 0 (and plays the signal sounds when it passes 420 / 360 / 300 / 240);
0x8002A398 sets -1 (nothing drawn). Text centred at (160, 110) (0x8006ADB4) with the third font, the context's draw
mode with blend 1 (tpage | 0x20):

| Timer | Text | Colour |
|---|---|---|
| 241..419 | digit '1' + (timer - 240) / 60 ("3" only at 360..419) | 0x6F6F6F |
| 121..240 | START, or REPLAY when 0x800A951C != 0 | 0x0A376E |
| 105..120 | same | semi-transparent, t = 120 - timer: (110, 55, 10) + t (34, 89, 134) / 16 (R, G, B) |
| 89..104 | same | semi-transparent grey 144 - 9 (t - 16) |
| other | nothing | |

The tokens are the overlay's own `lui` / `addiu` pair at 0x8002A1B8 / 0x8002A1C0 (+ the replay offset at 0x8002A1D0),
decoded at load - no string or address table in our code.

## 7. Licence medal lines (0x8002D12C, ported: `Hud::MedalLine`)

Mode 3 only, from 0x8002D308 after the licence record: medal k = 1..3 (gold 0x64C0E0, silver 0xE0C070, bronze 0x1050A0)
at y + 58 / 67 / 76: the time 0x8003D7B8(settings block, k) right-aligned at x + 3 (caption colour, 7 / 5 cells), then a
TILE 4 x 4 of the medal colour at (x - 54, y - 6) and a black TILE 6 x 6 at (x - 55, y - 7) inserted after it (drawn
first). gt2game fills the times from `LicenseTest::MedalTime`.

## 8. Warnings (0x8002E204, ported: `Hud::Warning`)

The string of car + 0x790 (`CarBody::messageCode`, requested by the shell's 0x80030308) through the jump table 0x8002F320
(codes 1..12; code 0 and others: the empty string 0x8002F318, nothing drawn); the large font centred at (160, 182),
colour 0x020C1879 (semi-transparent) with the context's blend bits cleared (mode 0: B/2 + F/2). Codes (US text block):
1 wrong way, 2 / 3 the two "WARNING" lines, 4 / 5 / 6 the pit-exit prompts, 7..11 the pit lane messages, 12 exit. The
tokens are decoded from the case code of the overlay (`lui v0` + `addiu s3, v0`), a build without the table draws
none.

## 9. Tyre panel (0x8002DE8C, ported: `Hud::TyrePanel`)

Drawn when the tyre-wear word 0x80046F48 != 0 or the shell's control class 0x800418E8 == 2 (gt2game: `SimConstants
wear.wearLimit` / `shellControlClass`; its default GT race has class 2, so the panel shows). Two rows centred on (296,
116) and (296, 132) (front, rear); per wheel two SPRT 24 x 13 (0x80081478, each followed by its E1 tpage 8): the damage
tint 0x8006B548(0x8002F904 = 0, 0x8002F908 = 0x80, wheel + 0x22, 256) (red 0..0x80) with sprite 0x8002F828 (left) /
0x8002F834 (right), then the wear colour 0x80043108(wheel + 0x3F, signed) with sprite 0x8002F81C / 0x8002F840:
v >= 64 (126, 127 - v, 0), 32..63 (126, 2 (95 - v), 0), 0..31 (4 v, 126, 0), -63..-1 (0, 126 - 2|v|, 2|v|), below
black. All four sprites of a row share the centre; the sheet's cells place the left / right tyre.

## 10. Native placement

The 320 x 240 layout is scaled uniformly by the window height; the lap block, "Replay" and the course map keep their
offset from the window's LEFT edge, the record block and the gauges theirs from the RIGHT edge, the countdown and
the message block stay centred. gt2game options: `--hud-mode N` (the shell mode whose layout is drawn; default 2 =
the arcade race layout with Record / Best Lap - mode 0, the GT-mode race gt2game simulates, has no record block),
`--kmh` (km/h converted from the mph readout: our option), `--replay-hud [--replay-view 1]`.

## 11. The dispatch per view, the 2 player views, the course map files (2026-09-19)

- **0x800293D4** (per view; Arcade 0x80029380): the pause menu when view + 0x2E9; game mode != 0: **0x8002E818** (in a replay only:
  the caption "Replay" at (16, 210) and the followed car's entry name at (16, 222), colour 0x606060) with the camera's car, then
  0x8002E63C(car 0, camera 1) only when camera 1 follows car 0; game mode 0 (the Arcade 2 player Battle, the Sim GT-mode race):
  split (view + 0x2EA) -> 0x8002E908 per car with its camera, else 0x8002E63C(the car camera 1 follows, camera 1) - no caption.
  `Hud::Build` draws the caption for game modes other than 0 only; `Hud::Build2PFull` is the 2P full view (both dial faces as
  0x8002E390 uploaded them, the face slot = car + 0x880).
- In a replay 0x8002E63C draws by **camera + 0x107** (0 only the lap block, 1 the full HUD, 2 + 0x8002DA3C / 0x8002DBB0, 3+
  nothing); the split HUD's halves use their own cameras' (the 2P replay capture: top 1, bottom 0).
- **0x8002F864** (u8; Arcade 0x8002F810): +1 in the HUD tick 0x8002E550 of every race frame while the race clock 0x80046F64 runs,
  0 at the HUD setup 0x8002E390; the running times add (n & 15) * 1000 / 900 ms. The split race models it (`SplitHudCounter`,
  = the original's in 44 snapshots).
- **Course map files**: 34 of the 120 crsmap/*.tim of the Sim disc (33 of 119 on the Arcade disc, e.g. tahiti_t_2p, laguna2,
  the test_* licence maps) have a wrong length word in the image block (0x240C for 12 + 96 * 96 / 2). The EXE's TIM upload
  0x8007BCA0 (-> 0x8007BC1C, 0x8007BBD4) only uses the length to step from the CLUT block to the image block and loads w x h
  words, so `LoadCourseMap` ignores the last block's length (it still requires the data inside the file).
- Checks: `gt2play <disc> --hud2p-check <snapshot ram.bin> <out.txt> <capture.txt>` (our 2P HUD from the snapshot's RAM: primitive
  list in sequence, then the run rasterised with the capture's VRAM and ours) - 0 differing pixels in 54 captures of the 2P race,
  its end and the 2P replay (split and full view); docs/research/arcade_disc.md 19.10.

## Open

Replay view 2 panels (0x8002DA3C / 0x8002DBB0); "Finish" of gt2game without the race flow (our placement); the tyre
panel's colour branches below 64 and the warning codes other than 1 are checked by disassembly only (no capture); the
counter 0x8002F864 the original adds to running times ((n & 15) * 1000 / 900 ms) is replaced by our frame counter in the
single-player race_view (the 2 player race models it, section 11).
