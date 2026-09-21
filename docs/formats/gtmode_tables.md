# GT-mode menu data: used cars, car catalogue, events, strings, colours, menu containers

Status 2026-09-18. US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; GT-mode menus
= GT2.OVL member 4, loaded at 0x80010000, "ovl4" below). Everything here comes from the bytes of the files named below
and from our own objdump of the EXE / ovl4. No community document was used. Addresses are guest addresses of that
build. Parser: `src/gt2formats/gtmode_tables.*`; tool: `gt2tool used-cars | events | car-prices | menu-data <disc>`.

## 1. Where the files come from

EXE file table 0x8009118C: `char* path[248]`, index = VOL file number used by the loaders 0x8005D8A0 (raw) /
0x8005D8D4 (gzip-aware). Relevant indices: 0 `/.carcolor`, 1 `/.carinfoa`, 4 `/.ccjapanese`, 5 `/.cclatain`,
9 `/.usedcar`, 10 `/.usedcar_jpn`, 11 `/.usedcar_usa`, 103..106 `carparam/usa_{gtmode_data, license_data,
gtmode_race, unistrdb}` (the language columns of EXE 0x800925A4 are {gtmode_data, arcade_data, license_data,
unistrdb, gtmode_race}; language byte 0x801C98E0, 1 = usa), 195 / 196 `gtmenu/commonpic.{dat,idx}`, 197..224
`gtmenu/<lang>/{gtmenudat.dat, gtmenudat.idx, iconimg.dat.gz, solodata.dat.gz}`. ovl4 0x800529E4 is the menu language
table, 8 bytes per language: u16 file index of {gtmenudat.dat, iconimg, solodata, gtmenudat.idx} (usa = 221, 223,
224, 222).

Load order at the GT-mode entry (ovl4 0x80013658..): 0x800222E4(1, buf) loads `.carcolor` then `.cclatain` (argument
0 would load `.ccjapanese`); 0x800224E0 the used-car period; 0x80076D74 `usa_gtmode_data.dat`; 0x80076CF8
`usa_gtmode_race.dat` (row sizes EXE 0x80092490 = 156, 96, 128). `.carinfoa` is loaded by 0x800609F8 (file 1 fixed;
pointer at 0x801C93D8). The menu containers are loaded by ovl4 0x80020D90 (commonpic.idx + gtmenudat.idx),
0x80020E4C (solodata) and 0x80020ECC (iconimg).

## 2. `.carinfoa` directory word (menu use)

Directory `{ u32 packedId; u32 word }` at +8 (see car_info.md), sorted by id; 0x80060A24 = binary search. The word:

| Bits | Meaning | Evidence |
|---|---|---|
| 0..17 | offset of the car's entry | 0x80060A88 `word & 0x3FFFF` + base |
| 18..22 | paint count - 1 | 0x80060A88 returns `((word >> 18) & 0x1F) + 1`; equals the entry's paint list length for all 1110 cars |
| 23..26 | region exclusion mask | 0x80060B30 returns `(word >> 23) & 0xF`; 0x80060B70(id) is 1 (car available) when the language's bit is clear: language 0 bit 0, 1 bit 1, 2 bit 3, 3..6 bit 2 |

## 3. `.usedcar_usa` ("UCAR", 123,780 bytes inflated; `.usedcar` / `.usedcar_jpn` have the same layout)

```
0x00 "UCAR", u32 0
0x08 u32 periodOffset[61]            periodOffset[0] = 0xFC, periodOffset[60] = file size
period p (at periodOffset[p]):
  +0x00 39 x { u16 listOffset; u16 count }   listOffset from the period start; first list at +0x9C, lists contiguous
  lists: count x 8-byte entries
entry: +0 u32 packed car id; +4 u32 & 0xFFFFFF price (credits); +7 u8 paint id (a character of the car's paint list)
```

- 0x800224E0(counter, buffer): loads file 11, copies period `counter % 60` (0x88888889 division) to 0x800B9544, then
  for each of the 39 lists keeps only the entries with 0x80060B70(id) != 0 (region filter) and rewrites the counts.
  The caller ovl4 0x800136B0 passes `u32[0x801C99D8] / 10`. The runtime trace identifies
  0x801C99D8 as the career day counter (starts at 1), so the lot changes every 10 days and repeats after 600.
- The list index is the manufacturer index of the catalogue (section 4, row + 0x3A): only lists 7, 11, 18, 22, 23, 30,
  31, 33 are non-empty, and every car id in list k has catalogue maker k (0 exceptions).
- Display row ovl4 0x800207F8: `lw +0` id, `lw +4 & 0xFFFFFF` price, `lb +7` paint -> 0x80060D28(id, paint) colour chip.
  The high price byte is used in 82 entries (prices above 65535). Every list is sorted by the 24-bit price (0
  exceptions in 14,271 entries); every paint id is in the car's .carinfoa paint list (0 exceptions); 656 entries are
  hidden in the US by the region mask.
