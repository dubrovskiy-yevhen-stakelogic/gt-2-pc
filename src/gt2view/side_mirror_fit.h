#pragma once
#include "gt2formats/gltf_reader.h"
#include "gt2view/cockpit_fit.h"
#include "gt2view/scene_types.h"
#include "gt2formats/car_texture.h"
#include <map>

namespace gt2view {
inline uint32_t LiveSideMirrorFlags(uint32_t flags) {
    // UVs in vertex colour must bypass native lighting and sphere reflection.
    return (flags & ~(kDebugBackfaces|kCabinLining|kModelOpaque|kModelBlend|kModelReflection|kModelNative))|kMirrorTexture;
}
// Match the shader's alpha-or-teal mirror silhouette exactly. The broader
// window-tint classifier also recognises colours outside this small sphere.
inline bool MirrorPixelCovered(const uint8_t* pixel) {
    const int r=int(pixel[0])-8,g=int(pixel[1])-16,b=int(pixel[2])-16;
    return pixel[3]>=128 || r*r+g*g+b*b<=40;
}
inline bool NeutralMirrorImage(const gt2::PngImage& image) {
    size_t neutral=0,visible=0;
    for(size_t i=0;i<image.rgba.size();i+=4){const auto* c=&image.rgba[i];if(c[3]<128)continue;++visible;
        const int high=std::max({c[0],c[1],c[2]}),low=std::min({c[0],c[1],c[2]});if(high-low<=24)++neutral;}
    return visible && neutral*100>=visible*85;
}
inline bool RearFacingMirrorTriangle(const std::array<float,3>& a,const std::array<float,3>& b,const std::array<float,3>& c) {
    const float x=(b[1]-a[1])*(c[2]-a[2])-(b[2]-a[2])*(c[1]-a[1]);
    const float y=(b[2]-a[2])*(c[0]-a[0])-(b[0]-a[0])*(c[2]-a[2]);
    const float z=(b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]);
    const float length=std::sqrt(x*x+y*y+z*z);
    return std::abs(z)>length*.55f && std::abs(y)<length*.5f;
}
// Recognise a small, rear-facing glass patch outside the passenger cell.
// The caller also excludes wheel/reflection draws.
// Keeping the test on an entire draw prevents window/body fragments from
// becoming mirrors merely because one triangle happens to face rearward.
inline bool IsSideMirrorGlass(const gt2::GltfPrimitive& primitive,const gt2::PngImage& image,
                              float scale,const std::array<float,3>& offset,const CockpitFit& fit,bool clampU=false,bool clampV=false) {
    if(primitive.indices.empty() || image.rgba.empty())return false;
    std::array<std::array<float,3>,2> lo{{{1e9f,1e9f,1e9f},{1e9f,1e9f,1e9f}}},hi{{{-1e9f,-1e9f,-1e9f},{-1e9f,-1e9f,-1e9f}}};
    std::array<size_t,2> counts{};
    double area=0,rearArea=0;
    double glassNormalZ=0;size_t glassNormals=0;
    std::array<double,3> rearNormal{};
    for(size_t i=0;i+2<primitive.indices.size();i+=3){
        std::array<std::array<float,3>,3> p;
        for(size_t j=0;j<3;++j){const auto& v=primitive.vertices.at(primitive.indices[i+j]);
            for(size_t k=0;k<3;++k)p[j][k]=v.position[k]*scale+offset[k];
            const size_t side=p[j][0]<0?0:1;++counts[side];
            for(size_t k=0;k<3;++k){lo[side][k]=std::min(lo[side][k],p[j][k]);hi[side][k]=std::max(hi[side][k],p[j][k]);}
        }
        // A strip spanning both sides is a dashboard/windscreen panel.
        // Per-side vertex bounds alone make its two outer edges look like a
        // pair of small mirrors while its triangles cover the whole bonnet.
        if(std::min({p[0][0],p[1][0],p[2][0]})<0 &&
           std::max({p[0][0],p[1][0],p[2][0]})>0)return false;
        std::array<float,3> a,b;for(size_t k=0;k<3;++k){a[k]=p[1][k]-p[0][k];b[k]=p[2][k]-p[0][k];}
        const std::array<float,3> n{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]};
        const float length=std::sqrt(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]);area+=length;
        if(RearFacingMirrorTriangle(p[0],p[1],p[2])){
            if(primitive.hasNormals)for(size_t j=0;j<3;++j){glassNormalZ+=primitive.vertices.at(primitive.indices[i+j]).normal[2];++glassNormals;}
            rearArea+=length;const float facing=n[2]<0?-1.f:1.f,side=p[0][0]<0?-1.f:1.f;
            rearNormal[0]+=n[0]*facing*side;rearNormal[1]+=n[1]*facing;rearNormal[2]+=n[2]*facing;
        }
    }
    const double coherent=std::sqrt(rearNormal[0]*rearNormal[0]+rearNormal[1]*rearNormal[1]+rearNormal[2]*rearNormal[2]);
    if(area<.002 || rearArea<area*.20 || coherent<rearArea*.95)return false;
    if(glassNormals && glassNormalZ<-.25*double(glassNormals))return false;
    for(size_t side=0;side<2;++side)if(counts[side]){
        const auto& a=lo[side];const auto& b=hi[side];
        const float inner=side?a[0]:-b[0],outer=side?b[0]:-a[0];
        if(inner<fit.halfWidth*.65f || outer>fit.halfWidth+.18f || b[0]-a[0]>.36f || b[1]-a[1]>.25f || b[2]-a[2]>.40f ||
           b[1]-a[1]<.025f || a[1]<fit.windshieldY-.18f || b[1]>fit.windshieldY+.32f ||
           a[2]<fit.windshieldZ-.85f || b[2]>fit.eye[2]+.35f)return false;
    }
    if(primitive.hasUvs) {
        size_t covered=0,samples=0;
        for(size_t i=0;i+2<primitive.indices.size();i+=3){
            const auto& a=primitive.vertices.at(primitive.indices[i]);const auto& b=primitive.vertices.at(primitive.indices[i+1]);const auto& c=primitive.vertices.at(primitive.indices[i+2]);
            if(!RearFacingMirrorTriangle(a.position,b.position,c.position))continue;
            for(const auto& weights:std::array<std::array<float,3>,4>{{{.34f,.33f,.33f},{.6f,.2f,.2f},{.2f,.6f,.2f},{.2f,.2f,.6f}}}) {
                float u=a.uv[0]*weights[0]+b.uv[0]*weights[1]+c.uv[0]*weights[2],v=a.uv[1]*weights[0]+b.uv[1]*weights[1]+c.uv[1]*weights[2];
                if(!clampU)u-=std::floor(u);if(!clampV)v-=std::floor(v);
                const int x=std::clamp(int(u*image.width),0,image.width-1),y=std::clamp(int(v*image.height),0,image.height-1);
                const auto* pixel=&image.rgba[(size_t(y)*size_t(image.width)+size_t(x))*4];++samples;
                if(MirrorPixelCovered(pixel))++covered;
            }
        }
        if(!samples || covered*4<samples)return false;
    }
    // A mirror's baked reflection is neutral/dark; painted housings are not.
    return NeutralMirrorImage(image);
}

