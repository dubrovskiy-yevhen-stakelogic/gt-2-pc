# Cockpit follow-up after 0.9.1

## Known issues

Full-catalogue checks cover 1,110 GT4 variants, 26,640 structural views and 93,240 source-body rows. Window overlap remains flagged on 20 models and source-body openings on 92. These sets overlap. Model IDs and counters are listed in [COCKPIT-REMAINING-0.9.1.json](COCKPIT-REMAINING-0.9.1.json).

## Planned fixes

- Resolve remaining backing/dashboard overlaps with window apertures, particularly TVR Tuscan z0187 and Ford GT z1040/z0050/z0212. Preserve mirror sightlines and fit the backing to the lower glazing boundary.
- Resolve remaining openings in authored interiors, especially Toyota SERA z0357 and Mazda roadsters.
- Distinguish original non-glass translucent details from retained glass in the glazing test. The thin Peugeot 206cc z0332 detail matches the original model geometry and texture; it must not be removed merely to satisfy a glass mask.
- Keep original circular openings separate from mirror surfaces. Preserve decals and accessories without using them as structural fitting evidence.
- Recheck roofs, furniture, instruments, lower body, wheel clearance and both eyes after these changes.

## Regression procedure

Run independent model checks concurrently with a bounded shared worker pool. Schedule previously slow models first. Preserve completed results only when executable and source hashes match. Require unique catalogue coverage and positive/negative controls; missing views remain failures. Keep the original source-body reference independent of generated cabin geometry.

Headset visual quality, comfort and frame timing require separate runtime checks.
