#pragma once
#include "gt2view/scene_assets.h"
namespace gt2view {
using Point=std::array<float,3>;
static std::vector<SceneVertex> Witness(const CockpitFit& f,bool reference=false) {
    std::vector<SceneVertex> v;
    auto quad=[&](Point a,Point b,Point c,Point d) {
        for(const auto& p:{a,b,c,a,c,d}) {SceneVertex x{};std::copy(p.begin(),p.end(),x.pos);x.color[0]=1;x.color[1]=1;x.color[2]=0;v.push_back(x);}
    };
    std::vector<Point> ring(f.cowl.begin(),f.cowl.end());
    for(size_t i=1;i<17;++i)ring.push_back(CockpitSideSillAt(f,1,f.frontCowl[1][2]+(f.rearCowl[1][2]-f.frontCowl[1][2])*float(i)/16));
    ring.push_back(f.rearCowl[0]);
    for(int i=15;i>=0;--i)ring.push_back(CockpitSideSillAt(f,0,f.frontCowl[0][2]+(f.rearCowl[0][2]-f.frontCowl[0][2])*float(i)/16));
    Point centre{0,f.floorY-(reference?0.f:.045f),(f.windshieldZ+f.rearWindowZ)*.5f};
    for(size_t i=0;i<ring.size();++i) {
        auto a=ring[i],b=ring[(i+1)%ring.size()];
        // Keep the witness below the window boundary, inset from its edge to
        // avoid classifying anti-aliased silhouette pixels as holes.
        if(!reference){a[1]-=.045f;b[1]-=.045f;}
        const float z=(a[2]+b[2])*.5f;
        const float dx=(a[0]+b[0])*.5f,dz=z-centre[2],length=std::sqrt(dx*dx+dz*dz);
        const float ox=(reference?0.f:.025f)*dx/std::max(length,.01f),oz=(reference?0.f:.025f)*dz/std::max(length,.01f);
        a[0]+=ox;a[2]+=oz;b[0]+=ox;b[2]+=oz;
        Point lowA{a[0],centre[1],a[2]},lowB{b[0],centre[1],b[2]};quad(lowA,lowB,b,a);
        for(const auto& p:{centre,lowB,lowA}){SceneVertex x{};std::copy(p.begin(),p.end(),x.pos);x.color[0]=x.color[1]=1;v.push_back(x);}
    }
    if(!f.openTop) {
        auto a=f.frontHeader[0],b=f.frontHeader[1],c=f.rearHeader[1],d=f.rearHeader[0];
        // A bowed headliner rises .035 m above its corner plane.
        for(auto* p:{&a,&b,&c,&d})(*p)[1]+=reference?0.f:.08f;
        quad(a,b,c,d);
    }
    if(reference)for(auto& x:v)std::fill_n(x.color,3,1.f);
    return v;
}
}
