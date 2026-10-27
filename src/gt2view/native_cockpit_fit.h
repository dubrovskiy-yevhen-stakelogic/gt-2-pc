#pragma once
#include "gt2formats/gltf_reader.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace gt2view {
inline bool NativeAlphaAcceptsZero(const gt2::GltfMaterial& m) {
    if(m.nativeGt4Type<0)return false;
    const int f=m.nativeAlphaFunc,r=m.nativeAlphaRef;
    return f<0 || f==1 || (f==2 && r>0) || (f==3 && r>=0) || (f==4 && r==0) ||
        (f==5 && r<=0) || (f==6 && r<0) || (f==7 && r!=0);
}
inline bool NativeAlphaBlendsZero(const gt2::GltfMaterial& m) {
    // GS AFAIL FB_ONLY/RGB_ONLY still executes colour blending when ATST
    // rejects zero. Promoting that packet to opaque fills wheel-spoke cutouts.
    return NativeAlphaAcceptsZero(m) || (m.nativeGt4Type>=0 && (m.nativeAlphaFail==1 || m.nativeAlphaFail==3));
}
inline bool NativeTextureAlphaDiscard(const gt2::GltfMaterial& m,uint8_t alpha) {
    // Type 1's recovered VU alpha is 127/128. DECAL bypasses modulation.
    // Match the shader's GS alpha test, including the equality boundary.
    if(m.nativeGt4Type!=1 || !m.nativeTextureAlpha || m.nativeTextureFunction>1 ||
       (m.nativeAlphaFail!=0 && m.nativeAlphaFail!=2))return false;
    const float a=float(alpha)*(m.nativeTextureFunction==0?127.f:128.f)/255.f;
    const float r=float(m.nativeAlphaRef);bool pass=true;
    switch(m.nativeAlphaFunc){case 0:pass=false;break;case 2:pass=a<r;break;
        case 3:pass=a<=r;break;case 4:pass=std::abs(a-r)<.5f;break;
        case 5:pass=a>=r;break;case 6:pass=a>r;break;case 7:pass=std::abs(a-r)>=.5f;break;}
    return !pass;
}
// Identify the authored steering rim before fitting the presentation camera.
// Road wheels are separate attachments. A cabin rim is an upright, shallow,
// nearly circular packet, not a dashboard-wide plane or a longitudinal column.
// This also accepts the alpha-masked rim used by the S2000.
struct NativeCockpitWheel {
    std::array<float,3> lo{},hi{},center{};
    std::vector<bool> primitives;
    bool valid=false;
};
inline NativeCockpitWheel FindNativeCockpitWheel(const gt2::GltfMesh& mesh) {
    NativeCockpitWheel result;result.primitives.resize(mesh.primitives.size());
    // Hash-bound importer evidence handles tilted wheels, tillers and paired
    // grips without widening the heuristic until road trim becomes a wheel.
    result.lo={1e9f,1e9f,1e9f};result.hi={-1e9f,-1e9f,-1e9f};
    for(size_t index=0;index<mesh.primitives.size();++index){
        const auto& p=mesh.primitives[index];if(p.material<0)continue;
        const auto& m=mesh.materials.at(size_t(p.material));
        if(!m.cockpitControl || m.wheelAttachment>=0 || m.reflection || m.lampStates)continue;
        result.primitives[index]=true;
        for(auto i:p.indices)for(size_t k=0;k<3;++k){const float v=p.vertices.at(i).position[k];result.lo[k]=std::min(result.lo[k],v);result.hi[k]=std::max(result.hi[k],v);result.valid=true;}
    }
    if(result.valid){for(size_t k=0;k<3;++k)result.center[k]=(result.lo[k]+result.hi[k])*.5f;return result;}
    struct Bounds {std::array<float,3> lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};};
    std::vector<Bounds> bounds;
    float best=0;
    for(const auto& p:mesh.primitives) {
        Bounds b;for(auto i:p.indices)for(size_t k=0;k<3;++k){const float v=p.vertices.at(i).position[k];b.lo[k]=std::min(b.lo[k],v);b.hi[k]=std::max(b.hi[k],v);}
        bounds.push_back(b);
        if(p.material<0 || p.indices.size()<12)continue;
        const auto& m=mesh.materials.at(size_t(p.material));
        if(m.reflection || m.rearView || m.lampStates || m.wheelAttachment>=0)continue;
        const float w=b.hi[0]-b.lo[0],h=b.hi[1]-b.lo[1],d=b.hi[2]-b.lo[2];
        const float x=(b.lo[0]+b.hi[0])*.5f,y=(b.lo[1]+b.hi[1])*.5f,z=(b.lo[2]+b.hi[2])*.5f;
        if(w<.25f || w>.52f || h<w*.80f || h>w*1.12f || d<.025f || d>w*.60f ||
           std::abs(x)>.65f || y<.40f || y>1.20f || z< -1.4f || z>.4f)continue;
        if(w*h>best){best=w*h;result.lo=b.lo;result.hi=b.hi;result.valid=true;}
    }
    if(!result.valid)return result;
    for(size_t k=0;k<3;++k)result.center[k]=(result.lo[k]+result.hi[k])*.5f;
    // Include the rim's back, spokes and hub packets, retaining its long column.
    for(size_t i=0;i<bounds.size();++i) {
        bool inside=true;for(size_t k=0;k<3;++k)
            inside&=bounds[i].lo[k]>=result.lo[k]-.025f && bounds[i].hi[k]<=result.hi[k]+.025f;
        result.primitives[i]=inside;
    }
    return result;
}
}
