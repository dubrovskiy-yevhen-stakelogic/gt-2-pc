#pragma once
#include "gt2view/cockpit_body.h"
namespace structure_audit {
using Point=gt2view::cockpit_structure::Point;
using gt2view::cockpit_structure::Sub;
using gt2view::cockpit_structure::Dot;
using gt2view::cockpit_structure::Cross;
using gt2view::cockpit_structure::Length;
// Independent alpha-aware ray witness through the actual output mesh. The
// oracle probes physical seams and opaque source roof samples, not vertex counts.
inline bool Covers(Point eye,Point target,std::span<const gt2::CarMeshVertex> mesh,const gt2::CarTexture& texture,
                   const std::array<uint16_t,16>& glass,float tolerance){
    const auto direction=Sub(target,eye);const float distance=Length(direction);
    for(size_t i=0;i+2<mesh.size();i+=3){
        const auto a=gt2view::cockpit_structure::Position(mesh[i]),b=gt2view::cockpit_structure::Position(mesh[i+1]),c=gt2view::cockpit_structure::Position(mesh[i+2]);
        const auto u=Sub(b,a),v=Sub(c,a),h=Cross(direction,v);const float determinant=Dot(u,h);
        if(std::abs(determinant)<1e-9f)continue;const auto s=Sub(eye,a),q=Cross(s,u);
        const float beta=Dot(s,h)/determinant,gamma=Dot(direction,q)/determinant,t=Dot(v,q)/determinant;
        if(beta<-.00001f || gamma<-.00001f || beta+gamma>1.00001f || t<=0 || std::abs(t-1)*distance>tolerance)continue;
        if(!mesh[i].textured)return true;
        const auto palette=gt2view::CockpitSourcePalette(mesh[i].palette);if(palette>=16 || texture.paints.empty())continue;
        const float alpha=1-beta-gamma;
        const int x=std::clamp(int(mesh[i].texel[0]*alpha+mesh[i+1].texel[0]*beta+mesh[i+2].texel[0]*gamma),0,255);
        const int y=std::clamp(int(mesh[i].texel[1]*alpha+mesh[i+1].texel[1]*beta+mesh[i+2].texel[1]*gamma),0,223);
        const auto index=texture.indices[size_t(y)*256+size_t(x)];
        if(!texture.paints[0].cluts[palette][index])continue;
        if(mesh[i].palette>=gt2view::kCockpitGlazingPalette && mesh[i].palette<gt2view::kCockpitLiningPalette && (glass[palette]&(1u<<index)))continue;
        return true;
    }
    return false;
}
struct Result{size_t roofSamples=0,roofMissing=0,pillarSamples=0,pillarMissing=0,mountSamples=0,mountMissing=0;};
inline Result Inspect(const gt2::CarModel& model,const gt2::CarTexture& texture,const gt2view::CockpitFit& fit,
                      const gt2view::CockpitBody& expected,std::span<const gt2::CarMeshVertex> actual){
    Result result;gt2::CarMeshOptions options;options.wheels=false;const auto source=gt2::BuildCarMesh(model,options);
    std::vector<std::array<Point,3>> panes;
    for(size_t i=0;i<source.size();i+=3)if(gt2view::cockpit_detail::GlassCoverage(std::span(source).subspan(i,3),texture,expected.glassMasks)>.25f)
        panes.push_back({gt2view::cockpit_structure::Position(source[i]),gt2view::cockpit_structure::Position(source[i+1]),gt2view::cockpit_structure::Position(source[i+2])});
    for(size_t i=0;i<source.size();i+=3){const auto a=gt2view::cockpit_structure::Position(source[i]),b=gt2view::cockpit_structure::Position(source[i+1]),c=gt2view::cockpit_structure::Position(source[i+2]);
        const auto normal=Cross(Sub(b,a),Sub(c,a));const float n=Length(normal);
        // This witness covers near-horizontal overhead roof. Steep header/
        // windscreen transitions belong to the separate pillar/aperture checks.
        if(n<.0001f || std::abs(normal[1])<n*.9f)continue;
        for(const auto& weight:std::array<std::array<float,3>,4>{{{.34f,.33f,.33f},{.6f,.2f,.2f},{.2f,.6f,.2f},{.2f,.2f,.6f}}}){
            Point p{};for(size_t k=0;k<3;++k)p[k]=a[k]*weight[0]+b[k]*weight[1]+c[k]*weight[2];
            if(p[1]<fit.eye[1]+.10f || p[2]<fit.roofFrontZ+.015f || p[2]>fit.roofRearZ-.015f)continue;
            // A steep windscreen banner can overlap the fitted roof interval.
            // Its coincident decal/backing is not structural roof: independently
            // witnessed glass at this source point establishes the aperture.
            if(std::any_of(panes.begin(),panes.end(),[&](const auto& pane){
                return Length(Sub(p,gt2view::cockpit_structure::Closest(p,pane[0],pane[1],pane[2])))<.0001f;}))continue;
            if(source[i].textured){const int x=std::clamp(int(source[i].texel[0]*weight[0]+source[i+1].texel[0]*weight[1]+source[i+2].texel[0]*weight[2]),0,255);
                const int y=std::clamp(int(source[i].texel[1]*weight[0]+source[i+1].texel[1]*weight[1]+source[i+2].texel[1]*weight[2]),0,223);
                const auto index=texture.indices[size_t(y)*256+size_t(x)],palette=source[i].palette;
                if(texture.paints.empty() || !texture.paints[0].cluts[palette][index] || (expected.glassMasks[palette]&(1u<<index)))continue;}
            ++result.roofSamples;const bool missing=!Covers(fit.eye,p,actual,texture,expected.glassMasks,.002f);result.roofMissing+=missing;
            if(missing && std::getenv("GT2_AUDIT_STRUCTURE_DEBUG"))std::printf("roof gap triangle %zu point %.6f %.6f %.6f palette %u normal %.3f %.3f %.3f\n",i/3,p[0],p[1],p[2],source[i].palette,normal[0]/n,normal[1]/n,normal[2]/n);
        }}
    auto probe=[&](const auto& supports,size_t& samples,size_t& missing){for(const auto& support:supports)for(int j=1;j<20;++j){
        const auto p=gt2view::cockpit_structure::Mix(support.root,support.tip,float(j)/20.f);
        const auto axis=Sub(support.tip,support.root),view=Sub(p,fit.eye);
        const float sine=Length(Cross(axis,view))/std::max(1e-6f,Length(axis)*Length(view));
        // At a grazing angle the visible cylinder wall is farther along the
        // ray than its centre line. A fixed depth tolerance reports false gaps.
        const float tolerance=support.radius*1.25f/std::max(.08f,sine)+.001f;
        ++samples;missing+=!Covers(fit.eye,p,actual,texture,expected.glassMasks,tolerance);
    }};
    probe(expected.pillarSeams,result.pillarSamples,result.pillarMissing);
    probe(expected.mirrorSupports,result.mountSamples,result.mountMissing);
    return result;
}
}
