# Controller input, key configuration, analog calibration and vibration

Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; race overlay = GT2.OVL
member 0, title = member 1). Evidence: our objdump / Ghidra pseudo-C of `work\re\license_race\ram.bin` (B-1 race),
`work\re\race_demo\ram.bin` and `work\re\title\ram.bin`; every routine named "verified" has a `gt2verify` row
(`tools\gt2verify\verify_pad.cpp`, 0 mismatches on those dumps, 2026-09-19). Port: `src\platform\input\ps1_pad.*`
(reader, handlers, race logical pad, actuators), `src\game\shell\key_config.*`, `src\game\shell\analog_config.*`;
PC devices: `src\platform\input\input_system.*`. See also `docs\hardware_boundary.md` (the SIO driver).

## 1. The reader (per field) and the pad object

`0x8007F978` (VBlank / frame) calls `0x8007F9CC(0x801F0C70, port)` and `0x8007FC30(pad object)` for both ports. The pad
manager 0x801F0C70: +8 the poll routine, +0x10 / +0x14 the pad objects of ports 1 / 2 (race: 0x800A9528 / 0x800A95D8,
title: 0x801FFD64 / 0x801FFDC8), +0x18 + port * 8 the actuator block {align pending, actuator mode, act[0], act[1]}.

`0x8007FC30`: receive buffer 0x801F0C98 + port * 0x22 (status, id, ~buttons u16, analogue bytes); type = id >> 4 (0 when
status != 0); analogue words: types 2 / 5 / 7 = bytes 4..7 as u16 (DualShock: RX, RY, LX, LY; neGcon: twist, I, II, L);
then the handler of the type from the object's table (+4; race: 0x800A6F5C) `handler(obj, buttons, words, sameType)` and
its post routine, then the vibration timer: `+0x60 -= 1`; at -1 the words +0x5A..+0x60 are cleared.

Race handler table 0x800A6F5C (12-byte rows {type, handler, post}): 2 -> 0x800832C0 / -, 4 -> 0x80083818 / 0x8008371C,
5 and 7 -> 0x800858CC / 0x8008371C; 1, 3, 6 and unknown types: none (tracker fed 0, axis mask 0). The Jogcon (14) has no
row in the race.

| Routine | What | Status |
|---|---|---|
| 0x80083A4C | raw buttons -> generic bits through 16 pairs (0x800A6F3C): up 0, down 1, left 2, right 3, L1 4, L2 5, L3 6, triangle 8, cross 9, square 10, circle 11, R1 12, R2 13, R3 14, start 16, select 17 | verified (PadRemap) |
| 0x800838B4 | button tracker (object + 0x0C): current, held / pressed / released accumulated since the last read, repeat (counters per bit: reload 0x1E, first 0x25 from 0x800A6FBC) | verified (PadTrack) |
| 0x80083818 | digital: tracker <- remapped buttons; axis mask +0x48 = 0 | verified (PadDigi) |
| 0x800858CC | analog: tracker; +0x48 \|= 0xF; axes +0x4A.. = 0x80085890(word): x < 91 -> x * 128 / 91, 91..164 -> 128 (dead zone), else (x - 165) * 128 / 91 + 128 | verified (PadAnlg, PadAxis) |
| 0x800832C0 | neGcon: tracker <- remapped \| 0x80083A88 (I / II / L >= 0x80 as cross / square / L1, rows 0x800A6ECC); +0x48 \|= 0xF; twist 0x800831BC and pedals 0x80083250 through the port's calibration 0x800A6EEC + port * 20 | verified (PadNegc, NegBtn, NegTwist, NegPedal) |
| 0x8008371C | actuators: small = \|+0x5E >> 4\|, large = \|+0x5C >> 4\| (each clamped 255) -> PadSetAct(port << 4, act, 2) (0x800874B8); mode (0x801F0C89 + port * 8, from 0x8007F9CC: PadGetState 2 -> 1, 6 -> PadInfoAct(port, -1, 0) = actuator count, else 0) 1: act = {0x40, small ? small : large >= 97}; the align table 0x800A6F34 {0, 1} is sent once the port is stable | verified (PadAct) |

Neither the handlers nor the race read the calibration of a DualShock: its stick filter has a fixed dead zone.

## 2. The race's logical pad (0x80014BB4, per race frame)

