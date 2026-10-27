# Platform compatibility

## Showroom, garage and open prototypes

Build `gt2showroomaudit`, `gt2nativecontractaudit`, `gt2arcadegaragechecks` and
`gt2interioraudit`. Run the owned-data catalogue regression from the repository root:

```powershell
python scripts/check_showroom_regression.py --disc <simulation.bin> --pack <gt4-pack> --native <native-v16> --expansion <expansion> --output work/showroom-candidate
python scripts/check_native_interiors.py --disc <simulation.bin> --pack <gt4-pack> --native <native-v16> --expansion <expansion> --output work/interior-candidate
```

The showroom check covers every native mapping from three views. It compares
the body environment coating with the same draw list without that coating;
independent transparent lenses remain in both images. GT-One's former coating
over empty wheel/body cutouts must fail the negative control. A positive shine
witness prevents disabling reflections from passing. Successful temporary PNGs
are removed except for the three named witnesses; failures and CSV rows remain.
`GT2_AUDIT_CAPTURE=1` retains all captures for a direct executable run.

Native wheel export requires `build_gt4_wheels.py --showroom <native-pose.json>`.
The input is bound to the owned source hash. No body-bounding-box height fallback
is used. Runtime placement is checked against the exported positions and dimensions;
`native_wheel_pose_checks` supplies synthetic matrix/ground tests. Models unavailable
in that native input retain the established fallback path.

Run `gt2arcadegaragechecks <arcade.bin> <work-output>` with the desired expansion
environment. It checks all catalogue names, actual glyph bounds, missing-title
identity, shared font VRAM after logo uploads, and expanded-save preservation.
Original `unknown` performance bars are not replaced by invented ratings.

Open authored cabins additionally compare the forward/upper stereo field with
the source geometry. A generated enclosure may not block it. Host checks and
package installation do not establish physical headset acceptance or FPS.

## Tyre contact and hillside shadows

Build `gt2roadcontactaudit`, then run from the repository root against prepared
disc data and the matching GT4 model set:

```powershell
./scripts/audit-gt4-road-contact.ps1 -DiscDirectory <prepared-disc> -ModelSet <gt4-pack> -Expansion <expansion> -Stock -Output work/road-stock
./scripts/audit-gt4-road-contact.ps1 -DiscDirectory <prepared-disc> -ModelSet <gt4-pack> -Expansion <expansion> -Output work/road-gt4
./scripts/audit-gt4-road-contact.ps1 -DiscDirectory <prepared-disc> -ModelSet <gt4-pack> -Expansion <expansion> -Stock -Drive -Cars bmmgn -Output work/road-landings
```

Use `-Build <build-directory>` if the executable is outside `build_gt4_models`.
The catalogue checks cover real Seattle road sections, suspension load states,
wheel and body clearance, and shadow vertices and triangle centres. The driving
check follows the Seattle jump section and records airborne samples and landings.
Stock models without a matching USA Arcade or Simulation configuration are listed
in `unconfigured.csv`; they are not counted as tested configurations.

For negative controls, set `GT2_AUDIT_NO_ROAD_FIT=1` or
`GT2_AUDIT_NO_ROAD_SHADOW=1`, then repeat the relevant check. Remove these variables
before the passing run. These checks measure rendered geometry against road
surfaces; they do not measure headset frame rate.

## Supported platforms

| Platform | Requirements and supported setup |
| --- | --- |
| Windows | Native desktop game; OpenXR for PCVR through SteamVR, Meta Link or Virtual Desktop |
| Quest | Standalone ARM64 application; installation and controls in [QUEST.md](QUEST.md) |
| macOS | Native Apple Silicon or Intel build, SDL2 and Vulkan through MoltenVK; see [MACOS.md](MACOS.md) |
| Steam Deck / Linux | SDL2 and Vulkan; Steam Deck installation uses Desktop Mode and Flatpak |
| Browser | WebAssembly and WebGL2, sufficient RAM for the full BIN, 8192-pixel textures and 512 array layers |

Platform-specific controls and installation requirements are documented in the
linked guides. Native VR, physical-wheel force feedback and optional HD media
are not available on every platform. See [browser limits](WEB.md).

Cockpit geometry follows the original low-polygon car body; unusual cars or
extreme seat positions may need manual seat adjustment. Browser performance
depends on the graphics driver and memory limits. Steam Deck Gaming Mode and
suspend/resume behavior may differ from Desktop Mode.

