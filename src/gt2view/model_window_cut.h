#pragma once
#include "gt2view/cockpit_window_cut.h"
#include "gt2view/cockpit_fit.h"
#include "gt2formats/gltf_reader.h"
#include <unordered_map>

namespace gt2view {
// With source-alpha blending, zero texture alpha contributes no colour even
// when GS AFAIL is FB_ONLY/RGB_ONLY. Such carrier corners must not attach a
// visible seat card to the shell during furniture classification. This only
// supplies classification support; unchanged source triangles are restored.
inline bool ModelZeroAlphaBlendSupport(const gt2::GltfMaterial& m) {
    return m.nativeGt4Type==1 && m.alphaBlend && !m.reflection &&
        m.nativeTextureAlpha && m.nativeTextureFunction<=1 &&
        m.nativeBlendEquation==0x44;
}
// The PS2 glass ramp is baked into several diffuse/reflection passes. Alpha
// alone is not coverage (it also stores reflectivity). Keep each connected
// pane separate so a black pillar between two panes survives the cut.
inline bool ModelGlassPixel(const uint8_t* p) {
    return p[1]>=p[2] && p[2]>p[0] && p[1]-p[0]<=24 && p[1]<=96;
}
inline bool ModelNeutralGlassPixel(const uint8_t* p) {
    const int high=std::max({p[0],p[1],p[2]}),low=std::min({p[0],p[1],p[2]});
    return p[3]>0 && p[3]<250 && high<=128 && high-low<=8;
}
// Blended native panes encode the frame in opaque texels of the same
// teal colour as the translucent glass. Colour alone must not erase it.
inline bool ModelBlendColourGlassPixel(const uint8_t* p) {
    const bool cyan=p[1]>=p[2] && p[2]>p[0] && p[1]-p[0]<=64;
    const bool darkBlue=p[2]<=32 && p[2]>=p[1] && p[1]>=p[0] && p[2]>p[0];
    const bool darkGreen=p[1]<=32 && p[1]>=p[2] && p[2]>=p[0] && p[1]>p[0];
    return p[3]>0 && p[3]<250 && (cyan || darkBlue || darkGreen);
}
inline bool ModelBrightGlassPixel(const uint8_t* p) {
    const int high=std::max({p[0],p[1],p[2]}),low=std::min({p[0],p[1],p[2]});
    return p[3]>0 && p[3]<250 && low>=128 && high-low<=2;
}
inline bool ModelBlendGlassPixel(const uint8_t* p) {
    return ModelBlendColourGlassPixel(p) || ModelNeutralGlassPixel(p) || ModelBrightGlassPixel(p);
}
// Mixed neutral glazing can carry small opaque fasteners. This classifier
// only enables exact aperture masks; it must never drop the whole material.
inline bool HasBrightNeutralWindowField(const gt2::PngImage& image) {
    if(image.width<4 || image.height<4 || image.rgba.size()!=size_t(image.width)*image.height*4)return false;
    size_t field=0;std::array<size_t,4> edge{},length{size_t(image.width),size_t(image.width),size_t(image.height),size_t(image.height)};
    for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){
        const auto* p=&image.rgba[(size_t(y)*image.width+x)*4];
        const int hi=std::max({p[0],p[1],p[2]}),lo=std::min({p[0],p[1],p[2]});
        if(p[3] && hi-lo>8)return false;
        if(!ModelBrightGlassPixel(p))continue;
        ++field;if(y==0)++edge[0];if(y==image.height-1)++edge[1];
        if(x==0)++edge[2];if(x==image.width-1)++edge[3];
    }
    if(field*10<size_t(image.width)*image.height*9)return false;
    for(size_t i=0;i<edge.size();++i)if(edge[i]*10<length[i]*9)return false;
    return true;
}
// Sparse one-texel neutral lines are mesh/defroster coverage, not a glass
// field. Cutting every half-alpha line leaves isolated opaque dots and turns
// a few authored triangles into thousands. Require a two-dimensional field;
// a wholly translucent tiny texture still represents a complete tint layer.
inline bool ModelHasTranslucentField(const gt2::PngImage& image,std::span<const uint8_t> mask) {
    const int w=image.width,h=image.height;
    if(w<=0 || h<=0 || mask.size()!=size_t(w)*h)return false;
    if(std::all_of(mask.begin(),mask.end(),[](uint8_t p){return p!=0;}))return true;
    for(int y=0;y+1<h;++y)for(int x=0;x+1<w;++x)
        if(mask[size_t(y)*w+x] && mask[size_t(y)*w+x+1] &&
           mask[size_t(y+1)*w+x] && mask[size_t(y+1)*w+x+1])return true;
    return false;
}
// Diffuse window atlases carry opaque pillars alongside translucent
// glass (or holes). Blending applies to the pane, not to the opaque frame.
// Exact UV aperture cuts below still remove the pane itself.
inline bool ModelHasOpaqueWindowFrame(const gt2::PngImage& image) {
    if(image.width<=0 || image.height<=0 ||
       image.rgba.size()!=size_t(image.width)*image.height*4)return false;
    bool opaque=false,aperture=false;
    for(size_t i=0;i<image.rgba.size();i+=4){
        const auto* px=&image.rgba[i];
        opaque=opaque || px[3]>=254;
        aperture=aperture || !px[3] || ModelBlendGlassPixel(px);
    }
    return opaque && aperture;
}
// Both GT4 diffuse material branches carry structural window masks.
// Dynamic lamp overlays and wheel attachments cannot provide a window frame;
// their near-full alpha highlights otherwise acquire false structural protection.
// Type 0 is used by the Focus body/window atlas; restricting this to the
// type 1 lighting branch lets the cabin volume erase its opaque B-pillar.
inline bool ModelHasStructuralWindowFrame(const gt2::PngImage& image,
                                          const gt2::GltfMaterial& material) {
    return (material.nativeGt4Type==0 || material.nativeGt4Type==1) &&
        !material.reflection && !material.lampStates && material.wheelAttachment<0 &&
        ModelHasOpaqueWindowFrame(image);
}
// A pure coverage atlas supplies its own aperture. A nearby pane can have
// different UVs/curvature and must not cut its opaque pillar silhouette.
// 254 is the near-full alpha produced by the original GT4 conversion.
inline bool ModelStructuralCoverageMask(const gt2::PngImage& image) {
    const int w=image.width,h=image.height;
    if(w<=0 || h<=0 || image.rgba.size()!=size_t(w)*h*4)return false;
    bool clear=false,solid=false;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const auto alpha=image.rgba[(size_t(y)*w+x)*4+3];
        if(!alpha){clear=true;continue;}
        if(alpha>=254){solid=true;continue;}
        bool edge=false;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
            const int nx=x+dx,ny=y+dy;
            if(nx>=0 && nx<w && ny>=0 && ny<h &&
               image.rgba[(size_t(ny)*w+nx)*4+3]>=254)edge=true;
        }
        if(!edge)return false;
    }
    return clear && solid;
}
// Coverage masks may contain antialiased edge texels. A fractional
// field is not a structural stencil: every fractional texel must touch
// opaque coverage. Both an aperture and opaque frame must be present.
inline bool ModelStructuralBlackMask(const gt2::PngImage& image) {
    const int w=image.width,h=image.height;
    if(w<=0 || h<=0 || image.rgba.size()!=size_t(w)*h*4)return false;
    bool clear=false,solid=false;
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){
        const auto* px=&image.rgba[(size_t(y)*w+x)*4];
        if(!px[3]){clear=true;continue;}
        if(std::max({px[0],px[1],px[2]})>32)return false;
        if(px[3]==255){solid=true;continue;}
        bool edge=false;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){
            const int nx=x+dx,ny=y+dy;
            if(nx>=0 && nx<w && ny>=0 && ny<h &&
               image.rgba[(size_t(ny)*w+nx)*4+3]==255)edge=true;
        }
        if(!edge)return false;
    }
    return clear && solid;
}
struct NeutralOverlay {bool translucent=false,flat=false;};
inline NeutralOverlay ClassifyNeutralOverlay(const gt2::PngImage& image) {
    size_t visible=0,dominant=0;std::unordered_map<uint32_t,size_t> counts;
    for(size_t i=0;i<image.rgba.size();i+=4){const auto* p=&image.rgba[i];if(!p[3])continue;
        if(p[3]>=250 || std::max({p[0],p[1],p[2]})-std::min({p[0],p[1],p[2]})>8)return {};
        ++visible;const uint32_t key=uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
        dominant=std::max(dominant,++counts[key]);
    }
    return {visible!=0,visible!=0 && dominant*2>=visible};
}
using ModelWindowHoles=std::vector<std::vector<cockpit_window_detail::Point>>;
inline ModelWindowHoles ModelGlassHoles(const gt2::PngImage& image,const std::vector<uint8_t>& invariant) {
    using namespace cockpit_window_detail;
    const int w=image.width,h=image.height;auto pending=invariant;ModelWindowHoles holes;
    if(!invariant.empty() && std::all_of(invariant.begin(),invariant.end(),[](uint8_t p){return p!=0;}))
        return {{{0,0},{1,0},{1,1},{0,1}}};
    for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
        const size_t at=size_t(y)*w+x;if(!pending[at])continue;
        std::vector<size_t> cells{at};pending[at]=0;std::vector<Point> corners;
        for(size_t head=0;head<cells.size();++head){
            const int cx=int(cells[head]%w),cy=int(cells[head]/w);
            corners.insert(corners.end(),{{float(cx)/w,float(cy)/h},{float(cx+1)/w,float(cy)/h},
                {float(cx+1)/w,float(cy+1)/h},{float(cx)/w,float(cy+1)/h}});
            for(const auto d:std::array<std::array<int,2>,4>{{{-1,0},{1,0},{0,-1},{0,1}}}){
                const int nx=cx+d[0],ny=cy+d[1];if(nx<0 || ny<0 || nx>=w || ny>=h)continue;
                const size_t next=size_t(ny)*w+nx;if(pending[next]){pending[next]=0;cells.push_back(next);}
            }
        }
        if(cells.size()>=12)holes.push_back(ConvexHull(std::move(corners)));
    }
    return holes;
}
inline std::vector<gt2::GltfVertex> CutModelWindows(std::span<const gt2::GltfVertex> triangle,
    const ModelWindowHoles& holes,const CockpitFit& fit,float scale,const std::array<float,3>& offset) {
    using namespace cockpit_window_detail;
    auto unchanged=[&](){return std::vector<gt2::GltfVertex>(triangle.begin(),triangle.end());};
    if(triangle.size()!=3 || holes.empty())return unchanged();
    std::array<std::array<float,3>,3> world{};
    for(size_t i=0;i<3;++i)for(size_t k=0;k<3;++k)world[i][k]=triangle[i].position[k]*scale+offset[k];
    if(cockpit_detail::TriangleAheadOfCowl(world,fit.cowl,0.f))return unchanged();
    std::array<float,3> center{};
    for(const auto& v:triangle)for(size_t k=0;k<3;++k)center[k]+=(v.position[k]*scale+offset[k])/3.f;
    const size_t side=center[0]<0?0:1;
    // Only cabin glazing: exclude lamps, mirrors, badges and body paint below
    // the authored sill. Colour classification is also invariant across paints.
    if(center[1]<CockpitSideSillAt(fit,side,center[2])[1]-.10f ||
       center[2]<fit.frontCowl[side][2]-.25f || center[2]>fit.rearCowl[side][2]+.25f ||
       std::abs(center[0])>fit.halfWidth+.10f)return unchanged();
    const Point a{triangle[0].uv[0],triangle[0].uv[1]},b{triangle[1].uv[0],triangle[1].uv[1]},c{triangle[2].uv[0],triangle[2].uv[1]};
    const float area=Cross(a,b,c);if(std::abs(area)<1e-8f)return unchanged();
    Polygon original;for(const auto& v:triangle){gt2::CarMeshVertex p{};p.texel[0]=v.uv[0];p.texel[1]=v.uv[1];original.push_back(p);}
    std::vector<Polygon> pieces{original};
    for(const auto& hole:holes){std::vector<Polygon> next;for(const auto& piece:pieces)Subtract(piece,hole,next);pieces=std::move(next);}
    std::vector<gt2::CarMeshVertex> clipped;for(const auto& piece:pieces)AppendTriangles(piece,clipped);
    std::vector<gt2::GltfVertex> result;result.reserve(clipped.size());
    for(const auto& v:clipped){
        const auto uv=UV(v);const float weights[]{Cross(b,c,uv)/area,Cross(c,a,uv)/area,Cross(a,b,uv)/area};
        auto out=triangle[0];out.position={};out.normal={};out.color={};out.uv={uv.x,uv.y};
        for(size_t i=0;i<3;++i){for(size_t k=0;k<3;++k){out.position[k]+=triangle[i].position[k]*weights[i];out.normal[k]+=triangle[i].normal[k]*weights[i];}
            for(size_t k=0;k<4;++k)out.color[k]+=triangle[i].color[k]*weights[i];}
        result.push_back(out);
    }
    return result;
}
}
