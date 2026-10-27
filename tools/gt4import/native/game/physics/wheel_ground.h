#pragma once
// Native ports of the per-wheel ground step 0x0036a758 of CORE.GT4 US 1.01 (suspension travel w[+0x94], spring /
// damper forces w[+0x9c] / w[+0xa0], wheel load w[+0x48], contact state w[+0xdc], per-wheel vertical loads
// +0x524) and its helpers. `car` = byte image of the car object, `spec` = image of the block at car[+0x10]
// (>= 0x10d8 bytes; per-axle suspension records at spec+0x308 stride 88), `entry` = its 0x00359510 entry.
// Verified by gt4verify rows phys_* / cap_phys_* (docs/research/vu0.md).
#include <cstdint>

namespace gt4::game {

// Image-resident inputs: `g` = image of the block 0x00620cb0..0x00620e14 (bump-stop mode word +0, tables +8 / +0x10
// / +0x18 / +0x20, flag bytes +0x129 (0x00620dd9) and +0x161 (0x00620e11)); `surface` = image of 0x00620288
// (256 bytes, per-class flag), `tyre` = image of the 32-byte record 0x00622268 + 32 * spec[+0x1076].
struct WheelGroundGlobals {
    const uint8_t* g;
    const uint8_t* surface;
    const uint8_t* tyre;
};

// 0x0036a048(wa, wb, axle): anti-roll term s[+0x30] * d * |d|, d = wa[+0x94] - wb[+0x94].
uint32_t PhysAntiRoll(const uint8_t* wa, const uint8_t* wb, const uint8_t* axle);
// 0x0035d248(car, i): spec axle value [+0x31c]; for spec[+0x32] == 1 cars with byte +0x788 != 0 it is kept only for
// the wheel selected by +0x544 and +0x788 (else 0).
uint32_t PhysAxleScale(const uint8_t* car, const uint8_t* spec, const uint8_t* entry, int64_t i);
// 0x00369b48(axle, k, x): damper force for the velocity ratio x (piecewise / square-root branches by flag byte
// 0x00620dd9, optional blend tables 0x00620cb8 + 4k / 0x00620cc0 + 4k (compression) and +0x10 / +0x18 (rebound)).
uint32_t PhysDamper(const uint8_t* axle, int64_t k, uint32_t x, const WheelGroundGlobals& gl);
// 0x003f24a0(car, i): tyre-type term: by tyre record type 2 (speed ramp +0x14/+0x18/+0x1c) or 3 (surface flag).
uint32_t PhysTyreTerm(const uint8_t* car, int64_t i, const WheelGroundGlobals& gl);
// 0x003f2650(car, i, x): tyre-type extra force for travel x (axle i >> 1).
uint32_t PhysTyreForce(const uint8_t* car, const uint8_t* spec, int64_t i, uint32_t x, const WheelGroundGlobals& gl);
// 0x00369de8(car, w, axle, i, k, x, r): one wheel: travel w[+0x94] / rate w[+0x98] / spring w[+0x9c] / damper
// w[+0xa0] / load w[+0x48] from the free length x and scale r.
void PhysWheelSpring(uint8_t* car, uint8_t* w, const uint8_t* spec, const uint8_t* axle, int64_t i, int64_t k, uint32_t x,
                     uint32_t r, const WheelGroundGlobals& gl);
// 0x0036a578(spec, i, a, b) / 0x0036a690(spec, i, a, b): camber-like wheel terms w[+4] / w[+8].
uint32_t PhysWheelTermA(const uint8_t* spec, const uint8_t* entry, int64_t i, uint32_t a, uint32_t b);
uint32_t PhysWheelTermB(const uint8_t* spec, const uint8_t* entry, int64_t i, uint32_t a, uint32_t b);
// 0x0036a758(car). Returns false (nothing written) when the original would read its anti-roll array outside the
// slots it wrote (entry[44+i] >= entry[2] for a paired wheel, or more than 4 axles).
bool PhysWheelGround(uint8_t* car, const uint8_t* spec, const uint8_t* entry, const WheelGroundGlobals& gl);

} // namespace gt4::game
