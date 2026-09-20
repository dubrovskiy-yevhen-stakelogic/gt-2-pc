# Course backdrop (.bso / .bsp)

Status 2026-09-18. Derived from the bytes of US Simulation v1.2 (EXE SHA-1 3030aa27...) `bgsobj/*.bso` (all 34
files parse to their last byte, `ParseBackdrop` throws otherwise) and from the original's GTE / GP0 traffic on the
Seattle attract race (`gt2play --prims 3600 work/play/prims_3600.txt`: one 1028-vertex transform = 257 quads x 4
corners whose vertices are exactly the `sea_ha_e.bso` vertex block). Parser: `src/gt2formats/backdrop.*`.
No external reference was used.

## Which backdrop a course uses

`.crsinfo` entry (24 bytes, see course_race_data.md) bytes 10..11 = u16 index into the `bgsobj/*.bso` files in
VOL directory (= sorted) order. Verified: Seattle (both lengths) -> 24 `sea_ha_e`, Rome -> 18 `roma_sky`,
Rome-Night -> 19 `romadark_sky`, Laguna Seca -> 9 `lagunasky`, High Speed Ring -> 31 `tl_sky4`, Grindelwald -> 14
`noon`, Trial Mountain / Test Course -> 11 `mskyX`. Values seen: 0..32; 34 files exist.

## .bso

| Offset | Field |
|---|---|
| 0x00 | `"BG\0\0"` |
| 0x04 | u8 sky R, G, B, u8 0x28 - the colour word (rgb + POLY_F4 code) of the fill above the horizon |
| 0x08 | u8 ground R, G, B, u8 0x28 - the fill below the horizon (Seattle: sky 77 7E A4, ground 58 58 58) |
| 0x0C | u32 vertex count |
| 0x10 | u16 count[8] by libgpu type: F3, F4, G3, G4, FT3, FT4, GT3, GT4 |
| 0x20 | vertices: { s16 x, y, z, 0 } |
| after | polygon records, list by list |

Record = `u32 w0, u32 w1` + two copies (double buffering) of the ready-made libgpu packet.
`v0 = w0 & 0xFFF, v1 = (w0 >> 12) & 0xFFF, v2 = (w1 >> 12) & 0xFFF, v3 = w1 & 0xFFF`, `w0 >> 24` = flags (0x80 on
1685 of 5292 records, meaning unknown). Packet: `u32 tag` (word count << 24: 4 F3, 5 F4, 6 G3, 8 G4, 7 FT3, 9 FT4,
9 GT3, 12 GT4), `u32 rgb | code << 24`, then per corner `[u32 rgb]` (gouraud, corners 1+), `u32 xy` (0 in the
file), `[u32 uv | clut/tpage << 16]` (textured). Record sizes: 48 F3, 56 F4, 64 G3, 80 G4, 72 FT3, 88 FT4, 88 GT3,
112 GT4. The vertex indices are in ring order; the packet's corners are the GPU's (v0, v1, v3, v2) of the ring, so the
packet's colour / UV slot 2 belongs to ring vertex 3 and slot 3 to ring vertex 2 (quads). Checked 2026-09-19 on the
licence test (`mskyX_2`, field 5300): projecting the file with the captured backdrop transform (rows [0 0 4095;
17 -3723 0; 4095 19 0], h 216) reproduces 64 of the frame's 66 backdrop primitives with exactly these corner colours
and UVs. Reading slot i for ring corner i (our renderer until then) swapped the dome's gradient and the mountain
panels' texture halves (the "slabs" and the hard diagonal in the sky).

Vertex space: model x = -world X, y = up, z = -world Z, centred on the camera (the transform the original uses
is the camera rotation with its first column negated and a zero translation; sea_ha_e spans x +-4073, y -459..3266,
z -4001..4100). Units are arbitrary - the dome is only ever seen at infinity.

Draw order in the original (field 3600): F4 fill below the horizon (ground colour, y >= horizon line), F4 fill
above it (sky colour), then the dome (gouraud band, one semi-transparent textured cloud quad with tpage blend 3
(B + F/4), the textured city panels), then the course.

## .bsp

Same TIM pack as `.trp` (`u32 count` + TIMs with their VRAM destinations); loaded next to the course pack. The
dome's textured polygons address these pages (Seattle: tpage 0x1B / 0x1C, i.e. VRAM x 704..832, y 256).

## Our renderer

`SceneAssets::UseTrack` loads the backdrop of the course (`.crsinfo` -> index -> name), builds the ground fill as a
disc 460 units below the camera (the dome's lowest ring is at -459, so the disc's horizon coincides with the
original's fill edge), draws the opaque dome first and the blended layers (per PS1 blend mode) after the cars; the
sky colour is the clear colour. Model matrix: translate to the camera, mirror x and z (`BackdropModel`).

## Open

Flag 0x80 in w0 (on mskyX_2 exactly the 80 textured mountain panels carry it); the `.crsinfo` bytes 12..23;
whether any course draws the backdrop with a translation.
