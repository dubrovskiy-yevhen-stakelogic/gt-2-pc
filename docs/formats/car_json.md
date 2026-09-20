# Car file (`gt2pc-car`, JSON) - the editable car format and the mod override layer

Our own format (not a game format). Code: `src\gt2formats\car_json.*` (reader/writer/override/diff, no
dependency beyond `json.*`), `src\gt2formats\gltf_reader.*` + `png_reader.*` (external meshes). Tools:
`gt2tool export-cars`, `gt2tool import-car`, `gt2game --mods <dir>`.

## What a file describes

One car per file. Two binary records of the game are expressed field by field:

- `config` = `gt2::CarConfig` (0x84 bytes): the car's part rows in the GTDT tables (`config.parts.*`: brakes,
  chassis, engine, gearbox, suspension, LSD, tyres, upgrades ...) and the menu settings (`config.settings.*`:
  gear ratios, final drive, brake balance, downforce, ride height, springs, dampers, stabilisers, LSD, ASM/TCS
  levels, flags). This is what the shell's record builder (0x800771AC) consumes - see `car_params.md`.
- `params` = `sim::CarParams` (0x1C0 bytes): the race record the physics setup (0x800319A8) reads, in groups
  `body` (dimensions mm, `weightKg`, weight distribution, inertia codes, Cd x100, downforce, ride height),
  `engine` (torque curve rpm/100 + torque, rev limit, idle, turbo), `gearbox` (`gearRatio` x1000, final,
  count, auto gearing), `drivetrain`, `brakes`, `steering`, `suspension.front/rear`, `tyres.front/rear`
  (slip-angle / slip-ratio / load / camber curves), `assists` (ASM/TCS).
  Units and derivations of every field: `src\game\sim\car_setup.h` (struct `CarParams`).

Both records are covered by field tables (`CarParamsFields`, `CarConfigFields`) that map every byte exactly
once (checked at tool start by `CheckCarJsonDescriptors`). Bytes whose meaning is unknown are written as hex
strings under `raw`, so nothing is lost. Values with known meanings may be written as enum names.

Descriptive blocks (optional in mods):

| Block | Content |
|---|---|
| `carId`, `name`, `model`, `paints[]` | id (= file name for mods), display name, body model id, paint ids with the .carinfo colour chip |
| `baseCar` | disc car whose tables build the base record (mods) |
| `mesh` | `file` (glTF 2.0, relative to the JSON), `scale` (multiplies positions; metres after scaling), `paint` (informational), `reflection` (`"materials"` default, `"all"`, `"none"`: see Reflections) |
| `body` | wheel geometry in metres, car axes (+X right, +Y up, -Z front), LEFT wheels (right side mirrored): `wheelFront[3]`, `wheelRear[3]`, `wheelRadius[2]`, `wheelWidth[2]` |
| `sound` | `engineSet` (engine/<set>*.es bank), `exhaust` (0..3, +4 turbo), `turbo` |

## Precedence (how gt2game resolves a mod)

1. Base configuration: the base car's stock `CarConfig` (`StockCarConfig`), base car = `baseCar`, or `carId`
   when that is a disc car.
2. The file's `config` fields override it (only the fields present).
3. The game's record builder turns the configuration into `CarParams`; the body lengths and tracks come from
   the `.cdo` - or, with a `mesh`, from the glTF bounding box and the wheel positions (same derivation as the
   original applies to the `.cdo`, `BodyDimensionsOfMesh`).
4. The file's `params` fields override the built record (only the fields present).

A file whose `carId` is not a disc car and has no `baseCar` is stand-alone: the record is its `params` alone
(it must then be complete). A disc-car export contains every field of both records; reading it back gives a
byte-identical record (checked by `export-cars` for all 618 cars of the Sim US v1.2 tables).

Without a `body` block the wheel positions of an external mesh are estimated from its bounds (the tools print a
note); give them explicitly for correct track widths and wheel placement.

## Usage

