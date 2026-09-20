# Course race data, course table and the race overlay's tuning tables

Facts of the US Simulation v1.2 disc (SCUS_944.88, SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), established
2026-09-18 with `gt2run watch` on the race load, the load-phase call graph (work\re\race_load) and the objdump of
the EXE / overlays. Parsers: `src\gt2formats\course_data.*`, `src\gt2formats\overlay_data.*`; derivations:
`src\game\sim\disc_data.*`; verification: `tools\gt2verify\verify_data.cpp` (rows ExeImage, OvlImage, CrsInfo,
CourseId, SimConst, TroHeader, RaceData, RaceRecs - all byte-equal to the RAM dump of the attract race).

## Modules on the disc (overlay_data.h)

| Module | Location | Load address | Notes |
|---|---|---|---|
| SCUS_944.88 | disc root | t_addr `0x80010000`, t_size `0x99000` (PS-X EXE header, image at file + 0x800) | address A = file offset `0x800 + (A - 0x80010000)` |
| GT2.OVL member i | disc root; table of `{u32 offset, u32 packedSize}` at offset 0, 6 gzip members | `0x80010000` (all members; loader 0x8005DAD8) | member 0 = race overlay: code `0x80010000..0x8005A77C`, tuning tables `0x80046C94..0x80046FC8` |

The race simulation reads the executable's tables at `0x800923E2` (automatic-gear table) and `0x800A4AC8` (arc-tangent
table) and the overlay's tables listed in `SimConstants` (race_sim.h). The values at `0x80046DB0..`, `0x80046E00..`,
`0x80046EF0..`, `0x80046F3C..`, `0x80046F48..`, `0x80046F88..` and the BSS objects `0x801C8730` / `0x801C8690` are NOT
plain file bytes: at race load `0x8003C12C -> 0x8003BA64 -> 0x8003B7B8` derive them from the raw byte tables at
`0x80046E20..0x80046EA4`, `0x80046ED4..0x80046ED9`, `0x80046EE5..0x80046EE7`, the shell's race settings block
`0x801C98A0..` (AI grip / scale percentages, tyre-wear parameters) and the course's dirt flag; `0x8001523C` sets the
frame constants `0x801C856C / 0x801C8570` from the frame-rate mode `0x801D5864`. `sim::LoadSimConstants` is the port.

## `.crsinfo` (GT2.VOL root): the course table

Header `"CRS\0"`, u16 version (2), u16 count (126). Entries of 24 bytes: u32 offset of the display name (NUL-terminated,
inside the file), u32 course file id, u16 flags (bit 2 = dirt course), u16 backdrop index (bytes 10..11: index into the
`bgsobj/*.bso` files in VOL directory order, see backdrop_bso.md), 12 bytes not decoded. The boot block loads the
file whole at `0x801E18E0` and relocates the name offsets (`0x80011C70`); `0x80060E94(index)` = `0x801E18E8 + index * 24`.

Course file id = `0x80083004(name)`: `h = rotl32(h, 6) + byte` over the base name of the course file (the part before
the first '.'). The boot block (`0x80011B70`) hashes every file of the VOL's `crsmap` directory into `0x801E33F0` and
`0x80060FB0(id)` maps an id back to that file; the same base name is `crsobj/<name>.tro`. Several entries may share
one file (mode variants); `CourseInfoTable::FindByFileName` returns the first.

## Race data inside the `.tro`

The course `.tro` is inflated whole to `0x800B4A34`; the header offsets `0x10..0x20` become pointers there.

| Header | Content |
|---|---|
| +0x24 | s32 count of start lines (0..3 on the corpus) |
| +0x28 | s32[count] course distances of the start lines, 16.16 m (`0x800B4A58` in RAM) |
| +0x54 | u32 start angle (& 0xFFF; Ry of every grid car, 0x80012CD4) |
| +0x58 | s32[3] x 16 grid slot positions (16.16 m): where the car's nose is (0x80017E74 moves the car back by its LOD 0 nose); slot 0 is also the chunk-search seed of the loader and of 0x80012CD4 (replay.md section 5) |
| +0x20 | offset of the race block |

Race block: `{ s32 size; u32 listCount; u32 listOffset[listCount] }` (offsets relative to the block, 0 = absent) then
the lists `{ s32 count; Record[count] }`, records of 0x28 bytes (`TrackRaceRecord`). On the disc only +0x00 type
(0 straight, 1 corner), +0x04/+0x08 position (1/4096 m), +0x18 signed radius and +0x24 heading carry data; the loader
`0x80038DA0` fills +0x0C height, +0x10 chunk, +0x12 surface, +0x13 attribute, +0x14 course distance, +0x1C / +0x20
slopes from the course geometry (walking the lists in the order of the byte table `0x80046DF8` = 6, 0, 1, 2, 5, 3, 4;
list 4 is the grid list whose distances behind the line are wrapped by the course length), rewrites corner types by the
sign of the radius and finally negates the size word. `sim::BuildRaceCourseData` is the port. 125 of the 126 courses
have 7 list slots (`maxspeed` has 6); licence courses carry a single list, some have no start line.
