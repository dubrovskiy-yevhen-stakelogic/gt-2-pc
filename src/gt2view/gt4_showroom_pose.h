#pragma once
#include "gt2formats/model_set.h"
#include <algorithm>

namespace gt2view {
// CAR4 vertices and wheel anchors share body-local coordinates. Preserve the
// native showroom pitch before fitting them to the GT2 contact plane. Applying
// a separate pitch to only the shell would detach the controls and axles.
inline void ApplyNativeBodyPose(gt2::ModelSetCar& car) {
    if(!car.nativeWheelPose)return;
    const auto rotate=[&](std::array<float,3> p,bool position) {
        if(position)for(size_t k=0;k<3;++k)p[k]-=car.nativeBodyPivot[k];
        std::array<float,3> result{};
        for(size_t row=0;row<3;++row) {
            for(size_t col=0;col<3;++col)result[row]+=car.nativeBodyRotation[col*3+row]*p[col];
            if(position)result[row]+=car.nativeBodyPivot[row];
        }
        return result;
    };
    for(auto* mesh:{&car.exterior,&car.cockpit,&car.lamps,&car.reflections}) {
        mesh->boundsMin.fill(1e9f);mesh->boundsMax.fill(-1e9f);
        for(auto& primitive:mesh->primitives)for(auto& vertex:primitive.vertices) {
            vertex.position=rotate(vertex.position,true);vertex.normal=rotate(vertex.normal,false);
            for(size_t k=0;k<3;++k){mesh->boundsMin[k]=std::min(mesh->boundsMin[k],vertex.position[k]);mesh->boundsMax[k]=std::max(mesh->boundsMax[k],vertex.position[k]);}
        }
    }
    car.eye=rotate(car.eye,true);car.mirror=rotate(car.mirror,true);
    const auto speed=rotate({car.speedometer[0],car.speedometer[1],car.speedometer[2]},true);
    std::copy(speed.begin(),speed.end(),car.speedometer.begin());
    for(auto& wheel:car.wheelWells) {
        const auto position=rotate({wheel[0],wheel[1],wheel[2]},true);
        std::copy(position.begin(),position.end(),wheel.begin());
    }
}
}