When reporting a problem, include the release version, platform, graphics device,
disc revision and the steps needed to reproduce it. Do not attach disc images,
firmware, credentials or personal saves to a public report.

## Developer regression checks

Run `ctest --test-dir <build-directory> --output-on-failure` after building the
test targets. The source also includes `gt2presentationaudit` and
`gt2cockpitregression` for an owned disc and a locally prepared GT4 model pack.
They render both eyes through the production Vulkan scene builders, checking
steering-wheel/HUD visibility, trim dimensions, live mirrors, seat adjustment,
bonnet gaps and raised-ground penetration. Set `GT2_AUDIT_COCKPIT=1`; the tools
require their output directory to be inside `work`. Set `GT2_AUDIT_CAPTURE=1`
when additional diagnostic images are needed.

These host checks complement a physical headset playtest. They do not establish
Quest comfort, frame rate or visual acceptance by themselves.

Backdrop ordering, image equivalence and Quest GPU regression coverage across
both disc catalogues are documented in [BACKDROP-REGRESSION.md](BACKDROP-REGRESSION.md).
The sky-first negative control is required because redundant sky shading can
increase GPU cost without changing a single final pixel.

`gt2cabinpaintaudit <disc> <gt4-pack> <work-output> [car-ids...]` covers every
selectable stock/replacement model when IDs are omitted. It checks cabin
luminance, authored steering-side alignment, replacement-rim exclusion and
sky chroma influence in both eyes. `gt2stockenvironmentchecks` additionally
tests sky coverage against opaque black scenery. These checks use locally
prepared local assets. The source kit contains no disc payloads or extracted
models and textures. Its two README screenshots illustrate gameplay; diagnostic
captures remain outside the source package.


`gt2surfaceaudit` extends the corpus with elevated/leaned views that retain the cockpit draw path, steering and suspension travel. It checks bonnet paint/holes, tyre and trim penetration, imported lamp states and clipping of cars inside the HUD mirror. Enable `GT2_AUDIT_WHEEL_COLOR=1` as well as cockpit instrumentation. `gt2cockpitmaterialaudit` uses an owned Arcade disc to test actual reflection, day/night lamp and live-mirror pixel changes on its GT4 replacements. Both use the same disc/pack/work-output arguments and optional IDs as the other audit tools.

Surface coverage also includes a front-to-rear view over the nose. An empty reference is a failure rather than a successful comparison with zero pixels. Run the same models with `GT2_AUDIT_INJECT_BONNET_HOLE=1` to verify detection of deliberately removed front-panel triangles, then unset it for acceptance. Generated-seat dimension limits are unchanged; tall bodies and height adjustments are handled by the generated seat base, without moving the camera to satisfy the test.

Uncut and cockpit body references use the same production depth range so that near-coplanar decals and grilles receive equal depth precision. Live mirror panes are excluded from the bonnet-paint reference by matching their actual triangles; mirror functionality is tested separately. Synthetic fixtures reject excluding adjacent or larger body panels as mirror glass.

## Extended-distance wall regression

Build `gt2wallaudit` and `gt2courseviews`, then run from the repository root:

```powershell
python scripts/check_course_walls.py --disc <arcade.bin> --disc <simulation.bin> --output work/course-wall-candidate
```

Every nonempty course geometry is inspected, deduplicating identical track
payloads within each disc. Contact surfaces, authored co-visible walls and
independent overhead structures must remain intact. Any affected course is then
rendered at every chunk, from driver and chase cameras, in both billboard modes.
The asphalt witness compares unlimited visibility with the authored view; it
must not be newly covered by added scenery. Grass and kerbs are excluded from
that broad witness because valid roadside barriers occupy their footprints.

The Autumn Ring regression additionally retains shoulders in the exact reported
chase view. Restoring the foreign-loop wall must fail in both billboard modes;
the corrected view must pass. Connected top rails remain part of the wall even
when they use another material or duplicated vertices. Removal is conditional
on the conflicting road being visible and the wall being absent from the
original camera visibility list. Its original near views retain the full wall.

The report binds both executables and disc hashes, checks complete pose coverage,
and fails on any missing result. These are host image/geometry checks, not Quest
frame-rate measurements or physical headset acceptance.
