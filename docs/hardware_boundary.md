# Hardware / kernel boundary of SCUS_944.88 (US Simulation v1.2)

Living document. Source of every row: `gt2run boot-trace` (our interpreter, no BIOS, no hardware: BIOS calls
answered with v0 = 0, I/O reads return 0) unless stated otherwise. Names of BIOS functions are from psx-spx.

## Survey 2026-09-18: boot from pc0 0x8005D600

Runs 1.31 M instructions, then stops at the first `syscall` (0x8008C94C, a0 = 2 = ExitCriticalSection, caller
0x8008BED0) because no kernel services exceptions yet. Everything up to there is the PsyQ libetc/libapi start-up
(`ResetCallback`), in the SDK zone 0x80086100..0x8008E040 identified by the scout.

| Order | Kind | What | Args / value | Caller pc |
|---|---|---|---|---|
| 1 | I/O W | 0x1F801074 I_MASK | 0 | 0x8008BE44 |
| 2 | I/O W | 0x1F801070 I_STAT | 0 | 0x8008BE50 |
| 3 | I/O W | 0x1F8010F0 DPCR | 0x33333333 | 0x8008BE60 |
| 4 | BIOS | B0:19 HookEntryInt | a0 = 0x800A7BB4 (handler jmp_buf) | ra 0x8008BE9C |
| 5 | I/O W | 0x1F801114 timer1 mode | 0x100 | 0x8008C564 |
| 6 | BIOS | B0:5B ChangeClearPAD | a0 = 0 | ra 0x8008C178 |
| 7 | BIOS | C0:0A ChangeClearRCnt | a0 = 3, a1 = 0 | ra 0x8008C184 |
| 8 | I/O W | 0x1F8010F4 DICR | 0 | 0x8008BCA4 |
| 9 | BIOS | A0:72 CdRemove | - | ra 0x8008BEC8 |
| 10 | syscall | ExitCriticalSection (a0 = 2) | - | 0x8008C94C |

## Consequences

- The game installs its own interrupt entry through HookEntryInt, i.e. it relies on the kernel's exception
  handler calling that hook. Whatever kernel we use (real BIOS image or HLE) must implement: exception
  entry at 0x80000080, syscalls 1/2 (Enter/ExitCriticalSection), the HookEntryInt dispatch, ChangeClear*.
- Decision ADR 0005 (real BIOS vs HLE kernel) is now the next blocker for going further than this point.

## Verified guest functions

See `db/sim_us12_symbols.yaml` (gzip_inflate 0x80082FAC passes the inflate oracle).

## Controller port (SIO0): how the game reads the pad (2026-09-18)

Source: `gt2run play` with `GT2_SIOLOG=<field>[,<count>]` (every SIO0 / I_STAT / timer-2 access with pc, plus one
`pad xchg` summary per transfer; logs in `work\play\pad_trace\sio600.txt`, `sio5300.txt`), the resident-EXE
disassembly (objdump of `work\re\race_load\ram.bin`, addresses = Sim US v1.2) and the Ghidra pseudo-C of the same
dump. The game does NOT use the BIOS pad (InitPAD/StartPAD are never called for input); it carries the PsyQ
libpad driver with multitap support and polls SIO0 itself from the VBlank interrupt chain.

Driver (all in the resident EXE, listed in `db\sim_us12_symbols.yaml`):

| Address | Role |
|---|---|
| 0x800871F8 | `PadStartCom`: EnterCriticalSection, SysDeqIntRP/SysEnqIntRP(2, 0x800A76FC), ChangeClearPAD(0)... |
| 0x800A76FC | interrupt-chain element {next = 0, handler = 0x800876D0, verifier = 0x80087668} |
| 0x80087668 | verifier: I_MASK.0 && I_STAT.0 (VBlank pending) -> 1 |
| 0x800876D0 | handler: for port 0..1: 0x80087860 (select) then the step table 0x800A7724 via 0x80087B94 |
| 0x80087860 | JOY_CTRL = 0x40 (reset), 0, JOY_MODE = 0xD, JOY_BAUD = 0x88, timer-2 delay 0x91 (0x50 for a multitap), JOY_CTRL = 0x1003 (port 1) / 0x3003 (port 2): TX enable + /JOYn + ACK IRQ enable |
| 0x80087C84 | first byte (0x01): wait JOY_STAT.0, delay, write JOY_DATA; later bytes: wait JOY_STAT.1, read RX, wait I_STAT.7, write TX |
| 0x80087EA8 | middle bytes: wait JOY_STAT.1, read RX, set BAUD, wait I_STAT.7 (timeout 400 ticks) then write the next TX |
| 0x80088124 | after each step: I_STAT = ~0x80, wait JOY_STAT.7 == 0 (/ACK released, timeout 0x3C), JOY_CTRL |= 0x10 |
| 0x80088B34 / 0x80088B54 | timeouts on timer 2 (0x1F801120, counted in sysclk/8 ticks, wrap by target 0x1F801128) |
| 0x80088BF4, 0x80088C3C, 0x80088D14, 0x80088DC0, 0x80088EDC | step table 0x800A7724: send 0x01, send 0x42 (or the config command in +0x37), read id (data length = (id & 0xF) * 2), read 0x5A, read the data bytes |
| 0x80089630 | end of a port: JOY_CTRL = 0 (deselect), next port |
| 0x80087148 | `PadInitDirect(buf1 = 0x801F0C98, buf2 = 0x801F0CBA)`; control blocks 0x801C95C8 + port * 0xF0 (rx buffer at +0x3C = 0x801C97A8) |
| 0x8007F978 | game side, once per frame: 0x8007FC30(pad) for both ports |
| 0x8007FC30 | reads buf[0] (0 = ok), type = buf[1] >> 4 (4 digital, 7 analog: 4 stick bytes at +4, 1 mouse, 2/5 neGcon-like, 6, 0xE), buttons = ~u16 at +2, then the per-screen handler table |

