# Course geometry (.tro)

Status 2026-09-18. Verified on US Simulation v1.2: all 126 `crsobj/*.tro` parse with strict checks
(`gt2tool track-scan`: 14,387 chunks, 1,431,126 vertices, 854,397 polygons; every polygon's prim code
matches its list, every vertex index is in range). Parser: `src/gt2formats/track.*`.
Container skeleton (header, chunk table, chunk field offsets) comes from Nenkai's MIT 010 template.
Everything marked **[new]** was derived here from real bytes and is not in any public source we found.

## File

Pointers are absolute file offsets (base 0 in shipped files: `InstancedObjectOffsets[0]` @0x118 == 0x19C).
Header: magic `@(#)GT-PS`, 0x10 chunk table, 0x14 LOD lookup table, 0x18 LOD data (scenery), 0x1C, 0x20 unknown.

Chunk table: s16 ?, s16 course length in metres, s16 chunk count, s16 uvCount, u32 uvTableOffset, u32 chunkOffset[count].

## Course UV table **[new]**

(The template calls it a vcoord lookup of two Vec3 - it is not.) uvCount x 32 bytes:

    near set: u0 v0 clut16 | u1 v1 tpage16 | u2 v2 | u3 v3      (standard PS1 texture words, 12 bytes)
    u32 extra (0x3F0, 0x7C0, 0x3D0 ... - unknown, probably the near/far switch)
    far set:  same 12 bytes, then u32 0

clut: x = (c & 0x3F) * 16, y = c >> 6. tpage: x = (t & 0xF) * 64, y = ((t >> 4) & 1) * 256, bits 7-8 depth.
Corner i of the set belongs to polygon corner i (ring order). Corpus: 127,711 entries, every textured
polygon's index is in range.

## Chunk

| Offset | Field |
|---|---|
| 0x00 | u16 prev, u16 next chunk index |
| 0x12 | s16 vcoord: metres along the course (consecutive chunks differ by the chunk length, ~39 m on testline) **[new]** |
| 0x18 | s32[3] origin: chunk start on the course line, 16.16 fixed-point metres **[new]** |
| 0x24 | s16[3] direction, 4096 = 1.0 **[new]** |
| 0x2C | s32 extent: added squared to the camera distance of the chunk's GTE precision choice (0x80026BB4; `TrackChunk::extent`) |
| 0x30 | s32[3] centre, 16.16 m |
| 0x3C | s32[22]: contains two sets of 4 corner points (x,z) of the chunk footprint in 16.16 m **[new, partial]**; + 0x44 / + 0x6C = the view-test boxes of the main / mirror view (0x80020EC4): {s32 x, s32 z} x 4, s32 yLow, s32 yHigh (`TrackChunk::bounds`) |
| 0x0C | u32 scenery mask: bit i = draw scenery instance list i (header 0x118) while this chunk is in view; the chunk drawer 0x80020110 ORs the masks of the chunks it renders and hands the result to 0x8002002C **[new]** |
| 0x94 | u32 -> second shape ("surround"): the chunk's LOW-DETAIL copy for the rear-view mirror only - a road quad plus box walls standing in for the scenery (seattle chunks 42-54: an 8.9 m high, 30 m long quad 6 m left of the grid, textured with a 16 x 16 pole tile). 0x8002993C calls the chunk drawer 0x80020110 twice per frame in a race with a mirror: param_3 = 0 (main view: shape at chunk + 0xA4, then the scenery 0x8002002C) and param_3 = 1 (mirror view at view + 0x4178: shape at chunk + 0x94, only chunks nearer than 100 m, bounds at chunk + 0x6C instead of + 0x44, no scenery). The main view never draws it **[new 2026-09-19]** |
| 0x98 / 0x9C | boundary collision / road surface grid (see TrackBoundary / TrackSurfaceGrid) |
| 0xA0 | u32 -> render list: u16 count, u16 entries = chunk index (bits 0-13) + flags (bits 14-15): the chunks drawn while the camera is in this chunk (0x80020110 walks this list of the camera's chunk (view + 0xA0); seattle chunk 0: 0..3 and 123..125 plain, 4..6 and 120..122 with flags 3). Every entry ORs its chunk's scenery mask (+0x0C) before the view test. An entry with flags > 2 draws only the shape's glow records (+0x28 / +0x42) when 0x800A951C == 0 (a player's race); in the attract race / replays (0x800A951C = 1) it draws the full chunk **[new]** |
| 0xA4 | road shape, inline |

