#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

namespace gt2view {
// World-space upward rays, accelerated by a small XZ grid. Only opaque
// surfaces can hide the sky; the query is independent of camera/head rotation.
class PaintProbeCoverage {
public:
    using Point = std::array<float,3>;
    struct Triangle { Point a,b,c; };
    void Clear() { triangles_.clear(); cells_.clear(); }
    void Add(Point a,Point b,Point c) {
        const float determinant=(b[0]-a[0])*(c[2]-a[2])-(b[2]-a[2])*(c[0]-a[0]);
        if(std::abs(determinant)<.01f) return; // vertical walls / degenerate faces
        const int x0=Cell(std::min({a[0],b[0],c[0]})),x1=Cell(std::max({a[0],b[0],c[0]}));
        const int z0=Cell(std::min({a[2],b[2],c[2]})),z1=Cell(std::max({a[2],b[2],c[2]}));
        if(int64_t(x1-x0+1)*(z1-z0+1)>4096) return;
        const auto index=triangles_.size(); triangles_.push_back({a,b,c});
        for(int x=x0;x<=x1;++x)for(int z=z0;z<=z1;++z)cells_[{x,z}].push_back(index);
    }
    float RoofHeight(const Point& p) const {
        float nearest=1e30f;
        const auto found=cells_.find({Cell(p[0]),Cell(p[2])});
        if(found==cells_.end())return nearest;
        for(size_t index:found->second) {
            const auto& t=triangles_[index];
            const float ux=t.b[0]-t.a[0],uz=t.b[2]-t.a[2],vx=t.c[0]-t.a[0],vz=t.c[2]-t.a[2];
            const float dx=p[0]-t.a[0],dz=p[2]-t.a[2],d=ux*vz-uz*vx;
            const float u=(dx*vz-dz*vx)/d,v=(ux*dz-uz*dx)/d;
            if(u<-.0001f || v<-.0001f || u+v>1.0001f)continue;
            const float y=t.a[1]+u*(t.b[1]-t.a[1])+v*(t.c[1]-t.a[1]);
            if(y>p[1]+.15f && y<p[1]+30.f)nearest=std::min(nearest,y);
        }
        return nearest;
    }
    bool Covered(const Point& p) const { return RoofHeight(p)<1e29f; }
    const std::vector<Triangle>& Triangles() const {return triangles_;}
private:
    static int Cell(float v) {return int(std::floor(v/16.f));}
    std::vector<Triangle> triangles_;
    std::map<std::pair<int,int>,std::vector<size_t>> cells_;
};
}
