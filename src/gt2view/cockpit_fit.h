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
// Shared by the authored-eye calibration and camera presentation. The fit
// must validate the eye actually used by rendering, after this fixed offset.
inline constexpr float kCockpitDefaultSeatBack=.15f;
inline constexpr float kNativeCockpitEyeForward=.30f;
inline constexpr float kGeneratedCockpitEyeForward=.10f;


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
    std::array<std::array<float,3>,17> rearCowlProfile{};
    bool rearCowlProfileValid=false;
    bool bonnetJoinValid = false;
    std::array<std::array<std::array<float,3>,17>,5> bonnetSeal{};
    bool bonnetSealValid=false;
    // Outer door-card boundary, front to back, with uniform Z stations per side.
    std::array<std::array<std::array<float, 3>, 17>, 2> sideSill{};
    std::array<std::vector<std::array<float,3>>,2> backingSideContour;
    bool sideSillValid = false;
    std::array<std::array<std::array<float,3>,13>,9> headliner{};
    std::vector<std::array<std::array<float,3>,3>> headlinerSurface;
    bool headlinerValid=false;
    std::vector<std::array<std::array<float,3>,3>> liningApertures;
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

// Reconcile the dense source contour after mirror sightline fitting has
// lowered the final coarse belt. Include every final belt knot so compaction
// cannot bridge a newly introduced notch. Neither profile may be raised.
inline std::vector<std::array<float,3>> FinalCockpitBackingSide(const CockpitFit& fit,size_t side) {
    const auto& dense=fit.backingSideContour[side];
    if(dense.empty())return {};
    std::vector<float> knots;
    for(const auto& q:dense)knots.push_back(q[2]);
    for(const auto& q:fit.sideSill[side])if(q[2]>dense.front()[2] && q[2]<dense.back()[2])knots.push_back(q[2]);
    std::sort(knots.begin(),knots.end());knots.erase(std::unique(knots.begin(),knots.end()),knots.end());
    std::vector<std::array<float,3>> result;
    size_t at=0;
    for(float z:knots) {
        while(at+2<dense.size() && dense[at+1][2]<z)++at;
        auto q=dense[at];
        if(at+1<dense.size()) {
            const auto& b=dense[at+1];const float t=std::clamp((z-q[2])/std::max(.000000001f,b[2]-q[2]),0.f,1.f);
            for(size_t axis=0;axis<3;++axis)q[axis]+=t*(b[axis]-q[axis]);
        }
        q[1]=std::min(q[1],CockpitSideSillAt(fit,side,z)[1]);result.push_back(q);
    }
    return result;
}

