#pragma once
#include "gt2formats/track.h"
#include "game/sim/ground.h"
#include <array>
#include <span>
#include <cmath>
#include <vector>
#include <algorithm>
#include <map>

namespace gt2view {
// Shared by the course renderer and the physical/rendered-height audit.
inline std::span<const size_t> CoursePolygonTriangles(bool quad) {
    static constexpr size_t triangle[]{0,1,2},quadrilateral[]{0,1,3,1,2,3};
    return quad?std::span<const size_t>(quadrilateral):std::span<const size_t>(triangle);
}
inline std::array<std::array<float,3>,4> CoursePolygonPositions(
    const gt2::TrackChunk& chunk,const gt2::TrackPolygon& polygon) {
    std::array<std::array<float,3>,4> points{};
    for(size_t k=0;k<(polygon.IsQuad()?4u:3u);++k)
        points[k]=gt2::TrackVertexToWorld(chunk,chunk.road.vertices.at(polygon.vertex[k]));
    return points;
}
struct CourseSurfaceMesh {
    std::vector<std::array<float,3>> positions;
    std::vector<std::array<float,4>> weights;
    std::vector<size_t> indices;
    bool contactFan=false;
};
// The contact solver uses four planes through the quantised centroid. Only
// non-planar, convex drivable quads need extra triangles; flat roads and walls
// keep the original GPU diagonal. This changes native geometry, never physics.
inline CourseSurfaceMesh CoursePolygonMesh(const gt2::TrackChunk& chunk,
    const gt2::TrackPolygon& polygon,bool physical) {
    CourseSurfaceMesh mesh;
    mesh.positions.resize(5);mesh.weights.resize(5);
    const auto corners=CoursePolygonPositions(chunk,polygon);
    for(size_t i=0;i<4;++i){mesh.positions[i]=corners[i];mesh.weights[i][i]=1;}
    const auto original=CoursePolygonTriangles(polygon.IsQuad());mesh.indices.assign(original.begin(),original.end());
    if(!physical || !polygon.IsQuad())return mesh;
    double footprintArea=0;for(size_t t=0;t<6;t+=3){const auto& a=corners[original[t]];const auto& b=corners[original[t+1]];const auto& c=corners[original[t+2]];footprintArea+=std::abs((b[0]-a[0])*(c[2]-a[2])-(b[2]-a[2])*(c[0]-a[0]));}if(footprintArea<.0001)return mesh;
    const auto& a=corners[0];const auto& b=corners[1];const auto& c=corners[3];
    const double dx=b[0]-a[0],dz=b[2]-a[2],ex=c[0]-a[0],ez=c[2]-a[2],det=dx*ez-dz*ex;
    if(std::abs(det)>.01){
        const double sx=((b[1]-a[1])*ez-(c[1]-a[1])*dz)/det;
        const double sz=(dx*(c[1]-a[1])-ex*(b[1]-a[1]))/det;
        if(std::abs(corners[2][1]-a[1]-sx*(corners[2][0]-a[0])-sz*(corners[2][2]-a[2]))<.001)return mesh;
    }
    gt2::TrackVertex centre{};
    int sumX=0,sumY=0,sumZ=0;
    for(size_t i=0;i<4;++i){const auto& v=chunk.road.vertices[polygon.vertex[i]];sumX+=v.x;sumY+=v.y;sumZ+=v.z;}
    centre.x=int16_t(sumX>>2);centre.y=int16_t(sumY>>2);centre.z=int16_t(sumZ>>2);
    const auto centreWorld=gt2::TrackVertexToWorld(chunk,centre);mesh.positions[4]=centreWorld;
    gt2::sim::SurfaceGrid grid;grid.originX=chunk.surfaceGrid.originX;grid.originZ=chunk.surfaceGrid.originZ;grid.shiftX=chunk.surfaceGrid.shiftX;grid.shiftZ=chunk.surfaceGrid.shiftZ;std::copy(chunk.surfaceGrid.cells.begin(),chunk.surfaceGrid.cells.end(),std::begin(grid.cells));
    double sign=0;bool convex=true;
    for(size_t i=0;i<4;++i){const auto& p=corners[i];const auto& q=corners[(i+1)%4];
        const double cross=(q[0]-p[0])*(mesh.positions[4][2]-p[2])-(q[2]-p[2])*(mesh.positions[4][0]-p[0]);
        if(std::abs(cross)<.001 || (sign && cross*sign<0))convex=false;if(std::abs(cross)>.001)sign=cross;
    }
    static constexpr size_t fan[]{0,1,4,1,2,4,2,3,4,3,0,4};
    if(convex){
        // Keep the original diagonal's UV/colour interpolation at the centre.
        for(size_t t=0;t<6;t+=3){const auto i=original[t],j=original[t+1],k=original[t+2];
            const auto& p=corners[i];const auto& q=corners[j];const auto& r=corners[k];const auto& o=mesh.positions[4];
            const float ax=q[0]-p[0],az=q[2]-p[2],bx=r[0]-p[0],bz=r[2]-p[2],d=ax*bz-az*bx;
            if(std::abs(d)<.001f)continue;const float u=((o[0]-p[0])*bz-(o[2]-p[2])*bx)/d,v=(ax*(o[2]-p[2])-az*(o[0]-p[0]))/d;
            if(u>=-.001f && v>=-.001f && u+v<=1.001f){mesh.weights[4][i]=1-u-v;mesh.weights[4][j]=u;mesh.weights[4][k]=v;break;}
        }
        mesh.indices.assign(std::begin(fan),std::end(fan));mesh.contactFan=true;
        double maxSlope=0;
        for(size_t t=0;t<12;t+=3){const auto& p0=mesh.positions[fan[t]];const auto& p1=mesh.positions[fan[t+1]];const auto& p2=mesh.positions[fan[t+2]];const double ax=p1[0]-p0[0],az=p1[2]-p0[2],bx=p2[0]-p0[0],bz=p2[2]-p0[2],den=ax*bz-az*bx;if(std::abs(den)<1e-10){maxSlope=1e30;break;}maxSlope=std::max(maxSlope,std::abs(((p1[1]-p0[1])*bz-(p2[1]-p0[1])*az)/den)+std::abs((ax*(p2[1]-p0[1])-bx*(p1[1]-p0[1]))/den));}
        bool accurate=true;
        if(maxSlope>1)for(size_t t=0;t<6 && accurate;t+=3)for(unsigned n=1;n<=149 && accurate;++n){const double u=std::sqrt(std::fmod(n*.61803398875,1.)),v=std::fmod(n*.41421356237,1.),weights[]{1-u,u*(1-v),u*v};std::array<double,3> p0{};for(size_t i=0;i<3;++i)for(size_t k=0;k<3;++k)p0[k]+=corners[original[t+i]][k]*weights[i];const int32_t point[]{int32_t(std::llround(p0[0]*65536)),0,int32_t(std::llround(p0[2]*65536))};const auto selected=gt2::sim::FindRoadPolygonAt(chunk,grid,point);if(selected<0 || chunk.road.polygons[size_t(selected)].vertex!=polygon.vertex)continue;
            const double height=gt2::sim::InterpolateRoadHeight(chunk,polygon,point)/65536.;bool found=false;
            for(size_t j=0;j<12;j+=3){const auto& a0=mesh.positions[fan[j]];const auto& a1=mesh.positions[fan[j+1]];const auto& a2=mesh.positions[fan[j+2]];const double ax=a1[0]-a0[0],az=a1[2]-a0[2],bx=a2[0]-a0[0],bz=a2[2]-a0[2],den=ax*bz-az*bx;if(std::abs(den)<1e-10)continue;const double bu=((p0[0]-a0[0])*bz-(p0[2]-a0[2])*bx)/den,bv=(ax*(p0[2]-a0[2])-az*(p0[0]-a0[0]))/den;if(bu< -1e-7 || bv< -1e-7 || bu+bv>1.0000001)continue;found=true;accurate=std::abs(height-a0[1]-bu*(a1[1]-a0[1])-bv*(a2[1]-a0[1]))<=.075;break;}
            accurate&=found;
        }
        if(accurate)return mesh;
    }
    // Most folded kerb faces are already accurate with the GPU diagonal. Avoid
    // refining their non-contact extrapolation: inspect real contact samples
    // before taking the expensive exceptional path. The audit uses independent
    // probes and still enforces the same 8 cm gate and original footprint.
    mesh.indices.assign(original.begin(),original.end());mesh.contactFan=false;
    bool inaccurate=false;
    // A vertical edge collapses in the contact footprint. Its centroid planes
    // can select a narrow sector that sparse probes miss, so preserve that
    // sector explicitly rather than accepting the original GPU diagonal.
    for(size_t i=0;i<4;++i)for(size_t j=i+1;j<4;++j)
        if(corners[i][0]==corners[j][0] && corners[i][2]==corners[j][2] &&
            std::abs(corners[i][1]-corners[j][1])>.075f)inaccurate=true;
    for(size_t t=0;t<6 && !inaccurate;t+=3){const auto& ta=corners[original[t]];const auto& tb=corners[original[t+1]];const auto& tc=corners[original[t+2]];if(std::abs((tb[0]-ta[0])*(tc[2]-ta[2])-(tb[2]-ta[2])*(tc[0]-ta[0]))<.0001)continue;
    for(unsigned n=1;n<=149 && !inaccurate;++n){const double u=std::sqrt(std::fmod(n*.61803398875,1.)),v=std::fmod(n*.41421356237,1.),weights[]{1-u,u*(1-v),u*v};std::array<double,3> p{};for(size_t i=0;i<3;++i)for(size_t k=0;k<3;++k)p[k]+=corners[original[t+i]][k]*weights[i];const int32_t point[]{int32_t(std::llround(p[0]*65536)),0,int32_t(std::llround(p[2]*65536))};
        const auto selected=gt2::sim::FindRoadPolygonAt(chunk,grid,point);if(selected<0 || chunk.road.polygons[size_t(selected)].vertex!=polygon.vertex)continue;
        const double h=gt2::sim::InterpolateRoadHeight(chunk,polygon,point)/65536.;inaccurate=std::abs(h-p[1])>.075;
    }}
    if(!inaccurate)return mesh;
    // Folded kerbs use quantised sector boundaries. Slice along integer Z
    // rows and X transitions; this is linear in boundary length, unlike recursive
    // subdivision of a long thin triangle. Preserve non-contact/vertical faces.
    struct V {std::array<double,3> p{};std::array<double,4> w{};};
    const auto origin=gt2::TrackVertexToWorld(chunk,gt2::TrackVertex{0,0,0});
    std::array<gt2::TrackVertex,4> local{};for(size_t i=0;i<4;++i)local[i]=chunk.road.vertices[polygon.vertex[i]];
    auto bitsAt=[&](int64_t ix,int64_t iz){const auto dx=ix-int64_t(origin[0]*64)-centre.x,dz=iz-int64_t(origin[2]*64)-centre.y;uint32_t bits=0;for(size_t i=0;i<4;++i)if(dx*(local[i].y-centre.y)-dz*(local[i].x-centre.x)<0)bits|=1u<<i;return bits;};
    auto planeHeight=[&](double wx,double wz,uint32_t bits){size_t edge=0;switch(bits){case 1:case 3:case 7:edge=3;break;case 4:case 12:case 13:edge=1;break;case 8:case 9:case 11:edge=2;break;default:break;}
        const auto& p0=local[edge];const auto& p1=local[(edge+1)%4];const double ax=p0.x-centre.x,az=p0.y-centre.y,ay=p0.z-centre.z,bx=p1.x-centre.x,bz=p1.y-centre.y,by=p1.z-centre.z,den=ax*bz-az*bx;
        return origin[1]+centre.z/64.+(den?((ay*bz-az*by)*(wx-origin[0]-centre.x/64.)+(ax*by-ay*bx)*(wz-origin[2]-centre.y/64.))/den:0);
    };
    auto axisClip=[](const std::vector<V>& input,size_t axis,double boundary,double keep){std::vector<V> out;for(size_t i=0;i<input.size();++i){const auto& p0=input[i];const auto& p1=input[(i+1)%input.size()];const double d0=keep*(p0.p[axis]-boundary),d1=keep*(p1.p[axis]-boundary);if(d0>=-1e-10)out.push_back(p0);if((d0>1e-10 && d1< -1e-10)||(d0< -1e-10 && d1>1e-10)){const double f=d0/(d0-d1);V v{};for(size_t k=0;k<3;++k)v.p[k]=p0.p[k]+f*(p1.p[k]-p0.p[k]);for(size_t k=0;k<4;++k)v.w[k]=p0.w[k]+f*(p1.w[k]-p0.w[k]);out.push_back(v);}}return out;};
    mesh.positions.clear();mesh.weights.clear();mesh.indices.clear();mesh.contactFan=true;
    auto emit=[&](const std::vector<V>& ring,bool adjust,uint32_t bits){for(size_t i=1;i+1<ring.size();++i){const auto& a0=ring[0].p;const auto& a1=ring[i].p;const auto& a2=ring[i+1].p;const double area=(a1[0]-a0[0])*(a2[2]-a0[2])-(a1[2]-a0[2])*(a2[0]-a0[0]);if(std::abs(area)<1e-10 && adjust)continue;
        for(size_t vi:{size_t(0),i,i+1}){const auto& v=ring[vi];mesh.indices.push_back(mesh.positions.size());mesh.positions.push_back({float(v.p[0]),float(adjust?planeHeight(v.p[0],v.p[2],bits):v.p[1]),float(v.p[2])});std::array<float,4> weights{};for(size_t k=0;k<4;++k)weights[k]=float(v.w[k]);mesh.weights.push_back(weights);}
    }};
    std::vector<uint16_t> physicalPolygons;for(const auto& cell:chunk.surfaceGrid.cells)physicalPolygons.insert(physicalPolygons.end(),cell.begin(),cell.end());std::sort(physicalPolygons.begin(),physicalPolygons.end());physicalPolygons.erase(std::unique(physicalPolygons.begin(),physicalPolygons.end()),physicalPolygons.end());
    for(size_t t=0;t<6;t+=3){std::vector<V> triangle;for(size_t i=0;i<3;++i){V v;const auto vi=original[t+i];for(size_t k=0;k<3;++k)v.p[k]=corners[vi][k];v.w[vi]=1;triangle.push_back(v);}
        const auto& a0=triangle[0].p;const auto& a1=triangle[1].p;const auto& a2=triangle[2].p;const double area=(a1[0]-a0[0])*(a2[2]-a0[2])-(a1[2]-a0[2])*(a2[0]-a0[0]);if(std::abs(area)<.0001){emit(triangle,false,0);continue;}
        double loX=a0[0],hiX=loX,loZ=a0[2],hiZ=loZ;for(const auto& v:triangle){loX=std::min(loX,v.p[0]);hiX=std::max(hiX,v.p[0]);loZ=std::min(loZ,v.p[2]);hiZ=std::max(hiZ,v.p[2]);}
        // Lookup boundaries of neighbouring contact polygons matter too: the
        // first polygon in the grid wins, independently of this face's sector.
        std::vector<uint16_t> relevant;for(auto pi:physicalPolygons){const auto& p0=chunk.road.polygons[pi];const auto ps=CoursePolygonPositions(chunk,p0);double x0=ps[0][0],x1=x0,z0=ps[0][2],z1=z0;for(size_t k=0;k<(p0.IsQuad()?4u:3u);++k){x0=std::min(x0,double(ps[k][0]));x1=std::max(x1,double(ps[k][0]));z0=std::min(z0,double(ps[k][2]));z1=std::max(z1,double(ps[k][2]));}if(x1>=loX-.02 && x0<=hiX+.02 && z1>=loZ-.02 && z0<=hiZ+.02)relevant.push_back(pi);}
        struct RectRun {int64_t firstZ,lastZ;uint32_t bits;};using RectKey=std::array<int64_t,3>;std::map<RectKey,RectRun> runs;
        auto plane=[](uint32_t bits){switch(bits){case 1:case 3:case 7:return 3;case 4:case 12:case 13:return 1;case 8:case 9:case 11:return 2;default:return 0;}};
        auto emitRect=[&](const RectKey& key,const RectRun& run){auto part=axisClip(triangle,2,run.firstZ/64.,1);part=axisClip(part,2,run.lastZ/64.,-1);part=axisClip(part,0,key[0]/64.,1);part=axisClip(part,0,key[1]/64.,-1);if(part.size()>=3)emit(part,key[2]!=0,run.bits);};
        for(int64_t iz=int64_t(std::floor(loZ*64));iz<=int64_t(std::floor(hiZ*64));++iz){auto row=axisClip(triangle,2,iz/64.,1);row=axisClip(row,2,(iz+1)/64.,-1);if(row.size()<3)continue;std::vector<double> xs{loX,hiX};
            auto transition=[&](int64_t coefficient,int64_t constant){if(!coefficient)return;const double root=-double(constant)/coefficient;const int64_t first=int64_t(std::ceil(root));const double x=first/64.;if(x>loX && x<hiX)xs.push_back(x);if(constant%coefficient==0){const double next=(first+1)/64.;if(next>loX && next<hiX)xs.push_back(next);}};
            const int64_t ox=int64_t(origin[0]*64),oz=int64_t(origin[2]*64);
            for(size_t i=0;i<4;++i){const int64_t ax=local[i].x-centre.x,az=local[i].y-centre.y;transition(az,-(ox+centre.x)*az-(iz-oz-centre.y)*ax);}
            for(auto pi:relevant){const auto& p0=chunk.road.polygons[pi];const size_t count=p0.IsQuad()?4:3;for(size_t i=0;i<count;++i){const auto& v0=chunk.road.vertices[p0.vertex[i]];const auto& v1=chunk.road.vertices[p0.vertex[(i+1)%count]];const int64_t edgeX=int(v1.x)-v0.x,edgeZ=int(v1.y)-v0.y;transition(-edgeZ,edgeX*(iz-oz-v0.y)+edgeZ*(ox+v0.x));}}
            for(int i=0;i<=4;++i){const double x=(ox+grid.originX+int64_t(i)*(int64_t(1)<<(grid.shiftX&31)))/64.;if(x>loX && x<hiX)xs.push_back(x);}
            std::sort(xs.begin(),xs.end());xs.erase(std::unique(xs.begin(),xs.end()),xs.end());
            struct Segment{int64_t lo,hi,mode;uint32_t bits;};std::vector<Segment> segments;
            for(size_t i=1;i<xs.size();++i){auto part=axisClip(row,0,xs[i-1],1);part=axisClip(part,0,xs[i],-1);if(part.size()<3)continue;const double x=(xs[i-1]+xs[i])*.5,z=(iz+.5)/64.;const int32_t query[]{int32_t(std::llround(x*65536)),0,int32_t(std::llround(z*65536))};const auto selected=gt2::sim::FindRoadPolygonAt(chunk,grid,query);const bool adjust=selected>=0 && chunk.road.polygons[size_t(selected)].vertex==polygon.vertex;const auto bits=bitsAt(int64_t(std::floor(x*64)),iz);const int64_t mode=adjust?plane(bits)+1:0;
                const int64_t from=std::llround(xs[i-1]*64),to=std::llround(xs[i]*64);if(!segments.empty() && segments.back().hi==from && segments.back().mode==mode)segments.back().hi=to;else segments.push_back({from,to,mode,bits});
            }
            for(const auto& segment:segments){const RectKey key{segment.lo,segment.hi,segment.mode};auto found=runs.find(key);if(found!=runs.end() && found->second.lastZ==iz)found->second.lastZ=iz+1;else{if(found!=runs.end())emitRect(found->first,found->second);runs[key]={iz,iz+1,segment.bits};}}
            for(auto it=runs.begin();it!=runs.end();){if(it->second.lastZ!=iz+1){emitRect(it->first,it->second);it=runs.erase(it);}else ++it;}
        }
        for(const auto& [key,run]:runs)emitRect(key,run);
    }
    return mesh;
}
}
