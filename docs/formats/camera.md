# Race cameras (race overlay 0x80010000..0x80011A58)

Status 2026-09-19. US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Code addresses are in
GT2.OVL member 0 (the race overlay, loaded at 0x80010000) unless marked EXE. Evidence: our disassembly of
`work\re\license_race\ram.bin` and `work\re\race_demo\ram.bin` (the camera code is identical in both), Ghidra pseudo-C in
`work\re\*\decomp`, gt2run sessions (`work\re\camera\`), gt2verify rows and captured frames (section 7). Port:
`src/game/camera/race_camera.*` (bit-exact routines), `src/game/camera/game_camera.*` (fed from `sim::RaceSim`), checks
`tools/gt2verify/verify_camera.cpp`.

## 1. Where the camera lives and when it runs

- One camera object per player: view object + 0xC4 + player * 0x110 (0x801FF8B8 in both dumps -> player 1 at 0x801FF97C,
  player 2 at 0x801FFA8C). The race load calls 0x80010000(camera, player) for both, sets camera + 0x9C = the player's pad
  object (0x800A9528 / 0x800A95D8), in game mode 0 (two players) camera + 0x103 = 1 (half-height views), then 0x800100F4.
- Per race frame the frame driver 0x80015B64 runs the physics tick 0x8003EBF0 (which ends with the render transforms
  0x800133F0: car + 0x81C matrix, car + 0x14 = body + 0x600 chunk) and then 0x800100F4 for player 1 (and player 2 in mode 0).
- The renderer 0x800294D4 (from 0x8002972C) calls 0x800298FC -> EXE 0x8007B374(camera, camera + 0xA4, ot, 16): the GTE
  state = P * V^T with P the screen matrix of the window (below). With camera + 0x109 set, no split view, game mode != 0
  and 0x801D5864 > 1 it first draws the rear-view mirror: a copy of the camera moved (0, 0x3333, 0x8000), pitched -32, z
  mirrored (0x8007B25C with (0x1000, 0x1000, -0x1000)), window (-60, 60) x (16, -16) at H 120 in a 120 x 32 rectangle at
  (100, 20) (0x8008034C draw area; 0x8007E780 outlines (0, 0, 120, 32) and (1, 1, 118, 30) in black), then 0x800298FC /
  0x8001545C (cars) / 0x8002993C (0x80018D1C draws no dome with param 1: the mirror's background is the backdrop's two
  flat colours 0x800AF234 + 4 / + 8 split at the horizon; 0x80020110 with param 1: the chunks' mirror copies + 0x94).
  Ported 2026-09-19: camera::MirrorCamera (gt2verify CamMirror: the copy handed to 0x800298FC, 400 cases, 0 mismatches);
  gt2game draws it as a scissored second view after the scene and before the HUD (depth cleared in the rectangle;
  course copies of the camera chunk's render list nearer than 100 m, the cars, the frame). Capture: work/play/prims_license.txt
  primitives 781..913 (draw area (100, 260)..(219, 291) of the second buffer); screenshot work/play/replay/ours_mirror_driver.png
  against work/play/camera/orig_driver.png.

## 2. Camera object (0x110 bytes; `camera::RaceCamera`)

| Offset | Field |
|---|---|
| +0x00 | s16 x, y, w, h: drawing rectangle (0x80010088: 0, 0, 320, 240 >> split) |
| +0x08..0x8F | the EXE camera's GTE state written by 0x8007B374 (P V^T at +8, translation +0x1C, -eye +0x28, clip planes, OFX/OFY +0x5C/+0x60, H +0x64) |
| +0x90 | s16 centreX, centreY = (window centre << 12) / H; +0x94 s16 spanX, spanY; +0x98 s16 H (EXE 0x8007B320; its far argument 0x7FFF is not stored) |
| +0x9C | pad object pointer (logical buttons: +0x6C replay pressed, +0x8C held, +0x90 pressed) |
| +0xA0 | s32 chunk: the course chunk whose render list is drawn (the car's in the race views; 0x80028394 for trackside cameras) |
| +0xA4 | MATRIX V: s16 3 x 3 (camera to world; columns right, up, back: the camera looks along -z) + s32 eye (16.16 m, world +Y up) |
| +0xC4 | s16 yaw, pitch, roll of V (EXE 0x800811B0) |
| +0xCC / +0xD8 | s32 previous eye / velocity = eye - previous |
| +0xE4 / +0xFC | s16 velocity direction (4096 = 1) / s32 speed (EXE 0x80081164) |
| +0xEC / +0xF4 | s16 column 2 (back) / column 0 (right) of V: the sound's listener axes |
| +0x100 | u16 word 0 of the trackside record in use |
| +0x103 | split (half-height) view |
| +0x105 / +0x106 / +0x107 | replays: mode 0 trackside / 1 onboard set / 2 trackside following the leader (mode 0); onboard set entry 0..8; Replay Info |
| +0x108 | the followed car is not drawn (0x800140A4 skips it: driver view, onboard views 0 / 7) |
| +0x109 | rear-view mirror (driver view, not looking back) |
| +0x10A | in-car sound (0x800146D8) |
| +0x10B | camera not attached to the car (trackside); 0 in all race views |
| +0x10C | followed car |
| +0x10E | Camera Position (0 Driver, 1 Chase 1, 2 Chase 2) |
| +0x10F | look-back button held this frame |

## 3. Projection

0x80010088(camera, H, cx, cy): rectangle (0, 0, 320, 240 >> s), window (cx - 160, cx + 160) x (cy' + 132', cy' - 132') with
s = split, 132' = 132 >> s, cy' = cy >> s, at distance H. 0x8007B374 turns it into P = [[sx, 0, cX], [0, -sy, -cY],
[0, 0, -4096]] with sx = (320 << 12) / 320 = 4096, sy = (240 << 12) / 264 = 3723, cX / cY = centre * s >> 12: the picture is
compressed vertically by 240 / 264 (tan of the half field of view: 160 / H across, 132 / H up). H by View Angle (career +
0xB2, 0x80010298): table 0x8002F370 = 277 / 216 / 190 (Narrow / Standard / Wide; 0x8002F376 = 0). The cameras override H:
intro 160 / 921..H / 921..450, trackside records their own (zooming). Native (`camera::ClipMatrix`): the same rows in float,
x in NDC divided by (120 * aspect) - the original's vertical field of view and scale at any window, wider for 16:9 (4:3 =
the original frame). Near plane: gt2game uses 0.1 m (the original clips per primitive in its draw code; not ported).

## 4. The player's race (0x800A951C == 0): 0x800102D8

- 0x800100F4: clears +0x10F / +0x108 / +0x109 / +0x10A, previous = eye, +0x10B = 1; then 0x800102D8 (0x800109FC in
  replays); then velocity, speed / direction (0x80081164), angles (0x800811B0), the axis copies.
- 0x800102D8: held & 0x200 -> look back (+0x10F); pressed & 0x100 -> position + 1 (wraps at 3); while the start hold
  0x800A9520 != 0 the intro 0x80010608, else 0x80010298 (H by View Angle) + 0x800103C0(camera, position).
  With the default key configuration 0x100 = R1 and 0x200 = L1 (gt2run session on the licence race, `work\re\camera\btn`).
- 0x800101FC (load the target): chunk = car + 0x14, V = the car's render matrix car + 0x81C (32 bytes, pad word included),
  angles of V.
- 0x800103C0(camera, position): the target; +0x10B = 0.
  - Driver (0): V moved (0, 0xCCCC, 0) along its own axes (0.8 m up, EXE 0x8007B050 = 0x8008220C); look back = V * Ry(0x800)
    (EXE 0x8007B14C), else mirror on; +0x108 = +0x10A = 1.
  - Chase 1 / 2: yaw = V's yaw - body + 0x73A, pitch = V's pitch + body + 0x73C (the physics' view offsets, 0x8003E8E4: the
    lag; Chase View Type 1 / 2 = 0x801C9990 selects their constants); look back: yaw + 0x800, pitch negated; split: pitch +
    32. V = identity (EXE 0x8007AF60), eye = the car's, rotated (yaw, pitch, 0) (EXE 0x8007B088 = V * R, R from 0x80081374),
    then moved (0, y, z) along its axes: table 0x8002F350 = {1.8 m, 1.8 m, 5.4 m, 6.8 m} (y1, y2, z1, z2), split view
    {1.26, 1.62, 6.48, 8.5 m} at 0x8002F360. No distance per car size: the same offsets for every car.
- The intro 0x80010608 (hold h, initial hold h0 0x800A951E, H0 = the View Angle's):
  - h < 60: H = H0 + (921 - H0) h / 60 (0x80010598, 64-bit division) on the chosen race view (0x80010388);
  - 60 <= h < 120: t = max(0, h - 66) * 4096 / 54; eye = car + 0.3 m up; rotated (yaw + 1472 t, pitch + 170 (1 - t), 0),
    moved (0, 0, 3.33 -> 2.5 m), rotated (140 t, -256 t, 0); H 160 (0x800105E0: a + (b - a) t >> 12, 64-bit);
  - h >= 120: f = max(0, h - 138) * 4096 / (h0 - 138); eye = car + 0.8 m up; rotated (yaw + 0x800, pitch + 24..170, 0),
    moved (0, 0, 5.83 m -> 0.3 m * (h0 - 120)), pitched -clamp(80 (1 - f), 0, 80); H = clamp(450..921, 450, 921); window
    centre (0, -80).

## 5. Replays and the attract race (0x800A951C != 0): 0x800109FC

Replay buttons (+0x6C): 0x800 replay mode (+0x105), 0x400 onboard entry (+0x106, 0..8), 0x200 Replay Info (+0x107), mode
0: 0x100 split; mode 6: 0x10 / 0x1000 (with no hold) set the followed car's + 0x21 to -1 / 1; other modes: 1 / 2 = the car
one race position ahead / behind (0x8001097C: the car whose + 0x77C equals it). Mode 1 -> the onboard set 0x80011704
(0x80010298, then entry 0 driver's eye, 1 / 2 chase, 3 top view (onboard 12), 4..8 onboard 7..11); mode 2 in game mode 0
follows the leader of the two players; else the trackside list:

- The list = .tro header + 0x1C (relocated at the load to 0x800B4A50): { u16 count, u16 lap modulus, u32 record[count] }.
  0x800117C4(list, lap = car + 0x634, distance = car + 0x630): lap wrapped by the modulus ((lap - 1) % m + 1); the first
  record with firstLap (+2) <= lap <= lastLap (+4) and start (+8) <= distance' < end (+0xC) (end < start: + course
  length; distance' = distance + course length when firstLap < lap). None: the race view (0x800102D8).
- Progress 0x800A8D6C = (distance - start) << 12 / (end - start) (64-bit, wrapped like above). +0x100 = word 0.
- Record types (word 0 & 0xF; all 126 courses: 3484 type 0, 156 type 1, 317 type 2, 1967 type 3):
  - 0 (0x800111A8): eye on the path at + 0x18, chunk by 0x80028394(chunk hint + 6), looking at the car + 0.8 m
    (0x80010E20: yaw / pitch from the direction, no roll; square roots 0x80081288), H = H0 + (H1 - H0) * progress >> 12
    (u16 + 0x10 / + 0x12);
  - 1 (0x800112C4): fixed eye s32 + 0x14.. (z negated), angles (-yaw + 0x22, -pitch + 0x20, roll + 0x24), H = the word
    at + 0x10;
  - 2 (0x80011378): onboard view + 0x10 of the table 0x8002F378 at H + 0x12;
  - 3 (0x80011568): eye on the path at + 0x1C looking at the car; H = (|d| (0x80011144: d >> 10 per axis) * 13824
    >> (lod scale - 16)) / lod size * zoom (+0x14) >> 12, clamped to [H1, H0] - the car's LOD 0 scale / size (+0x4C /
    +0x4E of the .cdo LOD 0 block through car + 0x878 -> + 0x870): the zoom follows the car's size.
- Paths (0x80011890, z negated afterwards): u16 kind, u16 count; kind 0 (0x800118F8): 16-byte points { s32 length of the
  stretch ending here, s32 x, y, z }, linear between the two points around total * progress; kind 1 (0x80011A58): 56-byte
  cubic segments { s32 length, s32 point[3], u32 scale, s32 (t^3, t^2, t) coefficients of x, y, z }: point + scale * (a t^3
  + b t^2 + c t) with t in 24-bit fixed point (64-bit products).
- Onboard table 0x8002F378 (13 x 24 bytes {s32 offset[3], s16 before[3], s16 after[3]}; 0x800113C0): eye = the car's frame
  rotated by `before`, moved by `offset`, rotated by `after`; 0 driver's eye (0.8 m), 1..6 fixed high views, 7 driver
  looking back, 8..11 beside the car (x = -/+ (car + 0x87C + 0.3 m) written into the table at every use; car + 0x87C =
  max(|bbox[0]|, bbox[4]) of the LOD 0 block << (scale - 16) << 4, 0x80017E74), 12 top view (15 m, pitched 90 degrees,
  car angles ignored). Views 0 and 7 hide the car and use the in-car sound.

## 6. EXE library

0x80081374 angles -> matrix (m = Ry(yaw) Rx(-pitch) Rz(roll) with Ry = [[c, 0, s], [0, 1, 0], [-s, 0, c]], Rx(a) = [[1, 0,
0], [0, c, -s], [0, s, c]], Rz = [[c, -s, 0], [s, c, 0], [0, 0, 1]]; the first two columns through two GTE MVMVAs whose
packed control words carry sign bits in the middle column, which only meet VY = 0; 4096 units per turn), 0x800811B0 matrix -> angles (pitch = asin m12 by the table
0x800A2AC4 = round(asin(i / 4096) * 4096 / 2 pi), generated natively; roll / yaw by the 16-bit arc tangent 0x80082E14; the
degenerate case is tested on |m21| >= 4096 through the cosine table 0x800A0A88, which has no zero below 4096), 0x8007B994
matrix product (MVMVA sf 1, IR saturated), 0x8008220C apply with a 32-bit vector (12-bit slices), 0x80081164 = 0x80082C58
(scale to 13 bits) + 0x80082CE0 (SQR, square root, GPF with 0x1000000 / length).

## 7. Verification

- gt2verify (`VerifyCamera`, on work\re\race_demo and work\re\license_race, 0 mismatches): AsinTab 4096, RotAngles 4000,
  RotToAng 4000, VecLength 4000, LookAlong 4000, CamInit 600, CamProj 2000, CamTarget 1000, CamRace 4000, CamPlayer 4000,
  CamIntro 4000, CamOnbrd 3000, CamOnSet 2000, CamFind 3000, CamPath 1440 / 2960 (every path of the course), CamReplay 5000,
  CamUpdate 6000 cases (whole RAM compared); CamConst (tables from GT2.OVL), CamList (the course's list from the .tro against
  the dump), CamScan (all 126 courses' lists read without error).
- Captured frames (`gt2verify --camera-capture <disc> <from> <to> <every> [script]`): the original from boot, at every flip
  the camera object against (a) our camera recomputed from the same RAM and (b) the camera the scene extractor derives from
  the frame's GTE transforms. Attract race fields 2200..3700 (trackside types 0 / 1 / 3): 695 frames, ours bit-exact in 695;
  licence B-1 fields 4300..4700 (intro phases, driver): 128 / 128; licence fields 5200..6400 (driver, chase 1, chase 2,
  look back in both): 601 / 601. The flip carries the transforms of the previous frame's camera: extracted vs that camera
  worst 0.10 m (the extractor's precision), 0.025 degrees, H equal in every frame.
- Screenshots (work\play\camera): orig_* (gt2play --original) and ours_* (gt2game --license B-1 --drive --shot) of the intro
  swing / zoom, driver, chase 1, chase 2, chase 2 look back, driver look back at the same race frame: the camera relative to
  the car is identical (camera eye - car position equal to the unit, e.g. chase 1 (353894, 117964, 0)); the scenes differed
  because gt2game's B-1 car started 1.9 m further along the course: fixed 2026-09-19 (the grid mark is the car's nose,
  0x80017E74; docs/formats/replay.md section 5) - B-1 now equals the original frame by frame (529 frames).
