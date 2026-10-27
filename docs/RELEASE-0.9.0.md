# GT2 0.9.0

GT4 cars can now join GT2 cars in the same race. This release also restores more
original GT4 interiors and brings together the cockpit, reflections, road contact,
smoke performance, HD media and startup fixes.

## GT4 cars and interiors

Import the optional model pack from a compatible GT4 disc image. It adds 722 cars
and supplies 388 replacements for existing GT2 entries: 1,110 catalogue mappings.
The added cars are available through GT4 Showroom in Simulation and the extended
Arcade garage. Paint variants, lights, native materials and wheel dimensions are
preserved by the importer. Existing GT2 cars remain available.

The source kit includes the tools needed to prepare and install the car pack
from the supported disc image. See [GT4 preparation and installation](GT4-CARS.md).

Restored interiors from 66 GT4 source models cover 89 catalogue entries and use
their authored dashboard, seats and controls. The **ORIGINAL INTERIOR** label
identifies these cars in the selection screens. Cars
without an authored cabin receive a generated interior fitted to the body.

The [GT4 cars section](../README.md#gt4-cars) includes the tested disc revision,
interior count and complete list of cars with restored cockpits. Generated cabins
can have fitting errors: the full catalogue has not been checked manually. The
cars use the GT2 simulation, with adaptations for GT4 data; they do not reproduce
GT4 handling exactly.

## Driving and graphics fixes

- Fixed GT2 wheels crossing their wheel arches and cars sinking into the road
  after landings. Corrected GT4 body height and tyre contact on slopes.
- Kept car shadows on the road through climbs and crests.
- Removed reflection film across GT2 wheel arches and broken reflection bands
  on GT4 bodywork. Improved sky colour, night lighting and glass/body separation.
- Improved cockpit roofs, pillars, dashboard joins, seats and mirrors. Fixed
  bodywork hiding the stereo HUD or tracked steering wheel.
- Added VR steering sensitivity, a centre dead zone, Hold/Toggle grip and finer
  steering in native replays. PCVR controllers and desktop gamepads can drive
  together without conflicting steering from an unheld wheel.
- Added the original hidden-car camera to the camera cycle and saved the choice
  across races and launches.

## Performance and installation

Heavy tyre smoke uses less GPU work on supported VR hardware. Texture-cache
fixes keep smoke on the filtered path, while preloaded HD position badges avoid
image-decoding stalls during overtakes. Frame rate still depends on the device,
scene and graphics settings.

Arcade startup no longer loads every GT4 paint palette just to build the car-name
list. Disc selection shows a loading screen immediately, and the prepared Windows
launcher no longer fails on an empty script path.

HD preparation now shows picture counts, resumes completed batches and keeps
prepared movies when menus are updated. Font/HUD reconstruction and tiled text
repairs reduce damaged lettering and pinholes.

## Download and update

`GT2-0.9.0.zip` contains the common source, Windows/PCVR files, signed Quest APK,
compiled browser version and installation/build scripts for the supported native
platforms. Supply your own game images. Converted GT4 assets, disc data, BIOS and
saves are not bundled.

GT4 preparation keeps unfinished conversions in a staging directory. The desktop
bundle installer verifies its contents before updating the runtime, retains
replaced GT4 assets under `gt4-backups/`, and preserves discs, saves and settings.

Keep the GT4 catalogue and its model pack together when updating: saved cars from
the expansion require those files. The browser version remains a flat-screen GT2
build and does not load the native GT4 model pack. See [installation](../README.md#install-and-play)
and the [full changelog](../CHANGELOG.md#090---gt4-cars-restored-interiors-and-rendering-fixes).
