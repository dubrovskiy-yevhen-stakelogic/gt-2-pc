#pragma once
#include "gt2view/scene_assets.h"
#include "game/sim/race_sim.h"
#include <limits>

namespace gt2view {
inline float RaceRoadClearance(const sim::NativeCourse& course,uint16_t chunk,const std::array<float,3>& p) {
    sim::ContactQuery query{};query.chunkIndex=chunk;
    query.x=int32_t(std::lround(p[0]*65536.f));query.height=int32_t(std::lround(p[1]*65536.f));
    query.y=-int32_t(std::lround(p[2]*65536.f));
    sim::QueryContact(course.Surface(),query);
    return query.surfaceHeight==INT32_MAX?std::numeric_limits<float>::infinity():p[1]-query.surfaceHeight/65536.f;
}
// Presentation-only surface queries. The simulation and its contact records
// remain authoritative and are never changed by replacement model fitting.
inline void FitRaceCarRoadContact(const SceneAssets& assets,int slot,const sim::NativeCourse& course,
                                 uint16_t chunk,float* world,std::array<std::array<float,16>,4>& wheels) {
    assets.FitCarRoadContact(slot,world,wheels,[&](const std::array<float,3>& p){
        return RaceRoadClearance(course,chunk,p);
    });
}
inline void FitRaceCarRoadShadow(SceneAssets& assets,int slot,const sim::NativeCourse& course,uint16_t chunk,const float* ground) {
    assets.FitCarRoadShadow(slot,ground,[&](const auto& p){return RaceRoadClearance(course,chunk,p);});
}
}
