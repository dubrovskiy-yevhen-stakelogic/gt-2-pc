# Multiplayer state recovery (development)

Scope: the stock circuit preview with matching locally loaded assets. Private
invitations authenticate and encrypt transport; a saved credential can restore the
same participant after process restart while the authoritative session survives.
This is not game-save import, host migration or an arbitrary physics-state API.

## Canonical state schema 1

`src/game/net/state_codec.cpp` lists every serialized field explicitly. The scalar
archive encodes integers in little-endian order and booleans as exactly 0 or 1.
No native struct representation, padding, `Checkpoint`, `Snapshot()` buffer,
host address or asset data is transmitted. Immutable car setup/curve parameters
and their guest-address tokens remain in the recipient's own initialized race.

The header contains `G2SS`, a 16-bit schema version, car count, zero flags, the
32-byte negotiated compatibility key, finish timeout and six 32-bit player IDs.
It is followed by the session tick/finish state, six participant result records,
dynamic car/wheel fields, contact tables, drivetrain work and requests, per-car
deltas, shell state, race order and task-complete flag. An eight-byte FNV-1a
checksum detects accidental corruption; it provides no authentication.

Six cars currently encode to 8981 bytes; two cars to 5681 bytes. Decoding rejects
payloads above 32 KiB, unsupported versions, different content/rosters/finish
policies, malformed booleans, invalid bounded indices, trailing fields and checksum
mismatches. The recipient captures a local checkpoint, decodes and validates into
that temporary state, then commits it. A rejected snapshot leaves the race intact.
The schema needs a version bump when its ordered field list changes.

The ground scratchpad is intentionally omitted: its overlapping native views are
rewritten by `PrepareContactQueries`, contact queries and `GroundPass` before use
on the next step. Restore clears it. Drivetrain work that survives between ticks,
wheel clutch requests and collision history are serialized explicitly. Continuation
tests exercise player controls, native wheel fields, AI, contacts and race completion.

## Live room transfer

Room control schema 5 retains the fixed 80-byte control envelope, including
`StateBegin` and `StateAck`. `Resume.tick = UINT32_MAX` requests a snapshot; a
normal resume automatically falls back to one when the host's 900-frame history
has expired. Existing slot/session/content/ticket checks still apply before the
host replaces any owner connection.

`StateBegin.tick` identifies the snapshot's next simulation tick. Ordered control
chunks follow, each containing a 32-byte `G2SC` header and up to 3072 payload bytes.
The header has version/zero flags, room session, snapshot tick, total length,
offset and chunk length. The assembler accepts exactly contiguous chunks for the
same transfer and at most 32 KiB. Duplicate, truncated, oversized, reordered or
wrong-session chunks are rejected. No state is exposed until assembly completes.

The network worker publishes immutable snapshot bytes to the presentation thread.
The latter applies them, discards prediction and queued historical effects, then
acknowledges the boundary. The host holds that client's authority delivery until
`StateAck`, then replays newer frames with the existing bounded drain window.
Other participants keep racing. A new disconnect before application acknowledgement
forces another snapshot, since the downloaded boundary may not have been applied.

The client retries connections every two seconds for up to 60 seconds. A snapshot
must be applied while its following input history is still retained. The car stays
reserved and missing controls expire to braking. F8 requests full-state repair;
a presentation queue overflow requests it on the next UI pump. R retains its
reverse binding. Android keyboard function keys use the NDK keycode constants;
physical keyboard operation on Quest has not been tested.

The host's presentation view uses the same canonical state codec through a local
immutable snapshot. Its authoritative worker keeps serving peers while the local
view applies that state and acknowledges its boundary. Pending host controls are
cleared and braking is applied; local commands resume after catch-up. History replay
uses at most 32 frames per worker iteration and a 128-frame consumer drain window.
If the snapshot or replay cursor outlives retained history, a fresh snapshot starts
the local repair again. Local host-view recovery itself adds no wire fields.
Host loss and a lost client process still require leaving the room. Android
focus/suspension behavior needs a headset playtest.

## Automatic divergence detection

Input/authority schema 2 uses bit 0 of the header flags to indicate an eight-byte
little-endian digest after the input payload. Unknown flags, truncated digests and
digests on client input messages are rejected. A six-car authority packet remains
96 bytes without the digest and becomes 104 bytes with it. The host supplies the
digest every 30 ticks and on the final race tick; retained history preserves it.
Room schema 5 and setup schema 2 reject earlier builds before starting a race.
Update both peers together; the canonical snapshot schema stays at version 1.

The digest is the FNV-1a checksum of the canonical snapshot payload, including
session results and the content key. The receiving view confirms the frame first,
then hashes the saved state at that exact boundary. If prediction runs ahead, this
uses its first pending checkpoint, without rewinding or mutating the live race.
Native padding, pointers and rebuilt ground scratch are excluded as in the codec.

A mismatch drops queued local commands and remaining drained frames and requests
a full snapshot. Events from the failed confirmation are not exposed. The host's
view repairs locally; a client uses full-state reconnect. A fourth mismatch after
three automatic repairs in the same race stops that view and prompts the player
to check the game builds. New local controls are rejected and missing input brakes
the car; the authoritative host continues serving other participants. The limit
applies to detected divergence, not ordinary reconnects or manual repairs.
This detects accidental drift; it is neither cryptographic integrity nor anti-cheat.