- Cross-check (day 1 = period 0, maker 18): the first nine entries have the prices 3341, 5148, 6905, 7084, 7696, 7771,
  7971, 7985, 8312 of the reference lot, in that order; the second entry's paint 'b' resolves through .carcolor /
  .cclatain to the colour name seen at runtime.

## 4. `carparam/usa_gtmode_data.dat`: tables 29 and 30 (the catalogue)

The GTDT has 62 entries; entries 31..61 (the second half) all have size 0. Tables 0..28: car_params.md.

**Table 30** (72-byte rows, 618 rows, sorted by car id, row i has the same car as chassis row i and its chassis field
= i). Lookups: 0x80077F54(table, id) (binary search on +0) and 0x80077DC4(object, 30, id).

| Offset | Field | Evidence |
|---|---|---|
| +00 | u32 packed car id | |
| +04..+37 | u16 row per part table in the order 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, **21, 17, 18, 19, 20**, 22, 23, 27, 28 | mapping 0x80092BB4 (copy into CarConfig); each row belongs to the car (checked for all fixed tables) |
| +38 | u16 row of table 29 -> CarConfig +38 | 0x80092BB4 |
| +3A | u8 manufacturer index 0..38 | = used-car list index (all 14,271 used-car entries of list k have maker k); the ids of one index share their first letter (one index has two); 0x80076FC0 copies it to CarConfig +79 in GT mode |
| +3C / +3E | u16 string indices of usa_unistrdb.dat: model name / grade name | range 8..850 of 851 strings; they compose the car's display name |
| +40 | u8 flag (1 in 88 rows: the 80 ids ending in 'r' and 8 others; their prices are 500000 / 1000000 / 2000000 only) | ovl4 0x8001781C: non-zero -> the car is tested against an event list (ovl4 0x80050B68) |
| +41 | u8 two-digit model year (0 = none) | equals the year in the display name for 312 of 329 names that carry one |
| +44 | u32 new-car price, credits | ovl4 0x800177D4 returns it; 0x80017A70 = price / 4 (the sell price) |

**Table 29** (8-byte rows, 197): indexed by CarConfig +38. 0x80076FC0 sets CarConfig +00 = `u32(row + 0) | (row[7] &
0x1F) << 8` (the attract replay's 0x300 / 0x100 are rows 0 / 10). Rows 0..15 are zero except bytes 6 / 7 (all 16
combinations of 0..3); rows 16.. carry a letter, a flag byte, a counter and a group number. **Table 29 = the wheels**
(2026-09-19): the wheel shop 0x80018100 finds the row of a wheel id with the binary search 0x80077E80 on the u32 at +0
and 0x80021B38 sets CarConfig +38 = row and +00 = u32 | (row[7] & 0x1F) << 8 (row[6] & 0x1F when the u32 is 0 and a
racing modification is fitted) - the wheel model / colour the car is drawn with (CarConfig +00 is not read by the
record builder). docs/research/menus_gtmode.md section 8.1.

**0x80076FC0(spec, config)** (spec = a table 30 row or a race table 1 row): 0x80076A20 (memset 0x84, +79 = 255); in GT
mode (global 0x80092878 == 2) +7A |= 0x40 and +79 = spec +3A; mapping 0x80092BB4 (part rows, +38); then from the rows
the config references (0x80076F2C = row of the loaded parameter file): gearbox +0A..+1B -> +3C..+4D, +21 -> +4E
(0x800928A0); racing modification +12 / +15 -> +52 / +53 (0x800928D8); turbo +0A..+0F -> +54..+59 (0x800928E4);
suspension +0B, +0E, +15, +18, +1F, +22, +23, +24 -> +5A..+63 and the default positions +2A, +2D, +31, +34, +38, +3B,
+3F, +42 -> damper levels +64..+6B, +46 / +4A -> anti-roll levels +6C / +6D (0x80092900); +5E / +5F = 128; brake
controller +0C -> +50 and +51 (0x800928CC); LSD +0D, +17, +10, +1A, +13, +1D -> +6E..+73 (0x8009294C); drivetrain +07
-> +4F; ASM +0B -> +74; TCS +0C -> +75; table 29 -> +00. 0x80076954(id, config) = this on the car's table 30 row (row
0 and return 0 when absent).

Check (`gt2tool car-prices`): for the attract replay's player car (arcade/demofile_us.gmr slot 0) the configuration
0x80076954 builds from the catalogue equals the replay's byte for byte except +76 / +77 (written by the record
builder) and flags bit 7 - including +79 = the maker index (so CarConfig +79 is the manufacturer for catalogue-built
cars). The five AI slots differ in word00 / +38 (table 29 row 10 instead of 5), the tyre rows, +3A (torque %), +79 (50)
and flags bit 0, i.e. they were not built from the catalogue.

