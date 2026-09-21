# Menus and GT-mode career: shell map, career state, save format, port plan

Status 2026-09-18. US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). All addresses are
guest addresses of that build; overlay addresses are valid while the named GT2.OVL member is loaded at 0x80010000
("ovl0" race, "ovl1" title, "ovl2" arcade menus, "ovl3" arcade loader, "ovl4" GT-mode menus, "ovl5" movie; see
`docs/research/scout_exe.md` section 4). Evidence = our own scripted runs of the original in the interpreter
(`gt2run session`, below), our Ghidra decompilation of RAM dumps taken in each overlay (pseudo-C only in `work\re\menu_*\decomp`),
objdump, and bytes of the disc files. Data-file formats (used-car lots, car catalogue, events, strings, colours, menu
containers) are in `docs/formats/gtmode_tables.md`; this document covers the code side, the career state and the save.

## 0. Summary

- The shell is a chain of overlays switched by `0x8005DA3C(member)` / `0x8005DA7C(member, entry, arg)` (loader
  `0x8005DAD8`: inflate member to 0x80010000, then jump to the entry with a fresh stack). Each overlay entry runs one or
  more "view loops" (C++ objects driven by `0x800833E8`) and then decides the next overlay from a few shell bytes at
  0x801EF5F0... Sim disc flow: boot -> ovl1 title -> ovl4 GT mode <-> ovl0 race (events, licences, machine tests) and
  ovl1 -> ovl0 (replay theater, attract demo). ovl2 / ovl3 (arcade) were never loaded in our runs: the only switch to
  member 2 is the race overlay's arcade loop with mode family 1, which no Sim-disc title path was seen to set.
- GT-mode menus are data driven: every screen is a "GM" page from `gtmenu/usa/gtmenudat.dat` (3386 gzip pages; index
  `.idx`), made of item records (rectangle, flags, type code, target page) that one interpreter in ovl4 draws
  (`0x8001B9AC`) and acts on (`0x80014380`). Game rules sit in small, clean functions next to it (buy, sell, garage,
  money, entry checks, prize preparation); the race overlay applies the results (prize money, stats, medals, prize
  car).
- The whole career is one block at RAM **0x801C98E0**; the first **0x7C9C** bytes are saved verbatim. The save file
  `BASCUS-94455GAME` (4 blocks) = 0x200-byte "SC" header + that block + CRC-32. Verified on a save written by the game
  in our interpreter: payload == RAM byte for byte, CRC matches (`gt2tool save-info`).
- A garage car is 0xA4 bytes and contains the full `CarConfig` (0x84 bytes, `car_params.md` section 3) at +8. The config
  the game stores on purchase is reproduced natively byte for byte (catalogue config 0x80076954 + 0x80016FEC flags +
  record builder write-backs).

## 1. Method and reproducible runs

New tracing command (tools only): `build\gt2run.exe session <disc> <fields> <outDir> "<script>" [card=<mcd>]
[snap=f1+f2..] [shots=N] [watch=addr:len[:max]] [watchfrom=N] [calls=A+B..] [trace=from:count] [io]` - logs every
overlay load, calls of chosen functions with a0..a3 (delay slot evaluated), watched writes with pc/ra, RAM dumps and
screenshots at chosen fields, a call graph window, I/O statistics (`tools/gt2run/session.cpp`). Runs take ~15-50 s.

Routes (field:button[:hold], fields are 1/60 s; all from a cold boot; `card=` attaches a memory card image):

| Goal | Script |
|---|---|
| world map (new game, day 1) | `1400:cross` (map from ~f1990; overlay 4 loads at f1421) |
| map directions from Home | up North City (European makers), right Licence, right+right East City (Japanese), right+down South City (US), left GTF (race events), down Machine Test, right+down+right the exit sign (back to the title) |
| used-car list of maker 18 (first dealer entered in the Japanese city) | `1400:cross,2000:right,2030:right,2100:cross,2300:cross,2560:right,2600:cross,3000:right,3040:cross` (the list is a popup: up/down select, cross/circle choose) |
| buy the 2nd used car (5148 cr) | above + `3100:down,3150:cross,3300:right,3340:right,3380:right,3430:cross,3600:cross` (garage written at f3601) |
| back to the map, exit to the title, save | + `4050:cross,4300:up,4330:up,4360:left,4390:left,4420:left,4450:cross,4700:right,4730:down,4760:right,4800:cross,5200:down,5300:down,5400:down,5500:cross,5800:cross,6050:left,6100:cross` with `card=` (file written at f6100; `work\memcards\gt2_save_1car.mcd`) |
| licence B-1 and back | `1400:cross,2000:right,2060:cross,2400:cross,2700:cross,3500:cross,4300:cross:1300,6000:start,6100:down,6150:cross,8000:down,8030:down,8060:down,8090:down,8130:cross` (race overlay at f2744, back in ovl4 at f8176, day 2) |

Ghidra projects (headless, `re/ghidra` scripts): `gt2_menu_gt` (RAM at the maker-18 used-car page, ovl4 + resident EXE,
862 functions), `gt2_menu_title` (ovl1 + EXE card code, 439), `gt2_menu_race` (ovl0 shell parts, 418). Seeds =
`work\listings\exe\sim_ovl*_funcs.tsv` + traced functions.

Emulation limits met: the 3D car of the dealer pages and the highlight bar of popup lists are not rasterised with
`skip3dRaster` (screens look "stuck" but the game runs); MDEC is a stub (not used by the menus we visited).

## 2. Shell map

### 2.1 Overlay switching

| Routine | Role |
|---|---|
| 0x8005DA3C(member) | entry = EXE table 0x80091174[member]; -> 0x8005DA7C |
| 0x8005DA7C(member, entry, arg) | saves the arguments at 0x801D945C, 0x8005DAD8(member), then jumps to `entry(arg)` on a reset stack (0x8007AD90, longjmp-like) |
| 0x8005DAD8(member) | stops sound (0x80078370 / 0x800783DC), GT2.OVL from the RAM cache or CD (0x8007AB74 into 0x800A8D5C), inflate 0x80082FAC to 0x80010000, FlushCache, 0x8005D9BC |

Observed loads (session logs): f476 ovl1 (from EXE main 0x8005D700), f1421 ovl4 (title "start" 0x800114EC), f2744 ovl0
entry 0x80011F64 (licence, from ovl4 0x800137FC), f8176 ovl4 (from ovl0 0x8001222C), f4819 ovl1 (map exit, ovl4
0x80013A00).

All switch sites (jal scan of EXE and all members, arguments from the instructions before the call):

| From | Site | To |
|---|---|---|
| EXE main | 0x8005D700 | ovl1 (title) |
| ovl1 | 0x800114EC | ovl4 (Start Game) |
| ovl1 | 0x80011450 / 0x8001179C / 0x80011714 | ovl1 restart at 0x8001172C; ovl0 0x80011F64 arg 1 (attract demo / replay theater) |
| ovl4 | 0x800137A0 | ovl1 at 0x8001172C (menu result 4) |
| ovl4 | 0x800137FC / 0x800139E4 | ovl0 0x80011F64 arg 0 (race: result 2 / 3) |
| ovl4 | 0x80013A00 / 0x80013A08 | ovl1 (exit to the title) |
| ovl0 | 0x8001222C | the member returned by the race loop: 1 title, 2 arcade, 4 GT mode |
| ovl2 | 0x800117F4 / 0x80011924 / 0x8001193C | ovl1 / ovl3 / ovl0 |
| ovl3 | 0x80012CD4 | ovl0 |
| ovl5 | 0x800114C8 / 0x80011504 | ovl1 |

### 2.2 Shell bytes (0x801EF5F0.., cleared by boot-block 0x800107E8)

| Address | Meaning (evidence) |
|---|---|
| 0x801EF5F0 | first-boot flag: ovl1 runs the logo view only while 0 |
| 0x801EF5F1 | mode set before a switch (4 = GT mode, 2 = replay theater, 0 = title) |
| 0x801EF5F2 | mode family: 0 title, 1 arcade, 2 replay theater, 3 GT mode; the race loops return member 2 when it is 1 and 4 when it is 3 (ovl0 0x80016F88, 0x80017D1C) |
| 0x801EF5F3 | title menu result (ovl1 0x80017984, u8 table ovl1 0x8004BC04 with stride 2): 0 Start Game -> ovl4, 1 Replay Theater, 2 Options, 3 Save Game, 4 Load Game, 5 Data Transfer (established 2026-09-19, section 10.1); 6 attract demo (after 900 idle fields) |
| 0x801EF5F5 | GT-mode menu result (ovl4 entry): 2 race of the selected event / licence / machine test, 3 race (other path), 4 to ovl1 at 0x8001172C, else title |
| 0x801EF5FE | attract-demo counter (ovl1 case 6) |

### 2.3 Title (ovl1, entry 0x80011384)

