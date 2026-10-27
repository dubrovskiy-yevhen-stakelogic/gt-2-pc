# Cockpit view

The Driver camera has a three-dimensional cabin fitted inside the selected car's original body. Its hood, roof, pillars and window outlines retain the original geometry. Window openings follow the glass textures; the inner frames receive dark trim while the exterior keeps its paint and reflections. Cockpit view is enabled by default.

Cars without authored interiors use shared procedural dashboard, steering wheel, seats and lower door panels, with a left-side driving layout. The speedometer and tachometer follow the live car. Authored GT4 interiors retain their own furniture and driving side: the seat and interactive wheel align with the model's steering rim. Replacing that rim with the interactive or decorative wheel hides the original rim, so only one wheel is drawn. Generated cabins do not require assets from another game.

Car previews in Arcade CAR SELECTION and GT Mode display **ORIGINAL INTERIOR** only when the currently loaded model has an authored cabin. Models with generated cabins have no label. The check respects whether GT4 replacements are enabled. Both kinds provide a cockpit view.

The GT4 pack restores authored cabins from 66 source models across 89 catalogue mappings. The [GT4 cars list](../README.md#gt4-cars) names every restored model. Interior availability and an open roof are separate properties: Toyota SERA keeps its closed race shell with authored dashboard, console and both front seats extracted from its showroom source. The showroom's open door is excluded. Special cars with a tiller or paired grips retain those controls when using the original model; virtual-wheel mode replaces the reviewed control geometry. Presentation eye positions are calibrated to the geometry, not claimed to be recovered original GT4 head poses.

Native assets must be rebuilt with `scripts/build_gt4_native.py` and the hash-bound decisions in `db/gt4-native-interiors.json`. The importer fails when a mapped open source has no interior decision, rather than silently losing its cabin. `native-v16/interiors.json` travels with the model bundle; installing the executable alone does not add the new geometry. `scripts/check_native_interiors.py` renders eight stereo directions per newly selected cabin, checks eye/control reach and reproduces the Model T alpha-fail defect as a negative control. Existing window, shape, surface, seating and frame catalogue gates remain required.

Complete authored interiors use their own floor and firewall. A generic enclosure
must not close intentional gaps around an open chassis. The native-interior gate
compares open pixels against the authored body and reproduces the former extra
walls with `GT2_AUDIT_LEGACY_NATIVE_ENCLOSURE=1`. Historical open carriages without
a dashboard receive no floating instrument panel; the ordinary HUD remains available.
NIKE's reviewed upper canopy triangles and matching reflection triangles are
exterior-only, while the lower shell, silver body panels and controls remain intact.
The source hash and triangle selections are part of the importer policy, not edits
to a generated mesh. Unit checks reject stale selections and preserve all instances.
SERA also retains the fixed inner door jamb (menu shape 7, packets 6/7/8).
This original geometry closes the right dashboard/door joint; the articulated
open door remains excluded. The native gate checks closed cabins for exposed
world pixels below the window perimeter in all eight stereo directions. It
recreates the prior missing-jamb defect in a separate owned-data fixture and
requires that fixture to fail, while the complete model passes.

```powershell
python scripts/check_native_interiors.py --disc <simulation.bin> --pack <gt4-modelset> --native <native-assets> --expansion <expansion> --output work/native-interior-candidate --workers 4
```

For a single large window scene dump, set `GT2_AUDIT_CAPTURE_SCENE` to a
`yaw,pitch` pair such as `0,0`; `all` captures every failing pose.
`GT2_AUDIT_GEOMETRY=1` writes geometry CSV without these large scene dumps.

## Controls

1. In a single-player race, **C** or the assigned camera button cycles **Cockpit → Chase 1 → Chase 2 → Original (hidden car) → Cockpit**. The last selection is saved and restored on retry, the next race and the next launch, across Arcade and Simulation.
2. Open **Cockpit / driver view** with **F10**. In VR, open settings with **L3 + R3** or **both grips + Menu**.
3. **Original (hidden car)** hides the player car and cockpit. When **Virtual wheel** steering is selected, the interactive wheel and tracked hands remain visible and usable. With stick/gamepad steering, no interactive wheel is drawn. This view is part of the camera cycle; it has no separate settings toggle.
4. Adjust **Default seat height** from **-20 to +20 cm** and **Seat forward / back** from **-20 to +40 cm**, in 2 cm steps. Positive values raise the eye or move it farther from the wheel. VR orientation follows the headset; the desktop view is wider and aimed slightly downward to include the dashboard.
5. Turn **Steering wheel** off when using a physical wheel if it obstructs your view. This changes the decorative wheel only; steering input and force feedback remain active. In VR virtual-wheel mode, the interactive wheel takes its place.
6. Set **Rear-view mirror** to **OFF** to remove the cockpit mirror and its rear-view pass. **Mirror size** adjusts both dimensions from **25% to 100%**, in 5% steps, around its fitted centre. **HUD elements > Rear-view mirror** also controls its visibility.

**This car seat height (GT2/GT4)** saves an absolute height override for the current car and model family, independently of the default height. The same car can have different heights with GT2 and GT4 models. **Reset this car height to default** removes only that override. Both settings use the same -20 to +20 cm range.

Settings save automatically across Arcade and Simulation. **Reset default cockpit settings** restores the visible decorative wheel, enabled full-size mirror and zero default seat adjustments; individual car heights and the selected camera remain saved. The instrument HUD starts **OFF** for new profiles; the physical dashboard instruments remain visible. Updates preserve saved preferences.

## Seating and visibility

Seat adjustments move the camera and procedural driver seat together. The cushion stays about 70 cm below the fitted eye, and the backrest stays behind the player instead of being shortened against the roof. Cars with authored GT4 interiors retain their original furniture. Authored and generated interiors use separate default eye offsets to fit their seating layouts. Eye adjustments are independent of the wheel anchors. Height offsets retain the full range, including convertibles; choose a lower per-car setting if the eye reaches a closed roof. The interactive VR wheel has its own reach calibration.

The instrument pod sits below the bonnet/cowl. The dashboard follows the curved windscreen base and overlaps the bonnet underneath its edge, with closed joins to the door trim. Body cutting follows that curve instead of removing whole triangles inside a rectangular box. Door cards follow the lower edge of the actual side-window openings. Inner body lining, dashboard and door edges use a common neutral dark colour. The steering column reaches the wheel hub, including its saved height, distance and world scale.

Virtual-wheel sensitivity below 100% softens the centre and smooths every change reaching the game axis, including small corrections and return to centre. At 50%, the response reaches 90% of a new target in about 414 ms; at 75%, about 207 ms. Steady input still reaches full lock. The visible wheel follows the hands immediately, and releasing the wheel or losing tracking clears steering immediately. At 100% and above, the temporal response remains immediate. This setting does not change physical-wheel or stick input.

Hands and the interactive wheel follow the body during suspension roll, pitch and impacts. Controller tracking and steering input remain in seated coordinates. The cabin, wheel and hands appear when the native countdown camera switches to Driver in its final second; vehicle movement begins at GO as before.

Window cutting preserves separate panes, pillars and opaque structural trim. Connected dark glass reflections are cleared, and exterior alpha decals over separately modelled glass are excluded from the inner windshield so they cannot become false black triangles. Roof paint and detached texture highlights do not create window holes. GT4 exterior shells also clear connected, paint-invariant glass regions in texture coordinates, retaining separate panes and their frames. Authored open interiors and the exterior model retain their original geometry. These operations prepare a cached cockpit mesh; source vehicle assets are unchanged.

## Mirror and rendering

The central mirror is a physical surface near the upper windshield with scene depth and stereo parallax. Its full-size frame is 25 cm wide and 7 cm high, fitted behind the windshield with a support to the roof.

One low-detail rear camera renders to a shared 480 x 128 texture, sampled in both eyes. There is no extra rear camera per eye, CPU readback or duplicate HUD mirror in cockpit mode. Reducing the visible mirror size keeps that pass; switching it off removes the pass. It is available in cockpit races whose original mode suppressed the HUD mirror, so those modes gain a rear-view pass when it is enabled.

GT2's **Original** reflections retain the original additive environment texture. **Modern** uses nearby course scenery plus a restrained sky contribution. The distant sky's intensity and colour saturation are bounded so blue skies do not overwhelm dark body paint. Capture coverage keeps the sky behind opaque scenery, including black tunnel roofs. This shares the existing environment captures; it adds no render pass. GT4 retains its native reflection shading in both external and cockpit views. Its lamp callbacks also remain active in cockpit views, including reflective and wheel-mounted lamp geometry. Interior lining and window openings are excluded from the GT2 reflection draw.

Occupied cabin surfaces, the virtual wheel and hands share a foreground depth range. They retain depth ordering with each other while road and grass cannot draw through them during suspension travel or contact with raised terrain. Transparent window openings continue to show the outside world.

GT4 replacements default to enabled when their model set is installed. A saved explicit on/off preference still takes priority. GT4 body paint selection does not change the original donor wheel-rim palette.

Side mirrors share the rear-camera image. Imported mirror glass can be isolated within a shared mesh primitive without treating adjacent paint as a mirror. Cars drawn into the Original camera's HUD mirror obey its projected rectangle in both eyes. The central mirror remains a fixed rear camera rather than a physically traced reflection.

## Developer regression checks

`gt2cabinpaintaudit` takes the same disc, pack and output arguments as the tools below. It checks both eyes for near-black cabin coverage, aligns authored driver positions with steering geometry, checks that replacement controls exclude the original rim, and compares equal-luminance neutral/blue skies to bound body recolouring. `gt2stockenvironmentchecks` separately verifies empty-sky versus opaque-black scenery coverage. Binary-transparent native blend draws must not become opaque black cabin caps, even when their source alpha test is disabled. `gt2modelsetchecks` covers this distinction and left/right steering-rim detection with synthetic geometry. `gt2cockpitshopchecks` checks catalogue metadata and the selection badge bounds.

Build `gt2cockpitregression` and `gt2paintwheelaudit` with CMake. Both tools take an owned Simulation disc, the GT4 model-set directory and an output directory under `work`; optional trailing car IDs restrict a diagnostic run. Omit IDs for the complete named, non-deleted car catalogue plus every GT4 mapping. Set `GT2_GT4_NATIVE` and `GT2_CAR_EXPANSION` to the corresponding prepared asset directories.

Set `GT2_AUDIT_COCKPIT=1` for `gt2cockpitregression`. It measures the generated driver seat and renders both eyes at five seat settings, including forward 20 cm. It checks bonnet seams against the uncut car, excludes windshield and one-pixel silhouette edges, and places raised green ground through the chassis to detect interior penetration. The Viper from 0.8.4 is a negative control for the seam, lowered-seat clearance and raised-ground checks.

`gt2paintwheelaudit` selects every body paint through the production material path and compares wheel coverage from both sides; GT4 variants also check for disappearing bright donor-rim pixels. `gt2cabinsealaudit` checks the rest of the closed cabin from eight directions and three head offsets. `gt2geometryaudit` covers road and animated-wheel witnesses with steering and suspension travel. These offscreen checks do not establish headset comfort or frame rate.

## Limits

- The cockpit applies to live single-player driving, including look-back. Replays, the external starting flythrough and two-player split-screen retain their existing views.
- Fitting follows the available low-polygon body geometry. It cannot add detail absent from the original model, and unusual bodies or extreme seat positions can still need adjustment.
- Cockpit settings change presentation; vehicle simulation and save formats remain unchanged.



`gt2surfaceaudit <disc> <gt4-pack> <work-output> [car-ids...]` adds three elevated/leaned head positions and a view back over the nose, with three steering/suspension states while keeping cockpit rendering active. Enable `GT2_AUDIT_COCKPIT=1` and `GT2_AUDIT_WHEEL_COLOR=1`. It checks opaque bonnet paint and coverage, protruding trim/tyres, reflection submission, exact diffuse geometry in six imported lamp states and HUD mirror clipping. Blended shadows are excluded from the opaque pixel comparison; the physical trim-intersection check still uses the full authored skin. Native furniture is identified by an independent diagnostic rendering, so its intentional exposure change is excluded only from the paint-colour comparison. Holes and wheel overlap remain checked there. The real cowl boundary excludes windshield geometry from the bonnet reference. Panels above the windshield's centre height remain eligible, and every steering state must have measurable reference coverage. Synthetic aperture and raised-trim controls, plus deliberately depth-forced tyres, verify that broken geometry is detected. `GT2_AUDIT_INJECT_BONNET_HOLE=1` temporarily removes real submitted front-panel triangles; this negative run must fail, including on the low-nose and open-wheel models.

`gt2cockpitmaterialaudit <arcade-disc> <gt4-pack> <work-output> [car-ids...]` renders reflective/matte, day/night and changing rear-view targets. Without IDs it checks every GT4 replacement in the selectable Arcade catalogue. Use `GT2_WINDOW=hidden` and `GT2_NO_FOCUS=1` for background host runs. The seat regression report separates dimension, seam and ground-occlusion failures; extreme-adjustment dimension failures are not hidden or corrected by moving authored furniture.

The bonnet comparison tags candidate fragments while rendering the complete source depth/alpha scene, and compares them against the complete occupied shell. Cross-boundary occluders remain present after re-triangulation. This distinguishes a visible panel from an identical black panel hidden by a wiper. Its negative control uses two identically black layers. Live mirror overlays are included on both sides of the comparison. Rounded inner window trim and the projecting instrument hood are bounded by the curved cowl; seating calibration does not raise the surface-fitted under-bonnet seal through exterior metal.

The generated driver seat has a raised base when needed to keep its cushion within seated reach of the configured eye height. Raising the player's height also raises that seat; a pedestal closes the space underneath. This furniture adjustment preserves camera placement, wheel anchors, passenger seating and saved settings. Authored seats remain part of their native interior.


## Window regression gate

Build `gt2windowaudit` and run the owned-asset catalogue gate from the repository root:

```powershell
python scripts/check_cockpit_windows.py --disc <simulation.bin> --pack <gt4-modelset> --native <native-assets> --expansion <expansion> --output work/window-candidate --workers 4
```

The runner snapshots the executable, inventories every GT2/GT4 path from the supplied
catalogue and renders both eyes at seven yaw angles and two pitches. Two flat
backgrounds expose dark and pale translucent layers; a checker image exercises
live mirror masks. Wheel geometry is outside this window gate. Controlled
background planes sit just ahead of and below the passenger cell, so exterior
lenses below the sill do not masquerade as window tint. Every cabin/window and
lamp-callback triangle is otherwise retained; the surface audit also keeps wheels. Side views must also have a visible opening, so an opaque
window cannot pass simply because both backgrounds are hidden. Interior materials
are rendered normally, without diagnostic backface recolouring.

Retaining the old glass on the Tuscan (`vst6n`, configurable with `--negative-car`)
must produce a visible failure. The full run requires all 14 poses per model path,
no visible tint cores and no missing imports. A failed import is recorded in
`errors.csv` and testing continues with the other cars. It never creates fake
passing views. Missing/duplicate views and partial imports are also covered by
`python tests/cockpit_window_runner_checks.py`.

Window preparation clears layered glass within the passenger cell and retains
opaque frames. Paint variants may change the glass tint without changing its
classification. Transparent body decals below the window sill do not define
reflection holes. A polygon spanning both sides of the car cannot be a pair of
side mirrors. These rules are exercised by synthetic checks and the full catalogue;
they do not substitute for driving and visual acceptance in the headset.

## Upper-frame and exterior backface regression

Generated GT4 cabins keep the authored opaque roof/pillars above the curved
cowl. The replacement volume still removes lower furniture, closed inner caps
and translucent glazing passes. It does not add a second procedural window
frame. This is independent of eye position and per-car calibration.

Build `gt2frameaudit` and `gt2windowaudit`, then run:

```powershell
python scripts/check_cockpit_frames.py --disc <simulation.bin> --pack <gt4-modelset> --native <native-assets> --expansion <expansion> --output work/frame-candidate --workers 4
```

The gate inventories every model path and measures lost opaque surface above
the highest curved cowl station during furniture subtraction. Re-enabling the
old unbounded cut on the Tuscan must fail. Failed models get left/right eye
views, raised head positions and four exterior views; set `GT2_AUDIT_CAPTURE=1`
when running `gt2frameaudit` directly to capture passing models too. The test
requires `GT2_AUDIT_COCKPIT=1` and fails incomplete catalogue runs.

The CAR4 importer starts with backface culling enabled, as the original car
draw program does, and retains explicit disable commands. Regenerate native
assets made by importer metadata version 6 or older. Original triangulation is
preserved. The gate captures both rear-window borders of the Tuscan in both
eyes and must reproduce black inner-shell fragments when culling is disabled.
A synthetic inner/outer shell raster control checks the same render state. Separate
window, surface, mirror and cabin-seal gates remain mandatory: preserving a
frame does not by itself prove that its windows are clear or its bonnet intact.

Depth-writing blended headlamp lenses below the cowl must not create cabin
apertures ahead of it. Preserve depth-writing glazing above the cowl: low
racing windscreens can extend ahead of the fitted knee. `check_glazing_body.py`
reproduces the former Prius lamp-rim cuts with
`GT2_AUDIT_UNBOUNDED_DEPTH_GLASS=1`, then requires both Prius variants to pass
the unchanged elevated cockpit surface gate with that override removed.
It also reproduces GT40 screen tint with a longitudinal-only boundary
(`GT2_AUDIT_FLAT_DEPTH_GLASS_BOUNDARY=1`) and requires the corrected height-aware
boundary to keep both GT40 variants clear:

```powershell
python scripts/check_glazing_body.py --disc <simulation.bin> --pack <gt4-modelset> --native <native-assets> --expansion <expansion> --output work/glazing-body-candidate
```

Run the complete window and surface catalogues after changing this shared
boundary; passing the Prius pair alone does not establish catalogue coverage.

Generated fascia shape has a separate [regression gate](COCKPIT-SHAPE-REGRESSION.md).
Closed geometry alone does not detect side protrusions caused by fitting the
windshield to mirrors or side quarter-windows. Run `check_cockpit_shapes.py`
alongside the window and surface catalogues when changing cockpit fitting.
