# Title overlay screens: pictures, fonts, strings, widgets

Status 2026-09-19. US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Code addresses are
in GT2.OVL member 1 ("ovl1", the title, loaded at 0x80010000) unless marked EXE. Evidence: our disassembly / Ghidra pseudo-C
of `work\re\title\ram.bin` (gt2run calltrace at the OPTIONS screen, 563 functions in `work\re\title\decomp`), session runs
`work\re\title\s_*` and the GP0 captures `work\play\title\*.txt` (`gt2play --prims`). Code: `src/gt2formats/title_assets.*`
(assets), `src/game/shell/*` (rules and frames), `src/gt2view/title_view.*` (Vulkan). The behaviour (state machines, option
bytes, card manager) is in `docs/research/menus_gtmode.md` section 10.

## 1. Screen

352 x 480, one drawing area at VRAM (0, 0) (E3 0,0 / E4 351,479); the view manager clears it every frame (E1 0x200 +
TILE 0x60 black 352 x 480, 0x800121A8) and uploads 32 x 480 at (352, 0) (not drawn). The console shows the 352-pixel
line across the full 4:3 picture.

## 2. VRAM (uploads of member 1)

File ids index the EXE table 0x801E2EF0 (u16 -> VOL TOC record, filled at boot); 0x8005D8A0 / 0x8005D8D4 read by id.

| Uploader | File id -> VOL path (US disc) | Destination |
|---|---|---|
| 0x80011178 | 0x16 `arcade/arc_key_config.tim` | tpage 0x1D (0x800110FC: the image block, after the CLUT block when present, own w / h) |
| 0x80011178 | 0x0D `arcade/arc_font.tim` | tpage 0x1E (the text font, 2 pages wide; CLUTs inside the image rows, 0x4038..) |
| 0x800111DC | 0x80023E7C[language] = 0x45 `arcade/topmenu_panels_us.tim` | tpage 0x0E |
| 0x800111DC | 0x3E `arcade/title_item.tim(.gz)` | tpage 0x0C: the title list's item names (CLUTs 0x3E37, 0x3E70..0x3E74 inside the image) |
| 0x800111DC | 0x80023EA8[language] = 0x3D `arcade/title_gtmode_us.tim(.gz)` | CLUT block -> (384, 511), image block (8-bit, 352 x 480) -> (384, 0) |
| 0x80011BC4 | 0x0E `arcade/arc_fontinfo` | RAM 0x800E15C0: u32 8, then 8 offsets = 4 fonts {glyph table, kerning table} |

## 3. Fonts

`arcade/arc_fontinfo` holds four fonts in the format of the EXE text engine (0x8007DD3C glyph, 0x8007DC78 advance;
`gt2formats/hud_assets.h` HudFont documents the glyph / kerning tables). Descriptors {u32 glyph, u32 kerning, u8 cell,
u8 gap} set by 0x80011BC4: 0x801B95F0 = offsets +4 / +8, cell 12 (headers); 0x801B9620 = +0x0C / +0x10, cell 7 (section
titles, card messages); 0x801B95C0 = +0x14 / +0x18, cell 5 (option rows, bar labels); 0x801B95D0 = +0x1C / +0x20, cell 3.
Texture page 0x1E (0x8006AC68(ctx, 0x1E) -> 0x8007DC40), CLUT base 0x4038; a glyph's page offset (sprite word bits 29..31)
selects 0x1E / 0x1F. Glyph packets: SPRT (colour bit 25 = semi-transparent) then E1 = page | context mode << 5 (the
context's flags bits 21..22, 1 = additive for the menus' texts).

Text layouts: 0x8006AC90 (left, spacing), 0x8006ADB4 (centred: x - width / 2), 0x8006AE28 (right), 0x8006AF40 /
0x8006B184 (fixed digit cells). The button bars use a "text object" (0x8006C460 init, 0x8006C5DC draw, 0x8006CD04 width):
per character a space or a digit takes the font cell (digits right-aligned in the middle of cell + extra), other characters
the engine's advance; each character adds the object's extra spacing (template EXE 0x80091EC8 +0x0A = 2 for bar titles,
the bar template's +0x22 = 1 for the labels); colour = colour * alpha >> 8 (title alpha 0x80, labels the bar's fade level).

## 4. Strings

`data-title.txd` (gzip member at ovl1 0x80021104, 25585 bytes) and `data-global.txd` (gzip at 0x80022D80, 13727 bytes):
NUL-terminated strings, one block per language. 0x80016254 copies block `language * 0xE47` (0xE47 bytes) of data-title to
RAM 0x801B9630; 0x8001DA08 copies block `language * 0x7A9` of data-global to 0x801EF6B0. The code refers to strings by
those RAM addresses (e.g. 0x801B9AB6 "OPTIONS", 0x801BA44D "SAVE GAME", 0x801EF709 "Select a Slot").
`gt2::TitleAssets::Text(address)` returns them from the disc.

## 5. Title list (view 0x8004BC78)

The EXE list widget (`gt2formats/gt_menu_list.h`) with the object ovl1 0x8004BC28: 8 rows, flags 0x24 (wrap, arrows without
blinking), 3 visible, row height 24 + gap 4, arrows 6 / 10, centre (176, 274), fade 16. Row callback 0x8001779C; rows with
a negative result (table 0x8004BC04 = {-1, 0, 1, 2, 3, 4, 5, -1}) draw nothing and are disabled; 0x8006D4B0(w, 1, 6) clamps
the selection. Item sprites: 12-byte entries {u8 u, u8 v, u16 clut, u16 w, u16 h, u16 tpage, pad} of the table
*(0x8004BC5C + language * 4) (US: 0x8004BA50), index 0x8004BC14[row]. Row template 0x8004BC24 {flags 0x0F, brightness 0x80,
s16 reveal width 32}. Row draw 0x80016410: selected = SPRT (colour * alpha >> 7) mode 2, black SPRT mode 0, SPRT mode 1;
others = SPRT (>> 7) mode 2 ... with alpha = draw alpha * widget fade / 16 >> 9 (a quarter): colour >> 8 subtractive then
>> 7 additive (the captured 0x101010 / 0x202020). Background 0x80017C00: four SPRTs of the 8-bit picture (tpages 0x86,
0x96, 0x88, 0x98; CLUT 0x7FD8) at brightness fade * 128 / 12, then E1 0x280.

