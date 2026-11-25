# Optional HD texture pack and R32 cockpit family

Requires the updated player. The published 0.9.2 executable does not support this
new drop-in layout. No disc, save, executable or setting is inside either pack.

## Install textures

Extract the HD archive into the **game data folder**, where `arcade` and
`simulation` are located (not necessarily the folder containing the MSI program).
The result is:

```text
runtime/
  arcade/disc.raw2352
  simulation/disc.raw2352
  textures/
    arcade/<course>/<image>.png
    arcade/_shared/<image>.png
    arcade/_hud/<image>.png
    arcade/_sponsors/<image>.png
    simulation/...
```

Enable **HD texture pack** in **HD textures** in the VR/graphics menu. Individual
categories can be disabled. Turning the pack off restores original textures.
The setting is saved. Select texture packs in settings; there is no R3 shortcut.

Edit an existing PNG under its original name, or delete it. Keep its dimensions
and alpha channel. Missing, corrupt or unsupported images leave the corresponding
original game texture in place. Restart the game after changing files. A missing
shared material (for example road detail or the window-interior atlas) restores
the original surfaces that depend on it. New names are not automatically mapped
to a surface: the shipped filenames and source-profile registrations identify
which original surface to replace. The player contains these registrations;
players do not need to edit manifests or repack an archive.

For registered sponsor logos, `_sponsors/<hash>.png` also supplies matching fixed
billboards on tracks. For example, the STI image
`_sponsors/6a28eb5d5c8cb15be7f162babdbdca324bc6e7fa.png` applies to the SSR5
billboard as well as race-selected STI adverts. This requires the player with
the fixed-billboard sponsor update; the original 1.0.0 player only used that file
for race-selected adverts.

Lookup priority is a track-specific PNG, then the matching `_sponsors` PNG for
registered logos, then `_shared`, then the original disc texture. Remove an old
track-specific override if you want the common sponsor image to apply there.
Only known matching logos are linked; unrelated signs are not matched by appearance.
The player reuses matching billboard images. If a race fills the texture budget,
it reduces the uploaded resolution of the remaining sponsor images to fit all
selected adverts; your PNG files and the memory limit remain unchanged.

`_gpu` contains optional compressed Quest atlas images. They are addressed by the
hash of the decoded current textures, so edited PNGs cannot accidentally use stale
compressed art. Removing `_gpu` is supported but may increase GPU bandwidth on
Quest. Original texture fallback remains available without any pack at all.

On Quest, copy `textures` into the app's external game-data `files` directory,
beside `arcade`/`simulation`. On Steam Deck, use the same game-data folder selected
by the installer. A `textures` folder directly beside a disc also works and takes
priority. Legacy `generated-world` folders remain supported only when no new
`textures` folder is present. For advanced packs an optional per-course `pack.txt`
can override the built-in registration, but must match the course source hash.

## R32 cockpits

The reviewed family contains 12 catalogue IDs: `nn32n`, `nr02n`, `nr32n`, `nv12n`,
`nv22n`, `z0351`, `z0773`, `z0906`, `z0907`, `z0908`, `z0909`, `z0910`.
The source kit keeps the exact manifests, GLBs and attribution in
`assets/cockpits/r32`; its inventory validates every file. Existing editable
Blender scenes remain in the development archive; this directory is the runtime
export, not a replacement for those authoring files.

GT4 preparation attaches the family automatically, and desktop/Quest GT4 bundles
include it. For an existing GT4 install, merge the cockpit archive's `modelsets`
folder into the data folder. The runtime discovers `modelsets/gt4/authored`
automatically. These models include GT4-derived bodies; the interior attribution
and licence remain in each `attribution.json`.

## Build the separate packs

```text
python scripts/package-cockpits.py --output <new-cockpit-zip>
python scripts/package-hd-textures.py --source <prepared-runtime> --output <new-HD-zip>
```

The HD builder reads prepared `arcade/generated-world` and
`simulation/generated-world` assets. It exports referenced PNGs, shared images and
optional hash-keyed ASTC caches, excluding discs, saves and registration files.
Use `--png-only` for an image-only pack without GPU caches. Developers can refresh
`src/gt2formats/generated_texture_catalogue.h` using `--catalogue <header-path>`
when shipping a newly registered art set. Custom replacement PNGs do not require
rebuilding the player.

## Sponsor replacement regression audit

Build `gt2sponsorpackaudit` and create a separate diagnostic pack (requires Pillow):

```text
python scripts/audit-sponsor-pack.py --pack <HD-pack.zip> --output <new-empty-marker-folder>
gt2sponsorpackaudit <disc-image-or-data-folder> <new-empty-marker-folder>
```

The audit loads every registered course/backdrop present on that disc using the
production texture loader. It checks every sponsor binding and renders each
registered source variant in both eyes. It also forces each available dynamic
sponsor TIM through the production upload path, avoiding random race selection.
Run other regional discs with `--dynamic-only` to check their additional logos.
Compare the printed `DYNAMIC` hashes across runs with `sponsor-inventory.json` to
verify coverage of the entire pack. The diagnostic pack contains green markers;
never install it over a player's texture pack. Normal race captures remain a
separate check of scene geometry and billboard visibility.
