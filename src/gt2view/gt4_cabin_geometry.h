#pragma once
#include "gt2formats/gltf_reader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

namespace gt2view {
// Only uniformly dark backing inside a wheel arch may be opened for the
// animated GT2 wheel. Never carve the coloured wing or a mixed body triangle.
inline bool ModelWheelBacking(std::span<const gt2::GltfVertex> triangle,
                              const gt2::GltfMaterial* material,
                              const gt2::PngImage* image) {
    if (triangle.size()!=3) return false;
    if (material && material->reflection) return false; // black paint is still a body panel
    for (int a=0;a<=4;++a) for (int b=0;b<=4-a;++b) {
        const float weights[]{float(a)/4,float(b)/4,float(4-a-b)/4};
        std::array<float,3> colour{};std::array<float,2> uv{};
        for (size_t i=0;i<3;++i) {
            for (size_t k=0;k<3;++k) colour[k]+=triangle[i].color[k]*weights[i];
            for (size_t k=0;k<2;++k) uv[k]+=triangle[i].uv[k]*weights[i];
        }
        if (image && image->width>0 && image->height>0) {
            auto coordinate=[](float value,int size,bool clamp) {
                const int pixel=int(std::floor(value*float(size)));
                return clamp?std::clamp(pixel,0,size-1):(pixel%size+size)%size;
            };
            const int x=coordinate(uv[0],image->width,material && material->clampU);
            const int y=coordinate(uv[1],image->height,material && material->clampV);
            const auto* rgba=&image->rgba[(size_t(y)*size_t(image->width)+size_t(x))*4];
            for (size_t k=0;k<3;++k) colour[k]*=float(rgba[k])/255.f;
        }
        for (size_t k=0;k<3;++k)
            if (colour[k]*(material?material->baseColorFactor[k]:1.f)>.10f) return false;
    }
    return true;
}

// Fit the animated donor tyre under the authored wing rather than pushing its
// rim through the wing. Sample independent angular sectors of the upper arch;
// a single bumper/trim vertex must not shrink a wheel.
inline std::array<float,5> FitModelWheel(std::span<const std::array<float,3>> painted,
                                       std::array<float,5> well) {
    const float radius=well[3],sign=well[0]<0?-1.f:1.f;
    std::array<float,6> sectors;sectors.fill(1e9f);
    float outer=0;
    for(const auto& p:painted) {
        const float dy=p[1]-well[1],dz=p[2]-well[2],r=std::hypot(dy,dz);
        if(p[0]*sign<std::abs(well[0])*.78f || dy<radius*.15f ||
           r<radius*.65f || r>radius*1.55f)continue;
        outer=(std::max)(outer,p[0]*sign);
        const int sector=std::clamp(int(std::atan2(dy,dz)*6.f/3.14159265f),0,5);
        sectors[size_t(sector)]=(std::min)(sectors[size_t(sector)],r);
    }
    std::vector<float> radii;
    for(float r:sectors)if(r<1e8f)radii.push_back(r);
    if(radii.size()>=4) {
        std::sort(radii.begin(),radii.end());
        well[3]=std::clamp(radii[radii.size()/2]*.94f,radius*.75f,radius);
        well[1]+=well[3]-radius; // retain the tyre contact plane
    }
    if(outer>0) {
        const float target=(std::max)(.1f,outer-well[4]*.5f-.025f);
        // This correction can tuck a tyre in; never move it farther outward.
        well[0]=sign*std::clamp(target,std::abs(well[0])-.20f,std::abs(well[0]));
    }
    return well;
}

// Highest presentation wheel centre allowed by the authored upper arch.
// The donor's suspension still drives physics; a differently shaped replacement
// wing supplies its own visual bump stop instead of being cut away.
inline float ModelWheelBumpStop(std::span<const std::array<float,3>> painted,
                                const std::array<float,5>& well) {
    float limit=1e9f;
    size_t samples=0;
    const float sign=well[0]<0?-1.f:1.f;
    for(size_t i=0;i+2<painted.size();i+=3)for(int a=0;a<=4;++a)for(int b=0;b<=4-a;++b) {
        std::array<float,3> p{};
        const float weights[]{float(a)/4,float(b)/4,float(4-a-b)/4};
        for(size_t k=0;k<3;++k)for(size_t j=0;j<3;++j)p[k]+=painted[i+j][k]*weights[j];
        const float dz=p[2]-well[2],dy=p[1]-well[1];
        if(p[0]*sign<std::abs(well[0]) || std::abs(dz)>well[3]*.65f ||
           dy<well[3]*.65f || dy>well[3]*1.5f)continue;
        limit=std::min(limit,p[1]-std::sqrt(std::max(0.f,well[3]*well[3]-dz*dz))-.01f);
        ++samples;
    }
    return samples>=3?std::max(well[1],limit):1e9f;
}

// Partition a surface at a piecewise-linear cowl without removing geometry.
// Long floor/wing triangles then have independent exterior and interior flags.
inline std::vector<gt2::GltfVertex> SplitModelCowl(std::span<const gt2::GltfVertex> triangle,
        const std::array<std::array<float,3>,17>& cowl,float scale,const std::array<float,3>& offset) {
    using Polygon=std::vector<gt2::GltfVertex>;
    float front=cowl.front()[2],back=front;
    for(const auto& p:cowl){front=std::min(front,p[2]);back=std::max(back,p[2]);}
    if(std::all_of(triangle.begin(),triangle.end(),[&](const auto& v){return v.position[2]*scale+offset[2]>=back+.025f;}) ||
       std::all_of(triangle.begin(),triangle.end(),[&](const auto& v){return v.position[2]*scale+offset[2]<=front+.025f;}))return {triangle.begin(),triangle.end()};
    // Test the complete piecewise-linear boundary, not only its global Z
    // extrema. Already-cut triangles commonly lie wholly on one side of a
    // curved cowl; splitting those across every station wastes the slot budget.
    bool positive=false,negative=false;
    auto sample=[&](float x,float z) {
        for(size_t i=1;i<cowl.size();++i)if(x<=cowl[i][0] || i+1==cowl.size()) {
            const auto& a=cowl[i-1];const auto& b=cowl[i];
            const float u=std::clamp((x-a[0])/std::max(.001f,b[0]-a[0]),0.f,1.f);
            const float d=z-a[2]-(b[2]-a[2])*u-.025f;
            positive|=d>1e-7f;negative|=d<-1e-7f;break;
        }
    };
    for(const auto& v:triangle)sample(v.position[0]*scale+offset[0],v.position[2]*scale+offset[2]);
    for(size_t edge=0;edge<triangle.size();++edge) {
        const auto& a=triangle[edge].position;const auto& b=triangle[(edge+1)%triangle.size()].position;
        const float ax=a[0]*scale+offset[0],bx=b[0]*scale+offset[0];
        if(std::abs(bx-ax)<1e-7f)continue;
        for(const auto& station:cowl)if(station[0]>std::min(ax,bx) && station[0]<std::max(ax,bx)) {
            const float u=(station[0]-ax)/(bx-ax);
            sample(station[0],(a[2]+(b[2]-a[2])*u)*scale+offset[2]);
        }
    }
    if(!positive || !negative)return {triangle.begin(),triangle.end()};
    auto clip=[](std::span<const gt2::GltfVertex> polygon,auto distance) {
        Polygon out;if(polygon.empty())return out;
        auto previous=polygon.back();float pd=distance(previous);
        for(const auto& current:polygon) {
            const float d=distance(current);
            if((d>=0)!=(pd>=0)) {
                const float t=pd/(pd-d);auto v=previous;
                for(size_t k=0;k<3;++k){v.position[k]+=(current.position[k]-v.position[k])*t;v.normal[k]+=(current.normal[k]-v.normal[k])*t;}
                for(size_t k=0;k<2;++k)v.uv[k]+=(current.uv[k]-v.uv[k])*t;
                for(size_t k=0;k<4;++k)v.color[k]+=(current.color[k]-v.color[k])*t;
                out.push_back(v);
            }
            if(d>=0)out.push_back(current);previous=current;pd=d;
        }
        return out;
    };
    Polygon result;
    for(size_t i=1;i<cowl.size();++i) {
        const auto& a=cowl[i-1];const auto& b=cowl[i];Polygon part(triangle.begin(),triangle.end());
        if(i>1)part=clip(part,[&](const auto& v){return v.position[0]*scale+offset[0]-a[0];});
        if(i+1<cowl.size())part=clip(part,[&](const auto& v){return b[0]-v.position[0]*scale-offset[0];});
        for(float sign:{-1.f,1.f}) {
            const auto half=clip(part,[&](const auto& v){
                const float x=v.position[0]*scale+offset[0],z=v.position[2]*scale+offset[2];
                const float u=std::clamp((x-a[0])/std::max(.001f,b[0]-a[0]),0.f,1.f);
                return sign*(z-a[2]-(b[2]-a[2])*u-.025f);
            });
            for(size_t j=1;j+1<half.size();++j){result.push_back(half[0]);result.push_back(half[j]);result.push_back(half[j+1]);}
        }
    }
    return result;
}

// Keep the outside of a convex volume. Unlike deleting whole triangles by
// their centre, clipping preserves a continuous boundary and interpolated UVs.
// Planes describe the inside as dot(normal, position) <= distance.
inline std::vector<gt2::GltfVertex> SubtractModelVolume(
    std::span<const gt2::GltfVertex> triangle, std::span<const std::array<float,4>> planes) {
    // Reject a disjoint triangle before splitting it against any plane. This
    // prevents untouched body panels acquiring extra vertices at each wheel.
    for (const auto& p:planes)
        if (std::all_of(triangle.begin(),triangle.end(),[&](const auto& v) {
            return p[0]*v.position[0]+p[1]*v.position[1]+p[2]*v.position[2]>p[3];
        })) return {triangle.begin(),triangle.end()};
    std::vector<gt2::GltfVertex> remaining(triangle.begin(),triangle.end()), result;
    for (const auto& plane:planes) {
        if (remaining.empty()) break;
        std::vector<gt2::GltfVertex> inside,outside;
        auto distance=[&](const auto& p) { return plane[0]*p.position[0]+plane[1]*p.position[1]+plane[2]*p.position[2]-plane[3]; };
        auto previous=remaining.back();float previousDistance=distance(previous);
        for (const auto& current:remaining) {
            const float currentDistance=distance(current);
            if ((previousDistance<=0)!=(currentDistance<=0)) {
                const float t=previousDistance/(previousDistance-currentDistance);
                auto edge=previous;
                for (size_t k=0;k<3;++k) {
                    edge.position[k]+=(current.position[k]-previous.position[k])*t;
                    edge.normal[k]+=(current.normal[k]-previous.normal[k])*t;
                }
                for (size_t k=0;k<2;++k) edge.uv[k]+=(current.uv[k]-previous.uv[k])*t;
                for (size_t k=0;k<4;++k) edge.color[k]+=(current.color[k]-previous.color[k])*t;
                inside.push_back(edge);outside.push_back(edge);
            }
            (currentDistance<=0?inside:outside).push_back(current);
            previous=current;previousDistance=currentDistance;
        }
        for (size_t i=1;i+1<outside.size();++i) {
            result.push_back(outside[0]);result.push_back(outside[i]);result.push_back(outside[i+1]);
        }
        remaining=std::move(inside);
    }
    return result;
}
} // namespace gt2view
