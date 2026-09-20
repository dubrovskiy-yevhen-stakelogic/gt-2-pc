# GT-mode menu pages (GM) and menu pictures (GTMP)

Status 2026-09-19. US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Code addresses are
in GT2.OVL member 4 ("ovl4", the GT-mode menus, loaded at 0x80010000) unless marked EXE. Evidence: bytes of the disc,
our disassembly / Ghidra pseudo-C of the ovl4 RAM dump `work\re\menu_gt\ram_003100.bin` (pseudo-C only under work\),
and the original's VRAM + GP0 primitives captured with `gt2play --prims` (below). Containers (idx/dat, solodata,
iconimg): `docs/formats/gtmode_tables.md` section 8. Code: `src/gt2formats/gt_menu.*` (pages),
`src/gt2formats/gt_menu_images.*` (pictures, fonts, software renderer), `tools/gt2tool/menu_cmds.cpp`
(`gt2tool menu-page`, `gt2tool menu-dump`).

## 1. Summary

- Every GT-mode screen is one page of `gtmenu/usa/gtmenudat.dat` (3386 gzip entries). A page = sprite groups + items
  (76-byte records: rectangle, flags, type code, data) + a trailer (page flags, back page, background picture) + the
  page's own 4-bit picture (tile list, 32 CLUTs, pixels). ALL static text of the menus is pre-rendered into the page
  picture; the interpreter only draws the dynamic items (money, days, prices, prizes, medals, car data...).
- The background is a `gtmenu/commonpic.dat` entry ("GTMP"): 8-bit 16 x 8 tiles + flat tiles over a 512 x 504 map.
- Navigation is geometric (no up/down/left/right fields): the cursor moves to the best item in the pressed direction
  (0x8001D754); cross / circle run the item's action (0x80014380), triangle / square go to the page's back target
  (0x800142CC).
- Verified: our renderer reproduces the original frame pixel for pixel (512 x 480, 0 differing pixels) on the world
  map (page 0), My Home (page 920), License Test (page 927), the used-car page 1186 and the garage page 919 with their
  popup lists closed and open, scrolling, wrapping and empty (section 9: 22 captures).

## 2. Page layout (parser 0x800213C4, `ParseMenuPage`)

| Offset | Size | Content |
|---|---|---|
| +0 | 4 | "GM\x03\0" |
| +4 | 4 | u32 G = number of sprite groups; the block +4..end of the groups is copied to 0x8004AC30 (0x80052A40) |
| +8 | .. | G groups: `u16 s; u16 n; s x sprite (12); n x item (76)` |
| | 4 | u32 c = number of page items |
| | c x 76 | items |
| | 4 | page flags -> 0x800A8D78 |
| | 4 | back page -> 0x800A8D70 (0xFFFFFFFF = none; 808 pages) |
| | 4 | commonpic entry of the background (0x80020FBC; reloaded only when it differs from 0x80052A34) |
| | .. | page picture (section 4) |

The parser consumes every page of the disc to its last byte (`gt2tool menu-dump`: 3386 / 3386). The items of all
groups, then the page items, are copied to 0x801C3150 + i * 0x4C (0x8002150C, at most 64; count 0x800A8D74); item i
= 0x800215C8(i).

Sprite (12 bytes, 0x800220C8): `s16 x, s16 y, u8 u, u8 v, u8 w, u8 h, u16 tpage, u16 clut` -> DR_TPAGE `0xE1000000 |
(tpage & 0x1F)` (4-bit) + SPRT 0x64 colour 0x808080. All 14367 group sprites of the disc use tpage 11 = iconimg (VRAM
704, 0); the most common are the four tab icons (map / home / race / trophy) and the exit sign.

Page flags (0x800A8D78):

| Bits | Meaning (evidence) |
|---|---|
| 0..7 | maker id: argument of the used-car list (0x8001D2CC -> 0x80020A0C) and the maker check of type 0x98 (current car slot byte +0x81); 0xFF elsewhere |
| 8 | keep history: the back target becomes the previous page instead of the page's back field (0x8001D2CC; only together with bit 9) |
| 9 | clear behind: black flat tiles are not drawn and the frame is cleared first (0x80021D30, 0x8001B9AC) - the 3D car shows through (car pages) |
| 16..23 | menu music track (0x80018FA4 -> 0x800228D4 -> SEQG file id table 0x80052A50[track]: sound/spu_02..10.seq, not XA; docs/formats/sound.md section 8); 0xFF keep |

History (0x8001D2CC, page object +8 = back target, +0xC = the one before): a normal page (id >= 0) without bit 8 sets
back = its back field; a message page (id bit 31) or a page with bit 8 sets back = the page it was opened from.
Message ids 0x800000nn resolve through `solodata.dat`'s page list, entry nn (0x800211FC).

## 3. Items (76 bytes)

| Offset | Content |
|---|---|
| +0x00 | s16 x0, y0, x1, y1 (screen pixels) |
| +0x08 | u32 flags: bits 0..15 type code, 16..31 below |
| +0x0C | u32 target page (bit 31 = message page) - or, for event / wheel items, a NUL-terminated name |
| +0x10 | type 6: packed car id; type 1: an alternative 12-byte sprite (the highlighted tab icon; no reader found in ovl4) |
| +0x4A | u8: licence index of flag bit 18 (0 -> licence 1, 1 -> licence 0, else never selectable) |
| +0x4B | u8: prize position of flag bit 22 (1..6) |

Flag bits 16..31 (`menu_item_flag`):

| Bit | Meaning |
|---|---|
| 18 | type 0: licence-gated (badge 0x80050A50 when held, 0x8001915C; selectable only then, 0x8001B680) |
| 19 | event item: result (0x8005DB90 nibble r: medal 0x80050978 + (4 - r) * 12 for r = 1..3, else "%d") |
| 20 | event item: licence requirement icon 0x800509FC + (req + 1) * 12 (0x80019634: rules bits 1..3 = 1..6 -> licence 5..0, else -1) |
| 21 | event item: power limit "~%dhp" (hp = limit * 1000 / 1014) or "free" (0x800196C8, event info +0x1E) |
| 22 | event item: prize of position +0x4B (0x800196EC, event info +4) |
| 23 | event item: championship result: trophy 0x80050A5C when first, else "%d" |
| 24 | generic action: go to +0x0C (with the checks of types 0x98 / 0x9C / 0xAD / 0xAE) |
| 25 | default item: the cursor starts here (0x8001D6CC; 2493 items) |
| 26 | set on all 609 type-6 items (no reader found) |
| 27 | event item: +0x0C = event / licence-test name (1784 items, e.g. "GBL0001", "GT30501") |
| 28 | wheel item: +0x0C = wheel code, e.g. "bb001--s" (maker pair of 0x80050904 "bbbrduenfaozraspyo", 3 digits, char 6 '4'/'5'/'6' size) |
| 29 | type 6: no price |
| 30 | event item: race without entry check, 0x801EF5F5 = 3 |
| 31 | text in the second font (28-px digits) |

Event info records (0x80019474, 0x24 bytes at 0x800B5E88 + i * 0x24, i = 0x800188B0(name) = index in the menu's
event list ovl4 0x80050D1C): +0 bonus, +4 prize[6], +0x1C rules, +0x1E power limit - the career port builds the same
records (`src/game/career/events.h EventInfo`), which `gt2tool menu-page` uses to draw event pages.

### 3.1 Item types (code = flags & 0xFFFF)

Actions (0x80014380(view, item, circle)). A new page is requested by view +0x1BC = page, +0x1C0 = argument, +0x1C4
= 3 (countdown), +0x19C = 8 (input hold-off); `0x80060840(n)` = menu sound.

