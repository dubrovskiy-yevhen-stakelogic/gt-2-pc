#pragma once
//00459780 ShowRoomCarParamSponsor, CORE US1.01 SHA1
//30031137ce494323816bb707b90d3ec0d07937d2. Geometry getters are separate.
#include "ee/fpu.h"
#include "game/math/libm.h"
#include <array>
#include <cstring>
#include "game/physics/wheel_ground.h"
#include "game/physics/steering.h"
#include "game/math/motion_transform.h"
namespace gt4::adhoc {
inline std::array<uint32_t,2> ShowroomBodyPose(uint32_t frontY,uint32_t rearY,
    uint32_t frontHeight,uint32_t rearHeight,uint32_t frontZ,uint32_t rearZ) {
    using namespace ee;
    auto add=[](uint32_t a,uint32_t b){return FpuAdd(a,b,nullptr);};
    auto sub=[](uint32_t a,uint32_t b){return FpuSub(a,b,nullptr);};
    auto mul=[](uint32_t a,uint32_t b){return FpuMul(a,b,nullptr);};
    auto div=[](uint32_t a,uint32_t b){return FpuDiv(a,b,nullptr);};
    frontY=FpuNeg(frontY); rearY=FpuNeg(rearY);
    const auto dy=sub(rearY,frontY),dh=sub(rearHeight,frontHeight),dz=sub(rearZ,frontZ);
    uint32_t cosine,sine;
    if (FpuEq(dy,0)) {
        cosine=0x3f800000; sine=0;
        if (!FpuEq(dz,0)) { sine=div(dh,dz); cosine=FpuSqrt(sub(0x3f800000,mul(sine,sine)),nullptr); }
    } else {
        const auto term=mul(dy,FpuSqrt(add(sub(mul(dz,dz),mul(dh,dh)),mul(dy,dy)),nullptr));
        const auto reciprocal=div(0x3f800000,add(mul(dz,dz),mul(dy,dy)));
        sine=mul(add(term,mul(dh,dz)),reciprocal);
        const auto otherSine=mul(FpuNeg(sub(term,mul(dh,dz))),reciprocal);
        cosine=div(sub(mul(dz,sine),dh),dy);
        const auto otherCosine=div(sub(mul(dz,otherSine),dh),dy);
        if (FpuLt(cosine,0) || (FpuLt(0x3f800000,cosine) && FpuLt(0,otherCosine))) {
            cosine=otherCosine; sine=otherSine;
        }
    }
    const auto pitch=FpuNeg(mul(game::LibmAsin(sine),0x42652ee0));
    const auto reciprocal=div(0x3f800000,cosine);
    const auto slope=mul(sine,reciprocal); //45998c: shared product precedes both height products
    rearZ=add(rearZ,mul(rearY,slope));
    frontZ=sub(FpuNeg(frontZ),mul(frontY,slope));
    const auto height=div(add(mul(add(frontHeight,mul(frontY,reciprocal)),rearZ),
        mul(add(rearHeight,mul(rearY,reciprocal)),frontZ)),add(frontZ,rearZ));
    return {height,pitch};
}
struct ShowroomGeometry {
    std::array<uint32_t,2> body{}; // height, pitch degrees
    std::array<std::array<uint32_t,3>,4> wheels{};
    std::array<std::array<uint32_t,3>,4> angles{},dimensions{}; // spin/steer/camber degrees; width/radius/rim
    std::array<uint32_t,4> travel{},sourceWheel{};
    bool operator==(const ShowroomGeometry&) const = default;
};
inline ShowroomGeometry ShowroomGeometryFromWork(const uint8_t* work,const uint8_t* entry) {
    auto word=[&](size_t offset){uint32_t value;std::memcpy(&value,work+offset,4);return value;};
    auto axle=[&](size_t wheel){return wheel < entry[1] ? entry[0x14+wheel] : 0;};
    auto y=[&](size_t wheel){const auto at=0x58*axle(wheel);return ee::FpuSub(
        ee::FpuSub(word(at+0x914),word(at+0x918),nullptr),0,nullptr);};
    auto z=[&](size_t wheel){
        if (wheel >= entry[1]) return uint32_t(0);
        const auto half=ee::FpuMul(word(0x618),0x3f000000,nullptr);
        const auto direction=static_cast<int8_t>(entry[0x3c+wheel]);
        return direction > 0 ? ee::FpuNeg(half) : direction < 0 ? half : 0;
    };
    ShowroomGeometry result;
    result.body=ShowroomBodyPose(y(0),y(2),word(0x8c4+4*axle(0)),word(0x8c4+4*axle(2)),z(0),z(2));
    result.sourceWheel=entry[0] == 1 ? std::array<uint32_t,4>{0,0,1,1} :
        entry[0] == 2 ? std::array<uint32_t,4>{0,0,1,2} :
        entry[0] == 3 ? std::array<uint32_t,4>{0,1,2,2} : std::array<uint32_t,4>{0,1,2,3};
    for (size_t slot = 0; slot < 4; ++slot) {
        const auto wheel=result.sourceWheel[slot];
        result.wheels[slot]={game::PhysWheelTermB(work+0x600,entry,wheel,0,0),y(wheel),z(wheel)};
        result.angles[slot]={0,ee::FpuAdd(ee::FpuMul(game::PhysWheelSteerAngle(work+0x600,entry,wheel,0),0x42652ee0,nullptr),0,nullptr),
            ee::FpuMul(game::PhysWheelTermA(work+0x600,entry,wheel,0,0),0x42652ee0,nullptr)};
        result.dimensions[slot]={word(0x8d4+4*axle(wheel)),word(0x8c4+4*axle(wheel)),word(0x8dc+4*axle(wheel))};
        result.travel[slot]=ee::FpuSub(word(0x910+0x58*axle(wheel)),word(0x918+0x58*axle(wheel)),nullptr);
    }
    return result;
}
inline ShowroomGeometry ShowroomLift(ShowroomGeometry geometry,uint32_t lift) { //00156950,00459ad8/00459c10
    if (!ee::FpuLt(0,lift)) return geometry;
    geometry.body[0]=ee::FpuAdd(geometry.body[0],lift,nullptr);
    for (size_t i = 0; i < 4; ++i) {
        const auto drop=ee::FpuLe(geometry.travel[i],lift) ? geometry.travel[i] : lift;
        geometry.wheels[i][1]=ee::FpuAdd(geometry.wheels[i][1],ee::FpuNeg(drop),nullptr);
    }
    return geometry;
}
inline std::array<uint32_t,16> ShowroomBodyMatrix(const ShowroomGeometry& geometry) {
    game::MotionRenderTransform transform;
    const uint32_t position[3]{0,geometry.body[0],0};
    transform.Translate(position);
    game::Vu0MatRotateY(transform.vu,transform.scratch.data(),0);
    game::Vu0MatRotateX(transform.vu,transform.scratch.data(),geometry.body[1]);
    game::Vu0MatRotateZ(transform.vu,transform.scratch.data(),0);
    return transform.Current();
}
inline std::array<uint32_t,16> ShowroomWheelMatrix(const ShowroomGeometry& geometry,size_t slot) {
    game::MotionRenderTransform transform;
    const auto body=ShowroomBodyMatrix(geometry);
    game::Vu0MatLoad(transform.vu,transform.scratch.data(),body.data());
    transform.Translate(geometry.wheels.at(slot).data());
    game::Vu0MatRotateY(transform.vu,transform.scratch.data(),geometry.angles.at(slot)[1]);
    game::Vu0MatRotateZ(transform.vu,transform.scratch.data(),slot%2 ? ee::FpuNeg(geometry.angles[slot][2]) : geometry.angles[slot][2]);
    const uint32_t widthOffset[3]{ee::FpuMul(geometry.dimensions.at(slot)[0],slot%2 ? 0x3f000000 : 0xbf000000,nullptr),0,0};
    transform.Translate(widthOffset);
    game::Vu0MatRotateX(transform.vu,transform.scratch.data(),geometry.angles[slot][0]);
    if (slot%2) game::Vu0MatRotateY(transform.vu,transform.scratch.data(),0x43340000);
    return transform.Current();
}
} // namespace gt4::adhoc
