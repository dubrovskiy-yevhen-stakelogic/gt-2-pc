#pragma once
#include "offscreen_car_audit.h"

namespace cockpit_fragment {
struct Pose {
    const char* name;
    float yaw, pitch, roll;
    std::array<float,3> offset;
};
// The IPD follows head orientation. Side views with a world-X IPD silently
// collapse the stereo baseline along the viewing direction.
inline constexpr std::array<Pose,12> poses{{
    {"forward",0,0,0,{}}, {"dashboard",0,-25,0,{}},
    {"front_left",-45,0,0,{}}, {"front_right",45,0,0,{}},
    {"left",-90,0,0,{}}, {"right",90,0,0,{}},
    {"rear_left",-135,-10,0,{}}, {"rear_right",135,-10,0,{}},
    {"rear",180,-10,0,{}},
    {"left_tilt",-75,8,18,{-.10f,.06f,0}},
    {"right_tilt",75,-8,-18,{.10f,-.06f,0}},
    {"rear_offset",170,-25,15,{0,.10f,.10f}}
}};
inline std::array<float,16> Matrix(const std::array<float,3>& origin,const Pose& pose,int eye) {
    constexpr float radians=3.14159265358979323846f/180;
    const float yaw=pose.yaw*radians,pitch=pose.pitch*radians,roll=pose.roll*radians;
    const std::array<float,3> right{std::cos(yaw),0,std::sin(yaw)};
    const std::array<float,3> up{-std::sin(yaw)*std::sin(pitch),std::cos(pitch),std::cos(yaw)*std::sin(pitch)};
    std::array<float,3> at{};
    for(size_t k=0;k<3;++k)at[k]=origin[k]+pose.offset[k]+(eye?.032f:-.032f)*(right[k]*std::cos(roll)+up[k]*std::sin(roll));
    auto matrix=View(at,pose.yaw,pose.pitch);
    for(size_t column=0;column<4;++column) {
        const float x=matrix[column*4],y=matrix[column*4+1];
        matrix[column*4]=x*std::cos(roll)-y*std::sin(roll);
        matrix[column*4+1]=x*std::sin(roll)+y*std::cos(roll);
    }
    return matrix;
}
}