| Type | Action | Handler |
|---|---|---|
| any + bit 24 | go to +0x0C | 0x80014380 (default of the bit-24 block) |
| any + bit 27 | start the event: licence names (first char 'L', 0x80018608) -> 0x80019B88, others -> entry check 0x8001973C; ok: name -> 0x800C1C10, 0x801EF5F5 = 2 (race), else the check's message page; with bit 30: no check, 0x801EF5F5 = 3 | 0x80014380 |
| any + bit 28 | buy wheels: code -> 0x80013A28 wheel id, transaction 5 (price 2000, 0x8001DB0C), GTDT table 0x1D row +6 / +7 (0x80021BEC) | 0x80014380 |
| 0x02 | yes: run the transaction (buy car / part / wheels / sell / select) -> result page | 0x8001DDAC |
| 0x03 | no: cancel the transaction, back | 0x8001DD3C |
| 0x04 | check the transaction (money, garage full, part prerequisites) -> confirmation / error page | 0x8001DFEC |
| 0x05 | the displayed car's page (solodata value of the car id, 0x80020F54) | 0x8001D698 |
| 0x06 | new car of the dealer: transaction 1 (car +0x10 at its catalogue price 0x800177D4), show the car | 0x8001DAA8, 0x80014348 |
| 0x09 | garage car list popup (popup mode 1, list 0x800A8D60, selection 0x800A8D62 = garage slot; choosing a row shows that car, 0x80017288(slot), goes to +0x0C; types 0xAA / 0xAB act on the same selection). menus_gtmode.md called it the new-car list: the rows are garage slots (+0x8C model, +4 paint, +8 config) | 0x800204EC / 0x8002055C |
| 0x0B | exit GT mode (0x801EF5F5 = 0 -> title) | 0x80014380 |
| 0x0D | car wash: money -= 50 when 0x80018274() and slot +0xA2 != 0; popup mode 3 | 0x80014380 |
| 0x10 | cancel the transaction and go back | 0x8001DCD0 |
| 0x12 | back (argument 1) | 0x80014380 |
| 0x13 | message page 0x8000002E | 0x80014380 |
| 0x14..0x46 | buy part kind (type - 0x14) for the current car: transaction 3 (0x8001DB24) + check 0x8001DFEC; at page load 0x8001DB90 computes the power before / after (0x801C309C / 0x801C30A0) | 0x80014380 default |
| 0x50 | used-car list popup of the page's maker (popup mode 2; row -> show car, transaction 1 at the lot price, +0x0C) | 0x80020A0C / 0x80020A94 |
| 0x95 / 0x96 | the displayed paint +1 / -1 (colour cycling on the car pages, not a setting; circle gives -1) | 0x8001A530 |
| 0x98 (+24) | tune shop of the car's maker: current car needed (else message 2), slot +0x81 == page maker (else message 0x1D) | 0x80014380 |
| 0x9C (+24) | racing modification: car needed, part kind 0x22 not owned (else 0x1A), 0x800174AC (else 0x19) | 0x80014380 |
| 0x9D | paint list of the car (0x80017530, show-car kind 3) | 0x80014380 |
| 0xAA | sell the car selected in the list (transaction 6) | 0x8001DA38 |
| 0xAB | make it the current car (transaction 7) | 0x8001DA98 |
| 0xAD (+24) | page with the current car shown (car needed) | 0x80014348 |
| 0xAE (+24) | page only when 0x80018210(car) (else message 0x1E) | 0x80018210 |
| 0xBB | no-op | |

Transaction object 0x801C3080 (state u16 +0): 1 buy car (+0xC car, +0x30 price), 3 buy part (+0x14 slot, +0x18 kind),
5 buy wheels, 6 sell (+4 index), 7 select (+8 index). Result pages of 0x8001DFEC / 0x8001DDAC (message ids & 0xFF): 0x00
confirm purchase, 0x01 not enough money, 0x0E garage full, 0x02 no current car, 0x03 part owned, 0x0B / 0x20 part
confirm, 0x0F / 0x10 / 0x11 part prerequisites, 0x19 no such part, 0x0D confirm sale, 0x05 car selected, 0x04 first car
bought (it becomes current), 0x08 car bought, 0x06 / 0x07 / 0x1F part bought, 0x09 part fitted, 0x1C sold.

Draw (0x8001B9AC; text colour 0x6E6E6E unless noted; "right" = right-aligned at (x1, y1), "left" = at (x0, y1)):

| Type | Draws |
|---|---|
| 0x00 + bit 18 | licence badge 0x80050A50 at the centre when held |
| 0x06 | catalogue price with commas, right at (x1 - 0x50, y1 + 0x14), colour 0x2808080 (semi-transparent, texpage mode 2), unless bit 29 |
| 0x07 | setting value (0x8001A4AC), right, other OT |
| 0x09 / 0x50 | the popup list (0x8002068C at 0x100, 0xB0 / 0x80020B70 at 0x100, 0x9C) |
| 0x0A | 3D car view in the rectangle (own draw environment; camera, car, floor disc: section 10) (0x8001A8A4) |
| 0x11 / 0x94 | the car's name logo at the centre (0x8001A654, section 10) / paint chips from (x1, y0) (0x8001A708); both into the view's second OT |
| 0x47 | current car name (0x8001B818) |
| 0x48 / 0x49 | power before / after "%dhp" (colour 0x145A78; "----" when none) |
| 0x4A / 0x4C | power "%dhp" / weight "%dlb" (kg * 22046 / 10000) of the view |
| 0x4D | money with commas (0x8001FCDC), right |
| 0x4E | transaction price, "Purchased" (0xF05028) or "N/A" (0x2850F0) |
| 0x4F | days "%d", right |
| 0x53.. / 0x5E.. / 0x69.. / 0x74.. / 0x7F.. / 0x8A.. (10 each) | licence test medal (record byte +1 - 1 -> 0x80050978 + m * 12) of licences 3 (IC), 5 (B), 4 (A), 2 (IB), 1 (IA), 0 (S): records 0x801CC030 / 0x801CCD00 / 0x801CC698 / 0x801CB9C8 / 0x801CB360 / 0x801CACF8 + test * 0xA4 |
| 0x97 | drive-train icon 0x8005093C + drive * 12 (drivetrain row +8 of the displayed car; section 8) |
| 0x9A | model year "'%d" or "----" |
| 0x9B | paint name (0x800183B8), centred |
| 0x9D | fill rectangle when 0x800174D0() < 2 |
| 0xB2..0xBA | car data: length / width / height "%dmm", weight, displacement "%dcc" / "%dx%dcc", drive train (table 0x80051260), engine text, max power "%dhp / %drpm", torque "%d.%dlb-ft / " + rpm |
| 0xBC..0xC6 | game status statistics (0x80019C60 .. 0x80019E98; section 11) |
| 0xC8..0xCB | licence counts (0x80019EF8(4..1)) |
| 0xCC | licence level icon 0x800509B4 + level * 12 (0x800191C4 < 6) |
| 0xCD | equipped parts of the chosen garage car (0x80017318 at 0x20, 0xA0; section 11; not a licence list) |
| 0xCF | event bonus with commas, right |
| 0xD0 | view value / 4 |

Types without an action or draw (0x00 without bits, 0x01 tab buttons, 0xAF, 0xB0, ...) are plain "go to +0x0C" items
when bit 24 is set, or cursor stops otherwise. Formats of the dynamic texts come from `data-gt.txd` (gzip inside ovl4 at
0x800242A8, one 0x88-byte block per language, copied to 0x801C30C0 by 0x80020C50: "N/A", "Purchased", "~%dhp", "free",
"%dhp / %drpm", "%d.%dlb-ft / ", "%d.%02dmph", "%drpm", "%dmm", "%dcc", "FR", "FF", "4WD", "MR", "RR", "%dhp",
"%dx%dcc").

### 3.2 Cursor and navigation

The cursor object (view + 0x1D0, init 0x8001E22C at (256, 252)) jumps to the default item (bit 25) or, without one,
to a selectable item under its position (0x8001D954). Directions (0x8001E328; pad bits of the view 1 / 2 / 4 / 8 and
0x10 / 0x20 / 0x1000 / 0x2000 for the diagonals): 2 up, 3 down, 4 left, 5 right, 6 up-right, 7 up-left, 8 down-right,
9 down-left, unit vectors 0x80051274. 0x8001D754 scores every other selectable item by cos^2 / distance of its centre
(cos = direction . (dx, dy) / d, items behind are skipped) and takes the best; no item -> buzzer and a small bounce
(0x8001E26C). Selectable = bit 24, or 0x8001B6F0 (event items without the display bits 19..23, and types 1..6, 8, 9,
0xB..0xD, 0x10, 0x12..0x46, 0x50, 0x95, 0x96, 0xAA, 0xAB, 0xBB, 0x9D when 0x800174D0 >= 2), and the licence gate of
0x8001B680. Choose (view bits 0xA00: cross 0x200 or circle 0x800, logical bits of EXE 0x800A6F3C) runs the action
with the cross bit as its third argument; back (0x500: triangle 0x100 or square 0x400) goes back (0x800142CC) unless
the action ran (section 8). The arrow (0x8001EA24): sprite 0x80050930 on an item, 0x80050924 elsewhere
(tpage 9 = gt_cursor.tim, 24 x 32), its tip at the item centre.

