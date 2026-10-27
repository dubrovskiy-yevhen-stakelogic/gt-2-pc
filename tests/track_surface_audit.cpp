// Owned-disc regression: rendered road height versus the actual contact solver.
#include "gt2view/course_surface.h"
#include "gt2view/scenery_visibility.h"
#include "gt2view/scenery_replacements.h"
#include "gt2view/scenery_road_cut.h"
#include "game/sim/ground.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2formats/json.h"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <map>

namespace {
struct Road {size_t chunk,polygon;std::array<std::array<float,3>,3> points;uint16_t page,clut;};
using Roads=std::map<std::array<int,2>,std::vector<Road>>;
bool Inside(const Road& r,double x,double z){const auto& a=r.points[0];const auto& b=r.points[1];const auto& c=r.points[2];const double dx=b[0]-a[0],dz=b[2]-a[2],ex=c[0]-a[0],ez=c[2]-a[2],det=dx*ez-dz*ex;if(std::abs(det)<1e-10)return false;const double u=((x-a[0])*ez-(z-a[2])*ex)/det,v=(dx*(z-a[2])-dz*(x-a[0]))/det;return u>=-1e-7 && v>=-1e-7 && u+v<=1.0000001;}
Roads Index(const gt2::Track& track) {
    Roads result;
    for(size_t ci=0;ci<track.chunks.size();++ci) {
        const auto& c=track.chunks[ci];std::vector<bool> physical(c.road.polygons.size(),false);
        for(const auto& cell:c.surfaceGrid.cells)for(auto pi:cell)if(pi<physical.size())physical[pi]=true;
        for(size_t pi=0;pi<physical.size();++pi)if(physical[pi]) {
            const auto& p=c.road.polygons[pi];if(!p.IsTextured())continue;
            const auto& uv=track.uvTable.at(p.uvIndex).nearSet;
            const auto points=gt2view::CoursePolygonPositions(c,p);
            constexpr size_t order[]{0,1,3,1,2,3};
            for(size_t t=0;t<(p.IsQuad()?6u:3u);t+=3) {
                Road r{ci,pi,{},uv.tpage,uv.clut};
                for(size_t k=0;k<3;++k)r.points[k]=points[p.IsQuad()?order[t+k]:k];
                auto lo=r.points[0],hi=lo;
                for(const auto& q:r.points)for(size_t k=0;k<3;++k){lo[k]=std::min(lo[k],q[k]);hi[k]=std::max(hi[k],q[k]);}
                for(int x=int(std::floor(lo[0]/32));x<=int(std::floor(hi[0]/32));++x)
                    for(int z=int(std::floor(lo[2]/32));z<=int(std::floor(hi[2]/32));++z)result[{x,z}].push_back(r);
            }
        }
    }
    return result;
}
}

