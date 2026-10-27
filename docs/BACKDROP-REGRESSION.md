# Backdrop overdraw regression

An opaque sky drawn before the road shades pixels that the road immediately
overwrites. The image can be exactly correct while Quest GPU time regresses.
The race renderer therefore defers the backdrop until after opaque world
draws, preserving transparency order and depth-clear boundaries.

## Fast, data-free gate

Build and run `gt2assetchecks`. `tests/backdrop_order_checks.h` checks ordering,
masked geometry, transparency, depth clears and the old sky-first negative
control. Also run `python tests/backdrop_runner_checks.py` to check the timing
thresholds used by the corpus runner.

## Owned-disc corpus gate

Build `gt2courseviews`, then run from the repository root:

```powershell
python scripts/check_backdrop_regression.py --disc <arcade.bin> --disc <simulation.bin> --output work/backdrop-new --stride 1 --workers 4
```

This discovers every course record on both supplied discs. Deduplication uses
the contents of track geometry, track textures, backdrop geometry and backdrop
textures together, never just a course name. `report.json` retains every alias.
Both forward and backward views and both billboard modes are checked. Stride 1
visits every track chunk; larger strides are explicitly sampled coverage.

A stopped host-only run can be continued with the same arguments plus `--resume`.
The runner checks the immutable executable snapshot, configuration, catalogue and
completed coverage before reusing results. GPU runs use one host worker.

For every view, the gate requires no opaque world draw after the first sky draw
and no image change exceeding one RGB UNORM unit. A deliberately injected
sky-first run must fail. Failure captures, CSV measurements and exit codes are
retained. Missing output or a failed process cannot count as success.

## Production integration gate

After building `gt2game`, run:

```powershell
python scripts/check_backdrop_game.py --disc <arcade.bin> --output work/backdrop-game
```

It uses disposable settings/cards under the output directory and a hidden
window. Cockpit, chase and hidden-car views must complete; deliberately
restoring sky-first order must trigger the production ordering diagnostic.
This catches a missing optimization call in the actual race renderer.

## Quest GPU gate

Build the Android `gt2renderbench` target. Add these options to the corpus command:

```powershell
--adb <adb.exe> --serial <Quest-3S-serial> --bench-directory <directory-containing-gt2renderbench-and-libopenxr_loader.so> --scale 130 --msaa 2 --foveation 2 --frames 120
```

For each unique course, the runner captures the start and two equally spaced
track chunks. It measures original/deferred/deferred/original ordering on the
same frame (ABBA), with warm-up before each timed run. The four phases share
one process and allocation/pipeline setup; setup time is not GPU frame time. Mean and p95 GPU times
come from GPU timestamps. A paired mean regression greater than 5% plus 0.20 ms,
or p95 greater than 10% plus 0.20 ms, fails. Raw samples and battery state are
saved so thermal drift and outliers can be investigated rather than hidden.

Use `--baseline work/backdrop-known-good/report.json` in later runs. The
baseline must have passed on the same serial and rendering configuration.
Missing cases fail; mean or p95 growth over 10% plus 0.20 ms fails. Keep the
passed report with its benchmark/audit hashes and compressed frame captures.
Change a baseline only after investigating and accepting the difference.

The runner starts only a standalone benchmark in its own temporary directory.
It does not launch or modify the installed game. These are saved-frame GPU
measurements, not proof of gameplay FPS, compositor timing or headset comfort.
Physical driving acceptance remains separate.

Run outputs contain owned game data and stay under `work/` or the private audit
archive. Only the tools, synthetic tests and this document belong in source kit.

## Bounded follow-up of an unstable GPU measurement

Keep a failed corpus report unchanged. After it completes, a paired GPU failure
can be investigated with the same retained frame, device, settings and benchmark:

```powershell
python scripts/recheck_backdrop_gpu.py --report work/backdrop-new/report.json --adb <adb.exe> --bench-directory <original-benchmark-directory> --output work/backdrop-followup
```

The follow-up runs exactly three ABBA repetitions for each failed frame. Every
repetition must pass the original mean and p95 thresholds. It cannot accept a
missing repetition, a different benchmark/capture, incomplete corpus, or a
host/historical regression. Its separate report retains the original report
hash, original failures and all repeated samples. A successful reviewed report
can serve as a historical baseline; it never overwrites the first failed run.
Both reports remain necessary evidence. There is no retry-until-success loop.

## Local geometry investigation

`gt2courseviews disc output course stride begin end lateral height` can inspect
a bounded section without changing the game camera or installed settings.
`GT2_COURSE_AUDIT_CHASE=1` uses the chase offset; `GT2_COURSE_FRACTION`,
`GT2_COURSE_YAW_OFFSET` and `GT2_COURSE_PITCH` vary the observation position.
With `GT2_COURSE_AUDIT_CAPTURE=1`, `GT2_COURSE_CAPTURE_EVERY=1` retains each
sampled VR-billboard frame for a standalone device replay. Keep the range small:
these files contain course assets. The corpus runner clears these diagnostic
overrides so local investigations cannot silently change regression coverage.