## 4. Pictures

### 4.1 Page picture (0x80021284)

`"GMLL" (not read); u32 k; k tile words (copied to 0x80048430); 64 x 8 CLUT words -> VRAM (576, 248); u32 n; pixels`
with n = number of 4-bit 16 x 8 texture tiles; the file stores (n / 32 + 1) * 8 rows of 256 bytes, the game uploads
ceil(n / 32) * 8 of them to VRAM (640, 256) (128 words wide = 512 texels, two 4-bit texture pages 0x1A / 0x1B); when n
is a multiple of 32 the last stored strip is not uploaded (93 pages). Tile word (0x8002202C):

- bit 7 set: flat 16 x 8 TILE (0x80021D30): x = bits 0..5 * 16, y = bits 8..15 * 8, colour = 15-bit BGR in bits
  16..30 (not drawn when black and page flag bit 9);
- else SPRT 16 x 8 (0x80021DE8): x = bits 0..6 * 16, y = bits 8..15 * 8, u = bits 16..19 * 16, v = bits 21..25 * 8,
  texpage 0x1A + bit 20, CLUT x = 576 + 16 * bits 27..28, y = 248 + bits 29..31.

### 4.2 Background: commonpic GTMP (0x8002117C, 0x80021968 stream reader, 0x80021608)

| Offset | Content |
|---|---|
| +0x0000 | "GTMP" |
| +0x0004 | 2016 tile words = 63 x 32 (0x80021F88 walks 63 x 32 without reading positions from the loop) |
| +0x1F84 | 512 x 8 CLUT words -> VRAM (0, 504): 16 CLUTs of 256 colours (0x8002136C) |
| +0x3F84 | u32 used texture tiles, the same u32 again, then bytes not read by the game |
| +0x4000 | pixels, 8-bit: rows of 512 bytes, streamed in 4-KB chunks (8 rows) to VRAM (768, 0), (768, 8), ... |

The first 0x4000 bytes go to RAM 0x80042430 (0x80052A2C). The entries are stored (not compressed), 4-KB aligned, all
458 parse and every texture tile lies inside the rows of its picture. Tile word: flat tile as above; else SPRT 16 x 8
(0x80021EB0): x = bits 0..6 * 16, y = bits 8..15 * 8, u = bits 16..19 * 16, v = bits 21..25 * 8, texpage 0x8C (8-bit,
x 768) + bit 20 (x 896) + bit 26 (y 256), CLUT x = 256 * bit 28, y = 504 + bits 29..31.

The draw list is built once per page load by 0x80022278 into a linked packet list (0x80021C5C, packets of up to 16
words): background tiles, page tiles, group sprites - in that order - and inserted into the OT by 0x8001D208.

### 4.3 VRAM while a page is shown (checked word for word against the original's VRAM)

| Area | Content | Loader |
|---|---|---|
| (0, 504) 512 x 8 | commonpic CLUTs | 0x8002136C |
| (768, 0) 256 x N | commonpic pixels (8-bit) | 0x80021608 |
| (576, 0) 64 x 156 | `arcade/gt_cursor.tim` (VOL file 0x2C, tpage 9) | 0x80013CF8 -> 0x80013B28 / 0x80013B60 |
| (512, 256) 64 x 157 | `arcade/gt_items.tim` (0x2D, tpage 0x18) | same |
| (640, 0) 64 x 247 | `arcade/gtmode_font.tim` (0x2E, tpage 10): both menu fonts | same |
| (704, 0) 64 x 256 | `gtmenu/usa/iconimg.dat` (group sprites, tpage 11) | 0x80020ECC |
| (576, 248) 64 x 8 | page CLUTs | 0x80021284 |
| (640, 256) 128 x N | page pixels (4-bit) | 0x80021284 |

The three `arcade/*.tim` files are TIMs whose single image block also holds their CLUT rows (e.g. the cursor CLUTs
0x2625 / 0x2626 = row 152); 0x80013B60 uploads the image block to the texture page named by the caller, with the
block's own width / height (the file's x / y are 0).

### 4.4 Fonts (member 4)

Both fonts are in gtmode_font.tim (tpage 10). Font A (default): 0x10B glyphs, glyph table 0x800513BC (12 bytes: u8 u,
u8 v, u16 clut, u16 w, u16 h, u16 tpage, pad) and metrics 0x80052100 (8 bytes: u16 code, s16 dx, s16 dy, s16
advance). Font B (item flag bit 31, 28-px digits and a few letters): 0x18 glyphs at 0x8005129C / 0x80052040. 0x8001F210
builds the 8-bit maps 0x800B9308 (A) / 0x800B9108 (B): map[code] = last glyph with that code (< 0x100); font A looks up
codes >= 0x100 by a binary search (0x8001F2C8). A glyph = SPRT (colour argument; bit 25 of the colour -> semi-transparent
0x66) at (x + dx, y + dy), then DR_TPAGE (tpage | 0x40); x += advance. Text routines: 0x8001FC28 (UTF-16, right-aligned),
0x8001FBEC (UTF-16, left), 0x8001FC64 (8-bit, left), 0x8001FCA0 (8-bit, right); each picks font A or B by its last
argument (item bit 31).

## 5. Renderer (`RenderMenuPage`, tools only)

Composes the VRAM of 4.3, draws the background tiles, page tiles and group sprites, then the dynamic items the given
state allows (money, days, dealer prices, event bonus / prizes / power limit / licence icon, results when a result
callback is given), collected and drawn in reverse (the OT prepends them), then the cursor. PS1 rules: texel 0 is
transparent, modulation (texel * c) >> 7, semi-transparency only for STP texels. The popup lists (types 9 / 0x50) are
drawn when the caller hooks them (`customItem`, section 9). Not drawn: the 3D car, items that need tune / garage /
licence state (they draw nothing on a new game anyway).

`gt2tool menu-page <disc> <page> [--png out.png] [--money N] [--day N] [--no-cursor] [--compare cap.vram.bin --side
out.png] [--rules ps1|interp] [--used-cars] [--garage [--card save.mcd]] [--list-frames N [--list-active] [--list-pad
f:button,...]] [--list-sel N] [--list-scroll N] [--list-flash N] [--list-state N] [--list-active-byte 0|1]` prints the
page (header, sprites, items with type names and handlers) and renders it; `--compare` checks the frame (VRAM 0..511
x 0..479, bit 15 ignored) and the composed VRAM areas against a `gt2play --prims` VRAM dump and writes ours | original
| differences (canvas rules: the interpreter's with `--compare`, else the PS1's; section 9.5). `--used-cars` /
`--garage` draw the page's list (the page maker's lot of `--day`; the garage of a card, empty without), loaded as a
page load does, optionally run for N updates with presses, then with the given state. `gt2tool menu-dump <disc>
<outDir>` parses and renders all pages (pages.txt + page_NNNN.png, work\ only).

## 6. Verification (2026-09-19, captures in work\play\menu)

| Screen | Route (gt2play --script) | Capture field | Page | Frame | VRAM areas |
|---|---|---|---|---|---|
| World map | `1400:cross` | 2300 | 0 | 0 differing pixels | all equal |
| License Test | `1400:cross,2000:right,2060:cross` | 2350 | 927 (0x39F) | 0 | all equal |
| My Home | `1400:cross,2000:cross` | 2350 | 920 (0x398) | 0 | all equal |
| Used cars (Mazda) | `1400:cross,2000:right,2030:right,2100:cross,2300:cross,2560:right,2600:cross` | 2900 | 1186 (0x4A2) | 0 with `--used-cars` (26097 without the list); more list captures in 9.6 | all equal |

Side-by-side images: `work\play\menu\side_{map,licence,home,usedcars}.png`; the captures `*_NNNN.txt` (GP0 list),
`.vram.bin`, `.vram.png`, `*_NNNN.png` (gt2play shot). The page of a capture is found by matching its 4-bit page
tiles (texpage 0x1A / 0x1B) against every page's tile list: a unique exact match each time. The primitive order of
the captures confirms the draw order (background, page tiles, group sprites, item texts, popup, cursor).

## 7. Open questions

