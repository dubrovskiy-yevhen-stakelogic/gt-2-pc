#pragma once
#include "gt2view/model_window_cut.h"
#include "gt2view/gt4_cabin_geometry.h"
#include <stdexcept>

namespace gt2view {
// The same pane can have an alpha-cut border, a black tint pass and an
// environment pass with unrelated UVs. Record its opening in model space,
// then apply it to every coplanar pass, never to nearby bodywork.
// Exact mask rectangles preserve opaque islands (notably the oval mirror
// inside a transparent rectangular carrier). A convex hull of the empty rim
// would incorrectly erase the mirror itself.
inline ModelWindowHoles ExactModelMask(const gt2::PngImage& image,std::vector<uint8_t> mask) {
    ModelWindowHoles holes;const int w=image.width,h=image.height;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        if(!mask[size_t(y)*w+x])continue;
        int right=x+1;while(right<w && mask[size_t(y)*w+right])++right;
        int bottom=y+1;
        for(;bottom<h;++bottom){bool full=true;for(int a=x;a<right;++a)full=full && mask[size_t(bottom)*w+a];if(!full)break;}
        for(int b=y;b<bottom;++b)for(int a=x;a<right;++a)mask[size_t(b)*w+a]=0;
        holes.push_back({{float(x)/w,float(y)/h},{float(right)/w,float(y)/h},
                         {float(right)/w,float(bottom)/h},{float(x)/w,float(bottom)/h}});
    }
    return holes;
}
// Texture-space convex partitions for a transparent reflection mask. Recursive
// spatial splitting preserves holes/pillars instead of taking one hull around
// an entire concave window. Opaque source pixels remain in the diffuse pass.
inline ModelWindowHoles ReflectionMaskParts(const gt2::PngImage& image,const std::vector<uint8_t>& mask) {
    using namespace cockpit_window_detail;
    ModelWindowHoles result;const int w=image.width,h=image.height;
    auto divide=[&](auto&& self,int x0,int y0,int x1,int y1)->void {
        int left=x1,right=x0,top=y1,bottom=y0;size_t count=0;std::vector<Point> corners;
        for(int y=y0;y<y1;++y)for(int x=x0;x<x1;++x)if(mask[size_t(y)*w+x]){
            ++count;left=std::min(left,x);right=std::max(right,x+1);top=std::min(top,y);bottom=std::max(bottom,y+1);
            corners.insert(corners.end(),{{float(x),float(y)},{float(x+1),float(y)},{float(x+1),float(y+1)},{float(x),float(y+1)}});
        }
        if(!count)return;
        auto hull=ConvexHull(std::move(corners));double area=0;
        for(size_t j=1;j+1<hull.size();++j)area+=Cross(hull[0],hull[j],hull[j+1])*.5;
        if(double(count)>=area*.98){for(auto& p:hull){p.x/=w;p.y/=h;}result.push_back(std::move(hull));return;}
        if(right-left>=bottom-top){const int mid=(left+right)/2;self(self,left,top,mid,bottom);self(self,mid,top,right,bottom);}
        else {const int mid=(top+bottom)/2;self(self,left,top,right,mid);self(self,left,mid,right,bottom);}
    };
    divide(divide,0,0,w,h);return result;
}
// Near-horizontal translucent decals (wiper shadows, bonnet trim) are not
// cabin glazing, even when their pixels match the glass tint ramp.
inline bool IsWindowLayerNormal(const std::array<float,3>& normal) {
    return std::abs(normal[1])<=.95f;
}
struct ModelGlazingOpening {
    bool reflectionOnly=false;
    std::array<std::array<float,3>,3> points;
    std::array<float,3> normal,lo,hi;
    std::array<std::array<float,4>,3> edges;
};
inline ModelGlazingOpening GlazingOpening(const std::array<gt2::GltfVertex,3>& t) {
    ModelGlazingOpening o{};
    for(size_t j=0;j<3;++j)o.points[j]=t[j].position;
    const auto& a=o.points[0];const auto& b=o.points[1];const auto& c=o.points[2];
    o.normal={(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]),
              (b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]),
              (b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0])};
    float length=0;for(float n:o.normal)length+=n*n;length=std::sqrt(length);
    if(length<1e-9f)return o;
    for(float& n:o.normal)n/=length;
    for(size_t k=0;k<3;++k){o.lo[k]=std::min({a[k],b[k],c[k]});o.hi[k]=std::max({a[k],b[k],c[k]});}
    for(size_t j=0;j<3;++j){const auto& p=o.points[j];const auto& q=o.points[(j+1)%3];
        const std::array<float,3> e{q[0]-p[0],q[1]-p[1],q[2]-p[2]};
        auto& plane=o.edges[j];plane={e[1]*o.normal[2]-e[2]*o.normal[1],e[2]*o.normal[0]-e[0]*o.normal[2],e[0]*o.normal[1]-e[1]*o.normal[0],0};
        for(size_t k=0;k<3;++k)plane[3]+=plane[k]*p[k];
    }
    return o;
}
// Subtract a union of convex volumes without re-triangulating intermediate
// polygons. Adjacent cowl strips otherwise multiply the same body triangles.
using ModelClipVolume=std::vector<std::array<float,4>>;
inline std::vector<gt2::GltfVertex> SubtractModelVolumes(std::span<const gt2::GltfVertex> triangle,
                                                       std::span<const ModelClipVolume> volumes) {
    const auto area=[](std::span<const gt2::GltfVertex> t){
        const auto& a=t[0].position;const auto& b=t[1].position;const auto& c=t[2].position;
        const float x=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
        const float y=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
        const float z=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);return std::sqrt(x*x+y*y+z*z);
    };
    using Poly=std::vector<gt2::GltfVertex>;
    std::vector<Poly> pieces{Poly(triangle.begin(),triangle.end())};
    // Neighbouring mask triangles partition the retained frame into convex
    // pieces. Merge only when their convex hull has exactly the same area:
    // this removes internal cuts without filling a glass opening or body gap.
    auto mergePieces=[&](){
      if(pieces.size()>1){
        const auto basis=GlazingOpening({triangle[0],triangle[1],triangle[2]});
        size_t drop=0;for(size_t k=1;k<3;++k)if(std::abs(basis.normal[k])>std::abs(basis.normal[drop]))drop=k;
        const size_t x=(drop+1)%3,y=(drop+2)%3;
        auto cross=[&](const auto& a,const auto& b,const auto& c){return
            (double(b.position[x])-a.position[x])*(double(c.position[y])-a.position[y])-
            (double(b.position[y])-a.position[y])*(double(c.position[x])-a.position[x]);};
        auto polyArea=[&](const Poly& poly){double sum=0;for(size_t k=1;k+1<poly.size();++k)sum+=cross(poly[0],poly[k],poly[k+1]);return std::abs(sum);};
        auto hull=[&](Poly points){
            std::sort(points.begin(),points.end(),[&](const auto& a,const auto& b){return a.position[x]<b.position[x] || (a.position[x]==b.position[x] && a.position[y]<b.position[y]);});
            points.erase(std::unique(points.begin(),points.end(),[&](const auto& a,const auto& b){return a.position[x]==b.position[x] && a.position[y]==b.position[y];}),points.end());
            if(points.size()<3)return Poly{};Poly out;
            for(const auto& p:points){while(out.size()>=2 && cross(out[out.size()-2],out.back(),p)<=1e-12)out.pop_back();out.push_back(p);}
            const size_t lower=out.size();
            for(size_t k=points.size()-1;k-->0;){const auto& p=points[k];while(out.size()>lower && cross(out[out.size()-2],out.back(),p)<=1e-12)out.pop_back();out.push_back(p);}
            out.pop_back();return out;
        };
        for(auto& poly:pieces)poly=hull(std::move(poly));
        std::erase_if(pieces,[&](const auto& poly){return poly.size()<3 || polyArea(poly)<1e-10;});
        bool merged=true;
        while(merged){merged=false;
            for(size_t a=0;a<pieces.size() && !merged;++a)for(size_t b=a+1;b<pieces.size();++b){
                Poly joined=pieces[a];joined.insert(joined.end(),pieces[b].begin(),pieces[b].end());joined=hull(std::move(joined));
                const double sum=polyArea(pieces[a])+polyArea(pieces[b]),combined=polyArea(joined);
                // Adjacent planes are stored as floats. Their independent
                // intersections can differ by a few ULPs; do not preserve
                // those sub-millimetre numerical seams as separate polygons.
                if(std::abs(sum-combined)>std::max(1e-9,sum*1e-5))continue;
                pieces[a]=std::move(joined);pieces.erase(pieces.begin()+ptrdiff_t(b));merged=true;break;
            }
        }
        // Hulls use positive projected winding; restore the source face winding.
        if(cross(triangle[0],triangle[1],triangle[2])<0)for(auto& poly:pieces)std::reverse(poly.begin(),poly.end());
    }
    };
    // A detailed mask can retain hundreds of separate frame fragments. An
    // unsuccessful all-pairs merge after every texel is cubic repeated work.
    // Revisit merging only after substantial growth, then once at the end.
    size_t nextMerge=32;
    for(const auto& planes:volumes) {
        std::vector<Poly> next;
        const auto clip=[](const Poly& input,const auto& p,double side){
            Poly out;if(input.empty())return out;
            const double epsilon=1e-6*std::sqrt(double(p[0])*p[0]+double(p[1])*p[1]+double(p[2])*p[2]);
            auto d=[&](const auto& v){const double distance=side*(double(p[0])*v.position[0]+double(p[1])*v.position[1]+double(p[2])*v.position[2]-p[3]);
                return std::abs(distance)<=epsilon?0.:distance;};
            auto prev=input.back();double pd=d(prev);
            const auto keep=[&](double distance){return side<0?distance>=0:distance>0;};
            for(const auto& v:input){const double vd=d(v);
                if(keep(pd)!=keep(vd)){const float t=float(pd/(pd-vd));auto edge=prev;
                    for(size_t k=0;k<3;++k){edge.position[k]+=(v.position[k]-prev.position[k])*t;edge.normal[k]+=(v.normal[k]-prev.normal[k])*t;}
                    for(size_t k=0;k<2;++k)edge.uv[k]+=(v.uv[k]-prev.uv[k])*t;
                    for(size_t k=0;k<4;++k)edge.color[k]+=(v.color[k]-prev.color[k])*t;
                    out.push_back(edge);
                }
                if(keep(vd))out.push_back(v);prev=v;pd=vd;
            }return out;
        };
        const auto polyArea=[&](const Poly& p){double sum=0;for(size_t i=1;i+1<p.size();++i){const std::array<gt2::GltfVertex,3> t{p[0],p[i],p[i+1]};sum+=area(t);}return sum;};
        for(const auto& original:pieces){
            // A disjoint aperture can still cross the infinite extension of a
            // polygon edge. Test the actual intersection before partitioning;
            // otherwise every remote mask texel fragments the retained frame.
            auto intersection=original;
            for(const auto& p:planes)intersection=clip(intersection,p,-1);
            if(polyArea(intersection)<1e-9){next.push_back(original);continue;}
            auto remaining=original;
            for(const auto& p:planes){
                if(remaining.empty())break;
                auto outside=clip(remaining,p,1);
                if(polyArea(outside)>1e-9)next.push_back(std::move(outside));
                remaining=clip(remaining,p,-1);
            }
        }
        pieces=std::move(next);if(pieces.empty())break;
        if(pieces.size()>nextMerge){mergePieces();nextMerge=std::max(nextMerge*2,pieces.size()*2);}
    }
    mergePieces();
    // Triangulate only once. Re-triangulating each intermediate subtraction
    // creates a quadratic number of tiny fragments along shared pane edges.
    std::vector<gt2::GltfVertex> result;
    for(const auto& poly:pieces)for(size_t i=1;i+1<poly.size();++i){
        const std::array<gt2::GltfVertex,3> t{poly[0],poly[i],poly[i+1]};
        if(area(t)>1e-9f)result.insert(result.end(),t.begin(),t.end());
    }
    return result;
}
inline std::vector<gt2::GltfVertex> CutGlazingLayers(std::span<const gt2::GltfVertex> triangle,
                                                   const std::vector<ModelGlazingOpening>& openings,float separation=.003f,bool reflection=false) {
    const auto sourcePlane=GlazingOpening({triangle[0],triangle[1],triangle[2]});
    std::vector<ModelClipVolume> volumes;
    for(const auto& o:openings) {
        if(o.reflectionOnly && !reflection)continue;
        // An aperture belongs to a layered surface. A nearby perpendicular
        // frame/door surface can cross its prism without being another pane.
        float alignment=0;for(size_t k=0;k<3;++k)alignment+=sourcePlane.normal[k]*o.normal[k];
        // Opaque trim beside a curved pane can be only millimetres away.
        // It must share the pane's plane, not merely point roughly the same
        // way. Broad curved-layer matching is reserved for blend/reflection
        // passes, whose caller explicitly requests the wider prism.
        if(std::abs(alignment)<(separation<=.003f?.9995f:.9f))continue;
        bool overlaps=true;
        for(size_t k=0;k<3;++k){float lo=triangle[0].position[k],hi=lo;
            for(const auto& v:triangle){lo=std::min(lo,v.position[k]);hi=std::max(hi,v.position[k]);}
            overlaps=overlaps && hi>=o.lo[k]-separation && lo<=o.hi[k]+separation;
        }
        if(!overlaps)continue;
        float minDistance=1e9f,maxDistance=-1e9f;
        for(const auto& v:triangle){float d=0;for(size_t k=0;k<3;++k)d+=(v.position[k]-o.points[0][k])*o.normal[k];
            minDistance=std::min(minDistance,d);maxDistance=std::max(maxDistance,d);
        }
        if(minDistance>=separation || maxDistance<=-separation)continue;
        // Reflection passes can triangulate a curved pane differently. Clip
        // only the part inside the thin aperture prism; rejecting a whole
        // triangle by its farthest corner leaves a large glass reflection.
        ModelClipVolume planes(5);
        std::copy(o.edges.begin(),o.edges.end(),planes.begin());float offset=0;
        for(size_t k=0;k<3;++k)offset+=o.normal[k]*o.points[0][k];
        planes[3]={o.normal[0],o.normal[1],o.normal[2],offset+separation};
        planes[4]={-o.normal[0],-o.normal[1],-o.normal[2],-offset+separation};
        volumes.push_back(std::move(planes));
    }
    return SubtractModelVolumes(triangle,volumes);
}
// Split at texture address boundaries before clipping: GT4 often extends a
// glass UV beyond 1 and relies on clamp-to-edge. Preserve original UVs so all
// generated vertices still sample identically in the retained body pass.
inline std::vector<gt2::GltfVertex> GlazingInside(std::span<const gt2::GltfVertex> triangle,
    const ModelWindowHoles& holes,bool clampU=true,bool clampV=true) {
    using namespace cockpit_window_detail;
    if(holes.size()==1 && holes[0].size()==4 && holes[0][0]==Point{0,0} &&
       holes[0][1]==Point{1,0} && holes[0][2]==Point{1,1} && holes[0][3]==Point{0,1})return {triangle.begin(),triangle.end()};
    using GlazingPolygon=std::vector<gt2::GltfVertex>;
    auto clip=[](const GlazingPolygon& poly,auto distance){
        GlazingPolygon next;if(poly.empty())return next;
        auto prev=poly.back();float pd=distance(prev);
        for(const auto& v:poly){const float vd=distance(v);
            if((vd>=0)!=(pd>=0)){const float t=pd/(pd-vd);auto edge=prev;
                for(size_t k=0;k<3;++k){edge.position[k]+=(v.position[k]-prev.position[k])*t;edge.normal[k]+=(v.normal[k]-prev.normal[k])*t;}
                for(size_t k=0;k<2;++k)edge.uv[k]+=(v.uv[k]-prev.uv[k])*t;
                for(size_t k=0;k<4;++k)edge.color[k]+=(v.color[k]-prev.color[k])*t;
                next.push_back(edge);
            }
            if(vd>=0)next.push_back(v);prev=v;pd=vd;
        }return next;
    };
    std::array<float,2> lo=triangle[0].uv,hi=lo;
    for(const auto& v:triangle)for(size_t k=0;k<2;++k){lo[k]=std::min(lo[k],v.uv[k]);hi[k]=std::max(hi[k],v.uv[k]);}
    struct Region{float lo,hi,offset;int clamp;};
    std::array<std::vector<Region>,2> regions;
    for(size_t k=0;k<2;++k){
        if(k==0?clampU:clampV){
            if(lo[k]<0)regions[k].push_back({lo[k]-1,0,0,-1});
            if(hi[k]>=0 && lo[k]<=1)regions[k].push_back({0,1,0,0});
            if(hi[k]>1)regions[k].push_back({1,hi[k]+1,0,1});
        }else{
            const int first=int(std::floor(lo[k])),last=int(std::floor(hi[k]));
            if(last-first>64)throw std::runtime_error("glazing UV span exceeds 64 repeats");
            for(int n=first;n<=last;++n)regions[k].push_back({float(n),float(n+1),float(n),0});
        }
    }
    GlazingPolygon result;
    for(const auto& u:regions[0])for(const auto& v:regions[1]){
        GlazingPolygon region(triangle.begin(),triangle.end());
        region=clip(region,[&](const auto& p){return p.uv[0]-u.lo;});
        region=clip(region,[&](const auto& p){return u.hi-p.uv[0];});
        region=clip(region,[&](const auto& p){return p.uv[1]-v.lo;});
        region=clip(region,[&](const auto& p){return v.hi-p.uv[1];});
        for(const auto& hole:holes){auto poly=region;
            for(size_t j=0;j<hole.size() && !poly.empty();++j){
                const auto a=hole[j],b=hole[(j+1)%hole.size()];
                poly=clip(poly,[&](const auto& p){
                    const Point uv{u.clamp<0?0.f:u.clamp>0?1.f:p.uv[0]-u.offset,
                                   v.clamp<0?0.f:v.clamp>0?1.f:p.uv[1]-v.offset};
                    return Cross(a,b,uv);
                });
            }
            for(size_t j=1;j+1<poly.size();++j)result.insert(result.end(),{poly[0],poly[j],poly[j+1]});
        }
    }
    return result;
}
}



