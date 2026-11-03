#pragma once
#include "gt2view/cockpit_fit.h"

namespace gt2view {
using HeadlinerTriangle=std::array<std::array<float,3>,3>;
// Keep every generated roof patch on the cabin side of the retained source
// roof. Constrain a whole patch against every source plane whose XZ footprint
// overlaps it; testing only grid vertices would miss an interior intersection.
inline void FitHeadlinerBelowSource(CockpitFit& fit,std::span<const HeadlinerTriangle> source) {
    if(fit.nativeInterior || fit.openTop || !fit.smoothShell)return;
    constexpr float clearance=.0005f;
    for(size_t z=0;z<9;++z)for(size_t x=0;x<13;++x){
        const float u=float(x)/12,v=float(z)/8;
        auto& p=fit.headliner[z][x];
        for(size_t k=0;k<3;++k){
            const float front=fit.frontHeader[0][k]*(1-u)+fit.frontHeader[1][k]*u;
            const float rear=fit.rearHeader[0][k]*(1-u)+fit.rearHeader[1][k]*u;
            p[k]=front*(1-v)+rear*v;
        }
        p[1]+=.035f*std::sin(3.14159265358979323846f*u)*std::sin(3.14159265358979323846f*v);
    }
    // Use finite authored roof patches for rendering. Extending a small
    // sloping source plane across a whole grid cell can create an invented
    // hanging wedge. Clip each real triangle to the fitted cabin footprint,
    // retaining its exact plane and window boundary.
    fit.headlinerSurface.clear();
    const std::array<std::array<float,3>,4> footprint{fit.frontHeader[0],fit.frontHeader[1],fit.rearHeader[1],fit.rearHeader[0]};
    double signedArea=0;for(size_t j=0;j<4;++j)signedArea+=double(footprint[j][0])*footprint[(j+1)%4][2]-double(footprint[(j+1)%4][0])*footprint[j][2];
    const double footprintSign=signedArea>=0?1.:-1.;
    for(const auto& triangle:source) {
        if(std::any_of(triangle.begin(),triangle.end(),[&](const auto& q){return q[1]<=fit.eye[1]+.05f;}))continue;
        std::array<double,3> u{},w{};
        for(size_t k=0;k<3;++k){u[k]=double(triangle[1][k])-triangle[0][k];w[k]=double(triangle[2][k])-triangle[0][k];}
        const std::array<double,3> n{u[1]*w[2]-u[2]*w[1],u[2]*w[0]-u[0]*w[2],u[0]*w[1]-u[1]*w[0]};
        const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if(length<1e-12 || std::abs(n[1])<length*.65 || std::abs(signedArea)<1e-12)continue;
        std::vector<std::array<float,3>> polygon(triangle.begin(),triangle.end());
        for(size_t edge=0;edge<4 && !polygon.empty();++edge) {
            const auto& a=footprint[edge];const auto& b=footprint[(edge+1)%4];
            const auto distance=[&](const auto& q){return footprintSign*((double(b[0])-a[0])*(q[2]-a[2])-(double(b[2])-a[2])*(q[0]-a[0]));};
            std::vector<std::array<float,3>> next;auto previous=polygon.back();double pd=distance(previous);
            for(const auto& q:polygon){const double d=distance(q);
                if((d>=0)!=(pd>=0)){const double along=pd/(pd-d);std::array<float,3> cut{};for(size_t k=0;k<3;++k)cut[k]=float(previous[k]+along*(double(q[k])-previous[k]));next.push_back(cut);}
                if(d>=0)next.push_back(q);previous=q;pd=d;
            }
            polygon=std::move(next);
        }
        // A finite opaque source patch already defines its exact window
        // boundary. Translating it downward moves that boundary into glass,
        // especially on a sloping canopy. Keep the authored plane here;
        // clearance remains necessary only for the fallback inferred grid.
        for(size_t k=1;k+1<polygon.size();++k)fit.headlinerSurface.push_back({polygon[0],polygon[k],polygon[k+1]});
    }
    // Header estimates can extend beyond the actual roof on a tapered cabin.
    // Do not extrapolate a short sloping roof face across that empty side strip:
    // doing so pulls the whole headliner down beside the driver's head.
    // Restrict each row to the source roof's measured XZ cross-section first.
    for(size_t row=0;row<9;++row) {
        const float z=(fit.headliner[row][0][2]+fit.headliner[row][12][2])*.5f;
        float left=std::numeric_limits<float>::infinity(),right=-left;
        for(const auto& t:source) {
            if(std::any_of(t.begin(),t.end(),[&](const auto& p){return p[1]<=fit.eye[1]+.05f;}))continue;
            std::array<double,3> edgeU{},edgeV{},n{};
            for(size_t k=0;k<3;++k){edgeU[k]=double(t[1][k])-t[0][k];edgeV[k]=double(t[2][k])-t[0][k];}
            n={edgeU[1]*edgeV[2]-edgeU[2]*edgeV[1],edgeU[2]*edgeV[0]-edgeU[0]*edgeV[2],edgeU[0]*edgeV[1]-edgeU[1]*edgeV[0]};
            const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
            if(length<1e-12 || std::abs(n[1])<length*.65)continue;
            for(size_t k=0;k<3;++k) {
                const auto& a=t[k];const auto& b=t[(k+1)%3];
                if(z<std::min(a[2],b[2])-1e-6f || z>std::max(a[2],b[2])+1e-6f)continue;
                if(std::abs(b[2]-a[2])<1e-7f){left=std::min({left,a[0],b[0]});right=std::max({right,a[0],b[0]});continue;}
                const float u=std::clamp((z-a[2])/(b[2]-a[2]),0.f,1.f),x=a[0]+u*(b[0]-a[0]);
                left=std::min(left,x);right=std::max(right,x);
            }
        }
        const float oldLeft=fit.headliner[row][0][0],oldRight=fit.headliner[row][12][0];
        // A narrow accessory or one-sided fragment cannot define a roof span.
        if(!(left<0 && right>0 && right-left>=(oldRight-oldLeft)*.5f))continue;
        left=std::max(left,oldLeft);right=std::min(right,oldRight);
        for(size_t col=0;col<13;++col)fit.headliner[row][col][0]=left+(right-left)*float(col)/12;
    }
    const auto original=fit.headliner;bool constrained=false;
    for(const auto& t:source){
        if(std::any_of(t.begin(),t.end(),[&](const auto& p){return p[1]<=fit.eye[1]+.05f;}))continue;
        std::array<double,3> a{},b{},n{};
        for(size_t k=0;k<3;++k){a[k]=double(t[1][k])-t[0][k];b[k]=double(t[2][k])-t[0][k];}
        n={a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        if(length<1e-12 || std::abs(n[1])<length*.65)continue;
        for(size_t z=0;z<8;++z)for(size_t x=0;x<12;++x){
            const std::array<std::array<float,3>,4> quad{original[z][x],original[z][x+1],original[z+1][x+1],original[z+1][x]};
            using Point=std::array<double,2>;std::vector<Point> polygon;
            for(const auto& p:t)polygon.push_back({p[0],p[2]});
            double winding=0;for(size_t j=0;j<4;++j)winding+=double(quad[j][0])*quad[(j+1)%4][2]-double(quad[(j+1)%4][0])*quad[j][2];
            if(std::abs(winding)<1e-12)continue;
            const double sign=winding>0?1.:-1.;
            for(size_t edge=0;edge<4 && !polygon.empty();++edge){
                const auto& q=quad[edge];const auto& r=quad[(edge+1)%4];
                auto distance=[&](const Point& p){return sign*((double(r[0])-q[0])*(p[1]-q[2])-(double(r[2])-q[2])*(p[0]-q[0]));};
                std::vector<Point> clipped;auto previous=polygon.back();double pd=distance(previous);
                for(const auto& p:polygon){const double d=distance(p);
                    if((d>=0)!=(pd>=0)){const double u=pd/(pd-d);clipped.push_back({previous[0]+u*(p[0]-previous[0]),previous[1]+u*(p[1]-previous[1])});}
                    if(d>=0)clipped.push_back(p);previous=p;pd=d;
                }
                polygon=std::move(clipped);
            }
            double area=0;for(size_t j=0;j<polygon.size();++j)area+=polygon[j][0]*polygon[(j+1)%polygon.size()][1]-polygon[(j+1)%polygon.size()][0]*polygon[j][1];
            if(std::abs(area)<1e-12)continue;
            for(size_t row:{z,z+1})for(size_t col:{x,x+1}){
                auto& p=fit.headliner[row][col];
                const double y=t[0][1]-(n[0]*(double(p[0])-t[0][0])+n[2]*(double(p[2])-t[0][2]))/n[1];
                p[1]=std::min(p[1],float(y-clearance));
            }
            constrained=true;
        }
    }
    fit.headlinerValid=constrained;
}
}