- (Answered 2026-09-19, section 9) Popup lists: the EXE list widget and its row callbacks are decoded and drawn.
- Popup lists: the widget's close (0x8006CED8), clamp (0x8006D4B0), own highlight bar (flags 8), disabled rows
  (command 8 = 0) and left / right jumps at the list ends are not used or not captured; garage lists of more than one
  car (scrolling with 11 visible rows) and the racing prefix glyph were not captured (no such card).
- Type 1 items' sprite at +0x10 (the highlighted tab icon): no reader in ovl4; maybe unused.
- Flag bit 26 of type 6 and item bits 16..17 (never set); +0x3F88.. of GTMP.
- (Answered 2026-09-19, section 8) `0x80081288` in the cursor search = `sim::SquareRoot(v, 0)`; the search is ported
  and verified (gt2verify MenuNav).
- Message pages keep the picture of the page under them? (they are complete pages of their own; not captured).

## 8. Native runtime (2026-09-19: `src/game/menu`, `src/gt2view/menu_view.*`, `gt2game --menu`)

`gt2::menu::MenuRuntime` (no rendering API) = the menu view of member 4: page load + history 0x8001D2CC, the page
change countdown (+0x1C4 = 3: the page loads in the view draw 0x8001B3AC two fields after the action; input hold-off
+0x19C = 8), the cursor 0x8001E328, the popup modes (+0x1CE 0 page, 1 garage list, 2 used-car list, 3 car wash),
back 0x800142CC, the actions 0x80014380 and the transaction object 0x801C3080 (check 0x8001DFEC, run 0x8001DDAC, cancel
0x8001DD3C / 0x8001DCD0). Career rules go through `MenuActions` (`CareerMenuActions` = `src/game/career`); what the
career model does not provide is reported through `MenuActions::NotAvailable` and refused, never faked. The frame is
`BuildMenuFrame` (gt2formats) with `MenuRuntime::RenderState()`: the career-dependent item texts of 0x8001B9AC go
through `MenuRenderState::dynamicItem` (generation order, reversed with the other items), the lists through
`customItem`. `gt2view::MenuView` draws the frame's primitives with the Vulkan renderer (page VRAM in the renderer's
console rows, PS1 sampling, integer colour modulation flag `kIntegerModulate`, STP semi-transparency passes).

Facts established for the port (member 4 unless marked EXE):

- Navigation 0x8001D754 (`menu_nav.cpp NearestItem`): the 5th argument (stack +0x10) is the item to skip, the 6th is
  not read; an item counts when (flag bit 24 or 0x8001B6F0) and 0x8001B680 == 1; centres `(a + b) >> 1` of the s16
  rectangle; `d = 0x80081288(dx*dx + dy*dy, 0)` (= `sim::SquareRoot`), items at d = 0 skipped; `dot = (vx * ((dx << 12)
  / d) >> 12) + (vy * ((dy << 12) / d) >> 12)` with the vectors 0x80051274 (read from the overlay), dot < 0 skipped;
  score = bits 12..43 of the signed product `(0x1000000 / d) * ((dot * dot) >> 12)`; strictly greater than the best
  (start 0) wins, so the first of equal scores is kept and a score of 0 is never chosen. **Verified**: gt2verify rows
  on `work\re\gtmode\ram.bin` (member 4 loaded; the rows skip themselves elsewhere by a hash of the routine's code):
  MenuNav 0x8001D754 20000 cases (random item arrays of 0..64 items with every type of 0x8001B6F0, random flag bits,
  licence records, tune-sheet words; cursor on item centres, random and extreme positions; all ten direction codes;
  random current item; unloaded page), 0 mismatches (12104 searches found an item); MenuSelect 0x8001B6F0 and MenuGate
  0x8001B680 5870 items each, MenuUnder 0x8001D954 and MenuDflt 0x8001D6CC 5000 cases each, MenuBodies 0x8005F858 400
  cases - all 0 mismatches.
- Pad bits (logical, EXE mapping table 0x800A6F3C): choose = 0xA00 (cross 0x200, circle 0x800), back = 0x500
  (triangle 0x100, square 0x400); 0x80014380's third argument is the cross bit (types 0x95 / 0x96: +1 with cross, -1
  with circle). Section 3.2's "cross / square" wording is superseded. `gt2game --menu` maps Enter = cross, Space =
  circle, Backspace / Esc = triangle (back).
- Cursor 0x8001E328: countdown +4 (8 after a move; while it is 7 / 6 a second held direction turns the move into a
  diagonal, searched again from the previous target with no item to skip); spring `v += ((((t * 256 - p) * 2500 - v *
  110) / 60) * 100) / 180; p += v / 60`, drawn position p / 256; no item in the direction -> sound 0 and the bounce
  0x8001E26C (velocity +-10,000,000 against the direction). After a page load 0x8001E924 moves it to the default item
  (bit 25) or keeps it and takes the item under it (0x8001D954).
- History: a normal page without flag bit 8 sets back = its back field; a message page (id bit 31) or a page with
  bit 8 sets back = the page it was opened from, except message -> message and history page -> history page.
- 0x800142CC goes back only when view +0x1F8 bit 0 is set: 0 after a transaction check (types 4, 0xAA, 0xAB, parts),
  1 after yes / no / cancel (the confirmation pages cannot be left with back).
- Type 0x97 draws the drive-train icon 0x8005093C + drive * 12 (drive = drivetrain row +8 of the displayed car; the
  same index selects the names of 0x80051260 for type 0xB7), not a tune icon. The car view data block (view +0x394)
  is filled by 0x8001AFA8 from the catalogue configuration or by 0x8001B10C from a garage slot (db).
- Formats: '%dlb' / '%dhp' of types 0x4C / 0xB5 / 0x4A are data-global.txd strings (title overlay, RAM 0x801EF6C1 /
  0x801EF6C6), '----' / "'%d" / '%d' / '%d.%02d' are member 4 rodata 0x80023D08 / 0x80023D10 / 0x80023CFC / 0x80023D00,
  the rest data-gt.txd (0x801C30C0..).
- Types drawn by the runtime from the career state: 0 bit 18 badge, 0x07 paint name, 0x47 current car name
  (0x8001B818, racing prefix 0x80050BB0), 0x4A power, 0x4C / 0xB5 weight, 0x4E transaction price / "Purchased" /
  "N/A", 0x53..0x93 licence test medals, 0x97 drive icon, 0x9A model year, 0xB2..0xB4 dimensions, 0xB6 displacement
  (not the rotary "%dx%dcc" form), 0xB7 drive name, 0xB9 power / rpm, 0xC8..0xCB licence counts (0x80019EF8), 0xCC
  licence level, 0xD0 price / 4, 0x94 paint chips (0x8001A708: per paint of the displayed car, right to left from
  (x1, y0) in steps of 12: 0x8006B6E4 gouraud quad black -> chip colour (15 -> 24 bit as `(c & 0x1F) << 3 | (c & 0x3E0)
  << 6 | (c & 0xF800) << 9`), a black TILE 10 x 8, the 5-point polyline 0x48 at (x - 13, y - 1)..(x - 2, y + 8) grey
  0xB4 for the displayed paint else 0x46, then E1 0x220), plus the built-in 0x06, 0x4D, 0x4F and the event items.
  Since 2026-09-19 also (sections 10 / 11): 0x0A the 3D car view, 0x11 the name logo, 0x48 / 0x49 power before / after,
  0xB8 / 0xBA engine / torque texts, 0xBC..0xC3 status statistics, 0xCD equipped parts. Not drawn: 0xC4..0xC6 (no page
  uses them), 0xCF outside event items, the rotary "%dx%dcc" form of 0xB6.

Verification of the native frames (Vulkan screenshot of a 512 x 480 square-pixel window vs the original's VRAM,
`gt2game <disc> --menu --window 512x480 --menu-square --menu-script ... --menu-shot N out.png --menu-compare
cap.vram.bin`, 5-bit channels; images `work\play\menurt\*_side.png`):

