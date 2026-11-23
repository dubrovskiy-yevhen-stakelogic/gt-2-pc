# Multiplayer development

Local development feature; not part of the published 0.9.2 release.

The restored implementation comes from the September 2026 server prototype,

updated for the current game. Do not connect it to an unmodified 0.6.0 server.

## Enter a room

Open **Arcade Mode > Multiplayer**, the extra tile beside **2 player Battle**.

The original local two-player mode, Bonus Items and Load Guest Garage remain.

Use arrows / D-pad to select a row, left/right to change it, Enter / Cross to

confirm, and Esc / Start to leave. On Quest, use the normal mapped menu controls.

- **Find a room**: loads the shared GT2 server's rooms. Select a waiting room to join. Refresh reloads the list; full and running rooms are labelled.

- **Create a room**: choose track, 2–6 player slots, laps, transmission, car rules, bot population and bot difficulty. **Same car** fixes a car for everyone; **Class** lets each participant select from Arcade S/A/B/C or Rally.

- The waiting room opens the original Arcade car carousel, including the model, specifications and colour chips. Changing a car cancels readiness. Players mark Ready after choosing. The host explicitly selects **Start race**; every connected player must be ready, but vacant places do not block the start. The server validates readiness, car rules and host permissions.

- Each player receives a private slot capability. Connection order cannot exchange selected cars. With **Bots: fill free places**, unused grid places are AI cars under the same car rule. **Bots: off** starts with only the connected humans, with at least two real players. **Easy / Normal / Hard** changes AI target speeds; Normal keeps the standard AI tuning and human physics is unchanged. Public room listings never contain these capabilities.

- Rally uses the four standard Arcade rally cars and their actual Arcade parameter/tyre records. All nine rally directions are listed, including Pikes Peak uphill/downhill. Point-to-point races have one finish, not a circuit lap target.

- **Back**, B/Circle, Triangle/Backspace or Escape return to the previous screen. Start also leaves the multiplayer menu. Cancel works while connecting or creating a room.

- No IP address entry, port forwarding or invitation-file copying is required for players. The client pins the directory's public key; per-room connection credentials are delivered over its encrypted connection and held in memory. Reconnect identity is saved automatically.

- F8 requests state recovery in a network race. Offline F8 texture comparison is unchanged. Network races do not write career results, rewards or replays.

The public directory is `62.238.36.19:29610/UDP`; game rooms use UDP 29620–29623.

It admits at most four rooms. A room allows up to six players; it can

start with at least two connected, ready human players. Remaining race-grid places are AI only when bots are enabled. **Remove player** is available to the host both in the lobby and during the race. Kicked players return to multiplayer and cannot reuse their old race admission. A guest cannot start, remove players or change

settings. If the host leaves, the oldest remaining occupied slot takes ownership.

Use **Return to room** in the race menu, or confirm after results, to return to the same room. **Resume race** only closes the menu. Returning early leaves other players racing. Membership, room name and

password remain; readiness is cleared once the remaining players return. The host

chooses the next **Track** with left/right; every player can open **Garage** again

and must mark Ready anew. Track changes invalidate old Ready messages and reset

car selection to the allowed default (rally tracks require rally cars). Every race

gets a fresh session and private admission credentials. Leaving the last occupied

place deletes the room. There is no fixed lifetime limit for an occupied room.

These changes use directory protocol **5**. Deploy matching clients and `gt2lobby`

together; older clients cannot use this directory. The local implementation

and localhost checks do not by themselves update the public server or devices.

The low-level invitation tools remain developer diagnostics, not the player UI.

## Race controls and completion

Press **L3** (keyboard **G**) once to open the emote wheel. Select with the left stick, then press L3/G again to send. The empty sector cancels. Recentering the stick keeps the selected sector; steering and pedals remain active. Confirmed emotes also appear to their sender. Texture packs are selected in settings, without an R3 shortcut.

The first human finisher starts a 30-second deadline. The race can end earlier when every human has finished; AI finishers neither start the deadline nor delay completion. Network races use Arcade collision rules without GT-mode suspension damage.

## Disc compatibility

The tested network datasets are **US Arcade v1.1** and **US Simulation v1.2**.

Every participant and server in one room must use the same dataset and physics

revision. Admission compares executable, race overlay, track physics, car records

and race settings. Mixing Arcade and Simulation in the same room is rejected.

GT4 visual replacements and HD textures do not provide a separate network mode.

Mods that change race setup and unsupported profiles remain rejected.

The old prototype's Simulation restriction came from the project's reference

executable and its verified data/address baseline. The race engine is shared;

career progression is not required for multiplayer. Arcade is the primary menu

entry for the restored version. European and combined-disc online acceptance

have not been established; they must not silently bypass the profile checks.

## Dedicated server

Build target `gt2server` with the normal CMake toolchain. For a trusted local test:

```text

gt2server <arcade-disc-image> --bind 127.0.0.1 --port 29600 --players 2 --cars 6 --trusted-lan

```

