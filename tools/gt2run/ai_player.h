#pragma once
// DEV CAPTURE AID (tools only, never the product): lets the ORIGINAL's AI drive the player's car, so that a scripted
// run of the original in our interpreter can finish whole event / championship races (the oracle captures of the
// race-end, prize and standings screens, docs/formats/race_screens.md). It changes guest state, so it is an RE aid
// only. The game target runs the native simulation and does not use this guest-state hook.
//
// The switch (US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a, GT2.OVL member 0 at 0x80010000):
// the race overlay's entry setup 0x80012CD4 gives an entry of kind 3 (player 1) the pad slot 2 (car + 0x18) and the
// control-class argument 0; any other kind gets pad slot 0 and class 2 (AI). The class is the 8th argument of the car
// start 0x80033384 (stack word sp + 0x1C at the call 0x800130DC), which stores it in body + 0x45D (0x80030D64) and sets
// the car up from it (AI class tuning, automatic transmission). Replacing that argument by 2 for the car whose pad slot
// is 2 keeps the player's entry (results record 0x801D5E88, HUD, race-end sequence for pad slot 2) but lets the AI
// driver 0x80037834 drive it - the state gt2game's --ai-player builds natively (race_sim.h RaceSlot::controlClass).
#include <cstdint>

#include "machine/machine.h"

namespace gt2 {

// Call from Machine::cpu.onCall(from, to). Returns true when it switched a car (once per race load).
inline bool AiPlayerSwitch(Machine& m, uint32_t from, uint32_t to) {
    if (to != 0x80033384u || from != 0x800130DCu) return false;
    uint32_t insn = 0;
    if (!m.bus.FastRead32(from, insn) || insn != 0x0C00CCE1u) return false; // jal 0x80033384 of the race overlay only
    const uint32_t body = m.cpu.gpr[4], car = body - 0x2C, sp = m.cpu.gpr[29];
    uint32_t padSlot = 0;
    if (!m.bus.FastRead32(car + 0x18, padSlot) || (padSlot & 0xFFFF) != 2) return false; // player 1 only
    m.bus.Write(sp + 0x1C, 2, 4);
    return true;
}

} // namespace gt2
