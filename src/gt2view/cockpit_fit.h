#pragma once
#include "gt2export/car_mesh.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <span>
#include <utility>
#include <vector>

namespace gt2view {

// Car-local metres: right +X, up +Y, forward -Z. The cabin is presentation only.
struct CockpitFit {
    float halfWidth = 0.82f, frontZ = -2.0f, rearZ = 2.0f;
    float floorY = 0.0f, roofY = 1.15f;
    float windshieldZ = -0.7f, dashboardY = 0.68f;
    float windshieldY = 0.66f, roofFrontZ = 0.0f, roofFrontY = 1.15f;
    float roofRearZ = 1.0f, roofRearY = 1.15f, rearWindowZ = 1.6f, rearWindowY = 0.70f;
    float windshieldHalfWidth = 0.78f, roofFrontHalfWidth = 0.62f;
    float roofRearHalfWidth = 0.62f, rearWindowHalfWidth = 0.75f;
    // Outer corners, left then right; Y/Z may differ from the centre-line profile.
    std::array<std::array<float, 3>, 2> frontCowl{{{-0.78f, 0.66f, -0.7f}, {0.78f, 0.66f, -0.7f}}};
    std::array<std::array<float, 3>, 2> frontHeader{{{-0.62f, 1.15f, 0.0f}, {0.62f, 1.15f, 0.0f}}};
    std::array<std::array<float, 3>, 2> rearHeader{{{-0.62f, 1.15f, 1.0f}, {0.62f, 1.15f, 1.0f}}};
    std::array<std::array<float, 3>, 2> rearCowl{{{-0.75f, 0.70f, 1.6f}, {0.75f, 0.70f, 1.6f}}};
    std::array<float, 3> eye{-0.28f, 0.98f, -0.05f};
    std::array<float, 3> mirror{0.f, 1.03f, -.2f};
    std::array<float, 4> nativeWheel{}; // authored centre xyz, radius; zero radius means no rim
    std::array<float, 4> speedometer{}; // native cabin display; width 0 disables it
    std::array<std::array<float,5>,4> wheelWells{}; // centre xyz, tyre radius, width
    std::array<std::array<float, 3>, 17> cowl{};
    std::array<std::array<float, 3>, 17> bonnetJoin{};
    bool bonnetJoinValid = false;
    std::array<std::array<std::array<float,3>,17>,5> bonnetSeal{};
    bool bonnetSealValid=false;
    // Outer door-card boundary, front to back, with uniform Z stations per side.
    std::array<std::array<std::array<float, 3>, 17>, 2> sideSill{};
    bool sideSillValid = false;
    bool nativeInterior = false; // Authored furniture retains its own seating and wheel placement.
    bool smoothShell = false; // Imported closed body: continuous inner roof/window surrounds.
    bool authoredUpperShell = false; // Retain the source roof/pillars instead of generating a second frame.
    bool valid = false, openTop = false;
    bool operator==(const CockpitFit&) const = default;
};

namespace cockpit_detail {
inline float CowlZAt(const CockpitFit& fit,float x) {
    for(size_t i=1;i<fit.cowl.size();++i)if(x<=fit.cowl[i][0] || i+1==fit.cowl.size()) {
        const auto& a=fit.cowl[i-1];const auto& b=fit.cowl[i];
        const float t=std::clamp((x-a[0])/std::max(.001f,b[0]-a[0]),0.f,1.f);
        return a[2]+(b[2]-a[2])*t;
    }
    return fit.windshieldZ;
}
// Audit reference triangles must lie wholly ahead of the curved cabin cut.
inline bool TriangleAheadOfCowl(const std::array<std::array<float,3>,3>& triangle,
                                const std::array<std::array<float,3>,17>& cowl,float tolerance=0.f) {
    for(const auto& v:triangle)for(size_t k=1;k<cowl.size();++k)
        if(v[0]<=cowl[k][0] || k+1==cowl.size()) {
            const auto& a=cowl[k-1];const auto& b=cowl[k];
            const float t=std::clamp((v[0]-a[0])/std::max(.001f,b[0]-a[0]),0.f,1.f);
            if(v[2]>a[2]+(b[2]-a[2])*t+tolerance)return false;
            break;
        }
    // The cabin boundary is not convex. A long triangle can have all three
    // vertices outside while its middle crosses the seating volume. Check
    // each slope change too; together these exhaust the piecewise-linear gap.
    for(const auto& station:cowl)for(size_t edge=0;edge<3;++edge) {
        const auto& a=triangle[edge];const auto& b=triangle[(edge+1)%3];
        if(station[0]<std::min(a[0],b[0]) || station[0]>std::max(a[0],b[0]))continue;
        if(std::abs(b[0]-a[0])<1e-7f)continue;
        const float t=(station[0]-a[0])/(b[0]-a[0]);
        if(a[2]+(b[2]-a[2])*t>station[2]+tolerance)return false;
    }
    return true;
}

inline float SurfaceHeight(std::span<const gt2::CarMeshVertex> mesh, float z, float x = 0.0f,
                           float ceiling = std::numeric_limits<float>::infinity()) {
    float height = -std::numeric_limits<float>::infinity();
    for (size_t i = 0; i + 2 < mesh.size(); i += 3) {
        const float* a = mesh[i].pos;
        const float* b = mesh[i + 1].pos;
        const float* c = mesh[i + 2].pos;
        const float bx = b[0] - a[0], bz = b[2] - a[2];
        const float cx = c[0] - a[0], cz = c[2] - a[2];
        const float determinant = bx * cz - bz * cx;
        if (std::abs(determinant) < 1e-8f) continue;
        const float u = ((x - a[0]) * cz - (z - a[2]) * cx) / determinant;
        const float v = (bx * (z - a[2]) - bz * (x - a[0])) / determinant;
        if (u >= -1e-5f && v >= -1e-5f && u + v <= 1.00001f) {
            const float y=a[1] + u * (b[1] - a[1]) + v * (c[1] - a[1]);
            if(y<=ceiling)height=std::max(height,y);
        }
    }
    return height;
}

// Trim is an underside closure, not a copy of wipers, mirror stalks or
// isolated windshield triangles. Lower single-station peaks; never raise
// surrounding points to an accessory, which would change the bonnet shape.
inline void RemoveTrimSpikes(std::array<std::array<float,3>,17>& curve, bool underside=false) {
    for(size_t pass=0;pass<curve.size();++pass)
        for(size_t i=1;i+1<curve.size();++i)
                curve[i][1]=std::min(curve[i][1],underside ? (curve[i-1][1]+curve[i+1][1])*.5f+.002f : std::max(curve[i-1][1],curve[i+1][1])+.025f);
}

// Follow the real surface between bonnet and cowl. A single straight band
// can cross a concave bonnet even when both of its edges are underneath it.
inline void FitBonnetSeal(CockpitFit& fit,std::span<const gt2::CarMeshVertex> mesh) {
    for(size_t row=0;row<fit.bonnetSeal.size();++row) {
        const float t=float(row)/float(fit.bonnetSeal.size()-1);
        for(size_t i=0;i<fit.cowl.size();++i) {
            auto& p=fit.bonnetSeal[row][i];
            for(size_t k=0;k<3;++k)p[k]=fit.bonnetJoin[i][k]*(1-t)+fit.cowl[i][k]*t;
            const float y=SurfaceHeight(mesh,p[2],p[0],p[1]+.012f);
            if(std::isfinite(y))p[1]=std::min(p[1],y);
            p[1]-=.030f;
        }
        RemoveTrimSpikes(fit.bonnetSeal[row],true);
    }
    // A sparse cell may bridge a steep wing recess. Bound its entire patch
    // below the sampled surface, rather than only fitting its four corners.
    const auto sampled=fit.bonnetSeal;
    for(size_t row=1;row<sampled.size();++row)for(size_t i=1;i<sampled[0].size();++i) {
        float lowest=std::numeric_limits<float>::infinity();
        const std::array<std::array<float,3>,4> quad{sampled[row-1][i-1],sampled[row-1][i],sampled[row][i],sampled[row][i-1]};
        for(size_t half=0;half<2;++half)for(int a=0;a<=6;++a)for(int b=0;b<=6-a;++b) {
            const float u=float(a)/6,v=float(b)/6;std::array<float,3> p{};
            for(size_t k=0;k<3;++k)p[k]=quad[0][k]*(1-u-v)+quad[half+1][k]*u+quad[half+2][k]*v;
            const float y=SurfaceHeight(mesh,p[2],p[0]);
            if(std::isfinite(y))lowest=std::min(lowest,y-.015f);
        }
        for(size_t r:{row-1,row})for(size_t c:{i-1,i})fit.bonnetSeal[r][c][1]=std::min(fit.bonnetSeal[r][c][1],lowest);
    }
    fit.bonnetSealValid=true;
}

inline float CabinSurfaceHeight(std::span<const gt2::CarMeshVertex> mesh, float z, float x = 0.0f) {
    const float y = SurfaceHeight(mesh, z, x);
    const float left = SurfaceHeight(mesh, z, x - 0.10f), right = SurfaceHeight(mesh, z, x + 0.10f);
    // Roof aerials and narrow accessories are not the headliner surface.
    if (std::isfinite(left) && std::isfinite(right) && y > std::max(left, right) + 0.075f) return std::max(left, right);
    return y;
}

inline std::vector<std::array<float, 2>> CentreProfile(std::span<const gt2::CarMeshVertex> mesh, float x = 0.f) {
    std::vector<float> stations;
    for (size_t i = 0; i + 2 < mesh.size(); i += 3)
        for (size_t edge = 0; edge < 3; ++edge) {
            const auto& a = mesh[i + edge]; const auto& b = mesh[i + (edge + 1) % 3];
            if (std::abs(a.pos[0]-x) < 1e-7f) stations.push_back(a.pos[2]);
            if ((a.pos[0]-x) * (b.pos[0]-x) < 0)
                stations.push_back(a.pos[2] + (b.pos[2] - a.pos[2]) * ((x-a.pos[0]) / (b.pos[0] - a.pos[0])));
        }
    std::sort(stations.begin(), stations.end());
    stations.erase(std::unique(stations.begin(), stations.end(), [](float a, float b) { return std::abs(a - b) < 1e-5f; }), stations.end());
    std::vector<std::array<float, 2>> profile;
    for (float z : stations) {
        const float y = CabinSurfaceHeight(mesh, z, x);
        if (!std::isfinite(y)) continue;
        while (profile.size() > 1) {
            const auto& a = profile[profile.size() - 2]; const auto& b = profile.back();
            if (std::abs((b[0] - a[0]) * (y - a[1]) - (b[1] - a[1]) * (z - a[0])) > 4e-5f) break;
            profile.pop_back();
        }
        profile.push_back({z, y});
    }
    return profile;
}

inline size_t WindshieldSlopeKnee(std::span<const std::array<float,2>> profile, size_t begin, size_t end) {
    float shallowLength=0;
    size_t result=begin;
    for (size_t i=begin;i+1<end;++i) {
        const auto& a=profile[i]; const auto& b=profile[i+1]; const auto& c=profile[i+2];
        const float previous=(b[1]-a[1])/(b[0]-a[0]);
        const float next=(c[1]-b[1])/(c[0]-b[0]);
        shallowLength=previous>=.28f && previous<=.50f ? shallowLength+b[0]-a[0] : 0.f;
        // A sloping bonnet may never flatten below the profile's rise threshold.
        // Its long shallow run still ends at a pronounced windscreen slope break.
        if (shallowLength>=.18f && next>=previous*1.50f && next>=.50f &&
            profile[end][1]-b[1]>=.17f && profile[end][0]-b[0]>=.12f) result=i+1;
    }
    return result;
}

struct CabinShell {
    std::vector<gt2::CarMeshVertex> vertices;
    std::vector<gt2::CarPolygon> polygons;
};

inline CabinShell ConnectedCabinShell(const gt2::CarLod& lod, std::span<const gt2::CarMeshVertex> mesh,
                                     float roofFrontZ, float roofFrontY) {
    std::vector<size_t> roots(lod.vertices.size());
    for (size_t i = 0; i < roots.size(); ++i) roots[i] = i;
    auto root = [&](size_t i) {
        while (roots[i] != i) { roots[i] = roots[roots[i]]; i = roots[i]; }
        return i;
    };
    auto join = [&](size_t a, size_t b) { roots[root(a)] = root(b); };
    for (size_t i = 0; i < roots.size(); ++i)
        for (size_t j = 0; j < i; ++j) {
            const auto& a = lod.vertices[i]; const auto& b = lod.vertices[j];
            if (a.x == b.x && a.y == b.y && a.z == b.z) join(i, j);
        }
    for (const auto& p : lod.polygons)
        for (size_t i = 1; i < (p.IsQuad() ? 4u : 3u); ++i) join(p.vertex[0], p.vertex[i]);
    std::vector<size_t> sizes(roots.size());
    for (const auto& p : lod.polygons) sizes[root(p.vertex[0])] += p.IsQuad() ? 6 : 3;
    size_t selected = roots.size(), at = 0, largest = 0;
    for (const auto& p : lod.polygons) {
        const size_t count = p.IsQuad() ? 6 : 3, group = root(p.vertex[0]);
        const float y = SurfaceHeight(mesh.subspan(at, count), roofFrontZ);
        if (std::abs(y - roofFrontY) < 0.005f && sizes[group] > largest) {
            selected = group; largest = sizes[group];
        }
        at += count;
    }
    CabinShell result;
    if (selected == roots.size() || largest < mesh.size() / 2 || largest == mesh.size()) return result;
    at = 0;
    for (const auto& p : lod.polygons) {
        const size_t count = p.IsQuad() ? 6 : 3;
        if (root(p.vertex[0]) == selected) {
            result.vertices.insert(result.vertices.end(), mesh.begin() + at, mesh.begin() + at + count);
            result.polygons.push_back(p);
        }
        at += count;
    }
    return result;
}

inline void WindowCorners(std::span<const gt2::CarMeshVertex> mesh, std::span<const gt2::CarPolygon> polygons, float lowerZ, float lowerY,
                          float upperZ, float upperY, float halfWidth, bool rear,
                          std::array<std::array<float, 3>, 2>& lower,
                          std::array<std::array<float, 3>, 2>& upper, bool connectedWindow = true,
                          std::vector<gt2::CarMeshVertex>* windowSurface = nullptr) {
    for (size_t side = 0; side < 2; ++side) {
        const float sign = side ? 1.0f : -1.0f;
        lower[side] = {sign * halfWidth * 0.90f, lowerY, lowerZ};
        upper[side] = {sign * halfWidth * 0.74f, upperY, upperZ};
    }
    float lowerExtent[2]{}, upperExtent[2]{}, extraExtent[2]{};
    auto extraLower = lower;
    // Detached mirrors can have the same height/slope as the windscreen.
    // Their extremities are not cabin corners. Retain components which reach
    // the actual centre-line window surface, including separately authored glass.
    std::vector<size_t> roots(mesh.size()/3);
    for(size_t i=0;i<roots.size();++i)roots[i]=i;
    auto root=[&](size_t i){while(roots[i]!=i){roots[i]=roots[roots[i]];i=roots[i];}return i;};
    std::map<std::array<float,3>,size_t> shared;
    struct Candidate { size_t at; bool atHeader; };
    std::vector<Candidate> candidates;
    std::vector<bool> windowComponent(roots.size());
    bool hasCentreSurface=false;
    size_t at = 0;
    for (const auto& polygon : polygons) {
        const size_t count = polygon.IsQuad() ? 6 : 3;
        const size_t end = at + count;
        float faceMaxY = -std::numeric_limits<float>::infinity();
        for (size_t i = at; i < end; ++i) faceMaxY = std::max(faceMaxY, mesh[i].pos[1]);
        for (size_t i = at; i < end; i += 3) {
            const float* a = mesh[i].pos; const float* b = mesh[i + 1].pos; const float* c = mesh[i + 2].pos;
            float nx = (b[1] - a[1]) * (c[2] - a[2]) - (b[2] - a[2]) * (c[1] - a[1]);
            float ny = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
            float nz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
            if (length < 1e-8f) continue;
            // Use the same facing distinction as side-window removal. A
            // quarter-window joined to the A-pillar must not become a swept
            // windscreen corner (and a deep, asymmetric dashboard wing).
            if(connectedWindow && std::abs(nx)>=std::abs(nz)*1.1f && std::abs(nx)>=length*.4f &&
               ((a[0]>halfWidth*.30f && b[0]>halfWidth*.30f && c[0]>halfWidth*.30f) ||
                (a[0]<-halfWidth*.30f && b[0]<-halfWidth*.30f && c[0]<-halfWidth*.30f)))continue;
            if (ny < 0) { ny = -ny; nz = -nz; }
            const float z = (a[2] + b[2] + c[2]) / 3.0f;
            if (ny / length < 0.12f || (rear ? nz : -nz) / length < 0.14f ||
                (rear ? std::max({a[1], b[1], c[1]}) : faceMaxY) < upperY - (rear ? .13f : .22f) ||
                z < std::min(lowerZ, upperZ) - 0.15f || z > std::max(lowerZ, upperZ) + 0.15f) continue;
            const bool atHeader=std::max({a[1],b[1],c[1]})>=upperY-(rear?.13f:.22f);
            candidates.push_back({i,atHeader});
        }
        at = end;
    }
    // Follow only window-facing surfaces. A mirror welded to a door is still
    // not part of the windshield; connectivity through the whole body would
    // allow the fitting walk to leave the window and reach that accessory.
    if(connectedWindow)for(const auto& candidate:candidates)for(size_t j=0;j<3;++j){
        const auto* p=mesh[candidate.at+j].pos;
        const auto [it,inserted]=shared.emplace(std::array{p[0],p[1],p[2]},candidate.at/3);
        if(!inserted)roots[root(candidate.at/3)]=root(it->second);
    }
    for(const auto& candidate:candidates){
        const float a=mesh[candidate.at].pos[0],b=mesh[candidate.at+1].pos[0],c=mesh[candidate.at+2].pos[0];
        if(std::min({a,b,c})<=0 && std::max({a,b,c})>=0){windowComponent[root(candidate.at/3)]=true;hasCentreSurface=true;}
    }
    for(const auto& candidate:candidates){
        if(connectedWindow && hasCentreSurface && !windowComponent[root(candidate.at/3)])continue;
        if(windowSurface)windowSurface->insert(windowSurface->end(),mesh.begin()+candidate.at,mesh.begin()+candidate.at+3);
        const float* a=mesh[candidate.at].pos;const float* b=mesh[candidate.at+1].pos;const float* c=mesh[candidate.at+2].pos;
        for (const float* p : {a, b, c}) {
                if (p[1] < lowerY - 0.20f || p[1] > upperY + 0.13f || std::abs(p[0]) > halfWidth + 0.06f) continue;
                const bool triangleAtHeader = candidate.atHeader;
                const size_t side = p[0] >= 0 ? 1 : 0;
                const float extent = std::abs(p[0]);
                if (extent < halfWidth*.30f) continue;
                if (p[1] <= lowerY + 0.075f && (!rear || p[2] >= upperZ + 0.06f)) {
                    if (triangleAtHeader && extent > lowerExtent[side]) {
                        lower[side] = {p[0], p[1], p[2]}; lowerExtent[side] = extent;
                    } else if (!triangleAtHeader && extent > extraExtent[side]) {
                        extraLower[side] = {p[0], p[1], p[2]}; extraExtent[side] = extent;
                    }
                }
                if (triangleAtHeader && p[1] >= upperY - 0.12f && std::abs(p[2] - upperZ) < 0.36f && extent > upperExtent[side]) {
                    upper[side] = {p[0], p[1], p[2]}; upperExtent[side] = extent;
                }
            }
    }
    // A curved quad's outer cowl corner may belong only to its lower triangle.
    if (!rear) for (size_t side = 0; side < 2; ++side)
        if (lowerExtent[side] > 0 && lowerExtent[side] < upperExtent[side] * 0.90f && extraExtent[side] > lowerExtent[side])
            lower[side] = extraLower[side];
}

inline gt2::CarMeshVertex Interpolate(const gt2::CarMeshVertex& a, const gt2::CarMeshVertex& b, float t) {
    gt2::CarMeshVertex out = a;
    for (size_t k = 0; k < 3; ++k) {
        out.pos[k] += (b.pos[k] - a.pos[k]) * t;
        out.color[k] += (b.color[k] - a.color[k]) * t;
    }
    for (size_t k = 0; k < 2; ++k) out.texel[k] += (b.texel[k] - a.texel[k]) * t;
    return out;
}

inline std::vector<gt2::CarMeshVertex> ClipPlane(std::span<const gt2::CarMeshVertex> polygon, size_t axis, float limit,
                                              bool keepBelow = true) {
    std::vector<gt2::CarMeshVertex> out;
    if (polygon.empty()) return out;
    out.reserve(polygon.size() + 1);
    auto previous = polygon.back();
    bool previousInside = keepBelow ? previous.pos[axis] <= limit : previous.pos[axis] >= limit;
    for (const auto& current : polygon) {
        const bool inside = keepBelow ? current.pos[axis] <= limit : current.pos[axis] >= limit;
        if (inside != previousInside) {
            const float t = std::clamp((limit - previous.pos[axis]) / (current.pos[axis] - previous.pos[axis]), 0.0f, 1.0f);
            auto edge = Interpolate(previous, current, t);
            edge.pos[axis] = limit;
            out.push_back(edge);
        }
        if (inside) out.push_back(current);
        previous = current;
        previousInside = inside;
    }
    return out;
}
} // namespace cockpit_detail

inline bool CockpitSideOpening(std::span<const gt2::CarMeshVertex> triangle, const CockpitFit& fit) {
    if (triangle.size()!=3) return false;
    const auto* a=triangle[0].pos; const auto* b=triangle[1].pos; const auto* c=triangle[2].pos;
    const float nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
    const float ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
    const float nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
    if (nx*nx<1e-12f || std::abs(nx)<std::abs(nz)*1.1f || nx*nx<.16f*(nx*nx+ny*ny+nz*nz)) return false;
    const float sign=a[0]<0?-1.f:1.f;
    return std::all_of(triangle.begin(),triangle.end(),[&](const auto& p) { return p.pos[0]*sign>fit.halfWidth*.30f; });
}

inline std::array<float,3> CockpitSideSillAt(const CockpitFit& fit, size_t side, float z) {
    const auto& front=fit.frontCowl[side]; const auto& rear=fit.rearCowl[side];
    const float along=std::clamp((z-front[2])/std::max(.01f,rear[2]-front[2]),0.f,1.f);
    if (!fit.sideSillValid) {
        return {front[0]+(rear[0]-front[0])*along,front[1]+(rear[1]-front[1])*along,
                front[2]+(rear[2]-front[2])*along};
    }
    const auto& sill=fit.sideSill[side];
    const float station=along*float(sill.size()-1);
    const auto index=std::min(size_t(station),sill.size()-2);
    const float t=station-float(index);
    std::array<float,3> result{};
    for (size_t axis=0;axis<3;++axis) result[axis]=sill[index][axis]+(sill[index+1][axis]-sill[index][axis])*t;
    return result;
}

inline void FitCockpitSideSills(CockpitFit& fit, std::span<const gt2::CarMeshVertex> openings) {
    if (!fit.valid) return;
    fit.sideSillValid=false;
    for (size_t side=0;side<2;++side) {
        auto& sill=fit.sideSill[side];
        const auto& front=fit.frontCowl[side]; const auto& rear=fit.rearCowl[side];
        for (size_t i=0;i<sill.size();++i) {
            const float t=float(i)/float(sill.size()-1);
            for (size_t axis=0;axis<3;++axis) sill[i][axis]=front[axis]+(rear[axis]-front[axis])*t;
            float lowest=std::numeric_limits<float>::infinity(),edgeX=sill[i][0];
            for (size_t at=0;at+2<openings.size();at+=3) {
                const auto triangle=openings.subspan(at,3);
                if (!CockpitSideOpening(triangle,fit) || size_t(triangle[0].pos[0]>=0)!=side) continue;
                for (size_t edge=0;edge<3;++edge) {
                    const auto* a=triangle[edge].pos; const auto* b=triangle[(edge+1)%3].pos;
                    if (sill[i][2]<std::min(a[2],b[2])-1e-6f || sill[i][2]>std::max(a[2],b[2])+1e-6f) continue;
                    const float u=std::abs(b[2]-a[2])<1e-7f ? (a[1]<b[1]?0.f:1.f) :
                        std::clamp((sill[i][2]-a[2])/(b[2]-a[2]),0.f,1.f);
                    const float y=a[1]+(b[1]-a[1])*u;
                    if (y<lowest) { lowest=y; edgeX=a[0]+(b[0]-a[0])*u; }
                }
            }
            if (std::isfinite(lowest)) {
                sill[i][1]=std::min(sill[i][1],lowest);
                if (std::abs(edgeX)>std::abs(sill[i][0])) sill[i][0]=edgeX;
            }
        }
        // A fixed-size profile must stay below corners between stations as well.
        // Only lower the two adjacent samples; no paint- or model-specific offsets.
        for (size_t at=0;at+2<openings.size();at+=3) {
            const auto triangle=openings.subspan(at,3);
            if (!CockpitSideOpening(triangle,fit) || size_t(triangle[0].pos[0]>=0)!=side) continue;
            for (const auto& vertex:triangle) {
                if (vertex.pos[2]<front[2] || vertex.pos[2]>rear[2]) continue;
                const float station=(vertex.pos[2]-front[2])/std::max(.01f,rear[2]-front[2])*float(sill.size()-1);
                const size_t index=std::min(size_t(station),sill.size()-2);
                const float t=station-float(index);
                const float y=sill[index][1]+(sill[index+1][1]-sill[index][1])*t;
                const float lower=std::max(0.f,y-vertex.pos[1]);
                sill[index][1]-=lower; sill[index+1][1]-=lower;
            }
        }
    }
    fit.sideSillValid=true;
}

// The last argument is used by the owned-data regression audit to reproduce
// the old detached-accessory fit; production callers use the connected surface.
inline CockpitFit FitCockpit(const gt2::CarModel& model, std::span<const gt2::CarMeshVertex> replacement = {},
                             bool connectedWindow = true) {
    CockpitFit fit;
    for (size_t i=0;i<4;++i) {
        const auto& w=model.wheels[i];
        fit.wheelWells[i]={w.z/4096.f,w.y/4096.f,w.x/4096.f,
            (i<2?model.wheelRadiusFront:model.wheelRadiusRear)/4096.f,
            (i<2?model.wheelWidthFront:model.wheelWidthRear)/4096.f};
    }
    if (model.lods.empty() || model.lods[0].vertices.empty()) return fit;
    gt2::CarMeshOptions options;
    options.wheels = false;
    const auto mesh = replacement.empty() ? gt2::BuildCarMesh(model, options) :
        std::vector<gt2::CarMeshVertex>(replacement.begin(), replacement.end());
    const std::vector<gt2::CarPolygon> replacementPolygons(replacement.size()/3);
    const auto& polygons = replacement.empty() ? model.lods[0].polygons : replacementPolygons;
    if (mesh.empty()) return fit;
    std::array<float, 3> lo{mesh[0].pos[0], mesh[0].pos[1], mesh[0].pos[2]}, hi = lo;
    for (const auto& vertex : mesh)
        for (size_t k = 0; k < 3; ++k) {
            if (!std::isfinite(vertex.pos[k])) return fit;
            lo[k] = std::min(lo[k], vertex.pos[k]);
            hi[k] = std::max(hi[k], vertex.pos[k]);
        }
    const float width = hi[0] - lo[0], length = hi[2] - lo[2], height = hi[1] - lo[1];
    // The car archive also contains logos, trophies and menu scenery.
    if (width < 0.8f || width > 3.0f || length < 2.0f || length > 8.0f || height < 0.35f || height > 3.0f) return fit;
    fit.valid = true;
    fit.halfWidth = std::clamp(width * 0.5f - 0.035f, 0.50f, 1.25f);
    fit.frontZ = lo[2]; fit.rearZ = hi[2]; fit.floorY = lo[1];
    float frontAxle = (model.wheels[0].x + model.wheels[1].x) / 8192.0f;
    float rearAxle = (model.wheels[2].x + model.wheels[3].x) / 8192.0f;
    if (rearAxle - frontAxle < 0.8f || rearAxle - frontAxle > 5.0f || frontAxle < lo[2] || rearAxle > hi[2]) {
        frontAxle = lo[2] + length * 0.22f;
        rearAxle = hi[2] - length * 0.22f;
    }
    const float wheelbase = rearAxle - frontAxle;
    auto profile = cockpit_detail::CentreProfile(mesh);
    if (profile.size() < 3) { fit.valid = false; return fit; }
    auto slope = [&](size_t i) { return (profile[i + 1][1] - profile[i][1]) / (profile[i + 1][0] - profile[i][0]); };
    size_t windStart = profile.size(), windEnd = 0;
    float bestRoof = -std::numeric_limits<float>::infinity(), bestRise = 0.17f;
    bool bestSupported = false;
    // Follow authored profile breaks, including curved windscreens and their painted header band.
    for (size_t i = 0; i + 1 < profile.size(); ++i) {
        if (profile[i][0] < frontAxle - wheelbase * 0.20f || profile[i][0] > rearAxle - wheelbase * 0.15f || slope(i) < 0.28f) continue;
        size_t end = i + 1;
        while (end + 1 < profile.size() && slope(end) >= 0.28f && profile[end][0] < rearAxle) ++end;
        const float rise = profile[end][1] - profile[i][1];
        const float roof = profile[end][1];
        const float behind = cockpit_detail::CabinSurfaceHeight(mesh, profile[end][0] + 0.20f);
        const bool supported = std::isfinite(behind) && behind >= roof - 0.10f;
        // Imported roadsters can omit the roof entirely. A supported bonnet
        // step must not outrank a much taller unsupported windscreen header.
        const bool tallerReplacement=!replacement.empty() && roof>bestRoof+.30f;
        const bool comparableRoof=replacement.empty() || roof>=bestRoof-.30f;
        if (rise > 0.17f && profile[end][0] - profile[i][0] > 0.12f &&
            (windStart == profile.size() || tallerReplacement || (comparableRoof && supported && !bestSupported) || (supported == bestSupported &&
             (roof > bestRoof + 0.03f || (std::abs(roof - bestRoof) <= 0.03f && rise > bestRise))))) {
            windStart = i; windEnd = end; bestRoof = roof; bestRise = rise; bestSupported = supported;
        }
        i = end - 1;
    }
    if (windStart == profile.size()) { fit.valid = false; return fit; }
    windStart=cockpit_detail::WindshieldSlopeKnee(profile,windStart,windEnd);
    fit.windshieldZ = profile[windStart][0]; fit.windshieldY = profile[windStart][1];
    fit.roofFrontZ = profile[windEnd][0]; fit.roofFrontY = profile[windEnd][1];
    fit.dashboardY = fit.windshieldY + 0.025f;
    auto rearBreaks = [&]() {
        size_t roofEnd = windEnd, rearEnd = windEnd;
        for (size_t i = windEnd; i + 1 < profile.size(); ++i) {
            if (profile[i][0] > hi[2] - 0.02f) break;
            if (slope(i) >= -0.14f || profile[i][1] < fit.roofFrontY - 0.15f) continue;
            size_t end = i + 1;
            while (end + 1 < profile.size() && profile[end][1] > fit.windshieldY + 0.035f) {
                if (slope(end) > 0.08f || (slope(end) > -0.14f && profile[end + 1][0] - profile[end][0] > 0.30f)) break;
                ++end;
            }
            if (profile[i][1] - profile[end][1] < 0.12f) continue;
            roofEnd = i; rearEnd = end; break;
        }
        return std::array<size_t, 2>{roofEnd, rearEnd};
    };
    auto ends = rearBreaks();
    // Ignore a separate wing only when a lower, connected roof continues underneath it.
    auto shell = replacement.empty() ? cockpit_detail::ConnectedCabinShell(model.lods[0], mesh, fit.roofFrontZ, fit.roofFrontY) : cockpit_detail::CabinShell{};
    if (!shell.vertices.empty()) {
        const float rearZ = profile[ends[0]][0], rearY = profile[ends[0]][1];
        const float attachedY = cockpit_detail::CabinSurfaceHeight(shell.vertices, rearZ);
        float attachedRoof = fit.roofFrontY;
        for (const auto& vertex : shell.vertices)
            if (vertex.pos[2] >= fit.roofFrontZ && vertex.pos[2] <= rearZ && std::abs(vertex.pos[0]) < fit.halfWidth * 0.8f)
                attachedRoof = std::max(attachedRoof, vertex.pos[1]);
        if (!std::isfinite(attachedY) || rearY - attachedY < 0.08f || rearY - attachedRoof < 0.06f ||
            attachedY < fit.roofFrontY - 0.15f) shell = {};
    }
    auto attachedProfile = shell.vertices.empty() ? std::vector<std::array<float, 2>>{} : cockpit_detail::CentreProfile(shell.vertices);
    if (attachedProfile.size() < 3) shell = {};
    const std::span<const gt2::CarMeshVertex> cabin = shell.vertices.empty() ? std::span(mesh) : std::span(shell.vertices);
    const std::span<const gt2::CarPolygon> cabinPolygons = shell.vertices.empty() ? std::span(polygons) : std::span(shell.polygons);
    if (!shell.vertices.empty()) {
        profile = std::move(attachedProfile);
        windEnd = size_t(std::min_element(profile.begin(), profile.end(), [&](const auto& a, const auto& b) {
            return std::abs(a[0] - fit.roofFrontZ) < std::abs(b[0] - fit.roofFrontZ);
        }) - profile.begin());
        ends = rearBreaks();
    }
    const auto roofEnd = ends[0], rearEnd = ends[1];
    if (roofEnd == windEnd) {
        // An isolated windscreen has no roof surface over the seats.
        fit.roofRearZ = std::min(rearAxle, fit.roofFrontZ + wheelbase * 0.45f);
        fit.roofRearY = fit.roofFrontY;
        fit.rearWindowZ = rearAxle; fit.rearWindowY = cockpit_detail::SurfaceHeight(mesh, rearAxle);
        if (!std::isfinite(fit.rearWindowY)) fit.rearWindowY = fit.windshieldY;
    } else {
        fit.roofRearZ = profile[roofEnd][0]; fit.roofRearY = profile[roofEnd][1];
        fit.rearWindowZ = profile[rearEnd][0]; fit.rearWindowY = profile[rearEnd][1];
        if (fit.rearWindowY < fit.windshieldY - 0.12f && rearEnd > roofEnd) {
            const auto& a = profile[rearEnd - 1]; const auto& b = profile[rearEnd];
            const float t = std::clamp((fit.windshieldY - a[1]) / (b[1] - a[1]), 0.0f, 1.0f);
            fit.rearWindowZ = a[0] + (b[0] - a[0]) * t; fit.rearWindowY = a[1] + (b[1] - a[1]) * t;
        }
    }
    // Correcting trim boundaries must not recalibrate the established seat or
    // virtual-wheel position. Keep their original body-derived lateral anchor.
    std::array<std::array<float,3>,2> anchorCowl{},anchorHeader{};
    cockpit_detail::WindowCorners(mesh, polygons, fit.windshieldZ, fit.windshieldY, fit.roofFrontZ, fit.roofFrontY,
                                  fit.halfWidth, false, anchorCowl, anchorHeader, false);
    std::vector<gt2::CarMeshVertex> frontWindowSurface;
    cockpit_detail::WindowCorners(mesh, polygons, fit.windshieldZ, fit.windshieldY, fit.roofFrontZ, fit.roofFrontY,
                                  fit.halfWidth, false, fit.frontCowl, fit.frontHeader, connectedWindow,
                                  connectedWindow?&frontWindowSurface:nullptr);
    cockpit_detail::WindowCorners(cabin, cabinPolygons, fit.rearWindowZ, fit.rearWindowY, fit.roofRearZ, fit.roofRearY,
                                  fit.halfWidth, true, fit.rearCowl, fit.rearHeader, false);
    auto widthOf = [](const auto& corners) { return (corners[1][0] - corners[0][0]) * 0.5f; };
    fit.windshieldHalfWidth = widthOf(fit.frontCowl); fit.roofFrontHalfWidth = widthOf(fit.frontHeader);
    for (size_t i=0; i<fit.cowl.size(); ++i) {
        const float x=fit.frontCowl[0][0]+(fit.frontCowl[1][0]-fit.frontCowl[0][0])*float(i)/float(fit.cowl.size()-1);
        const auto& corner=fit.frontCowl[x<0?0:1];
        const float side=std::clamp(x/corner[0],0.f,1.f);
        auto& point=fit.cowl[i];
        point={x,fit.windshieldY+(corner[1]-fit.windshieldY)*side,
               fit.windshieldZ+(corner[2]-fit.windshieldZ)*side};
        // Cross-sections must use the same connected windscreen as its corners.
        // Sampling the complete car here reintroduced mirror/quarter-window
        // peaks between otherwise correct endpoints.
        const auto section=cockpit_detail::CentreProfile(frontWindowSurface.empty()?mesh:std::span<const gt2::CarMeshVertex>(frontWindowSurface),x);
        float best=std::numeric_limits<float>::infinity();
        for (size_t j=0; j+1<section.size(); ++j) {
            const auto& a=section[j];
            if (a[0]<fit.windshieldZ-.2f || a[0]>fit.roofFrontZ || std::abs(a[1]-point[1])>.18f) continue;
            size_t end=j;
            while (end+1<section.size() && section[end+1][0]-section[end][0]>1e-5f &&
                   (section[end+1][1]-section[end][1])/(section[end+1][0]-section[end][0])>=.28f) ++end;
            if (end==j || section[end][1]-a[1]<.10f || section[end][1]<fit.roofFrontY-.15f) continue;
            const float score=std::abs(a[1]-fit.windshieldY)+.1f*std::abs(a[0]-fit.windshieldZ);
            if (score<best) { point={x,a[1],a[0]}; best=score; }
        }
    }
    fit.cowl.front()=fit.frontCowl[0]; fit.cowl.back()=fit.frontCowl[1];
    cockpit_detail::RemoveTrimSpikes(fit.cowl);
    for(size_t i=0;i<fit.cowl.size();++i) {
        auto point=fit.cowl[i];point[2]-=.18f;
        const float y=cockpit_detail::SurfaceHeight(mesh,point[2],point[0],point[1]+.012f);
        if(std::isfinite(y))point[1]=y;
        fit.bonnetJoin[i]=point;
    }
    fit.bonnetJoinValid=true;
    cockpit_detail::RemoveTrimSpikes(fit.bonnetJoin,true);
    cockpit_detail::FitBonnetSeal(fit,mesh);
    fit.roofRearHalfWidth = widthOf(fit.rearHeader); fit.rearWindowHalfWidth = widthOf(fit.rearCowl);
    fit.roofY = std::max(fit.roofFrontY, fit.roofRearY);
    for (int zi = 0; zi <= 12; ++zi)
        for (int xi = -3; xi <= 3; ++xi) {
            const float z = fit.roofFrontZ + (fit.roofRearZ - fit.roofFrontZ) * float(zi) / 12.0f;
            const float x = std::min(fit.roofFrontHalfWidth, fit.roofRearHalfWidth) * float(xi) * 0.24f;
            const float y = cockpit_detail::CabinSurfaceHeight(cabin, z, x);
            if (std::isfinite(y)) fit.roofY = std::max(fit.roofY, y);
        }
    const float headerZ = std::max(anchorHeader[0][2], anchorHeader[1][2]);
    const float eyeMinZ = headerZ + 0.12f;
    const float eyeMaxZ = std::max(eyeMinZ, fit.roofRearZ - 0.18f);
    fit.eye[0] = -0.34f * std::min(fit.halfWidth, widthOf(anchorCowl));
    fit.eye[2] = std::clamp(fit.windshieldZ + std::clamp(wheelbase * 0.25f, 0.55f, 0.72f), eyeMinZ, eyeMaxZ);
    int overhead = 0;
    for (int i = -2; i <= 2; ++i) {
        const float y = cockpit_detail::CabinSurfaceHeight(mesh, fit.eye[2] + float(i) * 0.06f, fit.eye[0]);
        if (std::isfinite(y) && y >= std::min(fit.roofFrontY, fit.roofRearY) - 0.15f) ++overhead;
    }
    fit.openTop = overhead < 3;
    const float seatRoof = cockpit_detail::CabinSurfaceHeight(mesh, fit.eye[2], fit.eye[0]);
    const float headRoof = std::isfinite(seatRoof) && !fit.openTop ? seatRoof : fit.roofFrontY;
    fit.eye[1] = headRoof - std::clamp((headRoof - fit.windshieldY) * 0.32f, 0.11f, 0.15f);
    if (fit.eye[1] <= fit.dashboardY + 0.06f) { fit.valid = false; return fit; }
    return fit;
}

inline std::vector<gt2::CarMeshVertex> ClipCockpitHood(std::span<const gt2::CarMeshVertex> body, const CockpitFit& fit) {
    std::vector<gt2::CarMeshVertex> out;
    if (!fit.valid) return out;
    out.reserve(body.size());
    for (size_t i = 0; i + 2 < body.size(); i += 3) {
        auto polygon = cockpit_detail::ClipPlane(body.subspan(i, 3), 2, fit.windshieldZ);
        polygon = cockpit_detail::ClipPlane(polygon, 1, fit.dashboardY);
        for (size_t k = 1; k + 1 < polygon.size(); ++k) {
            const auto& a = polygon[0]; const auto& b = polygon[k]; const auto& c = polygon[k + 1];
            const std::array<float, 3> u{b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
            const std::array<float, 3> v{c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
            const float nx = u[1] * v[2] - u[2] * v[1], ny = u[2] * v[0] - u[0] * v[2], nz = u[0] * v[1] - u[1] * v[0];
            if (nx * nx + ny * ny + nz * nz < 1e-14f) continue;
            out.push_back(a); out.push_back(b); out.push_back(c);
        }
    }
    return out;
}

} // namespace gt2view