Called from BeginFrame 0x80015B64 for both pad objects. 0x80083998 copies the tracker's accumulated words to +0x68..
(held, pressed, released, repeat) and clears them; +0x48..+0x59 (axis mask, axes) to +0x78..+0x89. Key table = pad
block (object + 0x64 -> career + 0x0A / + 0x5C) + 11 * t with t = 1 for types 5 / 7, 2 for type 2, 3 for type 14, else 0.
For function f = 0..10 (logical bit 1 << f: 0 left, 1 right, 2 accelerate, 3 brake, 4 handbrake, 5 reverse, 6 shift up,
7 shift down, 8 change views, 9 rear view, 10 steering curve) and entry e:

- e < 0x80: generic bit e of the snapshot -> logical +0x8C / +0x90 / +0x94 / +0x98 (held / pressed / released / repeat);
- e >= 0x80, axis a = e & 0x1F valid in the snapshot mask: +0x9C |= 1 << f and +0x9E + 2f = value: 0x80..0x9F the axis
  as is, 0xA0.. 127 - v, 0xC0.. v - 128, 0xE0.. \|v - 128\| style (v < 128 ? 127 - v : v - 128), the last three clamped
  to 0..127 and doubled ((v << 1) \| (v >> 7)). An axis entry of a function above 8 writes past the 0xB0-byte object.

0x80013C90 records {flags (+0x9C bits 0 / 2 / 3 -> 1 / 2 / 4, logical 0x400 -> 8), logical buttons & 0xFF, +0x9E,
+0xA2 >> 4, +0xA4 >> 4} and turns the frame into the physics' pad record (analogue steer (0x80 - axis) * 0x20, pedals
through 0x8002F4D4[v >> 4]); `docs\formats\replay.md` section 3. Verified: PadLogic (1600 cases on license_race, both
pad objects, random tables).

Default tables (EXE 0x80091570 digital, 0x8009157C analog, 0x80091588 neGcon, 0x80091594 Jogcon; the new game copies them
to both pad blocks):

| Function | digital | analog (DualShock) | neGcon | Jogcon |
|---|---|---|---|---|
| left / right | D-pad left / right | 0x82 0x82: left stick X | 0x80: twist | 0x80 |
| accelerate / brake | cross / square | cross / square | 0x81 I / 0x82 II | cross / square |
| handbrake / reverse | circle / triangle | circle / triangle | circle(A) / triangle(B) | circle / triangle |
| shift up / down | R2 / L2 | R2 / L2 | up / down | R2 / L2 |
| change views / rear view | R1 / L1 | R1 / L1 | R1 / L1 | R1 / L1 |
| steering curve | up | up | up | down |

Camera: logical 0x100 pressed = next camera position, 0x200 held = look back (`docs\formats\camera.md`).

## 3. Vibration (the tail of 0x800133F0, per car and race frame)

