#pragma once
#include "gt2view/cockpit_fit.h"
#include <map>
namespace gt2view {
// A curved side pane can extend ahead of the fitted side-belt endpoint.
// Refine the closed backing wherever its X/Z footprint enters real exposed
// side glazing, using that glazing's measured lower edge. No eye-dependent
// cuts or additional wall-clearance distances are involved.
inline std::vector<std::array<float,3>> FitBackingGlazingFootprint(
        const CockpitFit& fit,const std::vector<std::array<float,3>>& perimeter) {
    using Point=std::array<float,3>;
    if(perimeter.empty() || fit.liningApertures.empty())return perimeter;
    std::vector<gt2::CarMeshVertex> openings;
    for(const auto& triangle:fit.liningApertures) {
        std::array<gt2::CarMeshVertex,3> q{};
        for(size_t i=0;i<3;++i)std::copy(triangle[i].begin(),triangle[i].end(),q[i].pos);
        if(CockpitSideOpening(q,fit))openings.insert(openings.end(),q.begin(),q.end());
    }
    if(openings.empty())return perimeter;
    const std::vector<uint8_t> connected(openings.size()/3,1);
    std::vector<Point> result;
    std::array<std::map<float,std::array<float,4>>,2> bands;
    for(size_t segment=0;segment<perimeter.size();++segment) {
        const auto& a=perimeter[segment];const auto& b=perimeter[(segment+1)%perimeter.size()];
        struct Interval{double lo,hi;size_t side;};std::vector<Interval> intervals;
        std::vector<double> knots{0,1};
        for(size_t at=0;at+2<openings.size();at+=3) {
            const auto* p0=openings[at].pos;const auto* p1=openings[at+1].pos;const auto* p2=openings[at+2].pos;
            const double ux=double(p1[0])-p0[0],uz=double(p1[2])-p0[2],vx=double(p2[0])-p0[0],vz=double(p2[2])-p0[2];
            const double det=ux*vz-uz*vx;if(std::abs(det)<1e-12)continue;
            const auto weights=[&](const Point& p){const double x=double(p[0])-p0[0],z=double(p[2])-p0[2];
                const double u=(x*vz-z*vx)/det,w=(ux*z-uz*x)/det;return std::array<double,3>{1-u-w,u,w};};
            const auto wa=weights(a),wb=weights(b);double lo=0,hi=1;
            for(size_t k=0;k<3;++k){const double slope=wb[k]-wa[k];
                if(std::abs(slope)<1e-12){if(wa[k]<0){lo=1;hi=0;break;}}
                else if(slope>0)lo=std::max(lo,-wa[k]/slope);else hi=std::min(hi,-wa[k]/slope);}
            if(hi<lo)continue;
            intervals.push_back({lo,hi,size_t(p0[0]>=0)});knots.push_back(lo);knots.push_back(hi);
        }
        std::sort(knots.begin(),knots.end());knots.erase(std::unique(knots.begin(),knots.end(),[](double x,double y){return std::abs(x-y)<1e-8;}),knots.end());
        for(double t:knots) {
            Point q{};for(size_t k=0;k<3;++k)q[k]=a[k]+float(t)*(b[k]-a[k]);
            std::array<bool,2> needsBand{};
            for(const auto& interval:intervals)if(t>=interval.lo-1e-8 && t<=interval.hi+1e-8)needsBand[interval.side]=true;
            for(size_t side=0;side<2;++side)if(needsBand[side]){
                auto [it,inserted]=bands[side].try_emplace(q[2]);
                if(inserted)it->second=CockpitUpperSideApertureAt(fit,openings,side,q[2],connected);
                const auto& band=it->second;if(std::isfinite(band[3]))q[1]=std::min(q[1],band[1]);
            }
            // Shared endpoints can be constrained by either adjacent wall.
            if(!result.empty() && std::abs(result.back()[0]-q[0])<1e-6f && std::abs(result.back()[2]-q[2])<1e-6f)result.back()[1]=std::min(result.back()[1],q[1]);
            else result.push_back(q);
        }
    }
    if(result.size()>1 && std::abs(result.front()[0]-result.back()[0])<1e-6f && std::abs(result.front()[2]-result.back()[2])<1e-6f){result.front()[1]=std::min(result.front()[1],result.back()[1]);result.pop_back();}
    // Discard redundant UV subdivisions, preserving every geometric bend.
    std::vector<Point> compact;
    for(const auto& q:result){
        while(compact.size()>1){const auto& a=compact[compact.size()-2];const auto& b=compact.back();
            double length=0,dot=0;for(size_t k=0;k<3;++k){const double d=double(q[k])-a[k];length+=d*d;dot+=(double(b[k])-a[k])*d;}
            if(length<1e-20)break;const double t=dot/length;if(t<0 || t>1)break;
            double error=0;for(size_t k=0;k<3;++k){const double d=double(b[k])-a[k]-t*(double(q[k])-a[k]);error+=d*d;}
            if(error>1e-10)break;compact.pop_back();}
        compact.push_back(q);
    }
    return compact;
}
}