## Shape **[new]**

Header: u32 vertexOffset, u32 listOffset[8], u32 off24, u32 off28, s32 vertexCount, s16 listCount[8], s32 count24.

- Vertex block = vertexCount x { s16 x, y, z, 0 }. (The template lists this block as unknown int pairs.)
- The 8 lists are by libgpu primitive type: F3, F4, G3, G4, FT3, FT4, GT3, GT4.
- Polygon stride: 12 bytes; gouraud tris 20 (two extra RGBx), gouraud quads 24 (three extra RGBx).
- Polygon: u32 w0, u32 w1, u8 r, g, b, u8 primCode, [extra corner colours].
  - w0 bits 0-8 / 9-17 / 18-26 = vertex 0 / 1 / 2 (9-bit indices), bits 27-31 = render order.
  - w1 bits 0-8 = vertex 3 (quads), bits 9-22 = index into the course UV table (14 bits: the drawers 0x8002106C /
    0x800234F8 address the entry as ((w1 >> 4) & 0x7FFE0) + table; seattle has 2219 entries and 2614 of its 7857
    textured chunk polygons use an index >= 1024 - the earlier 10-bit field gave them the wrong texture: the
    "patches" and stretched pieces on the asphalt, the missing yellow lines, start line, boards, bushes, road
    shadows), bits 23-31 = surface attributes (ground.cpp: bits 23-24, 25-26, 28-31).
  - w0 bits 27-28: ordering-table offset, 16 OT entries per step (OT index = base + ((w0 >> 21) & 0xC0) + farthest
    corner depth; larger = drawn earlier). Coplanar decals (grid boxes, start line) use a smaller value than the
    asphalt under them (seattle chunk 48: asphalt 27, decals 28). w0 bit 31: back-face culling (the NCLIP of
    (v0, v2, v1) - the GTE gets v1 in slot 2 - must be >= 0: front = NCLIP(v0, v1, v2) < 0 on the y-down screen);
    all flat road polygons seen from above are front-facing (4474 of 4827 on seattle, the rest face down).
  - Shape + 0x24 / + 0x40: billboards, 16-byte records {s16 x, y, z, u16 uvIndex, s16 width, s16 height, u32 code
    word}: camera-facing quads (trees, poles; seattle has 277), see track.h TrackBillboard. + 0x28 / + 0x42: 20-byte
    glow records (none on seattle; parsed, ported and drawn natively, see "Glow records" below).
  - Quad corners are in ring order (no bow-ties in renders).

## World placement **[new]**

World space is 16.16 fixed-point metres, +Y up. Local vertices are 1/64 m and relative to the origin of
the 64 m grid cell containing the chunk centre:

    cell = floor(centre / (64 << 16)) * (64 << 16)        (per X, Y and Z)
    worldX = cellX + x * 1024;  worldZ = cellZ + y * 1024;  worldY = cellY + z * 1024

The vertical cell offset matters even near sea level: a slightly negative centre Y selects -64 m.
Omitting it moved the Tahiti rally starting road 64 m above its collision surface. The renderer now
uses the same vertical cell offset as `InterpolateRoadHeight` (verified 2026-09-20).

Check: for 13,207 of 13,209 chunks all vertices fall inside the chunk's footprint corners (+8 m margin);
the 2 exceptions are in `test_s2.tro`. Renders of `testline` (High Speed Ring) are seamless.

## Textures: `.trp` **[new]**

`u32 count` + `count` standard TIM files (4bpp + CLUT in the corpus), each carrying its own VRAM
destination. All 126 packs parse to their last byte (14,138 TIMs). testline: images at VRAM x 704-960,
y 0-512; CLUTs at x 576-672, y 496-511. Rendering = rebuild a 1024x512 VRAM image (`PsxVram`) and sample
it with the polygon's tpage/clut words. Verified visually on testline (grandstand, crowd, banners, trees).

## Scenery (header 0x14 / 0x18 / 0x118) **[new]**