Finding for car_params.md: `gt2::StockCarConfig` agrees with 0x80076FC0 on the part rows for only 326 of the 618
catalogue cars. The catalogue fits a **stock turbo** row (table 12, price 0, the car's own id) to 265 cars, and a
brake controller / ASM / TCS row (rows of the generic ids) to 88 cars, where StockCarConfig takes row 0; and the
catalogue's damper / anti-roll / ASM / TCS levels are the rows' default positions (e.g. 7 of 10), not 1.

## 5. `carparam/usa_gtmode_race.dat` (GTDT, 6 entries)

| Entry | Rows x size | Contents |
|---|---|---|
| 0 | 248 x 0x9C | events, sorted by name index |
| 1 | 1220 x 0x60 | opponent cars |
| 2 | 94 x 0x80 | car lists: 32 x u32 packed id, 0-terminated; every id is in the catalogue |
| 3 | 2670 bytes | name pool of table 0 (u16 count = 292; { u8 len; char[len]; u8 0 }): event names, course file names, tags |
| 4, 5 | empty | |

Event lookup by name: 0x800781E0 / 0x8007830C with 0x80092878 == 2 use `*0x80092870` + 8 (this file's table 0) and
its entry 3 as the pool (licence.md section 3 describes the same search). The event name list of the menu is ovl4
0x80050D1C (248 pointers); 0x80019474 builds a 36-byte info record per event at 0x800B5E88 with 0x8001928C.

Event names: `XXXnnrr` = series base `XXXnn` with nn races (the two characters before the race number rr, read as
digits by 0x80018A00; nn < 2 = a single event) - the US disc's series are GT305, GT505, GTW05 and FREECHAMP05, five races
each (docs/research/menus_gtmode.md section 8.1); G400 / G1000 / GMAX are the machine tests (0x8001861C).

**Event row** (the licence test layout, licence.md section 4):

| Offset | Field | Evidence |
|---|---|---|
| +00 / +02 | u16 name pool index of the event name / course file name (the pool name "none" in 102 rows) | 0x80010078: 0x8007816C -> 0x8005E548 / 0x8005E5F0 |
| +04 | 16 slots u32 `(paint << 26) \| opponent`; opponent = table 1 row + 1 (0x800768C0: `& 0x3FFFFFF`, - 1); paint indexes the 6-bit charset "-0-9a-z" at EXE 0x80091620 (0 = '-' in all GT events; licence rows carry a paint). Used slots are leading (0x8007812C counts them), max 16 | 0x80010078, 0x800103FC |
| +44 | settings block (0x40 bytes) copied to 0x801C98A0 | 0x80010078 |
| +44 +00 | start speed km/h (0 = standing: race block +0D = 1) | 0x80010078 |
| +44 +01 | laps -> race block +0F (series: the block 0x801D5DF4 keeps row +0x45 per race, 0x80018A84) | 0x80010078 |
| +44 +03 | required licence 0..6 | 0x8001973C: fails when (6 - 0x800191C4()) < value |
| +44 +31 | 1 = the car must pass 0x8005E874(car, 45) (set only on the dirt-course events: a dirt-tyre requirement) | 0x8001973C bit 0 |
| +44 +32 | car list: table 2 row + 1, 0 = none | 0x8001924C; 0x8001973C requires the car id to be in the list |
| +44 +33 | drive restriction: 1 FF, 2 FR, 3 MR, 4 RR, 5 4WD | 0x8001973C jump table ovl4 0x800239F4 maps 1..5 to the drive codes 1, 0, 3, 4, 2 of the car record's u16 +94 >> 13; the codes are those of drivetrain table +08 (0 FR, 1 FF, 2 4WD, 3 MR, 4 RR by the cars carrying them) |
| +78 | u16 prize[6] x 100 credits by position | 0x8001928C multiplies by 100 |
| +84 | u32 prize car ids [4] (0 = none) | all packed ids of the catalogue |
| +94 | u16 name pool index of a tag string (9 distinct) | 0x80010078 strcpy's it to 0x801D58A0; use not traced |
| +96 | u16 power limit (0 = none) | 0x8001973C fails when the car record's u16 +98 & 0x3FFF (its power) exceeds it |
| +98 | u16 x 100 credits (non-zero in 4 rows: the first race of four five-race series) | 0x8001928C |
| +9A | u8 0/1/2: 1 requires u8[0x800B44E4] == 0, 2 requires != 0 (3 + 3 rows) | 0x8001973C bits 7..8 |
| +9B | u8 0/1/2: 1 requires s16[0x800B5C66] <= 0, 2 requires > 0 (non-zero in exactly the 94 rows with a car list: 54 x 1, 40 x 2) | 0x8001973C bits 9..10 |

The bits of the info record (+1C): bit 0 = +44+31, bits 1..3 = +44+03, 4..6 = +44+33, 7..8 = +9A, 9..10 = +9B;
+1E = power limit; +00 = +98 x 100; +04..+18 = prizes x 100; +20 / +22 = start / count of the car list copied to
0x800B8168. The check 0x8001973C(slot, message) reads the car record at 0x801C98E0 + 0x3C74 + 4 + slot * 0xA4 (u32 +00
car id, u16 +94, u16 +98) and returns 1 or a negative reason (-2 dirt tyres, -3 licence, -4 drive, -5 +9A, -6 +9B,
-7 power, -8 car list).

**Opponent row** (table 1, 0x60): +00..+39 = the catalogue spec (section 4, same order); +3A u16 final drive, +3C
gear auto-final, +3D..+42 LSD initial / accel / decel front, then rear, +43 downforce[2], +45 camber[2], +47 toe[2],
+49 ride height[2], +4B spring[2], +4D damper levels[8], +55 anti-roll levels[2], +57 ASM level, +58 TCS level,
+59..+5B 0, +5C u16 torque multiplier % (100..; CarConfig +3A, flags |= 1), +5E u16 number = row + 1. 0x80076F5C =
0x80076FC0 + mapping 0x80092C24 (those fields) + flags |= 1. Every part row belongs to the row's car.

## 6. `carparam/usa_unistrdb.dat` ("WSDB")

`u32 file size; "WSDB"; u16 count (851); count x { u16 length; u16 utf16[length]; u16 0 }` - ends exactly at the file
size. Contents: part names and the catalogue's model / grade names (table 30 +3C / +3E).

## 7. `.carcolor` + `.cclatain` / `.ccjapanese`

`.carcolor`: `"CCOL00\0\0"`, `u16 offset[n]` with n = (offset[0] - 8) / 2 = 1110 = the .carinfoa car count (same
order), entry = u16 colour-name index per paint (the entry length is 2 x the paint count for all 1110 cars). The
name files: `u16 offset[count]` (count = offset[0] / 2 = 1164), NUL-terminated names; all indices < 1164.

## 8. Menu containers

- `gtmenudat.idx` / `commonpic.idx`: `u32 count; u32 offset[count + 1]`, offset[count] = .dat size. Entry i: the
  loader 0x80021078 (gtmenudat) / 0x8002117C (commonpic) reads `offset[i + 1] - offset[i]` bytes from `offset[i] &
  ~3` (+ the file's sector base, VOL table 0x801E35F0 + 0x10). gtmenudat: 3386 entries, each a single gzip member
  that ends inside that range; inflated, every one starts with "GM\x03\0". commonpic: 458 stored entries, 4-KB
  aligned, magic "GTMP" (not decoded; read by 0x80021968).
- GM page (0x800213C4): `"GM\3\0"; u32 groups; groups x { u16 a; u16 b; 4 + a * 12 bytes; b * 76-byte records }`,
  then `u32 c; c * 76-byte records; u32 -> 0x800A8D78; u32 -> 0x800A8D70; u32 commonpic entry index (< 458 in all
  pages); image block` (0x80021284: u32, u32 k, u32[k] copied to a table, a 64 x 8 CLUT to VRAM 576,248, u32 v and
  pixels to VRAM 640,256, width 128, height ceil(v / 32) * 8). The 76-byte records go to 0x801C3150 (max 64). The walk
  succeeds on all 3386 pages.
- `solodata.dat` (inflated 4564): `u32 n (50); u16 page[n] (padded to 4); u32 m (557); m x { u32 carId; u32 value }`
  sorted by id (0x80020F54 = binary search). 0x800211FC(i): i with bit 31 set -> gtmenudat entry page[i & 0x7FFFFFFF]
  (0x80020FF0). Values are < 3386 (5 ids are not in .carinfoa).
- `iconimg.dat` (inflated 32768): raw VRAM data, 0x80020ECC LoadImage to x 704, y 0, 64 x 256.

## 9. Open questions

- (Answered: table 29 = wheels, see section 4.) Catalogue +40 exact meaning, event +94 tags, +98 (series bonus?), +9A / +9B
  (the menu variables 0x800B44E4 / 0x800B5C66 they test), settings +02 and the rest of the block for events.
- (Answered 2026-09-19, docs/research/menus_gtmode.md section 8, src/game/career/events.cpp PickEventOpponents): events
  pick six opponents at random among the used slots (0x80010A30 / 0x80010714), the licence path 0x80010078 builds one
  car from slot 0. The paint code of a slot (0 = random paint of the car's list) and the racing-modification body of
  the opponent row (table 5 row +0x0E stage -> row +8 model) are used.
- solodata value semantics (probably the car's picture page), GM record contents, GTMP format.
