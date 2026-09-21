#pragma once
#include "gt2formats/track.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

namespace gt2view {
// Distant scenery can contain coarse copies of the detailed course. Match their
// boundary vertices in world space, with only the two formats' quantisation error
// as tolerance. Object size and visibility distance are not classification rules.
class SceneryDetailIndex {
    struct Point { std::array<float,3> position; uint16_t chunk; };
    std::map<std::array<int,3>,std::vector<Point>> cells_;
    struct Triangle { std::array<float,3> a,b,c; uint16_t chunk; };
    std::vector<Triangle> triangles_;
    std::map<std::array<int,3>,std::vector<size_t>> triangleCells_;
    const gt2::Track& track_;
    static float SurfaceDistance(const std::array<float,3>& p,const Triangle& t) {
        auto sub=[](const auto& a,const auto& b){return std::array<float,3>{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
        auto dot=[](const auto& a,const auto& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
        const auto ab=sub(t.b,t.a),ac=sub(t.c,t.a),ap=sub(p,t.a);
        const float aa=dot(ab,ab),bb=dot(ac,ac),cross=dot(ab,ac),pa=dot(ap,ab),pb=dot(ap,ac);
        const float determinant=aa*bb-cross*cross;
        if(determinant>1e-12f) {
            const float u=(bb*pa-cross*pb)/determinant,v=(aa*pb-cross*pa)/determinant;
            if(u>=0 && v>=0 && u+v<=1) {
                const std::array<float,3> normal={ab[1]*ac[2]-ab[2]*ac[1],ab[2]*ac[0]-ab[0]*ac[2],ab[0]*ac[1]-ab[1]*ac[0]};
                const float d=dot(ap,normal);
                return d*d/dot(normal,normal);
            }
        }
        float best=1e30f;
        const std::array<float,3> points[]={t.a,t.b,t.c};
        for(size_t i=0;i<3;++i) {
            const auto edge=sub(points[(i+1)%3],points[i]),offset=sub(p,points[i]);
            const float along=std::clamp(dot(offset,edge)/std::max(dot(edge,edge),1e-12f),0.0f,1.0f);
            const std::array<float,3> delta={offset[0]-along*edge[0],offset[1]-along*edge[1],offset[2]-along*edge[2]};
            best=std::min(best,dot(delta,delta));
        }
        return best;
    }
    static std::array<int,3> Cell(const std::array<float,3>& p) {
        return {int(std::floor(p[0]/8)),int(std::floor(p[1]/8)),int(std::floor(p[2]/8))};
    }
public:
    explicit SceneryDetailIndex(const gt2::Track& track) : track_(track) {
        for(size_t ci=0;ci<track.chunks.size();++ci) {
            const auto& c=track.chunks[ci];
            std::vector<bool> used(c.road.vertices.size(),false);
            for(const auto& p:c.road.polygons) for(size_t k=0;k<(p.IsQuad()?4u:3u);++k) used[p.vertex[k]]=true;
            for(const auto& polygon:c.road.polygons) {
                constexpr size_t corners[]={0,1,3,1,2,3};
                for(size_t k=0;k<(polygon.IsQuad()?6u:3u);k+=3) {
                    std::array<float,3> points[3];
                    for(size_t n=0;n<3;++n) points[n]=gt2::TrackVertexToWorld(c,c.road.vertices[polygon.vertex[polygon.IsQuad()?corners[k+n]:n]]);
                    const size_t index=triangles_.size();
                    triangles_.push_back({points[0],points[1],points[2],uint16_t(ci)});
                    auto lo=points[0],hi=points[0];
                    for(size_t n=1;n<3;++n) for(size_t axis=0;axis<3;++axis) {lo[axis]=std::min(lo[axis],points[n][axis]);hi[axis]=std::max(hi[axis],points[n][axis]);}
                    const auto low=Cell(lo),high=Cell(hi);
                    for(int x=low[0];x<=high[0];++x) for(int y=low[1];y<=high[1];++y) for(int z=low[2];z<=high[2];++z)
                        triangleCells_[{x,y,z}].push_back(index);
                }
            }
            for(size_t vi=0;vi<used.size();++vi) if(used[vi]) {
                const auto p=gt2::TrackVertexToWorld(c,c.road.vertices[vi]);
                cells_[Cell(p)].push_back({p,uint16_t(ci)});
            }
        }
    }
    std::vector<std::vector<uint16_t>> Match(const gt2::TrackSceneryInstance& instance,
                                            const gt2::TrackSceneryModel& model,
                                            std::vector<std::array<float,3>>* joinedVertices=nullptr) const {
        const auto matrix=gt2::SceneryInstanceMatrix(instance,model);
        const float scale=float(std::max({std::abs(int(instance.scale[0])),std::abs(int(instance.scale[1])),std::abs(int(instance.scale[2]))}))/4096;
        const float tolerance=1.01f*std::sqrt(3.0f)*(float(gt2::SceneryMetresPerUnit(model))*scale+1.0f/64);
        std::array<float,3> registration{};
        // Some authored distant copies have a rounded instance origin. Require
        // an empty near LOD and a coherent offset across most of the mesh before
        // matching that copy to the detailed course; never widen every surface's
        // matching tolerance (which could swallow a nearby independent object).
        const auto& lods=track_.sceneryLods;
        bool hiddenNear=false;
        if(instance.lodList<lods.size() && !lods[instance.lodList].empty()) {
            const auto& nearModel=track_.sceneryModels[lods[instance.lodList].front().model];
            hiddenNear=nearModel.polygons.empty() && nearModel.billboards.empty() && nearModel.glows.empty();
        }
        if(hiddenNear) {
            const float originTolerance=1.0f+tolerance;
            std::vector<bool> used(model.vertices.size(),false);
            for(const auto& polygon:model.polygons)
                for(size_t k=0;k<(polygon.IsQuad()?4u:3u);++k) used[polygon.vertex[k]]=true;
            std::vector<std::array<float,3>> offsets;
            size_t usedCount=0;
            for(size_t vi=0;vi<model.vertices.size();++vi) if(used[vi]) {
                ++usedCount;
                const auto& v=model.vertices[vi];
                std::array<float,3> p{};
                for(size_t k=0;k<3;++k) p[k]=matrix[k]*v.x+matrix[4+k]*v.y+matrix[8+k]*v.z+matrix[12+k];
                auto lo=p,hi=p;
                for(size_t k=0;k<3;++k) {lo[k]-=originTolerance;hi[k]+=originTolerance;}
                const auto a=Cell(lo),b=Cell(hi);
                float best=3*originTolerance*originTolerance;std::array<float,3> offset{};bool foundPoint=false;
                for(int x=a[0];x<=b[0];++x) for(int y=a[1];y<=b[1];++y) for(int z=a[2];z<=b[2];++z) {
                    const auto found=cells_.find({x,y,z});if(found==cells_.end()) continue;
                    for(const auto& q:found->second) {
                        std::array<float,3> d{};float squared=0;
                        for(size_t k=0;k<3;++k) {d[k]=q.position[k]-p[k];squared+=d[k]*d[k];}
                        if(std::abs(d[0])<=originTolerance && std::abs(d[1])<=originTolerance && std::abs(d[2])<=originTolerance && squared<best) {
                            best=squared;offset=d;foundPoint=true;
                        }
                    }
                }
                if(foundPoint) offsets.push_back(offset);
            }
            size_t bestCount=0;
            const float residual=std::min(tolerance*2,.125f);
            for(const auto& candidate:offsets) {
                size_t count=0;std::array<float,3> sum{};
                for(const auto& offset:offsets) {
                    float squared=0;for(size_t k=0;k<3;++k) squared+=(offset[k]-candidate[k])*(offset[k]-candidate[k]);
                    if(squared<=residual*residual) {++count;for(size_t k=0;k<3;++k) sum[k]+=offset[k];}
                }
                if(count>=6 && count*5>=usedCount*3 && count>bestCount) {
                    bestCount=count;for(size_t k=0;k<3;++k) registration[k]=sum[k]/float(count);
                }
            }
        }
        std::vector<std::vector<uint16_t>> matches(model.vertices.size());
        std::vector<std::array<float,3>> joined(model.vertices.size());
        for(size_t vi=0;vi<model.vertices.size();++vi) {
            const auto& v=model.vertices[vi];
            joined[vi]={float(v.x),float(v.y),float(v.z)};
            std::array<float,3> p{};
            for(size_t k=0;k<3;++k) p[k]=matrix[k]*v.x+matrix[4+k]*v.y+matrix[8+k]*v.z+matrix[12+k]+registration[k];
            auto lo=p,hi=p;
            for(size_t k=0;k<3;++k) { lo[k]-=tolerance;hi[k]+=tolerance; }
            const auto a=Cell(lo),b=Cell(hi);
            float closest=tolerance*tolerance;
            for(int x=a[0];x<=b[0];++x) for(int y=a[1];y<=b[1];++y) for(int z=a[2];z<=b[2];++z) {
                const auto found=cells_.find({x,y,z});
                if(found==cells_.end()) continue;
                for(const auto& q:found->second) {
                    float distance=0;
                    for(size_t k=0;k<3;++k) distance+=(p[k]-q.position[k])*(p[k]-q.position[k]);
                    if(distance<=tolerance*tolerance) {
                        matches[vi].push_back(q.chunk);
                        if(joinedVertices && distance<=closest) {
                            closest=distance;
                            for(size_t axis=0;axis<3;++axis) {
                                float squared=0,delta=0;
                                for(size_t k=0;k<3;++k) {
                                    squared+=matrix[axis*4+k]*matrix[axis*4+k];
                                    delta+=matrix[axis*4+k]*(q.position[k]-p[k]+registration[k]);
                                }
                                joined[vi][axis]=float(axis==0?v.x:axis==1?v.y:v.z)+(squared>0?delta/squared:0);
                            }
                        }
                    }
                }
            }
            // A coarse corner can lie on an edge or inside a detailed triangle,
            // rather than at one of its subdivision vertices.
            if(matches[vi].empty()) {
                for(int x=a[0];x<=b[0];++x) for(int y=a[1];y<=b[1];++y) for(int z=a[2];z<=b[2];++z) {
                    const auto found=triangleCells_.find({x,y,z});
                    if(found==triangleCells_.end()) continue;
                    for(size_t ti:found->second) if(SurfaceDistance(p,triangles_[ti])<=tolerance*tolerance)
                        matches[vi].push_back(triangles_[ti].chunk);
                }
            }
        }
        std::vector<std::vector<uint16_t>> result(model.polygons.size());
        std::vector<bool> sharedBoundary(model.vertices.size(),false);
        for(size_t pi=0;pi<model.polygons.size();++pi) {
            const auto& p=model.polygons[pi];
            if(!p.sortFarthest || (p.primCode&2)) continue;
            auto& chunks=result[pi];
            size_t anchoredCorners=0;
            for(size_t k=0;k<(p.IsQuad()?4u:3u);++k) {
                const auto& corner=matches[p.vertex[k]];
                if(corner.empty()) continue;
                ++anchoredCorners;
                chunks.insert(chunks.end(),corner.begin(),corner.end());
            }
            // Three anchors identify a surface patch; coarse quads can have a
            // fourth corner lifted by their reduced subdivision (Midfield road).
            if(anchoredCorners<3) { chunks.clear();continue; }
            std::sort(chunks.begin(),chunks.end());
            chunks.erase(std::unique(chunks.begin(),chunks.end()),chunks.end());
            for(size_t k=0;k<(p.IsQuad()?4u:3u);++k) sharedBoundary[p.vertex[k]]=true;
        }
        if(joinedVertices) {
            *joinedVertices=std::move(joined);
            for(size_t vi=0;vi<model.vertices.size();++vi) if(!sharedBoundary[vi]) {
                const auto& v=model.vertices[vi];
                (*joinedVertices)[vi]={float(v.x),float(v.y),float(v.z)};
            }
        }
        return result;
    }
};
inline bool DetailedReplacementVisible(const std::vector<uint16_t>& chunks,const std::vector<bool>& drawn) {
    return !chunks.empty() && std::all_of(chunks.begin(),chunks.end(),[&](uint16_t chunk){return chunk<drawn.size() && drawn[chunk];});
}
}