0x80011178 init; logo view (vtable ovl1 0x8004B900) on first boot; 0x800117B4(state) copies the two 20-byte pad
configurations state+0x48 / +0x9A to 0x800A6EEC; title menu view (0x8004BC78, input 0x80017984: items Start Game,
Replay Theater, Options, Save Game, Load Game); then per result: GT mode (0x801EF5F1 = 4, 0x801EF5F2 = 3,
0x8005DA3C(4)), replay theater view 0x8004B2CC then ovl0, options view 0x8004BFD4, save view (default case,
0x8004B4C4), load view 0x8004B56C, view 0x8004C4A0 (case 5), attract demo (0x800104E0 builds a race block from a demo
event, ovl0 with arg 1). Save / load / guest-garage go through the EXE memory-card manager 0x80070000..0x80073400
(called from ovl1 0x800128D4..0x80013218 and from ovl0 0x800501A4..0x800505xx for replays). Save = title menu
"Save Game" -> slot -> confirm (section 4); the GT-mode overlay itself never touches the card.

### 2.4 GT mode (ovl4, entry 0x80013628)

```
0x80020C50, 0x800187DC                 init
0x800609F8(0x800D0850, 0xC800)         .carinfoa -> pointer 0x801C93D8
0x800222E4(1, "CCOL00" buffer)         .carcolor + .cclatain (colour names)
0x800224E0((days / 10) % 60, buffer)   used-car period -> 0x800B9544 (region-filtered); days = u32 0x801C99D8
0x80076D74 -> *0x80092E6C              carparam/usa_gtmode_data.dat (car tables)
0x80076CF8(0x80024430, 0x2C4C0) -> *0x80092E70   carparam/usa_gtmode_race.dat (events)
0x80019474; view 0x80013BD4 .. 0x80013CD0 (loading view), view 0x80014E60 .. 0x80014E9C (the menu)
dispatch on 0x801EF5F5 (section 2.2)
```
Before a race (result 2): licence names start with 'L' (0x80018608); machine tests are recognised by name
(0x8001861C -> race sub-mode 7 / 8 / 9) and set up by 0x80012C6C (ported and verified 2026-09-20: race_screens.md 5.7,
career::PrepareMachineTest, gt2verify MtSetup; the machine test page is GM page 929); other events: day += 1, 0x80018A84 builds the
championship race list at 0x801D5DF4, 0x80018C8C prepares the prize block 0x801D55AC (section 5.4), free-run events are
recognised by name prefix (0x80018690 / 0x80018768), 0x80013108 builds the race block 0x801D585C (course, opponents
through 0x80076F5C, settings row+0x44 -> 0x801C98A0, one CarParams record per car at 0x801C98E0 + 0x14FDA + i * 0x1C0
via the builder 0x800771AC). Licences: day += 1, 0x8001050C. Then 0x801EF5F1 = 4, 0x801EF5F2 = 3, ovl0.