| Screen | Our script | Original capture | Differing pixels |
|---|---|---|---|
| World map (page 0) | - (field 60) | `work\play\menu\map_2300.txt` | 0 |
| My Home (920) | `20:cross` (field 120) | `home_2350.txt` | 0 |
| License Test (927) | `20:right,60:cross` (field 160) | `lic_2350.txt` | 0 |
| Mazda dealer (1117) | `20:right,50:right,120:cross,320:cross` (field 520) | `work\play\menurt\orig_maker_2500.txt` | 0 |
| New car RX-7 LM (1162) | + `400:right,430:right,460:right,500:cross` (field 673 = the capture's yaw) | `orig_newcar_2700.txt` | 8606, only inside the 3D car (section 10: silhouette IoU 0.91); logo, floor, texts, chip equal (was 26166 before the car view) |
| Used cars, list in page mode (1186) | `20:right,50:right,120:cross,320:cross,580:right,620:cross` (field 920) | `work\play\menu\maker_2900.txt` | 0 |
| Garage, new game (919) | `20:cross,320:cross` (field 720) | `gar_empty_2700.txt` | 0 |
| Garage, one car (`--career work\memcards\gt2_save_1car.mcd`) | `20:cross,320:cross` (field 720) | `gar_car_5350.txt` | 0 |
| Garage, list opened | + `700:cross` (fields 701..765) | `gar_open_5420.txt` | 127 at the best phase (fields 704 / 706 / 765: a moving segment on the list frame's bottom line y = 194), 4494..7642 elsewhere: the row highlight pulses with period 61 fields and our scripted press is not at the original's phase |

End-to-end purchase through the native menus (new game; `gt2game <disc> --menu --menu-script "20:right,50:right,
120:cross,320:cross,580:right,620:cross,700:cross,760:down,800:cross,900:right,930:right,960:right,990:cross,1100:cross"
--save-out bought.sav`: used-car list, second row, BUY, YES): the career block written afterwards equals the one the
original saved after the same purchase (`work\memcards\gt2_save_1car.mcd`, menus_gtmode.md section 1): 0 of 0x7C9C
bytes differ (money 4,852, the car with its paint, configuration and figures, current car 0). Selling it again from the
garage car page (0xAA, YES) gives money 6,102 (+ 5,000 / 4).

Polygons and lines of the native view are the PS1 rasteriser's pixels (MenuCanvas, emitted as 1 x 1 quads with their
exact 5-bit colour); the comparisons above use the rules of our interpreter's GPU (`MenuCanvas::Rules::kInterpreter`,
chosen automatically with `--menu-compare`), because that GPU produced the captures: with the PS1 rules the used-car
list's paint chips differ in 224 pixels from the capture.

## 9. Popup list widget (types 9 / 0x50)

The garage list (type 9, page 919) and the used-car list of a dealer (type 0x50, pages 1030 / 1106 / 1186 / 1264 /
1349 / 1423 / 1493 / 1645 = makers 7 / 11 / 18 / 22 / 23 / 30 / 31 / 33) are one resident EXE widget driven by a row
callback of member 4. Code: `src/gt2formats/gt_menu_list.*` (widget, rows, drawing); `gt2tool menu-page --used-cars /
--garage` (below). Evidence: our disassembly of `work\re\gtmode\ram.bin` (member 4 loaded; Ghidra hides the pad
argument a1 of 0x8006CFC4 in 0x80020A94 and the stack arguments of 0x8006B988) and the captures of 9.6.

| Routine | Role |
|---|---|
| EXE 0x8006CDCC(w, cb, arg) | reset: state -1, selection 0, blink 0, scroll 0, revealed 0, reveal delay / period 6, fade 0, callback; command 0 per row |
| EXE 0x8006CE70(w) | open: period / delay 6, scroll 0, revealed 0, fade 0, state 0, blink 30; command 7 (selection) |
| EXE 0x8006CED8(w) | close: state -65, fade = maximum; command 2 for every row but the selection (no caller found) |
| EXE 0x8006D4B0(w, lo, hi) | clamps the selection (scroll 0 when it moves it; no caller found) |
| EXE 0x8006CFC4(w, pad) | update: -2 nothing, -1 back, -3 moved, -4 choose on a disabled row, >= 0 the row chosen |
| EXE 0x8006D50C(w, ot) | draw |
| EXE 0x8006B548(a, b, t, max) | colour a + (b - a) * clamp(t, 0, max) / max per channel (C division), byte 3 of a kept |
| EXE 0x8006B6E4 / 0x8006B814 | highlight quad (POLY_G4 0x3A) / highlight bar |
| EXE 0x8006B988 / 0x8006BB08 / 0x8007E780 | scroll arrow (POLY_F3 0x22) / paint chip / rectangle outline (polyline 0x48 by 0x8007E738; LINE 0x40 by 0x8007F7F4 when w or h is 1) |
| 0x80020490 / 0x800209B0 | reset the garage / used-car widget with its callback (0x80020198 / 0x800206F8), list count 0, flash -1 |
| 0x800204EC / 0x80020A0C | load (0x8001D2CC, page load): n = garage count 0x801CD554 / rows of 0x80022634; n > 0: widget count = n, open, selection clamped to n - 1; list count = n, list +4 = item. The used-car list is reset first when the page argument is 0 |
| 0x8002055C / 0x80020A94 | per-frame update (9.2) |
| 0x800204D8 / 0x800209F8 | anchor x, y = (0x100, 0xB0) / (0x100, 0x9C), set by 0x8001B9AC before each draw |
| 0x8002068C / 0x80020B70 | draw: the widget, or when count < 1 the 8-bit string 0x801C30C0 ("N/A") at (x - 152 / x - 188, y + 64), colour 0xF0780A (0x8001F520) |
| 0x80022634(maker, &n, &rows) | rows of the maker in the period copy 0x800B9544 (0x800224E0 at the GT-mode entry: period (day / 10) % 60, cars hidden in the language dropped): n = u16 [base + maker * 4 + 2], rows = base + u16 [base + maker * 4]; row pointer -> 0x800B9510 |
| 0x80020BDC / 0x80020C00 / 0x80020C24 | the chosen used car: row(selection) +0 car id, (s8) +7 paint, +4 & 0xFFFFFF price |

### 9.1 Objects

Widget (0x30 bytes; initial values in the overlay image: garage 0x80052958, used cars 0x8005299C):

