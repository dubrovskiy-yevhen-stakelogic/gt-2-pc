# Replays, the start grid and the AI catch-up

Status 2026-09-19. US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; race overlay = GT2.OVL
member 0). Evidence: our disassembly / Ghidra export of `work\re\race_load`, `work\re\race_demo`, `work\re\license_race`,
`work\re\menu_race`; frame captures of the original (`gt2verify --race-capture`, below); `arcade/demofile_us.gmr` against
the attract race's RAM. Code: `src/gt2formats/replay.*` (stream, frames, file), `src/game/sim/race_sim.*` (grid placement,
race-state init, catch-up, the pad source hook), `tools/gt2game/race_common.*` (ReplayDriver, LoadReplayRace,
BuildReplayFile, FramesCompare, ReplayCheck), `tools/gt2game/race_view.cpp` (recording, replay view), checks
`tools/gt2verify/verify_grid.cpp`, `verify_replay.cpp`. No external reference was used.

## 1. A replay re-runs the race

GT2 does not record positions: a replay runs the whole race again from its start with the recorded pad input of the
pad-driven cars; the AI cars are simulated again (physics and AI are deterministic, the arcade modes' catch-up
included). The attract race is such a replay: `arcade/demofile_us.gmr` holds the race block, the six cars and player 1's
input stream; the race overlay runs with the demo / replay flag `0x800A951C` set (the camera takes the replay cameras
0x800109FC, the shell records no results).

## 2. Input stream object (player 1: 0x801D5F84, player 2: 0x801DA49C, capacity 0x4400 from 0x80012CD4)

| Offset | Field |
|---|---|
| +0x00 | s32 frames recorded |
| +0x04 | s32 frames left to play (playback) |
| +0x08 | s32 repeat count of the current frame (-1 = none yet) |
| +0x0C | s16 ended / full |
| +0x0E | u16 read / write position in the data |
| +0x10 | u16 bytes used |
| +0x12 | u16 capacity |
| +0x14 | u8 header of the current run |
| +0x15..+0x18 | the current frame: flags, buttons, steering, throttle \| brake << 4 |
| +0x19 | the coded runs |

- 0x800163B8(stream, playback, capacity): record = memset(stream, 0, 0x1C); playback = current frame cleared, ended 0,
  position 0, frames left = frames. Both: capacity, repeat = -1.
- 0x800166CC (record): frames + 1; the frame equal to the current one extends the run; otherwise the run is flushed
  (0x80016598) and the frame becomes current with the change mask as its header (0x78 for the first frame).
- 0x80016598 (flush): header = (mask & 0xF8) | (count & 7) | 0x80 when count > 7, the changed bytes (mask bits 3..6 =
  flags, buttons, steering, pedals), then count >> 3 in 1 byte (< 0x80), 2 (0x80 | high), 3 (0xC0 | bits 16..21) or 4
  (0xE0 | bits 24..28); position = used = the end; full (ended) when capacity - 0x11 <= position.
- 0x800167D0(stream, 0): ended; flushes the pending run once.
- 0x80016428 (read): frames left - 1; when that reaches 0 the stream ends and gives no frame (a stream of N frames drives
  N - 1 steps); a used-up run reads the next header, changed bytes and count; the frame = the current bytes.

## 3. Frames and the pad record (0x80013C90, from 0x80013EF0 <- 0x8003C250 in the physics core, pad slot 2)

Record side (demo flag 0), from the pad object 0x800A9528: flags = bit 0 (+0x9C bit 0, analogue steering), bit 1 (+0x9C
bit 2, analogue throttle), bit 2 (+0x9C bit 3, analogue brake), bit 3 (logical button 0x400); buttons = +0x8C & 0xFF
(1 left, 2 right, 4 throttle, 8 brake, 0x10 handbrake, 0x20 reverse, 0x40 shift up, 0x80 shift down); steering axis
+0x9E; throttle +0xA2 >> 4; brake +0xA4 >> 4. Recorded while 0x800A9522 (fields since the finish) < 300 (game mode 3:
60), then the stream ends. The car is driven from the frame in both directions: steering (0x80 - axis) * 0x20 or
(buttons & 1) * 2 - (buttons & 2 ? 2 : 0); throttle / brake = table 0x8002F4D4[value] (u16[16]) or the button bit;
handbrake (record +10) = 0x10, reverse (+9) = 0x20, shift (+8) = (0x40) - (0x80). Playback reads the frame (zero after
the end) and sets 0x800A8D68 = 1 when the stream has ended.

## 4. The replay file (.gmr: a memory-card save, "SC" header)

Offsets established by comparing `arcade/demofile_us.gmr` (57344 bytes) with the attract race's RAM
(`work\re\race_demo`): file 0x1580..0x1B00 = RAM 0x801D585C.. (file + 0x801D42DC), file 0x1CC0.. = RAM 0x801D5E7C..
(file + 0x801D41BC; the region 0x801D5DDC..0x801D5E7B is not in the file). What the port reads / writes:

| File | RAM | Content |
|---|---|---|
| 0x1580 | 0x801D585C | race block (0x5C): +1 / +4 / +9 mode flags, +8 frame-rate mode, +0xA game mode, +0xB / +0xC licence / test, +0xD countdown (0x801D5869), +0xF laps, +0x10 event name ("MSC0002"; a licence: the test "LJB00"), +0x20 course display name, +0x40 course file id (0xA2D762AE = seattle), +0x44 sponsor category ("General02"), +0x54 sponsor seed (0x14D57), +0x58 dirt level, +0x5A car count |
| 0x15DC | 0x801D58B8 | 6 car slots x 0xD0: u32 car id, u8 paint code (+4, a .carinfoa paint id), CarConfig (+8, 0x84 bytes), entry (+0x8C: ?, grid slot, kind 3 player 1 / 1 AI, transmission), name (+0x90) |
| 0x1DC8 | 0x801D5F84 | player 1's stream object as it was after the recording (ended, position 0); 0x1BEB frames, 0x8A3 bytes in the attract file |

The race settings block 0x801C98A0 is not in the file: the title overlay copies the event row of the race block's event
name from `carparam/usa_gtmode_race.dat` (member 1 0x800109C0: 0x8007830C(*0x80092E70, race block + 0x10), record + 0x44
.. + 0x84); the port does the same (`LoadReplayRace`, licence replays: the licence test row).