The menu view object (0x860 bytes, on the entry's stack; update 0x80013EEC, draw 0x8001B3AC):

| Offset | Field |
|---|---|
| +0x19C | input hold-off counter |
| +0x1A0..+0x1B8 | "show car" request: model id, car id, colour name, garage index, flags, slot +8 word, slot +A2 (0x80014348) |
| +0x1B0 | show-car pending (1 = do it this frame: 0x8001D5C8 displays the car) |
| +0x1BC / +0x1C0 / +0x1C4 | next page id, page argument, page-change countdown (3 frames) |
| +0x1CE | popup mode: 0 page, 1 list 0x800A8D60 (new cars), 2 list 0x800A8D68 (used cars), 3 wait (clears slot +A2) |
| +0x2FC | page object: 0x8001D2CC(page, id) loads GM page `id` (seen: 0x45D maker page, 0x4A2 used-car page), 0x8001D208 draw, 0x8001D258 input |

Items: 0x800215C8(i) returns item i of the current page: s16 rect[4], u32 flags (+8: bits 0..15 type code, bit 24
"action", bits 27.. display options), u32 target page (+0xC), variable data. 0x80014380(view, item) is the action
switch; codes seen: 0x09 garage car list (popup 1; corrected 2026-09-19, docs/formats/gt_menu.md), 0x50 used-car list (popup 2; choosing a row shows the car and jumps to
the item's target page), 0x98 / 0x9C / 0xAD / 0xAE car-dependent pages (need a current car 0x801D156C >= 0, else
message page 0x80000002; 0x9C also checks part kind 0x22 and 0x800174AC), 0xBB no-op; any other item with bit 24:
jump to its target page. Messages are pages with ids 0x800000xx (0x1D/0x1E/0x19/0x1A/0x02 seen). 0x8001B9AC draws the
items: text through 0x8001FC28 / 0x8001FC64 (font 0x8001F2C8 / 0x8001F38C), sprites through the EXE primitive
allocator 0x80081478 + 0x8007DA44, medal / licence icons from the result nibbles (0x8005DB90) and the licence level
(0x800191C4). Popup lists: 0x8002055C / 0x80020A94 over the EXE list widget 0x8006CFC4 (returns index, -1 none, -2
cancel, -3 moved, -4).

Car-screen helpers: 0x80018350(carId, colour) colour name; 0x80019718 / 0x800196EC / 0x800196C8 / 0x80019634 per-item
event data (name, prize, ..., licence requirement 1..6 from 0x800B5EA4 + i * 0x24 bits 1..3).

### 2.5 Race overlay shell (ovl0, entry 0x80011F64)

Race block 0x801D585C (0x58C bytes, cleared by the setups): +0 = state byte +1, +4 (0x801D5860) = state byte +5,
+8 (0x801D5864) game mode (2 events, 3 licences), +9 (0x801D5865) loop kind (0 arcade 0x80016F88, 1 GT 0x80017D1C,
2 replay 0x80011F24), +A (0x801D5866) sub-mode (1 event, 2 / 3, 5 licence, 7 / 8 / 9 machine tests, 6 / 10 / 12
replays), +0x44.. course / names, +0x5C six 0xD0-byte car slots, 0x801D58A0 sponsor-board tag (event row +0x94).
The GT loop returns member 4 when 0x801EF5F2 == 3. Post-race rules: section 5.4.

## 3. Career state (RAM 0x801C98E0, "GameState")

Size of the object: 0x15A5C bytes (0x8005E73C clears it); the saved part is the first 0x7C9C bytes. The new-game
defaults are set by the EXE boot block 0x800106A0..0x800107E0 (runs once, before ovl1 overwrites that code; money
write seen with `watch` at f476, pc 0x800107A8).

| Offset | Size | Content | Evidence |
|---|---|---|---|
| 0x0000 | 1 | language (1 = USA; selects the carparam file column of EXE 0x800925A4) | car_params.md, 0x80076D74 |
| 0x0001 | 1 | copied to race block +0 (0x801D585C) | ovl4 0x80012C6C, ovl1 0x800104E0 |
| 0x0005 | 1 | copied to race block +4 (0x801D5860) | same |
| 0x0004..0x0047 | | button assignment tables (bytes 0..0x0D permutations, 0x80/0x82 markers) | dump; written each frame by the options screen 0x80019540.. |
| 0x0048 / 0x009A | 2 x 20 | pad configurations copied to 0x800A6EEC.. by 0x800117B4 | ovl1 0x800117B4 |
| 0x00AE..0x00B5 | | options: +AE, +AF = 0; +B1, +B2, +B5 = 1; +B3 = 0xF0, +B4 = 0xC0 (volumes) on a new game | boot block 0x80010650.. |
| 0x00B8 | 0x160 | career record (RAM 0x801C9998, new game: cleared, days = 1, 0x800107B4) | below |
| 0x0218 | N x 0x24 | course records, N = .crsinfo count (u16 at +6 of the table 0x801E18E0); each starts with a 0x14-byte time record (0x8005DD68: 5 x u32 -1, u16 0) | boot block loop 0x800106D0 |
| 0x1418 | 6 x 10 x 0xA4 | licence test records, licence L (0 S, 1 IA, 2 IB, 3 IC, 4 A, 5 B - the name prefix order LIS/LIA/LIB/LIC/LJA/LJB of ovl4 0x80050C1C) at + L * 0x668 + test * 0xA4: +0, +1 (non-zero = passed), +2 bytes, 5 x 0x14 time records at +4, 5 x 0xC at +0x68 (0x8005DE1C) | 0x8001915C |
| 0x3A88 / 0x3B2C / 0x3BD0 | 3 x 0xA4 | machine-test records (sub-modes 7 / 8 / 9): 8 entries of 0x14 (u32 -1 at +8 of each on a new game, 0x8005E07C) | boot block, 0x8006A2F4.. accessors |
| 0x3C74 | 0x4028 | garage (below) | 0x80010798, 0x8006A2E4 |
| 0x7C9C | 0x4028 | guest garage (2P battle; loaded from another save by 0x8006A2A8; not saved) | 0x8006A2A8 |
| 0xBCC4.. | | not saved: race data (e.g. car records 0x801C98E0 + 0x14FDA), shell variables | |

Career record (+0xB8; base 0x801C9998):

| Offset | RAM | Field | Evidence |
|---|---|---|---|
| +0x40 | 0x801C99D8 | u32 day counter (1 on a new game; +1 per event, licence or machine-test race; used-car period = day / 10 % 60) | ovl4 entry; day 2 after the licence run |
| +0x44 / +0x46 | 0x801C99DC / DE | u16, cleared by the title entry | ovl1 0x80011384 |
| +0x4C | 0x801C99E4 | u32 wins | 0x8005DC64 |
| +0x50 | | u32 sum of finishing positions | 0x8005DC64 |
| +0x54 | | u32 races | 0x8005DC64 |
| +0x58 / +0x5C | | prize total: +0x5C modulo 100,000,000, +0x58 the carry (saturates at 0x7FFFFFFF) | 0x8005DC9C |
| +0x60..+0x15F | 0x801C99F8.. | result nibbles, entry n = byte n / 2 (low nibble for even n); stores a position / medal 1..3 only if better (0x8005DBC0); index = the race's result index 0x801D5DD8 | 0x8005DB90 / 0x8005DBC0, ovl0 0x80059A7C / 0x80017A28 |
| +0x15D | 0x801C9AF5 | set to 1 when a championship ends with 0x801D5DDC set | ovl0 0x80059800 |

Garage block (+0x3C74; RAM 0x801CD554; a second block at +0x4028 for the guest garage - every garage routine takes a
player index p and uses 0x801CD554 + p * 0x4028):

| Offset | Field |
|---|---|
| +0x0000 | u16 car count (0..100) |
| +0x0004 | 100 x 0xA4 car slots |
| +0x4014 | u32 money (RAM 0x801D1568), new game 10,000, clamped to 0..99,999,999 by 0x8005E7B0 |
| +0x4018 | s16 current car (0x801D156C), -1 = none |
| +0x401B | u8, cleared on a new game |

Garage car slot (0xA4; written by 0x8001EC0C, `src/gt2formats/save_data.h GarageCar`):

| Offset | Field |
|---|---|
| +0x00 | u32 packed car id |
| +0x04 | u32 paint id (a character of the car's paint list, e.g. 'b' = 0x62) |
| +0x08 | CarConfig (0x84 bytes) |
| +0x8C | u32 model id (= car id on purchase; shown / raced model) |
| +0x90 | u32 value: purchase price, + part prices (0x8005E8B0) |
| +0x94 | u16: bits 0..12 weight kg of the built record (+0x5A), bits 13..15 drive type (+0x8A) |
| +0x96 | u16: result +4 of 0x80075930 (torque figure) |
| +0x98 | u16: bits 0..13 result +0 of 0x80075930 (power figure; the event power limit tests it), bit 14 = gearbox row +9 < 3 (0x800178E4), bit 15 = 0x80017750 result |
| +0x9A | u8[7] parts owned: bit k = part kind k (0x8005E874 test, 0x8005E900 / 0x8005E8B0 set); for catalogue rows with +0x40 set the fitted kinds of the list ovl4 0x80050B68 are pre-set (0x8001781C) |
| +0xA2 | u16, cleared on purchase and by the view in popup mode 3 |

## 4. Save format (memory card)

- File name `BASCUS-94455GAME` (EXE 0x80091C94; replays `BASCUS-94455REPLAY`, 0x80091AB8), size 0x7EA0 bytes
  (0x80069FF8), 4 blocks (0x8006A000: `(size + 0x1FFF) >> 13`).
- +0x000: "SC" header (0x8006A038): +2 = 0x13 (icon flags, 3 frames), +3 = block count, +4 Shift-JIS title (EXE
  0x80091CA8), +0x60 CLUT (0x80091CC4), +0x80 / +0x100 / +0x180 icon frames (0x80091CE4..).
- +0x200: 0x7C9C bytes = RAM 0x801C98E0.. (0x8006A214 packs, 0x8006A278 unpacks and runs 0x8006A348).
- +0x7E9C: u32 CRC-32 over bytes 0..0x7E9B: 0x80083178 = reflected CRC-32 (table EXE 0x800A6ACC = the standard
  0xEDB88320 table, checked word for word), init 0xFFFFFFFF, final NOT (= zlib crc32). 0x8006A31C compares it on load.
- No compression, no encryption. The file is created by the EXE card manager (0x80072FD0 builds header + payload
  when the save screen opens; the card write happens on "Yes").
- Verification: the save written by the game in our interpreter (`work\memcards\gt2_save_1car.mcd`): stored CRC
  0x27E0A893 = computed; payload equal to the RAM dump of the same moment (0 differing bytes).

## 5. Rules established (to port bit-exactly)

### 5.1 New game (boot block)
Money 10,000, current car -1, garage empty (both blocks), day 1, all time records "none" (-1), options as in
section 3.

### 5.2 Buying and selling (ovl4)
- 0x80017914(price, p): 1 if money >= price and count != 100; -1 not enough money; -2 garage full.
- 0x8001796C(carId, paint, price, p) (new and used cars alike): config = 0x80076954(carId) (catalogue row of table
  30 -> 0x80076FC0, `gtmode_tables.h CatalogueCarConfig`); 0x80017750 runs the setting-screen object 0x801DA4B8 on it
  (0x80015404, 0x80015428, 0x80016C5C copies the config and the stage bytes (+8) of its part rows, 0x80016FEC applies
  0x8005EAC0(obj, 6, gearbox stage), sets flags |= 0xC0 and copies the config back); returns `*0x801DBC8E > 0` ->
  slot bit 15; then 0x8001EE78 -> 0x8001EC0C (builds the record with 0x800771AC on the scratchpad and 0x80075930 for
  the power / torque figures) and money -= price.
- Sell 0x80017A70(i, p): value = table 30 row +0x44 (catalogue price, 0x800177D4); remove slot (0x8001EDAC, keeps the
  current-car index consistent); money += value / 4 (signed division rounding toward zero).
- Move 0x8001EF10(garage, from, to); add prize car 0x8005E7F0 (copies a prepared 0xA4 slot if count <= 99).
- Part purchase check 0x80017B40(i, kind, p): part row by 0x80076570(kind, carId); -8 no such part, -1 money, -3
  already owned, prerequisites: kind 0x13 needs 0x12, 0x14 needs 0x13, 0x22 needs 0x14 (returns -5 / -6 / -7); buy:
  0x8005E8B0 sets the bit and adds the price to the slot value.

### 5.3 Entry requirements (0x8001973C, per event; the table decoding in gtmode_tables.md section 5)
Current car needed (-1); two special checks 0x800183EC / 0x8001859C (-9 / -10); dirt tyres (-2, part kind 0x2D);
licence: requirement r (1 B .. 6 S) met when `6 - first held licence index >= r`, licence L held when all 10 of its test
records have byte +1 != 0 (0x8001915C, 0x800191C4); then drive type, power limit, car list (-8).

### 5.4 Race results (ovl0)
Prepared by ovl4 0x80018C8C in 0x801D55AC: +0 championship bonus = event row +0x98 x 100, +4.. six prizes = row
+0x78.. x 100, +0x1C prize-car count, +0x20 up to 4 prize cars (row +0x84 ids; each a full garage slot built like a
purchase with a random paint of the car's list, value = catalogue price).
- Single race 0x80059A7C: p = position 0x801D5DE8; stats (0x8005DC64: races + 1, position sum + p, wins + 1 if
  p == 1), prize total += prize[p - 1], result nibble[0x801D5DD8] = p if better, money += prize[p - 1], if there are
  prize cars and p == 1: one of them chosen by `rand % count` (0x800597C4: 0x80083AE0 seeded from 0x8007D23C) is added.
- 0x80059704: per-race variant without the nibble.
- Championship end 0x80059800: money += bonus, random prize car without a position condition, 0x801C9AF5 = 1 when
  0x801D5DDC is set.
- Licences: 0x80017A28 stores 1 + (index of the first empty entry of a 6-byte results array) in the nibble array
  (medal; exact derivation not traced); test records via 0x8005DEFC.

## 6. Data the menus consume

| Data | Where | Status |
|---|---|---|
| used-car lots | `.usedcar_usa` (UCAR) period `day / 10 % 60`, 39 maker lists, region-filtered | parsed (`gt2tool used-cars`); period 0 equals the oracle's RAM copy 0x800B9544 (205 entries, maker and price sequence identical, checked 2026-09-18) |
| new-car catalogue | `usa_gtmode_data.dat` table 30 (72-byte rows = chassis rows): part rows, maker, year, price +0x44, flag +0x40 | parsed (`gt2tool car-prices`); `CatalogueCarConfig` reproduces the stored garage config |
| events | `usa_gtmode_race.dat`: table 0 events (0x9C, settings block +0x44, prizes, prize cars, restrictions), table 1 opponents (0x60), table 2 car lists (0x80), name pool | parsed (`gt2tool events`) |
| licence tests | `usa_license_data.dat` | parsed earlier (`license.md`) |
| colour names | `.carcolor` + `.cclatain` | parsed |
| strings | `usa_unistrdb.dat` (WSDB, UTF-16) | parsed |
| menu pages | `gtmenu/usa/gtmenudat.dat/.idx` (3386 gzip "GM" pages), `gtmenu/commonpic.dat/.idx` (458 "GTMP" images), `iconimg.dat` (VRAM 704,0), `solodata.dat` | containers parsed, page records / GTMP not decoded |
| title / race texts | `data-title.txd`, `data-global.txd` (gzip inside ovl1), `data-gt.txd` (ovl4), `.text/data-race.txd` | located, not parsed |
| fonts | `font/racefont.dat` (parsed for the HUD), `arcade/gtmode_font.tim`, the menu font of 0x8001F2C8 / glyph table ovl4 0x800513BC / 0x80052102 | not parsed |

## 7. Port plan (native menus + career)

Principle: rules and state bit-exact (they decide money, cars and saves); drawing and page layout native (modern UI
over the same data, or a native renderer of the GM pages later). Verification against the oracle = the RAM of scripted
runs (`gt2run session` routes above) and the saves the game writes.

| # | Step | Port how | Verify | Size |
|---|---|---|---|---|
| 1 | `CareerState` C++ struct = the 0x7C9C bytes (static_asserts), load/save through `save_data` (done: parser, CRC, builder); new-game defaults | bit-exact | save-info on game saves; our saves loaded by the original (card image in gt2run) | 1 day |
| 2 | garage / money rules (5.2), catalogue config, record builder hook-up (CarConfig -> CarParams already ported) | bit-exact | done for purchase; add scripted runs: sell, buy parts, buy a new car; compare RAM | 2-3 days |
| 3 | minimal native UI shell: world map, maker page, new / used lists, car info, buy / sell, garage, home, game status | native (ImGui-like or own 2D over the existing HUD sprite path) | screenshots vs. oracle routes | 4-6 days |
| 4 | event flow: event list from `usa_gtmode_race.dat`, entry checks (5.3), race block build (0x80013108 / 0x80076F5C opponents), launch gt2game race with the player's garage car, results (5.4), day counter | bit-exact rules; the race itself already native | run one event in both, compare 0x801D585C block and post-race state | 4-5 days |
| 5 | licences screen: test list, medals, licence levels (records 0x1418), launch the existing licence races | bit-exact records | licence route | 1-2 days |
| 6 | tune shop: parts list per car (tables 0..28 rows, prices +4, kind order), prerequisites, setting screens (CarConfig settings ranges from the part rows) | bit-exact rules | scripted part purchases | 4-6 days |
| 7 | used-car rotation, car wash / wheel shop / machine test (sub-modes 7-9, records 0x3A88..) | bit-exact | routes | 2-3 days |
| 8 | title, options, save / load UI, replay theater | native | save round trip with the original | 2-3 days |
| 9 | menu graphics: decode GM pages + GTMP images (commonpic) to reuse the original layouts / art, text from unistrdb / txd | formats | render pages side by side with oracle screenshots | 5-8 days |
| 10 | arcade mode (Arcade disc: ovl2 / ovl3) | later | | 5+ days |

Total for a playable GT-mode career with native UI (steps 1-8): about 4-5 weeks of implementation work; with the original page
graphics (9): +1-2 weeks.

## 8. Port status (2026-09-19: plan steps 1, 2, 4 without UI)

Code: `src/game/career/` (namespace `gt2::career`, library `gt2career`): `career_state.*` (the block, new game, save
file / card image), `garage.*` (money, slots, parts, tune sheet, purchase), `results.*` (race results, licence level,
day), `events.*` (event info records, entry check, prize block, opponents, course). Verification:
`tools/gt2verify/verify_career.cpp` on `work/re/gtmode/ram.bin` (gt2run calltrace, route "buy the 2nd used car",
fields 3560..3640: GT-mode overlay loaded, one car) and on the race-overlay dumps (result appliers). Ghidra project
`gt2_gtmode` (875 functions, `work/re/gtmode/decomp`). Facts: `db/sim_us12_symbols.yaml` (career section).

Findings of the port (addresses = Sim v1.2):

- New game = EXE 0x800104A0 (not 0x800106A0): memset of bytes 0..0xB5, four 11-byte button tables (EXE 0x80091570,
  0x8009157C, 0x80091588, 0x80091594) at +10 / +21 / +32 / +43 and the 20-byte pad configuration (0x800A6ED8) at +0x48
  for pad 1, the same at +92.. / +0x9A for pad 2; options +0 = 1, +3 = 2, +4 = 1, +6 = 2, +8 = 1, +B1 / +B2 / +B5 = 1,
  +B3 = 240, +B4 = 192; both garages 0x80010798 (slots NOT cleared); course time records for the .crsinfo count
  (u16 0x801E18E6 = 126); the time record is {-1 x4, u16 0, u16 0xFFFF} (0x8005DD68 stores five -1 then clears the
  u16 at +0x10). The code is overwritten by the overlays; gt2verify restores it from SCUS_944.88 to run it.
- Save header (0x8006A038): "SC", 0x13, 4 blocks, title = EXE 0x80091CA8 in EUC-JP converted to Shift-JIS, CLUT
  0x80091CC4, icons 0x80091CE4. The header built natively equals the one of `work/memcards/gt2_save_1car.mcd`.
- Tune sheet (0x801DA4B8 for purchases / prize cars, 0x800B4490 for the current car of the menus): 0x17E8 bytes, layout
  in `garage.h TuneSheet`. 0x80015428 puts the car's own row of every part kind at the slot named by the row's stage
  byte (+8; lightweight +0x0B, racing modification +0x0E) and the upgrades of 0x80076570 at slots 1..n; the rear-tyre
  upgrades use the row index 0x80076570 returned for the FRONT table. 0x80016C5C copies the config and the stage bytes
  of its rows to s16 +0x17B0 + kind * 2. Slot bit 15 of a garage car = racing-modification stage of its own row > 0.
- 0x8005EAC0 kind 6 with a gearbox row whose gearAutoSet (+0x20) is set (88 of the 618 catalogue cars, and every
  fully customisable gearbox) regenerates the ratios through 0x8005E93C -> 0x80074B38 into an 8-entry STACK buffer of
  which only reverse..top gear are written; the caller copies all eight. Ported (2026-09-19, second wave below); the
  entries above the top gear are residue that the port cannot and need not reproduce (the gearbox residue, below).
- Buy part = 0x80017C98 (check 0x80017B40, bit + value 0x8005E8B0, money -= price without clamp). Fitting an owned
  part = 0x80017D6C (ported, below). Select car = 0x8001DFEC case 6 (current = index); buying with no current car
  makes the new car current (0x8001DDAC case 0).

### 8.1 Second wave (2026-09-19): parts, settings, championships, rolling start

Code: `src/game/career/tuning.*` (sheet record / figures, fit / remove parts, wheels, racing bodies, the settings
library), `src/game/career/championship.*` (series block, points, standings, championship end, race block name /
course fields), `src/game/sim/car_setup.cpp RollingStart` (0x8003311C, called by StartCar). Rows in
`tools/gt2verify/verify_career.cpp`, all 0 mismatches: GT-mode dump CarSheet 0x800173E8 (200), FitPart 0x80017D6C (600),
RaceTyres 0x80018004 (300), Wheels 0x80018100 (300), Bodies 0x80017530 / 0x800174F4 (200), GetSet 0x8005FC9C (1200),
SetSet 0x8005F9DC (1200), DfltSet 0x80060410 (1200), Series 0x80018A84 (400), Standings 0x8005E6B0 (400), AddPoints
0x8005E67C (200), EventName 0x8005E548 (200), Course 0x8005E590 (300); Purchase / BuyCar / Prizes now run all their
cases (the formerly skipped auto-set gearboxes included; Prizes now also series names); race dumps: RollStart 0x8003311C
(600), Slider 0x80054D10 (2000). The rows build realistic garages (catalogue cars bought through 0x80017750 and tuned
with random sheet stages).

- **0x800173E8 is not SetTuneConfig** (open question 4): it loads the menus' sheet 0x800B4490 for garage car (index,
  player) = 0x80015404 + 0x80015428(car id) + 0x80016C5C(car config). SetTuneConfig is 0x80016C5C.
- Fitting (0x80017D6C(index, kind, player, paint), menu transactions 0x8001DDAC case 2 = buy + fit for kinds 7, 9,
  0x20, 0x12..0x14, 0x22 and case 3 = fit): -4 not owned, -8 no row; racing modification (0x22) = sheet kind 19 at the
  body chosen on the page (s16 0x800B5C78, cycled by 0x80017530 among 0x8005F858 bodies), the slot's paint and model id
  become the body's; tyres 0x27..0x2D = front and rear stage kind - 0x26; other kinds through 0x8001706C (kind -> sheet
  kind and stage, see the symbol file). Then 0x80016F10 writes the sheet config back with the figures of 0x8005F958,
  which builds the record from the sheet's SLOTS (0x8005F410): the port builds the configuration whose row indices are
  the slots' index words (identical rows) - `SheetRowsConfig`. The fit uses the sheet as it is: the menus load it for the
  current car first (0x80017480 on car select).
- Removing parts: the GT-mode menus never go back to a lower stage except for dirt tyres: 0x80018004 (from 0x80013108
  before every event race) fits part 0x2D for a dirt event (event rules bit 0 = row +0x75; such events race ONE car,
  sub-mode 10) when owned, else puts fitted dirt tyres (stage 7) back to stage 0. The race overlay's settings pages
  offer stage 0 or an owned stage per kind (ovl0 0x80052D84 case 8); the port exposes that as `ChangePartStage` /
  `RemovePart` (0x8005EAC0 + 0x80016F10).
- Table 29 (CarConfig +0x38) is the WHEELS: the wheel shop 0x80018100 (index, wheel id, price, player) finds the row of
  the id (0x80077E80 binary search), 0x80021B38 sets config +0x38 = row and +0x00 = row u32 | (row[7] & 0x1F) << 8
  (row[6] when the u32 is 0 and a racing modification is fitted), writes the slot, money -= price.
- **Settings screens are in the race overlay** (the pre-race "machine settings"), on the sheet 0x8016E894, but the
  rules are the EXE library: 0x8005FC9C get (entries {value, min, max, field}; which settings exist depends on the
  selected stages: suspension 1 camber / dampers, 2 ride height, 3 springs / toe / anti-roll / separate rebound; brake
  controller -> brake balance; gearbox stage 3 -> gear ratios with ranges from the auto-set snapshot (0x8005E99C,
  0x80074E04) and the auto-set top speed; LSD rows -> the entries whose row min < max; ASM / TCS; racing modification
  -> downforce), 0x8005F9DC set, 0x80060410 default. The action codes 0x95 / 0x96 of the GT-mode menus are NOT settings:
  0x8001A530 cycles the displayed paint (wraps with the paint count 0x8001A504; 0x8001A454 returns its character).
  The +-1 of a setting is the race overlay's slider 0x80054D10 (value +- 1, +- 10 with L1/R1 held, clamped to the end
  it moves towards). Commit 0x80056FF0 (race overlay) writes the sheet to the race record, the race slot and the
  garage slot with the builder's write-backs (engine word, exhaust byte, flags) - the port's `StoreSettings` does the
  garage part (not verified as a whole: 0x80056FF0 needs a settings-screen dump).
