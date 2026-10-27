# Generated cockpit shape regression

The 0.8.9 correction addresses two large side protrusions in the generated
cockpit of the original GT2 TVR Tuscan. A sealed cockpit is not necessarily a
correctly shaped cockpit: the old aperture tests accepted this deformation.

## Cause and correction

`WindowCorners` previously selected the widest suitably sloped surface near the
windshield. Mirror shells and angled side quarter-windows could satisfy that
test. The generated dashboard then followed those false corners into the cabin.

Front-window fitting now follows connected window-facing surfaces that reach
the centre-line windshield. Connectivity through an unrelated door panel does
not qualify a mirror. Side-facing quarter-windows are excluded using the same
normal distinction as side-window removal. The authored body mesh, native GT4
interiors, wheel controls and saved settings are not rewritten by this fit correction.
The original lateral seat anchor is retained independently of trim width.
Cross-sections use that same connected window surface; scanning the entire car
again would reintroduce peaks between the corrected endpoints. For genuinely
deeply swept windscreens, the fascia's central surface is brought back enough
to keep its recess within 18 cm, independently of the seat and wheel anchors.

## Tests

`CockpitFitChecks` includes detached mirrors, mirrors welded through a door
panel, and connected side quarter-windows. Adding these accessories must not
change the known windshield corners. The unfiltered algorithm must deform the
fixture, so the test cannot silently stop exercising the defect.

`gt2shapeaudit` measures actual generated fascia triangles, separately on the
left and right. The generated design allows up to 20 cm of rearward sweep
relative to the central fascia; larger wings fail. This is a presentation
constraint, not a measurement of an original manufacturer's interior.
Authored native interiors are retained and are not judged by this generated
fascia constraint. A closed two-wing synthetic deformation must fail while its
flat counterpart passes. The negative control warps a copy of the actual
generated fascia triangles, so fixing the generator cannot disable the detector.

`scripts/check_cockpit_shapes.py` also runs the original unfiltered fit on the
owned-disc TVR as a negative control. It must fail in GT2 mode and pass after
correction. It then enumerates the current catalogue and checks every available
GT2/GT4 model path, rejecting missing or duplicate paths. Each path is rendered
in stereo at left, centre and right headings at two downward pitches. The TVR
captures and all failure captures are retained for visual review. Shape is
judged from geometry; successful rendering alone is not a visual pass.

The CSV records seat and wheel-anchor differences against the complete original
GT2 fitting pipeline, including the existing forward-clearance refinement.

```powershell
python scripts/check_cockpit_shapes.py --disc <owned-simulation-disc.bin> `
  --pack <model-set-directory> --native <native-asset-directory> `
  --expansion <expansion-directory> --output work/cockpit-shape-check --workers 2
```

Build `gt2shapeaudit`, `gt2windowaudit` and `gt2assetchecks` first. The output
directory must be fresh. The runner records executable, disc and asset-manifest
hashes. It does not download data, launch the game or change headset settings.

Also run the existing window, upper-frame, bonnet/wheel-intersection and occupied
cabin regression checks after changing fit boundaries. Host tests and captures
do not replace physical headset acceptance.

## Stock roof, pillars and mirror attachment

The stock occupied body retains its authored roof and window silhouette. It
must not receive a second generic rounded frame across that opening. Where two
glazing surfaces share a sloping pillar edge, the narrow lining follows that
actual edge; the old baked dark fringe is removed from those pane surfaces so
it cannot remain as detached tabs. This does not change the normal exterior.
Shared edges must also lie inside the passenger aperture and above its belt.
The full surface audit caught headlamp and rear-lamp seams with the same glazing
palette/face angles; their false supports changed original exterior paint.
Front, rear and low-body negative fixtures now reject these non-cabin edges.
An already identified roof panel receives continuous opaque inner lining;
livery alpha must not send that panel back through the window-cutting path.
The fixtures distinguish such a roof from sloping glazing, which stays open.

Small detached side-mirror components are checked for an alpha break between a
body-seated root and opaque housing. A repair is limited to a short 6 mm radius
stalk along that component's own edge. A remote accessory, coloured non-mirror
surface, wholly transparent plane or intact stalk must not receive a mount.
The fixture suite checks these cases; the source texture and exterior paint
remain intact. This repair does not classify every possible mirror topology.

`gt2frameaudit` independently casts alpha-aware rays through the actual occupied
stock mesh at opaque near-horizontal source-roof samples and along the expected pillar/mount
seams. It excludes independently witnessed glass and its coincident decals from
the structural roof reference. A grazing-angle allowance follows the physical
support radius; a fixed depth allowance would report intact sloping posts as
holes. Native GT4 paths retain their existing upper-frame area preservation
gate and exterior backface raster controls.

An opaque roof with a removed triangle must fail. On the owned-disc TVR,
`GT2_AUDIT_REMOVE_STRUCTURE=1` restores the broken stock seams in the production
upload path; the catalogue runner requires both pillar and mirror checks to
fail. Corrected controls must pass before the full catalogue starts. CSVs record
sample counts and missing coverage separately, including zero-sample categories.
Use `GT2_AUDIT_CAPTURE=1` for PNG views; the much larger replay payload is saved
only when `GT2_AUDIT_SAVE_CAPTURE=1` is explicitly requested.