int main(int argc,char** argv) {
    try {
        if(argc<3)throw std::runtime_error("disc output.json [course substring]");
        gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
        auto report=gt2::json::Value::Array();size_t failures=0;
        for(const auto& file:vol.Files()) {
            if(!file.path.ends_with(".tro.gz") && !file.path.ends_with(".tro"))continue;
            if(argc>3 && file.path.find(argv[3])==std::string::npos)continue;
            const auto track=gt2::ParseTrack(vol.Read(file));
            double worst=0;size_t tested=0,bad=0,worstChunk=0,worstPolygon=0,nonContact=0,coverageHoles=0,courseVertices=0,maxPolygonVertices=0;
            const auto roads=Index(track);const gt2view::SceneryDetailIndex detail(track);
            const gt2view::SceneryRoadCut roadCut(track);
            size_t raised=0,raisedDecor=0;double highest=0;size_t worstInstance=0,worstSceneryPolygon=0;
            std::vector<gt2::sim::SurfaceGrid> contacts(track.chunks.size());
            for(size_t ci=0;ci<track.chunks.size();++ci){const auto& grid=track.chunks[ci].surfaceGrid;auto& c=contacts[ci];c.originX=grid.originX;c.originZ=grid.originZ;c.shiftX=grid.shiftX;c.shiftZ=grid.shiftZ;std::copy(grid.cells.begin(),grid.cells.end(),std::begin(c.cells));}
            auto anomalies=gt2::json::Value::Array();
            auto contactErrors=gt2::json::Value::Array();
            auto largeMeshes=gt2::json::Value::Array();
            std::vector<bool> allDrawn(track.chunks.size(),true);
            for(size_t ii=0;ii<track.sceneryInstances.size();++ii) {
                const auto& inst=track.sceneryInstances[ii];
                const auto& lods=track.sceneryLods.at(inst.lodList);
                const int selected=gt2view::SceneryEntry(track,inst,UINT32_MAX,0,0,500,true);
                if(selected<0)continue;
                const auto& model=track.sceneryModels.at(lods[size_t(selected)].model);
                const auto m=gt2::SceneryInstanceMatrix(inst,model);
                const auto replaced=detail.Match(inst,model);
                for(size_t pi=0;pi<model.polygons.size();++pi) {
                    const auto& p=model.polygons[pi];
                    if(!p.IsTextured() || (p.primCode&2) || gt2view::DetailedReplacementVisible(replaced[pi],allDrawn))continue;
                    const auto cut=roadCut.Cut(inst,model,pi);
                    for(size_t t=0;t<cut.vertices.size();t+=3) {
                        std::array<std::array<double,3>,3> v{};
                        for(size_t n=0;n<3;++n) {
                            v[n]=cut.vertices[t+n].position;
                        }
                        const double nx=(v[1][1]-v[0][1])*(v[2][2]-v[0][2])-(v[1][2]-v[0][2])*(v[2][1]-v[0][1]);
                        const double ny=(v[1][2]-v[0][2])*(v[2][0]-v[0][0])-(v[1][0]-v[0][0])*(v[2][2]-v[0][2]);
                        const double nz=(v[1][0]-v[0][0])*(v[2][1]-v[0][1])-(v[1][1]-v[0][1])*(v[2][0]-v[0][0]);
                        const double normalLength=std::sqrt(nx*nx+ny*ny+nz*nz);
                        if(normalLength<.01 || std::abs(ny)/normalLength<.7)continue;
                        size_t hits=0;double maximum=0;
                        for(const auto& w:std::array<std::array<double,3>,4>{{{1./3,1./3,1./3},{.6,.2,.2},{.2,.6,.2},{.2,.2,.6}}}) {
                            std::array<double,3> pos{};for(size_t k=0;k<3;++k)pos[k]=v[0][k]*w[0]+v[1][k]*w[1]+v[2][k]*w[2];
                            const auto cell=roads.find({int(std::floor(pos[0]/32)),int(std::floor(pos[2]/32))});if(cell==roads.end())continue;
                            double closest=1e9;bool closestRendered=false;
                            for(const auto& r:cell->second) {
                                const auto& chunk=track.chunks[r.chunk];const int32_t query[]{int32_t(std::llround(pos[0]*65536)),0,int32_t(std::llround(pos[2]*65536))};
                                if(gt2::sim::FindRoadPolygonAt(chunk,contacts[r.chunk],query)!=int32_t(r.polygon))continue;
                                const double d=pos[1]-gt2::sim::InterpolateRoadHeight(chunk,chunk.road.polygons[r.polygon],query)/65536.;
                                if(std::abs(d)<std::abs(closest)-1e-8){closest=d;closestRendered=Inside(r,pos[0],pos[2]);}
                                else if(std::abs(d-closest)<1e-8)closestRendered|=Inside(r,pos[0],pos[2]);
                            }
                            if(closestRendered && closest>.08 && closest<3){++hits;maximum=std::max(maximum,closest);}
                        }
                        if(hits>=3){if(!cut.roadMaterial){++raisedDecor;continue;}++raised;if(maximum>highest){highest=maximum;worstInstance=ii;worstSceneryPolygon=pi;}
                            auto a=gt2::json::Value::Object();a.Set("instance",gt2::json::Value::Int(int64_t(ii)));a.Set("model",gt2::json::Value::Int(lods[size_t(selected)].model));
                            a.Set("polygon",gt2::json::Value::Int(int64_t(pi)));a.Set("lift",gt2::json::Value::Double(maximum));
                            a.Set("sortFarthest",gt2::json::Value::Bool(p.sortFarthest));a.Set("normalY",gt2::json::Value::Double(std::abs(ny)/normalLength));
                            a.Set("selectedLod",gt2::json::Value::Int(selected));a.Set("nearModel",gt2::json::Value::Int(lods.front().model));
                            a.Set("nearEmpty",gt2::json::Value::Bool(track.sceneryModels[lods.front().model].polygons.empty()));
                            auto vertices=gt2::json::Value::Array();for(const auto& q:v){auto point=gt2::json::Value::Array();for(double k:q)point.Push(gt2::json::Value::Double(k));vertices.Push(std::move(point));}
                            a.Set("vertices",std::move(vertices));anomalies.Push(std::move(a));}
                    }
                }
            }
            for(size_t ci=0;ci<track.chunks.size();++ci) {
                const auto& c=track.chunks[ci];
                gt2::sim::SurfaceGrid contact;
                contact.originX=c.surfaceGrid.originX;contact.originZ=c.surfaceGrid.originZ;
                contact.shiftX=c.surfaceGrid.shiftX;contact.shiftZ=c.surfaceGrid.shiftZ;
                std::copy(c.surfaceGrid.cells.begin(),c.surfaceGrid.cells.end(),std::begin(contact.cells));
                std::vector<bool> physical(c.road.polygons.size(),false);
                for(const auto& cell:c.surfaceGrid.cells)for(auto pi:cell)if(pi<physical.size())physical[pi]=true;
                for(size_t pi=0;pi<c.road.polygons.size();++pi) {
                    if(!physical[pi])continue; // walls and scenery do not carry wheel contacts
                    const auto& p=c.road.polygons[pi];const auto mesh=gt2view::CoursePolygonMesh(c,p,true);
                    courseVertices+=mesh.indices.size();maxPolygonVertices=std::max(maxPolygonVertices,mesh.indices.size());
                    if(mesh.indices.size()>1000){auto info=gt2::json::Value::Object();info.Set("chunk",gt2::json::Value::Int(int64_t(ci)));info.Set("polygon",gt2::json::Value::Int(int64_t(pi)));info.Set("count",gt2::json::Value::Int(int64_t(mesh.indices.size())));auto vs=gt2::json::Value::Array();for(const auto& p0:gt2view::CoursePolygonPositions(c,p)){auto v=gt2::json::Value::Array();for(float x:p0)v.Push(gt2::json::Value::Double(x));vs.Push(std::move(v));}info.Set("vertices",std::move(vs));largeMeshes.Push(std::move(info));}
                    const auto& points=mesh.positions;const auto indices=mesh.indices;
                    for(size_t t=0;t<indices.size();t+=3) {
                        const auto& a=points[indices[t]];const auto& b=points[indices[t+1]];const auto& d=points[indices[t+2]];
                        const double area=std::abs((b[0]-a[0])*(d[2]-a[2])-(b[2]-a[2])*(d[0]-a[0]));
                        if(area<.01)continue;
                        for(const auto& weights:std::array<std::array<double,3>,4>{{{1./3,1./3,1./3},{.6,.2,.2},{.2,.6,.2},{.2,.2,.6}}}) {
                            std::array<double,3> position{};
                            for(size_t k=0;k<3;++k)position[k]=a[k]*weights[0]+b[k]*weights[1]+d[k]*weights[2];
                            const int32_t query[]{int32_t(std::llround(position[0]*65536)),0,int32_t(std::llround(position[2]*65536))};
                            // Overlapping triangles, shoulders and triangulation
                            // outside a concave polygon do not select this contact
                            // plane. Compare the plane a real wheel would select.
                            if(gt2::sim::FindRoadPolygonAt(c,contact,query)!=int32_t(pi)){++nonContact;continue;}
                            const double height=gt2::sim::InterpolateRoadHeight(c,p,query)/65536.;
                            const double error=std::abs(height-position[1]);++tested;
                            if(error>.08){++bad; // five local height units, far below a tyre's radius
                                auto e=gt2::json::Value::Object();e.Set("chunk",gt2::json::Value::Int(int64_t(ci)));e.Set("polygon",gt2::json::Value::Int(int64_t(pi)));
                                e.Set("quad",gt2::json::Value::Bool(p.IsQuad()));e.Set("fan",gt2::json::Value::Bool(mesh.contactFan));
                                auto pos=gt2::json::Value::Array();for(double k:position)pos.Push(gt2::json::Value::Double(k));e.Set("position",std::move(pos));
                                e.Set("physicalHeight",gt2::json::Value::Double(height));auto verts=gt2::json::Value::Array();
                                for(size_t n=0;n<(p.IsQuad()?4u:3u);++n){auto v=gt2::json::Value::Array();for(float k:points[n])v.Push(gt2::json::Value::Double(k));verts.Push(std::move(v));}
                                e.Set("vertices",std::move(verts));contactErrors.Push(std::move(e));
                            }
                            if(error>worst){worst=error;worstChunk=ci;worstPolygon=pi;}
                        }
                    }
                    // Independent probes are generated in the original footprint,
                    // rather than only inside the new mesh's emitted triangles.
                    // This catches holes and the tiny quantised kerb sectors too.
                    if(p.IsQuad()){const auto source=gt2view::CoursePolygonPositions(c,p);const auto order=gt2view::CoursePolygonTriangles(p.IsQuad());
                        for(size_t t=0;t<order.size();t+=3){const auto& sa=source[order[t]];const auto& sb=source[order[t+1]];const auto& sc=source[order[t+2]];if(std::abs((sb[0]-sa[0])*(sc[2]-sa[2])-(sb[2]-sa[2])*(sc[0]-sa[0]))<1e-8)continue;
                        for(unsigned n=1;n<=23;++n){const double u=std::sqrt(std::fmod(n*.754877666,1.)),v=std::fmod(n*.569840296,1.);const double w[]{1-u,u*(1-v),u*v};std::array<double,3> pos{};for(size_t i=0;i<3;++i)for(size_t k=0;k<3;++k)pos[k]+=source[order[t+i]][k]*w[i];
                            const int32_t query[]{int32_t(std::llround(pos[0]*65536)),0,int32_t(std::llround(pos[2]*65536))};if(gt2::sim::FindRoadPolygonAt(c,contact,query)!=int32_t(pi))continue;
                            const double height=gt2::sim::InterpolateRoadHeight(c,p,query)/65536.;bool found=false;double rendered=0;
                            for(size_t k=0;k<indices.size();k+=3){const auto& a=points[indices[k]];const auto& b=points[indices[k+1]];const auto& d=points[indices[k+2]];const double dx=b[0]-a[0],dz=b[2]-a[2],ex=d[0]-a[0],ez=d[2]-a[2],det=dx*ez-dz*ex;if(std::abs(det)<1e-10)continue;
                                const double bu=((pos[0]-a[0])*ez-(pos[2]-a[2])*ex)/det,bv=(dx*(pos[2]-a[2])-dz*(pos[0]-a[0]))/det;if(bu< -1e-7 || bv< -1e-7 || bu+bv>1.0000001)continue;rendered=a[1]+bu*(b[1]-a[1])+bv*(d[1]-a[1]);found=true;break;
                            }
                            ++tested;if(!found){++coverageHoles;++bad;continue;}const double error=std::abs(height-rendered);if(error>.08){++bad;
                                auto e=gt2::json::Value::Object();e.Set("chunk",gt2::json::Value::Int(int64_t(ci)));e.Set("polygon",gt2::json::Value::Int(int64_t(pi)));e.Set("fan",gt2::json::Value::Bool(mesh.contactFan));e.Set("independent",gt2::json::Value::Bool(true));
                                auto location=gt2::json::Value::Array();for(double value:pos)location.Push(gt2::json::Value::Double(value));e.Set("position",std::move(location));e.Set("physicalHeight",gt2::json::Value::Double(height));e.Set("renderedHeight",gt2::json::Value::Double(rendered));
                                auto vertices=gt2::json::Value::Array();for(const auto& corner:source){auto vertex=gt2::json::Value::Array();for(float value:corner)vertex.Push(gt2::json::Value::Double(value));vertices.Push(std::move(vertex));}e.Set("vertices",std::move(vertices));contactErrors.Push(std::move(e));
                            }if(error>worst){worst=error;worstChunk=ci;worstPolygon=pi;}
                        }}
                    }
                }
            }
            auto row=gt2::json::Value::Object();
            row.Set("track",gt2::json::Value::String(file.path));row.Set("samples",gt2::json::Value::Int(int64_t(tested)));
            row.Set("failedSamples",gt2::json::Value::Int(int64_t(bad)));row.Set("maxErrorMetres",gt2::json::Value::Double(worst));
            row.Set("nonContactSamples",gt2::json::Value::Int(int64_t(nonContact)));
            row.Set("worstChunk",gt2::json::Value::Int(int64_t(worstChunk)));row.Set("worstPolygon",gt2::json::Value::Int(int64_t(worstPolygon)));
            row.Set("raisedSceneryTriangles",gt2::json::Value::Int(int64_t(raised)));row.Set("maxSceneryLiftMetres",gt2::json::Value::Double(highest));
            row.Set("raisedDecorationTriangles",gt2::json::Value::Int(int64_t(raisedDecor)));
            row.Set("worstInstance",gt2::json::Value::Int(int64_t(worstInstance)));row.Set("worstSceneryPolygon",gt2::json::Value::Int(int64_t(worstSceneryPolygon)));
            row.Set("anomalies",std::move(anomalies));
            row.Set("contactErrors",std::move(contactErrors));
            row.Set("coverageHoles",gt2::json::Value::Int(int64_t(coverageHoles)));row.Set("contactVertices",gt2::json::Value::Int(int64_t(courseVertices)));row.Set("maxPolygonVertices",gt2::json::Value::Int(int64_t(maxPolygonVertices)));
            row.Set("largeMeshes",std::move(largeMeshes));
            if(argc>4){
                auto geometry=gt2::json::Value::Array();
                auto emit=[&](const auto& vertices,const auto& polygons,const auto& world,int64_t instance,int64_t model,bool nearEmpty){
                    auto item=gt2::json::Value::Object();item.Set("instance",gt2::json::Value::Int(instance));item.Set("model",gt2::json::Value::Int(model));item.Set("nearEmpty",gt2::json::Value::Bool(nearEmpty));
                    auto vs=gt2::json::Value::Array();for(const auto& v:vertices){auto point=gt2::json::Value::Array();for(float k:world(v))point.Push(gt2::json::Value::Double(k));vs.Push(std::move(point));}item.Set("vertices",std::move(vs));
                    auto ps=gt2::json::Value::Array(),materials=gt2::json::Value::Array();for(const auto& p:polygons){auto poly=gt2::json::Value::Array();for(size_t k=0;k<(p.IsQuad()?4u:3u);++k)poly.Push(gt2::json::Value::Int(p.vertex[k]));ps.Push(std::move(poly));
                        auto mat=gt2::json::Value::Array();if(p.IsTextured()){
                            if constexpr(requires{p.tpage;}){mat.Push(gt2::json::Value::Int(p.tpage));mat.Push(gt2::json::Value::Int(p.clut));for(size_t k=0;k<4;++k){mat.Push(gt2::json::Value::Int(p.u[k]));mat.Push(gt2::json::Value::Int(p.v[k]));}}
                            else for(const auto& uv:{track.uvTable[p.uvIndex].nearSet,track.uvTable[p.uvIndex].farSet}){mat.Push(gt2::json::Value::Int(uv.tpage));mat.Push(gt2::json::Value::Int(uv.clut));for(size_t k=0;k<4;++k){mat.Push(gt2::json::Value::Int(uv.u[k]));mat.Push(gt2::json::Value::Int(uv.v[k]));}}
                        }materials.Push(std::move(mat));}item.Set("polygons",std::move(ps));item.Set("materials",std::move(materials));geometry.Push(std::move(item));};
                for(size_t ci=0;ci<track.chunks.size();++ci){const auto& c=track.chunks[ci];emit(c.road.vertices,c.road.polygons,[&](const auto& v){return gt2::TrackVertexToWorld(c,v);},-1,int64_t(ci),false);}
                for(size_t ii=0;ii<track.sceneryInstances.size();++ii){const auto& inst=track.sceneryInstances[ii];const int selected=gt2view::SceneryEntry(track,inst,UINT32_MAX,0,0,500,true);if(selected<0)continue;const auto& lods=track.sceneryLods[inst.lodList];const auto& model=track.sceneryModels[lods[size_t(selected)].model];const auto m=gt2::SceneryInstanceMatrix(inst,model);
                    emit(model.vertices,model.polygons,[&](const auto& v){std::array<float,3> w{};for(size_t k=0;k<3;++k)w[k]=m[k]*v.x+m[4+k]*v.y+m[8+k]*v.z+m[12+k];return w;},int64_t(ii),lods[size_t(selected)].model,track.sceneryModels[lods.front().model].polygons.empty());}
                row.Set("geometry",std::move(geometry));
            }
            report.Push(std::move(row));if(bad || raised)++failures;
            std::cout<<file.path<<" samples="<<tested<<" failed="<<bad<<" max="<<worst<<" raised="<<raised<<" lift="<<highest<<" instance="<<worstInstance<<" polygon="<<worstSceneryPolygon<<std::endl;
        }
        if(report.Size()==0)throw std::runtime_error("No tracks examined");
        gt2::json::WriteFile(argv[2],report);return failures?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}
}
