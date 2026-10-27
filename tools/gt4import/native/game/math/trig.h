#pragma once
// Native sin / cos / atan2 reproducing the CORE.GT4 US 1.01 VU0-based routines bit for bit (decision D2).
// Floats are raw 32-bit EE patterns; EE FPU steps use src/ee/fpu, VU0 steps (macro ops and the micro program
// at VU0 micro address 0x548) use src/ee/vu_float. Verified by gt4verify rows "sin", "cos", "atan2"
// (docs/research/vu0.md). VU0 register side effects of the originals (vf1..vf8, ACC, Q, I, MAC/status flags)
// are not reproduced: callers never read them (they are scratch registers in the core's VU0 macro code).
#include <cstdint>

namespace gt4::game {

struct TrigResult {
    uint32_t value = 0;
    // The original executes a trapping MIPS `sub`/`add` on the quadrant index; for |x| beyond ~1.3e9 it raises an
    // EE integer-overflow exception instead of returning (the native port reports it instead of inventing a value).
    bool overflowException = false;
};

// 0x004ac260: sin(x). Argument reduction by pi/2 on the EE FPU, polynomial on VU0 macro ops (constants 0x006b0060).
TrigResult Sin(uint32_t x);

// 0x004ac334: cos(x). Same reduction; quadrant sign handling as in the original, including its quirk: when the
// quadrant bit 1 of (k + 1) is clear the result is XORed with (k + 1) itself, not with 0.
TrigResult Cos(uint32_t x);

// 0x004ac118: atan2(y = $f12, x = $f13). Octant reduction on the EE FPU, atan(min/max) by the VU0 micro program
// at 0x548 (uploaded by 0x004a0574 from 0x006d3280), octant tables at 0x006b0018 / 0x006b0038.
uint32_t Atan2(uint32_t y, uint32_t x);

} // namespace gt4::game
