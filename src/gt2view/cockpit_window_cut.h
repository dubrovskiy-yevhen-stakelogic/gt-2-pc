#pragma once
#include "gt2export/car_mesh.h"
#include "gt2formats/car_texture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <utility>
#include <vector>

namespace gt2view {

struct WindowCut {
    std::vector<gt2::CarMeshVertex> frame;
    std::vector<gt2::CarMeshVertex> opening;
    size_t openings = 0;
};

namespace cockpit_window_detail {
struct Point {
    float x, y;
    bool operator==(const Point&) const = default;
};
inline float Cross(Point a, Point b, Point p) {
    return (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x);
}
inline Point UV(const gt2::CarMeshVertex& v) { return {v.texel[0],v.texel[1]}; }

inline bool IsGlass(Point p, const gt2::CarTexture& texture, uint16_t mask) {
    if (p.x<0 || p.y<0 || p.x>=gt2::CarTexture::kWidth || p.y>=gt2::CarTexture::kHeight) return false;
    const auto index=texture.indices[size_t(int(std::floor(p.y)))*gt2::CarTexture::kWidth+size_t(int(std::floor(p.x)))];
    return index<16 && (mask & (1u<<index))!=0;
}

inline bool CellIntersectsTriangle(int x, int y, const std::array<Point,3>& triangle) {
    const float area=Cross(triangle[0],triangle[1],triangle[2]);
    if (std::abs(area)<1e-8f) return false;
    const float left=float(x),top=float(y),right=left+1,bottom=top+1;
    if (std::max({triangle[0].x,triangle[1].x,triangle[2].x})<=left ||
        std::min({triangle[0].x,triangle[1].x,triangle[2].x})>=right ||
        std::max({triangle[0].y,triangle[1].y,triangle[2].y})<=top ||
        std::min({triangle[0].y,triangle[1].y,triangle[2].y})>=bottom) return false;
    const float sign=area>0?1.f:-1.f;
    for (size_t i=0;i<3;++i) {
        const auto a=triangle[i],b=triangle[(i+1)%3];
        const float maximum=std::max({Cross(a,b,{left,top})*sign,Cross(a,b,{right,top})*sign,
                                      Cross(a,b,{right,bottom})*sign,Cross(a,b,{left,bottom})*sign});
        if (maximum<=1e-7f) return false;
    }
    return true;
}

inline std::vector<Point> ConvexHull(std::vector<Point> points) {
    std::sort(points.begin(),points.end(),[](Point a,Point b) { return a.x<b.x || (a.x==b.x && a.y<b.y); });
    points.erase(std::unique(points.begin(),points.end()),points.end());
    if (points.size()<3) return {};
    std::vector<Point> hull;
    hull.reserve(points.size()+1);
    for (Point p:points) {
        while (hull.size()>=2 && Cross(hull[hull.size()-2],hull.back(),p)<=0) hull.pop_back();
        hull.push_back(p);
    }
    const size_t lower=hull.size();
    for (size_t i=points.size()-1;i-->0;) {
        const Point p=points[i];
        while (hull.size()>lower && Cross(hull[hull.size()-2],hull.back(),p)<=0) hull.pop_back();
        hull.push_back(p);
    }
    hull.pop_back();
    return hull;
}

inline gt2::CarMeshVertex Interpolate(const gt2::CarMeshVertex& a, const gt2::CarMeshVertex& b, float t) {
    auto out=a;
    for (size_t i=0;i<3;++i) {
        out.pos[i]+=(b.pos[i]-a.pos[i])*t;
        out.color[i]+=(b.color[i]-a.color[i])*t;
    }
    for (size_t i=0;i<2;++i) out.texel[i]+=(b.texel[i]-a.texel[i])*t;
    return out;
}

using Polygon=std::vector<gt2::CarMeshVertex>;
inline Polygon Clip(std::span<const gt2::CarMeshVertex> polygon, Point a, Point b, bool keepLeft) {
    Polygon result;
    if (polygon.empty()) return result;
    result.reserve(polygon.size()+1);
    const float sign=keepLeft?1.f:-1.f;
    auto previous=polygon.back();
    float previousDistance=Cross(a,b,UV(previous))*sign;
    for (const auto& current:polygon) {
        const float distance=Cross(a,b,UV(current))*sign;
        if ((distance>=0)!=(previousDistance>=0)) {
            const float t=std::clamp(previousDistance/(previousDistance-distance),0.f,1.f);
            result.push_back(Interpolate(previous,current,t));
        }
        if (distance>=0) result.push_back(current);
        previous=current; previousDistance=distance;
    }
    return result;
}

inline float Area(std::span<const gt2::CarMeshVertex> polygon) {
    if (polygon.size()<3) return 0;
    float area=0;
    for (size_t i=1;i+1<polygon.size();++i) area+=Cross(UV(polygon[0]),UV(polygon[i]),UV(polygon[i+1]));
    return std::abs(area)*.5f;
}

inline void Subtract(const Polygon& polygon, std::span<const Point> hole, std::vector<Polygon>& output) {
    Polygon intersection=polygon;
    for (size_t edge=0;edge<hole.size() && intersection.size()>=3;++edge)
        intersection=Clip(intersection,hole[edge],hole[(edge+1)%hole.size()],true);
    if (Area(intersection)<=1e-7f) { output.push_back(polygon); return; }
    Polygon inside=polygon;
    for (size_t edge=0;edge<hole.size() && inside.size()>=3;++edge) {
        const auto a=hole[edge],b=hole[(edge+1)%hole.size()];
        bool anyInside=false,anyOutside=false;
        for (const auto& vertex:inside) {
            const float distance=Cross(a,b,UV(vertex));
            anyInside|=distance>0; anyOutside|=distance<0;
        }
        if (!anyOutside) continue;
        if (!anyInside) { output.push_back(std::move(inside)); return; }
        auto outside=Clip(inside,a,b,false);
        if (Area(outside)>1e-7f) output.push_back(std::move(outside));
        inside=Clip(inside,a,b,true);
    }
}

inline bool DegenerateIsGlass(std::span<const gt2::CarMeshVertex> triangle,
                              const gt2::CarTexture& texture, uint16_t mask) {
    // A collapsed UV triangle can still cross a painted strip between two windows.
    for (size_t edge=0;edge<3;++edge) {
        const auto a=UV(triangle[edge]),b=UV(triangle[(edge+1)%3]);
        const int steps=std::max(1,int(std::ceil(std::max(std::abs(b.x-a.x),std::abs(b.y-a.y))*4)));
        for (int i=0;i<=steps;++i) {
            const float t=float(i)/float(steps);
            if (!IsGlass({a.x+(b.x-a.x)*t,a.y+(b.y-a.y)*t},texture,mask)) return false;
        }
    }
    return true;
}

inline void AppendTriangles(std::span<const gt2::CarMeshVertex> polygon, std::vector<gt2::CarMeshVertex>& out) {
    for (size_t i=1;i+1<polygon.size();++i) {
        if (std::abs(Cross(UV(polygon[0]),UV(polygon[i]),UV(polygon[i+1])))<=1e-7f) continue;
        out.push_back(polygon[0]); out.push_back(polygon[i]); out.push_back(polygon[i+1]);
    }
}
} // namespace cockpit_window_detail

inline bool IsCockpitEndWindowFace(std::span<const gt2::CarMeshVertex> face) {
    if (face.size()!=3 && face.size()!=6) return false;
    std::array<float,3> lo{face[0].pos[0],face[0].pos[1],face[0].pos[2]},hi=lo,normal{};
    float area=0;
    for (const auto& vertex:face) for (size_t axis=0;axis<3;++axis) {
        if (!std::isfinite(vertex.pos[axis])) return false;
        lo[axis]=std::min(lo[axis],vertex.pos[axis]); hi[axis]=std::max(hi[axis],vertex.pos[axis]);
    }
    for (size_t i=0;i<face.size();i+=3) {
        std::array<float,3> a{},b{};
        for (size_t axis=0;axis<3;++axis) {
            a[axis]=face[i+1].pos[axis]-face[i].pos[axis];
            b[axis]=face[i+2].pos[axis]-face[i].pos[axis];
        }
        const std::array<float,3> n{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        area+=.5f*std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);
        for (size_t axis=0;axis<3;++axis) normal[axis]+=n[axis];
    }
    const float length=std::sqrt(normal[0]*normal[0]+normal[1]*normal[1]+normal[2]*normal[2]);
    return area>=.045f && hi[0]-lo[0]>=.20f && hi[1]-lo[1]>=.12f && length>1e-6f &&
        std::abs(normal[2])>=std::abs(normal[0])*1.15f && std::abs(normal[2])>=length*.25f;
}

// A complete authored face shares one mask across both triangles of a quad.
inline WindowCut CutCockpitWindows(std::span<const gt2::CarMeshVertex> face, const gt2::CarTexture& texture,
                                   const std::array<uint16_t,16>& masks, bool paintedBacking = false) {
    using namespace cockpit_window_detail;
    WindowCut result;
    auto unchanged=[&]() { result.frame.assign(face.begin(),face.end()); return result; };
    if ((face.size()!=3 && face.size()!=6) || texture.indices.size()!=gt2::CarTexture::kWidth*gt2::CarTexture::kHeight)
        return unchanged();
    const auto palette=face[0].palette;
    if (palette>=masks.size() || !masks[palette]) return unchanged();
    for (const auto& v:face) {
        if (!v.textured || v.palette!=palette || !std::isfinite(v.texel[0]) || !std::isfinite(v.texel[1]) ||
            std::abs(v.texel[0])>4096 || std::abs(v.texel[1])>4096) return unchanged();
    }
    const uint16_t mask=masks[palette];
    uint16_t blackMask=0,blueShadowMask=0;
    if (!texture.paints.empty() && (IsCockpitEndWindowFace(face) || paintedBacking)) {
        for (size_t index=0;index<16;++index) {
            const auto color=texture.paints.front().cluts[palette][index]&0x7fffu;
            const auto r=color&31u,g=(color>>5)&31u,b=(color>>10)&31u;
            const bool blue=b>g && g>=r;
            // The glass shadow ramp has one additional blue-biased dark step.
            // Neutral trim keeps its stricter limit so it cannot widen a pane.
            if (std::max({r,g,b})>(blue?3u:2u) || std::max({r,g,b})-std::min({r,g,b})>1) continue;
            const bool invariant=std::all_of(texture.paints.begin(),texture.paints.end(),[&](const auto& paint) {
                return (paint.cluts[palette][index]&0x7fffu)==color;
            });
            if (invariant) {
                blackMask|=uint16_t(1u<<index);
                if (blue) blueShadowMask|=uint16_t(1u<<index);
            }
        }
    }
    std::vector<std::array<Point,3>> triangles;
    float minX=face[0].texel[0],maxX=minX,minY=face[0].texel[1],maxY=minY;
    for (const auto& v:face) {
        minX=std::min(minX,v.texel[0]); maxX=std::max(maxX,v.texel[0]);
        minY=std::min(minY,v.texel[1]); maxY=std::max(maxY,v.texel[1]);
    }
    for (size_t i=0;i<face.size();i+=3) triangles.push_back({UV(face[i]),UV(face[i+1]),UV(face[i+2])});
    const int left=int(std::floor(std::clamp(minX,0.f,255.f))),right=int(std::floor(std::clamp(maxX,0.f,255.f)));
    const int top=int(std::floor(std::clamp(minY,0.f,223.f))),bottom=int(std::floor(std::clamp(maxY,0.f,223.f)));
    const int width=right-left+1,height=bottom-top+1;
    std::vector<uint8_t> cells(size_t(width)*size_t(height));
    size_t coveredCells=0,tealCells=0,blackCells=0,blueShadowCells=0;
    for (int y=top;y<=bottom;++y) for (int x=left;x<=right;++x) {
        const Point pixel{float(x)+.5f,float(y)+.5f};
        const bool teal=IsGlass(pixel,texture,mask);
        if (!teal && !blackMask) continue;
        for (const auto& triangle:triangles) if (CellIntersectsTriangle(x,y,triangle)) {
            const size_t at=size_t(y-top)*size_t(width)+size_t(x-left);
            ++coveredCells;
            if (teal) { cells[at]=1; ++tealCells; }
            else if (IsGlass(pixel,texture,blackMask)) {
                const bool blue=IsGlass(pixel,texture,blueShadowMask);
                cells[at]=blue?4:3; ++blackCells; blueShadowCells+=blue;
            }
            break;
        }
    }
    // Some windscreen faces use black glass with just a thin teal reflection.
    // Side windows need a coincident painted backing to preserve their pillars.
    const bool darkPane=blackCells*100>=coveredCells*65;
    const bool reflectedDarkPane=blackCells*2>=coveredCells && (blackCells+tealCells)*5>=coveredCells*4;
    const bool reflectionGradient=IsCockpitEndWindowFace(face) && blackCells*5>=coveredCells &&
        tealCells*5>=coveredCells*2 && (blackCells+tealCells)*100>=coveredCells*85;
    const bool extendAllDark=darkPane || reflectedDarkPane || reflectionGradient;
    const bool blueGradient=IsCockpitEndWindowFace(face) && tealCells*4>=coveredCells*3 &&
        (blueShadowCells+tealCells)*100>=coveredCells*95;
    if (tealCells && (extendAllDark || blueGradient)) {
        std::vector<size_t> flood;
        for (size_t i=0;i<cells.size();++i) if (cells[i]==1) flood.push_back(i);
        for (size_t cursor=0;cursor<flood.size();++cursor) {
            const size_t cell=flood[cursor];
            auto visit=[&](size_t next) {
                if (cells[next]==4 || (extendAllDark && cells[next]==3)) { cells[next]=1; flood.push_back(next); }
            };
            if (cell%size_t(width)>0) visit(cell-1);
            if (cell%size_t(width)+1<size_t(width)) visit(cell+1);
            if (cell>=size_t(width)) visit(cell-size_t(width));
            if (cell+size_t(width)<cells.size()) visit(cell+size_t(width));
        }
    }
    std::vector<std::vector<size_t>> components;
    size_t largestComponent=0;
    for (size_t seed=0;seed<cells.size();++seed) {
        if (cells[seed]!=1) continue;
        auto& component=components.emplace_back();
        component.push_back(seed); cells[seed]=2;
        for (size_t cursor=0;cursor<component.size();++cursor) {
            const size_t cell=component[cursor];
            auto visit=[&](size_t next) { if (cells[next]==1) { cells[next]=2; component.push_back(next); } };
            if (cell%size_t(width)>0) visit(cell-1);
            if (cell%size_t(width)+1<size_t(width)) visit(cell+1);
            if (cell>=size_t(width)) visit(cell-size_t(width));
            if (cell+size_t(width)<cells.size()) visit(cell+size_t(width));
        }
        largestComponent=std::max(largestComponent,component.size());
    }
    std::vector<std::vector<Point>> holes;
    for (const auto& component:components) {
        // Detached one-texel reflections along a pillar are not additional windows.
        // Keep small panes with a solid core, and every face's principal opening.
        if (component.size()<=6 && component.size()*50<=largestComponent) {
            const bool hasCore=std::any_of(component.begin(),component.end(),[&](size_t cell) {
                return cell%size_t(width)+1<size_t(width) && cell/size_t(width)+1<size_t(height) &&
                    cells[cell+1]==2 && cells[cell+size_t(width)]==2 && cells[cell+size_t(width)+1]==2;
            });
            if (!hasCore) continue;
        }
        std::vector<Point> points;
        points.reserve(component.size()*4);
        for (size_t cell:component) {
            const float x=float(left+int(cell%size_t(width))),y=float(top+int(cell/size_t(width)));
            points.insert(points.end(),{{x,y},{x+1,y},{x+1,y+1},{x,y+1}});
        }
        auto hull=ConvexHull(std::move(points));
        if (hull.size()>=3) holes.push_back(std::move(hull));
    }
    bool droppedDegenerate=false;
    for (size_t i=0;i<face.size();i+=3) {
        const auto triangle=face.subspan(i,3);
        if (Area(triangle)<=1e-7f) {
            if (DegenerateIsGlass(triangle,texture,mask)) {
                droppedDegenerate=true;
                result.opening.insert(result.opening.end(),triangle.begin(),triangle.end());
            }
            else result.frame.insert(result.frame.end(),triangle.begin(),triangle.end());
            continue;
        }
        std::vector<Polygon> pieces{Polygon(triangle.begin(),triangle.end())};
        for (const auto& hole:holes) {
            Polygon opening(triangle.begin(),triangle.end());
            for (size_t edge=0;edge<hole.size() && opening.size()>=3;++edge)
                opening=Clip(opening,hole[edge],hole[(edge+1)%hole.size()],true);
            AppendTriangles(opening,result.opening);
            std::vector<Polygon> next;
            for (const auto& piece:pieces) Subtract(piece,hole,next);
            pieces=std::move(next);
            if (pieces.empty()) break;
        }
        for (const auto& piece:pieces) AppendTriangles(piece,result.frame);
    }
    result.openings=holes.size()+((holes.empty() && droppedDegenerate)?1u:0u);
    return result;
}

} // namespace gt2view
