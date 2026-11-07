#ifdef GT2_FURNITURE_TRACE
#include <cstdio>
#endif
#pragma once
#include "gt2view/cockpit_fit.h"
#include "gt2formats/gltf_reader.h"
#include <map>

namespace gt2view {
// Exterior LODs often represent a seat by two intersecting textured cards.
// A volume cut leaves the shoulders/headrest outside the cut as floating
// fragments. Remove a complete detached patch only when the source shell
// encloses every one of its vertices. The actual side/roof silhouette supplies
// that evidence; the procedural dashboard's narrow fitted header does not.
inline std::vector<std::vector<uint8_t>> Gt4CockpitFurniture(
        const gt2::GltfMesh& mesh,const CockpitFit& fit,float scale,
        const std::array<float,3>& offset) {
    using Point=std::array<float,3>;
    using Key=std::array<int32_t,3>;
    struct Triangle {std::array<Point,3> p;size_t primitive,face;};
    std::vector<std::vector<uint8_t>> removed(mesh.primitives.size());
    if(fit.nativeInterior || fit.openTop)return {};
    std::vector<Triangle> envelope,patches;
    std::vector<size_t> parents;
    std::map<std::array<Key,2>,size_t> edges;
    auto root=[&](size_t i) {
        while(parents[i]!=i){parents[i]=parents[parents[i]];i=parents[i];}
        return i;
    };
    auto key=[](const Point& p){Key k;for(size_t a=0;a<3;++a)k[a]=int32_t(std::lround(p[a]*10000.f));return k;};
    for(size_t pi=0;pi<mesh.primitives.size();++pi) {
        const auto& p=mesh.primitives[pi];removed[pi].resize(p.indices.size()/3);
        const auto* m=p.material>=0?&mesh.materials.at(size_t(p.material)):nullptr;
        if(m && (m->reflection || m->wheelAttachment>=0 || m->lampStates))continue;
        for(size_t i=0;i+2<p.indices.size();i+=3) {
            Triangle t{{},pi,i/3};
            for(size_t v=0;v<3;++v)for(size_t a=0;a<3;++a)
                t.p[v][a]=p.vertices.at(p.indices[i+v]).position[a]*scale+offset[a];
            envelope.push_back(t);
            // Blended exterior LODs also contain seat/headrest cards. Their
            // blend state does not make them part of the body envelope; the
            // same enclosure and near-shell proof must classify them.
            if(m && (m->exteriorOnly || m->cockpitControl))continue;
            const size_t ti=patches.size();patches.push_back(t);parents.push_back(ti);
            for(size_t e=0;e<3;++e) {
                std::array<Key,2> edge{key(t.p[e]),key(t.p[(e+1)%3])};
                if(edge[1]<edge[0])std::swap(edge[0],edge[1]);
                const auto [it,inserted]=edges.emplace(edge,ti);
                if(!inserted)parents[root(ti)]=root(it->second);
            }
        }
    }
    std::map<size_t,std::vector<size_t>> components;
    for(size_t i=0;i<patches.size();++i)components[root(i)].push_back(i);
    const float back=std::max({fit.rearWindowZ,fit.rearCowl[0][2],fit.rearCowl[1][2]})-.025f;
    auto inCabin=[&](const Point& p) {
        return p[2]>=cockpit_detail::CowlZAt(fit,p[0])+.025f && p[2]<=back &&
            std::abs(p[0])<=fit.halfWidth;
    };
    // The fitted procedural floor may sit above the original seat-card feet.
    // Validate vertical enclosure against the original body below and above
    // each vertex; do not keep an entire card merely because its foot crosses
    // the replacement floor. Original floor/underbody vertices remain outside.
    // Candidate components cannot supply their own cross-section outline.
    // Components that leave the cabin supply its structural contour;
    // closed-window rays still use all source
    // glass, including independent transparent carrier primitives.
    std::vector<size_t> structure;
    for(const auto& [component,triangles]:components) {
        (void)component;bool boundary=false;
        for(size_t ti:triangles)for(const auto& p:patches[ti].p)boundary=boundary || !inCabin(p);
        if(boundary)structure.insert(structure.end(),triangles.begin(),triangles.end());
    }
    auto dot=[](const Point& a,const Point& b){return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];};
    auto minus=[](const Point& a,const Point& b){return Point{a[0]-b[0],a[1]-b[1],a[2]-b[2]};};
    auto cross=[](const Point& a,const Point& b){return Point{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};};
    auto thinCard=[&](const std::vector<size_t>& triangles) {
        // A folded front/back seat impostor still has two broad dimensions.
        // A solid mirror stalk does not: filling an absent side window must
        // not turn volumetric attachments into disposable furniture.
        for(size_t ti:triangles) {
            const auto& t=patches[ti];auto u=minus(t.p[1],t.p[0]);
            auto normal=cross(u,minus(t.p[2],t.p[0]));
            const float length=std::sqrt(dot(u,u)),area=std::sqrt(dot(normal,normal));
            if(length<.00001f || area<.00000001f)continue;
            for(size_t a=0;a<3;++a){u[a]/=length;normal[a]/=area;}
            const auto v=cross(normal,u);
            Point lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};
            for(size_t other:triangles)for(const auto& p:patches[other].p) {
                const Point projected{dot(p,u),dot(p,v),dot(p,normal)};
                for(size_t a=0;a<3;++a){lo[a]=std::min(lo[a],projected[a]);hi[a]=std::max(hi[a],projected[a]);}
            }
            if(hi[2]-lo[2]<=.25f*std::min(hi[0]-lo[0],hi[1]-lo[1]))return true;
        }
        return false;
    };
    auto nearShell=[&](const std::vector<size_t>& triangles) {
        // Detached lining can follow a curved pillar a few millimetres inside
        // the painted shell. Preserve a whole patch within the same 25mm
        // source inset used by the ray test, even if the convex contour spans
        // across that concavity. Seat-card shoulders extend farther inboard.
        for(size_t ti:triangles)for(const auto& p:patches[ti].p) {
            bool close=false;
            const auto& candidate=patches[ti];
            const auto candidateNormal=cross(minus(candidate.p[1],candidate.p[0]),minus(candidate.p[2],candidate.p[0]));
            const float candidateArea=dot(candidateNormal,candidateNormal);
            for(size_t si:structure) {
                const auto& t=patches[si];
                const auto ab=minus(t.p[1],t.p[0]),ac=minus(t.p[2],t.p[0]),ap=minus(p,t.p[0]);
                const auto normal=cross(ab,ac);const float nn=dot(normal,normal);
                // Lining follows its shell. Proximity to an intersecting
                // interior closure is not evidence that a detached card is
                // bodywork. Allow curved lining up to 45 degrees either way.
                const float alignment=dot(normal,candidateNormal);
                if(candidateArea<1e-16f || alignment*alignment<.5f*nn*candidateArea)continue;
                if(nn>.000000000001f) {
                    const float plane=dot(ap,normal),aa=dot(ab,ab),bb=dot(ab,ac),cc=dot(ac,ac);
                    const float av=dot(ap,ab),cv=dot(ap,ac),det=aa*cc-bb*bb;
                    if(det>.000000000001f) {
                        const float u=(av*cc-cv*bb)/det,v=(cv*aa-av*bb)/det;
                        if(u>=0 && v>=0 && u+v<=1 && plane*plane<=.025f*.025f*nn)close=true;
                    }
                }
                for(size_t edge=0;edge<3 && !close;++edge) {
                    const auto e=minus(t.p[(edge+1)%3],t.p[edge]),d=minus(p,t.p[edge]);
                    const float u=std::clamp(dot(d,e)/std::max(.000000000001f,dot(e,e)),0.f,1.f);
                    Point residual{};for(size_t a=0;a<3;++a)residual[a]=d[a]-u*e[a];
                    if(dot(residual,residual)<=.025f*.025f)close=true;
                }
                if(close)break;
            }
            if(!close)return false;
        }
        return true;
    };
    using SectionPoint=std::array<float,2>;
    struct Section {std::vector<SectionPoint> hull;float beltLeft=1e9f,beltRight=-1e9f;};
    std::map<int32_t,Section> sections;
    auto openWindowEnclosed=[&](const Point& p) {
        const auto zkey=int32_t(std::lround(p[2]*10000.f));
        auto found=sections.find(zkey);
        if(found==sections.end()) {
            Section section;std::vector<SectionPoint> points;
            const std::array<float,2> belt{CockpitSideSillAt(fit,0,p[2])[1],CockpitSideSillAt(fit,1,p[2])[1]};
            for(size_t ti:structure) {
                std::vector<SectionPoint> slice;const auto& t=patches[ti];
                for(size_t edge=0;edge<3;++edge) {
                    const auto& a=t.p[edge];const auto& b=t.p[(edge+1)%3];
                    if(std::abs(a[2]-p[2])<.00001f)slice.push_back({a[0],a[1]});
                    if((a[2]<p[2] && b[2]>p[2]) || (a[2]>p[2] && b[2]<p[2])) {
                        const float u=(p[2]-a[2])/(b[2]-a[2]);
                        slice.push_back({a[0]+u*(b[0]-a[0]),a[1]+u*(b[1]-a[1])});
                    }
                }
                points.insert(points.end(),slice.begin(),slice.end());
                for(size_t side=0;side<2;++side)for(const auto& a:slice)for(const auto& b:slice) {
                    if(belt[side]<std::min(a[1],b[1]) || belt[side]>std::max(a[1],b[1]))continue;
                    const float u=std::abs(b[1]-a[1])<.00001f?0.f:(belt[side]-a[1])/(b[1]-a[1]);
                    const float x=a[0]+u*(b[0]-a[0]);
                    if(side==0)section.beltLeft=std::min(section.beltLeft,x);
                    else section.beltRight=std::max(section.beltRight,x);
                }
            }
            std::sort(points.begin(),points.end());points.erase(std::unique(points.begin(),points.end()),points.end());
            auto turn=[](const SectionPoint& a,const SectionPoint& b,const SectionPoint& c) {
                return (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
            };
            if(points.size()>=3) {
                for(const auto& q:points) {
                    while(section.hull.size()>1 && turn(section.hull[section.hull.size()-2],section.hull.back(),q)<=0)section.hull.pop_back();
                    section.hull.push_back(q);
                }
                const size_t lower=section.hull.size();
                for(size_t i=points.size()-1;i>0;--i) {
                    const auto& q=points[i-1];
                    while(section.hull.size()>lower && turn(section.hull[section.hull.size()-2],section.hull.back(),q)<=0)section.hull.pop_back();
                    section.hull.push_back(q);
                }
                section.hull.pop_back();
            }
            found=sections.emplace(zkey,std::move(section)).first;
        }
        const auto& section=found->second;
        // A real open side window has both a supporting belt below it and a
        // roof above it. Missing side/body evidence is not an open window.
        if(p[1]<=CockpitSideSillAt(fit,p[0]<0?0:1,p[2])[1]+.025f ||
           p[0]<=section.beltLeft+.025f || p[0]>=section.beltRight-.025f || section.hull.size()<3)return false;
        for(size_t i=0;i<section.hull.size();++i) {
            const auto& a=section.hull[i];const auto& b=section.hull[(i+1)%section.hull.size()];
            const float dx=b[0]-a[0],dy=b[1]-a[1];
            if(dx*(p[1]-a[1])-dy*(p[0]-a[0])<=.025f*std::sqrt(dx*dx+dy*dy))return false;
        }
        return true;
    };
    std::map<Key,bool> enclosure,directEnclosure;
    auto enclosed=[&](const Point& p) {
        const auto k=key(p);if(const auto found=enclosure.find(k);found!=enclosure.end())return found->second;
        float left=1e9f,right=-1e9f,roof=-1e9f,bottom=fit.floorY+.024f;
        // Intersect the source's full glass/body triangles before texture
        // apertures are opened. They still locate an open side window's edge.
        for(const auto& t:envelope)for(size_t axis: {size_t(0),size_t(1)}) {
            const size_t a=axis==0?1:0,b=2;
            const auto& v=t.p[0];const auto& w=t.p[1];const auto& q=t.p[2];
            const float wa=w[a]-v[a],wb=w[b]-v[b],qa=q[a]-v[a],qb=q[b]-v[b];
            const float det=wa*qb-wb*qa;if(std::abs(det)<1e-8f)continue;
            const float u=((p[a]-v[a])*qb-(p[b]-v[b])*qa)/det;
            const float s=(wa*(p[b]-v[b])-wb*(p[a]-v[a]))/det;
            if(u<-.00001f || s<-.00001f || u+s>1.00001f)continue;
            const float value=v[axis]+u*(w[axis]-v[axis])+s*(q[axis]-v[axis]);
            if(axis==0){left=std::min(left,value);right=std::max(right,value);}
            else {roof=std::max(roof,value);if(value<fit.floorY)bottom=std::min(bottom,value);}
        }
        const bool aboveSourceFloor=p[1]>=bottom+.001f;
        const bool direct=aboveSourceFloor && p[0]>left+.025f && p[0]<right-.025f && p[1]<roof-.035f;
        // A seat-card corner can approach the roof or a sloping side within
        // the conservative 25/35 mm inset while the rest of the card occupies
        // the cabin. Require actual ray enclosure here; the whole-component
        // thin-card and near-shell checks below protect lining and attachments.
        const bool closeEnclosed=aboveSourceFloor && p[0]>left+.001f && p[0]<right-.001f && p[1]<roof-.001f;
        directEnclosure.emplace(k,direct || closeEnclosed);
        const bool inside=direct || closeEnclosed || (aboveSourceFloor && p[1]<roof-.035f && openWindowEnclosed(p));
        enclosure.emplace(k,inside);return inside;
    };
    for(const auto& [component,triangles]:components) {
        (void)component;
        bool inside=true;
#ifdef GT2_FURNITURE_TRACE
        std::printf("FURNCOMP primitive %zu material %d faces %zu\n",patches[triangles[0]].primitive,mesh.primitives[patches[triangles[0]].primitive].material,triangles.size());
#endif

        // Reject exterior-connected components before doing ray intersections.
        for(size_t ti:triangles)for(const auto& p:patches[ti].p) {
            if(!inCabin(p)){
#ifdef GT2_FURNITURE_TRACE
                if(inside)std::printf("FURNBOUNDS %.9g %.9g %.9g\n",p[0],p[1],p[2]);
#endif
                inside=false;
            }
        }
        // The generated enclosure replaces low detached cards as well as
        // headrests. UV-supported pieces can be disconnected below the belt;
        // requiring an above-belt vertex left those fragments floating on doors.
        // Full source enclosure, floor limits and near-shell protection still
        // apply to every vertex of the complete component.
        if(!inside)continue;
        for(size_t ti:triangles)for(const auto& p:patches[ti].p)
            if(inside && !enclosed(p)){
#ifdef GT2_FURNITURE_TRACE
                std::printf("FURNENCLOSURE %.9g %.9g %.9g\n",p[0],p[1],p[2]);
#endif
                inside=false;
            }
        if(inside) {
            bool needsWindowProof=false;
            for(size_t ti:triangles)for(const auto& p:patches[ti].p)
                needsWindowProof=needsWindowProof || !directEnclosure.at(key(p));
            // Real ray enclosure supports solid seats as well as flat cards.
            // A missing side-window surface needs the stricter card proof.
            // Shell-adjacent lining is protected in both cases.
            if((needsWindowProof && !thinCard(triangles)) || nearShell(triangles))inside=false;
        }
#ifdef GT2_FURNITURE_TRACE
        std::printf("FURNRESULT %d\n",inside?1:0);
#endif
        if(inside)for(size_t ti:triangles)removed[patches[ti].primitive][patches[ti].face]=1;
    }
    return removed;
}
}