For a car with pad slot (car + 0x18) 2 or 3 and the demo / replay flag 0x800A951C clear, on the pad object of car + 0x0C:
`+0x60 = 2`; when the pad block's vibration byte (+0x2C, option 14; 0 = on) is 0 and the car has not finished (car +
0x24 == 0): `+0x5A = body + 0x760` (only cleared by SetupCar 0x80033028), `+0x5C = (body + 0x762) << 4` (the load / skid
/ impact level that ground.cpp computes for the sounds), `+0x5E = (body + 0x763) << 4` (1 while a wall impact is
recent); else the three words are 0. The next polls send them (small motor on while an impact is recent, large motor =
the level 0..255) and they stop two polls after the physics stops feeding them (pause, end of the race, replays).
Verified: VibFeed (the whole 0x800133F0 run, the pad object compared). 0x800834FC (not in the race's table) drives a
single-motor device from +0x5A.

## 4. gt2game (ours)

- `src\platform\input\input_system.*`: XInput slots (Xbox layout onto the PS1 one: A cross, B circle, X square, Y
  triangle, LB / RB L1 / R1, LT / RT L2 / R2 above the XInput threshold 30, Back select, Start, stick clicks L3 / R3,
  D-pad; sticks -> bytes with 0x80 centre, Y down), DirectInput game controllers (Sony VID 054C layout for DualShock 4 /
  DualSense, a generic layout, wheels as a neGcon with pedals read inverted when they rest high), the `--fake-pad`
  script; hot-plug (XInput slots every 120 fields, DirectInput on WM_DEVICECHANGE); port 1 = the most recently used
  device (a fake pad always). The analogue trigger travel of XInput / Sony pads is offered as extra axes 4 (R2) and 5
  (L2), raw 0..255 like the neGcon pedals (ours: no PS1 controller has them).
- `tools\gt2game\game_window.*`: the pad's buttons are also the keys of the menus' keyboard convention (D-pad = arrows,
  cross = Enter, circle = Space, triangle = Backspace, square = Delete, start = S, L1 = Q, R1 = W), so every screen
  (title, GT-mode menus, race panels, arcade menus) gets the pad with its own button semantics; the title and its
  options also take every pad button as generic bits (0x80083A4C).
- `tools\gt2game\race_view.cpp`: the pad object is polled every field with the ported reader, its logical pad built
  every race frame with 0x80014BB4 through the career's key tables, merged with the keyboard (buttons or-ed; a function
  the keyboard drives in that frame drops the pad's axis) into the logical pad of 0x80013C90 (the recorded replay frame):
  steering / pedals reach the verified physics only through the original's frame conversion. Start pauses; R1 / L1
  (logical 0x100 / 0x200) are the camera buttons; the replay controls read the generic pad (0x800 / 0x400 / 0x200).
  Motors: the ported vibration tail after every step, the ported actuator bytes every poll -> XInput (large motor =
  act[1] * 257, small = act[0] bit 0 full), scaled by `rumble_scale` of `saves\settings.txt`.
- Triggers profile (ours, `trigger_pedals=1` default): for a pad with trigger travel the analog table's accelerate /
  brake become 0x84 / 0x85 (R2 / L2 travel) and a function that was on R2 / L2 takes the button accelerate / brake had
  (default: shift up cross, shift down square). `trigger_pedals=0` = the original table.
- `--fake-pad "field:control=value[:hold],..."` (or a file): controls `type` (digital, analog, negcon or a number), `lx ly
  rx ry` / `a0..a3` (bytes), `l2p r2p` (trigger travel), buttons (`cross circle square triangle l1 r1 l2 r2 l3 r3 start
  select up down left right`, 1 / 0); the latest item of a control wins, a hold returns it to rest. Motor changes are
  printed (`fake-pad fN: motors small S large L`); runs with a fake pad are deterministic like scripted runs.

## 5. KEY CONFIGURATION (title options page 2)

Page object 0x800B12D0 {s16 row of port 1, row of port 2, blink 0..0x2C, input delay}; list objects 0x800B13D8 / 0x800B13F0
(0x14 bytes: s32 type 0 digital / 1 analog / 2 neGcon / 3 none, u8 steering mode, u8 pedal preset, u8 saved accelerate,
u8 saved brake, the 11-byte table). Pad block + 0x2D + 4 * table keeps the list's four state bytes per table.

| Routine | What | Status |
|---|---|---|
| 0x8001A7E0 / 0x8001A834 / 0x8001A84C | init / enter (both rows on EXIT, 4 fields without input) / leave | disassembled |
| 0x80019388 / 0x80019498 | load / store a list by the port's controller type (4 -> 0, 5 / 7 -> 1, 2 -> 2) | verified (KeyLoad, KeyStore) |
| 0x80019598 / 0x800196E8 | which function uses a button / assign (the previous user takes the old button; neGcon cross / square / L1 = I / II / L 0x81..0x83) | verified (KeyFind, KeyAssign) |
| 0x80019934 | steering row: left / right cycle left stick (0x82) / right stick (0x80) / D-pad (analog), twist / D-pad (neGcon) | verified (KeySteer) |
| 0x80019B24 | accelerate / brake rows of an analog pad: left / right cycle the presets 0x8004C120 (1..8: C3/A3, A3/C3, C2/A2, A2/C2, C1/A1, A1/C1, C0/A0, A0/C0 = left / right stick up / down), 0 restores the saved buttons | verified (KeyPreset) |
| 0x80019C4C | Default: the executable's table of the type | verified (KeyDefault) |
| 0x80019D5C | one row: rows map 0x8004C0DC {0, 2, 3, 5, 4, 6, 7, 9, 8, -1 Default, -2 EXIT}; a face / shoulder button assigns, START held + D-pad assigns a direction | verified (KeyEdit) |
| 0x8001A860 | the page field (both ports, rows move with up / down, 0 = EXIT chosen) | verified (KeyPage) |
| 0x8001A624 / 0x80019F44 | draw: labels 0x8004C0F4 (small font, centred, 0x1A apart from y + 0x40), port cells at x -/+ 0x50 (icon records 0x8004C144: buttons 0..11, neGcon 12..21, stick pedals 22..29, steering 30..33; maps 0x8004C2DC / 0x8004C2FC), blinking arrows on the steering / pedal rows, "Default" / "EXIT" (tiny font) while editing, "Hold START to Assign D-pad" (0xB3060) | disassembled; our frame from the same records (no capture) |

gt2game: port 1 = the controller in use (the keyboard alone counts as a digital pad), port 2 = none. The tables are the
career's (saved on the card like the original's); the race reads them (race_view.cpp) and `saves\settings.txt` mirrors
them (`pad1=` / `pad2=` hex) for races without a career.