- Championships: the series is named by the event name: `XXXnnrr` = series base `XXXnn`, nn = race count (digits of
  the base's last two characters, 0x80018A00), rr = race number; nn < 2 = a single event. 0x80018A84 builds the block
  0x801D5DF4 (race names "%s%02d", course ids, laps from row +0x45; "none" courses random from ovl4 0x80050C4C).
  Prizes (0x80018C8C) come from race 1's row; the per-race payout 0x80059704 needs the player's finish (0x801D5E88 >
  0); points by position = race overlay 0x8002F4CC (8, 6, 4, 3, 2, 1) into +0x8E, summed by 0x8005E67C; after the last
  race 0x80017A28 sorts (0x8005E6B0), stores place + 1 in the result entry and runs 0x80059800 for a champion (bonus,
  random prize car, career +0x215 = 1 for the "GTW" series = 0x801D5DDC set by 0x80013108 from 0x8001859C). Series of
  the US disc: GT30501..05, GT50501..05, GTW0501..05, FREECHAMP0501..05. Machine tests G400 / G1000 / GMAX (0x8001861C)
  never build a series. Race block: +0x10 event name (0x8005E548), +0x20 course display name and +0x40 course id
  (0x8005E590 / 0x80060EB4), +0x57C result index, +0x580 GTW flag.
- Rolling start 0x8003311C (settings +0 km/h; 0x80010078 clears race block +0x0D = 0x801D5869 for it, which also
  disables the start hold): see the symbol file; 52 of the 60 licence tests and the endurance / GT300 / GT500 events (80 km/h)
  start rolling.
- For the menus (also verified, GT-mode dump): 0x8001DB90 parts-page figures (price 0x80017B04, owned, power before =
  0x800771AC of the sheet's configuration, after = 0x80017F18 -> 0x8005F044 preview with the part's stage; tyre kinds
  unchanged; quirks: the auto-set gearbox preview generates from the current selection, the turbo preview applies the
  LSD stage-0 row settings) - PartPower 600 cases; wheel codes 0x80013A28 ("mmNNN-kc", maker pairs ovl4 0x80050904) -
  WheelId 400; wheel colour byte 0x80021BEC - WheelCol 300; licence-test entry 0x80019B88 (0x800190E4 prefix of ovl4
  0x80050D04; licence L < 5 needs L + 1; messages ovl4 0x80051164) - LicEntry 300.
- **The gearbox residue (task: 0x8005E93C stack garbage)**: running 0x80017750 in the interpreter with the stack below
  the harness filled first with 0x00 and then with 0xAA changes exactly the ratio entries above the top gear of the 88
  auto-set catalogue cars and nothing else: those words are not written by the routine's own call tree (0x80017750's
  earlier callees 0x80015428 / 0x80016C5C stay above that depth; 0x80076F2C / 0x80077D5C leave their argument slots
  there unwritten), so in the game they are whatever deeper calls of the menus (drawing) left there - not
  reproducible without emulating the menus' whole call history. They are unobservable in play: the only reader of
  CarConfig +0x3C.. is the record builder (0x80077A6C copies all eight into the record), and the record's ratios are
  read only up to the gear count (0x800347C4, the settings screens 0x8005FC9C / 0x8005F9DC loop 1..gearCount, the race
  start's auto-set 0x80034740 is off because the purchase sets config flags bit 7). They reach only the save / replay
  bytes. The port keeps the gearbox row's values there; gt2verify masks exactly those entries (`GearResidueFixup`) and
  reports the cases where the mask changed a byte.
- The mapping interpreter 0x80077634 uses only operation 4 (copy of size x count bytes through BIOS A(27h) bcopy)
  in the tables 0x80092888..0x8009294C / 0x80092C24.
- Event info records (0x80019474): built from the menu's list of 248 names (ovl4 0x80050D1C); the rule word ORs full
  bytes. The entry check reads the current car's tune sheet 0x800B4490: +0x54 (turbo boost 0 = naturally aspirated,
  rule +9A), stage of kind 19 > 0 (racing modified, rule +9B 1 / 2), all stages of kinds 0..2, 6..22 < 1 (stock, rule
  +9B 3). Special series: "PFL" / "EPL" events need result 1 in every event of ovl4 0x80050BB4, "GTW" of 0x80050BF8.