```
gt2tool export-cars "<disc.bin>" work\export\cars_all            # 618 files + glTF/bin/png, round trip checked
gt2tool import-car  "<disc.bin>" <dir>\cars\mymod.json           # validate, print the diff against the base car
gt2game "<disc.bin>" --mods <dir> --car mymod                    # <dir>\cars\<id>.json (+ its mesh)
gt2game "<disc.bin>" --mods <dir> --cars 4 --ai-cars mymod,light,cc69n   # mod / disc cars as the AI opponents
```

## AI opponents (2026-09-19)

`--ai-cars a,b,...` names the cars of the AI slots 1, 2, ... in order (the rest keep the attract line-up); with `--mods` an id
with a file `<dir>\cars\<id>.json` is resolved exactly like the player's mod car (same precedence), otherwise it is a disc
car. A glTF body is loaded into its own scene slot under the mod id before the other cars (`mod_scene.h
PreloadModOpponents`); a mod on a disc body loads that `.cdo` under the mod id. The AI drives the mod's record (the physics
does not distinguish mod cars). Sound: the file's `sound` block (or the configuration's) is used when the disc has that
engine set and exhaust bank; otherwise the base car's set, then the first car of the chassis table (printed). AI cars only
use the shared AI exhaust bank of their turbo flag (`engine/ene_t.es` / `ene_n.es`), so an opponent always has sound.
Checked: `--cars 4 --ai-cars mymod,light,cc69n` (glTF Shelby, lightened disc Shelby, disc Camaro) races headless and in the
window (`work\play\mod_opponents.png`, looking back from the pole).

## Reflections (2026-09-19)

The game draws every body polygon whose flag word has bit 15 a second time with the course's environment map
(`scene_assets.h UpdateCarReflection`, 0x800616C4 / 0x80061798: uv = 64 + (R512 . n) >> 12, n = the polygon corner's
normal at 512). A glTF body opts in per material with `"extras": {"gt2pc": {"reflection": true}}` (the `mesh.reflection`
default `"materials"`), or wholesale with `"reflection": "all"`; `"none"` switches it off. The pass uses the mesh's vertex
normals (glTF `NORMAL`, in the car axes; face normals when a primitive has none) scaled to 512 and the same map, colour
and blend (additive) as the disc cars. `gt2tool export-cars` / `car-gltf` now write `NORMAL` (the .cdo normal table) and
put the bit-15 polygons into the materials `textured_reflective` / `flat_reflective` with that extra, so an exported disc
body keeps its reflection when it comes back as a mod. Checked on `mymod` (exported us36n body): `work\play\refl_mymod.png`
(pass on) vs `refl_mymod_norefl.png` (`"reflection": "none"`) vs `refl_us36n.png` (disc car).

Minimal mod (a lighter Shelby with the disc mesh exported to glTF):

```json
{ "format": "gt2pc-car", "version": 1, "carId": "mymod", "baseCar": "us36n",
  "mesh": { "file": "us36n.gltf", "scale": 1.0 },
  "params": { "body": { "weightKg": 900 } } }
```

Verified 2026-09-19: `import-car` reports exactly the named fields as changed; `gt2game --mods` drives the car
with the external glTF body (textured, wheels and shadow drawn). Note that the physics is the original's: e.g.
lowering `weightKg` alone lowers the tyre loads and the car spins its wheels more at the start (Shelby at 900 kg:
77.1 km/h at 5 s with 6516 rpm in 2nd vs stock 84.7 km/h / 5014 rpm) - mods should change related parameters
together, as the game's own lightweight upgrades do through the builder (`config.parts.lightweight`).

## Limits

- glTF reader: positions, normals, UVs, indices, one base-colour PNG per material (8-bit RGB/RGBA); no
  skinning, animations, KTX or embedded JPEG.
- Mesh textures use the renderer's external RGBA store (`kExternalTexture`, 16 MiB), nearest sampling with
  alpha mask.
- The scene has 8 car mesh slots of 32,768 vertices; a large glTF takes several (placeholder slots), so a race with many
  big mod bodies (and course objects, track_json.md) can run out - the slot loader prints what it truncates / skips.
