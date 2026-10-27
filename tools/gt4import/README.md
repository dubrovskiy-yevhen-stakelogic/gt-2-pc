# GT4 import tools

These command-line tools decode CAR4, MDLS, Tex1 and SpecDB data for the GT4 model
import scripts. They are built from the source included here and do not require
a separate GT4 checkout. Supply a local Gran Turismo 4 image; game data and
converted assets are not included.

Build the `gt4-import-tools` CMake target. The five exporters can also be built
independently from the main game:

```text
cmake -S tools/gt4import -B build_gt4_import
cmake --build build_gt4_import --config Release
```

- `gt4assettool tex <file> <output-directory> --offset <offset>` exports Tex1
  images with the original palette and PS2 texture layout.
- `gt4nativemetadata <CAR4-file> <output-directory> [external-Pat0-file]` exports
  original draw state, complete instance transforms, materials and textures.
- `gt4materialmetadata <CAR4-file> <output-directory>` exports material and lamp
  state used to align the imported model.
- `gt4specdbexport <raw-SpecDB-directory> <new-output-directory>` decodes table
  rows and labels into the JSON format used by the wheel preparation script.
- `gt4showroomexport <CORE.elf> <raw-SpecDB-directory> <output.json>
  <label-or-@label-list> [...]` computes the original static body and wheel poses.
  Use `scripts/build_gt4_showroom.py` to select model labels and bind the output
  to the decoded SpecDB tables and verified CORE image. The complete preparation
  pipeline extracts and verifies these inputs automatically.

The parser and arithmetic modules are handwritten source from the native GT4
reconstruction project. `PROVENANCE.json` and `SHOWROOM-PROVENANCE.json` record
the imported file paths and SHA-256 hashes. They contain no generated decompiler output, game executables,
tables or art assets. Keep all extracted and converted data outside the source
tree, normally under `work/`.