- Opponents (0x80010A30 / 0x80010714): six random picks among the event's used slots with the generator 0x80083AE0
  seeded from the VSync counter 0x801F0680 (0x8007D23C(0)); hidden cars (language) re-picked; a repeat of an earlier
  car / opponent number re-picked when (rand & 0x1F) < 0x1D while a budget of 64 lasts; paint = the slot's 6-bit code
  or a random paint of the car's list, re-drawn once when the chip colour's saturation ((max - min) * 32 / max of the
  5-bit channels) is <= 5; opponents whose racing-modification row has a stage race its body (row +8) with a new
  paint (0x80010984). The player replaces slot 0 afterwards (0x80011000: model id, paint, config, flags |= 0x40). The
  course of events named "none" is a random one of ovl4 0x80050C4C (0x8001907C) from a fresh copy of the same seed.
  The slot's name is strcpy'd from .carinfoa entry + 3 x paints + 1, i.e. with the 0x7F padding bytes before the name.
  An event whose opponent cars are all hidden in the language makes the pick loop re-draw forever (BVN0001 on the US
  disc; the port throws instead).
- The race file (usa_gtmode_race.dat) is reloaded to 0x80024430 before each race build: the menus use that memory
  for GPU packets, so a menu dump does not hold it (gt2verify puts it back, relocated like 0x80076AE8 does).
- Result appliers: 0x80059A7C / 0x80059704 verified with the UI calls (0x800595E0 results screen, 0x800481C8 view)
  replaced by nop; 0x80059800 ported as rules only (it loads the prize screen from the CD).
- Day counter: +1 in 0x80013628 for events (race path) and licences; machine tests (0x8001861C) do not advance it
  there.

### 8.2 Races from the menus (2026-09-19): one window, licence results, settings commit

gt2game now runs title -> GT-mode menus -> race -> back in ONE window with one career in memory (code:
`tools/gt2game/game_window.*`, `race_view.*`, `career_race.*`, `panel.*`, `settings_screen.*`, `menu_mode.cpp
RunMenuSession`, `title_mode.cpp`; `src/gt2view/panel_view.*`). What the original does around a race, as far as it was
observed (gt2run sessions `work/re/menu_lic3`, `work/re/lic_pass*`, B-1 route of section 8 + brake at field 6675):

- The race overlay (member 0, loaded at f2744 with entry 0x80011F64) shows the licence test's own menu (Replay, Start,
  Records, Save Replay, Demonstration, Exit; `field_008100.png`) before and after a test. A pass: the "BRONZE PRIZE!"
  result waits for a button, then the replay runs; Start in the replay = pause "Continue / Exit"; Exit -> the licence
  menu; its Exit -> ovl4 again (f8176, entry 0x80013628). During a race, Start = pause "Continue / Exit"; Exit = back to
  that menu, the race abandoned (no result). The native flow keeps this structure with our own panels: pre-race panel
  (Start / Retry, Machine Settings for events, Exit), Esc = pause (Continue / Exit -> pre-race panel), the shell's
  results wait (0x8002A700) needs X = Enter, then a result panel, then licence -> pre-race panel (retry), event -> back
  to the menus. (2026-09-19, later: the race overlay's own screens are decoded and drawn - licence test menu, event menu
  "SINGLE RACE", CHANGE PARTS / PARTS SETTING, pause menu, race-end display with the licence prize; see
  `docs/formats/race_screens.md`. Still not decoded: replay theater, records / name entry, demonstration, the prize money
  screen 0x800595E0 and the championship end screen; an event race of the original was never finished in the
  interpreter, so the event / championship race-end display is ported from code only.)