| Offset | Field | Garage | Used cars |
|---|---|---|---|
| +00 s16 | rows | garage count | lot rows |
| +02 u16 | flags: 4 wrap up / down, 8 the widget's own highlight bar (0x0F0F0F -> 0x363636, pulse = blink), 0x10 left / right jump, 0x20 arrows without blinking | 0x14 | 0x14 |
| +04 s16 | visible rows | 11 | 9 |
| +06 s16 | selection | | |
| +08 s16 | width of the own bar | 0x180 | 0x1A0 |
| +0A / +0C s16 | row height / gap (pitch = sum) | 18 / 2 | 22 / 2 |
| +0E / +0F s8 | arrow half width / height | 8 / 10 | 8 / 10 |
| +10 / +12 s16 | centre x / top y (the anchor) | 0x100 / 0xB0 | 0x100 / 0x9C |
| +14 s16 | fade maximum | 6 | 6 |
| +16 u16 | callback flags: bits 0 / 1 / 2 / 3 / 4 / 7 suppress commands 0 / 1 / 2 / 3 / 4 / 7 | 0 | 0 |
| +18 / +1A / +1C s16 | rows revealed / updates to the next reveal / reveal period (command 1) | | |
| +1E u8 | 1 when the last update had a pad (the arrows need it) | | |
| +20 s16 | scroll: -8 / +8 after a move up / down, one step towards 0 per update | | |
| +22 s16 | blink 0..60 (0 after a move; pulses the own bar) | | |
| +24 s16 | fade 0..maximum (not drawn by these lists) | | |
| +26 s16 | state: -1 closed, 0..45 open (cycles; the arrows' blink), -65..-2 closing (command 2 for the selection at -58) | | |
| +28 / +2C | callback(command, &block, row) / its argument (0) | 0x80020198 | 0x800206F8 |

Behind each widget its colours (0x8006B548 keeps byte 3 of the first): garage 0x8005298C base 000000, 0x80052990
text 2C3C50 (r, g, b), 0x80052994 frame 502800, 0x80052998 current-car marker A05014; used cars 0x800529D0 base 000000,
0x800529D4 text 2C3C50, 0x800529D8 price 2C3C50, 0x800529DC frame 502800. List objects 0x800A8D60 (garage) /
0x800A8D68 (used cars): s16 count, s16 chosen row (0x800A8D62 = the garage slot of types 0xAA / 0xAB), u32 item. The
row highlight's pulse ("flash"): s16 0x800B9508 / 0x800B950C.

Callback commands (block: +0 widget, +4 OT or pad; command 4 also +0xC x, +0xE row centre y, +0x10 alpha, +0x12 rows
drawn, +0x14 enabled): 0 reset, 1 reveal, 2 close, 3 every row every update, 4 draw a row, 5 the row the selection
leaves, 6 the row it enters, 7 open, 8 enabled? (0: alpha halved, choose returns -4). Both callbacks return 1 and act
only on 4 (draw) and 6 (flash = 0).

### 9.2 Update

0x80013EEC calls per popup mode (view +0x1CE): 0 (page) both lists with (pad null, active 0); 1 / 2 the garage /
used-car list with the view pad (null while the input hold-off +0x19C > 0) and active 1. 0x8002055C / 0x80020A94:
flash = -1 when not active, else flash + 1 (0 after 60); count <= 0 -> -1; r = 0x8006CFC4: -3 -> sound 6, -1; -4 ->
-1; -2 -> -1 (garage: pad +4 bit 0x10000 = start -> 0x8001EF10(0x801CD554, selection, 0) moves that car to slot 0,
sound 1; with pad null the original reads RAM word 4, 0 in every dump); -1 -> -2; >= 0 -> list +2 = r, r. The view:
-2 -> popup mode 0, sound 2; >= 0 -> 0x80014380(view, list item, 0): type 9 shows garage slot 0x800A8D62 (0x80014348,
0x80017288) and goes to +0x0C; type 0x50 shows the lot car (0x80020BDC, paint 0x80018350 of 0x80020C00), starts
transaction 1 at 0x80020C24's price (0x8001DAA8) and goes to +0x0C.

0x8006CFC4(w, pad): active = 0; command 3 per row; scroll one step towards 0; blink + 1 (0 after 60). State < 0: -1
returns -2 at once; else state + 1 (command 2 for the selection at -58), fade - 1 (>= 0), returns -2. Open: fade + 1
(<= maximum), state + 1 (0 after 45); with a pad: active = 1; pad +4 & 0x500 (triangle / square) -> -1; & 0xA00 (cross
/ circle) -> the selection, or -4 when command 8 returns 0; else bits = pad +4 | pad +0xC: up (1) selection - 1 (below
0: count - 1 with wrap, else 0), down (2) + 1 (past the end: 0 / count - 1); with flags 0x10 left (4) selection -
visible - 1 (>= 0) and right (8) selection + visible - 1 (<= count - 1); a changed selection (left / right: always) ->
-3, scroll -8 (up / left) or +8 (down / right), blink 0; commands 5 (old row) and 6 (new row) after any direction.
Then the reveal: while revealed < count: delay - 1, stop while > 0; command 1 (revealed), delay = period, revealed +
1; go on only while the delay is negative. Pad words (view +0x178, 0x80083998's copy of what 0x800838B4 accumulates):
+0 held, +4 pressed, +8 released, +0xC auto-repeat; logical bits = raw pad bits mapped by EXE 0x800A6F3C (1 up, 2
down, 4 left, 8 right, 0x10 L1, 0x20 L2, 0x40 L3, 0x100 triangle, 0x200 cross, 0x400 square, 0x800 circle, 0x1000
R1, 0x2000 R2, 0x4000 R3, 0x10000 start, 0x20000 select). Checked: square closes the open list (capture
`uc_square_3200` = the page-mode list of field 2900), circle chooses (`uc_circle_3300`: the car page).

### 9.3 Rows

Used cars: the 8-byte lot entry {u32 car id, u32 price (& 0xFFFFFF), s8 paint at +7}. Garage: slot i = 0x801CD558 +
i * 0xA4: +00 car id (names), +04 paint character, +8C model id (whose paint list colours the chip), +98 bit 15
(racing modification: the u16 string 0x80050BB0 = glyph 0x7F before the name; 0x80050BAC = "" otherwise); the marker
row = the current car 0x801D156C. Names: 0x800182A8 / 0x800182FC = catalogue (table 30) row +3C / +3E -> unistrdb
string (0x80076C14), "No Name" (0x80050B9C) without a row. Chip colour 0x80060D28(car, paint): the .carinfoa chip
colour of the first paint whose character equals the paint (letters compared lower-case, 0x80060C90), index 0 when none.

### 9.4 Drawing (0x8006D50C)

Window: half = visible >> 1, first = selection - half clamped to [0, count - visible], y = top + (scroll * pitch
(+7 if negative)) >> 3, rows first .. first + visible - 1 with centres y + pitch * k + (rowHeight >> 1). While
scrolling one more row is drawn and the window keeps still at the ends of the list; the row entering the view gets
alpha 128 - |scroll| * 16, the row leaving it |scroll| * 16 (the exact branches: `MenuListDraw`). Per row: the own bar
(flags 8, the selection, state >= 0), command 8, command 4. Then, when state >= 0 and active: the arrows (r = 255 * k /
10, g = r / 2, k = clamp(40 - state, 0, 10), state 0 with flags 0x20) above at (x, top - gap, apex 10 up) when first
> 0 and below at (x, top + pitch * visible, apex 10 down) when rows remain below; then E1 0x20 (additive).

Row (command 4) with x = the anchor x, c = row centre y + 4, alpha of the row:

| Element | Garage (0x80020198) | Used cars (0x800206F8) |
|---|---|---|
| highlight (flash >= 0, the selected row) | bar at (x, c - 14) 384 x 20, then E1 0x220 | bar at (x, c - 16) 416 x 24, then E1 0x220 |
| name (UTF-16, font A, 0x8001F38C) at c | [prefix] model at x - 152, grade 5 px after the model | model at x - 188, grade 5 px after |
| paint chip (x, y, 9, 12) | (x - 165, c - 9), colour of slot +8C / +04 | (x - 201, c - 9) |
| current-car marker | TILE 0x60 (x - 184, c - 8) 14 x 8, marker colour | - |
| price | - | with commas (0x8001FCDC), right-aligned at x + 200 |
| frame | outline (x - 192, c - 14) 384 x 20, frame colour | outline (x - 208, c - 16) 416 x 24 |

Colours = base -> text / price / frame / marker at alpha / 128. Highlight bar 0x8006B814(rect centred at x, c0
0x141414, c1 0x505050, t = flash * 128 / 60): bar = w * min(t, 16) / 16; mid = c0 -> c1 at clamp(64 - t, 0, 48) / 48;
quads (POLY_G4 0x3A, left colour at v0 / v2) (left, bar, c0 -> mid) and (left + w - bar, bar, mid -> c0), and while
t <= 16 also (left + bar, w - bar, mid -> c0) and (left, w - bar, c0 -> mid); then E1 0x20, | 0x200 (dither) while t <=
64. Paint chip 0x8006BB08(rect, colour15, alpha): POLY_G4 0x38 inset by one pixel (x + 1 .. x + w - 1, y + 1 .. y + h
- 1) with grey alpha * 0xF4 >> 7 at v0, the chip colour (5-bit channel * alpha >> 4) at v1, black at v2 / v3; a black
TILE 0x60 of the whole rectangle; E1 0x200. The outline: polyline (x, y) (x + w - 1, y) (x + w - 1, y + h - 1)
(x, y + h - 1) (x, y).

GPU order: everything goes into the item pass's OT entry, which prepends, so the list is drawn in the reverse order of
generation - last row first; inside a row: frame, price, E1 0x200, TILE, chip, name, E1 0x220 and the bar's E1, the bar
quads - and after the texts of the items that follow the list item. On all 9 list pages the list item precedes every
text item, so `BuildMenuFrame`'s placement of the `customItem` primitives after all item texts is the original's order
there (a custom item placed after text items would need its primitives inserted at its position in the reversed
item order). Hook: `MenuRenderState::customItem` -> `MenuPopupList::Draw(drawOrder)` for types 9 / 0x50.

### 9.5 Rasterisation

The captures come from our interpreter's GPU (src/machine/gpu.cpp: floating-point barycentric gouraud without dither,
8-bit blending), so the comparisons use `MenuCanvas::Rules::kInterpreter`, which reproduces those rules;
`Rules::kPs1` (the default outside `--compare`) follows the PS1 GPU (edges walked in 32.32 fixed point, 12.12 colour
steps from the leftmost vertex, 4x4 dither of shaded primitives when E1 bit 9, 5-bit blending, fixed-point lines) -
derived from hardware documentation, not checked against a console capture. The differences between the two in the
frames below are only the dithered paint chips and highlight bars.

### 9.6 Verification (2026-09-19)

Captures `work\play\menu\<name>.txt` (+ `.vram.bin`) with `gt2play --script "<route>" --prims <field>` from a cold
boot, R = `1400:cross,2000:right,2030:right,2100:cross,2300:cross,2560:right,2600:cross,3000:right,3040:cross` (the
Mazda used-car list, opened at 3040); B = R + `3100:down,3150:cross,3300:right,3340:right,3380:right,3430:cross,
3600:cross,4050:cross,4300:up,4330:up,4360:left,4390:left,4420:left,4450:cross,4750:cross,5050:cross` (buys the 2nd
used car, back to the map, My Home, the garage page). The list state of the capture's last frame (the one in the VRAM
dump) is read from its primitives (highlight quad widths / colours = flash, row offsets = scroll, arrow colour =
state, highlighted row = selection; `uc_scroll_3180` is instead reproduced by running the update from the open with
the presses) and given to `gt2tool menu-page <disc> <page> --used-cars | --garage [--card work\memcards\gt2_save_1car.mcd
--money 4852] --compare <capture>.txt.vram.bin --side ...` (`--no-cursor --list-active-byte 1 --list-sel S
--list-scroll K --list-flash F --list-state T` for an open list). Frame = VRAM 0..511 x 0..479, interpreter rules;
the PS1-rule counts are only the dithered chips / bars. Side images `side_usedcars*.png` / `side_garage_*.png`.