// A GT4 draw can batch disconnected handles and both mirror shells using one
// texture. Test complete connected surfaces, never isolated convenient triangles.
inline std::vector<gt2::GltfPrimitive> SideMirrorPatches(const gt2::GltfPrimitive& p,const gt2::PngImage& image,
        float scale,const std::array<float,3>& offset,const CockpitFit& fit,bool clampU=false,bool clampV=false) {
    if(IsSideMirrorGlass(p,image,scale,offset,fit,clampU,clampV))return {p};
    const size_t count=p.indices.size()/3;
    std::vector<size_t> parent(count);for(size_t i=0;i<count;++i)parent[i]=i;
    auto root=[&](size_t i){while(parent[i]!=i){parent[i]=parent[parent[i]];i=parent[i];}return i;};
    std::map<std::array<float,3>,size_t> owner;
    for(size_t i=0;i<count;++i)for(size_t j=0;j<3;++j) {
        const auto& point=p.vertices.at(p.indices[i*3+j]).position;
        const auto [it,added]=owner.emplace(point,i);
        if(!added)parent[root(i)]=root(it->second);
    }
    std::map<size_t,std::vector<uint32_t>> groups;
    for(size_t i=0;i<count;++i)for(size_t j=0;j<3;++j)groups[root(i)].push_back(p.indices[i*3+j]);
    std::vector<gt2::GltfPrimitive> result;
    if(groups.size()<2)return result;
    for(auto& [_,indices]:groups) {
        auto patch=p;patch.indices=std::move(indices);
        if(IsSideMirrorGlass(patch,image,scale,offset,fit,clampU,clampV))result.push_back(std::move(patch));
    }
    return result;
}