(2026-09-19, later) These offsets are replay 0 of a replay FILE: the .gmr files are the memory-card replay file format of
section 9 (a directory of up to 32 replays in 128-byte sectors; entry 0 stored from sector 0 lands at 0x1580). Saved
replays (`gt2game --replay-out`) are now whole replay files (`WriteReplayFile`: the EXE's save header, a directory, CRCs)
that the original's loaders accept (section 9.4).

## 5. The start grid (0x80012CD4 up to 0x80033384)

Chunk hint = 0x80028288(.tro + 0x58 = grid slot 0), car + 0x81C = identity moved to the slot's position (.tro + 0x58 +
slot * 12, slot = entry byte 0x801D5945; 2 in game mode 6 without the course flag 0x20), turned by Ry(.tro + 0x54), moved
along its own z by the body model's nose (0x80017E74: LOD 0 bbox min z << (scale - 16) << 4 - the grid marks are where
the cars' fronts are); chunk = 0x80028394(hint, position); x = + 0x830 >> 4, y = -(+ 0x838) >> 4, heading = the sine
table at the start angle. The port used the slot position itself and the nearest chunk's direction: the B-1 car stood
1.897 m (the Vitz's nose) further along, the attract grid had the player on pole instead of slot 5. Checks: GridArgs,
GridDisc (below). After the cars, 0x8003C200 initialises the contact tables (0x8003FE8C) and the race order (0x80042680,
0x80042568).

## 6. The AI catch-up of the arcade modes (0x8003EBF0 in modes 2 / 4 / 0xC)

0x80041E4C derives 0x80046F6C..0x80046F84 from the settings block (+0x14, +0x17..+0x1C; `race_sim.h CatchUpTuning`);
after the race order every AI car's time scale (body + 0x766) follows its gap to car 0 (0x801C98B6 = 0) or to its
neighbour in the race order: 0x80042230 -> 0x800423BC (laps / distance, wrapped), 0x800420AC (speed-up ramp, not
off-road) / 0x80042174 (slow-down ramp), * F84 >> 12. Mode 0 calls 0x80042038 instead (not ported).

## 7. Verification

- `gt2verify --race-capture <disc> <to> <out.bin> [script]` records the original from boot, one record per physics tick
  (car array, shell counters, player 1's logical pad and stream); `gt2game <disc> [--license B-1 | --replay demo]
  --frames-compare <out.bin>` runs the same race natively (licence: the captured logical pad recorded and driving the
  car like 0x80013C90; attract: the file's stream played back) and compares every frame: the car records' simulated part
  (body up to + 0x798 and + 0xA60..0xA78; curve pointers relocated) and the stream object byte for byte.
  - Licence B-1 with the gt2play route script (X held from field 4300): 529 frames (fields 4445..5500), 0 differ.
  - Attract race from `demofile_us.gmr`: 3195 frames (fields 1955..9000, six cars), 0 differ (899 differed before the
    grid slots, the race-state init and the catch-up were ported).
  - Since 2026-09-19 the capture also writes `<out.bin>.setup` (`race_capture.h RaceCaptureSetup`: the race block 0x801D585C
    and the settings block 0x801C98A0 at the first race setup), and the capture runs on any disc with an address profile
    (`gt2formats/exe_profile.h`). Without `--replay` / `--license`, `--frames-compare` builds the capture's own race from
    it (entries' configurations, grid slots, kinds, laps, countdown, settings). US Arcade v1.1 arcade race (event A0A,
    Tahiti Road, the script of `arcade_disc.md` section 1, to field 17000): 6184 frames (fields 4614..16999), 0 differ.
- `gt2game <disc> [--cars 6 | --license B-1] --replay-check <s> [--replay-out f.gmr]`: a scripted race recorded, replayed
  from memory and from the saved file: the final states byte-identical (seattle 6 cars 1799 steps, B-1 499 steps).
- gt2verify rows: GridArgs (600 random cases per dump), GridDisc (the dump's cars from the disc), CamMirror (400).

## 8. Open questions

- (Answered 2026-09-19, section 9) The file's other sections: 0..0x200 the save header, 0x200..0x1580 the directory,
  0x1B0C the parameter record, 0x1CCC the results record; player 2's stream follows player 1's in game mode 0; the
  original checks the directory (0x800691DC) and the entry's CRC (0x800692DC) when it loads a replay.
- Entry byte +0 (0x801D5944, 1 in every slot); car slot + 5..+7.
- (Answered, arcade_disc.md 19.2 / 19.8) Mode 0's 0x80042038 and 0x8003B73C / 0x8003B69C; the latter two also run at a replay's
  race load (section 9.8).

## 9. The replay file on the memory card and the demo files; the title's Replay Theater (2026-09-19)

Evidence: our disassembly / Ghidra pseudo-C of the EXE's replay-file class and card manager (work/re/theater: RAM at the
theater's screens, decomp of 325 functions), a replay the original saved in our interpreter (licence B-1 route + "Save
Replay" of the licence menu: `work/play/theater/card_b1_replay.mcd`, every CRC matching), the four demo files. Code:
`src/gt2formats/replay_card.*` (format), `src/game/shell/title_replay.*` + `title_screens.*` (screens),
`tools/gt2game/title_mode.cpp` (flow, `WriteReplayOut`), rows `tools/gt2verify/verify_title_replay.cpp`.

### 9.1 File "BASCUS-94455REPLAY" (EXE 0x80091AB8; the demo files arcade/demofile*.gmr have the same format)

| Offset | Content |
|---|---|
| 0x0000 | save header: "SC", +2 icon flags 0x13, +3 blocks, +4 Shift-JIS title (EXE 0x80091ACC via 0x8007D370), +0x60 CLUT 0x80091AEC, +0x80 3 icons 0x80091B0C (0x8007D32C + 0x8006911C) |
| 0x0200 | s8 replay count (<= 32); +0x202 s16 data sectors = blocks * 64 - 43 |
| 0x0204 | sector table: s16 count (= +0x202), s16 next[960]: 0x3C0 free, -1 = last sector of a chain (0x80068E2C init) |
| 0x0988 | 32 entries x 0x5C: +0 title[32] ("No Name" 0x8008FB14 until the name entry), +0x20 slot 0's car name[32] (0x800690B8: cut to 31 with +0x3F = 0x3F; 0x80069048 then prints ".." after 31 characters), +0x40 race block + 9, +0x41 game mode, +0x42 ghost (1 = a mode-6 ghost record), +0x44 race block + 0x40, +0x48 slot 0's car id, +0x50 s16 first sector, +0x52 s16 sectors, +0x54 s32 payload bytes, +0x58 u32 CRC-32 of the payload |
| 0x1508 | u32 CRC-32 (0x80083178 = zlib's) of 0..0x1507 |
| 0x1580 | data: sector s at 0x1580 + s * 0x80, an entry's sectors follow its chain |

Directory rules: 0x800691DC valid = blocks 2..15, count <= 32, every entry's first sector below the total, the entries'
sector counts add up to the used table slots (0x80068FE8; free ones 0x80068FA8), total = the table's count, the CRC.
0x80069358 fits(index, size): a new entry needs count < 32 (else 1) and free sectors >= ceil(size / 128) (else 2); a
replaced entry adds its own sectors. 0x80069418 store(index, desc, payload, size, list): frees the replaced chain
(0x80068EBC), takes the first free slots in table order (0x80068EE4), links them (0x80068E50), copies the 0x5C-byte
description, sets first / sectors / size / CRC, count + 1 for a new entry, the directory CRC; the data goes to the listed
sectors (0x80069758 -> 0x8007D658(7)). 0x800695DC removes an entry (chain freed, later entries moved down, count - 1; the
CRC is refreshed by 0x800693EC before mode 2 writes). An entry is read by following its chain (0x80020E14 for the demo
files in RAM, 0x800697E8 -> 0x80068F50 + 0x8007D658(6) from a card) and accepted when its CRC matches (0x800692DC).
The uncompressed VOL file arcade/demofile.gmr (5 blocks) is loaded by no code; its entry 1 does not unpack within its size.

### 9.2 Payload (0x80069948 packs / 0x80069AC4 unpacks; every piece padded to 4 bytes, the padding is the buffer's residue)

RAM 0x801D585C (0x58C bytes: race block 0x5C, 6 car slots 0xD0, 0x50 more); then per player (game mode 0: two, else
one): the parameter record 0x801DE8BA + i * 0x1C0 (not in game mode 0), the results record 0x801D5E88 + i * 0x4518 (0xFC;
0x8005E2FC first), the stream object 0x801D5F84 + i * 0x4518 (0x19 + its used bytes, the length taken from the source's
+0x10). Game mode 6 (Time Trial): the results record, u8 ghost count (the s16 0x801D5F84), per ghost 0xE0 bytes of
0x801D5F88 + k * 0x10FC and the stream object behind them. The replay's slot configurations rebuild the cars (the title's
0x800109C0 / 0x80010B08 call 0x800771AC for every slot), so the saved parameter record is overwritten before the race.
The save side: 0x80072E7C (mode 0, replay: 0x800724F8 packs and describes it) / 0x80072EB4 (mode 0, ghost: 0x80072598,
0x80069CC0 / 0x80069D58), then the card manager's states 7 / 8 ("Select Number of Blocks", 3 .. free blocks) / 10 (the
list with "- New File -", L1 + R1 delete mode) / 11 (name entry 0x80073548) / 12 (create 0x800696EC) / 13 (write).

### 9.3 Replay Theater (title jump table slot 1) and how a replay plays

Views of member 1: 0x8004B2CC (20 empty fields, CD track 0, the demo file of the language loaded by 0x80020DCC: id
*(0x8004C8A8 + language * 4), US 0x25 = arcade/demofile_us.gmr) -> 0x8004B374 "REPLAY THEATER" (list 0x8004B16C, 4 rows
= panels of arcade/topmenu_panels_us.tim, sprites 0x8004B104, row callback 0x800125D0 -> the title row draw 0x80016410)
-> Load Replay 0x8004B3C8 (card manager mode 1), Rename & Delete 0x8004B41C (mode 2), Copy Replay 0x8004B470 (member 1's
0x80015CF8 / 0x80015DC0 / 0x80015F4C), Demonstration 0x8004B224 (list 0x8004B1D8 of the demo file, rows 0x80012B84).
Card manager mode 1 (0x80072EEC): states 0 -> 2 "Select a Slot" -> 3 (file present: 7, none / unformatted: 0x11 "No
Replay Files Found", no card: 4) -> 7 "Checking Replay File" (0x800697AC reads 0x1580 bytes, progress 0x90500C; count 0
-> 0x14 "No Replay Data in File"; 0x800691DC fails -> "Loading Failed") -> 0x12 the list 0x80091FE4 (rows 0x8006F060 ->
the row printer 0x8006A4E4: title, kind (Race / License / Time Trial / Test Run / 0-400m / 0-1000m / Max Speed / 2P /
Ghost), course (.crsinfo name of +0x44; a licence: "S-%d".."B-%d" of +0x44 >> 16 / & 0xFF - the original writes the
course id there, so its own licence replays read "S-255"), car name, "%d sector"; ghost rows disabled) -> 0x13 "Loading..."
(the entry's sectors, progress 0x50782C, CRC, 0x80069AC4) -> exit 0x28. The view then sets manager + 0x21C (0x800128F8),
0x8004B320 waits 20 fields; the entry 0x80011384 runs 0x80010EDC (settings block of the event row, the cars rebuilt) and
the race overlay with argument 1. The race frame (ovl0 0x80015DCC) returns 0 once 0x800A8D68 (the stream ran out) is
set: the replay ends by itself and member 1 comes back with 0x801EF5F2 == 2, i.e. into the theater.

gt2game: title -> Replay Theater -> Load Replay reads the card slots (`--card` / `--card2`, default saves\card1.mcd) and
Demonstration the demo file; the chosen replay plays in the same window (`RunRaceView` with `replayEndLeaves`, Esc leaves
it) and the theater comes back. (2026-09-19, later: Rename & Delete and game mode 6 records - section 9.5 / 9.6.) Copy Replay: section 9.7 (ported
2026-09-19). Two-player records (a slot of kind 4, game mode 0) play in the split screen since 2026-09-19 (section 9.8).

### 9.5 The card manager's replay modes 0 / 2 / 3 (2026-09-19)

Evidence: our disassembly / Ghidra pseudo-C of the EXE (work/re/theater/decomp: 0x8006F298, 0x8006F3E0, 0x8006F5DC, 0x8006F6E8,
0x8006FF8C, 0x80070254, 0x8007031C, 0x800704C4, 0x800705D0, 0x80070798, 0x80070C14, 0x80070E38, 0x800717B8, 0x80071B30, 0x80072044,
0x8007263C, 0x800728F0, 0x80072B78; the state table 0x800921D4), gt2run sessions with call logs (work/re/cards/rd1, sg1, lg1) and
gt2play captures (work/play/cards/rd_cap, rd_cap2, sgcap). Port: `src/game/shell/title_screens.*` (CardManager), the keyboard
adapter `gt2view/race_record_screens.h MakeTitleCardKeyboard`, the race overlay's views `gt2view/race_card_screens.*`.

- Entries: 0x80072E7C (mode 0, replay: 0x800724F8 = 0x80069948 into mgr + 0x26A4, size mgr + 0xA6A4, the description mgr + 0x1EC4:
  "No Name", + 0x40 0x801D5865, + 0x41 0x801D5866, + 0x42 0, + 0x44 0x801D589C, + 0x48 0x801D58B8, car name 0x801D5948 by
  0x800690B8), 0x80072EB4 (mode 0, ghost: 0x80072598 = the same with 0x80069CC0 and + 0x42 = 1), 0x80072F20 (mode 2), 0x80072F54
  (mode 3; clears the loaded flag 0x801C94E0, which 0x80072F8C returns). All: 0x8007263C (the bars mgr + 0x50 + k * 0x98 with the
  templates 0x80091F04, F34, F64, F94, 0x80092018, 048, 078, 0A8, 0D8, 108, 138; the block selector 0x80091FC4; the list 0x80091FE4
  with the row callback 0x8006F060; the keyboard mgr + 0x6D8 = 0x80073548 with the descriptor 0x800921A0, + 0x1E = 31 characters,
  + 0x20 = 256 pixels, the name buffer mgr + 0x752), then state 0.
- 0x800728F0 per field: mgr + 0x20 = (held & 0x1010) == 0x1010 (L1 + R1: the delete mode) in modes 0 / 2; the band, the bars
  0..3, the block selector (0x8006D9DC), the list (0x8006CFC4), bars 4..6, the keyboard (0x80073720), bars 7..10, the state.
- State 3 (0x80071B30): mode 0: the file -> 7, none -> 8, unformatted -> 5 (after formatting 0x80072044 goes back to 3; mode 4
  to 0x1F); modes 1 / 3: file -> 7 else 0x11; mode 2: file -> 7 else 0x16. State 7 (0x8006F3E0) reads the directory (0x1580
  bytes) and goes on to 10 (mode 0, the list after one field), 0x12 (mode 1), 0x17 (mode 2, selection 0, one field), 0x1A (mode
  3); an empty file: 0x14 / 0x18 "No Replay Data in File" (mode 0 lists "- New File -" only).
- State 8 (0x8006F298, mode 0 without a file): "Select Number of Blocks" when the card has >= 3 free blocks (else 9 "Not Enough
  Empty Blocks"): the selector 0x80091FC4 {x 0xB0, y 0x118, value = min 3, max = the free blocks, colour 0x78A0BE, the medium font,
  height 24, spacing 2, digit shift -4, sound 6, anim}: left / right step (sound 6), the value centred by 0x8006B0EC at y + 12,
  POLY_F3 arrows at x -/+ cell * 2 (value != min / max) of (t * 255 / 10) | (that >> 1) << 8, t = clamp(0x28 - anim, 0, 10),
  anim 0..0x2D; choose -> 0x8006911C (the file of that many blocks in memory), 12 fields, state 10; back -> 2.
- States 10 / 0x17 (0x8006F6E8): h(0) clears the lines, + 0x934 = the file's total, + 0x936 = its used sectors (0x80068FE8), the
  rows mgr + 0x890 + i * 4 {entry, kind, enabled, fit}: mode 0 = the entries (enabled when 0x80069358(i, size) == 0) and "- New
  File -" (kind 1, 0x80069358(-1, size)), selection = the count; mode 2 = the entries (enabled) and "- OK -" (kind 2), selection
  + 0x932. h(1): the list opens when + 0x12 runs out (sound 7); -3 sound 6, -4 sound 0; back: sound 2 -> 2 (mode 0) / 0x15 (mode
  2); a row: sound 1 (mode 2: + 0x932 = the row), "- OK -" -> 0x19; in delete mode 0x800695DC removes the entry (in memory) ->
  10 after one field, else + 0x930 = the entry (-1 = new) -> 0xB after 12 fields. Enabled (command 8): delete mode = kind 0,
  else + 0x892. The row colour DAT_800921CC = 0x324052 in delete mode (0x5A4A3E). h(2) (US): "Press L1 + R1 for Delete Mode"
  (small font, spacing 0, 0x020C3060, right-aligned to 0x148 at y 0x7C), the sector bar 0x8006AA68(+ 0x934, 0xB0, 0x1C2, the
  list's fade, the selected entry's sectors, mode 0: the payload's sectors / mode 2: the entry's): two semi POLY_G4 of 8 lines at
  y - 4 (0x80091E98 -> 0x80091EA0 for the other used sectors, 0x80091E9C -> 0x80091E98 for the selection / the payload; widths
  * 256 / total), E1 0x220, a black TILE 256 x 8 and the frame TILE 0x102 x 0xC of 0x80091EA4, all faded toward 0x80091E7C by
  0x80 - fade; "total %d replay" and "%d/%d sector free" (tiny font, spacing 1, 0x024A4136, right-aligned to 0x148 at y 0x1A4 /
  0x1B4), mode 0 also "need %d sector" at 0x140 - both widths. The Japanese action line (0x8006EE08) draws nothing in the US build.
- State 0xB (0x8006FF8C): after 12 fields the keyboard opens on the entry's title ("" for a new entry), caret at its end; OK:
  sound 1, mode 2: 0x80069028 into the entry, mode 0: into the description, 0x80069418 stores the payload, 0x800691DC (a failure
  -> "Loading Failed"), then 0xD (the file exists) or 0xC (0x80070254: 0x800696EC creates it, "Failed to Create Replay File");
  CANCEL: sound 2; both of mode 2 and CANCEL -> 10 after 12 fields. h(2): "Edit Replay Data File" as the hint above.
- State 0x15 (0x8006F5DC, mode 2 back): "Changes are not Saved" / "Cancel?", the bar + 0x640 with No: Yes -> 2 (the changes
  dropped: the next state 7 reads the card again), No / back -> the list after one field.
- Saving: 0x19 / 0xD (0x8007031C): mode 2: 0x800693EC (the directory CRC), the progress over 0x150C bytes, -> 0xE at once; mode 0:
  0x80069758 writes the entry's sectors (the progress over size + 0x150C), then 0xE. 0xE (0x800704C4): 0x8006971C writes the
  file's first 0x1580 bytes -> 0x10 "Saving Complete" (0x80070798: OK Change Slot / Exit [Exit]); a failure -> 0xF (0x800705D0:
  "Saving Data Failed" / "Try Saving Again?", Yes -> 0xD, No -> 2).
- Mode 3: 0x1A (0x80070C14) lists the entries with + 0x42 == 1 and + 0x44 == 0x801D589C (the race's course; none: "No Matching
  Data Found" -> 0x14), selection 0, opened at once (sound 7); a row: + 0x930 = its entry -> 0x1B (0x80070E38: the sectors with the
  progress 0x50782C, 0x800692DC, 0x80069D58, 0x801C94E0 = 1) -> 0x1C (0x800717B8 = 0x25: "Loading Complete").
- The ghost file (0x80069CC0 / 0x80069D58, `replay_card.h ReplayGhostRecord`): race block entry 1 (0x801D5988, 0xD0), its
  parameter record 0x801DEA7A (0x1C0), the reference lap's head 0x801DA4A0 (0xE0) and stream object 0x801DA580 (0x19 + used),
  each padded to 4.

Observed residue (not reproduced): the description buffer mgr + 0x1EC4 keeps earlier bytes after the title's and the car name's
terminators (0x80069028 / 0x800690B8 copy only the strings), so the original's entries differ from ours in those invisible bytes
and in the directory CRC over them; the payload sectors' tails after the payload likewise hold the buffer's residue in the
original (ours zero).

### 9.6 Game mode 6 records in the theater; the arcade replays' car tables (2026-09-19)

- The theater's "Demo 02" / "Demo 05" and any Save Replay of a Time Trial / Rally are mode 6 records (race block, results record,
  the lap ring's laps). The title's 0x80010EDC then runs the race overlay with argument 1: player 1 alone plays the ring's laps from
  lap buffer 0 (the arcade loop's replay of 17.10). Port: `tools/gt2game/ghost_replay.*` (GhostRecordOf, LoadGhostRecordRace: the
  ring from the record, ReplayRaceData; FramesCompareGhostRecord), the theater and `gt2game --replay <file>#N` play them.
- 0x80010EDC by race block + 9: 0 (an arcade race) -> 0x80076E04 loads the arcade data (the file of 0x800925A4[language * 10 + 2]
  into 0x800B15C0) and 0x80010C50 takes the event row from its race table (*0x80092E6C), so 0x800771AC builds every slot with the
  arcade car tables (a configuration with + 0x7A bit 6 keeps the GT tables, arcade_race.cpp's rule); 1 -> 0x800109C0 with the
  GT-mode race table (*0x80092E70). gt2game used the GT-mode tables for every replay: the demo file's arcade replays (Demo 04 /
  06 / 07, race block + 9 == 0) drove other cars (Demo 04: 3124 of 3124 frames differed). LoadReplayRace and LoadGhostRecordRace now
  follow + 9.
- Verification (the original's theater captured with `gt2verify --race-capture <Sim disc> ... "1200:down,1260:cross,1400:down,
  1430:down,1460:down,1500:cross,1720:down[,1760:down,1800:down],1800|1850:cross"`, work/re/theater6): `gt2game <Sim disc> --replay
  demo#1 --frames-compare demo02.bin` 2984 frames, 0 differ (car 0, the ring's counters and lap buffers, the snapshot / playback
  block, the clock); `--replay demo#3 --frames-compare demo04.bin` 3124 frames, 0 differ (was 3124 differing); the earlier runs
  (attract 3195, card_b1_replay 1257, ours 1053) still 0.

### 9.7 Copy Replay (theater row 2; 2026-09-19)

Member 1's own card screen (GT2.OVL member 1 of US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): view
0x8004B470 "COPY REPLAY" (colour 0x2878F2; init 0x80012AA4, update 0x80012B00: the screen's exit plays sound 4 and goes back to
the theater, draw 0x80012B60) over setup 0x80015CF8 (object 0x8004B1BC), init 0x80015B6C, per field 0x80015DC0, draw 0x80015F4C
and the 21 state handlers of the table 0x8004B7D8. It copies chosen entries of the replay file on card 1 into the replay file on
card 2. Evidence: our disassembly / Ghidra pseudo-C (work/re/theater), gt2run sessions and gt2play captures of the original with two
cards (work/play/copy: s1..s3, cap, capa, capb, capd). Port: `src/game/shell/title_copy.*` (CopyReplayScreen), gt2game
`title_mode.cpp`; facts `db/sim_us12_symbols.yaml` (block "The replay theater's Copy Replay").

- The interpreter's second card port (this work): `Machine::AttachMemoryCard(path, slot)`; the BIOS card calls name the slot by
  port 0x00 / 0x10 and the file API by "bu00:" / "bu10:"; `gt2play --card2`, `gt2run session ... card2=`, `GT2_CAPTURE_CARD2`.
- States (h(0) enter / h(1) field / h(2) draw): 0 24 fields -> 2; 1 the error text + 0x20 (centred (0xB0, 0x122), 0x022A485C) with
  bar + 0x60; 2 main: the lines of 0x80013EA0 (card 1: "Cannot Detect Memory Card 1" / "Cannot Find Replay Data on Memory Card 1" /
  "Memory Card 1 is Corrupt"; card 2: "Cannot Detect ..." / "... is Not Formatted" / "... is Corrupt") or "While Copying Data" /
  "Please Do Not Remove Memory Card", bar + 0x190 "Copy Replay" Start / Exit (its fill + 0x21C = 0x02080830 while a problem stands
  and the cursor is on Start); card 2 ready without a replay file -> 5, unformatted -> 3 (sound 7); Start: 0x80013E14(1) (card 1
  ready with a replay file, card 2 ready) -> sound 1, the chosen flags cleared, 9 (file) / 5; refused: sound 0; Exit / back -> 0x16;
  3 "Memory Card 2 is Not Formatted" / "Do You Want to Format?" (bar + 0x228, No) -> 4 (0x8007F5A8(1)) -> 5; 5 "Select No. of
  Blocks on Memory Card 2" (selector 0x8004B704: min 3, max = card 2's free blocks 0x801F0BE2, sound 5; < 3 -> 6 "Not Enough Empty
  Blocks on Memory Card 2"): a value -> 0x8006911C(file 2, blocks) -> 7 (0x800696EC create, "Creating Replay File on Memory Card 2")
  -> 8 (0x8006971C writes the header and directory, progress 0x0C5090 over 0x150C) -> 2; back -> 0x16; 9 "Checking Slot 2..." (card
  2's directory, 0x800691DC) -> 10 "Checking Slot 1..." (card 1's; 0x800136EC keeps card 2's total / used for the sector bar;
  empty -> 0x14 "Memory Card 1" / "No Replay Data in File") -> 0xB the list.
- The list 0x8004B724 (rows 0x80013AA4, the row printer 0x8006A4E4 with the style 0x8004B7C0; opened one field after state 0xB,
  sound 7): card 1's entries and a kind-2 row ("- OK -", enabled when an entry is chosen). An entry row is enabled when chosen, or
  when card 2's free sectors minus the chosen ones hold it (else "Data is Too Large") and card 2's count plus the chosen ones is not
  32 ("Max 32 Files"); choosing it toggles its "COPY" mark (sound 1 twice; the list stays open, the selection kept); moves sound 5,
  a disabled row sound 0, back sound 2 -> 2. h(2): "total %d replay" (card 2 + chosen), "%d/%d sector free" (after the copy), the
  sector bar 0x8006AA68 at (0xB0, 0xAA) with the chosen sectors highlighted, "Copy %d replay." and the selected row's message at
  (0xB0, 0x1B8) ("Copy" / "Data is Too Large" / "Max 32 Files" / "Confirm" / "Cannot Find Replay File to Copy").
- The copy (the kind-2 row): 0xC 0x80015158 gathers the chosen entries in order (their 0x5C descriptions, payloads rounded to
  sectors into + 0x2FD0); 0xD / 0xE read each entry's sectors from card 1 (0x800697E8, CRC 0x800692DC; "Loading from Memory Card
  1...", progress 0x50782C); 0xF .. 0x11 per entry 0x80069418(file 2, -1, description, data, size) and 0x80069758 (whole sectors
  from the buffer: past the payload the last sector keeps card 1's bytes); 0x12 the header and directory (0x8006971C); 0x13
  "Copy Complete" (sound 7, bar + 0x2C0 Restart / Exit) -> 2 / 0x16.
- Verification: frames in title.md 9.2 (0 differing pixels); the cards: a fresh card 2 (the copy creates a 3-block file and copies
  "License" + "Short B1" of `work/play/cards/card_r3.mcd`) and card 2 holding that file (+ "Demo 01") - after the same presses
  gt2game's card 2 equals the original's byte for byte (all 131072 bytes, both runs: `work/play/copy/nat6/c2.mcd` =
  `work/play/copy/orig_c2.mcd`, `work/play/copy/natd/c2.mcd` = `work/play/copy/capd/c2.mcd`).
- Not compared: the "Not Formatted" path (our interpreter's HLE card reports a zeroed image as a formatted card: the original went
  to "Select No. of Blocks" with it), "Not Enough Empty Blocks", "No Replay Data in File", the error states, the progress frames
  (our card rate: 8 sectors per field), the bars' pulse / title shine (title.md 9). Our card transfers are faster than the
  original's in the interpreter: the list opens ~1260 fields after the run start of the capture's timeline instead of ~1180.

### 9.8 Two-player records (game mode 0; 2026-09-19)

US Arcade v1.1 records (the 2 player Battle, docs/research/arcade_disc.md 19.8), played by either disc's theater. Code:
`gt2formats/replay.h ReplayFile::stream2`, `replay_card.* PayloadOfReplay / ToReplayFile` (player 2), `tools/gt2game/split_race.*`
(SplitPads, RunSplitRace with `SplitRaceConfig::replay`, FramesCompareSplitReplay), `race_common.cpp LoadReplayRace` (mode 0's race
load settings), `title_mode.cpp` (the theater's kind-4 records), `arcade_mode.cpp RunArcadeBattleSession` (Save Replay ...).

- Both players are recorded: pad slot 2 (0x80013EF0, stream 0x801D5F84) and pad slot 3 (0x80014030 with the pad object 0x800A95D8,
  stream 0x801DA49C = 0x801D5F84 + 0x4518), both through 0x80013C90 and the same end rule (0x800A9522 < 300: 300 fields after the
  first finish, so the record ends ~150 frames after the winner's finish). Playback (0x800A951C): both streams; either one ending
  sets 0x800A8D68.
- The payload (0x80069948, section 9.2): the race block, then player 1's results record and stream object, then player 2's; no
  parameter records in mode 0.
- The race of a record (0x80010EDC with race block + 9 == 0: the arcade car tables and the arcade race table's event row "A2P"),
  then the race load 0x8003C12C: mode 0 applies Tire Damage (race block + 3) and Slow Car Boost (+ 7) to the settings
  (race_sim.h RaceLoadSettings, the catch-up enable) - also for a replay. LoadReplayRace now does so for mode 0: before, the
  original's theater replay of a record had the tyre wear bytes of the option in its cars (body + 0x498 / + 0x49F / + 0x4C4 per
  wheel) and ours not (1500 of 1500 frames differed).
- Verification (the Sim disc's theater, `GT2_CAPTURE_CARD=<card> gt2verify --race-capture <Sim disc> 7200 <out>
  "1200:down,1260:cross,1400:cross,1550:cross,1800:cross"`, then `gt2game <Sim disc> --replay <card>#0 --frames-compare <out>`:
  both cars' bodies byte for byte and player 1's stream object every frame): gt2game's record (both players driven by keys /
  `--fake-pad2`, `work/re/theater2p/ours_card.mcd`) 1500 frames, 0 differ; the original's record (saved by its 2PLAYER BATTLE menu,
  `orig_card.mcd`) 1290 frames, 0 differ.

### 9.8 The race overlay's replays of GT mode; the licence demo runs (2026-09-19)

- After every race of the menus the overlay plays its replay at once (state 10 -> 12, race_screens.md 5.5); the menus' Replay rows
  play it again, their "Save Replay ..." rows run the card manager's mode 0 (view 0x8005B51C) on it. gt2game: RaceFlow in
  race_view.cpp, SAVE REPLAY `career_race.cpp RunSaveReplayScreen`. Replays gt2game saved that way from a licence run, a Test Run
  and an event race play in the original's theater exactly as in gt2game (1303 / 3400 / 4742 frames, 0 differ).
- The licence menu's Demonstration plays `license/a_<l>NN.lgf.gz` (36956 bytes, a RAM image of 0x801D585C..: race block, six slots,
  player 1's stream object at + 0x728, a parameter record at + 0x4C40 = 0x801DA49C; EXE 0x80069EF8 / 0x80069F28, race_screens.md
  5.5): 180 files, a_ / e_ / i_ blocks of 60 (the US build takes a_: the path list entry 225). `gt2game --replay licence-demo:B-1`
  (`LicenceDemoReplay`, the slots' configurations rebuilt with the licence tables as for any licence replay) = the original's
  Demonstration of B-1, 1246 frames 0 differ.
- The race block of a GT-mode race holds the career's option bytes + 2 .. + 8 at + 1 .. + 7 (both captured blocks); gt2game's saves
  from the menus now write them (BuildReplayFile alone still writes the attract dump's + 4 = 1).
- (later) Race block + 0x44 is the row's sponsor category (licence LJB00: "0", not "General01") and + 0x57C..+ 0x58B the menus' tail
  (result index, GTW flag, player, garage slot, power limit, flags; db race_block_tail): gt2game's licence save now equals the original's
  own save (card_b1_replay.mcd) in the race block, the six slots and 0x53C..0x58B; slot 0 + 0x8F is the TRANSMISSION choice
  (race_screens.md 5.6).

### 9.4 Verification

- gt2verify on `work\re\title\ram.bin` (the EXE rows run on any dump; the directories are the disc's demo files):
  RepValid 0x800691DC 3000 cases (mutated directories), RepCrc 400, RepFits 2000, RepStore 0x80069418 1500 (directory
  bytes + sector list), RepDel 337, RepNew 0x8006911C 14 (the save header from the EXE included), RepUnpack 0x80069AC4 and
  RepPack 0x80069948 327 payloads each (the demo files' and random ones of modes 0 / 1-4 / 6 / 7 / 10 / 11), RepGather
  0x80020E14 27 (every entry of the three demo files the game loads) - 0 mismatches.
- The original's saved replay, loaded natively and played: `gt2verify --race-capture` of the original's theater playing it
  (`GT2_CAPTURE_CARD=<copy of card_b1_replay.mcd>`, script `1200:down,1260:cross,1400:cross,1550:cross,1800:cross`, 4700
  fields) against `gt2game <disc> --replay work\play\theater\card_b1_replay.mcd --frames-compare rc_orig.bin`: 1257 frames,
  0 differ (car bodies byte for byte + the stream).
- A replay gt2game saved (`--license B-1 --replay-out ours_b1.mcd` with a scripted drive) loaded and played by the original's
  theater, captured the same way: 1053 frames, 0 differ against `gt2game --replay ours_b1.mcd --frames-compare`. The file's
  save header, the card's directory frame and the sector table are byte-identical to the original's own save.
- `gt2game --replay-check` (seattle 6 cars 1799 steps, B-1 499 steps): identical with the new file writer.
- The frames of the screens: `docs/formats/title.md` section 9.

`--replay <file>[#N]` plays replay N of a replay file (.gmr / demo files) or of a memory card image (.mcd); `--replay-out
<file.mcd>` adds the race's replay to the card's replay file (created with 3 blocks when missing). gt2game's replays leave
the race block's option copies (+1..+8) and licence / test bytes (+0xB / +0xC) zero (BuildReplayFile): the original then
shows the course map in a licence replay; the driving is identical (above).

Title-launched replays and Start (2026-09-19, docs/formats/title.md section 11): the title starts the race overlay with argument 1
for the theater and the attract demo alike; the overlay keeps it in 0x800A9500 (byte 0 of the race task, 0x800121AC) and its frame
0x80015B64 ends the race on a pressed Start when it is set (0x80015C90: return 0 -> member 1) instead of opening the pause (observed
in the attract: Start -> member 1, Cross ignored). The demo file's seven replays play frame-exact natively on both discs (title.md
section 11); the race block's + 0x58 dirt level (0x801D58B4) is player 1's dirtiness at the start (LoadReplayRace -> RaceData::dirtLevel).