## 6. Headers, bands, bars, progress (EXE / ovl1 helpers)

- View header 0x8001191C (view table +0x0C colour, +0x10 title): the title in the header font, letter spacing
  0x22 - alpha / 4, centred on 352 at baseline 90 in grey 0x66 * alpha / 128 with a black copy at (+3, +3), a red TILE
  underline (x + 1, 92, width, 2) of 0xF2 * alpha / 128, a POLY_G4 gradient (0, 48 -+ 48 * alpha / 128) .. (352, ...)
  from the colour to black, E1 0x220. Colours: OPTIONS 0x6ED23C, SAVE GAME 0x0000F2, LOAD GAME 0xF20000, REPLAY THEATER
  0xD67890, DATA TRANSFER 0xD6142C.
- Band 0x8006BEF4 (0x1C bytes: s16 w, h, steps, flags; u32 c0, c1, target, targetOut; s16 anim; tick 0x8006BE64, alpha
  0x8006BEB4 = anim * 128 / steps): POLY_G4 c0 (left) -> c1 (right), widened by w * (steps - anim) / steps on both sides
  and blended toward `target` while it grows in.
- Button bar (EXE templates 0x80091F04.. of 0x30 bytes: x, y, title / label strings, title / label / fill / gradient
  colours, flags (bits 1..2 size: 4 = 96 x 24), label spacing +0x22, sound +0x2C; state 0x98 bytes; init 0x8006E1CC,
  open 0x8006E388, close 0x8006E3FC, update 0x8006E43C, draw 0x8006E5B8): two buttons left / right of x, outlines
  (0x7F grey * fade), POLY_G4 gradient from the template colour (third OT slot), the selected button's TILE in the fill
  colour flashing toward white after a move (second slot), texts in the first slot.
- Progress bar (0x8006C04C / 0x8006C074 / 0x8006C174): 32 TILEs 4 x 24 at x + 5 i; segment i lights when
  done^2 / total >= i * total / 32 and fades from 0xD4D4D4 to the colour over 24 fields; unlit = colour / 4.

## 7. Verification

`gt2game <disc> --window 352x480 --title-square --title-script ... --title-shot N out.png --title-compare cap.vram.bin`
renders the native frame (Vulkan) and the same primitives on the software canvas and compares both with a `gt2play --prims`
VRAM dump. With the interpreter GPU's sprite rules (the captures' own rasteriser) the canvas equals the capture pixel for
pixel on: the title (Start Game and Save Game selected), GLOBAL OPTIONS, RACE OPTIONS, GLOBAL OPTIONS while editing, SAVE
GAME slot select, "Start Saving?" prompt, "Start Loading?" prompt (section 10.6 of menus_gtmode.md lists the routes).

## 8. The arcade disc's title (US Arcade v1.1, 2026-09-19)

Member 1 of the arcade GT2.OVL runs the same title list: its tables sit 0xB2C lower (results 0x8004B0D8, sprite index 0x8004B0E8,
widget 0x8004B0FC; `shell::TitleMenu` reads them through the build profile), the uploads are the same files except the background
`arcade/title_arcade_us.tim` (`TitleAssets::LoadArcade`). The native title frame equals the capture (`work\play\arcade_menu\cap\title_1200`,
native field 60) at 0 differing pixels with the interpreter's rules.