## 6. 1P / 2P ANALOG SETTINGS (title options pages 3 / 4)

A neGcon-type controller (type 2) in the page's port is required (0x8001C690 != 1: sound 0, no editing). Page objects
0x800B12D8 / 0x800B1358 (0x80 bytes: s16 item, port, state 0 right end / 1 left end / 2 menu / 3 idle; steering model
{centre, lock, margin, max, min} at +8, accelerate {lock, margin} +0x1C, brake +0x24, three bands +0x2C (templates
0x8004C350)); globals 0x800B1408..0x800B14AF (help line, items {kind, sub, text} 0x800B1410, count, which functions
are on axes, live bytes 0x800B14A0.., the two range ends 0x800B14A8 / AC).

| Routine | What | Status |
|---|---|---|
| 0x8001C2A0 | items from the neGcon table (pad block + 0x16): steering on an axis -> Center / Lock / Margin, accelerate / brake on an axis -> Lock / Margin, then Exit | verified (AnaItems) |
| 0x8001C48C / 0x8001C610 / 0x8001C648 | models from the calibration (0x8001C170 steering if on axis 0: centre = (dead lo + dead hi) / 2, margin, lock, range 0..255; 0x8001C1C8 pedals), bands / page slides | verified (AnaReset) |
| 0x8001C690 | enter: state 0 (steering on an axis) or 2 | verified (AnaEnter) |
| 0x8001C7B8 | the field: R1 / START (either port) takes the right then the left end (0x8001ADFC range), triangle / square cancel; in the menu R1 / START take the live byte (0x800B14A0.. from the receive buffer + 4 + axis), left / right step (clamps 0x8001ACB4: centre 0x40..0xC0 inside the range, lock / margin; 0x8001B7F8: pedal lock 2..255 > margin >= 1), up / down move, Exit (cross / circle) stores (0x8001C210: twist = {c - lock, c - margin, c + margin, c + lock}; 0x8001C270 pedals {min = margin, max = lock}) | verified (AnaUpdate) |
| 0x8001CE28 (+ 0x8001B2C0, 0x8001AF6C, 0x8001BC5C, 0x8001B9D8, 0x8001AE34, 0x8001B8A0) | draw: gauges (outline 0x8007E780, zones as TILEs, live marker 0x8006B988), labels Center / Max / Margin with values, "N/A" for a function on a button, "Press R or START to Set Position", EXIT; the help lines 0x8001AB40 only when career + 0 == 0 (not the US game) | disassembled; our frame from the same code (no capture: our interpreter models no neGcon) |

gt2game: the ANALOG pages work with a DirectInput wheel (as a neGcon) or `--fake-pad "0:type=negcon,..."`; the
calibration is stored in the career's pad block (+0x3E) and used by the ported neGcon handler in the race.

## 7. Open

- No capture of the KEY CONFIGURATION / ANALOG pages (the interpreter's pad is digital; its KEY CONFIGURATION page could
  be captured with it): the frames follow the disassembly, not a pixel comparison.
- The DualShock in digital mode (id 0x41) and analog mode switching (config commands 0x43 / 0x44 / 0x4D) are not
  modelled: an XInput pad is always an analog controller (type 7).
- DirectInput wheel layouts (axes of pedals, buttons) are guesses of the common layout; not tested with hardware.
- Player 2 (port 2) is not fed from a second PC device.
