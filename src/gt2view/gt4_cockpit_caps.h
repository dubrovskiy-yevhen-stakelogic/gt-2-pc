#pragma once
#include "gt2view/model_glazing.h"
#include "gt2view/native_cockpit_fit.h"

namespace gt2view {
// The imported model's eye is unset for generated interiors. Compare aligned
// source vertices with the fitted seating point, both in world metres.
inline bool Gt4RoofCoatAboveEye(std::span<const gt2::GltfVertex> triangle,
                              float scale,float offsetY,float fittedEyeY) {
    return std::all_of(triangle.begin(),triangle.end(),[&](const auto& point){
        return point.position[1]*scale+offsetY>=fittedEyeY+.025f;
    });
}

// Geometry is shared by all paints. A coloured coat in any palette needs
// its physical backing even when paint zero uses an achromatic texel there.
inline bool Gt4RoofCoatPixel(const uint8_t* c) {
    const int high=std::max({c[0],c[1],c[2]}),low=std::min({c[0],c[1],c[2]});
    return c[3]>=64 && high>32 && high-low>24 &&
        !ModelGlassPixel(c) && !ModelBlendColourGlassPixel(c);
}
inline bool Gt4UnionRoofCoatPixels(std::vector<uint8_t>& mask,
        const gt2::PngImage& base,const gt2::PngImage& paint) {
    if(base.width!=paint.width || base.height!=paint.height ||
       base.rgba.size()!=paint.rgba.size() || mask.size()*4!=paint.rgba.size())return false;
    for(size_t pixel=0;pixel<mask.size();++pixel)
        mask[pixel]=uint8_t(mask[pixel] || Gt4RoofCoatPixel(&paint.rgba[pixel*4]));
    return true;
}
inline void Gt4AppendWindscreenGuard(ModelClipVolume& planes,const CockpitFit& f,
        const std::array<float,3>& cowl) {
    const float u=std::clamp((cowl[0]-f.frontHeader[0][0])/
        std::max(.001f,f.frontHeader[1][0]-f.frontHeader[0][0]),0.f,1.f);
    const float headerY=f.frontHeader[0][1]+u*(f.frontHeader[1][1]-f.frontHeader[0][1]);
    const float headerZ=f.frontHeader[0][2]+u*(f.frontHeader[1][2]-f.frontHeader[0][2]);
    if(headerY>cowl[1]+.05f && headerZ>cowl[2]) {
        const float rake=(headerZ-cowl[2])/(headerY-cowl[1]);
        planes.push_back({0,rake,-1,rake*cowl[1]-cowl[2]-.025f});
    }
}
inline bool Gt4UntexturedBlackCap(std::span<const gt2::GltfVertex> triangle,const gt2::GltfMaterial& material) {
    // Unlit type 0 preserves the authored vertex RGB. Zero/dark RGB
    // remains black for every paint. Lit/reflected packets are not eligible.
    if(material.baseColorImage>=0 || material.nativeGt4Type!=0 || !material.nativeDepthWrite || material.alphaBlend || material.reflection)return false;
    for(const auto& vertex:triangle)for(size_t k=0;k<3;++k)
        if(vertex.color[k]*material.baseColorFactor[k]>32.f/255.f)return false;
    return true;
}

// Both unlit (type 0) and lit (type 1) native packets can use a
// discarded black alpha-zero border. Its filtered black fringe belongs to
// the replacement region; other packet modes remain untouched.
inline bool Gt4CapDiscardedBlackBorder(const gt2::GltfMaterial* material) {
    return material && (material->nativeGt4Type==0 || material->nativeGt4Type==1) &&
        material->nativeDepthWrite && !material->alphaBlend && !material->reflection &&
        material->nativeTextureAlpha && material->nativeTextureFunction<=1 &&
        material->nativeAlphaFail==0 && !NativeAlphaAcceptsZero(*material);
}
inline bool Gt4CapNativeOpaqueAlpha(const gt2::GltfMaterial* material) {
    // Native GS diffuse alpha also carries environment-pass weight. Its
    // 127/128 encoding produces alpha 254 in an entirely opaque depth pass.
    // Only use the recovered alpha-test contract for known nonblended packets.
    return material && material->nativeGt4Type==1 && material->nativeDepthWrite &&
        !material->alphaBlend && !material->reflection && material->nativeTextureAlpha &&
        material->nativeTextureFunction<=1 &&
        (material->nativeAlphaFail==0 || material->nativeAlphaFail==2) &&
        !NativeAlphaAcceptsZero(*material);
}
inline std::vector<uint8_t> Gt4CapOpaquePixels(const gt2::PngImage& image,const gt2::GltfMaterial* material=nullptr) {
    std::vector<uint8_t> mask(image.rgba.size()/4);
    // A dedicated achromatic upholstery atlas contains shading, not separate
    // pieces of furniture. Pixel-wise black clipping leaves the brighter
    // stitching as floating strands. Require an actual dark covered texel and
    // exact achromatic colour throughout the visible atlas; mixed body/paint
    // atlases retain the conservative per-pixel path. All paints intersect
    // this evidence before any spatial cabin cut is applied.
    bool achromatic=true,dark=false;
    for(size_t pixel=0;pixel<mask.size();++pixel) {
        const auto* c=&image.rgba[pixel*4];
        if(c[3])achromatic=achromatic && c[0]==c[1] && c[1]==c[2];
        const bool covered=Gt4CapNativeOpaqueAlpha(material)?!NativeTextureAlphaDiscard(*material,c[3]):c[3]==255;
        dark=dark || (covered && std::max({c[0],c[1],c[2]})<=32);
    }
    for(size_t pixel=0;pixel<mask.size();++pixel) {
        const auto* c=&image.rgba[pixel*4];
        const bool covered=Gt4CapNativeOpaqueAlpha(material)?!NativeTextureAlphaDiscard(*material,c[3]):c[3]==255;
        // A black texel discarded by the original strict alpha test is a
        // harmless part of the replacement region. Bilinear filtering between
        // opaque black and zero-alpha black still draws a black depth-writing
        // fringe. Requiring four opaque samples leaves that fringe suspended
        // over the generated door. Never extend this to colored hidden texels,
        // blended glazing, or alpha-fail modes that retain framebuffer/depth.
        const bool discardedBlack=Gt4CapDiscardedBlackBorder(material) &&
            c[3]==0 && std::max({c[0],c[1],c[2]})<=32;
        mask[pixel]=uint8_t((covered && ((achromatic && dark) || std::max({c[0],c[1],c[2]})<=32)) || discardedBlack);
    }
    return mask;
}
inline void IntersectGt4CapOpaquePixels(std::vector<uint8_t>& mask,
        const gt2::PngImage& reference,const gt2::PngImage& variant,const gt2::GltfMaterial* material=nullptr) {
    if(reference.width!=variant.width || reference.height!=variant.height ||
       variant.rgba.size()!=mask.size()*4) {
        std::fill(mask.begin(),mask.end(),uint8_t(0));return;
    }
    const auto other=Gt4CapOpaquePixels(variant,material);
    for(size_t pixel=0;pixel<mask.size();++pixel)mask[pixel]=uint8_t(mask[pixel] && other[pixel]);
}
// Require every texel in a conservative UV rectangle, including its bilinear
// border, to be opaque black. A dark centroid in a shared atlas is insufficient.
inline bool Gt4CapOpaqueFootprint(std::span<const gt2::GltfVertex> triangle,
        const gt2::PngImage& image,std::span<const uint8_t> mask,bool clampU,bool clampV) {
    if(triangle.size()!=3 || image.width<=0 || image.height<=0 ||
       mask.size()!=size_t(image.width)*image.height)return false;
    std::array<int,2> lo{},hi{};
    for(size_t axis=0;axis<2;++axis) {
        float a=triangle[0].uv[axis],b=a;
        for(const auto& v:triangle){a=std::min(a,v.uv[axis]);b=std::max(b,v.uv[axis]);}
        if(axis?clampV:clampU){a=std::clamp(a,0.f,1.f);b=std::clamp(b,0.f,1.f);}
        const int dimension=axis?image.height:image.width;
        lo[axis]=int(std::floor(a*dimension-.5f));hi[axis]=int(std::floor(b*dimension+.5f));
        if(axis?clampV:clampU){lo[axis]=std::clamp(lo[axis],0,dimension-1);hi[axis]=std::clamp(hi[axis],0,dimension-1);}
        else if(hi[axis]-lo[axis]>dimension+1)return false;
    }
    for(int y=lo[1];y<=hi[1];++y)for(int x=lo[0];x<=hi[0];++x) {
        const int u=clampU?std::clamp(x,0,image.width-1):(x%image.width+image.width)%image.width;
        const int v=clampV?std::clamp(y,0,image.height-1):(y%image.height+image.height)%image.height;
        if(!mask[size_t(v)*image.width+u])return false;
    }
    return true;
}
// A mixed atlas must be clipped by its actual black coverage. Requiring
// a whole triangle's UV box to be black preserves a huge closure when only
// its far tip reaches painted texels. Cells below describe where ALL FOUR
// bilinear samples are invariant opaque black, including clamped edge texels.
inline ModelWindowHoles Gt4CapCoverageHoles(const gt2::PngImage& image,
        std::span<const uint8_t> opaque,bool clampU,bool clampV) {
    const int w=image.width,h=image.height;
    gt2::PngImage cells;cells.width=w+1;cells.height=h+1;
    std::vector<uint8_t> mask(size_t(w+1)*size_t(h+1));
    auto covered=[&](int x,int y){
        x=clampU?std::clamp(x,0,w-1):(x%w+w)%w;
        y=clampV?std::clamp(y,0,h-1):(y%h+h)%h;
        return opaque[size_t(y)*w+x]!=0;
    };
    for(int y=-1;y<h;++y)for(int x=-1;x<w;++x)
        mask[size_t(y+1)*(w+1)+x+1]=covered(x,y)&&covered(x+1,y)&&covered(x,y+1)&&covered(x+1,y+1);
    auto holes=ExactModelMask(cells,std::move(mask));
    for(auto& hole:holes)for(auto& p:hole){p.x=std::clamp((p.x*float(w+1)-.5f)/float(w),0.f,1.f);p.y=std::clamp((p.y*float(h+1)-.5f)/float(h),0.f,1.f);}
    return holes;
}
inline std::vector<gt2::GltfVertex> Gt4CutPartlyBlackCap(std::span<const gt2::GltfVertex> triangle,
        const ModelWindowHoles& holes,bool clampU,bool clampV,std::span<const ModelClipVolume> volumes) {
    const auto black=GlazingInside(triangle,holes,clampU,clampV);
    if(black.empty())return {triangle.begin(),triangle.end()};
    // Subtract only the intersection of proven black coverage and cabin.
    // Splitting/reassembling all black texels outside the cabin creates huge
    // meshes even when none of those texels is removed.
    std::vector<ModelClipVolume> cuts;
    for(size_t i=0;i+2<black.size();i+=3) {
        const auto opening=GlazingOpening({black[i],black[i+1],black[i+2]});
        float n2=0;for(float n:opening.normal)n2+=n*n;if(n2<.5f)continue;
        for(const auto& volume:volumes) {
            auto cut=volume;cut.insert(cut.end(),opening.edges.begin(),opening.edges.end());
            cuts.push_back(std::move(cut));
        }
    }
    return SubtractModelVolumes(triangle,cuts);
}
// Preserve every texel that can contribute through bilinear filtering.
// A fully transparent carrier is not a physical attachment to the body.
inline ModelWindowHoles Gt4VisibleMaskCoverage(const gt2::PngImage& image,
        std::span<const uint8_t> visible,bool clampU,bool clampV) {
    const int w=image.width,h=image.height;
    if(w<=0 || h<=0 || visible.size()!=size_t(w)*h)return {};
    gt2::PngImage cells;cells.width=w+1;cells.height=h+1;
    std::vector<uint8_t> support(size_t(w+1)*size_t(h+1));
    auto covered=[&](int x,int y){
        x=clampU?std::clamp(x,0,w-1):(x%w+w)%w;
        y=clampV?std::clamp(y,0,h-1):(y%h+h)%h;
        return visible[size_t(y)*w+x]!=0;
    };
    for(int y=-1;y<h;++y)for(int x=-1;x<w;++x)
        support[size_t(y+1)*(w+1)+x+1]=covered(x,y)||covered(x+1,y)||covered(x,y+1)||covered(x+1,y+1);
    auto holes=ExactModelMask(cells,std::move(support));
    for(auto& hole:holes)for(auto& q:hole){q.x=std::clamp((q.x*float(w+1)-.5f)/float(w),0.f,1.f);q.y=std::clamp((q.y*float(h+1)-.5f)/float(h),0.f,1.f);}
    return holes;
}
inline bool Gt4CapSpansPassenger(std::span<const gt2::GltfVertex> triangle,
        const CockpitFit& fit,float scale,const std::array<float,3>& offset) {
    if(triangle.size()!=3)return false;
    const auto plane=GlazingOpening({triangle[0],triangle[1],triangle[2]});
    float left=1e9f,right=-1e9f;
    for(const auto& v:triangle) {
        const float x=v.position[0]*scale+offset[0];left=std::min(left,x);right=std::max(right,x);
    }
    // A cap can be authored as separate left/right wedges. Its penetration
    // into the central passenger lane, not individual triangle width, identifies
    // replacement furniture. Side-only shoulders remain outside this lane.
    // Classify a shelf by its dominant upward/downward normal: a tilted
    // dashboard cap is still a shelf up to 45 degrees. Passenger penetration,
    // invariant black coverage and the cabin volume remain mandatory.
    return plane.normal[1]*plane.normal[1]>=plane.normal[0]*plane.normal[0]+plane.normal[2]*plane.normal[2] &&
        left<fit.halfWidth*.50f && right>-fit.halfWidth*.50f;
}
// This is replacement of the generated closed cabin's horizontal LOD caps.
// Native/open cabins have no corresponding replacement enclosure. The same
// curved cowl, floor, rear wall and header bounds protect the exterior shell.
// The generated lower enclosure replaces interior cards regardless of
// their normal. Follow the measured side-window belt, inset from the actual
// body, instead of extending a narrow windscreen cut to a guessed width.
// Only invariant opaque-black UV coverage is submitted to these volumes.
inline bool Gt4HorizontalSideShelf(std::span<const gt2::GltfVertex> triangle) {
    if(triangle.size()!=3)return false;
    const auto plane=GlazingOpening({triangle[0],triangle[1],triangle[2]});
    return std::abs(plane.normal[1])>=.85f;
}
// Interior black closures can extend beyond a vertical header-width cut.
// Follow the inward lean of the side shell, while retaining a 50 mm border.
// Each slab follows the measured sill line and uses its most inward lean;
// it therefore cannot cross the measured curved side boundary.
inline std::vector<ModelClipVolume> Gt4UpperCabinVolumes(const CockpitFit& f,
        float scale,const std::array<float,3>& offset) {
    std::vector<ModelClipVolume> result;
    if(f.nativeInterior || f.openTop || !f.authoredUpperShell || !f.sideSillValid)return result;
    const float roof=std::min({f.frontHeader[0][1],f.frontHeader[1][1],f.rearHeader[0][1],f.rearHeader[1][1]});
    const float leftTop=std::max(f.frontHeader[0][0],f.rearHeader[0][0]);
    const float rightTop=std::min(f.frontHeader[1][0],f.rearHeader[1][0]);
    const float front=std::max(f.frontCowl[0][2],f.frontCowl[1][2])+.025f;
    // The rear cowl may come from the engine cover/body profile when no
    // rear glazing exists. It does not authorize removing upper side panels
    // beyond the passenger roof. Preserve that opaque rear-quarter structure.
    const float back=std::min({f.rearCowl[0][2],f.rearCowl[1][2],
        f.rearHeader[0][2],f.rearHeader[1][2]})-.025f;
    if(back<=front || roof<=f.floorY+.15f)return result;
    std::vector<float> knots{front,back};
    for(const auto& side:f.sideSill)for(const auto& q:side)if(q[2]>front && q[2]<back)knots.push_back(q[2]);
    std::sort(knots.begin(),knots.end());knots.erase(std::unique(knots.begin(),knots.end()),knots.end());
    // Interpolated sill samples are not geometric corners. Keeping them as
    // separate clipping cells multiplies UV fragments on highly detailed cars.
    // Remove only knots lying on both source sill segments within the existing
    // one-micrometre clipping band; genuine curved/asymmetric corners remain.
    for(size_t i=1;i+1<knots.size();) {
        const float u=(knots[i]-knots[i-1])/(knots[i+1]-knots[i-1]);bool linear=true;
        for(size_t side=0;side<2;++side){
            const auto a=CockpitSideSillAt(f,side,knots[i-1]);
            const auto b=CockpitSideSillAt(f,side,knots[i+1]);
            const auto q=CockpitSideSillAt(f,side,knots[i]);
            for(size_t axis=0;axis<2;++axis)linear=linear && std::abs(q[axis]-(a[axis]+u*(b[axis]-a[axis])))<=.0000002f;
        }
        if(linear)knots.erase(knots.begin()+ptrdiff_t(i));else ++i;
    }
    for(size_t i=1;i<knots.size();++i) {
        const float z0=knots[i-1],z1=knots[i];
        const auto l0=CockpitSideSillAt(f,0,z0),l1=CockpitSideSillAt(f,0,z1);
        const auto r0=CockpitSideSillAt(f,1,z0),r1=CockpitSideSillAt(f,1,z1);
        if(roof<=std::max({l0[1],l1[1],r0[1],r1[1]})+.08f || z1<=z0)continue;
        // Follow each linear sill segment continuously instead of making a
        // staircase of constant-width slabs. The greater inward lean at the
        // endpoints bounds the entire segment without moving outside the shell.
        const float ls=std::max({0.f,(leftTop-l0[0])/(roof-l0[1]),(leftTop-l1[0])/(roof-l1[1])});
        const float rs=std::min({0.f,(rightTop-r0[0])/(roof-r0[1]),(rightTop-r1[0])/(roof-r1[1])});
        const float ldx=(l1[0]-l0[0])/(z1-z0),ldy=(l1[1]-l0[1])/(z1-z0);
        const float rdx=(r1[0]-r0[0])/(z1-z0),rdy=(r1[1]-r0[1])/(z1-z0);
        const float lz=ldx-ls*ldy,rz=-rdx+rs*rdy;
        ModelClipVolume planes{{-1,ls,lz,-l0[0]+ls*l0[1]+lz*z0-.05f},
            {1,-rs,rz,r0[0]-rs*r0[1]+rz*z0-.05f},
            {0,-1,ldy,-l0[1]+ldy*z0},{0,-1,rdy,-r0[1]+rdy*z0},
            {0,1,0,roof-.06f},{0,0,-1,-z0},{0,0,1,z1}};
        // Retain the existing front-screen rake protection as well as the roof.
        for(const auto& q:f.cowl)Gt4AppendWindscreenGuard(planes,f,q);
        for(auto& plane:planes)plane[3]=(plane[3]-plane[0]*offset[0]-plane[1]*offset[1]-plane[2]*offset[2])/scale;
        result.push_back(std::move(planes));
    }
    return result;
}
inline std::vector<ModelClipVolume> Gt4LowerCabinVolumes(const CockpitFit& fit,
        float scale,const std::array<float,3>& offset,bool horizontalShelf=false) {
    std::vector<ModelClipVolume> result;
    if(fit.nativeInterior || fit.openTop || !fit.authoredUpperShell || !fit.sideSillValid)return result;
    // Only nearly horizontal, invariant-black shelves use this taller cut.
    // Their inward reach is measured from the actual window, not half of the
    // much wider exterior body. A low, wide car can otherwise keep an old cap
    // entirely outside the arbitrary central passenger lane.
    const float shelfTop=std::min({fit.frontHeader[0][1],fit.frontHeader[1][1],
                                  fit.rearHeader[0][1],fit.rearHeader[1][1]})-.06f;
    if(horizontalShelf && shelfTop<=fit.floorY+.1f)return result;
    const float roofFrontZ=(fit.frontHeader[0][2]+fit.frontHeader[1][2])*.5f;
    const float roofBackZ=(fit.rearHeader[0][2]+fit.rearHeader[1][2])*.5f;
    auto roofLimit=[&](float z){
        if(roofBackZ<=roofFrontZ+.1f)return shelfTop;
        const float u=std::clamp((z-roofFrontZ)/(roofBackZ-roofFrontZ),0.f,1.f);
        const float local=(1-u)*std::min(fit.frontHeader[0][1],fit.frontHeader[1][1])+
            u*std::min(fit.rearHeader[0][1],fit.rearHeader[1][1])-.06f;
        // A rising rear header does not prove that every upper source card
        // below its interpolated line is furniture. Preserve the established
        // upper-shell band above the front header throughout the cabin.
        const float sourceRoofGuard=std::min(fit.frontHeader[0][1],fit.frontHeader[1][1])-.025f;
        return std::min(local,sourceRoofGuard);
    };

    const float sideInset=horizontalShelf?.05f:.025f;
    float front=1e9f,frontMax=-1e9f;
    for(const auto& q:fit.cowl){front=std::min(front,q[2]);frontMax=std::max(frontMax,q[2]);}
    front+=.025f;frontMax+=.025f;
    // The rear aperture can be asymmetric. A constant plane at its
    // foremost corner leaves an interior wedge across the opposite side.
    // Follow the actual rear edge while retaining its 25 mm body margin.
    const float rearWidth=fit.rearCowl[1][0]-fit.rearCowl[0][0];
    const float rearSlope=rearWidth>.001f?(fit.rearCowl[1][2]-fit.rearCowl[0][2])/rearWidth:0.f;
    const float rearLimit=fit.rearCowl[0][2]-rearSlope*fit.rearCowl[0][0]-.025f;
    const float back=std::max(fit.rearCowl[0][2],fit.rearCowl[1][2])-.025f;
    if(back<=front)return result;
    // Include every original sill knot from both sides. This preserves
    // curved/asymmetric door boundaries without interpolating over a corner.
    std::vector<float> stations{front,back};
    if(frontMax>front && frontMax<back)stations.push_back(frontMax);
    for(float z:{roofFrontZ,roofBackZ})if(z>front && z<back)stations.push_back(z);
    const float roofRise=std::min(fit.rearHeader[0][1],fit.rearHeader[1][1])-
        std::min(fit.frontHeader[0][1],fit.frontHeader[1][1]);
    if(roofBackZ>roofFrontZ+.1f && roofRise>.035f) {
        const float crossing=roofFrontZ+(roofBackZ-roofFrontZ)*(.035f/roofRise);
        if(crossing>front && crossing<back)stations.push_back(crossing);
    }

    for(const auto& side:fit.sideSill)for(const auto& q:side)
        if(q[2]>front && q[2]<back)stations.push_back(q[2]);
    std::sort(stations.begin(),stations.end());stations.erase(std::unique(stations.begin(),stations.end()),stations.end());
    for(size_t i=1;i<stations.size();++i){
        // Left/right measurements can differ by a few micrometres. Skipping
        // their short interval leaves a real uncut strip between the volumes.
        const float z0=stations[i-1],z1=stations[i],dz=z1-z0;if(!(dz>0.f))continue;
        const auto l0=CockpitSideSillAt(fit,0,z0),l1=CockpitSideSillAt(fit,0,z1);
        const auto r0=CockpitSideSillAt(fit,1,z0),r1=CockpitSideSillAt(fit,1,z1);
        const float ls=(l1[0]-l0[0])/dz,rs=(r1[0]-r0[0])/dz;
        const float ly=(l1[1]-l0[1])/dz,ry=(r1[1]-r0[1])/dz;
        // The fitted belt is sampled from texel-sized aperture steps. A 25 mm
        // vertical overlap removes coplanar inner caps; the separate 25 mm
        // lateral inset preserves the actual outer sill and window frame.
        // Each door follows its own measured belt. Taking the minimum of
        // both sides across the entire cabin leaves a shelf under the higher
        // sill whenever the original model is asymmetric.
        for(size_t side=0;side<2;++side) {
            ModelClipVolume planes{{-1,0,ls,-l0[0]+ls*z0-sideInset},{1,0,-rs,r0[0]-rs*z0-sideInset},
                {0,-1,0,-fit.floorY-.025f},{0,0,-1,-z0},{0,0,1,z1},{-rearSlope,0,1,rearLimit}};
            // A rising rear belt may reach above the front header. Lower
            // enclosure replacement must never cut the protected upper shell.
            const float roof0=roofLimit(z0),roof1=roofLimit(z1),roofSlope=(roof1-roof0)/dz;
            // Preserve the local sloping roof. The lowest rear header is not
            // a horizontal ceiling through the entire passenger compartment.
            if(!horizontalShelf && shelfTop>fit.floorY+.1f)planes.push_back({0,1,-roofSlope,roof0-roofSlope*z0});
            if(horizontalShelf)planes.push_back({0,1,-roofSlope,roof0-roofSlope*z0});
            else planes.push_back(side?std::array<float,4>{0,1,-ry,r0[1]-ry*z0+.025f}:std::array<float,4>{0,1,-ly,l0[1]-ly*z0+.025f});
            planes.push_back({side?-1.f:1.f,0,0,0});
            // A raked windscreen backing can meet the horizontal-shelf
            // normal threshold. Keep its authored envelope outside every
            // lower replacement volume, including the taller shelf pass.
            if(z0>=frontMax)for(const auto& cowl:fit.cowl)Gt4AppendWindscreenGuard(planes,fit,cowl);
            auto append=[&](ModelClipVolume volume){
                for(auto& plane:volume)plane[3]=(plane[3]-plane[0]*offset[0]-plane[1]*offset[1]-plane[2]*offset[2])/scale;
                result.push_back(std::move(volume));
            };
            if(z0>=frontMax)append(std::move(planes));
            else {
                // The forward door region follows the curved cowl in X, not
                // its rearmost corner across the whole cabin. Partition only
                // the short forward region; all rear stations stay unchanged.
                auto strip=[&](float left,float right,float leftZ,float rightZ,float leftY,float rightY){
                    if(right<=left || std::min(leftZ,rightZ)+.025f>=z1)return;
                    const float slope=(rightZ-leftZ)/(right-left);
                    auto part=planes;
                    part.push_back({-1,0,0,-left});part.push_back({1,0,0,right});
                    part.push_back({slope,0,-1,slope*left-leftZ-.025f});
                    Gt4AppendWindscreenGuard(part,fit,{(left+right)*.5f,(leftY+rightY)*.5f,(leftZ+rightZ)*.5f});
                    append(std::move(part));
                };
                strip(-fit.halfWidth,fit.cowl.front()[0],fit.cowl.front()[2],fit.cowl.front()[2],fit.cowl.front()[1],fit.cowl.front()[1]);
                for(size_t c=1;c<fit.cowl.size();++c)strip(fit.cowl[c-1][0],fit.cowl[c][0],fit.cowl[c-1][2],fit.cowl[c][2],fit.cowl[c-1][1],fit.cowl[c][1]);
                strip(fit.cowl.back()[0],fit.halfWidth,fit.cowl.back()[2],fit.cowl.back()[2],fit.cowl.back()[1],fit.cowl.back()[1]);
            }
        }
    }
    return result;
}
inline std::vector<ModelClipVolume> Gt4CabinCapVolumes(const CockpitFit& fit,
        float scale,const std::array<float,3>& offset) {
    std::vector<ModelClipVolume> result;
    if(fit.nativeInterior || fit.openTop || !fit.authoredUpperShell)return result;
    const float back=std::max({fit.rearWindowZ,fit.rearCowl[0][2],fit.rearCowl[1][2]})+.025f;
    const float frontRoof=std::min(fit.frontHeader[0][1],fit.frontHeader[1][1]);
    const float rearRoof=std::min(fit.rearHeader[0][1],fit.rearHeader[1][1]);
    const float frontRoofZ=(fit.frontHeader[0][2]+fit.frontHeader[1][2])*.5f;
    const float rearRoofZ=(fit.rearHeader[0][2]+fit.rearHeader[1][2])*.5f;
    // Follow a descending roof instead of letting the horizontal front-header
    // plane cut through its rear half. Invalid rear-header evidence retains
    // the previous conservative front-header guard.
    const float roofSlope=rearRoofZ>frontRoofZ+.1f && rearRoof>fit.floorY+.2f?
        std::min(0.f,(rearRoof-frontRoof)/(rearRoofZ-frontRoofZ)):0.f;
    const float high=frontRoof-.06f-roofSlope*frontRoofZ;
    // Header corners can sit above a lower transverse inner roof strip.
    // Cabin replacement must also stay below the lowest measured header,
    // not only a line descending from the high front corner.
    const float lowestRoof=(rearRoof>fit.floorY+.2f?std::min(frontRoof,rearRoof):frontRoof)-.06f;
    for(size_t strip=1;strip<fit.cowl.size();++strip) {
        const auto& a=fit.cowl[strip-1];const auto& b=fit.cowl[strip];
        const float slope=(b[2]-a[2])/std::max(.001f,b[0]-a[0]);
        const float left=a[0],right=b[0];
        ModelClipVolume planes{{1,0,0,right},{-1,0,0,-left},{0,-1,0,-fit.floorY},{0,1,-roofSlope,high},{0,1,0,lowestRoof},
            {slope,0,-1,slope*a[0]-a[2]-.025f},{0,0,1,back}};
        Gt4AppendWindscreenGuard(planes,fit,{(a[0]+b[0])*.5f,(a[1]+b[1])*.5f,(a[2]+b[2])*.5f});
        for(auto& plane:planes)plane[3]=(plane[3]-plane[0]*offset[0]-plane[1]*offset[1]-plane[2]*offset[2])/scale;
        result.push_back(std::move(planes));
    }
    // CowlZAt clamps outside the fitted windscreen. Keep that same boundary
    // when extending to a wide source cap; extrapolating its last slope can
    // cross the bonnet on an outward-forward cowl corner.
    for(size_t side=0;side<2;++side) {
        const auto& end=side?fit.cowl.back():fit.cowl.front();
        const float left=side?end[0]:-fit.halfWidth+.025f;
        const float right=side?fit.halfWidth-.025f:end[0];
        if(right<=left)continue;
        ModelClipVolume planes{{1,0,0,right},{-1,0,0,-left},{0,-1,0,-fit.floorY},{0,1,-roofSlope,high},{0,1,0,lowestRoof},
            {0,0,-1,-end[2]-.025f},{0,0,1,back}};
        Gt4AppendWindscreenGuard(planes,fit,end);
        for(auto& plane:planes)plane[3]=(plane[3]-plane[0]*offset[0]-plane[1]*offset[1]-plane[2]*offset[2])/scale;
        result.push_back(std::move(planes));
    }
    return result;
}
// Keep only the exact projected coloured roof footprint. Texture borders
// can cross a triangle, so protecting its whole face is not sufficient.
template<class Cut> inline std::vector<gt2::GltfVertex> Gt4CutWithRoofBacking(
        std::span<const gt2::GltfVertex> triangle,
        const std::vector<ModelGlazingOpening>& coats,float scale,Cut cut) {
    const auto initial=cut(triangle);
    const auto unchanged=[&](const auto& result){return result.size()==triangle.size() &&
        std::equal(result.begin(),result.end(),triangle.begin(),[](const auto& a,const auto& b){return a.position==b.position && a.uv==b.uv;});};
    if(triangle.size()!=3 || coats.empty() || unchanged(initial))return initial;
    const auto plane=GlazingOpening({triangle[0],triangle[1],triangle[2]});
    if(std::abs(plane.normal[1])<.65f)return initial;
    std::vector<ModelGlazingOpening> parallel;
    for(const auto& coat:coats){float dot=0;for(size_t k=0;k<3;++k)dot+=plane.normal[k]*coat.normal[k];if(std::abs(dot)>=.98f)parallel.push_back(coat);}
    const auto volumes=ModelGlazingCutVolumes(triangle,parallel,.012f/scale);
    if(volumes.empty())return initial;
    const auto outside=SubtractModelVolumes(triangle,volumes);
    if(outside.size()==3 && std::equal(outside.begin(),outside.end(),triangle.begin(),
       [](const auto& a,const auto& b){return a.position==b.position && a.uv==b.uv;}))return initial;
    // Subtract the unprotected planar pieces to obtain the complementary
    // protected union without overlap from adjacent texture-mask partitions.
    std::vector<ModelGlazingOpening> unprotected;
    for(size_t i=0;i+2<outside.size();i+=3)unprotected.push_back(GlazingOpening({outside[i],outside[i+1],outside[i+2]}));
    auto result=SubtractModelVolumes(triangle,ModelGlazingCutVolumes(triangle,unprotected,.001f/scale));
    for(size_t i=0;i+2<outside.size();i+=3) {
        const auto kept=cut(std::span<const gt2::GltfVertex>(outside).subspan(i,3));
        result.insert(result.end(),kept.begin(),kept.end());
    }
    return result;
}
} // namespace gt2view
