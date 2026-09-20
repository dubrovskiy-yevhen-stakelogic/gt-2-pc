#pragma once
// DEV CAPTURE AID (tools only, never the product): the arcade-disc counterpart of ai_player.h - the ORIGINAL's AI drives the
// player's car so that a scripted run of the arcade disc finishes its races (oracle captures of the arcade race end, results
// and records screens; docs/research/arcade_disc.md section 17). Same switch as ai_player.h, at the arcade build's addresses:
// US Arcade v1.1 (EXE SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95, GT2.OVL member 0): the race overlay's
// entry setup 0x80012CD4 (same address as the Simulation build) calls the car start at 0x800130DC with `jal 0x80033330`
// (word 0x0C00CCCC; Simulation 0x80033384, gt2tool exe-map "shifted", 0 real differences); its 8th argument (sp + 0x1C) is
// the control class. Checked in work/re/arcade_race/ram.bin.
#include <cstdint>

#include "machine/machine.h"

namespace gt2 {

// Call from Machine::cpu.onCall(from, to). Returns true when it switched a car (once per player car and race load). Both player
// entries of a 2 player Battle (kinds 3 / 4 -> pad slots 2 / 3, docs/research/arcade_disc.md section 19) are switched.
inline bool ArcadeAiPlayerSwitch(Machine& m, uint32_t from, uint32_t to) {
    if (to != 0x80033330u || from != 0x800130DCu) return false;
    uint32_t insn = 0;
    if (!m.bus.FastRead32(from, insn) || insn != 0x0C00CCCCu) return false; // jal 0x80033330 of the arcade race overlay only
    const uint32_t body = m.cpu.gpr[4], car = body - 0x2C, sp = m.cpu.gpr[29];
    uint32_t padSlot = 0;
    if (!m.bus.FastRead32(car + 0x18, padSlot) || ((padSlot & 0xFFFF) != 2 && (padSlot & 0xFFFF) != 3)) return false; // players 1 and 2 (pad slots 2 / 3)
    m.bus.Write(sp + 0x1C, 2, 4);
    return true;
}

} // namespace gt2