## Verification on 2026-09-24

- Windows and Quest: 1417 protocol/archive/chunk checks and a 425-tick six-participant
  room scenario, including replay reconnect, full-state reconnect, recovery of a
  client that overflowed its 300-frame queue, and matching participant states.
- Windows: all six menu circuits and a two-car Seattle setup restore at tick 1500
  and continue for 240 steps with identical state/results/events. Countdown restore
  is covered too. Windows and Quest both pass completed-race restore with no repeated
  finish events, plus the existing 1800-step prediction trace.
- Windows-to-Quest Seattle snapshot and Quest-to-Windows Autumn Ring snapshot:
  byte-for-byte identical canonical state and identical subsequent simulation.
- A relay dropped every datagram in both directions for 33 seconds over Wi-Fi.
  Both host roles automatically used one state restore, retained client slot 1 and
  matched at 1500 confirmed ticks. Quest host: `f1e5f8e0b5447368` (153 datagrams
  dropped). Windows host: `c001b4e7dd9b82fb` (195 dropped). These are per-run diagnostic
  state digests, not security fingerprints.
- Two rendered Windows processes: a 33-second outage (185 dropped datagrams),
  snapshot applied at tick 1109, then racing and acceleration resumed in the same
  assigned car. Captures are `work/multiplayer-060/client-state-reconnecting.png`
  and `client-state-restored.png`; logs are `rendered-state-*.log`.
- All nine Windows CTest suites pass. Android application and diagnostics compile;
  console checks use the shell UID. No APK replacement, game launch on Quest,
  installed-app networking or headset acceptance is implied.
- Host presentation consumer paused for twelve seconds over actual Wi-Fi in both
  host roles: 480 confirmed ticks, one local host state restore, no reconnects,
  stable ownership and safe braking input for the paused host. Paired digests:
  Windows host `e4c5768e0f6fb91b`; Quest host `492289d2363d48ad`.
- Windows loopback: a host snapshot requested at tick 390 remained unapplied until
  tick 1400, beyond retained history. A second snapshot completed recovery, with
  both peers matching at tick 1500 (`152f6832918c1ead`) without reconnecting.
- Two rendered Windows processes: two host F8 repairs and one client F8 repair
  applied successfully, followed by acceleration in the assigned cars. R presses
  produced no repair requests. Captures: `view-host-restored.png` and
  `view-client-restored.png`; logs: `view-render-*.log`, all in
  `work/multiplayer-060/`. These are desktop checks, not XR focus-loss acceptance.
- Automatic divergence checks: deliberately corrupted retained lap results in both
  local views, then detected and restored once per participant over actual Wi-Fi
  in both Windows/Quest host roles. Both reached 300 confirmed ticks with ten
  comparisons, one intentional mismatch and digest `947964a092a959ef` per run.
- Both platforms now pass 1431 protocol/archive/transfer assertions, the 425-tick
  six-participant room check, and 1800 comparisons against confirmed state while
  prediction is ahead. Completed-race canonical digests also agree after restore.
- Windows repeated-corruption test: three repairs, fourth mismatch stops local
  play, rejects controls and leaves the authority running. Two rendered Windows
  processes also drove and manually repaired without false divergence reports.
  Current evidence uses `work/multiplayer-060/desync-*` logs and captures.

## Diagnostics

Use owned local disc data. Exported snapshots are runtime artifacts and belong in
`work/` (or the separate diagnostic directory on Quest), never release source/data.

```text
gt2netcheck <disc> seattle 6 --state-check
gt2netcheck <disc> seattle 6 --state-export <work-path>
gt2netcheck <disc> seattle 6 --state-import <work-path>
gt2netcheck <disc> autumn 6 --recovery-listen 0.0.0.0 29602
gt2netcheck <disc> autumn 6 --recovery-connect <host-IPv4> 29602
gt2netcheck <disc> autumn 6 --view-listen 0.0.0.0 29603
gt2netcheck <disc> autumn 6 --view-connect <host-IPv4> 29603
gt2netcheck <disc> autumn 6 --view-late-listen 0.0.0.0 29604
gt2netcheck <disc> autumn 6 --view-late-connect <host-IPv4> 29604
gt2netcheck <disc> autumn 6 --desync-listen 0.0.0.0 29605
gt2netcheck <disc> autumn 6 --desync-connect <host-IPv4> 29605
gt2netcheck <disc> autumn 6 --desync-repeat-listen 0.0.0.0 29606
gt2netcheck <disc> autumn 6 --desync-repeat-connect <host-IPv4> 29606
```

The recovery peer diagnostic runs 1500 ticks with an 85-second race deadline.
The shorter `--room-listen` / `--room-connect` modes retain their 300-tick limit.
The `--view-*` pair tests a twelve-second host consumer pause over 480 ticks;
`--view-late-*` holds the host snapshot past retained history over 1500 ticks.
Both require uninterrupted peer ownership and transport, then compare printed
digests between the two processes in the same run.
The `--desync-*` pair corrupts each view's retained result once, requires automatic
repair, then compares printed state at tick 300. `--desync-repeat-*` injects four
corruptions per view and requires bounded failure after three repairs while the
authoritative worker continues. These mutations exist only in the diagnostic.
State-file diagnostics use an explicit test-only compatibility key and compare
against a freshly simulated reference; production rooms use the negotiated content
key. File import is a verification mode, not a player-facing reconnect feature.
