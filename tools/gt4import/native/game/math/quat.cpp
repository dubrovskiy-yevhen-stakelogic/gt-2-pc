#include "game/math/quat.h"

#include <cstring>

#include "ee/fpu.h"
#include "game/math/libm.h"
#include <array>

namespace gt4::game {

using namespace gt4::game::vu;
using namespace gt4::ee;
void QuatFromEuler(uint32_t out[4],uint32_t x,uint32_t y,uint32_t z) {
    uint32_t sx,cx,sy,cy,sz,cz;
    LibmSinCos(FpuMul(x,0x3f000000,nullptr),&sx,&cx);
    LibmSinCos(FpuMul(y,0x3f000000,nullptr),&sy,&cy);
    LibmSinCos(FpuMul(z,0x3f000000,nullptr),&sz,&cz);
    const uint32_t sxcy = FpuMul(sx,cy,nullptr), cxcy = FpuMul(cx,cy,nullptr);
    const uint32_t sxsy = FpuMul(sx,sy,nullptr), cxsy = FpuMul(cx,sy,nullptr);
    out[0] = FpuSub(FpuMul(sxcy,cz,nullptr),FpuMul(cxsy,sz,nullptr),nullptr);
    out[1] = FpuAdd(FpuMul(cxsy,cz,nullptr),FpuMul(sxcy,sz,nullptr),nullptr);
    out[2] = FpuSub(FpuMul(cxcy,sz,nullptr),FpuMul(sxsy,cz,nullptr),nullptr);
    out[3] = FpuAdd(FpuMul(cxcy,cz,nullptr),FpuMul(sxsy,sz,nullptr),nullptr);
}
void QuatSlerp(uint32_t out[4],const uint32_t first[4],const uint32_t second[4],uint32_t blend) {
    if (FpuEq(blend,0)) { std::memmove(out,first,16); return; }
    if (FpuEq(blend,0x3f800000)) { std::memmove(out,second,16); return; }
    uint32_t dot = FpuMul(first[0],second[0],nullptr);
    for (size_t i = 1; i < 4; ++i) dot = FpuAdd(dot,FpuMul(first[i],second[i],nullptr),nullptr);
    if (FpuLt(dot,0)) {
        std::array<uint32_t,4> opposite;
        for (size_t i = 0; i < 4; ++i) opposite[i] = FpuNeg(second[i]);
        QuatSlerp(out,first,opposite.data(),blend);
        return;
    }
    if (!FpuLt(0xbf800000,dot)) dot = 0xbf800000;
    if (!FpuLt(dot,0x3f800000)) dot = 0x3f800000;
    const uint32_t angle = FpuSub(0x3fc90fda,LibmAsin(dot),nullptr);
    const uint32_t sine = LibmSin(angle);
    if (FpuEq(sine,0)) { std::memmove(out,first,16); return; }
    const uint32_t inverse = FpuDiv(0x3f800000,sine,nullptr);
    const uint32_t a = FpuMul(LibmSin(FpuMul(angle,FpuSub(0x3f800000,blend,nullptr),nullptr)),inverse,nullptr);
    const uint32_t b = FpuMul(LibmSin(FpuMul(angle,blend,nullptr)),inverse,nullptr);
    std::array<uint32_t,4> result;
    for (size_t i = 0; i < 4; ++i) result[i] = FpuAdd(FpuMul(first[i],a,nullptr),FpuMul(second[i],b,nullptr),nullptr);
    std::memcpy(out,result.data(),16);
}
void QuatToMatrix(uint32_t out[16],const uint32_t translation[3],const uint32_t quat[4]) {
    const uint32_t x = quat[0], y = quat[1], z = quat[2], w = quat[3];
    const uint32_t dx = FpuAdd(x,x,nullptr), dy = FpuAdd(y,y,nullptr), dz = FpuAdd(z,z,nullptr);
    const uint32_t zz = FpuMul(dz,z,nullptr), zw = FpuMul(dz,w,nullptr);
    const uint32_t xy = FpuMul(dx,y,nullptr), xz = FpuMul(dx,z,nullptr), yz = FpuMul(dy,z,nullptr);
    const uint32_t xw = FpuMul(dx,w,nullptr), yw = FpuMul(dy,w,nullptr);
    const uint32_t xx = FpuMul(dx,x,nullptr), yy = FpuMul(dy,y,nullptr);
    std::array<uint32_t,16> matrix{};
    matrix[0] = FpuSub(FpuSub(0x3f800000,yy,nullptr),zz,nullptr);
    matrix[1] = FpuAdd(xy,zw,nullptr); matrix[2] = FpuSub(xz,yw,nullptr);
    matrix[4] = FpuSub(xy,zw,nullptr);
    matrix[5] = FpuSub(FpuSub(0x3f800000,zz,nullptr),xx,nullptr); matrix[6] = FpuAdd(yz,xw,nullptr);
    matrix[8] = FpuAdd(xz,yw,nullptr); matrix[9] = FpuSub(yz,xw,nullptr);
    matrix[10] = FpuSub(FpuSub(0x3f800000,xx,nullptr),yy,nullptr);
    std::memcpy(matrix.data()+12,translation,12); matrix[15] = 0x3f800000;
    std::memcpy(out,matrix.data(),64);
}

void Vu0Cross(Vu0Regs& v, uint32_t out[4], const uint32_t a[4], const uint32_t b[4]) {
    std::memcpy(v.vf[1].f, a, 16);
    std::memcpy(v.vf[2].f, b, 16);
    OpMula(v.acc, v.vf[1], v.vf[2]);
    OpMsub(v.vf[1], v.acc, v.vf[2], v.vf[1]);
    std::memcpy(out, v.vf[1].f, 16);
}

void Vu0QuatMul(Vu0Regs& v, uint32_t out[4], const uint32_t a[4], const uint32_t b[4]) {
    uint32_t f0 = FpuMul(FpuNeg(a[0]), b[0], nullptr);
    const uint32_t f5 = FpuMul(a[1], b[1], nullptr);
    const uint32_t f1 = FpuMul(a[3], b[3], nullptr);
    const uint32_t f2 = FpuMul(a[2], b[2], nullptr);
    f0 = FpuSub(f0, f5, nullptr);
    f0 = FpuSub(f0, f2, nullptr);
    f0 = FpuSub(f0, f1, nullptr);
    std::memcpy(v.vf[1].f, a, 16);
    std::memcpy(v.vf[2].f, b, 16);
    v.vf[3].f[3] = f0;   // xyz come from stack garbage and are overwritten by OPMSUB below
    OpMula(v.acc, v.vf[1], v.vf[2]);
    OpMsub(v.vf[3], v.acc, v.vf[2], v.vf[1]);
    MulS(v.acc, v.vf[1], v.vf[2].f[kW], XYZW);
    MaddS(v.vf[4], v.acc, v.vf[2], v.vf[1].f[kW], XYZW);
    Add(v.vf[3], v.vf[4], v.vf[3], XYZW);
    std::memcpy(out, v.vf[3].f, 16);
}

} // namespace gt4::game