- (2026-09-19, later) The race overlay's own state machine is decoded (race_screens.md 5.5): after a race the replay plays at
  once, a licence result is applied after it (0x8004E104), the pause's Exit of an event race leaves the overlay; the menus' Replay,
  Save Replay, Demonstration, Test Run rows are ported; GT events put the player on grid slot 5 (ovl4 0x80012F7C: entry i on 5 - i)
  and run in game mode 2 (0x8001710C) - gt2game had the player on pole and game mode 0 (no catch-up, no mirror, the mode-0 HUD).
- Licence result into the career = race overlay 0x8004DD80 (the medal byte +1 of the test record, the fourth-prize
  counter +2; `db/sim_us12_symbols.yaml`), run by the menu state 0x8004E104 after the result when the result code
  0x801D5DEC is 1 (pass flag 0x8005B3D0). Found with `gt2run session ... watch=801CCD00:668` (licence B records): the
  only write in the whole pass -> replay -> exit sequence was +1 = 2 at f8857 by pc 0x8004DEE0. Port
  `career::RecordLicenceResult` (results.h), gt2verify row LicRecord (1200 cases on work/re/license_race, 0 mismatches).
  The race block names the record: +0x0B licence, +0x0C test, written by ovl4 0x80010078 from the test name.
  Not ported: the all-gold prize car (0x8004DF04 adds prize block car 0; the prize block of a licence test is not
  traced), the best-time table with name entry (0x8005DE8C / 0x8005DEFC).
- Day counter: +1 when a race is entered from the menus (0x80013628), also for licence tests; retries inside the race
  overlay do not add days.
- 0x80056FF0 (settings commit) is now ported as a whole (`career::CommitSettings`) and verified without a settings-screen
  dump: gt2verify StoreSet runs the routine on work/re/license_race with a real garage car's sheet at 0x8016E894, random
  settings and every garage selector / game mode (400 cases, 0 mismatches). The native settings panel uses 0x800173E8,
  0x8005FC9C, 0x80054D10 (step), 0x8005F9DC, 0x80060410 and 0x80056FF0's garage part; the race car's record is then
  rebuilt with the race's builder (0x800771AC) from the committed configuration.

- (2026-09-19, later) The TRANSMISSION bar (AT / MT) before the licence menu's Start and the event menu's Test Run / Start Race, the
  event menu's "Exit?" bar, the licence menu's test selector and the menus' view transitions (SAVE REPLAY sliding in / out) are ported
  (race_screens.md 5.6). The choice is career + 0x7C8E (garage + 0x401A, `GarageBlock::byte401A`: saved with the career, the bar opens
  on it) and race slot 0 + 0x8F; the event menu skips the bar for a garage car whose + 0x98 bit 14 is set (a gearbox of fewer than 3
  gears). The races' sponsor category is the event / licence row's tag (sponsor_boards.md).

### 8.3 Licence cars, race options, fitted wheels, the licence prize car (2026-09-19, third pass)