| Capture | Route | Page | List state | Differing pixels (PS1 rules) |
|---|---|---|---|---|
| maker_2900 | R without `3040:cross` | 1186 | page mode, 25 rows, selection 0 | 0 (224) |
| uc_square_3200 | R + `3100:square` (closes the list) | 1186 | page mode | 0 (224) |
| uc_open_3090 | R | 1186 | open, selection 0, flash >= 31 | 0 (224) |
| uc_down_3104 | R + `3100:down` | 1186 | selection 1, flash 9, state 39 | 0 (5188) |
| uc_down_3140 | R + `3100:down` | 1186 | selection 1, flash >= 31 | 0 (224) |
| uc_mid_3175 | R + down at 3100, 3115 .. 3175 | 1186 | selection 6, scroll 3, flash 5 | 0 (4564) |
| uc_scroll_3180 | same | 1186 | update run: 146 frames, downs at 60 .. 135 (+ triangle at 145: -2) -> selection 6, flash 10 | 0 (4198) |
| uc_scroll_3240 | same | 1186 | selection 6, flash 9, state 37 | 0 (5208) |
| uc_updown_3264 | same + `3265:up` | 1186 | selection 5, scroll -4, flash 4 | 0 (2730) |
| uc_right_3098 | R + `3100:right` (jump) | 1186 | selection 8, scroll 5, flash 3, state 33 | 0 (4587) |
| uc_left_3198 | R + `3100:right,3200:left` | 1186 | selection 0, scroll -5, flash 3, state 42 | 0 (4505) |
| uc_wrapup_3098 | R + `3100:up` (wraps) | 1186 | selection 24, scroll -5, flash 3, state 33 | 0 (4472) |
| uc_wrapdown_3158 | R + `3100:up,3160:down` (wraps) | 1186 | selection 0, scroll 5, flash 3 | 0 (4524) |
| uc_downlast_3218 | R + `3100:up,3160:up,3220:down` | 1186 | selection 24, scroll 5, flash 3 | 0 (4508) |
| uc_bottom_3300 | R + `3100:down:150` | 1186 | selection 19, flash >= 31 | 0 (245) |
| uc_bottomup_3318 | same + `3320:up` | 1186 | selection 18, scroll -5, flash 3 | 0 (4525) |
| uc_down20_3318 | same + `3320:down` | 1186 | selection 20, scroll 5, flash 3 | 0 (4525) |
| uc_hold400_3600 | R + `3100:down:400` | 1186 | selection 4 (the long hold stops repeating) | 0 (224) |
| uc_down5_3618 | same + `3620:down` | 1186 | selection 5, scroll 5, flash 3 | 0 (4550) |
| gar_empty_2700 | `1400:cross,2000:cross,2400:cross` | 919 | no car: "N/A" | 0 (0) |
| gar_car_5350 | B | 919 | page mode, 1 row, current car 0 (rows from the card saved after the same purchase) | 0 (28) |
| gar_open_5420 | B + `5360:cross` | 919 | open, flash 3 (four bar quads) | 0 (3309) |

The garage captures also need item type 0x47 (current car name, 0x8001B818: [prefix] model name, left at (x0, y1))
from the card; `gt2tool` draws it through `dynamicItem`.

## 10. The car view (types 0x0A / 0x11, 2026-09-19)

