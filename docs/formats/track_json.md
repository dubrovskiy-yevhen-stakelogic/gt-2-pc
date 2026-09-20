# Course file (`gt2pc-track`, JSON) - the editable course format and the course mod layer

Our own format (not a game format), 2026-09-19. It carries everything the native game takes from a course: the `.tro`
(chunks, road shapes, walls, road lookup grids, chunk chain and distances, render lists, mirror copies, billboards, the
course UV table, scenery models / LOD lists / instances, the start grid - `track_tro.md`), the race data inside the `.tro`
(start lines and the seven race lists - `course_race_data.md`), the trackside replay cameras (`.tro` header + 0x1C,
`camera.md`) and the course's `.crsinfo` entry (name, flags, backdrop - `backdrop_bso.md`, `sponsor_boards.md`), plus
what a mod adds. Code: `src\gt2formats\track_json.*` (reader / writer / resolution / canonical bytes / diff),
`src\gt2export\track_gltf.*` (the drawable geometry as glTF), `tools\gt2tool\track_cmds.*`, `tools\gt2game\mods.*` and
`mod_scene.*` (loader, race view, physics check).

## Tools

```
gt2tool export-tracks "<disc.bin>" work\export\trackmods\tracks [--course NAME] [--no-gltf]
gt2tool import-track  "<disc.bin>" <dir>\tracks\<name>.json      # validate, resolve, print the diff against the base course
gt2game "<disc.bin>" --mods <dir> --track <name> [...]           # race on <dir>\tracks\<name>.json
gt2game "<disc.bin>" --mods <dir> --mod-physics-check <steps> [--track <name>] [--cars N]
```

`export-tracks` writes `<name>.json` + `<name>.gltf/.bin/.png` for every `crsobj/*.tro`, reads each JSON back the way
gt2game does (`ReadCourseFile` + `ResolveCourseFile`) and compares the result with the disc's parsed data byte for byte
(`CanonicalTrackBytes` over every field of `gt2::Track`, `CanonicalRaceBytes`, `CanonicalCameraBytes` - also through
the re-laid-out camera list the camera code reads -, `CanonicalInfoBytes`, and the texture pack bytes); it also reads the
glTF back with the game's glTF reader and checks the triangle count. Exit code 0 only when every course matches.

