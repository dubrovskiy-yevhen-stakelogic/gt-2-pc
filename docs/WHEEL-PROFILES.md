# Offline wheel profiles

The 0.4.0 Windows input backend embeds 72 device records in
`src/platform/input/wheel_profiles_data.h` (about 15 KB of source). Players do not
download or install another racing game. A device record describes physical
controls; inclusion is not a claim that the hardware has been tested in GT2.

Matching uses Windows USB vendor/product IDs, not a device-name substring or a
mapping guessed for an entire manufacturer. Driver compatibility modes can use
different IDs. A steering base, separate pedal set and separate shifter are
selected independently. With multiple eligible devices the player chooses one
in **Devices and clutch**. Once selected, an unplugged peripheral does not cause
the game to silently switch to another source.

## Mapping evidence

Axis names/directions and button/action relationships were normalized from a
local inspection of BeamNG.drive factory input maps, Steam build 24617469. Exact
reference filenames and SHA-256 hashes are recorded in
`wheel-profile-provenance.json`. The compiled data contains the relevant
hardware facts in GT2's own representation, not the original input-map files,
game scripts, assets, force curves, force gains or response filters.

The [BeamNG binding documentation](https://documentation.beamng.com/modding/input/bindings/)
describes factory mapping locations and USB matching. Read-only extraction is
implemented by `scripts/audit-wheel-inputmaps.py`; the audited intermediate
catalogue is compiled by `scripts/compile-wheel-catalog.py`. Neither tool nor
the reference game is required to build/run GT2 from the checked-in header.
The generated revision changes when mapping data changes, allowing automatic
setups to refresh without resetting a player's FFB strength or gearbox choice.

The inspected source had 74 candidate racing-device records. Two analog-only
accessory records are not compiled because this backend does not consume their
analog handbrake binding. Some compiled records are components or partial
profiles: 41 source candidates supplied steering/throttle/brake together and
28 wheel candidates supplied all seven basic menu actions. For missing controls,
the guided setup records the actual connected device. Extra menu buttons can be
assigned without changing driving bindings.

Examples of deliberately distinct mappings:

- Fanatec GT DD Pro / CSL DD (0EB7:0020): steering X, inverted accelerator Y,
  inverted brake RZ, optional inverted clutch Slider 1.
- Fanatec CSL LC pedals over USB (0EB7:6205): accelerator X, brake Y, clutch Z,
  all non-inverted. This is different from pedals connected to the wheel base.
- MOZA CRP pedals (346E:0001): RX/RY/RZ, non-inverted.
- Logitech G25/G27: clutch on Slider 2.

Combined pedal axes use opposite halves of one axis. Clutch input is opt-in:
advertising a clutch axis does not establish that a third pedal is fitted.

## Exclusions and rim variants

- The inspected Fanatec USB shifter 0EB7:1A92 record has reversed action/control
  fields. It is not activated automatically; the H-pattern wizard records its
  real gate buttons, including reverse and neutral transitions.
- The inspected Microsoft SideWinder 045E:0034 and Thrustmaster Ferrari F1
  Integral T500 records have conflicting identifiers. The latter also contains
  an unverified slider name. Neither discrepancy is repaired by guessing.
- T150, TMX and RGT reference records contain metadata only, so no automatic
  assignment is inferred from their names.
- MOZA R16/R21, R9 and R5 reference button mappings describe the CS V2 rim;
  Simucube 2 Pro/Sport mappings describe a wireless rim with paddles. Automatic
  axes still apply, but these button presets require choosing the matching rim
  option. Other rims use **Learn buttons on my wheel**.
- Digital pedal buttons and analog handbrakes are not converted into guessed
  analog axes. Unknown layouts still need device-specific work; a partial
  record is not complete support for every feature of that device.

## Verification boundary

Fanatec bases can expose multiple HID game-controller collections with the same
USB ID and Windows name. Automatic selection and the normal device picker use
the primary collection for the catalogue layout; extended controls remain
reserved from ordinary gamepad input. Two physically separate bases still
require a choice. This was checked on the connected 0EB7:0020 base: collection 1
has 108 buttons, collection 2 has 63, and steering, accelerator and brake were
observed changing on collection 1. See also
[Fanatec's explanation of multiple devices](https://www.fanatec.com/us/en/s/faq-why-is-the-csw-v2-5-and-the-csl-e-wb-ps4-detected-as-two-or-even-3-devices-on-pc).

Device lists use catalogue model names and filter independent USB pedals and
shifters separately from wheel bases. Base-connected pedals are labelled by
their connection, since the base USB ID cannot identify the attached pedal model.

Host tests cover exact-ID matching, independent USB devices, pedal polarity,
ambiguous selection, source retention after disconnection, settings migration,
clutch opt-in, menu buttons, shifter wizard validation, physics/replays and force
limits. Desktop overlay tests run the real executable. Physical driver/firmware,
force direction and driving feel still require hardware checks.

DirectInput axis discovery uses `GetObjectInfo(DIPH_BYOFFSET)` against the chosen
DIJOYSTATE2 layout. `EnumObjects.dwOfs` is a raw device offset and is not a safe
substitute; see Microsoft's [object information documentation](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee417906(v=vs.85))
and [offset warning](https://learn.microsoft.com/en-us/previous-versions/windows/desktop/ee416612(v=vs.85)).