Texts (arcade, SCUS_944.55 SHA-1 231f9dba..., member 1 SHA-1 20bb63ff...): data-title.txd is the gzip at member 1 0x80020D88 (24514
bytes, 7 blocks of 0xDAE; 0x800161B4 copies block language * 0xDAE to 0x801B9330), data-global.txd the gzip at 0x8002247C (13321
bytes, 7 x 0x76F; 0x8001D674 -> 0x801EF0E0). The strings sit at other offsets than in the Simulation blocks (and differ in places:
the handicap value is "%dm" without the minus sign). `TitleAssets::LoadArcade` loads both blocks at the arcade addresses and
`Text()` translates a Simulation string address through the build profile (the reference runs of the lui-built string references
plus 16 string facts of `db\arcade_us11_symbols.yaml` for strings the code builds from a base register or reads from a table).
`TitleAssets::SimLayoutScreens()` gives the screens that name member 1's / the EXE's tables by Simulation address (options, card
manager, key / analog pages) images at the Simulation addresses: the arcade bytes copied through the profile, member 1's pointers
into member 1 and both images' string pointers translated (string pointers become build-text tokens, address + 0x10000000).
Frames (native canvas with the interpreter's rules against `gt2play --prims` captures of the original arcade, `work\play\arcade_title`):
GLOBAL OPTIONS, editing, RACE OPTIONS, KEY CONFIGURATION (digital pad), 1P / 2P ANALOG SETTINGS, title with Save Game selected,
SAVE GAME slot select and "Start Saving?", LOAD GAME slot select and "Start Loading?", the first-boot "Auto Loading Complete":
0 differing pixels; "Saving Complete" 5813 and "Loading Complete" 165 (the bar texts' reveal animation, as on the Simulation disc).
Details: docs/research/arcade_disc.md section 17.8.

## 9. Replay Theater screens (2026-09-19)

Behaviour and file format: `docs/formats/replay.md` section 9. Port: `src/game/shell/title_replay.*` (ReplayTheaterMenu,
DemonstrationScreen, the row printer DrawReplayRow), `title_screens.*` (CardManager mode 1 = LOAD REPLAY), gt2game
`title_mode.cpp`. Captures: `work\play\theater\cap` (gt2play --prims, a card with the replay the original saved).

- REPLAY THEATER (view 0x8004B374): header 0xD67890 "REPLAY THEATER" (0x801B99B4); list 0x8004B16C {4 rows, flags 0x0C (wrap +
  the widget's own highlight bar 0x8006B814, 288 wide), 4 visible, row height 63 + 8, arrows 6 / 10, centre (176, 140), fade
  12}; rows 0x80016410 with the template 0x8004B168 {flags 0x0E, brightness 0x80, reveal 64} and the sprites 0x8004B104 (256 x 63
  panels of topmenu_panels_us.tim in tpages 0x0E / 0x0F, CLUTs 0x3F38..); the rows are drawn with alpha 0x80 whatever the
  selection (0x800125D0 command 4 calls 0x80016410(row, ot, 0)); 20 fields after the view starts the list opens. Moves: sound 5,
  choice 3, back 4.
- DEMONSTRATION (0x8004B224): header 0x1428DE; the list 0x8004B1D8 (flags 0x0C, 3 visible, row 0x54 + 4, centre (176, 140), fade
  12) over the demo file's entries after 24 fields (sound 7); rows 0x80012B84 = the row printer with the style 0x8004B20C.
- LOAD REPLAY (0x8004B3C8, header 0xD63B54 "LOAD REPLAY" 0x801B99C7): the card manager's frame (0x80072B78: lines, "Memory
  Card N" band, bars) with, after the bars at mgr + 0x50..0x218, E1 0x20 and the list 0x80091FE4 (rows 0x8006F060, drawn into the
  list's OT slot + 1: behind the arrows and the highlight).
- Row printer 0x8006A4E4 (kind 0): title (medium font, colour 0x02565656) at (x - 0x80, y - 10); title band 0x8006B77C (x - 0x80,
  y - 0x12, 256 x 8, 0x0214377A -> 0); E1 0x200; rules (y - 6, y + 0xE, y + 0x26; 256 x 1, 0x028E8452); E1 0x200; "%d sector"
  right-aligned to x + 0x7C at y + 0x26 (0x02381A10, digit cells); kind at (x - 0x7A, y + 0xE), course at (x - 0x7A, y + 0x26), car
  at (x - 0x20, y + 0xE): 0x8006A3FC = the text in digit cells at + 5 (colour 0x02102E4C) cut by 0x8006AE98 to 0x48 / 0xAC / 0x94
  pixels ("..." 0x8008FA74), a 4 x 6 TILE marker at (x, y - 12), E1 0x220; last the row background 0x8006B77C (x - 0x90, y - 0x2A,
  0x120 x 0x54, 0x5A4A3E -> 0x171310) and E1 0x220. Every colour fades with 0x80 - alpha; the row slides in from x + (0x80 -
  fade ratio) * 11 / 8.
- The bars of the card manager pulse every 60 fields (0x8006E43C wraps the bar's counter 0x47 -> 12: the fill flashes toward grey
  over 15 fields) and their title text objects shine in the same rhythm (template 0x80091EC8: +6 period 60, +8 20 steps, +1 two
  characters per step, colour -> +0x18 white); the port draws neither (as the text reveal, section 10.6 of menus_gtmode.md), so
  the frames below are captures outside the pulse; LOAD REPLAY's list rows show the list highlight's blink phase, which depends
  on when the list opened (our card transfer is faster than the original's).

Frames (the canvas of the native primitives with the interpreter GPU's rules against the capture's VRAM, 352 x 480; native
script / shot vs gt2play script / capture field):

| Screen | Native (`--title-script`, shot) | Capture | Differing pixels |
|---|---|---|---|
| REPLAY THEATER, Load Replay selected | `20:down,80:cross`, 200 / 210 / 220 | `1200:down,1260:cross`, 1390 | 0 |
| REPLAY THEATER, Demonstration selected (highlight blink) | + `220:down,250:down,280:down`, 305 | + `1400:down,1430:down,1460:down`, 1480 | 0 |
| DEMONSTRATION list (Demo 01 selected) | + `320:cross`, 525 | + `1500:cross`, 1700 | 0 |
| DEMONSTRATION, Demo 02 selected | + `540:down`, 551 / 625 | + `1720:down`, 1726 / 1800 | 0 / 0 |
| DEMONSTRATION, Demo 05 selected (list scrolled) | + `670:down,720:down,770:down`, 870 | + `1850:down,1900:down,1950:down`, 2050 | 0 |
| LOAD REPLAY, Select a Slot | `20:down,80:cross,220:cross`, 410 | `1200:down,1260:cross,1400:cross`, 1595 | 0 |
| LOAD REPLAY, the list (B-1 replay: "License", "S-255", "17 sector") | + `470:cross`, 659..689 | + `1650:cross`, 1880 | 0 |
| LOAD REPLAY, No Replay Files Found (card without a replay file) | + `470:cross`, 600 / 618..660 | + `1650:cross`, 1810 | 0 |
| LOAD REPLAY, No Memory Card Detected (slot 2 empty) | + `420:right,470:cross`, 600 / 618..660 | + `1600:right,1650:cross`, 1810 | 0 |

The native frames of the theater run 5 fields behind the capture's field minus 1180 (the pad script's timing), the card manager's
after its transfer. Not compared: "Checking Replay File" / "Loading..." with a partly lit progress bar (our card rate), the view
slides. The Vulkan view differs in the gradients (float gouraud), as the other title screens.

### 9.1 RENAME & DELETE (2026-09-19)

View 0x8004B41C "RENAME & DELETE" (header 0x288DC0, 0x801B99F0; init 0x800129C4: 0x8007284C with the object 0x8004B1A0 - fonts
+0xC 0x801B95D0 tiny, +0x10 0x801B95C0 small, +0x14 0x801B9620 medium, page 0x1E - and EXE 0x80072F20 = card manager mode 2; update
0x80012A20: the manager's exit -> sound 4, back to the theater; draw 0x80012A80 = 0x80072B78). The states and drawing: replay.md
section 9.5. gt2game: title -> Replay Theater -> Rename & Delete (`shell::CardManager::kRenameReplay` with the keyboard of
`gt2view/race_record_screens.h MakeTitleCardKeyboard`, the EXE keyboard widget on the descriptor 0x800921A0; keys: Q / W = L1 / R1,
held together = the delete mode, S = Start).

Frames (the canvas with the interpreter GPU's rules against gt2play --prims VRAM, 352 x 480; the card `work/play/cards/card_r3.mcd`:
the original's B-1 replay, gt2game's seattle record and "Demo 01"; gt2play script `1200:down,1260:cross,1400:down,1440:cross,
1650:cross,1850:down,2000:cross,2150:cross,2200:start,2230:cross,2350:l1:150,2350:r1:150,2420:cross,2550:triangle,2650:cross,
2750:down,2780:down,2820:cross`, native the same presses 1180 fields earlier; captures `work/play/cards/rd_cap`, `rd_cap2`):

| Screen | Capture | Native | Differing pixels |
|---|---|---|---|
| the list, row 0 (sector bar, totals, "Press L1 + R1 ...") | rd_1760 | 540 | 0 |
| the list, row 1 selected | rd_1900 | 707..725 | 0 |
| the keyboard on "Short B1" / after a character | rd_2100 / rd_2170 | 924 / 995 | 0 / 0 |
| the list after the rename | rd_2300 | 1123 | 0 |
| delete mode (L1 + R1 held: the rows' colour 0x324052, "- OK -" disabled) | rd_2400 | 1225 | 0 |
| the list after a delete (2 replays, "- OK -") | rd_2470 | 1295 | 0 |
| "Changes are not Saved" / "Cancel?" | rd_2600 | 1405 | 0 |
| the list again / the keyboard on an empty title | rd_2720 / rd_2800 / rd_2900 | 1532 / 1625 / 1725 | 0 / 0 / 0 |
| rename + delete + OK: the list, "Saving Complete", the theater after Exit | rs_2240 .. rs_2520, rs_2700, rs_2850 | 1062 .. 1332, 1505, 1675 | 0 |

Not compared: "Select a Slot" during the bars' pulse (rd_1600, 668 at best: the pulse, title.md 9) and "Saving..." with a partly
lit progress bar (our card rate). The card after the second run (rename "Short B100", delete "Demo 01", OK, Exit) is byte-identical
to the card the original wrote through the same presses (all 131072 bytes: `rd_nat2/card_native.mcd` = `rd_cap2/card.mcd`).

### 9.2 COPY REPLAY (2026-09-19)

View 0x8004B470 over member 1's copy screen (behaviour: replay.md section 9.7; port `src/game/shell/title_copy.*`). Captures:
`gt2play --original --card <copy of work/play/cards/card_r3.mcd> --card2 <copy of work/play/copy/blank.mcd>` (the interpreter's
second card port), script `1200:down,1260:cross,1400:down,1430:down,1460:cross,1760:cross,2000:cross,2250:cross,2300:down,
2330:cross,2400:down,2430:down,2460:cross,2900:cross` (work/play/copy/cap); native `gt2game <Sim disc> --window 352x480
--title-square --card .. --card2 ..` with the presses before the list 1180 fields earlier and from the list on 1261 fields earlier
(our card reads are faster; the list's open field aligned from its blink / state counters in the capture's RAM); the scan scripts
`work/play/copy/scan*.sh`.

| Screen | Capture | Native | Differing pixels |
|---|---|---|---|
| Select No. of Blocks on Memory Card 2 (arrow blink phase) | c_1700 | 525 | 0 |
| Copy Replay Start / Exit, "While Copying Data" | c_1900 | 700..716 | 0 |
| the list opened (no entry chosen) | c_2140 | 885 | 0 |
| "License" chosen ("COPY" mark, totals, sector bar) | c_2270 | 1015 | 0 |
| both chosen, "Short B1" selected | c_2350 | 1095 | 0 |
| "- OK -" selected, "Confirm" (list scrolled) | c_2450 | 1194 | 0 |
| Copy Complete (Restart / Exit) | c_2700 | 1440..1450 | 0 |
| back in the main state after Restart | c_2950 | 1690..1700 | 0 |
| no card 2: "Cannot Detect Memory Card 2", Start refused (red fill) | capa a_1576 / a_1592 / a_1640 | 392.. / 402.. / 452.. | 0 |
| card 2 with a replay file: the list (115/149 free), after two downs | capb b_1850 / b_1960 | 596 / 704..707 | 0 |
| copying "Demo 01" into it: the list, Copy Complete | capd d_2010 / d_2500 | 755 / 1236..1250 | 0 |

Frames inside the bars' 60-field pulse differ by the pulse / title shine (not ported, section 9; e.g. a_1560: 2414 pixels).

## 10. Data Transfer (2026-09-19)

Reachable on the Simulation disc: title list row 5 -> view 0x8004C4A0 (20 empty fields, CD track 7) -> 0x8004C548 "DATA
TRANSFER" (header colour 0xD6142C, text 0x801B9CCF; list 0x8004C3B4 = the theater's list layout with the row template
0x8004C3B0 and the panel sprites 0x8004B134: TRADE, MIX RECORDS, CONVERT); the rows open the views of 0x8004C3A4. Everything runs
in GT2.OVL member 1 (EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Port: `src/game/shell/title_transfer.*` (rules +
TransferManager + TradeScreen), the menu = ReplayTheaterMenu::TransferLayout() (`title_replay.*`), gt2game `title_mode.cpp`.

- Member 1's card manager (object 0x800B1588; init 0x80020788 + 0x80020828(mode), per field 0x80020868, draw 0x80020A3C, states
  0x8004C878) reuses the EXE card manager's bars with member 1's templates (+0x50 error 0x8004C718, +0xE8 slot 0x8004C748 with
  Slot 2 preselected, +0x180 no card 0x8004C778, +0x218 complete 0x8004C7A8, +0x348 no cars 0x8004C7D8, +0x2B0 "Start Loading?"
  0x8004C808; band 0x8004C6FC). States: 0 (25 fields) -> 2 "Select a Slot" -> 3 "Checking Slot..." -> 6 / 10 "Start Loading?" ->
  7 (GT2 save "BASCUS-94455GAME", 0x7F00 bytes, CRC 0x8006A314) / 11 (GT1 save "BASCUS-94194GT", 0xA000 bytes, 0x8001FC7C) ->
  mode 0: the TRADE list (no car: 9 "Cannot Find Cars in Game File"); modes 1 / 2: 8 "Combining Records Complete" / "Converting
  Data Complete"; 5 "No Game File Found" / "Cannot Find Gran Turismo Game Data"; 4 no card; 1 an error text; 0xD exit.
- TRADE (mode 0, view 0x8004C59C, header 0xD68C9A) then the list view 0x8004C5F0 (init 0x8001F084, update 0x8001F11C, draw
  0x8001F358): the loaded save's garage in the list 0x8004C43C (9 visible rows of 0x18 + 3; rows 0x8001EE24 / 0x8001EBD4: paint
  chip, the .carinfoa name 0x80060AE8, value "1,234" 0x8001E808, a 256 x 24 panel of title_item.tim), the chosen row's
  availability 0x8001EAD8 ("Available" 0x606060, "Garage is Full", "Not Enough Money" 0x142864), our money "Cr." and "N/100 cars"
  over the bands 0x8004C404 / 0x8004C420. An available car opens the "Buy ?" bar (0x800B14F0, template 0x8004C470, No
  preselected); Yes = 0x8001EB60 (the car appended with 0x8005E7F0, its value +0x90 paid). The loaded save is not changed.
- MIX RECORDS (mode 1, view 0x8004C644, 0x90B4D6): 0x8001E550 on success = course records 0x8001E014 (the first 128; the loaded
  record replaces ours when ours is empty or slower), licence times 0x8001E284 (ranked by 0x8001E11C, stored with 0x8005DEFC;
  prize bytes untouched), machine tests 0x8001E38C / 0x8001E408 / 0x8001E484 through EXE 0x8005E0D0 (sorted, at most 8, one
  entry per car, 0-400 m / 0-1000 m smaller is better, max speed larger).
- CONVERT (mode 2, view 0x8004C698, 0x7878D6): the GT1 file's data = file + 0x200; 0x8001FBFC(data, size) = CRC-16 (0x1021, init
  0x3770, msb first) low half + running sum (0xAAAA, (sum + b) ^ (b << 8)) high half; 0x8001FC7C: the sum over 0x6BA4 bytes equals
  the u32 behind them. 0x8001E61C: GT1's B licence (8 bytes at +0x2B5C all non-zero) -> every test of GT2's B licence
  (licences[5]) whose +1 byte is 0 gets 1; only then GT1's A (+0x2B64) -> GT2's A (licences[4]). Checked on the original: after
  CONVERT of our synthetic GT1 card the prize bytes of licences 4 and 5 read 1 in RAM.
- Rules verification: gt2verify rows DtCourse, DtLicRank, DtLicence, DtMachine, DtMachMix, DtGt1Sum, DtGt1Ok, DtConvert, DtAvail,
  DtBuy (`tools/gt2verify/verify_title_transfer.cpp`, title dumps with member 1 loaded), 0 mismatches.
- Not ported: the bars' pulse and text shine (as in section 9), the card transfer rate (ours reads 8 sectors per field).

Frames (as section 9; captures in `work\play\transfer\cap`, gt2play base script `1200:down,1230:down,1260:down,1290:down,
1320:down,1380:cross,...`; native base `20:down,50:down,80:down,110:down,140:down,200:cross,...`; test cards
`work\play\transfer\card_*.mcd`, the GT1 card synthetic):

| Screen | Native shot | Capture | Differing pixels |
|---|---|---|---|
| DATA TRANSFER menu | 345 | menu_1525 | 0 |
| TRADE, Select a Slot | 560..590 | slot_1750 | 0 |
| TRADE, Start Loading? | (scan) | start_1900 | 0 |
| TRADE list (loaded save's garage) | 880..900 | list_2575 | 0 |
| TRADE, Buy ? bar | 910..985 | buy_2700 | 0 |
| TRADE list after a purchase (money and car count updated) | 1089 | bought_2920 | 0 |
| MIX RECORDS, Combining Records Complete | (scan) | mix_2900 | 0 |
| CONVERT, Converting Data Complete | (scan) | conv_3100 | 0 |
| CONVERT, Cannot Find Gran Turismo Game Data | (scan) | nogt1_1900 | 0 |

The TRADE list's highlight sweeps (four semi gouraud quads moving across the row), so only one native field in a period
matches a given capture.

## 11. The attract demo (title result 6; both discs, 2026-09-19)

US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a) and US Arcade v1.1 (SCUS_944.55, SHA-1
231f9dba7191b9ef915621662afdc40a7c66df95; member 1 SHA-1 20bb63ff...). Evidence: objdump of member 1 / member 0 / the EXE, `gt2run
session` runs of the originals without input (`work\attract\sim_idle`, `arc_idle`, `arc_idle2`: calls of the gather / overlay
switch / member 1 entry), `gt2verify --race-capture` of the attract (`work\attract\sim_attract.bin`, `arc_attract.bin`), `gt2play
--prims` of demo frames (`work\attract\prims`). Port: `src/game/shell/title_attract.*` (the cycle, the demo file), gt2game
`tools/gt2game/title_attract.*` (the replay), `title_mode.cpp` (Simulation title case 6), `arcade_mode.cpp` (arcade title case 6).
Facts: `db\sim_us12_symbols.yaml` (0x80011624, 0x800A9500), `db\arcade_us11_symbols.yaml` (0x8001156C).

- **Trigger.** The title list view ends with result 6 after 900 fields without a held button (0x80017AC8 / arcade 0x80017734
  `slti 901`). Measured: the original's title view 0x8004BC78 runs 918 updates (0x80017984 from f643 to f1559 of the idle boot)
  before case 6; `TitleMenu` returns result 6 after 918 updates as well.
- **Simulation, case 6 = 0x80011624.** 0x801EF5F1 = 0x801EF5F2 = 0; 0x80020DCC reads the language's demo file (member 1 table
  0x8004C8A8: US language 1 -> id 0x25 -> file table 0x801E2EF0 -> VOL record 0x2D `arcade/demofile_us.gmr`) to 0x800E15C0 and
  returns its replay count; 0x80020E14 gathers replay *0x801EF5FE to 0x801055C0; 0x801EF5FE = (index + 1) mod count; 0x80010EDC
  (settings, cars) and 0x8005DA7C(0, 0x80011F64, 1) - the race overlay with argument 1, the Replay Theater's launch. No movie.
- **Arcade, case 6 = 0x8001156C.** First 0x801EF030 = (+ 1) mod 4; 0 -> 0x8005D9AC(5): member 5 plays the intro (movie 24) and
  enters member 1 again; the overlay switch 0x8005D9EC does not return, so the demo index stays. Otherwise the Simulation
  sequence with the arcade addresses: table 0x8004BD7C (language 1 -> id 0x1E -> file table 0x801E2950 -> VOL record 0x26, the
  same `arcade/demofile_us.gmr`: byte-identical to the Simulation disc's), file 0x800E12C0, gather 0x80020A98 to 0x801052C0,
  index 0x801EF02E, 0x80010EDC, 0x8005D9EC(0, 0x80011F64, 1). The boot block 0x8001083C clears the index and the counter.
- **Order.** The US demo file holds 7 replays: Demo 01 (Seattle, mode 2, Shelby GT350), 02 (Time Trial / Rally record, mode 6,
  Impreza Rally Car, `no_name_dirt`), 03 (Super Speedway GT50502, mode 2, rolling start), 04 (Rome-Night, mode 4), 05 (mode 6,
  Lancer Evo.V Rally Car, `pikes`), 06 (Laguna Seca, mode 4), 07 (Clubman Stage Route 5, mode 4). Simulation: 01, 02, 03, ...,
  07, 01, ... Arcade: 01, 02, 03, intro, 04, 05, 06, intro, 07, 01, 02, intro, ... Observed (no input): Simulation title f547,
  demo 01 gathered f1567, member 1 again f18150, demo 02 f19147, member 1 f25511, demo 03 f26508; Arcade intro to f9844, title
  f9914, demo 01 f10930, 02 f28504, 03 f35860, member 1 f48042, member 5 (intro) f49030, title f58468, demo 04 f59459, 05 f69540.
- **The demo.** A replay like the theater's: the race block's race with the slots' cars (0x80010EDC), the trackside replay
  cameras, the replay HUD (lap block, "Replay" + the car name, the START / REPLAY caption); a mode 6 record replays player 1's
  lap ring (ghost_replay.h). It ends when the stream runs out (0x800A8D68) - member 1 comes back with the title list.
- **Start ends it.** The race overlay keeps its argument in the race task's byte 0x800A9500 (entry 0x80011F64, `sb s4` at
  0x800121AC). In the frame 0x80015B64 a pressed Start (generic 0x10000, 0x80015C4C) reaches 0x80015C90: argument != 0 -> the frame
  returns 0 (the race ends and the overlay switches to member 1 at 0x8001222C), argument 0 -> the pause (+0x2E9). So every
  title-launched replay (attract AND Replay Theater) ends on Start, without a pause menu. Observed: Start at f3000 of demo 01 ->
  member 1 at f3146; Cross at f3000 does nothing. The exit fade (section 11.1): from the frame after Start no scene is drawn; every field one RECT 320 x 240 of the colour word 0x080810 (R 16, G 8, B 8), semi-transparent with blend 2 (B - F), goes into the buffer displayed next, alternately, for 69 fields; then 0x8005DA3C(1) at f3076.
- **gt2game.** `TitleAttractDemos` keeps the index / counter for the session; the replay runs through `PlayTitleReplay` (race or
  mode 6 record) in the same window; Start (pad) / Esc ends it at once; then the title list restarts. The arcade intro cycles play
  the intro movie (`--no-movies`: nothing, the title restarts). The exit fade: section 11.1. Not ported: the CD / overlay load times between the title and the race (the original
  starts demo 01's race at f1955, 388 fields after the gather).
- **Verification.** Simulation frames (`gt2verify --race-capture <Sim> 62000 sim_attract.bin`, split per race, against `gt2game
  <Sim> --replay demo#N --frames-compare`): demo 01 7147 frames, 02 (mode 6) 2984, 03 5623, 04 3986, 05 (mode 6) 3121, 06 1999 (the
  capture's end) - 0 differ (car bodies byte for byte + player 1's stream; mode 6: car 0, lap ring, snapshot / playback, clock).
  Demo 03 first differed in 2 bytes (body + 0x658, the dirtiness 0x0ADF): the race block's + 0x58 dirt level (19) is now carried
  into player 1's slot (`RaceData::dirtLevel`, 0x80012CD4). Arcade (`arc_attract2.bin`, to field 76000): demo 01 7147, 02 (mode 6) 2984,
  03 5623, 04 3986, 05 (mode 6) 3106 (to the capture's end) - 0 differ. HUD (`gt2play --prims`, "# hud-ours" from the original's inputs of the frame, all present in
  the frame's draw list): demo 02 f21000 (mode 6) 36 / 36, demo 03 f30000 78 / 78, demo 04 f42000 (Rome-Night, mode 4) 57 / 57 (the harness gives our HUD no car name, so the capture's car-name line is not compared; ours lists "Replay" before the lap block, the original after the car name - the texts do not overlap). gt2game's run of the title (`--title --fast
  --no-sound`) plays the demos in the original's order (log lines "attract demo 'Demo NN'"; arcade: the intro every fourth).

### 11.1 The exit fade (both exits of a title-launched replay; 2026-09-19)

Captures (US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): `gt2play <Sim disc> --original --script
3000:start --prims F ...` for F = 2990, 3000 .. 3080 every 8 fields (work/play/fade), and the same without input around the end of
demo 01's stream, F = 17900 .. 18148 (work/play/fade/end).
- After Start at f3000 the scene is still drawn up to f3007 (4 flips); from f3008 every field holds exactly: E1 0x200, the drawing
  area of the buffer not displayed (E3 0 / E4 0x03BD3F or E3 0x03C000 / E4 0x077D3F), E1 0x040 (blend 2), RECT 0x62 colour
  0x080810 (0, 0) 320 x 240, then GP1 05 displays that buffer. 69 such fields (f3008 .. f3076), then nothing (the overlay switch).
  At the end of the stream the same: RECTs in f18012 .. f18080 (69), then member 1. So both exits of a replay the title launched
  (the attract demo and the Replay Theater: argument 1 of the race overlay) fade the same way.
- Model: every RECT subtracts (R 2, G 1, B 1) 5-bit units from its buffer, clamped at 0; the displayed buffer j fields into the
  fade (j = 0 .. 68) has had (j + 1) / 2 RECTs. Checked on all pixels of both buffers of the capture's VRAM: from the f3008 dump
  every later dump (f3016 .. f3072) = the model, 0 of 1228800 pixels differ (both buffers hold the same last frame).
- US Arcade v1.1 (SCUS_944.55 SHA-1 231f9dba...): the same (`gt2play <Arcade disc> --original --script 12000:start`, work/play/fade/arc:
  RECTs 0x080810 in f12008 .. f12076, 69 fields; the model from the f12008 dump: 0 of 1228800 pixels differ).
- gt2game (`tools/gt2game/title_attract.* PlayTitleExitFade`, `RaceViewConfig::exitFade`, set by the attract and the theater):
  the last presented frame stays and a full-window quad of min(255, m x (16, 8, 8)) with the renderer's blend 2 is drawn over it,
  m = (j + 1) / 2, for 69 fields after Start / the stream's end; then the title (or the theater) as before. Checked on our own
  frames (`gt2game <Sim disc> --title --no-sound --window 640x480 --script 1600:esc --shot-at ...`, `work/play/fade/check_fade.ps1`):
  every fade frame (f1600 .. f1668) = the first one minus m x (16, 8, 8) per channel, clamped: 0 differing pixels; f1669 is the
  title's first (black) frame. Deviations: the original draws the scene for ~7 more fields after Start before the fade (ours fades
  from the next field); our frame is the native 8-bit picture, not the 15-bit one.

## 12. The arcade disc's Replay Theater and Data Transfer (US Arcade v1.1, 2026-09-19)

US Arcade v1.1 (SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95, GT2.OVL member 1 SHA-1 20bb63ff...). Evidence: a
normalised instruction diff of member 1 against the Simulation's (objdump of `work\ovl\arcade_us11\ovl1.bin` / `work\ovl\sim_us12\ovl1.bin`,
immediates and addresses masked, difflib alignment), gt2run sessions (`work\play\arc_theater\sess1`, `sess2`) and gt2play `--prims`
captures of the original arcade (`work\play\arc_theater\cap1`, `cap2`, `rd`, `rs`, `copy`, `trA`, `trM`, `trC`).

- **The same code.** The title's jump table slot 1 (0x800115E8, Simulation 0x8001167C) and slot 5 (view 0x8004B974, Simulation
  0x8004C4A0) run member 1's theater views, the Copy Replay screen, the Demonstration list and the DATA TRANSFER views / manager / TRADE
  list with the Simulation's instructions: Simulation 0x80011648..0x800150A0 = arcade 0x800115B4.. (-0x94), 0x800150D4..0x80016284 =
  0x80015034.. (-0xA0), 0x8001DB10..0x800210A0 = 0x8001D794.. (-0x37C: the transfer code); the differing immediates are string / data
  address halves and one file id of the boot's uploads. The data tables sit 0xA8C (theater, copy screen, Demonstration: 0x8004B104..)
  and 0xB2C (DATA TRANSFER: 0x8004C3A4..) lower. So the arcade Data Transfer offers the same three modes (TRADE, MIX RECORDS,
  CONVERT; captured: the three panels, a TRADE purchase, "Combining Records Complete", "Converting Data Complete" of a GT1 file), with
  the same save file "BASCUS-94455GAME" and the replay file "BASCUS-94455REPLAY".
- **One difference: Copy Replay's message line.** In the list state 0xB the Simulation code (0x800150C4) calls 0x8006AC68(ctx, page)
  and prints the selected row's message ("Copy" / "Data is Too Large" / "Max 32 Files" / "Confirm" / "Cannot Find Replay File to
  Copy") with the small font 0x800A8DE0; the arcade code (0x80015034) skips 0x8006AC68 and passes the medium font 0x800A8AE8
  (= Simulation 0x800A8DF0; the arcade's small font is 0x800A8AD8). Its text context on the stack (0x801FFAD0, a0 of the font call
  0x8007D990 from 0x80015038 in session `sess1`) keeps what the list widget's highlight record left there in the same draw: the
  watched write at pc 0x8006D700 (ra 0x80016168, EXE 0x8006D6C8.. = Simulation 0x8006D7B8..: {x 0xB0, y, 0x120, 0x54, 0x0F0F0F,
  0x363636}) is the last one to + 0xC before the message. So the glyphs get CLUT base 0x3636 and page 0x36 (both halves of 0x363636):
  the message samples the wrong VRAM (every captured list frame: E1 0x36, CLUT 0x3638). Port: `CopyReplayScreen::arcadeMessageContext`.
- **Strings.** Twelve strings of these screens that no reference run maps are `kind: data` facts in `db\arcade_us11_symbols.yaml`
  (view titles LOAD REPLAY / COPY REPLAY / RENAME & DELETE / DEMONSTRATION / TRADE / MIX RECORDS / CONVERT, the transfer manager's lines,
  "Failed to Create Replay File on Memory Card 2", "Cancel?"); `gt2tool gen-profile` regenerated `exe_profiles.inc` (40 facts);
  gt2verify ArcTText now also checks every string constant of title_replay / title_copy / title_transfer and the card manager's replay
  modes (223 cases, 0 mismatches on `work\play\arcade_title\cap\opt_1450.txt.ram.bin`).
- **Port.** `tools\gt2game\arcade_title.*` (ArcadeTitleScreens: the theater's and DATA TRANSFER's views over the Simulation screens
  ReplayTheaterMenu / DemonstrationScreen / CardManager modes 1 and 2 / CopyReplayScreen / TransferManager / TradeScreen with the arcade
  assets in the Simulation layout; the demo file by member 1's table 0x8004BD7C; the music tracks 0 / 7 of the arcade EXE's table),
  `arcade_mode.cpp` (the rows open them; a loaded / chosen replay plays through `PlayTitleReplay`, then the theater starts again, as
  0x801EF022 == 2 does), `title_attract.cpp PlayTitleReplay` (two-player records in the split screen, as the Simulation theater).
  Keys in the arcade session: Q / W = L1 / R1, Delete = square.

Frames (the canvas of the native primitives with the interpreter GPU's rules against the capture's VRAM, 352 x 480; captures with
`600:start` first, native presses 1180 fields earlier than the capture's up to the card read; `--card` a copy of
`work\play\cards\card_r3.mcd`, the TRADE runs `work\play\transfer\card_rich.mcd` (the first-boot auto-load) + `card_s.mcd` in slot 2,
CONVERT `card_gt1.mcd`):

| Screen | Capture | Native | Differing pixels |
|---|---|---|---|
| REPLAY THEATER, Load Replay / Demonstration selected | cap1 th_1390 / th_1480 | 205..215 / 305 | 0 / 0 |
| DEMONSTRATION list, Demo 02 selected, scrolled to Demo 05 | cap1 demo_1700 / 1726 / 1800 / 2050 | 525 / 551 / 625 / 862..866 | 0 |
| LOAD REPLAY, Select a Slot / the list | cap2 lr_1595 / lr_1880 | 400..404 / 600..606 | 0 / 0 |
| RENAME & DELETE: list, keyboard, rename, delete mode, after delete, Cancel?, back (13 frames) | rd rd_1760 .. rd_3200 | 500 .. 2025 | 0 each |
| RENAME & DELETE + OK: the list, the theater after Exit | rs rs_2600 / rs_3000 | 1405 / 1805 | 0 / 0 |
| COPY REPLAY: blocks, Start, the list (4 frames with the message), Copy Complete, Restart | copy c_1700 .. c_2950 | 525 .. 1671 | 0 each |
| DATA TRANSFER menu, TRADE Select a Slot, Start Loading?, the list, Buy ?, after the purchase | trA t_1700 / 1950 / 2100 / 2500 / 2700 / 2950 | 532 / 729 / 899 / 1300 / 1500 / 1750 | 0 each |
| MIX RECORDS: Combining Records Complete | trM t_2800 | 1200.. | 0 |
| CONVERT: Converting Data Complete | trC t_2700 | 1200.. | 0 |

Not compared (as on the Simulation disc, section 9): the bars' pulse / title shine (TRADE Select a Slot inside the pulse: 954; "Saving
Complete"'s Exit button: 2014) and the "OK" reveal (165), progress frames at our card rate. Cards: after rename "Short B1 " + delete of
"Demo 01" + OK the native card = the original's (`rs\card.mcd` = `rsn\card.mcd`, 131072 bytes); after Copy Replay card 2 = the
original's (`copy\c2.mcd` = `cpn\card2.mcd`). The original arcade theater playing the card's "License" replay (`gt2verify --race-capture
<Arcade disc> 5200 rc\rc_lic.bin "600:start,1200:down,1260:cross,1400:cross,1550:cross,1800:cross"`, GT2_CAPTURE_CARD) against
`gt2game <Arcade disc> --replay card_r3.mcd#0 --frames-compare`: 1257 frames, 0 differ.