Byte exchange seen in our runtime at the title menu (field 600, Down held) and in the license test (field 5300,
Cross held): `tx 01 42 00 00 00 -> rx FF 41 5A BF FF` and `-> rx FF 41 5A FF BF` (bit set = released; Down = bit 6
of byte 4, Cross = bit 6 of byte 5). Port 2 gets `01 -> FF` and no /ACK; the driver times out after 0x1AE
timer-2 ticks (430 * 8 sysclk = ~100 us) and marks the port empty. What the driver needs from the hardware model:

- JOY_STAT.0/2 (TX ready) set, JOY_STAT.1 (RX FIFO not empty) after every byte, JOY_STAT.7 (/ACK level) low
  again when polled, JOY_STAT.9 mirroring the IRQ flag, cleared by JOY_CTRL.4.
- I_STAT.7 set by the device /ACK after every byte except the last of the transfer (~600 instructions =
  ~35 us after the TX write in our model; the timeout is ~100 us); the driver polls I_STAT directly with
  interrupts disabled (it runs inside the VBlank interrupt chain) and clears the bit itself (I_STAT = 0xFFFFFF7F).
- timer 2 counting (sysclk / 8 when mode bit 9 is set; the driver divides by 8 itself otherwise).
- SysEnqIntRP priority 2 elements called from the VBlank interrupt with the verifier/handler convention.

Our runtime (`src\machine\machine.cpp`, `SioExchange`) models a digital pad (id 0x41) in port 1 and nothing in
port 2; the exchange above is what it produces. Verified end to end with `gt2run play` and `gt2play --script`
(2026-09-18): the title menu cursor follows Down presses, and the scripted sequence
`1400:cross,2000:right,2060:cross,2400:cross,2700:cross,3500:cross,4300:cross:1300` (title -> Start Game -> map
-> LICENSE -> B-License -> test 1 -> Start -> hold accelerate) drives the B-1 license test (Toyota Vitz, course
TC_lisence; 56 mph in gear 3 at field 5300, screenshots `work\play\pad_gt2play_race_original.png`,
`work\play\pad_gt2play_race_native.png`, `work\play\pad_race\field_005399.png`). Timeline of the boot in our
runtime: black until ~field 900 (correction 2026-09-19: the Simulation disc has NO movie - no STREAM.DAT, main goes to the title, Sim 0x8005D700; the black part is loading; movies exist only on the Arcade disc and are decoded since 2026-09-19, src/machine/mdec.*, docs/formats/str_video.md), title menu from ~field 1000,
attract replay from ~field 1700 when nothing is pressed. Earlier "input has no effect" reports were made
before field 1000 (black screen) or with the gt2play window not in the foreground (`ReadPad` only reads keys
when `GetForegroundWindow() == hwnd`).

Open: analog / DualShock (id 0x73, config commands 0x43/0x44/0x4D that the driver can send through +0x37) are
not modelled - the game accepts the digital id; multitap (first byte 0x80) is not modelled; memory-card
traffic goes through the HLE kernel (`_card_*`, BIOS file API), never through SIO0.

### The controller in the native game (2026-09-19)

gt2game does not emulate SIO0: the boundary is the reader's view of a controller (`src\platform\input\ps1_pad.h`
`Ps1PadFrame` = what 0x8007FC30 takes from the receive buffer 0x801F0C98 + port * 0x22: type = id >> 4, ~buttons, the
four analogue bytes). Above it the original's code is ported and compared with the dumps (`tools\gt2verify\verify_pad.cpp`,
`docs\formats\pad_input.md`): the race's handlers 0x80083818 / 0x800858CC / 0x800832C0 with the tracker 0x800838B4, the
race's logical pad 0x80014BB4 through the career's key tables, the actuator bytes 0x8008371C (DualShock: small motor
on / off, large motor 0..255) fed by the vibration tail of 0x800133F0. Below it `src\platform\input\input_system.*`
turns XInput pads (type 7, both motors via XInputSetState), DirectInput pads / wheels (type 7 / neGcon type 2, no
motors) and the scripted `--fake-pad` into that frame for port 1. The libpad protocol itself (PadSetAct / PadInfoAct /
PadGetState / align table, 0x800873D8..0x800874F8) is not needed: an XInput pad is treated as a stable DualShock with
two actuators (mode 2 of 0x8007F9CC).