inline void FitMirrorSightlines(CockpitFit& fit,std::span<const SceneVertex> mirrors,bool adjustCowl=true) {
    if(mirrors.empty())return;
    if(!fit.sideSillValid){
        for(size_t side=0;side<2;++side)for(size_t i=0;i<17;++i){
            const float z=fit.frontCowl[side][2]+(fit.rearCowl[side][2]-fit.frontCowl[side][2])*float(i)/16.f;
            fit.sideSill[side][i]=CockpitSideSillAt(fit,side,z);
        }
        fit.sideSillValid=true;
    }
    auto eye=fit.eye;eye[2]+=.05f;
    for(const auto& mirror:mirrors){
        // Bonnet-mounted mirrors can be seen through an outer windshield
        // corner, before the ray reaches a side door. Fit that corner too.
        for(size_t i=1;adjustCowl && i<fit.cowl.size();++i){
            auto& a=fit.cowl[i-1];auto& b=fit.cowl[i];
            const float dx=b[0]-a[0],dz=b[2]-a[2];
            const float rx=mirror.pos[0]-eye[0],rz=mirror.pos[2]-eye[2];
            const float denominator=rx*dz-rz*dx;
            if(std::abs(denominator)<1e-6f)continue;
            const float t=((a[0]-eye[0])*dz-(a[2]-eye[2])*dx)/denominator;
            const float u=((a[0]-eye[0])*rz-(a[2]-eye[2])*rx)/denominator;
            if(t<=0 || t>=1 || u<0 || u>1)continue;
            const float y=eye[1]+t*(mirror.pos[1]-eye[1])-.015f;
            a[1]=std::min(a[1],y);b[1]=std::min(b[1],y);
        }
        const size_t side=mirror.pos[0]<0?0:1;const auto& front=fit.frontCowl[side];const auto& rear=fit.rearCowl[side];
        if(std::abs(mirror.pos[0]-eye[0])<.1f)continue;
        float z=mirror.pos[2],t=0;
        for(int step=0;step<4;++step){const auto edge=CockpitSideSillAt(fit,side,z);
            t=(edge[0]-eye[0])/(mirror.pos[0]-eye[0]);z=eye[2]+t*(mirror.pos[2]-eye[2]);}
        if(t<=0 || t>=1 || z<front[2] || z>rear[2])continue;
        const float y=eye[1]+t*(mirror.pos[1]-eye[1])-.015f;
        const float station=(z-front[2])/std::max(.01f,rear[2]-front[2])*16.f;
        const size_t at=std::min(size_t(station),size_t(15));
        // Only generated trim is lowered. The authored body remains the
        // occlusion boundary, so this cannot punch a hole through a door.
        fit.sideSill[side][at][1]=std::min(fit.sideSill[side][at][1],y);
        fit.sideSill[side][at+1][1]=std::min(fit.sideSill[side][at+1][1],y);
    }
    if(adjustCowl)fit.frontCowl={fit.cowl.front(),fit.cowl.back()};
}

