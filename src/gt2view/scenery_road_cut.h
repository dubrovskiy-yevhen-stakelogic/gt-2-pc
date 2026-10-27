#pragma once
#include "gt2view/course_surface.h"
#include "game/sim/ground.h"
#include <algorithm>
#include <map>

namespace gt2view {
// Remove only the overlap of a coarse road/terrain texture with its detailed
// contact surface. Keep the rest of the scenery polygon (hills and shoulders),
// and retain the original polygon whenever the required chunks are not drawn.
class SceneryRoadCut {
public:
    struct Vertex {std::array<double,3> position{};std::array<double,4> weights{};};
    struct Result {std::vector<Vertex> vertices;std::vector<uint16_t> chunks;bool roadMaterial=false;};
private:
    struct Material {uint16_t page,clut;uint8_t u0,u1,v0,v1;};
    struct Road {std::array<std::array<float,3>,3> p;uint16_t chunk,polygon;Material nearMaterial,farMaterial;};
    const gt2::Track& track_;
    std::vector<gt2::sim::SurfaceGrid> contacts_;
    std::vector<Road> roads_;
    std::vector<Material> groundMaterials_;
    std::map<std::array<int,2>,std::vector<size_t>> cells_;
    static Material Mat(uint16_t page,uint16_t clut,const auto& u,const auto& v,size_t count){
        const auto ub=std::minmax_element(u.begin(),u.begin()+count),vb=std::minmax_element(v.begin(),v.begin()+count);
        return {uint16_t(page&0x19f),clut,*ub.first,*ub.second,*vb.first,*vb.second};
    }
    static bool Same(Material a,Material b){return a.page==b.page && a.clut==b.clut && std::max(a.u0,b.u0)<=std::min(a.u1,b.u1) && std::max(a.v0,b.v0)<=std::min(a.v1,b.v1);}
    static double Area(const auto& a,const auto& b,const auto& c){return (b[0]-a[0])*(c[2]-a[2])-(b[2]-a[2])*(c[0]-a[0]);}
    static double Height(const Road& r,double x,double z){const auto& a=r.p[0];const auto& b=r.p[1];const auto& c=r.p[2];const double d=Area(a,b,c);
        return a[1]+(((x-a[0])*(c[2]-a[2])-(z-a[2])*(c[0]-a[0]))*(b[1]-a[1])+((b[0]-a[0])*(z-a[2])-(b[2]-a[2])*(x-a[0]))*(c[1]-a[1]))/d;
    }
    static bool Inside(const Road& r,double x,double z){const std::array<double,3> p{x,0,z};const double sign=Area(r.p[0],r.p[1],r.p[2]);for(size_t i=0;i<3;++i)if(Area(r.p[i],r.p[(i+1)%3],p)*sign<-.0001)return false;return true;}
    static std::vector<Vertex> Clip(const std::vector<Vertex>& input,const auto& distance,double keep){std::vector<Vertex> out;
        for(size_t i=0;i<input.size();++i){const auto& p=input[i];const auto& q=input[(i+1)%input.size()];const double a=keep*distance(p),b=keep*distance(q);
            if(a>=-1e-9)out.push_back(p);
            if((a>1e-9 && b< -1e-9)||(a< -1e-9 && b>1e-9)){const double t=a/(a-b);Vertex v;for(size_t k=0;k<3;++k)v.position[k]=p.position[k]+t*(q.position[k]-p.position[k]);for(size_t k=0;k<4;++k)v.weights[k]=p.weights[k]+t*(q.weights[k]-p.weights[k]);out.push_back(v);}
        }return out;
    }
public:
    explicit SceneryRoadCut(const gt2::Track& track):track_(track),contacts_(track.chunks.size()){
        for(size_t ci=0;ci<track.chunks.size();++ci){const auto& c=track.chunks[ci];std::vector<bool> contact(c.road.polygons.size());for(const auto& cell:c.surfaceGrid.cells)for(auto pi:cell)if(pi<contact.size())contact[pi]=true;
            const auto& grid=c.surfaceGrid;auto& actual=contacts_[ci];actual.originX=grid.originX;actual.originZ=grid.originZ;actual.shiftX=grid.shiftX;actual.shiftZ=grid.shiftZ;std::copy(grid.cells.begin(),grid.cells.end(),std::begin(actual.cells));
            for(size_t pi=0;pi<contact.size();++pi){const auto& p=c.road.polygons[pi];if(!contact[pi] || !p.IsTextured() || (p.primCode&2))continue;
                // Cutting needs the authored footprint, not the tiny triangles
                // of quantised kerbs. Contact height/priority is queried from
                // the real solver below; indexing kerb tessellation here would
                // multiply load-time clipping work without improving the mask.
                const auto positions=CoursePolygonPositions(c,p);const auto indices=CoursePolygonTriangles(p.IsQuad());const auto& uv=track.uvTable.at(p.uvIndex);
                groundMaterials_.push_back(Mat(uv.nearSet.tpage,uv.nearSet.clut,uv.nearSet.u,uv.nearSet.v,p.IsQuad()?4:3));groundMaterials_.push_back(Mat(uv.farSet.tpage,uv.farSet.clut,uv.farSet.u,uv.farSet.v,p.IsQuad()?4:3));
                for(size_t t=0;t<indices.size();t+=3){Road r{};r.chunk=uint16_t(ci);r.polygon=uint16_t(pi);for(size_t k=0;k<3;++k)r.p[k]=positions[indices[t+k]];
                    if(std::abs(Area(r.p[0],r.p[1],r.p[2]))<.001)continue;r.nearMaterial=Mat(uv.nearSet.tpage,uv.nearSet.clut,uv.nearSet.u,uv.nearSet.v,p.IsQuad()?4:3);r.farMaterial=Mat(uv.farSet.tpage,uv.farSet.clut,uv.farSet.u,uv.farSet.v,p.IsQuad()?4:3);
                    const auto index=roads_.size();roads_.push_back(r);auto lo=r.p[0],hi=lo;for(const auto& q:r.p)for(size_t k:{size_t(0),size_t(2)}){lo[k]=std::min(lo[k],q[k]);hi[k]=std::max(hi[k],q[k]);}
                    for(int x=int(std::floor(lo[0]/32));x<=int(std::floor(hi[0]/32));++x)for(int z=int(std::floor(lo[2]/32));z<=int(std::floor(hi[2]/32));++z)cells_[{x,z}].push_back(index);
                }
            }
        }
    }
    Result Cut(const gt2::TrackSceneryInstance& inst,const gt2::TrackSceneryModel& model,size_t pi)const{
        Result result;const auto& p=model.polygons.at(pi);const auto matrix=gt2::SceneryInstanceMatrix(inst,model);const auto order=CoursePolygonTriangles(p.IsQuad());
        std::array<Vertex,4> corners{};for(size_t i=0;i<(p.IsQuad()?4u:3u);++i){const auto& v=model.vertices[p.vertex[i]];for(size_t k=0;k<3;++k)corners[i].position[k]=matrix[k]*v.x+matrix[4+k]*v.y+matrix[8+k]*v.z+matrix[12+k];corners[i].weights[i]=1;}
        const bool eligible=p.sortFarthest && p.IsTextured() && !(p.primCode&2);const auto mat=Mat(p.tpage,p.clut,p.u,p.v,p.IsQuad()?4:3);
        result.roadMaterial=eligible && std::any_of(groundMaterials_.begin(),groundMaterials_.end(),[&](auto ground){return Same(mat,ground);});
        for(size_t t=0;t<order.size();t+=3){const std::array<Vertex,3> tri{corners[order[t]],corners[order[t+1]],corners[order[t+2]]};std::vector<std::vector<Vertex>> pieces{{tri.begin(),tri.end()}};
            const auto& a=tri[0].position;const auto& b=tri[1].position;const auto& c=tri[2].position;
            const double nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]),ny=Area(a,b,c),nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
            if(result.roadMaterial && ny*ny>.49*(nx*nx+ny*ny+nz*nz) && std::abs(ny)>.001){
                auto lo=a,hi=a;for(const auto& v:tri)for(size_t k:{size_t(0),size_t(2)}){lo[k]=std::min(lo[k],v.position[k]);hi[k]=std::max(hi[k],v.position[k]);}
                std::vector<size_t> candidates;for(int x=int(std::floor(lo[0]/32));x<=int(std::floor(hi[0]/32));++x)for(int z=int(std::floor(lo[2]/32));z<=int(std::floor(hi[2]/32));++z){const auto found=cells_.find({x,z});if(found!=cells_.end())candidates.insert(candidates.end(),found->second.begin(),found->second.end());}
                std::sort(candidates.begin(),candidates.end());candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
                for(auto ri:candidates){const auto& road=roads_[ri];
                    // First establish a real overlap. An overhead bridge whose
                    // own contact surface is closer must not be cut by a road below.
                    std::vector<Vertex> overlap{tri.begin(),tri.end()};const double sign=Area(road.p[0],road.p[1],road.p[2])>0?1:-1;
                    for(size_t e=0;e<3 && !overlap.empty();++e)overlap=Clip(overlap,[&](const Vertex& v){return sign*Area(road.p[e],road.p[(e+1)%3],v.position);},1);
                    if(overlap.size()<3)continue;
                    Vertex centre;for(const auto& v:overlap)for(size_t k=0;k<3;++k)centre.position[k]+=v.position[k]/overlap.size();
                    auto contactLift=[&](const Vertex& v){const int32_t query[]{int32_t(std::llround(v.position[0]*65536)),0,int32_t(std::llround(v.position[2]*65536))};double nearest=1e30;uint16_t previous=UINT16_MAX;
                        for(auto other:candidates){const auto ci=roads_[other].chunk;if(ci==previous)continue;previous=ci;const auto pi=gt2::sim::FindRoadPolygonAt(track_.chunks[ci],contacts_[ci],query);if(pi<0)continue;const double d=v.position[1]-gt2::sim::InterpolateRoadHeight(track_.chunks[ci],track_.chunks[ci].road.polygons[size_t(pi)],query)/65536.;if(std::abs(d)<std::abs(nearest))nearest=d;}return nearest;};
                    // A coarse copy only a few millimetres above the road still
                    // wins the floating-point depth test over a large screen
                    // area. Preserve exact contact decks, not a 2.5 cm band of
                    // visibly raised duplicates.
                    const double centreLift=contactLift(centre);bool replaces=centreLift>.001 && centreLift<3;
                    for(const auto& v:overlap){const double d=contactLift(v);replaces|=d>.002 && d<3;}
                    if(!replaces)continue;
                    std::vector<std::vector<Vertex>> kept;bool removed=false;
                    const double cutSign=ny>0?1:-1; // clipping preserves the source winding, including collinear boundary vertices
                    for(auto inside:pieces){std::vector<Vertex> intersection=inside;for(size_t e=0;e<overlap.size() && !intersection.empty();++e)intersection=Clip(intersection,[&](const Vertex& v){return cutSign*Area(overlap[e].position,overlap[(e+1)%overlap.size()].position,v.position);},1);
                        if(intersection.size()<3){kept.push_back(std::move(inside));continue;}
                        removed=true;for(size_t e=0;e<overlap.size() && !inside.empty();++e){auto distance=[&](const Vertex& v){return cutSign*Area(overlap[e].position,overlap[(e+1)%overlap.size()].position,v.position);};auto outside=Clip(inside,distance,-1);if(outside.size()>=3)kept.push_back(std::move(outside));inside=Clip(inside,distance,1);}
                    }
                    if(removed)result.chunks.push_back(road.chunk);pieces=std::move(kept);if(pieces.empty())break;
                }
            }
            for(const auto& piece:pieces)for(size_t i=1;i+1<piece.size();++i){if(std::abs(Area(piece[0].position,piece[i].position,piece[i+1].position))<1e-8)continue;result.vertices.push_back(piece[0]);result.vertices.push_back(piece[i]);result.vertices.push_back(piece[i+1]);}
        }
        std::sort(result.chunks.begin(),result.chunks.end());result.chunks.erase(std::unique(result.chunks.begin(),result.chunks.end()),result.chunks.end());return result;
    }
};
}
