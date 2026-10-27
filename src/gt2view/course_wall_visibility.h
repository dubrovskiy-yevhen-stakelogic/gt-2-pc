#pragma once
#include "gt2view/course_surface.h"
#include <map>

namespace gt2view {
// Some retail chunks contain walls for a lower loop which intersect the
// contact surface of another loop. Their authored render lists never expose
// both. Extending distance must not resurrect those walls from an unlisted
// chunk. Keep the original mesh whenever its chunk is explicitly visible.
class CourseWallVisibility {
    struct Road { std::array<std::array<float,3>,3> p; size_t chunk; };
    struct Vertex { std::array<double,3> p; };
    std::vector<Road> roads_;
    std::map<std::array<int,2>,std::vector<size_t>> cells_;
    std::vector<std::vector<bool>> visible_;
    static double Area(const auto& a,const auto& b,const auto& c) {
        return (b[0]-a[0])*(c[2]-a[2])-(b[2]-a[2])*(c[0]-a[0]);
    }
    static std::vector<Vertex> Clip(const std::vector<Vertex>& input,const auto& distance) {
        std::vector<Vertex> out;
        for(size_t i=0;i<input.size();++i) {
            const auto& a=input[i];const auto& b=input[(i+1)%input.size()];
            const double da=distance(a.p),db=distance(b.p);
            if(da>=-1e-8)out.push_back(a);
            if((da>1e-8 && db< -1e-8)||(da< -1e-8 && db>1e-8)) {
                Vertex v{};const double t=da/(da-db);
                for(size_t k=0;k<3;++k)v.p[k]=a.p[k]+t*(b.p[k]-a.p[k]);
                out.push_back(v);
            }
        }
        return out;
    }
public:
    std::vector<bool> Classify(const gt2::Track& track,size_t ci,std::vector<size_t>* conflicts=nullptr)const {
        const auto& chunk=track.chunks[ci];const auto& polygons=chunk.road.polygons;
        std::vector<bool> masked(polygons.size(),false),physical(polygons.size(),false);
        for(const auto& cell:chunk.surfaceGrid.cells)for(auto pi:cell)if(pi<physical.size())physical[pi]=true;
        for(size_t pi=0;pi<polygons.size();++pi) {
            const auto& p=polygons[pi];masked[pi]=!(p.primCode&2) && HiddenIntersection(ci,CoursePolygonPositions(chunk,p),p.IsQuad(),physical[pi],conflicts);
        }
        // Follow the complete connected vertical wall, including alpha-tested
        // railings above the opaque base. Material changes and a T junction at
        // one vertex do not split an authored fence into independent objects.
        // Contact surfaces and horizontal decks never enter the component.
        auto normal=[&](const gt2::TrackPolygon& p){const auto q=CoursePolygonPositions(chunk,p);std::array<double,3> n{
            (q[1][1]-q[0][1])*(q[2][2]-q[0][2])-(q[1][2]-q[0][2])*(q[2][1]-q[0][1]),
            (q[1][2]-q[0][2])*(q[2][0]-q[0][0])-(q[1][0]-q[0][0])*(q[2][2]-q[0][2]),
            (q[1][0]-q[0][0])*(q[2][1]-q[0][1])-(q[1][1]-q[0][1])*(q[2][0]-q[0][0])};
            const double length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);if(length>1e-9)for(auto& v:n)v/=length;return n;};
        std::map<std::array<float,3>,std::vector<size_t>> vertices;
        for(size_t pi=0;pi<polygons.size();++pi) {
            const auto& p=polygons[pi];if(physical[pi] || (p.primCode&2))continue;
            const auto n=normal(p);if(std::abs(n[1])>.25)continue;
            const auto points=CoursePolygonPositions(chunk,p);
            for(size_t v=0;v<(p.IsQuad()?4u:3u);++v)vertices[points[v]].push_back(pi);
        }
        std::vector<std::vector<size_t>> neighbours(polygons.size());
        for(const auto& [vertex,faces]:vertices){(void)vertex;for(auto a:faces)for(auto b:faces)if(a!=b) {
            const auto na=normal(polygons[a]),nb=normal(polygons[b]);
            if(std::abs(na[0]*nb[0]+na[1]*nb[1]+na[2]*nb[2])>.995)neighbours[a].push_back(b);
        }}
        std::vector<size_t> pending;for(size_t pi=0;pi<masked.size();++pi)if(masked[pi])pending.push_back(pi);
        while(!pending.empty()){const auto pi=pending.back();pending.pop_back();for(auto next:neighbours[pi])if(!masked[next]){masked[next]=true;pending.push_back(next);}}
        if(conflicts){std::sort(conflicts->begin(),conflicts->end());conflicts->erase(std::unique(conflicts->begin(),conflicts->end()),conflicts->end());}
        return masked;
    }
    explicit CourseWallVisibility(const gt2::Track& track) {
        visible_.resize(track.chunks.size(),std::vector<bool>(track.chunks.size(),false));
        for(size_t ci=0;ci<track.chunks.size();++ci) {
            const auto& c=track.chunks[ci];
            if(c.renderList.empty())std::fill(visible_[ci].begin(),visible_[ci].end(),true);
            for(auto e:c.renderList)if((e&0x3fff)<track.chunks.size() && (e>>14)<=2)visible_[ci][e&0x3fff]=true;
            visible_[ci][ci]=true;
            std::vector<bool> physical(c.road.polygons.size(),false);
            for(const auto& cell:c.surfaceGrid.cells)for(auto pi:cell)if(pi<physical.size())physical[pi]=true;
            for(size_t pi=0;pi<physical.size();++pi)if(physical[pi]) {
                const auto& polygon=c.road.polygons[pi];
                if(polygon.primCode&2)continue;
                const auto points=CoursePolygonPositions(c,polygon);
                const auto order=CoursePolygonTriangles(polygon.IsQuad());
                for(size_t t=0;t<order.size();t+=3) {
                    Road r{{points[order[t]],points[order[t+1]],points[order[t+2]]},ci};
                    if(std::abs(Area(r.p[0],r.p[1],r.p[2]))<.01)continue;
                    auto lo=r.p[0],hi=lo;
                    for(const auto& p:r.p)for(size_t k:{size_t(0),size_t(2)}){lo[k]=std::min(lo[k],p[k]);hi[k]=std::max(hi[k],p[k]);}
                    const auto index=roads_.size();roads_.push_back(r);
                    for(int x=int(std::floor(lo[0]/32));x<=int(std::floor(hi[0]/32));++x)
                        for(int z=int(std::floor(lo[2]/32));z<=int(std::floor(hi[2]/32));++z)cells_[{x,z}].push_back(index);
                }
            }
        }
    }
    bool HiddenIntersection(size_t source,const std::array<std::array<float,3>,4>& points,bool quad,bool physical,std::vector<size_t>* conflicts=nullptr)const {
        if(physical)return false;
        bool found=false;
        const auto order=CoursePolygonTriangles(quad);
        for(size_t t=0;t<order.size();t+=3) {
            const auto& a=points[order[t]];const auto& b=points[order[t+1]];const auto& c=points[order[t+2]];
            const double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
            const double ny=Area(a,b,c),nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
            // Walls only; sloping road shoulders, terrain and horizontal bridge
            // decks keep their existing render/physics reconstruction.
            if(nx*nx+ny*ny+nz*nz<.01 || ny*ny>.0625*(nx*nx+ny*ny+nz*nz))continue;
            auto lo=a,hi=a;for(const auto* p:{&b,&c})for(size_t k:{size_t(0),size_t(2)}){lo[k]=std::min(lo[k],(*p)[k]);hi[k]=std::max(hi[k],(*p)[k]);}
            std::vector<size_t> candidates;
            for(int x=int(std::floor(lo[0]/32));x<=int(std::floor(hi[0]/32));++x)
                for(int z=int(std::floor(lo[2]/32));z<=int(std::floor(hi[2]/32));++z) {
                    const auto it=cells_.find({x,z});if(it!=cells_.end())candidates.insert(candidates.end(),it->second.begin(),it->second.end());
                }
            std::sort(candidates.begin(),candidates.end());candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
            for(auto ri:candidates) {
                const auto& road=roads_[ri];if(visible_[road.chunk][source])continue;
                std::vector<Vertex> overlap;
                for(const auto* p:{&a,&b,&c})overlap.push_back({{(*p)[0],(*p)[1],(*p)[2]}});
                const double area=Area(road.p[0],road.p[1],road.p[2]),sign=area>0?1:-1;
                for(size_t e=0;e<3 && !overlap.empty();++e)
                    overlap=Clip(overlap,[&](const auto& p){return sign*Area(road.p[e],road.p[(e+1)%3],p);});
                if(overlap.size()<3)continue;
                double low=1e30,high=-1e30;
                for(const auto& vertex:overlap) {
                    const auto& p=vertex.p;const auto& r=road.p;
                    const double u=((p[0]-r[0][0])*(r[2][2]-r[0][2])-(p[2]-r[0][2])*(r[2][0]-r[0][0]))/area;
                    const double v=((r[1][0]-r[0][0])*(p[2]-r[0][2])-(r[1][2]-r[0][2])*(p[0]-r[0][0]))/area;
                    const double lift=p[1]-(r[0][1]+u*(r[1][1]-r[0][1])+v*(r[2][1]-r[0][1]));
                    low=std::min(low,lift);high=std::max(high,lift);
                }
                // Require a crossing, not a touching edge or an overhead wall.
                if(low<-.05 && high>.05){found=true;if(conflicts)conflicts->push_back(road.chunk);}
            }
        }
        return found;
    }
};
}