Code: `src/game/menu/menu_car.*` (camera, floor, logo; exact integer GTE path), `src/gt2view/menu_view.* MenuCarView` (the
car through the race renderer's car items), `tools/gt2game/menu_mode.cpp`. Evidence: our disassembly of
`work\re\gtmode\ram.bin` / `work\re\menu_car\ram_002700.bin` (a `gt2run session` of the new-car route with `snap=2700`: page
object 0x801FF93C, car view +0x28, camera +0x478) and `gt2play --prims` captures (`work\play\car3d\`).

- **Showing a car**: 0x80014348 sets view +0x1B0 = kind (1 = at this frame's draw, 3 = two draws later; 0x9D uses 3); the draw
  0x8001B3AC calls 0x8001D5C8 = camera reset 0x8001D090 (pitch 0xA0, yaw 0x1500, position (0, 0, 0x94CCC) 16.16 m, floor
  on, floor colour 0x525252, +0xCC = 400) + 0x8001AC20(model, colour index, garage index, flag, wheel word, dirt): model /
  texture files by the boot's table 0x801DF5D0 ({u32 car id, u16 .cdo file, u16 logo file}, count s16 0x801C93C8, binary
  search 0x8005D950; texture = .cdo file + 1), paint 0x8001A5B8 (the .cdp paint of the colour index), the car's data block
  (+0x394: 0x8001AFA8 / 0x8001B10C). The wheel shop page calls 0x8001AC20 directly (0x8001AB6C, no camera reset). While a
  car is pending (+0x1B0 != 0) the view update skips its input and the turn.
- **Turn** (0x8001D258, every view update while no page / car / wheel change is pending): pitch = 110, yaw = (yaw + 12) &
  0x3FFF, + 113 more while the car wash timer (page +0x552 / +0x554, 64 updates) runs; the matrices use yaw & 0xFFF (one
  turn = 341 updates).
- **Viewport** (0x8001B9AC case 0x0A): draw environment = the item's rectangle (0x8008034C: E3 / E4 / E5 = (x0, y0) ..
  (x1 - 1, y1 - 1), offset (x0, y0)); camera +0xBC = (0, 0, w, h), window a = ((w * 256) / h) >> 1: (-a, a) x (179, -50) at H =
  800, far 0x7FFF. 0x8007B320: centre ((l + r) / 2 << 12) / H, ((t + b) / 2 << 12) / H (0, 327); 0x8007B374: P = [[sx, 0,
  cx], [0, -sy, -cy], [0, 0, -4096]] with sx = (w << 12) / (r - l), sy = (h << 12) / (t - b), cx / cy = centre * s >> 12;
  OFX / OFY = w << 15 / h << 15, H = 800 (loaded into the GTE by 0x8007B778); camera matrix = P * V^T (MVMVA, >> 12).
- **Camera** (0x8001A8A4): V = Ry(-yaw) (0x8007B14C: [[c, 0, s], [0, 1, 0], [-s, 0, c]]) * Rx(-pitch) (0x8007B0C4: [[1, 0,
  0], [0, c, -s], [0, s, c]]), eye = V (0, 0, 0x94CCC) (0x8008220C, 12-bit split); world +Y up, camera looks along -z. The
  car (0x80067444: LOD 0 forced, wheels, shadow, reflection pass at colour 0x40 with the map of page 9 = the 128 x 128 area
  inside arcade/gt_cursor.tim and CLUT 0x2624) sits at (0, (model +0x18 - model +0x22) * 16, 0) = front wheel radius minus
  the front-left wheel's y (0x80061544), unrotated. Then the floor with the camera without the yaw.
- **Floor disc** (0x8006C274 / 0x8006C31C): 25 points on a 4 m circle in 24 steps (`((1024 * cos) >> 12) << 8`, 16.16 m) +
  the centre, transformed by 0x8007E5E0 (relative to the camera, scaled so the largest component has 13 bits, RTPS with
  TR = 0, depth (IR3 << shift) >> 13); 24 POLY_G3 0x30 (0x32 when camera +0xD1) centre 0x525252 -> rim black, E1 0x200, in
  the car's OT by the largest depth of the three. Reproduced exactly (integer GTE incl. the UNR division): the floor
  pixels of the three captures below are equal. The camera state computed natively equals the dump (V, eye (0, 102225,
  600853), P V^T row 1 = (0, -2592, 230) for the floor pass).
- **Name logo** (type 0x11, 0x8001A654): the TIM of the table's logo file: CLUT to (576, 164), image to (576, 165) (their own
  sizes); SPRT 0x808080 (w * 4) x h at uv (0, 0xA5), tpage 9, CLUT 0x2924, centred on the item. Logo file =
  carlogo/<id>n--.tim, else <id>l--.tim (the race cars), else the directory's first file a-a7rl--.tim (26 cars) - this rule
  gives all 1110 entries of the table.
- **Draw order**: 0x8001B9AC draws texts into the view's OT +0x98 (`param_2`) and the colour name 0x07, the logo 0x11 and
  the chips 0x94 into +0x88 (`param_4`, which gets the cursor first); the GPU draws the page, the +0x98 items, the car
  environment (floor, car; clipped to the viewport), then the +0x88 items and the cursor (`MenuFrame::layer3dAt`,
  `MenuRenderState::lateItem`). Natively the page quads before the layer are drawn at the far depth, the car is
  depth-tested (the PS1 sorts it by OT), the rest in front.

Verification (Vulkan 512 x 480 vs the original's VRAM; the car's pixels = those differing from the same frame without the
car; the native field is chosen where the yaw equals the capture's RAM value):

| Capture | Page / car | Yaw | Differing pixels | Car silhouette ours / original | IoU |
|---|---|---|---|---|---|
| `newcar_2700.txt` (new-car route) | 1162, RX-7 LM (a2l7r, paint 0) | 7428 | 8606, all inside the car | (133,170)-(346,246) / (131,170)-(343,250), centroid (229.2, 211.5) / (228.5, 212.9) | 0.912 |
| `newcar_2800.txt` | same, 100 fields later | 8628 | 8617 | (171,170)-(356,254) / (164,171)-(360,255) | 0.906 |
| `garcar_5600.txt` (route B + `5360:cross,5420:cross`) | 930, garage car a2bsn paint 3 (Blaze Red) | 7272 | 11971 | (114,161)-(364,257) / (114,161)-(362,260) | 0.934 |

Logo, floor disc, paint chips, texts and the colour name are pixel-equal in all three; the remaining differences are the
car's rasterisation (z-buffer vs OT order, texture sampling) and its shadow's lower edge (3 - 4 px). Side images
`work\play\car3d\*_side.png`. Fitted wheels on the displayed car (0x800615E8 / 0x8001AEF8, carwheel/
TIMs): done 2026-09-19, `docs/research/menus_gtmode.md` 8.3 (gt2verify WheelFile; frame not captured). Not done: the car wash's `+0xD1` semi-transparent floor was not captured.

## 11. Remaining item types and the career actions (2026-09-19)

Item types (0x8001B9AC; OT +0x98, colour 0x6E6E6E, font by flag bit 31 unless noted; `MenuRuntime::DrawDynamic`):

| Type | Draws |
|---|---|
| 0x48 / 0x49 | power before / after the part (transaction +0x1C / +0x20 from 0x8001DB90 = `career::PreviewPart` at the page load): "%dhp" (data-global 0x801EF6C6) of p * 1000 / 1014, UTF-16 right at (x1, y1), colour 0x145A78; 0x49 without a part: "----" (0x80023D08) 8-bit left at (x0, y1) |
| 0xB8 | engine texts: unistrdb(block +0x1C) left at (x0, y1), then unistrdb(block +0x24) 12 px after it (0x8001B2B8 / 0x8001B338; "" without a car) |
| 0xBA | torque t (kgm x 10) < 1: "----"; else v = t * 72329 / 10000, "%d.%dlb-ft / " (0x801C30FA) of v / 10, v % 10 8-bit left, then the rpm text (unistrdb id of the engine row +0x34 for dealer cars, the "%drpm" buffer for garage cars; 0x8001B378) right after it |
| 0xBC | 0x80019D38: events whose result nibble is 1 among the menu's 248, v = n * 10000 / 219, "%d.%02d" (0x80023D00) 8-bit right |
| 0xBD / 0xC3 | 0x8001FE0C(hi, lo): "0", lo with commas, or hi then lo as 8 digits with commas: prize total (record +0x58 / +0x5C); garage value (sum of slot +0x90, carried once past 100,000,000) - UTF-16 right |
| 0xBE / 0xBF / 0xC2 | record +0x48 / wins +0x4C / garage count, "%d" 8-bit right |
| 0xC0 / 0xC1 | min(wins, 0x68DB8) * 10000 / record +0x48 and positionSum * 100 / races (unsigned, 0 without races), "%d.%02d" |
| 0xC4 / 0xC5 / 0xC6 | machine-test best speed / most powerful car / best time: no page uses them (not drawn) |
| 0xCD | 0x80017318(ot, 0x20, 0xA0): for the entries {s16 kind, base, dx, dy} of ovl4 0x80050AB0 (until kind < 0): stage = sheet +0x17B0[kind] of the garage car chosen in the list (0x80017288), stage > 0 -> unistrdb(base + stage - 1) left at (0x20 + dx, 0xA0 + dy), colour 0x606060, font A |

Verification (`gt2game --menu --window 512x480 --menu-square --menu-compare`, captures in `work\re\menu_items\` and
`work\play\menuitems\`): Game Status 918 new game and with one car (`status_new_2700`, `status_1car_5650`) 0 / 0 differing
pixels; SPEC of the dealer car 793 (`spec_793_3150`) 0; SPEC of the garage car 791 (`spec_791_6300`) 0; Equipped Parts 792
(`parts_792_6700`, stock car: nothing drawn) 0. Not captured: 0xCD with fitted parts, 0x48 / 0x49 against the original.
Also unverified: type 0x9B may be the part name of transaction +0x18 (0x800183B8), not a paint name.

Career actions wired into the runtime (the career model's `src/game/career/tuning.h`, `events.h`): the tune sheet of the
current car = `LoadCarSheet` (0x800173E8: clear 0xFF, rows, configuration); the part page load runs 0x8001DB90
(`PreviewPart`: price, owned, power before / after) and 0x8001DB24 (a part kind of 9 also loads the garage list, as the
original compares the kind with 9 / 0x50); 0x8001DDAC: buying kinds 7 / 9 / 0x20 fits at once (message 6), 0x12 / 0x13 /
0x14 / 0x22 fits at once (message 0x1F), others ask (message 7, state 4) and state 4 fits (0x80017D6C = `FitPart`, message 9
for kinds 6, 0x10, 0x11, 0x1A..0x1F, 0x2E..0x31); the wheel shop (bit 28: `WheelIdOfCode`, `WheelColour`, transaction 5 at
2000, check 0x800181D0 money >= price -> message 0x20 / 0x01, run 0x80018100 = `BuyWheels`, page load 0x8001DAF0 /
0x8001AB6C); licence tests 0x80019B88 = `LicenceEntryCheck` (admitted tests are handed to the race hook, which runs events
only - a licence test runs with `gt2game --license`); racing-modification bodies 0x9C -> `FirstRacingBody` (shown), 0x9D ->
`NextRacingBody` (shown after the delay of kind 3). Types 0x95 / 0x96 are the paint cycling of the car pages (0x8001A530),
not settings. Checked end to end: `gt2game --menu --career work\memcards\gt2_save_1car.mcd --menu-page 1114 --menu-script
"30:down,60:right,90:cross,150:cross,220:cross"` buys Sports Brakes (4,800) for the Protege: confirmation 779, "bought - fit?"
809, yes -> fitted (config +0x04 brakes row 17 -> 18), "Purchased" on the page, money 52 (`work\play\menu2\part_*.png`);
page 1107 shows 175hp -> 184hp for the intercooler.

Menu sounds and music: `docs/formats/sound.md` section 8.
