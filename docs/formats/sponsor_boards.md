# Sponsor boards (`.crstims.tsd`)

Status 2026-09-19. Derived from the bytes of US Simulation v1.2 (SCUS_944.88, EXE SHA-1
3030aa271c0a4022fc69ce09d76a6bc75e69a32a; race overlay = GT2.OVL member 0) and from the race-load routine
0x800275E8 (our disassembly / Ghidra export of `work/re/race_load`), checked against the VRAM of the captured
Seattle attract race (`gt2play --prims 3600`). Parser and placement: `src/gt2formats/sponsor_boards.*`; used by
`SceneAssets::UseTrack` (gt2game, gt2play). No external reference was used.

At race load the original writes sponsor logos over the course's own billboard textures (the `.trp` defaults, e.g.
"TOYOTA / BRIDGESTONE" on Seattle): 19 fixed VRAM slots in five size groups, filled at random from a category of
the file.

## File (GT2.VOL root `.crstims.tsd.gz`, 159,072 bytes inflated)

| Offset | Field |
|---|---|
| 0x00 | u16 recordsPerCategory = 90 (= the number of logos) |
| 0x02 | u16 categoryCount = 10 |
| 0x04 | u16 logosPerGroup[5] = 47, 15, 14, 8, 6 |
| 0x0E | u16 0 |
| 0x10 | `categoryCount` categories of 376 bytes: char name[16] + 90 x {u16 chance, u16 logoId} |
| 0xEC0 | 95 TIMs (4-bit, 16-colour CLUT): 5 blank templates (one per group), then the 90 logos group by group |

Categories: General01, General02, One-Make, JP, US, UK, DE, FR, IT, TUNE. Record i of a category belongs to logo i;
`chance` is out of 4096 (0 .. 4096; 0x555 = 1/3 etc.), `logoId` 1..81 (the maker / brand; id 0x13 is never
placed). Group image sizes (16-bit words): 24 x 32, 16 x 64, 18 x 40, 18 x 32, 24 x 32 (TIM strides 0x640, 0x840,
0x5E0, 0x4C0, 0x640). The TIMs' own VRAM destinations are not used.

## Slots (race overlay 0x8002F5A4: 5 x {u32 slot list, u16 clutX, u16 clutY, u16 TIM size, u8 slotCount})

| Group | CLUTs | Slots (image x, y in VRAM words) |
|---|---|---|
| 0 (24 x 32) | (384, 496 + s) | (640,64) (664,0) (640,0) (664,32) (664,64) (640,96) (640,32) |
| 1 (16 x 64) | (384, 503 + s) | (688,64) (688,0) (688,128) |
| 2 (18 x 40) | (384, 506 + s) | (658,176) (640,216) (640,176) (658,216) |
| 3 (18 x 32) | (384, 510 + s) | (686,192) (686,224) |
| 4 (24 x 32) | (400, 496 + s) | (640,128) (664,128) (664,96) |

## Selection (0x800275E8, called with the course index)

1. No boards when bit 6 (0x40) of the course's `.crsinfo` flags is set (e.g. TC_lisence - the licence capture's
   VRAM has no logo).
2. Category = "General01" when the byte 0x801D5865 is 0, else the string at 0x801D58A0 that the menus set (the
   attract race: 0x801D5865 = 1, "General02"); the loader keeps the last category of that name.
3. Generator (0x80083AE0): `state = state * 17 + 17; draw = state ^ (state rotated by 16 bits)`, seeded with
   0x801D58B0 - set to VSync(-1) (the frame counter, 0x8007D23C) at the load of a player's race, kept as it is when
   0x800A951C != 0 (attract race / replay; the attract dump has 0x14D57).
4. For each group g in order: a random permutation of its slots (per slot index i: draw until `draw % slotCount`
   hits an unused position, which gets i); then for each logo j of the group, while slots remain: one draw; the logo
   is placed when `(draw & 0xFFF) <= chance[j]` and `logoId[j] != 0x13` - image to the next slot of the permutation,
   CLUT to (clutX, clutY + slot). No more draws once the group's slots are full. Remaining slots get the group's
   blank template.

Check: with category General02 and seed 0x14D57 the rule places logos 5, 6, 14, 16, 29, 30, 32 / 56, 61, 65 /
68, 70, 71, 75 / (templates) / 89, 91, 92 - all 19 slot images and CLUTs equal the captured VRAM of the attract race
word for word. gt2play reads category and seed from the running original and reproduces its boards; gt2game uses
General01 (`--sponsors <category>`) with a clock seed like the original (`--sponsor-seed N`; screenshot runs use
0x14D57 for reproducibility).

## Category of the GT-mode races (2026-09-19)

The menus clear the race block and strcpy the name pool string of the event / licence row's u16 + 0x94 into 0x801D58A0 with
+ 9 = 1 (ovl4 0x80010078 licences, 0x80010B64 / 0x80012D74 events; the race overlay's copy 0x8004C7A0 for the licence menu's
selector): events "General01", "General02", "One-Make", "0", ... (`gt2tool events`: "tag"), licence tests "0" (e.g. LJB00 / LJB01),
"General01" (LIA00, LJB06), "General02" (LJA05). 0x800275E8 places no boards when no category of the file has the name (the
course's own billboards stay) and then leaves the seed + 0x54 unwritten (0 in the licence dumps). Checked on the dumps: LJB00 "0"
seed 0, LJB06 "General01", CBM0001 "General01", GT30501 "0" seed 0, MSC0002 "General02". gt2game: the race view's category is the
row's tag (career_race.cpp), the saved replays carry it (race_screens.md 5.6).
