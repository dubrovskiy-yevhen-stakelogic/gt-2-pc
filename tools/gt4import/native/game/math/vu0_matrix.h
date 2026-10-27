#pragma once
// Native port of the CORE.GT4 US 1.01 VU0 "current matrix" library (0x004a5348, 0x004a7350..0x004a7b10).
// The current matrix is held in VU0 vf28..vf31 (rows; vf31 = translation row); every function that changes it
// sets the byte at scratchpad 0x70002000 to 1 (a dirty flag). `scratchpad` is the native image of the 16 KiB
// scratchpad (0x70000000). Verified bit for bit by gt4verify rows "vu0m_*" (docs/research/vu0.md, math library).
// Native code reproduces every VU register the originals write (also scratch vf1..vf6, ACC, I, vi15), except the
// z/w lanes that QMTC2 copies from undefined upper GPR halves (translate/scale: vf1..vf3.zw are left untouched).
#include <cstdint>

#include "game/math/vu0_regs.h"

namespace gt4::game {

void Vu0MatIdentity(Vu0Regs& v, uint8_t* scratchpad);                         // 0x004a7454
void Vu0MatResetRotation(Vu0Regs& v, uint8_t* scratchpad);                    // 0x004a747c: 3x3 = I, w column kept
void Vu0MatClearTranslation(Vu0Regs& v, uint8_t* scratchpad);                 // 0x004a74a0
void Vu0MatLoad(Vu0Regs& v, uint8_t* scratchpad, const uint32_t m[16]);       // 0x004a74b4
void Vu0MatStore(const Vu0Regs& v, uint32_t out[16]);                         // 0x004a7550
void Vu0MatMultiply(Vu0Regs& v, uint8_t* scratchpad, const uint32_t m[16]);   // 0x004a7698: M = A * M (rows of A)
void Vu0MatPreMultiply(Vu0Regs& v, uint8_t* scratchpad, const uint32_t m[16]);// 0x004a7774: M = M * A
void Vu0MatTranslate(Vu0Regs& v, uint8_t* scratchpad, uint32_t x, uint32_t y, uint32_t z);   // 0x004a7844
void Vu0MatScale(Vu0Regs& v, uint8_t* scratchpad, uint32_t x, uint32_t y, uint32_t z);       // 0x004a7890
void Vu0MatTransformPoint(Vu0Regs& v, uint32_t out[4], const uint32_t in[4]);                // 0x004a7aa8
void Vu0MatTranspose(Vu0Regs& v, uint8_t* scratchpad);                        // 0x004a7a00
void Vu0MatInvertRigid(Vu0Regs& v, uint8_t* scratchpad);                      // 0x004a7a54
// 0x004a5348: switch among three saved matrix contexts (index byte at 0x70002001, slots at 0x70002ed0/f10/f50,
// vi13 saved at 0x70002f90/92/94). `index` is the full 64-bit argument register.
void Vu0MatSelectContext(Vu0Regs& v, uint8_t* scratchpad, uint64_t index);

// 0x004a7438: angle in degrees -> vf1 = (cos-like, sin-like, ...) via 0x004a7350 + VU0 micro routine 0x58.
void Vu0SinCosDegrees(Vu0Regs& v, uint32_t degrees);
// 0x004a7988 / 0x004a79b0 / 0x004a79d8: rotate the current matrix by an angle in degrees about X / Y / Z
// (VU0 micro routines 0x78 / 0xb8 / 0xf8).
void Vu0MatRotateX(Vu0Regs& v, uint8_t* scratchpad, uint32_t degrees);
void Vu0MatRotateY(Vu0Regs& v, uint8_t* scratchpad, uint32_t degrees);
void Vu0MatRotateZ(Vu0Regs& v, uint8_t* scratchpad, uint32_t degrees);

} // namespace gt4::game