- Licence tests: the race car of every test is built as the original's builders 0x80010078 / ovl0 0x8004C7A0 do
  (table 31 row -> 0x80076FC0, racing-modification body, paint code) - `docs/formats/license.md` section 5 (rows LicSpec,
  LicBuild: all 60 tests equal the original's slot / configuration / record). The test row's car slot 1 (six tests) is
  read by no code: those tests race one car (B-7 run in the interpreter: `work/re/lic2_b7`, frames 574 / 0 differ). All
  60 tests start and run headless; the IB-1 / S-1 "no stock row" failure was the port's stock-row rule.
- Race options of the career reach the races of the menus: `RunMenuRace` gives the race view the option bytes of the
  career it runs on (+0xAE replay info, +0xAF camera position, +0xB0 chase view, +0xB1 course map, +0xB2 view angle,
  +0xB3 / +0xB4 volumes; `shell::ReadGameOptions`), as the race overlay reads them from the career block, instead of the
  settings file the process started with (a change on the title's OPTIONS page was lost until a restart). Checked: a
  career with camera position 1 / course map off / view angle 2 (and no settings file) races B-1 from the licence page
  with "camera: position 1, view angle 2" (chase camera in the frame).
- Fitted wheels in the car view: 0x8001AC20 (garage car, flag page +433, word = CarConfig +0x00) and 0x8001AEF8 (the
  wheel shop's preview) load a carwheel/ TIM with 0x800615E8: word &= 0xFFFFE0FF, nothing when 0; file = the boot's
  table (0x8001194C: carwheel/ entries in VOL order, ids by 0x80011570 with the maker pairs of EXE 0x80033DD0; u32
  0x801E30F0, count s16 0x801C93B4, first file u16 0x801E2FC6) searched by 0x80060D74 (not found -> the first file).
  0x800678E8(buffer, slot, 1): image over the top-left 48 x 48 texels of the car texture ((slot & 15) * 64, (slot & 16)
  * 16), CLUT to (slot * 64, 224 + ..) = the rims' CLUT (model +0x40, 0x8006101C) - separate from the paint CLUTs;
  0x8006155C(model, 0, CLUT entry 0) sets model +0x08 (its reader was not traced; not reproduced). The preview also
  sets the dish: 0x80061504 defaults, then both axles' dish = s16 0x80091A70[colour byte] (unmasked), 0x80061308.
  Port: `menu::LoadMenuWheelFiles / MenuWheelFile / MenuWheelDish / LoadMenuWheelTexture` (menu_car.*),
  `gt2view::MenuCarView::SetWheels`, `SceneAssets::SetCarWheelTexture` (rim vertices use CLUT column 16 of the paint
  rows: CLUT 0 of the paint normally, the wheel's CLUT when fitted). gt2verify WheelFile (GT-mode dump, 971 cases): the
  dump's boot table equals the native one and 0x80060D74 equals `MenuWheelFile` for every id, ids with dish codes,
  neighbours and random words. The frame was checked by eye only (the wheel shop preview of bb009-5s on the Protege,
  `work/lic2/wb190_crop.png`); no capture of the original's wheel shop exists. The race overlay loads fitted wheels
  too (ovl0 0x80028F5C -> 0x800615E8 / 0x800678E8): wired 2026-09-19 (race_view.cpp, every car of the race). The race
  load's loop (car i, texture slot 26 - i): model 0x8005D950, paint 0x80061634 -> the paint's CLUT set model + 0x20 + paint *
  0x240; CarConfig +0x00 -> 0x800615E8 loads the TIM, its 32-byte CLUT is copied over the paint's CLUT 0 (memcpy 0x8008CFE0),
  0x800678E8(buffer, slot, 0) the image. Checked against the original: the CBM0001 race of `work\memcards\gt2_save_1car.mcd`
  with the Protege's +0x00 set to the id of carwheel/en013-5s.tim (`work\play\wheels\card_wheel.mcd`, route
  `work\re\ev_route.txt`, gt2play --ai-player --prims 5800): the TIM's image is at VRAM (640, 256) = the top-left 48 x 48 texels
  of slot 26 and its CLUT at (640, 480) = the car's CLUT 0, which only the 32 rim polygons of the frame use (clut 0x7828); the
  body uses CLUTs 1..15. gt2game's race loads the same file for car 0 (`wheels: car 0 (a2bsn) fitted with
  carwheel/en013-5s.tim`) into the scene's rim CLUT column and the slot's texels (`work\play\wheels\ours_1700.png` against the
  stock wheels `stock_1700.png`, a replay built from the capture's race block).
- The licence prize car: 0x80013864 prepares the prize block with 0x80018C8C for a licence name from the licence's
  first test row (+0x84 = the all-gold prize car); 0x8004DF04 adds prize car 0 when the licence becomes all gold
  (`license.md` section 7; `career_race.cpp`). Not compared with the original.

## 9. Open questions

- (Answered 2026-09-19, docs/formats/gt_menu.md) GM page record layout, item types / actions, GTMP and page pictures,
  menu fonts; the renderer matches the original frame pixel for pixel. Open there: the popup list widget.
- (Answered 2026-09-19, section 10.3) Options block bytes: the option ids and their bytes; 0x801D5860 = career +5..+8
  (2P car damage, 2P laps, handicap, slow car boost), 0x801D585C = +1..+4.
- Course record 0x24 / licence record / machine-test record inner fields beyond the time-record initialisation.
- (Answered 2026-09-19, section 8.1) 0x8005E93C residue, fitting parts, 0x800173E8, series, race block name / course.
  Still open there: (0x80056FF0 as a whole: answered 2026-09-19, section 8.2), the rally one-car
  race of dirt events (0x80019BF0 loads a file into the colour buffer for it; ghost?), the race overlay's other
  pre-race pages (change car, tyres). (Answered 2026-09-19: 0x801D5866 = 2 is written by the race overlay's "Start
  Race" (0x800178B0 -> 0x8001710C) for every event race, single or series; docs/formats/race_screens.md section 3.)
- 0x801C9AF5 (+0x215) meaning, career +0x48, +0x15D; "COMPLETE %" of the status screen (not traced).
- (Partly answered 2026-09-19, section 8.2: licence record +1 = prize 1..4, +2 = fourth-prize count, +4.. five best
  times, +0x68.. five 12-byte names; the event pre-race menu and the race-end display: docs/formats/race_screens.md.)
  Answered 2026-09-19 with captures of whole races (the original's AI driving the player's car, race_screens.md
  section 6): the prize money view 0x800595E0 and the RESULTS / post-race views (section 5.2 there), championship races
  run with sub-mode 2 and show Results / Points / Total Points, the pre-race menu of a series is titled "SESSION 1" ..
  (GT300 capture), the name entry and records table (5.3). The champion's end view (0x80059800) - answered 2026-09-19:
  captured with the GT300 series won (FTO LM), it follows the last race's post-race menu "Continue" and leaves to the
  GT-mode menus (race_screens.md 5.4). Still open: the replay screens, what Exit in the middle of a championship does
  to the series. (Answered 2026-09-19, section 8.3: the prize block of a licence test = 0x80018C8C of its first test.)
- (Answered 2026-09-19, section 10.4) Card manager: the state graph of save / load.
- (Answered 2026-09-19, section 10.1) The first boot auto-loads "BASCUS-94455GAME" from memory card 1 (view 0x8004B900).

## 10. Title overlay (member 1): title, options, save / load (2026-09-19)

Evidence: our disassembly / Ghidra pseudo-C of `work\re\title\ram.bin` (gt2run calltrace, route
`1200:down,1260:down,1320:cross,1480:cross,1520:down,1540:right`, fields 1500..1560: OPTIONS, the global page being
edited; Ghidra project `gt2_title`, 563 functions in `work\re\title\decomp`), session runs `work\re\title\s_*` (screens
every 20 fields, RAM snaps), GP0 captures `work\play\title\*.txt` (`gt2play --prims`). Pictures, fonts, strings and the
widgets' formats: `docs/formats/title.md`. Port: `src/game/shell/*` (namespace `gt2::shell`), `src/gt2formats/title_assets.*`,
`src/gt2view/title_view.*`, `tools/gt2game/title_mode.*`, rows in `tools/gt2verify/verify_title.cpp`.

### 10.1 Views and state machine

Every screen is a "view" object {init, update, draw, header colour +0x0C, header title +0x10} run by the EXE view loop
0x800833E8 on the manager *0x800A8D68 (view stack +0x1CC.., 0x800122A0 pushes a view: the options / save / load entries
are two views, a 16 / 20-field delay 0x80019118 / 0x80012F30 / 0x800130B0 followed by the real one; views slide in / out
over 16 fields, 0x800120D4). The entry 0x80011384:

```
0x80011178 (uploads) ; first boot (0x801EF5F0 == 0): logo / auto-load view 0x8004B900 {0x80016BD8, 0x80016C28, 0x80016F6C}
0x8001636C (txd copies) ; 0x800117B4 (pad configurations out) ; 0x800111DC (uploads) ; 0x801C99DC / DE = 0
0x801EF5F2 == 2 -> replay theater ; != 0 -> 0x8005DA7C(1, 0x8001172C) (restart without the logo)
loop: title view 0x8004BC78 {0x8001792C, 0x80017984, 0x80017C00} -> 0x801EF5F3:
  0 Start Game     -> 0x801EF5F1 = 4, 0x801EF5F2 = 3, 0x8005DA3C(4) (GT mode)
  1 Replay Theater -> 0x801EF5F1 = 2, 0x801EF5F2 = 2, view 0x8004B2CC (music 0) ...
  2 Options        -> view 0x8004BFD4 -> 0x8004C07C (music 6), then 0x800117B4
  3 Save Game      -> view 0x8004B4C4 -> 0x8004B518: EXE 0x8007284C + 0x80072F9C(0) (music 7)
  4 Load Game      -> view 0x8004B56C -> 0x8004B5C0: EXE 0x8007284C + 0x80073010(0) (music 7)
  5 Data Transfer  -> view 0x8004C4A0 -> 0x8004C548 (GT1 save "BASCUS-94194GT": trade cars, mix records, convert; music 7)
  6 (900 idle fields) attract demo: 0x800104E0 builds the race block of a demo event (names DON0001.. at 0x80020F2B), ovl0 arg 1
```
Title view (0x80017984): 16 fields of delay (0x800B122E) then the list opens with reveal period -1 (0x8004BC44: every row
at once); background brightness 0x8004BC00 +1 per field to 12; any held pad bit resets the idle counter 0x800B1228, more
than 900 idle fields close the list with result 6; moves play sound 6, a choice sound 3, closes the list and after 17
fields of fade (0x8004BC02) the view returns 4; triangle / square do nothing on the title; 0x801EF601 = start held at the
choice. Rows 0 and 7 are blank (docs/formats/title.md section 5).

First-boot view (0x80016C28): slot 0 status 0 and 0x8007D3E8(0, "BASCUS-94455GAME") -> read (0x8006A1B4) with "Loading
Save Data..." (0x801B9934) + progress bar (object 0x8004B82C: (96, 320)); CRC (0x8006A314) ok -> 0x8006A278 and "Auto
Loading Complete" (0x801B9956) for 180 fields, mismatch -> "Save Data is Corrupt!" (0x801B998D) 240 fields, read error ->
"Auto Loading Failed" (0x801B9971) 240 fields (a face button 0xF00 skips); no card / no file -> nothing. Only slot 1 is tried.

### 10.2 Options (view 0x8004C07C)

A page switcher (0x8004BFB8: centre (176, 130), width 200, 5 pages; 0x8001D4B0 / 0x8001D4D4 / 0x8001D4E8 / 0x8001D4F4 /
0x8001D754 / 0x8001D7C0; callback 0x800186E4): left / right (pressed | repeat) change the page with a 12-field slide and
sound 7, cross / circle (on page 4 read from pad 2) enter the page, triangle / square leave the options (sound 4, 16-field
fade); arrows 0x8006BA48 blink while the page is not edited. Pages (titles 0x8004BFA4): 0 GLOBAL OPTIONS, 1 RACE OPTIONS
(list widgets 0x8004BDDC / 0x8004BF14, rows 0x8004BD3C (8) / 0x8004BE88 (7), row callback 0x80018574, row draw 0x8001805C,
per-row y offsets 0x8004BE14 / 0x8004BF4C, section bands 0x8004BE34 / BE50 / BE6C / BF6C / BF88), 2 KEY CONFIGURATION
(0x800B12D0: 0x8001A7E0.., edits the 11-byte button tables), 3 / 4 1P / 2P ANALOG SETTINGS (0x800B12D8 / 0x800B1358:
0x8001C48C.., steering calibration; entering needs 0x8001C690 == 1). While a list page is edited: up / down move (sound 6),
left / right (pressed | repeat) step the selected row, L1 / R1 held step slider / lap rows every field, cross / circle /
triangle / square leave editing (sound 1); a value changed by left / right plays sound 5.

### 10.3 Option bytes (career block 0x801C98E0; 0x80017D74 get, 0x80017E68 set, 0x80017F2C step)

| Id | Row | Byte | Values (new game, 0x800104A0) | Readers |
|---|---|---|---|---|
| 0 | arcade Car Damage | +0x02 | 0 / 1 (stored value != 0) (0) | race block 0x801D585C + 1 (the GT setups copy +1..+4 there) |
| 1 | arcade Race Laps | +0x03 | 1..99, "1Lap" / "%dLap" (2) | 0x801D585E |
| 2 | 2P Tire Damage | +0x04 | None / Slow / Fast (1) | 0x801D585F |
| 3 | 2P Car Damage | +0x05 | 0 / 1 (0) | 0x801D5860 |
| 4 | 2P Race Laps | +0x06 | 1..99 (2) | 0x801D5861 |
| 5 | Handicap Start | +0x07 | s8 -100..100, "1P -%dm" / "2P -%dm" of 10 x value (0) | 0x801D5862 |
| 6 | Slow Car Boost | +0x08 | s8 None / Slow / Fast (1) | 0x801D5863 |
| 7 | Replay Info | +0xAE | None / Level1 / Level2 (0) | race camera object +0x107 in replays (ovl0 0x80010000; sub-mode 3 forces 2) |
| 8 | Camera Position | +0xAF | Driver / Chase1 / Chase2 (0) | race camera object +0x10E (ovl0 0x80010000) |
| 9 | Chase View | +0xB0 | Type1 / Type2 (0) | 0x8003E8E4 view yaw constants 0x80046C94 / 0x80046C9C (sim::RaceSim viewMode; Type2 follows the velocity) |
| 10 | Course Map | +0xB1 | Off / On (1) | HUD 0x80029064 draws the map only when set |
| 11 | View Angle | +0xB2 | Narrow / Standard / Wide (1) | ovl0 0x80010298 / 0x80010608: projection distance s16 0x8002F370[angle] = 277 / 216 / 190 |
| 12 | Music Volume | +0xB3 | 0..254 (240) | 0x80080F24: CD volume (v * 0x8000 / 0xFF) * 0x8CC >> 12 at each track start |
| 13 | SFX Volume | +0xB4 | 0..254 (192) | car sound 0x8001826C, 0x800785A8, the menu sounds' master (0x800784A0) |
| 14 | Vibration | +0x36, +0x88 | On / Off; stored (value != 1) in both pads' blocks (0 = On) | 0x800133F0 feeds the actuators only when the pad block's +0x2C == 0 |

Step rules (0x80017F2C): choice rows value + left / right; volume rows round up to a multiple of 16 and move by 16
(clamped to 0..254); slider and lap rows value + left / right + L1 / R1; always clamped to [min, max - 1] of the row, returns
"changed". The pad blocks are 0x52 bytes at +0x0A / +0x5C: four 11-byte button tables, the vibration byte at +0x2C, the
20-byte analog configuration at +0x3E (copied out by 0x800117B4 / 0x8006A348). There is no units option: the US build
shows mph only (`--kmh` and the PC SETTINGS page are ours).

### 10.4 Memory-card manager (EXE)

Save = 0x80072F9C (the file image built once when the screen opens: 0x8006A038 header, 0x8006A214 block + CRC, mode 4),
load = 0x80073010 (mode 5); per field 0x800728F0 updates all button bars with the pad, then the state handler (table
0x800921D4, h(1, 0)); a handler's result switches state (0x80072494; the entry h(0, 0) may chain at once). States of modes
4 / 5 (strings of data-global.txd, the preselected button in brackets):

| State | Shows | Next |
|---|---|---|
| 0 | - | 25 fields -> 2 |
| 2 | "Select a Slot", bar "Memory Card Slot" Slot1 / Slot2 (cursor kept) | slot -> 3; back -> exit |
| 3 | "Checking Slot..." + header band "Memory Card N" | save: no card 4, not formatted 5, read error 1, file present 0x1D, free blocks < 4 -> 9, else 0x20; load: no card 4, error 1, file 0x22, else (no file, unformatted) 0x24; busy: stay |
| 4 | "No Memory Card Detected", bar Change Slot / Exit [Change Slot] | Change Slot 2, Exit / back exit, card inserted 3 |
| 5 | "Memory Card is not Formatted" / "Do You Want to Format?", Yes / No [No] | Yes 6, No / back 2 |
| 6 | "Formatting..." / "Please Do Not Remove Memory Card" | done 0x1F (create), failed -> "Formatting Failed" 1 |
| 9 | "Not Enough Empty Blocks", bar ERROR! Change Slot / Exit | 2 / exit |
| 0x1D | "Game File Already Exists" / "Overwrite File?", "Do You Want to Overwrite?" Yes / No [No] | Yes 0x1E (write in place), No / back 2 |
| 0x20 | "Start Saving?", "Save?" Yes / No [No] | Yes 0x1F, No / back 2 |
| 0x1F | "Saving..." (create "BASCUS-94455GAME", 4 blocks) | 0x1E; failure 0x21 |
| 0x1E | "Saving..." + progress (colour 0x000C5090), write 0x7F00 bytes | done 0x10; failure 0x21 |
| 0x10 | "Saving Complete", bar OK Change Slot / Exit [Exit] | 2 / exit |
| 0x21 | "Saving Data Failed", ERROR! Change Slot / Exit | 2 / exit (back ignored) |
| 1 | the error text (+0x24) at (176, 234), ERROR! Change Slot / Exit | 2 / exit |
| 0x22 | "Start Loading?", "Load?" Yes / No [No] | Yes 0x23, No / back 2 |
| 0x23 | "Loading..." + progress (colour 0x0090500C), read, CRC | ok -> 0x8006A278 -> 0x25; CRC mismatch "Loading Failed" 1; read error "Loading Data Failed" 1 |
| 0x25 | "Loading Complete", OK Change Slot / Exit [Exit] | 2 / exit |
| 0x24 | "No Game File Found", ERROR! Change Slot / Exit | 2 / exit; no card / error 3 |

One save per card (the fixed name "BASCUS-94455GAME", 4 blocks = (0x7EA0 + 0x1FFF) >> 13; free blocks are checked only for a
new file; an existing file is overwritten in place). Loading replaces the whole saved block (options and pad
configurations included) only after the CRC matched. Every exit returns 0x27 (the title gets no success flag). Card status
0x800A8D64 comes from the slot state by 0x8006EB64 (0 ready, 1 busy, 2 no card, 3 not formatted: sector 0 without "MC", 4
error); free blocks = 15 minus the chains of 0x51 entries (0x800826C8); the file search walks the 15 entries (0x80082814).
Unverified by a run (code only): the error paths (states 1, 5, 6, 9, 0x21, 0x24); "Failed to Create Game File" is stored but
never drawn; "Creating File..." / "Retry" / "Try Saving Again?" are not reachable in modes 4 / 5.

### 10.5 Port (src/game/shell)

| Original | Port | Verified (gt2verify on work\re\title\ram.bin) |
|---|---|---|
| 0x80017D74 / 0x80017E68 / 0x80017F2C | `OptionValue`, `SetOptionValue`, `StepOption` | OptGet 4000, OptSet 4000, OptStep 6000 cases, 0 mismatches |
| 0x80018574 command 3 | `OptionRowInput` | OptInput 4000, 0 |
| 0x8001792C + 0x80017984 (+ 0x8001779C, 0x800163C8, the list widget) | `TitleMenu::Reset / Update` | TitleSeq 60 sequences, 20518 fields, 0 |
| 0x8006A214 / 0x8006A314 / 0x8006A278 + 0x8006A348 / 0x8006A038 | career_state `BuildCareerSaveFile`, `LoadCareerSaveFile().CrcOk()`, `BuildSaveHeader` | SavePack / SaveCheck / SaveUnpack 60 each, SaveHead 1, 0 |
| 0x80016410, 0x80017C00, 0x8001191C, 0x8001805C, 0x800186E4 (draw), 0x8001D7C0, 0x8006BEF4, 0x8006E5B8, 0x8006C5DC, 0x8006C174 | frames (`title_draw`, `title_screens`) | pixel comparisons (10.6) |
| 0x8001D4F4 page switcher, 0x800186E4 (update), the card manager's states, the first-boot view | `OptionsScreen`, `CardManager`, gt2game title mode | scripted flows (10.6) |

Not ported (a page or message says so, nothing is faked): the key configuration and analog settings pages (they show "Not
available yet"; entering them behaves like the original's analog page without an analog controller), Replay Theater, Data
Transfer, the attract demo after 900 idle fields (the title restarts), the view slide transitions and the text objects'
reveal animation (the settled screens are drawn), card slot 2 unless `--card2` names a file, the card's own timing (our
transfer shows 8 sectors per field).

### 10.6 Verification of the native frames

The software canvas of the native primitives (with the interpreter GPU's rules, which made the captures) against the
captured VRAM, 352 x 480:

| Screen | Capture (gt2play --script, --prims field) | Native (gt2game --title-script, shot field) | Differing pixels |
|---|---|---|---|
| Title, Start Game | none, 1150 | none, 60 | 0 |
| Title, Save Game selected | `1200:down,1260:down,1320:down`, 1370 | `20:down,40:down,60:down`, 110 | 0 |
| GLOBAL OPTIONS | `1200:down,1260:down,1320:cross`, 1450 | `20:down,40:down,60:cross`, 122 (arrow phase) | 0 |
| GLOBAL OPTIONS, Chase View being edited | + `1480:cross,1520:down`, 1560 | + `220:cross,260:down`, 296..304 | 0 |
| RACE OPTIONS | + `1500:right`, 1550 | + `240:right`, 310 | 0 |
| SAVE GAME, Select a Slot | `...,1320:down,1380:cross`, 1500 | `...,80:cross`, 220 | 0 |
| SAVE GAME, Start Saving? (empty card) | + `1560:cross`, 1700 | + `150:cross`, 295 (fill flash phase) | 0 |
| LOAD GAME, Start Loading? | `...,1380:down,1440:cross,1600:cross`, 1760 | `...,80:down,100:cross,170:cross`, 320..340 | 0 |
| SAVE GAME, Saving Complete | + `1800:left,1860:cross`, 2400 | 370..395 | 5813: the reveal animation (ghosts) of the original's bar texts |
| LOAD GAME, Loading Complete | + `1800:left,1860:cross`, 2300 | 380 | 165: the "OK" text object's reveal |

The title's unselected rows differ from the capture by 1212 / 1860 pixels under the PS1 sprite rules of MenuCanvas (5-bit
modulation and subtractive blending); the interpreter GPU modulates and blends in 8 bits, and with its rules the frames are
equal. The Vulkan view draws polygons as triangles (float gouraud) and differs from the canvas in the gradients' pixels.

Saves: a native save of a new-game career on a formatted empty card is byte-identical (all 131072 bytes of the card image)
to the card the original wrote through the same Save Game flow in gt2play (`work\play\title\card1_after_save.mcd`); the
native Load Game of that card replaced a one-car career with the new game (money 10000, 0 cars).
