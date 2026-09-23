#pragma once
#include "gt2view/cockpit_fit.h"
#include "gt2view/cockpit_window_cut.h"
#include "gt2view/cockpit_materials.h"
#include "gt2formats/car_texture.h"

namespace gt2view {

// Separate CLUT columns keep the normal exterior and every paint unchanged.
constexpr uint32_t kCockpitGlazingPalette = 17;
constexpr uint32_t kCockpitLiningPalette = 33;

inline uint32_t CockpitSourcePalette(uint32_t palette) {
    if (palette >= kCockpitLiningPalette) return palette - kCockpitLiningPalette;
    if (palette >= kCockpitGlazingPalette) return palette - kCockpitGlazingPalette;
    return palette;
}

inline void CockpitLining(gt2::CarMeshVertex& vertex) {
    if (vertex.textured) {
        vertex.palette += uint8_t(kCockpitLiningPalette);
        vertex.rawTexture = true;
    } else {
        vertex.rawTexture = false;
        std::copy(kCockpitInteriorRgb.begin(),kCockpitInteriorRgb.end(),vertex.color);
    }
}

inline std::array<uint16_t, 16> CockpitGlassMasks(const gt2::CarTexture& texture) {
    std::array<uint16_t, 16> masks{};
    if (texture.paints.empty()) return masks;
    for (size_t palette = 0; palette < masks.size(); ++palette) {
        for (size_t index = 0; index < 16; ++index) {
            const auto color = texture.paints.front().cluts[palette][index] & 0x7fffu;
            const int r = color & 31, g = (color >> 5) & 31, b = (color >> 10) & 31;
            // The glazing ramp includes teal and its darkest green shades, independent of body paint.
            if (g < b || b < r || g <= r || g - r > 2 || g > 12) continue;
            const bool invariant = std::all_of(texture.paints.begin(), texture.paints.end(), [&](const auto& paint) {
                return (paint.cluts[palette][index] & 0x7fffu) == color;
            });
            if (invariant) masks[palette] |= uint16_t(1u << index);
        }
    }
    return masks;
}

namespace cockpit_detail {
inline bool OutsideCabinShell(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit) {
    bool left = true, right = true;
    std::array<float, 2> nearest{std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    std::array<float, 2> farthest{}, distanceSum{};
    std::array<size_t, 2> nearCorners{};
    size_t uniqueCorners = 0;
    float maxY = -std::numeric_limits<float>::infinity(), maxZ = maxY;
    for (size_t at = 0; at < face.size(); ++at) {
        const auto& vertex = face[at];
        bool repeated = false;
        for (size_t before = 0; before < at; ++before)
            repeated |= std::equal(vertex.pos, vertex.pos + 3, face[before].pos);
        if (repeated) continue;
        ++uniqueCorners;
        maxY = std::max(maxY, vertex.pos[1]); maxZ = std::max(maxZ, vertex.pos[2]);
        for (size_t side = 0; side < 2; ++side) {
            auto along = [&](const auto& front, const auto& back) {
                const float t = std::clamp((vertex.pos[2] - front[2]) / std::max(.1f, back[2] - front[2]), 0.f, 1.f);
                return std::array<float, 2>{front[0] + (back[0] - front[0]) * t, front[1] + (back[1] - front[1]) * t};
            };
            const auto lower = along(fit.frontCowl[side], fit.rearCowl[side]);
            const auto upper = along(fit.frontHeader[side], fit.rearHeader[side]);
            const float height = std::clamp((vertex.pos[1] - lower[1]) / std::max(.1f, upper[1] - lower[1]), 0.f, 1.f);
            const float edge = lower[0] + (upper[0] - lower[0]) * height;
            const float distance = (vertex.pos[0] - edge) * (side ? 1.f : -1.f);
            if (side == 0) left &= distance > .035f;
            else right &= distance > .035f;
            nearest[side] = std::min(nearest[side], distance);
            farthest[side] = std::max(farthest[side], distance);
            distanceSum[side] += distance;
            nearCorners[side] += distance <= .035f;
        }
    }
    if (left || right) return true;
    // A mirror can share one mounting vertex with the lower front corner of the cabin.
    if (uniqueCorners >= 3 && maxY < fit.roofFrontY - .13f && maxZ < fit.roofFrontZ + .20f)
        for (size_t side = 0; side < 2; ++side)
            if (nearest[side] >= -.02f && nearCorners[side] <= 1 && farthest[side] > .12f &&
                distanceSum[side] / float(uniqueCorners) > .07f) return true;
    return false;
}

inline float GlassCoverage(std::span<const gt2::CarMeshVertex> triangle, const gt2::CarTexture& texture,
                           const std::array<uint16_t, 16>& masks) {
    if (triangle.size() != 3 || !triangle[0].textured || texture.indices.size() != 256 * 224) return 0;
    const auto mask = masks[triangle[0].palette];
    if (!mask) return 0;
    int samples = 0, glass = 0;
    for (int a = 0; a <= 8; ++a) for (int b = 0; b <= 8 - a; ++b) {
        const float weights[] = {float(a) / 8, float(b) / 8, float(8 - a - b) / 8};
        float u = 0, v = 0;
        for (size_t k = 0; k < 3; ++k) {
            u += triangle[k].texel[0] * weights[k];
            v += triangle[k].texel[1] * weights[k];
        }
        const auto x = std::clamp(int(std::floor(u)), 0, 255), y = std::clamp(int(std::floor(v)), 0, 223);
        glass += (mask >> texture.indices[size_t(y) * 256 + x]) & 1;
        ++samples;
    }
    return float(glass) / float(samples);
}

inline bool RoofPanel(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit) {
    const float height=std::min(fit.roofFrontY,fit.roofRearY)-.06f;
    for (const auto& v:face)
        if (v.pos[1]<height || v.pos[2]<fit.roofFrontZ-.05f || v.pos[2]>fit.roofRearZ+.05f) return false;
    for (size_t i=0;i<face.size();i+=3) {
        const auto* a=face[i].pos; const auto* b=face[i+1].pos; const auto* c=face[i+2].pos;
        const float nx=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        const float ny=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        const float nz=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
        if (ny*ny<.81f*(nx*nx+ny*ny+nz*nz)) return false;
    }
    return true;
}

inline bool InnerSidewall(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit) {
    if (face.empty()) return false;
    const size_t side=face[0].pos[0]>=0;
    const float sign=side?1.f:-1.f;
    float minY=std::numeric_limits<float>::infinity(),maxY=-minY,maxZ=-minY,centerZ=0;
    for (const auto& vertex:face) {
        // End panels and bonnet faces cross the car centre; wing mirrors sit
        // outside the body or entirely above its door belt.
        if (vertex.pos[0]*sign<fit.halfWidth*.40f || vertex.pos[0]*sign>fit.halfWidth+.06f) return false;
        minY=std::min(minY,vertex.pos[1]); maxY=std::max(maxY,vertex.pos[1]);
        maxZ=std::max(maxZ,vertex.pos[2]);
        centerZ+=vertex.pos[2]/float(face.size());
    }
    const bool lowerFrontOverlap=maxZ>=fit.frontCowl[side][2]+.10f && maxY<=fit.frontCowl[side][1]+.02f;
    if ((!lowerFrontOverlap && centerZ<fit.frontCowl[side][2]+.02f) ||
        centerZ>fit.rearZ || maxY<fit.floorY+.10f) return false;
    const float along=std::clamp((centerZ-fit.frontCowl[side][2])/
        std::max(.1f,fit.rearCowl[side][2]-fit.frontCowl[side][2]),0.f,1.f);
    const float belt=fit.frontCowl[side][1]+(fit.rearCowl[side][1]-fit.frontCowl[side][1])*along;
    return minY<=belt+.02f;
}

inline bool ExtendedWindowFace(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit, float glassCoverage) {
    if (glassCoverage<.25f || face.size()<3) return false;
    std::array<float,3> lo{},hi{},center{};
    std::copy(face[0].pos,face[0].pos+3,lo.begin()); hi=lo;
    for (const auto& vertex:face) for (size_t axis=0;axis<3;++axis) {
        lo[axis]=std::min(lo[axis],vertex.pos[axis]); hi[axis]=std::max(hi[axis],vertex.pos[axis]);
        center[axis]+=vertex.pos[axis]/float(face.size());
    }
    const float belt=std::min(fit.frontCowl[0][1],fit.frontCowl[1][1]);
    if (center[2]<fit.windshieldZ-.10f || center[2]>fit.rearZ-.12f ||
        hi[1]<belt+.15f || center[1]<belt-.06f) return false;
    // Curved screens and tall rear quarters can extend outside the four-corner
    // approximation. Real window surfaces remain much longer than mirror glass.
    if (IsCockpitEndWindowFace(face)) return true;
    return hi[2]-lo[2]>=.25f && hi[1]-lo[1]>=.18f &&
        (lo[0]>fit.halfWidth*.30f || hi[0]<-fit.halfWidth*.30f) &&
        (CockpitSideOpening(face.first(3),fit) || (face.size()==6 && CockpitSideOpening(face.last(3),fit)));
}

inline bool HeaderTrim(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit, float glassCoverage) {
    if (glassCoverage<=0.f || glassCoverage>=.50f) return false;
    const float lower=fit.windshieldY+(fit.roofFrontY-fit.windshieldY)*.60f;
    return std::all_of(face.begin(),face.end(),[&](const auto& vertex) { return vertex.pos[1]>=lower; });
}

inline bool WindowOverlay(std::span<const gt2::CarMeshVertex> face, std::span<const gt2::CarMeshVertex> pane) {
    if (!face[0].textured) return false;
    auto onPane=[&](const float* point) {
        for (size_t i=0;i+2<pane.size();i+=3) {
            if (face[0].palette==pane[i].palette) continue;
            std::array<float,3> a{},b{},p{};
            for (size_t k=0;k<3;++k) {
                a[k]=pane[i+1].pos[k]-pane[i].pos[k];
                b[k]=pane[i+2].pos[k]-pane[i].pos[k];
                p[k]=point[k]-pane[i].pos[k];
            }
            auto dot=[](const auto& x,const auto& y) { return x[0]*y[0]+x[1]*y[1]+x[2]*y[2]; };
            const float aa=dot(a,a),ab=dot(a,b),bb=dot(b,b),pa=dot(p,a),pb=dot(p,b);
            const float determinant=aa*bb-ab*ab;
            if (determinant<1e-10f) continue;
            const float u=(bb*pa-ab*pb)/determinant,v=(aa*pb-ab*pa)/determinant;
            if (u<-.025f || v<-.025f || u+v>1.025f) continue;
            float distance2=0;
            for (size_t k=0;k<3;++k) { const float d=p[k]-u*a[k]-v*b[k]; distance2+=d*d; }
            // Authored decals can sit slightly above curved glass to avoid z-fighting.
            if (distance2<=.025f*.025f) return true;
        }
        return false;
    };
    for (const auto& vertex:face) if (!onPane(vertex.pos)) return false;
    for (size_t i=0;i+2<face.size();i+=3) {
        std::array<float,3> center{};
        for (size_t k=0;k<3;++k) center[k]=(face[i].pos[k]+face[i+1].pos[k]+face[i+2].pos[k])/3.f;
        if (!onPane(center.data())) return false;
    }
    return true;
}

inline bool InteriorCentrePlane(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit) {
    if (face.empty() || face[0].textured) return false;
    std::array<float,3> lo{},hi{};
    std::copy(face[0].pos,face[0].pos+3,lo.begin()); hi=lo;
    for (const auto& vertex:face) for (size_t axis=0;axis<3;++axis) {
        lo[axis]=std::min(lo[axis],vertex.pos[axis]);hi[axis]=std::max(hi[axis],vertex.pos[axis]);
    }
    // These structural partitions are hidden inside the closed retail body.
    // The cockpit replaces their cabin portion with its floor and console.
    return hi[0]-lo[0]<.04f && std::max(std::abs(lo[0]),std::abs(hi[0]))<fit.halfWidth*.20f &&
        hi[1]-lo[1]>.25f && hi[1]<fit.windshieldY+.12f && hi[2]-lo[2]>.50f &&
        lo[2]<fit.windshieldZ && hi[2]>fit.windshieldZ+.10f;
}

inline bool InteriorRaisedFloor(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit) {
    if (face.empty() || face[0].textured) return false;
    std::array<float,3> lo{},hi{};
    std::copy(face[0].pos,face[0].pos+3,lo.begin());hi=lo;
    for (const auto& vertex:face) for (size_t axis=0;axis<3;++axis) {
        lo[axis]=std::min(lo[axis],vertex.pos[axis]);hi[axis]=std::max(hi[axis],vertex.pos[axis]);
    }
    return hi[0]-lo[0]>fit.halfWidth*1.2f && hi[1]-lo[1]<.08f &&
        lo[1]>fit.floorY+.08f && hi[1]<fit.windshieldY-.05f && hi[2]-lo[2]>.40f &&
        hi[2]>fit.windshieldZ+.10f && lo[2]<fit.rearWindowZ-.10f;
}

inline bool InteriorChassisBulkhead(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit) {
    if (face.empty() || face[0].textured) return false;
    std::array<float,3> lo{},hi{};
    std::copy(face[0].pos,face[0].pos+3,lo.begin());hi=lo;
    for (const auto& vertex:face) for (size_t axis=0;axis<3;++axis) {
        lo[axis]=std::min(lo[axis],vertex.pos[axis]);hi[axis]=std::max(hi[axis],vertex.pos[axis]);
    }
    // The vertical ends of a raised retail chassis box occupy the same cabin
    // as its lid. The cockpit supplies the visible firewall and footwell.
    return hi[0]-lo[0]>fit.halfWidth*1.2f && hi[2]-lo[2]<.10f &&
        lo[1]<=fit.floorY+.04f && hi[1]>fit.floorY+.15f && hi[1]<fit.windshieldY-.05f &&
        lo[2]>fit.windshieldZ+.10f && hi[2]<fit.rearWindowZ-.10f;
}

inline bool SharesEdge(std::span<const gt2::CarMeshVertex> a, std::span<const gt2::CarMeshVertex> b) {
    size_t shared=0;
    for (size_t at=0;at<a.size();++at) {
        bool repeated=false;
        for (size_t before=0;before<at;++before)
            repeated |= std::equal(a[at].pos,a[at].pos+3,a[before].pos);
        if (repeated) continue;
        shared+=std::any_of(b.begin(),b.end(),[&](const auto& vertex) {
            return std::equal(a[at].pos,a[at].pos+3,vertex.pos);
        });
    }
    return shared>=2;
}

inline bool UpperCabinTrim(std::span<const gt2::CarMeshVertex> face, const CockpitFit& fit) {
    if (face.empty()) return false;
    float centerZ=0;
    for (const auto& vertex:face) {
        if (vertex.pos[1]<fit.eye[1]+.035f || vertex.pos[1]>fit.roofY+.025f ||
            std::abs(vertex.pos[0])>fit.halfWidth+.05f) return false;
        centerZ+=vertex.pos[2]/float(face.size());
    }
    return centerZ>=fit.windshieldZ && centerZ<=fit.rearWindowZ+.10f;
}
}

struct CockpitBody {
    std::vector<gt2::CarMeshVertex> vertices;
    std::vector<gt2::CarMeshVertex> windowOpenings;
    std::array<uint16_t, 16> glassMasks{};
    size_t windowTriangles = 0;
    std::vector<size_t> exteriorPolygons;
};

inline CockpitBody BuildCockpitBody(const gt2::CarModel& model, const gt2::CarTexture& texture, const CockpitFit& fit) {
    CockpitBody result;
    if (!fit.valid) return result;
    result.glassMasks = CockpitGlassMasks(texture);
    gt2::CarMeshOptions options;
    options.wheels = false;
    const auto original = gt2::BuildCarMesh(model, options);
    std::array<uint16_t,16> transparentMasks{};
    if (!texture.paints.empty()) for (size_t p=0;p<16;++p) for (size_t i=0;i<16;++i)
        if (std::all_of(texture.paints.begin(),texture.paints.end(),[&](const auto& paint) { return paint.cluts[p][i]==0; }))
            transparentMasks[p]|=uint16_t(1u<<i);
    std::vector<std::span<const gt2::CarMeshVertex>> faces;
    std::vector<bool> backing;
    std::vector<float> coverage;
    std::vector<gt2::CarMeshVertex> endPanes;
    size_t offset=0;
    for (const auto& polygon : model.lods.front().polygons) {
        const size_t count=polygon.IsQuad()?6:3;
        faces.push_back(std::span(original).subspan(offset,count)); offset+=count;
        bool transparent = false;
        for (size_t tri = 0; tri < count; tri += 3)
            transparent |= cockpit_detail::GlassCoverage(faces.back().subspan(tri,3),texture,transparentMasks)>.25f;
        backing.push_back(transparent);
        float glass=0;
        for (size_t tri=0;tri<count;tri+=3)
            glass=std::max(glass,cockpit_detail::GlassCoverage(faces.back().subspan(tri,3),texture,result.glassMasks));
        coverage.push_back(glass);
        if (glass>=.25f && IsCockpitEndWindowFace(faces.back()) &&
            cockpit_detail::ExtendedWindowFace(faces.back(),fit,glass) && !cockpit_detail::RoofPanel(faces.back(),fit) &&
            (transparent || !cockpit_detail::HeaderTrim(faces.back(),fit,glass)))
            endPanes.insert(endPanes.end(),faces.back().begin(),faces.back().end());
    }
    result.vertices.reserve(original.size() * 2);
    size_t at = 0;
    for (size_t polygonIndex=0; polygonIndex<model.lods.front().polygons.size(); ++polygonIndex) {
        const auto& polygon=model.lods.front().polygons[polygonIndex];
        const size_t count = polygon.IsQuad() ? 6 : 3;
        const auto face = std::span(original).subspan(at, count);
        at += count;
        std::array<float, 3> center{};
        float maxY = face[0].pos[1];
        for (const auto& vertex : face) {
            for (size_t axis = 0; axis < 3; ++axis) center[axis] += vertex.pos[axis] / float(count);
            maxY = std::max(maxY, vertex.pos[1]);
        }
        const bool raisedBulkhead=cockpit_detail::InteriorChassisBulkhead(face,fit) &&
            std::any_of(faces.begin(),faces.end(),[&](const auto& candidate) {
                return cockpit_detail::InteriorRaisedFloor(candidate,fit) && cockpit_detail::SharesEdge(face,candidate);
            });
        if (cockpit_detail::InteriorCentrePlane(face,fit) || cockpit_detail::InteriorRaisedFloor(face,fit) || raisedBulkhead) {
            for (size_t tri=0;tri<count;tri+=3) {
                for (bool front:{true,false}) {
                    const auto clipped=cockpit_detail::ClipPlane(face.subspan(tri,3),2,
                        front?fit.windshieldZ:fit.rearWindowZ,front);
                    for (size_t i=1;i+1<clipped.size();++i) {
                        result.vertices.push_back(clipped[0]);result.vertices.push_back(clipped[i]);result.vertices.push_back(clipped[i+1]);
                    }
                }
            }
            continue;
        }
        const float along = std::clamp((center[2] - fit.windshieldZ) /
            std::max(.1f, fit.rearWindowZ - fit.windshieldZ), 0.f, 1.f);
        const float belt = fit.windshieldY + along * (fit.rearWindowY - fit.windshieldY);
        const bool cabin = center[2] >= fit.windshieldZ - .18f && center[2] <= fit.rearWindowZ + .12f &&
            maxY > belt + .035f && center[1] > belt - .10f && !cockpit_detail::OutsideCabinShell(face, fit);
        const float glassCoverage=coverage[polygonIndex];
        const bool windowSurface=cabin || cockpit_detail::ExtendedWindowFace(face,fit,glassCoverage);
        // Exterior sponsor decals have no structural thickness. Do not turn their
        // reverse side into opaque cockpit trim across an independently modelled
        // window. Require alpha and geometric coverage by a proven end pane;
        // ordinary pillars, roof panels and single-layer window frames remain.
        if (windowSurface && backing[polygonIndex] && glassCoverage<.5f &&
            cockpit_detail::WindowOverlay(face,endPanes)) continue;
        const float glassThreshold = IsCockpitEndWindowFace(face) ? .005f : .025f;
        const bool roof = windowSurface && cockpit_detail::RoofPanel(face,fit);
        // Small teal letters and highlights on an otherwise opaque upper band
        // are not windows; retaining the band avoids perforating sponsor text.
        const bool header=windowSurface && !backing[polygonIndex] && cockpit_detail::HeaderTrim(face,fit,glassCoverage);
        const bool glazing=windowSurface && !roof && !header && glassCoverage>glassThreshold;
        bool alphaOpening=false,paintedBacking=false;
        if (windowSurface && !header) {
            for (size_t tri=0;tri<count;tri+=3)
                alphaOpening |= cockpit_detail::GlassCoverage(face.subspan(tri,3),texture,transparentMasks)>.05f;
            if (glazing) for (size_t other=0;other<faces.size();++other) {
                if (other==polygonIndex || !backing[other] || faces[other][0].palette==face[0].palette || faces[other].size()!=count) continue;
                const bool same=std::all_of(face.begin(),face.end(),[&](const auto& vertex) {
                    return std::any_of(faces[other].begin(),faces[other].end(),[&](const auto& candidate) {
                        return std::equal(vertex.pos,vertex.pos+3,candidate.pos);
                    });
                });
                if (same) { paintedBacking=true; break; }
            }
        }
        if (glazing || alphaOpening) {
            auto cutMasks=result.glassMasks;
            for (size_t p=0;p<16;++p) cutMasks[p]|=transparentMasks[p];
            auto cut = CutCockpitWindows(face, texture, cutMasks, paintedBacking);
            result.windowTriangles += count / 3;
            if (cut.openings) {
                // Alpha also describes wheel arches and exterior trim. Only an
                // authored glazing colour establishes a window for cabin fitting.
                if (glazing) result.windowOpenings.insert(result.windowOpenings.end(),cut.opening.begin(),cut.opening.end());
                for (auto& vertex : cut.frame) {
                    // Geometry owns the opening; source alpha must not perforate
                    // the new smooth edge with another grid of transparent texels.
                    vertex.textured=false;
                    CockpitLining(vertex);
                }
                result.vertices.insert(result.vertices.end(), cut.frame.begin(), cut.frame.end());
            } else {
                for (auto vertex : face) {
                    vertex.palette += uint8_t(kCockpitGlazingPalette);
                    result.vertices.push_back(vertex);
                }
            }
            continue;
        }
        const size_t start = result.vertices.size();
        result.vertices.insert(result.vertices.end(), face.begin(), face.end());
        if (windowSurface || cockpit_detail::InnerSidewall(face,fit) || cockpit_detail::UpperCabinTrim(face,fit)) {
            // Matching inner trim also avoids coplanar exterior decals fighting the window frame.
            for (auto& vertex : std::span(result.vertices).subspan(start)) {
                CockpitLining(vertex);
            }
        } else result.exteriorPolygons.push_back(polygonIndex);
    }
    return result;
}

} // namespace gt2view
