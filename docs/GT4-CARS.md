# GT4 cars

The optional pack adds 722 cars to the GT2 catalogue and provides 388 replacement
models for existing GT2 entries. GT2 and GT4 cars can participate in the same race.
The [README](../README.md#gt4-cars) has the tested image revision, a gameplay
screenshot and the full list of 66 models with restored original interiors.

## Required data and tools

- Your **Gran Turismo 4 USA v1.01 (SCUS-97328)** ISO. The importer verifies its
  table-of-contents fingerprint before conversion. Other revisions are rejected.
- Your prepared **GT2 USA Simulation v1.2** disc directory, and optionally
  **USA Arcade v1.1** for a second catalogue overlay. A complete BIN is also accepted.
- Python 3.11 or later with Pillow and NumPy, and the .NET 8 SDK.
- The complete release's `native/tools` directory contains the Windows native
  exporters. Building from source instead requires CMake 3.22+, a C++20 compiler
  and the `gt4-import-tools` and `gt2tool` targets.

The first conversion builds pinned GTPS2ModelTool/PDTools source and the bundled
catalogue reader. It needs GitHub and NuGet access. Source archives are checked
against pinned hashes before building. No game images or preconverted cars are
downloaded. For offline conversion, supply previously built model/catalogue tools
with `--model-tool`, `--catalog-tool` and `--offline`.

## Prepare the complete pack

From the extracted complete release, run:

```powershell
python scripts/prepare-gt4-release.py --iso "D:/Games/GT4-USA-1.01.iso" --simulation "runtime/simulation" --arcade "runtime/arcade" --output "work/gt4-090" --tools "native/tools"
```

Use your actual image and runtime paths. `IMPORT-GT4-MODELS.bat` wraps the same
pipeline with PowerShell parameters `-Iso`, `-Simulation`, `-Arcade`, `-Output`
and `-Build`. For example, `-Build native/tools` selects the bundled exporters.

The output must be a new directory under this checkout's `work/`. Preparation
keeps intermediate assets in a `.preparing` directory and only renames it after
all stages succeed. It does not modify an installed game or its saves. Conversion
processes the full catalogue and can take a while; progress and stage logs are
kept beside the generated files.

The final directory contains:

- `modelsets/gt4`, including `native-v16`: body models, paints, materials,
  authored interior choices and recovered wheel poses.
- `expansions/simulation` and, when requested, `expansions/arcade`: separate
  catalogue overlays for the two discs.
- `READY.json`: counts and source identity. A complete result has 1,110 model
  mappings, 722 added catalogue entries and 89 authored-interior mappings.

`--models` is a developer diagnostic option. A subset does not produce an
activation-ready catalogue and must not be installed as the complete expansion.

## Install on desktop

First create a verified bundle from the complete result:

```powershell
python scripts/package-quest-gt4.py --pack work/gt4-090/modelsets/gt4 --native work/gt4-090/modelsets/gt4/native-v16 --simulation-expansion work/gt4-090/expansions/simulation --arcade-expansion work/gt4-090/expansions/arcade --output work/gt4-090/gt4-models.tar
python scripts/install-gt4-pack.py --bundle work/gt4-090/gt4-models.tar --runtime "C:/Games/GT2/runtime"
```

Use the runtime directory containing `arcade/` and `simulation/`. If preparing
only Simulation, omit `--arcade` during conversion and `--arcade-expansion` when
packaging. The installer checks every bundle file before replacing model and
catalogue directories. Existing GT4 assets are retained under `gt4-backups/`;
disc data, saves and settings are preserved. Close the game before installing.

Start the usual desktop or PCVR launcher. Enable **GT4 models** in Graphics if
that setting was previously disabled. The replacement toggle affects existing
GT2 entries; added GT4 catalogue cars still use their required models.

## Install on Quest

Create the same bundle, then pass it to the normal release installer:

```powershell
./native/scripts/install-player.ps1 -Target Quest -Gt4Bundle work/gt4-090/gt4-models.tar -InstallDir "C:/Games/GT2/runtime" -Serial YOUR_QUEST_SERIAL
```

The installer updates the APK and transfers the prepared discs and GT4 bundle.
If Python is not available as `python`, pass `-Python` with its executable path.
Use the same APK signing identity as the installed release. Do not uninstall to
work around a signature mismatch. Back up saves before changing installations;
see [save transfer](SAVE-TRANSFER.md).

## Limits

Cars use adapted specifications within GT2's simulation. GT4 handling, sounds and
every source-car feature are not reproduced. Generated cabins can have fitting
or clipping errors; the full catalogue has not been checked manually. Saved cars
from the expansion require its catalogue and mandatory models to remain installed.
The browser version does not load the native GT4 pack.

## Build the native exporters

```powershell
cmake -S tools/gt4import -B build_gt4import
cmake --build build_gt4import --config Release --target gt4-import-tools
```

With a multi-configuration generator, executables are under `build_gt4import/Release`.
The main project also builds these targets alongside `gt2tool`. All required
CAR4/MDLS/Tex1, SpecDB and showroom-pose source is included; a separate GT4
development checkout is not required.