For an encrypted server, replace `--trusted-lan` with:

```text

--host-key <private.gt2host> --invite-out <room.gt2invite> --public-address <host-address>

```

The server can load an image or extracted disc directory. It opens no game

window, audio device or career save. `--seconds N` bounds a diagnostic run.

The room-directory service uses the `gt2lobby` target and `scripts/server/gt2lobby.service`.

The original single-room `gt2server` and invitation CLI remain available for diagnostics.

The directory needs the Arcade image and a private host key readable only by the service

account; never package either with the client. `lobby_endpoint.h` contains only the

public identity used to authenticate the directory. No shared secret is embedded.

When changing the directory key, update the client public identity together with the service.

The directory is public: knowing its address grants room browsing/creation subject to

capacity and request limits. Account-based abuse controls are not implemented.

## Synchronization and checks

The authority runs at 30 Hz. Clients predict at most four ticks, confirm the

whole interacting race and restore a canonical snapshot when required. The

server retains 900 input frames for reconnect. A private invitation supports

persisted participant identity after process restart. State hashes detect drift;

three automatic repairs are allowed before a persistently diverging view stops.

See `NETWORK_STATE.md` for the snapshot format and its bounds.

Build `gt2netcheck`, `gt2networkchecks`, `gt2transportcheck`, `gt2privatechecks`

and `gt2hydrogenchecks`. CTest includes protocol, transport and crypto checks.

Run against your own disc data:

```text

gt2netcheck <arcade-disc-image> seattle 6 --state-check

gt2netcheck <arcade-disc-image> seattle 6 --private-room-check

gt2netcheck <arcade-disc-image> seattle 6 --room-check

```

The room diagnostic covers ownership, readiness, rejection, reconnect, snapshot

repair and a stalled presentation consumer. It retains authority frames across

its own intermediate test boundaries instead of dropping them.

On 2026-11-15, the room directory and dedicated race were checked on the public Linux VPS with two Windows clients: 300 confirmed ticks, matching state hashes and one client restart. Hidden Windows clients also completed creation/join through the UI, entered the race and returned to Arcade. Native Windows and Quest builds are checked locally. Headless checks and hidden

Windows rendering do not establish physical headset latency, comfort, Internet

quality or cross-platform determinism. Browser networking is not provided; its

build omits native UDP and the Multiplayer tile.

Remaining product work includes car selection, spectators, public-service load/abuse testing and

end-to-end Quest/PC acceptance on separate devices. Keep this feature in local

work until release validation is complete.

## Local verification: six-player staging (2026-11-15)

Room wire revision 9, setup revision 4, directory revision 3. Update server and clients together.

The old two-client public VPS result does not describe this candidate.

Local encrypted directory tests cover six distinct player clients, class validation, wrong password, invalid car, readiness barrier, reverse connection order and 300 identical authority ticks on all nine Rally directions. Same-car two-player staging also passes. Native race tests reach real finishes on all nine Rally directions, with rollback and terminal state recovery. Arcade garage rendering has been inspected in a hidden Windows client. Device playtest and deployment are separate checks.

## Room lifecycle regression

Build `gt2lobby`, `gt2lobbychecks`, `gt2networkchecks`, `gt2transportcheck` and

`gt2privatechecks`. Run the matching CTest targets. For the optional real-directory

integration test, use a throwaway local key and a legally owned Arcade image:

```text

gt2lobbychecks --key <temporary-host-key>

gt2lobby <arcade-disc-image> <temporary-host-key> 127.0.0.1 120

gt2lobbychecks --directory <temporary-host-key>

```

Run the last command while the local directory is active. It exercises actual

private transports and authoritative starts with 2/6, 6/6 and 1/6 humans, sparse

slots after a kick, host-only commands, Ready gating, return acknowledgements,

track changes, stale Ready rejection, renewed admission keys and empty-room

cleanup. It does not drive a whole race to the finish or validate headset UI.


## Between races and race controls

After results, each player selects **Return to room** (A / Cross / Enter). The room shows **In room** or **In race/results** for each participant, rather than stale Ready flags. Ready resets after the race; the host can start again once everyone returns and explicitly readies. Directory wire revision 6 carries the return status.

After the first human finishes, all racing clients show the remaining 30-second deadline. Results use player nicknames and BOT labels.

L3 opens the emote wheel; L3, Cross/A or Enter confirms the selection. The empty sector closes without sending. Steering and both pedals remain active in the wheel and the multiplayer race menu.

## Track selection

Select the Track row to open a map preview. Choose the circuit, available time-of-day and direction, then Use this track. Rome-Night is a different layout from daytime Rome. Unsupported variants are fixed, not simulated. The catalogue contains 50 layouts in 26 groups. Reverse Smokey Mountain South shares the original circuit map because the disc has no separate reverse picture.

Hosts can change Car rules in the room. In Same car mode, the host garage chooses the car for everyone; guests see that fixed selection. Dirt tracks require rally cars. Returning to a road track restores the host's preceding road class, shared car and car-selection rule.