inline std::vector<SceneVertex> StockSideMirrorGlass(const gt2::CarModel& model,const gt2::CarTexture& texture,const CockpitFit& fit) {
    gt2::CarMeshOptions options;options.wheels=false;
    const auto mesh=gt2::BuildCarMesh(model,options);
    std::vector<SceneVertex> result;
    size_t first=0;
    for(const auto& polygon:model.lods.at(0).polygons){
        const size_t count=polygon.IsQuad()?6:3;
        gt2::GltfPrimitive p;int u0=255,v0=223,u1=0,v1=0;
        for(size_t i=0;i<count;++i){const auto& source=mesh.at(first+i);gt2::GltfVertex v;
            std::copy_n(source.pos,3,v.position.begin());p.vertices.push_back(v);p.indices.push_back(uint32_t(i));
            u0=std::min(u0,int(source.texel[0]));u1=std::max(u1,int(source.texel[0]));v0=std::min(v0,int(source.texel[1]));v1=std::max(v1,int(source.texel[1]));}
        first+=count;
        if(!polygon.IsTextured() || texture.paints.empty())continue;
        const gt2::PngImage neutral{1,1,{128,128,128,255}};
        if(!IsSideMirrorGlass(p,neutral,1,{0,0,0},fit))continue;
        gt2::PngImage patch;patch.width=std::max(1,u1-u0+1);patch.height=std::max(1,v1-v0+1);
        for(int y=v0;y<=v1;++y)for(int x=u0;x<=u1;++x){
            const size_t at=size_t(std::clamp(y,0,223))*256+size_t(std::clamp(x,0,255));
            const auto index=texture.indices.at(at);const auto color=texture.paints[0].cluts[polygon.palette][index];
            for(int shift:{0,5,10})patch.rgba.push_back(uint8_t(((color>>shift)&31)*255/31));patch.rgba.push_back(color?255:0);
        }
        if(!IsSideMirrorGlass(p,patch,1,{0,0,0},fit))continue;
        // A body-colour polygon is not glass, even if its first paint is grey.
        bool fixed=true;
        for(const auto& paint:texture.paints)for(int y=v0;y<=v1 && fixed;++y)for(int x=u0;x<=u1;++x){
            const auto index=texture.indices.at(size_t(std::clamp(y,0,223))*256+size_t(std::clamp(x,0,255)));
            if(paint.cluts[polygon.palette][index]!=texture.paints[0].cluts[polygon.palette][index]){fixed=false;break;}}
        if(!fixed)continue;
        if(std::getenv("GT2_DEBUG_SIDE_MIRRORS"))std::printf("Stock mirror candidate: first %zu eye %.3f %.3f %.3f\n",first-count,fit.eye[0],fit.eye[1],fit.eye[2]);
        float xmin=1e9f,xmax=-1e9f,ymin=1e9f,ymax=-1e9f;
        for(const auto& v:p.vertices){xmin=std::min(xmin,v.position[0]);xmax=std::max(xmax,v.position[0]);ymin=std::min(ymin,v.position[1]);ymax=std::max(ymax,v.position[1]);}
        if(std::getenv("GT2_DEBUG_SIDE_MIRRORS"))std::printf("  patch x %.3f %.3f y %.3f %.3f z %.3f\n",xmin,xmax,ymin,ymax,p.vertices[0].position[2]);
        for(size_t i=0;i+2<p.vertices.size();i+=3) {
            if(!RearFacingMirrorTriangle(p.vertices[i].position,p.vertices[i+1].position,p.vertices[i+2].position))continue;
            for(size_t j=0;j<3;++j){const auto& v=p.vertices[i+j];SceneVertex out{};std::copy(v.position.begin(),v.position.end(),out.pos);out.pos[2]+=.003f;
                out.texel[0]=(out.pos[0]-xmin)/std::max(.001f,xmax-xmin);out.texel[1]=1-(out.pos[1]-ymin)/std::max(.001f,ymax-ymin);out.flags=kMirrorTexture;result.push_back(out);}
        }
    }
    return result;
}
}
