#pragma once
// Native ports of the VU0-macro vector helpers at 0x0048d4c0..0x0048d5c8 (CORE.GT4 US 1.01): 3D cross product and
// quaternion product (xyz = vector part, w = scalar part). Inputs/outputs are 16-byte quadwords (LQ/SQ), all VU0
// registers the originals write are reproduced. Verified by gt4verify rows "vec_*" (docs/research/vu0.md).
#include <cstdint>

#include "game/math/vu0_regs.h"

namespace gt4::game {
//0048b888, Euler radians -> quaternion using original EE sin/cos pair.
void QuatFromEuler(uint32_t out[4],uint32_t x,uint32_t y,uint32_t z);
//0048ca50: shortest-arc interpolation, original EE/libm ordering; supports aliased output.
void QuatSlerp(uint32_t out[4],const uint32_t first[4],const uint32_t second[4],uint32_t blend);
//0048a970: quaternion + raw translation -> 4x4 matrix, EE arithmetic.
void QuatToMatrix(uint32_t out[16],const uint32_t translation[3],const uint32_t quat[4]);

// 0x0048d4c0: out.xyz = a x b, out.w = a.w.
void Vu0Cross(Vu0Regs& v, uint32_t out[4], const uint32_t a[4], const uint32_t b[4]);
// 0x0048d530: out = a * b with w = a.w*b.w - (a.x*b.x + a.y*b.y + a.z*b.z) computed as in the original
// (-ax*bx - ay*by - az*bz - aw*bw, then + aw*bw + bw*aw on VU0).
void Vu0QuatMul(Vu0Regs& v, uint32_t out[4], const uint32_t a[4], const uint32_t b[4]);

} // namespace gt4::game