`--mod-physics-check` runs, for every `<dir>\tracks\*.json` (or the `--track` one), the same race on the mod course and on
its disc base course side by side (the self-test's scripted player pad, the other cars on the AI) and compares the whole
simulation state (`RaceSim::Snapshot`) after every step.

Verified 2026-09-19 on US Simulation v1.2: `export-tracks` 126 / 126 courses byte-identical (0 mismatches, 318 MB of
output, 28 s); `--mod-physics-check 300 --cars 6` over the 126 exported courses: 120 identical for 300 steps x 6 cars
(20,710 state bytes per step), 6 courses have no AI list on the disc either (test_20a / 30a / 40a / hs1 / hs2 / l1 - the
race set-up refuses them identically; test_hs1 with `--cars 1`: identical).

## File layout

All keys are optional in a mod file except `format`; a complete export has all of them. Units: positions in metres
(decimal; the game's 16.16 / 1/4096 m values are dyadic, so the written decimals are exact and read back exactly),
angles in degrees (4096 game units = 360 degrees, exact), bulk geometry in the game's integer units (documented per
field). World axes: x, y up, z (the game's world; the simulation plane is (x, -z)).

| Key | Content |
|---|---|
| `format`, `version` | `"gt2pc-track"`, 1 |
| `course` | course id (the file name for mods) |
| `baseCourse` | disc course (`crsobj/<name>.tro`) that supplies every block the file does not name, and the default textures / course map |
| `assets` | `mesh` (informational: the exported glTF), `texturePack` (a `.trp`-style TIM pack relative to the JSON), `textureCourse` (a disc course whose `.trp` is used; default `baseCourse`), `courseMap` (disc course of the HUD map `crsmap/`; default `baseCourse`) |
| `info` | `.crsinfo` entry: `name`, `flags` (bit 0 / 1 reflection map = `crstim.arc` entry 5 / 4 else 3, bit 2 dirt, bit 5 point to point, bit 6 no sponsor boards), `backdrop` (`bgsobj/<name>.bso` + `.bsp`), `tail` (bytes 12..23, hex, not decoded). Keys override the base entry one by one. |
| `start` | `angle` (degrees; the grid heading), `grid` (16 slots `[x, y, z]` m: where a car's nose stands; slot 0 = pole) |
| `uvTable` | course UV table, one array of 21 integers per entry: near set `u0 v0 u1 v1 u2 v2 u3 v3 clut tpage`, `extra` (the near / far switch area), far set `u0 .. tpage` |
| `courseLength`, `chunks` | course length (m) and the chunks (below); always together |
| `scenery` | `models`, `lods`, `instances` (below); one block |
| `addSceneryInstances` | instances appended to the resolved scenery (mods; same object form as `scenery.instances`) |
| `race` | `startLines` (course distances, m), `listCount` (7; `maxspeed` has 6), `lists` (7 entries: `null` = absent, else records) |
| `replayCameras` | `null` (none) or `{lapModulus, records}` |
| `objects` | external glTF meshes placed in the course (mods, below) |

### Chunk

| Key | Content |
|---|---|
| `prev`, `next` | chunk chain (indices) |
| `distance` | course distance at the chunk (m; chunk + 0x10) |
| `weightNext`, `weightThis` | distance interpolation weights (chunk + 0x14 / + 0x16) |
| `vcoord` | metres along the course (s16, chunk + 0x12) |
| `origin`, `centre` | m; `direction` s16 x3, 4096 = 1.0. The centre also selects the 64 m cell the game keeps the vertices relative to |
| `sceneryMask` | u32: bit i = scenery instance list i is drawn while the chunk is in view |
| `renderList` | u16 entries: chunk index (bits 0-13) + flags (bits 14-15; > 2 = glow records only in a player's race) |
| `road` | the chunk's shape (drawn and driven on): `vertices` flat `[X, Y, Z, ...]` in 1/64 m. Version 1 stores world X/Z but cell-relative Y; add `floor(centre.y / 64) * 64` metres for world height. `polygons` below |
| `walls` | `[vertexA, vertexB, normal0, normal1]`: an edge between two `road` vertices with its normal (used as (n1, -n0)) |
| `surfaceGrid` | `origin` `[X, Z]` world 1/64 m, `shift` `[x, z]` (cell = 1 << shift), `cells`: 16 lists (row-major, 4 x 4) of `road` polygon indices a height query at that cell tries |
| `billboards` | `[X, height, Z, uv, width, height, "rrggbb", code]` (1/64 m; uv = UV table entry, near set) - camera-facing quads |
| `lightCount` | glow records of the shape (not exported; count only) |
| `mirror` | the low-detail copy the rear-view mirror draws (same form as `road`) |

Chunk polygon: `[code, order, v0, v1, v2, v3, uv, surface, colours...]` - `code` = libgpu primitive code (0x20 F3, 0x28 F4,
0x30 G3, 0x38 G4, 0x24 FT3, 0x2C FT4, 0x34 GT3, 0x3C GT4; + 2 semi-transparent, + 1 raw texture), `order` = word0 bits
27-31 (bits 27-28 ordering-table tier, bit 31 back-face culling), vertices (9-bit; v3 is stored for triangles too),
`uv` = UV table index (14 bits), `surface` = word1 bits 23-31 (the physics' surface / attribute bits, `ground.cpp`),
colours `"rrggbb"`: one (flat) or one per corner (gouraud). Polygons must stay grouped by list in the order above.
Quad corners are in ring order.

Walls, the grid and the render lists reference vertices / polygons / chunks by index: moving vertices keeps them valid;
adding or removing polygons needs the grid cells and the walls updated by the author (no generator yet).

### Scenery

- `models[]`: `scaleExponent` (metres per unit = 2^(e - 16) / 4096), `boundsMin` / `boundsMax` (s16 x3), `vertices` flat
  `[x, y (up), z, ...]` model units, `polygons` `[code, flags, v0, v1, v2, v3, (u0 v0 u1 v1 u2 v2 u3 v3 clut tpage when
  textured), colours...]` (flags bit 0 = sort by the nearest corner, bit 1 = back-face culling; 10-bit vertices),
  `billboards` `[x, y, z, width, height, "rrggbb", code, u0 v0 u1 v1 u2 v2 u3 v3, clut, tpage]`, `lightCount`.
- `lods[]`: per LOD list `[[threshold, model], ...]`, most detailed first; `threshold` is the file word (the game compares
  `(threshold >> 14)^2` with its view measure, `track_tro.md`).
- `instances[]`: `{list (0..32; 32 = always drawn), lod (LOD list), position [m], angle [deg: x, y (yaw), z], scale [1.0 =
  4096], lodDivisor (4096)}`; world = position + Ry(-angle y) Rx(-angle x) Rz(angle z) diag(sx, sy, -sz) v.

### Race lists

Record: `[type, x, z, radius, heading]` (0 straight / 1 corner; world metres; signed radius m; heading in the game's angle
units), plus `[height, chunk, surface, attribute, distance, slopeAcross, slopeAlong]` only when the file carried non-zero
loader fields (none on the disc: the race loader fills them from the geometry). List 4 is the grid list; the others are the
AI / section lines (`course_race_data.md`).

### Replay cameras

`records[]`: `{kind (flags & 15: 0 path looking at the car, 1 fixed, 2 onboard, 3 path with zoom), flags, laps [first,
last], chunk (search seed), start, end (course distances, m), data}` - `data` = the record's bytes from + 0x10 (H values,
position / angles or the inline path; hex, as stored). The loader lays the list out again (`CourseCameras::Blob`).

### Objects (mods)

`{mesh (glTF 2.0 relative to the JSON), scale, position [m], rotation [yaw, pitch, roll deg], shadow (bool)}` - drawn
every frame, no collision, no reflection pass. Each distinct mesh takes one of the renderer's 8 car-sized mesh slots (32,768
vertices each; the race's cars take theirs first) - an object without a free slot is skipped with a message.

## Precedence (how gt2game resolves a mod course)

1. Base course = `baseCourse`, or `course` when it is a disc course. A file without all blocks needs one.
2. Every block the file names replaces the base course's (`info` key by key); `addSceneryInstances` are appended.
3. The result is validated with the parser's rules (index ranges, 9 / 10-bit vertices, list order, cell rule) before the race.
4. Race: the file's race data goes through the game's race-data builder (0x80038DA0 port) on the resolved geometry, like a
   disc course; dirt / point-to-point from the resolved flags; the `.crsinfo` index is the base course's.
5. Race view: textures = `texturePack` or `textureCourse`'s `.trp`; backdrop and reflection map from `info`; sponsor boards
   from `info.flags` bit 6; HUD map from `courseMap`; trackside cameras from `replayCameras`; `objects` drawn with the course.

## glTF export

`<name>.gltf` (+ `.bin`, `.png`): one node / mesh per chunk (`chunk_NNN`, road shape in world metres; `extras.gt2pc.chunk`),
one mesh per scenery model (model units) and one node per instance with the game's instance matrix (`extras.gt2pc` instance,
model, list; most detailed LOD). Texture: every (tpage, clut) region the polygons use, decoded from the course VRAM (the
`.trp`) into one RGBA atlas (texel 0x0000 transparent). Vertex colours = PS1 modulation (128 = 1.0). Materials: textured /
untextured x opaque / blended. Quads split along v1-v3 like the PS1 GPU. Not in the glTF: billboards (camera-facing), mirror
copies, glow records. The glTF is the viewable / reusable form (e.g. as `objects` of another course); the JSON is the course.

## Examples (work\export\mods)

- `tracks\seattle_mod.json` (partial): base `seattle`, `info.name`, `addSceneryInstances` (a second grandstand, instance 44's
  LOD list 100 turned 180 degrees across the road), `objects` (the exported Shelby glTF at 3x as a monument). Screenshots:
  `work\play\track_mod_start.png` vs `track_disc_start.png` (start fly-by, same frame), `track_mod.png`.
- `tracks\seattle_narrow.json` (full export edited by a script): the left barrier after the start (chunks 49..56, every
  road / mirror vertex at z >= 246 m) moved 6 m towards the road between x = -60 and -200 m (tapered over 30 m). `import-track`
  reports 8 chunks with moved vertices; the self-test driver scrapes the moved wall (135.9 km/h at step 300 instead of
  137.8); `work\play\track_mod_wall.png` vs `track_mod_wall_disc.png`. The surface grid was not regenerated: points between
  the old and the new wall line find no road polygon (the wall keeps cars out).

## Limits / open

- No generator for surface grids, walls, render lists or distances from new geometry: a new road must bring them (or be a
  moved copy of an existing one). A track compiler (glTF road -> chunks) is the next step.
- Textures: a mod ships its own `.trp`-style TIM pack or uses a disc course's; PNG -> TIM conversion is not provided.
- Glow records (night courses) are not exported (not drawn natively yet); `lightCount` is kept.
- Replays (`--replay-out`) of a mod course store the base course's `.crsinfo` index.