Status 2026-09-19. Established from the original's scenery pass (US v1.2 race overlay: 0x8002002C walks the
instance lists, 0x8001F7F8 places one instance, 0x80019B58 / 0x8001C17C draw a model, helpers 0x80081374
rotation, 0x8007B25C scale, 0x8007AE38 + 0x8007AEF4 LOD choice, 0x8007B8A0 precision scaling, 0x8007B8F8 view
test) and verified against the captured GTE traffic of the Seattle attract race (`gt2play --prims 3600`: the
listing ends with `# scenery-check` lines): all 52 view-test transforms of the frame match an instance of
`seattle.tro` (worst corner error 0.58 m, at the GTE's rounding), and all 6 drawn objects feed exactly the model
file's vertex list (459/459 identical). Parser: `Track::sceneryModels / sceneryLods / sceneryInstances`,
`SceneryInstanceMatrix`. Corpus: 6,084 instances, 151 models on seattle, every pointer/index in range.

### Instances (header 0x118: 33 lists, u32 count + 28-byte records)

    s16 angle[3]        +0, +2, +4   rotation, 4096 = one turn (below)
    u16 lodList         +6           index into the header-0x14 table
    s16 scale[3]        +8..+12      4096 = 1.0 (a handful of records use 4055 / 3907 / 3295)
    s16 lodDivisor      +14          4096; divides the view-distance factor of the LOD choice
    s32 position[3]     +16          16.16 m world (x, y up, z)

List 32 is always drawn; list i < 32 when bit i of the accumulated chunk scenery mask (chunk + 0x0C) is set.
Corpus usage: lists 0-15 only. Rotation: 0x80081374(m, a = -angle[1], b = angle[0], c = angle[2]) builds
Ry(a) Rx(-b) Rz(c) from the 4096-entry sine/cosine tables; 0x8007B25C then scales the columns by (scale[0],
scale[1], -scale[2]) / 4096 - the model's +z is mirrored, so an unrotated model's +z points to world -Z. Corpus:
1,802 records have a yaw (angle[1]), 76 a pitch (angle[0]), 33 a roll (angle[2]).

    world = position + Ry(-angle[1]) Rx(-angle[0]) Rz(angle[2]) diag(sx, sy, -sz) / 4096 * v * metresPerUnit

### LOD lists (header 0x14: u32 count + offsets; list = u32 n + n x {u32 threshold, u32 modelOffset})

The entry is chosen by 0x8007AEF4 (established 2026-09-19): the first whose threshold exceeds the measure of
0x8007AE38, -1 (not drawn) beyond the last. The file's u32 is NOT the threshold itself: at course load 0x8007ADC8
replaces every entry by `(w >> 14)^2` (srl, mult, low word; seattle 0x0096C0C6 -> 603^2 = 363609). The measure
(0x8001F7F8 at 0x8001F944..): t = the instance position in camera space, 16.16 m - 0x8007B008 composes the view's
camera matrix (view + 8: rotation with the y row scaled by the screen aspect 3723 / 4096, and translation) with the
instance matrix (0x8008220C: t = R_cam * position + T_cam); k = (view + 0x78 << 12) / lodDivisor with view + 0x78 =
((max(320, 240) >> 1) << 12) / h (0x8007B374; h = the view's projection distance - the replay cameras zoom, h 160 ..
~1000); s = sum over the axes of ((t >> 14) as a 16-bit IR register)^2 (GTE SQR, 32-bit sum); measure =
low 32 bits of ((s * k) >> 12) * k >> 12. Units: quarter metres squared. Checked by tracing every call of the
original (gt2play --prims prints "# lod-measure ... measure M choice C" and "# scenery-lod" lines): the formula is
exact for 63 of 63 calls in each of the frames 2400, 2849, 3000 and 3600 of the attract race and 13 of 13 in the
licence test, and our camera-space translation from the extracted camera gives the original's entry for all of
them (at a replay camera cut the traced calls belong to the frame before the extracted camera; the check prints both).
The earlier check (7 / 20, 37 / 52) compared raw file words in 1/32 m. Implementation: `track.h SceneryLodK /
SceneryLodMeasure / SceneryCameraSpace / SceneryLodThreshold / SceneryLodIndex`; the renderer applies it
(`SceneAssets::AppendTrackItems`), `gt2game --max-detail` draws entry 0 of every instance without the cut-off.

### Models (header 0x18: u32 count + offsets)

Same 0x44-byte shape header as the chunk shapes, then:

    0x40 u16 billboardCount   28-byte records at off24: {s16 x, s16 y, u32 z (low 16 bits), s16 width, s16 height,
                              u32 code word, T0, T1, T2} - camera-facing quads in model units (0x8001F7F8; parsed)
    0x42 u16 lightCount       20-byte glow records at off28 (TrackSceneryModel::glows, see "Glow records")
    0x44 s16 boundsMin[3], 0  0x4C s16 boundsMax[3], 0   -> the 8 corners are transformed first (view test, 0x8007B8F8)
    0x54 s16 scaleExponent    metres per unit = 2^(scaleExponent - 16) / 4096 (same rule as the .cdo LOD scale):
                              23 -> 1/32 m, 22 -> 1/64 m, 21 -> 1/128 m, 20 -> 1/256 m, 17 -> 1/2048 m (corpus 16..26)
    0x58 two vertices         copies of vertices 0 and 1 (purpose unknown)

Vertices: s16 x, y (UP), z, 0 - the earlier "Y down" note was wrong; the sign comes from the instance's negated
z scale. Polygons (lists F3 F4 G3 G4 FT3 FT4 GT3 GT4, strides 12 12 20 24 24 24 32 36):

    word0  bits 0-9 / 10-19 / 20-29 = vertex 0 / 1 / 2 (10-bit, unlike the chunk shapes' 9-bit fields)
           bit 30 = ordering-table depth from the nearest corner + a bias (else the farthest corner)
           bit 31 = back-face culling on (the NCLIP sign is tested only when set)
    word1  bits 0-9 = vertex 3 (quads)
    word2  r, g, b, code
    textured: T0 = u0 v0 clut, T1 = u1 v1 tpage, T2 = u2 v2 u3 v3, then the extra gouraud colours (G: right after word2)

Corner i uses UV i (verified corner by corner on the captured FT4 primitives: 12/12 exact). The GPU quad is
emitted as (v0, v1, v3, v2) - see "UV corner order" below.

The original scales each object's matrix by a power of two so the GTE stays in 16 bits (0x8007B8A0: shift =
max(0, bitlength(max|t| + 2^scaleExponent) - scaleExponent - 2)); the captured rows are therefore the camera
rotation at 4096 >> shift, the y row additionally carries the 3723.5 / 4096 screen aspect factor, and the
translation is in the object's unit >> shift. The transform's vertex list is: 8 bound corners, one vector
(unknown), then the model vertices in file order.

Runtime banners: the small TIMs of the `.trp` at VRAM x 640-710 (the advertising boards) are replaced at race load by
sponsor logos from `.crstims.tsd` (0x800275E8; format, slots and the random selection rule in `sponsor_boards.md`,
reproduced word for word for the attract race; the native renderer applies it).

## Header start grid **[new]**

0x54 s16 start angle (4096 = 360 deg; 3072 on testline where the road heads -X), 0x58 s32[3] x 16 grid slots
in 16.16 m (slot 0 = pole, slots alternate left/right going backwards). Verified in gt2view: a car placed on
slot 0 sits inside the painted grid box, and banner text reads correctly, so the world is not mirrored.

## What the original's draw list showed (Seattle attract race, field 3600, `gt2play --prims`) **[new, partial]**

- The sky / horizon is NOT in the .tro: it is the course's backdrop model `bgsobj/*.bso` (backdrop_bso.md).
- Chunk shapes are transformed with the camera rotation scaled by 4 and the columns permuted to (x, -worldZ,
  height) -> model (x, up, -z); scenery models (header 0x18) and cars use plain (x, up, -world Z) model axes.
- UV corner order (resolved 2026-09-19 from the chunk polygon drawer 0x800234F8 and the capture): the earlier
  "471 polygons carry the right set in another corner order" was an artefact of comparing GPU slots with file
  corners. Every quad drawer of the game (chunks 0x800234F8, scenery 0x80019B58, cars 0x80061798) emits the
  file's ring order (v0, v1, v2, v3) as the GPU's POLY_x4 slots (v0, v1, v3, v2) - the PS1 quad is two triangles
  (0,1,2) (1,2,3), so slots 2 and 3 are swapped to keep the ring - and the texture words follow the vertices:
  slot uv0 = T0 (u0 v0 | clut), uv1 = T1 (u1 v1 | tpage), uv2 = T2 >> 16 (u3 v3), uv3 = T2 & 0xFFFF (u2 v2).
  Corner i of the file therefore uses UV i, exactly what the parser and renderer assume. Re-checking the captured
  field 3600 draw list against `seattle.tro`'s UV table: 161 quads equal a table set in slot order (0,1,3,2)
  (57 near + 104 far sets), 27 triangles equal a set's first three corners, 6 quads equal a set in slot order
  (0,1,2,3) (symmetric sets), 10 match only as an unordered set (mirrored duplicates), 1,257 textured primitives
  have no set with the same UVs (chunk polygons the clipper subdivided: 0x800234F8 splits large near polygons
  into 4 with interpolated UVs) and 3,480 use (clut, tpage) pairs outside the table (scenery, cars, HUD).
- Near / far UV set: the polygon's screen area (|NCLIP| of both triangles) is compared with the entry's
  `extra` word (u16 at +12): area > extra -> near set (first 12 bytes), else far set (+16) (0x800234F8:
  `(u16)entry[3] < MAC0` selects the near set). The far set is the same texture at a coarser mip level (checked on
  entry 803: the same ground image at half size); the native renderer always uses the near set.
- Quad split: the GPU draws the slots (v0, v1, v3, v2) as triangles (v0, v1, v3) + (v1, v2, v3), i.e. along the
  v1-v3 diagonal of the ring. Non-parallelogram UV sets (trapezoid ground / kerb polygons) only look right with
  that split; the native renderer uses it for chunks, scenery, cars and the backdrop.

## Glow ("light") records (2026-09-19, ported and verified against captures)

Courses with records (`gt2tool track-scan` prints them): roma_night / rev_roma_night / 2p_roma_night (328 in chunks),
highway / Rhighway / 2p_highway (chunks 234..250, models 31..45), shortway / Rshortway / 2p_shortway, sprint2 /
rev_sprint2 / 2p_sprint2, spL1..3, bill_checker, speed (models). Parsed: `TrackShape::glows` (chunk road / surround shapes:
shape + 0x28, count u16 at + 0x42), `TrackSceneryModel::glows` (model + 0x28 / + 0x42). Record (20 bytes):

    +0  s16 x, y     +4 s16 z      +6 s16 size (loaded as DQA; DQB = 0)
    +8  8 bytes      (not read by the drawers)
    +16 u32 colour   (0x00BBGGRR)

Drawn inline by the chunk drawers (0x800205EC before the chunk's polygons; 0x80020A00 = a render-list entry with flags > 2
in a player's race: glows only) and by the scenery instance pass (0x8001FBA8, after the model's billboards, before its
polygons 0x80019B58 / 0x8001C17C), with the chunk's / model's GTE transform loaded. Per record: RTPS; skipped when FLAG
bit 31 is set; four POLY_GT4 (code 0x3E: gouraud, textured, semi-transparent; tpage 0x29 = page (576, 0), 4-bit, blend 1
additive; CLUT 0x7F57 = (368, 509)) around the projected centre C, with s = sin 22.5 (1567), c = cos 22.5 (3784) from the
sine table 0x80093150 (+ 0x200 / + 0xA00), A = (r c / 2, r s / 2), B = (-r s / 2, r c / 2) - computed register by register
with the halves `>> 13` and the wholes `>> 12` of the 32-bit products (`glow.cpp DrawGlows`):

| Quad (packet) | v0 | v1 | v2 | v3 | uv0 / uv1 / uv2 / uv3 |
|---|---|---|---|---|---|
| 0 | C + B | C + B - A | C | C - A | (160, 64) (128, 64) (160, 96) (128, 96) |
| 1 | C + B | C + A + B | C | C + A | (160, 64) (191, 64) (160, 96) (191, 96) |
| 2 | C - B | C - A - B | C | C - A | (160, 127) (128, 127) (160, 96) (128, 96) |
| 3 | C - B | C + A - B | C | C + A | (160, 127) (191, 127) (160, 96) (191, 96) |

Vertex colours: v0 and v3 = colour / 8, v1 = colour / 64, the centre v2 = colour / 2 (per channel, (c & 0xFEFEFE) >> 1
etc.). The four packets are added to the ordering table in quad order 0..3, 8 entries nearer than the depth (base - 32
bytes), so the GPU draws them 3, 2, 1, 0. The texture (u 128..191, v 64..127 of the page) is `crstim.arc` record 0, which
0x8002EB08 uploads to (608, 64) with its CLUT at (368, 509).

Radius and depth: chunks r = (MAC0 << shift) >> 15, entry = min(SZ3 >> (shift + 3), 4095) with shift = scratch
0x1F80009A (0..2, see below); models r = (MAC0 >> (s - 10)) >> 15 (MAC0 << (10 - s) when s < 10), entry = min((SZ3 << s) >> 13,
4095) with s = scratch 0x1F800098 (0x8007B8A0: scale exponent - 12 for a near model; the GTE unit is 2^(s - 16) m). In
metres both are the same star: radius r corresponds to size / 32 m at the point.

The chunk pass 0x80020110 (per view; its a2 = 1 for the rear-view mirror, set in the jal's delay slot):
- copies view + 0x08..+0x6F to the scratchpad (= camera::RaceCamera::gteState: rotation, TR, the camera offset view + 0x28,
  light / colour matrices = frustum planes, OFX, OFY, H, clip limits + 0x5E / + 0x60 / near + 0x62), swaps the rotation's
  columns 1 and 2 (0x80020E38: chunk vertices are (x, z, height)), loads the GTE (0x80020E84 -> 0x8007B778 shift 0, TR = 0);
- per render-list entry of the camera chunk (view + 0xA0): key 0x80020FD8 = max + mid / 2 + min / 4 of |chunk centre +
  offset| (mirror: skipped above 0x63FFFF), the scenery mask, the view test 0x80020EC4 (the box at chunk + 0x44, mirror
  + 0x6C: 4 corners {s32 x, s32 z} x heights + 0x20 / + 0x24, each corner + offset reduced to 13 bits by 19 - LZCS, then
  0x8007B640: RTPS, LLM / LCM plane signs, near test, screen limits; culled when all 8 corners share an outside code),
  an insertion into a list sorted by key ascending (equal keys after);
- per sorted chunk 0x80026BB4: shift = min(2, k - 1) where k = number of quarterings of 2^50 until it is <= the squared
  distance |centre + offset|^2 + (chunk + 0x2C)^2 (64-bit); rotation << shift (0x80081A34); TR = MVMVA(rotation, IR =
  ((centre & 0xFFC00000) + offset) >> 10 as (x, z, y), 16-bit IR registers); scratch + 0x9A = shift;
- then the glows (and billboards / polygons unless glows only).

Verification (`gt2play --prims`, which lists per frame "# glow-pass" with our chunk order, "# glow-guest-chunk" for every
0x80026BB4 call of the original, "# glow-model" per model glow call and at the flip "# glow-frame" + our primitives in GPU
order): the original was run on the Arcade disc (v1.1, SCUS_944.55; the course row of COURSE SELECTION poked to Rome-Night /
Special Stage Route 5, the original's AI driving the player's car: `tools/gt2run/ai_player_arcade.h`), captures
`work/play/glow/rn_{5600,5700,6000,7000,8500,10000,11500}.txt` (roma_night) and `hw_{5600,9300,9500,10500,12000}.txt` (highway,
with model glows in 9300 / 9500 and a glows-only render-list entry in 5600). Result: all complete frames equal - 13 roma_night
frames (32..96 primitives) and 14 highway frames (16..100 primitives, model glows interleaved in the same ordering table) have
identical glow primitive lists in order, coordinates, colours, UV, CLUT and tpage; 38 of 38 chunk passes draw the original's
chunks in the original's order; all model records equal the parsed ones. (Arcade and Simulation share the drawing code; the
addresses are mapped by the exe profile.) The comparison takes, per "# glow-frame" block, our lines against the GP0 lines with
CLUT 7F57 up to the next block (the frame's draw list follows its flip), and per pass our chunk list against the guest's.

Native (gt2game, gt2play): `SceneAssets::AppendTrackItems` adds per drawn render-list entry (glows-only entries included)
the chunk's stars and per drawn scenery instance the model's stars (`glow.h AppendChunkGlowSprites / AppendModelGlowSprites`):
four quads in the camera plane (right, down axes) with radius size / 32 m, the PS1 UVs / colours, blend mode 1; the texture
is loaded with the smoke sprites. Not reproduced: the original's ordering-table sorting (our z-buffer), the view test
(Vulkan clips), the mirror view's surround-shape glows (no course seen with any).

## Open

Word0 bits 29-30 of the chunk polygons; the glow records' bytes 8..15; the unknown vector in the
scenery transforms; the 2 outlier chunks; the chunk
render-list flag bits (14-15) beyond "present".
