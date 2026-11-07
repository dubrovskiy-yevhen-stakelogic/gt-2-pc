#pragma once
#include "gt2view/model_glazing.h"
namespace gt2view {
// A lining occupies the thin interior strip next to the authored skin.
// Clear only that strip inside actual transparent texels. Original opaque
// bodywork, seats and other deep cabin objects are not part of this operation.
inline std::vector<SceneVertex> ClipCockpitLining(std::span<const SceneVertex> vertices,
        const std::vector<std::array<std::array<float,3>,3>>& apertures,
        std::span<const std::array<float,3>> eyes={}) {
#ifdef GT2_LEGACY_LINING_CLIP
    (void)apertures;return {vertices.begin(),vertices.end()};
#else
    if(apertures.empty())return {vertices.begin(),vertices.end()};
    std::vector<ModelGlazingOpening> windows;
    for(const auto& a:apertures){std::array<gt2::GltfVertex,3> t{};for(size_t j=0;j<3;++j)t[j].position=a[j];windows.push_back(GlazingOpening(t));}
    std::vector<SceneVertex> result;result.reserve(vertices.size());
    for(size_t at=0;at+2<vertices.size();at+=3){
        std::array<gt2::GltfVertex,3> t{};
        std::array<float,3> lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};
        for(size_t j=0;j<3;++j)for(size_t k=0;k<3;++k){
            t[j].position[k]=vertices[at+j].pos[k];
            // SubtractModelVolumes linearly interpolates this attribute;
            // carry the unlit lining colour, rather than a shading normal.
            t[j].normal[k]=vertices[at+j].color[k];
            lo[k]=std::min(lo[k],t[j].position[k]);hi[k]=std::max(hi[k],t[j].position[k]);
        }
        std::vector<ModelClipVolume> volumes;
        for(const auto& w:windows){
            const bool side=std::abs(w.normal[0])>1.1f*std::abs(w.normal[2]) && w.normal[0]*w.normal[0]>.16f;
            const float depth=side?.16f:.08f;
            bool overlaps=true;
            for(size_t k=0;k<3;++k)overlaps&=hi[k]>=w.lo[k]-depth && lo[k]<=w.hi[k]+depth;
            if(!overlaps)continue;
            float offset=0,minD=1e9f,maxD=-1e9f;
            for(size_t k=0;k<3;++k)offset+=w.normal[k]*w.points[0][k];
            for(const auto& q:t){float d=-offset;for(size_t k=0;k<3;++k)d+=w.normal[k]*q.position[k];minD=std::min(minD,d);maxD=std::max(maxD,d);}
            if(minD>=depth || maxD<=-depth)continue;
            ModelClipVolume planes(5);std::copy(w.edges.begin(),w.edges.end(),planes.begin());
            planes[3]={w.normal[0],w.normal[1],w.normal[2],offset+depth};
            planes[4]={-w.normal[0],-w.normal[1],-w.normal[2],-offset+depth};volumes.push_back(planes);
            // The normal prism alone misses recessed trim seen obliquely.
            // Add sight cones only inside the same thin physical lining band;
            // never remove deeper furniture or the independent closed backing.
            for(const auto& eye:eyes) {
                float eyeDistance=-offset;for(size_t k=0;k<3;++k)eyeDistance+=w.normal[k]*eye[k];
                if(std::abs(eyeDistance)<=depth)continue;
                ModelClipVolume cone=planes;bool valid=true;
                for(size_t edge=0;edge<3;++edge) {
                    const auto& a=w.points[edge];const auto& b=w.points[(edge+1)%3];const auto& c=w.points[(edge+2)%3];
                    std::array<float,3> u{},v{},n{};
                    for(size_t k=0;k<3;++k){u[k]=b[k]-a[k];v[k]=eye[k]-a[k];}
                    n={u[1]*v[2]-u[2]*v[1],u[2]*v[0]-u[0]*v[2],u[0]*v[1]-u[1]*v[0]};
                    const float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
                    if(length<1e-9f){valid=false;break;}
                    float d=0,inside=0;for(size_t k=0;k<3;++k){n[k]/=length;d+=n[k]*a[k];inside+=n[k]*(c[k]-a[k]);}
                    if(inside>0){for(float& x:n)x=-x;d=-d;}
                    cone[edge]={n[0],n[1],n[2],d};
                }
                if(valid)volumes.push_back(std::move(cone));
            }
        }
        if(volumes.empty()){result.insert(result.end(),vertices.begin()+at,vertices.begin()+at+3);continue;}
        for(const auto& q:SubtractModelVolumes(t,volumes)){
            auto out=vertices[at];for(size_t k=0;k<3;++k){out.pos[k]=q.position[k];out.color[k]=q.normal[k];}result.push_back(out);
        }
    }
    return result;
#endif
}
}