// At a longitudinal station, use the connected vertical glass band above
// the independently fitted cowl. Separate transparent trim below that band
// cannot lower the door lining. The returned W is the top of that band.
// A sloping front side pane need not reach the windscreen header chord.
// Prove it belongs to the same source aperture as glass behind the header;
// disconnected lamp/wing transparency cannot supply that evidence.
inline std::vector<uint8_t> CockpitConnectedSideGlass(const CockpitFit& fit,
        std::span<const gt2::CarMeshVertex> openings) {
    const size_t count=openings.size()/3;
    std::vector<size_t> parent(count);for(size_t i=0;i<count;++i)parent[i]=i;
    const auto root=[&](size_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
    std::map<std::array<int,3>,size_t> owners;
    std::vector<uint8_t> eligible(count),seed(count),connected(count);
    for(size_t i=0;i<count;++i){const auto t=openings.subspan(i*3,3);
        if(!CockpitSideOpening(t,fit))continue;eligible[i]=1;
        const size_t side=t[0].pos[0]>=0;
        for(const auto& q:t){
            const std::array<int,3> key{int(std::lround(q.pos[0]*10000)),int(std::lround(q.pos[1]*10000)),int(std::lround(q.pos[2]*10000))};
            const auto [at,inserted]=owners.emplace(key,i);if(!inserted)parent[root(i)]=root(at->second);
            const auto& front=fit.frontCowl[side];const auto& rear=fit.rearCowl[side];
            const float u=std::clamp((q.pos[2]-front[2])/std::max(.01f,rear[2]-front[2]),0.f,1.f);
            const float outer=std::min(std::abs(front[0]+u*(rear[0]-front[0]))-.025f,
                std::min(std::abs(fit.frontHeader[side][0]),std::abs(fit.rearHeader[side][0])));
            if(q.pos[2]>=fit.frontHeader[side][2] && q.pos[2]<=fit.rearCowl[side][2] &&
               std::abs(q.pos[0])>=outer &&
               q.pos[1]>std::min(front[1],rear[1])+.025f)seed[i]=1;
        }
    }
    // A tapered pane can cross the seed region without any original
    // vertex lying inside it (low outer corners, high inboard corners).
    // Test its surface, not just its tessellation vertices.
    for(size_t i=0;i<count;++i)if(eligible[i] && !seed[i]) {
        const auto tri=openings.subspan(i*3,3);const size_t side=tri[0].pos[0]>=0;
        const float sign=side?1.f:-1.f;
        std::vector<std::array<float,3>> poly;
        for(const auto& q:tri)poly.push_back({q.pos[0],q.pos[1],q.pos[2]});
        const auto clip=[&](auto distance){
            std::vector<std::array<float,3>> next;if(poly.empty())return;
            auto a=poly.back();float da=distance(a);
            for(const auto& b:poly){const float db=distance(b);
                if((da>=0)!=(db>=0)){auto q=a;const float u=da/(da-db);for(size_t k=0;k<3;++k)q[k]+=u*(b[k]-a[k]);next.push_back(q);}
                if(db>=0)next.push_back(b);a=b;da=db;
            }poly=std::move(next);
        };
        clip([&](const auto& q){return q[2]-fit.frontHeader[side][2];});
        clip([&](const auto& q){return fit.rearCowl[side][2]-q[2];});
        clip([&](const auto& q){return q[1]-std::min(fit.frontCowl[side][1],fit.rearCowl[side][1])-.0251f;});
        clip([&](const auto& q){return sign*q[0]-std::min(std::abs(fit.frontHeader[side][0]),std::abs(fit.rearHeader[side][0]));});
        if(poly.size()>=3)seed[i]=1;
    }
    // UV/material splits can make a T-junction: a pane vertex lands on
    // another triangle's edge without sharing its endpoint. Join only actual
    // adjacent source surfaces, never an inboard driver silhouette.
    const auto touches=[&](size_t i,size_t j){
        const auto a=openings.subspan(i*3,3),b=openings.subspan(j*3,3);
        const auto inside=[](const auto& point,const auto& triangle){
            std::array<double,3> u{},v{},n{},q{};double length2=0;
            for(size_t k=0;k<3;++k){u[k]=triangle[1].pos[k]-triangle[0].pos[k];v[k]=triangle[2].pos[k]-triangle[0].pos[k];q[k]=point.pos[k]-triangle[0].pos[k];}
            for(size_t k=0;k<3;++k){n[k]=u[(k+1)%3]*v[(k+2)%3]-u[(k+2)%3]*v[(k+1)%3];length2+=n[k]*n[k];}
            if(length2<1e-20)return false;double d=0;for(size_t k=0;k<3;++k)d+=q[k]*n[k];
            if(d*d>1e-6*length2)return false;
            size_t axis=0;for(size_t k=1;k<3;++k)if(std::abs(n[k])>std::abs(n[axis]))axis=k;
            const size_t x=(axis+1)%3,y=(axis+2)%3;const double determinant=u[x]*v[y]-u[y]*v[x];
            const double a=(q[x]*v[y]-q[y]*v[x])/determinant,b=(u[x]*q[y]-u[y]*q[x])/determinant;
            return a>=-1e-5 && b>=-1e-5 && a+b<=1.00001;
        };
        for(const auto& point:a)if(inside(point,b))return true;
        for(const auto& point:b)if(inside(point,a))return true;
        return false;
    };
    std::map<std::array<int,3>,std::vector<size_t>> cells;
    for(size_t i=0;i<count;++i)if(eligible[i]){
        std::array<int,3> low{},high{};
        for(size_t k=0;k<3;++k){float a=1e9f,b=-1e9f;for(size_t corner=0;corner<3;++corner){const float q=openings[i*3+corner].pos[k];a=std::min(a,q);b=std::max(b,q);}low[k]=int(std::floor((a-.001f)*4));high[k]=int(std::floor((b+.001f)*4));}
        std::vector<size_t> candidates;
        for(int x=low[0];x<=high[0];++x)for(int y=low[1];y<=high[1];++y)for(int z=low[2];z<=high[2];++z){auto& bucket=cells[{x,y,z}];candidates.insert(candidates.end(),bucket.begin(),bucket.end());bucket.push_back(i);}
        std::sort(candidates.begin(),candidates.end());candidates.erase(std::unique(candidates.begin(),candidates.end()),candidates.end());
        for(size_t j:candidates)if(root(i)!=root(j) && touches(i,j))parent[root(i)]=root(j);
    }
    for(size_t i=0;i<count;++i)if(seed[i])connected[root(i)]=1;
    for(size_t i=0;i<count;++i)seed[i]=uint8_t(eligible[i] && connected[root(i)]);
    return seed;
}
inline std::array<float,4> CockpitUpperSideApertureAt(const CockpitFit& fit,
        std::span<const gt2::CarMeshVertex> openings,size_t side,float z,
        std::span<const uint8_t> connectedGlass={}) {
    const auto& front=fit.frontCowl[side];const auto& rear=fit.rearCowl[side];
    const float t=std::clamp((z-front[2])/std::max(.01f,rear[2]-front[2]),0.f,1.f);
    const float baseline=front[1]+(rear[1]-front[1])*t;
    std::vector<std::array<float,6>> intervals;
    for(size_t at=0;at+2<openings.size();at+=3) {
        const auto triangle=openings.subspan(at,3);
        if(!CockpitSideOpening(triangle,fit) || size_t(triangle[0].pos[0]>=0)!=side)continue;
        float low=std::numeric_limits<float>::infinity(),high=-low,x=0,xHigh=0;
        for(size_t edge=0;edge<3;++edge) {
            const auto* a=triangle[edge].pos;const auto* b=triangle[(edge+1)%3].pos;
            if(z<std::min(a[2],b[2])-1e-6f || z>std::max(a[2],b[2])+1e-6f)continue;
            if(std::abs(b[2]-a[2])<1e-7f) {
                for(const auto* q:{a,b}){if(q[1]<low){low=q[1];x=q[0];}if(q[1]>high){high=q[1];xHigh=q[0];}}
            } else {
                const float u=std::clamp((z-a[2])/(b[2]-a[2]),0.f,1.f);
                const float y=a[1]+(b[1]-a[1])*u;
                if(y<low){low=y;x=a[0]+(b[0]-a[0])*u;}if(y>high){high=y;xHigh=a[0]+(b[0]-a[0])*u;}
            }
        }
        if(std::isfinite(low))intervals.push_back({x,low,z,high,xHigh,at/3<connectedGlass.size()?float(connectedGlass[at/3]):0.f});
    }
    std::sort(intervals.begin(),intervals.end(),[](const auto& a,const auto& b){return a[1]<b[1];});
    // Height overlap alone joins a transparent driver/seat carrier to the
    // real side glass. Require spatial continuity in the X/Y cross-section;
    // then select the outermost glass band, not an inboard silhouette hole.
    std::vector<size_t> parents(intervals.size());for(size_t i=0;i<parents.size();++i)parents[i]=i;
    const auto root=[&](size_t i){while(parents[i]!=i){parents[i]=parents[parents[i]];i=parents[i];}return i;};
    const auto xAt=[](const auto& q,float y){const float u=std::abs(q[3]-q[1])<1e-7f?0.f:std::clamp((y-q[1])/(q[3]-q[1]),0.f,1.f);return q[0]+(q[4]-q[0])*u;};
    for(size_t i=0;i<intervals.size();++i)for(size_t j=i+1;j<intervals.size();++j){
        const auto& a=intervals[i];const auto& b=intervals[j];// Narrow opaque trim can split one window into upper glass and a
        // lower clear strip. Its existing 25 mm stitch band must bridge that
        // trim as well as touching alpha partitions on the same surface.
        if(b[1]>a[3]+.025f)break;
        const float low=std::max(a[1],b[1]),high=std::min(a[3],b[3]);
        if(std::abs(xAt(a,low)-xAt(b,low))<=.025f && std::abs(xAt(a,high)-xAt(b,high))<=.025f)parents[root(j)]=root(i);
    }
    std::vector<std::array<float,4>> bands(intervals.size(),{0,std::numeric_limits<float>::infinity(),z,-std::numeric_limits<float>::infinity()});
    for(size_t i=0;i<intervals.size();++i){auto& band=bands[root(i)];const auto& q=intervals[i];
        if(q[1]<band[1] || (q[1]==band[1] && std::abs(q[0])>std::abs(band[0]))){band[0]=q[0];band[1]=q[1];}band[3]=std::max(band[3],q[3]);}
    std::vector<uint8_t> connectedBand(intervals.size());
    for(size_t i=0;i<intervals.size();++i)if(intervals[i][5]>0)connectedBand[root(i)]=1;
    std::array<float,4> result{0,baseline,z,-std::numeric_limits<float>::infinity()};
    const float shellChord=std::abs(front[0]+(rear[0]-front[0])*t);
    // When a pillar blocks the outer window, an inboard driver transparency
    // is not replacement evidence. Leave that station unmeasured so the
    // neighbouring real apertures bridge the opaque pillar normally.
    // Ahead of the A-pillar, a headlamp/wing aperture can rise above the
    // door-belt chord without reaching the sloping windscreen. Require the
    // upper band to reach that measured front-glass envelope as well.
    float requiredTop=baseline+.0001f;
    const auto& header=fit.frontHeader[side];
    if(header[2]>front[2]+.025f && z<header[2] && header[1]>front[1]) {
        const float u=std::clamp((z-front[2])/(header[2]-front[2]),0.f,1.f);
        requiredTop=std::max(requiredTop,front[1]+u*(header[1]-front[1])-.025f);
    }
    // A sub-stitch-width isolated transparency cannot locate a door belt.
    // Tiny alpha islands at the A-pillar otherwise lift a full sill station
    // to the roof. Leave those stations unmeasured and bridge actual glass.
    for(size_t bandIndex=0;bandIndex<bands.size();++bandIndex){const auto& band=bands[bandIndex];
    if(band[3]-band[1]>.025f && (connectedBand[bandIndex] || band[3]>requiredTop) && (connectedBand[bandIndex] || std::abs(band[0])>=std::min(shellChord-.025f, std::min(std::abs(fit.frontHeader[side][0]),std::abs(fit.rearHeader[side][0])))) &&
        (std::abs(band[0])>std::abs(result[0])+.0001f || (std::abs(std::abs(band[0])-std::abs(result[0]))<=.0001f && band[3]>result[3])))result=band;}
    return result;
}
inline void FitCockpitSideSills(CockpitFit& fit, std::span<const gt2::CarMeshVertex> openings,
                               bool upperApertureOnly=false,bool sourceExposed=false,
                               std::span<const uint8_t> exposedSeeds={}) {

    if (!fit.valid) return;
    fit.sideSillValid=false;
    for(auto& contour:fit.backingSideContour)contour.clear();
    // Independently exposed source glazing needs no donor-width seed:
    // curved windows can be narrower than the provisional header chord.
    auto connectedGlass=sourceExposed?std::vector<uint8_t>(openings.size()/3,1):
        (upperApertureOnly?CockpitConnectedSideGlass(fit,openings):std::vector<uint8_t>{});
    // Original connected panes remain valid even when only part of their
    // tessellation is directly exposed. Exposure supplements that evidence.
    if(upperApertureOnly && exposedSeeds.size()==connectedGlass.size())
        for(size_t i=0;i<connectedGlass.size();++i)connectedGlass[i]|=exposedSeeds[i];
    for (size_t side=0;side<2;++side) {
        auto& sill=fit.sideSill[side];
        std::array<bool,17> measured{};
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
            if(upperApertureOnly) {
                const auto band=CockpitUpperSideApertureAt(fit,openings,side,sill[i][2],connectedGlass);
                lowest=std::isfinite(band[3])?band[1]:std::numeric_limits<float>::infinity();
                edgeX=band[0];
            }
            if (std::isfinite(lowest)) {
                measured[i]=true;
                // A measured upper aperture can rise above the cowl chord.
                // Capping it to that chord exposes the original outer door
                // above the generated lining on tall-window bodies.
                sill[i][1]=upperApertureOnly?lowest:std::min(sill[i][1],lowest);
                // A measured side window can taper inward from the front/rear cowl chord.
                // Use its actual X coordinate for upper-shell fitting.
                if (upperApertureOnly || std::abs(edgeX)>std::abs(sill[i][0])) sill[i][0]=edgeX;
            }
        }
        // A pillar has no transparent texels. It must not pull the door
        // back to the narrower cowl chord at that station. Bridge gaps between
        // measured apertures or the fixed cowl endpoints; preserve endpoints.
        if(upperApertureOnly)for(size_t i=1;i+1<sill.size();++i)if(!measured[i]) {
            size_t before=i,after=i;
            while(before>0 && !measured[before])--before;
            while(after+1<sill.size() && !measured[after])++after;
            // The independently fitted front/rear cowl are endpoint anchors.
            // Leading/trailing gaps must join those anchors to real glass,
            // rather than retaining an unrelated whole-cabin height chord.
            if((!measured[before] && before!=0) ||
               (!measured[after] && after+1!=sill.size()))continue;
            const float t=(sill[i][2]-sill[before][2])/(sill[after][2]-sill[before][2]);
            for(size_t axis=0;axis<2;++axis)sill[i][axis]=sill[before][axis]+(sill[after][axis]-sill[before][axis])*t;
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
                if(upperApertureOnly && vertex.pos[1]<y) {
                    const auto band=CockpitUpperSideApertureAt(fit,openings,side,vertex.pos[2],connectedGlass);
                    // At a tapered window tip the vertical section has zero
                    // height. Connectivity already proves this is real glass;
                    // do not discard its lower corner as a tiny alpha island.
                    if((at/3>=connectedGlass.size() || !connectedGlass[at/3]) &&
                       (!std::isfinite(band[3]) || vertex.pos[1]<band[1]-.0001f))continue;
                }
                const float lower=std::max(0.f,y-vertex.pos[1]);
                if(!upperApertureOnly){sill[index][1]-=lower;sill[index+1][1]-=lower;}
                else if(lower>0) {
                    // Lower the high endpoint responsible for the overshoot.
                    // Repeated uniform subtraction depresses the already-low
                    // endpoint below every measured corner and leaves a notch.
                    auto& a=sill[index][1];auto& b=sill[index+1][1];
                    if(t<=1e-6f)a=vertex.pos[1];
                    else if(t>=1.f-1e-6f)b=vertex.pos[1];
                    else if(a>=b && a-lower/(1.f-t)>=b)a-=lower/(1.f-t);
                    else if(b>a && b-lower/t>=a)b-=lower/t;
                    else {a=std::min(a,vertex.pos[1]);b=std::min(b,vertex.pos[1]);}
                }
            }
        }
        // A small upper aperture above an opaque pillar is not the door belt.
        // Its isolated upward/inward point must bridge the neighbouring lower
        // glass boundaries, just as an unmeasured opaque pillar does.
        if(upperApertureOnly)for(size_t pass=0;pass<sill.size();++pass)for(size_t i=1;i+1<sill.size();++i){
            const auto& a=sill[i-1];const auto& b=sill[i+1];auto& q=sill[i];
            if(q[1]>std::max(a[1],b[1])+.025f && std::abs(q[0])<std::min(std::abs(a[0]),std::abs(b[0]))-.025f){
                const float t=(q[2]-a[2])/std::max(.0001f,b[2]-a[2]);
                for(size_t axis=0;axis<2;++axis)q[axis]=a[axis]+t*(b[axis]-a[axis]);
            }
        }
        if(upperApertureOnly) {
            // The independently measured cowl borders are the shared anchors
            // of the front, rear and side enclosure. Do not replace them with
            // a higher pane seen at the same longitudinal coordinate.
            const auto anchor=[](auto& measured,const auto& cowl) {
                if(measured[1]>cowl[1]+.025f && std::abs(measured[0])<std::abs(cowl[0])-.025f)measured=cowl;
            };
            anchor(sill.front(),front);anchor(sill.back(),rear);
            // A curved upper pane can occupy several stations. Apply the
            // existing high/inboard island rule to the whole run, rather than
            // requiring one sample to stand above its immediately adjacent
            // samples (which are themselves part of that same island).
            for(size_t span=sill.size()-1;span>=2;--span)for(size_t begin=0;begin+span<sill.size();++begin) {
                const size_t end=begin+span;const auto a=sill[begin],b=sill[end];bool island=true,raised=false;
                for(size_t j=begin+1;j<end;++j) {
                    const float t=float(j-begin)/float(span);
                    const float y=a[1]+t*(b[1]-a[1]);const float x=a[0]+t*(b[0]-a[0]);
                    island &= sill[j][1]>=y-.0001f && std::abs(sill[j][0])<std::abs(x)-.025f;
                    raised |= sill[j][1]>y+.025f;
                }
                if(island && raised)for(size_t j=begin+1;j<end;++j) {
                    const float t=float(j-begin)/float(span);
                    for(size_t axis=0;axis<2;++axis)sill[j][axis]=a[axis]+t*(b[axis]-a[axis]);
                }
            }
        }
    }
    fit.sideSillValid=true;
    // A uniformly sampled chord cannot preserve every bend in a source
    // window. Measure the lower glass edge at all authored longitudinal
    // corners for the closed backing only. Keep the established furnishing
    // profile independent: a structural refinement must not move door trim
    // or the dashboard into a different part of a swept source window.
    if(upperApertureOnly)for(size_t side=0;side<2;++side) {
        const auto& fixed=fit.sideSill[side];
        std::vector<float> knots;
        for(const auto& q:fixed)knots.push_back(q[2]);
        for(size_t at=0;at+2<openings.size();at+=3) {
            const auto t=openings.subspan(at,3);
            if(!connectedGlass[at/3] || !CockpitSideOpening(t,fit) || size_t(t[0].pos[0]>=0)!=side)continue;
            for(const auto& q:t)if(q.pos[2]>fixed.front()[2] && q.pos[2]<fixed.back()[2])knots.push_back(q.pos[2]);
        }
        std::sort(knots.begin(),knots.end());
        knots.erase(std::unique(knots.begin(),knots.end(),[](float a,float b){return std::abs(a-b)<.000001f;}),knots.end());
        std::vector<std::array<float,3>> contour;
        for(float z:knots) {
            auto q=CockpitSideSillAt(fit,side,z);
            const auto band=CockpitUpperSideApertureAt(fit,openings,side,z,connectedGlass);
            if(z>fixed.front()[2]+.000001f && z<fixed.back()[2]-.000001f && std::isfinite(band[3])) {
                const bool pillarIsland=band[1]>q[1]+.025f && std::abs(band[0])<std::abs(q[0])-.025f;
                // Refine only downward notches missed by the sampled belt.
                // The coarse fit has already rejected upper/inboard islands;
                // a dense query must not raise it or move the shell inward.
                if(!pillarIsland)q[1]=std::min(q[1],band[1]);
            }
            contour.push_back(q);
        }
        // UV tessellation adds many longitudinal stations on the same
        // straight source edge. Retain bends, discard numerically collinear
        // subdivisions before generating and clipping the interior mesh.
        std::vector<std::array<float,3>> compact;
        for(const auto& q:contour) {
            while(compact.size()>1) {
                const auto& a=compact[compact.size()-2];const auto& b=compact.back();
                const double t=double(b[2]-a[2])/std::max(.000000001,double(q[2]-a[2]));
                double error=0;
                for(size_t axis=0;axis<2;++axis) {
                    const double d=double(b[axis])-double(a[axis])-t*double(q[axis]-a[axis]);error+=d*d;
                }
                if(error>1e-10)break;
                compact.pop_back();
            }
            compact.push_back(q);
        }
        fit.backingSideContour[side]=std::move(compact);
    }
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
