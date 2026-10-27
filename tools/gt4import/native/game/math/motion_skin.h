#pragma once
// Skin vertex micro routines at CORE006d3280 +2e8/308/460/478 (US1.01 SHA1
//30031137ce494323816bb707b90d3ec0d07937d2). Own CORE bytes match captured micro0..7ff.
#include "game/math/vu0_regs.h"

namespace gt4::game {
// vf1: four weights; vf2: position; vf3: optional normal; vf4.x: position scale;
// vf8..23: four column-major palette matrices. Outputs vf2 position, vf3 unit normal.
inline void MotionSkinVertex(Vu0Regs& v,bool packed,bool normal) {
    using namespace vu;
    if (packed) {
        Itof(v.vf[1],v.vf[1],0,XYZW);
        if (normal) Itof(v.vf[3],v.vf[3],0,XYZW);
        Itof(v.vf[2],v.vf[2],0,XYZW);
        v.i = 0x3c010204;
        MulS(v.vf[1],v.vf[1],v.i,XYZW);
    }
    if (normal) {
        MulS(v.vf[5],v.vf[3],v.vf[1].f[kX],XYZW);
        MulS(v.vf[6],v.vf[3],v.vf[1].f[kY],XYZW);
        MulS(v.vf[7],v.vf[3],v.vf[1].f[kZ],XYZW);
        MulS(v.vf[3],v.vf[3],v.vf[1].f[kW],XYZW);
        MulS(v.acc,v.vf[8],v.vf[5].f[kX],XYZW);
        MaddS(v.acc,v.acc,v.vf[9],v.vf[5].f[kY],XYZW);
        MaddS(v.acc,v.acc,v.vf[10],v.vf[5].f[kZ],XYZW);
        for (int bone = 1; bone < 4; ++bone) {
            const auto& weighted = v.vf[bone == 1 ? 6 : bone == 2 ? 7 : 3];
            MaddS(v.acc,v.acc,v.vf[8+4*bone],weighted.f[kX],XYZW);
            MaddS(v.acc,v.acc,v.vf[9+4*bone],weighted.f[kY],XYZW);
            if (bone == 3) MaddS(v.vf[3],v.acc,v.vf[10+4*bone],weighted.f[kZ],XYZW);
            else MaddS(v.acc,v.acc,v.vf[10+4*bone],weighted.f[kZ],XYZW);
        }
    }
    MulS(v.vf[2],v.vf[2],v.vf[4].f[kX],XYZW);
    MulS(v.vf[5],v.vf[2],v.vf[1].f[kX],XYZW);
    MulS(v.vf[6],v.vf[2],v.vf[1].f[kY],XYZW);
    MulS(v.vf[7],v.vf[2],v.vf[1].f[kZ],XYZW);
    MulS(v.vf[2],v.vf[2],v.vf[1].f[kW],XYZW);
    MulS(v.acc,v.vf[8],v.vf[5].f[kX],XYZW);
    MaddS(v.acc,v.acc,v.vf[9],v.vf[5].f[kY],XYZW);
    MaddS(v.acc,v.acc,v.vf[10],v.vf[5].f[kZ],XYZW);
    MaddS(v.acc,v.acc,v.vf[11],v.vf[1].f[kX],XYZW);
    for (int bone = 1; bone < 4; ++bone) {
        const auto& weighted = v.vf[bone == 1 ? 6 : bone == 2 ? 7 : 2];
        MaddS(v.acc,v.acc,v.vf[8+4*bone],weighted.f[kX],XYZW);
        MaddS(v.acc,v.acc,v.vf[9+4*bone],weighted.f[kY],XYZW);
        MaddS(v.acc,v.acc,v.vf[10+4*bone],weighted.f[kZ],XYZW);
        if (bone == 3) MaddS(v.vf[2],v.acc,v.vf[11+4*bone],v.vf[1].f[bone],XYZW);
        else MaddS(v.acc,v.acc,v.vf[11+4*bone],v.vf[1].f[bone],XYZW);
    }
    if (normal) {
        Vec4 squared; Mul(squared,v.vf[3],v.vf[3],XYZW);
        uint32_t length = ee::VuAdd(squared.f[kX],squared.f[kY],nullptr);
        length = ee::VuAdd(length,squared.f[kZ],nullptr);
        const uint32_t inverse = ee::VuRsqrt(v.vf[0].f[kW],length,nullptr);
        MulS(v.vf[3],v.vf[3],inverse,XYZW);
    }
}
} // namespace gt4::game
