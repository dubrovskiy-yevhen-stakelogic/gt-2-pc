#pragma once
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace gt2view {
inline constexpr uint32_t kPaintEnvironmentSize=128,kPaintEnvironmentMips=8;
inline constexpr uint32_t kNativeEnvironmentWidth=64,kNativeEnvironmentHeight=128;
// GT4's three asymmetric environment cameras, at half the original dimensions.
inline std::array<std::array<float,16>,3> NativeEnvironmentCameras(const std::array<float,3>& eye) {
    const auto mul=[](const auto& a,const auto& b) {
        std::array<float,9> r{};for(size_t y=0;y<3;++y)for(size_t x=0;x<3;++x)for(size_t k=0;k<3;++k)r[y*3+x]+=a[y*3+k]*b[k*3+x];return r;
    };
    const float d=3.14159265358979323846f/180.f,c15=std::cos(15.f*d),s15=std::sin(15.f*d);
    // Fixed main-camera rows are right +X, up +Y, forward -Z.
    // Their determinant is -1, as in GT4; identity rows reverse GT2 winding.
    const std::array<float,9> basis{-1,0,0,0,c15,-s15,0,s15,c15};
    std::array<std::array<float,16>,3> result{};
    for(size_t i=0;i<3;++i) {
        const float a=-79.975f*d,b=-120.f*float(i)*d;
        const std::array<float,9> rx{1,0,0,0,std::cos(a),-std::sin(a),0,std::sin(a),std::cos(a)};
        const std::array<float,9> rz{std::cos(b),-std::sin(b),0,std::sin(b),std::cos(b),0,0,0,1};
        const auto v=mul(mul(rx,rz),basis);auto& m=result[i];
        for(size_t k=0;k<3;++k) {
            m[4*k]=.31980f*v[k];m[4*k+1]=-(.19285f*v[3+k]-.09088f*v[6+k]);
            m[4*k+2]=-.1599f/(200.f-.1599f)*v[6+k];m[4*k+3]=v[6+k];
            m[12]-=m[4*k]*eye[k];m[13]-=m[4*k+1]*eye[k];m[14]-=m[4*k+2]*eye[k];m[15]-=m[4*k+3]*eye[k];
        }m[14]+=.1599f*200.f/(200.f-.1599f);
    }return result;
}
// Vulkan cube order and orientation. The same axes build the capture cameras.
inline std::array<float,3> PaintCubeDirection(size_t face,float u,float v) {
    const std::array<std::array<float,3>,6> d{{{1,-v,-u},{-1,-v,u},{u,1,v},{u,-1,-v},{u,-v,1},{-u,-v,-1}}};
    auto p=d.at(face);const float length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
    for(auto& x:p)x/=length;return p;
}
inline std::vector<uint32_t> PaintStudioCube() {
    std::vector<uint32_t> pixels(6*kPaintEnvironmentSize*kPaintEnvironmentSize);
    const auto lightA=PaintCubeDirection(5,-.2f,-.55f),lightB=PaintCubeDirection(0,.9f,-.6f);
    for(size_t face=0;face<6;++face)for(uint32_t y=0;y<kPaintEnvironmentSize;++y)for(uint32_t x=0;x<kPaintEnvironmentSize;++x) {
        const auto n=PaintCubeDirection(face,2*(x+.5f)/kPaintEnvironmentSize-1,2*(y+.5f)/kPaintEnvironmentSize-1);
        const float sky=std::clamp(n[1]*.65f+.45f,0.f,1.f);
        const auto dot=[&](const auto& a){return std::max(0.f,n[0]*a[0]+n[1]*a[1]+n[2]*a[2]);};
        // Long rectangular studio lights give broad, continuous panel bands.
        // A camera orbit traverses the same fixed environment, with no per-frame
        // render target or texture upload.
        const float front=std::max(.08f,std::abs(n[2])),side=std::max(.08f,std::abs(n[0]));
        const float u=n[0]/front,v=n[1]/front;
        const float su=n[2]/side,sv=n[1]/side;
        const float stripA=(n[2]<0?1.f:0.f)*1.5f*std::exp(-((u+.55f)*(u+.55f)/.065f+(v-.25f)*(v-.25f)/1.8f));
        const float stripB=(n[0]>0?1.f:0.f)*1.1f*std::exp(-(su*su/1.6f+(sv-.6f)*(sv-.6f)/.055f));
        const float softbox=stripA+stripB+.6f*std::pow(dot(lightA),65.f)+.4f*std::pow(dot(lightB),42.f);
        uint32_t rgba=0xff000000u;
        for(uint32_t c=0;c<3;++c) {
            const float ground[]{.018f,.018f,.02f},ceiling[]{.12f,.125f,.135f};
            const float radiance=ground[c]+sky*(ceiling[c]-ground[c])+softbox;
            // Square-root encoding keeps bright light detail in an RGBA8 cube.
            rgba|=uint32_t(std::lround(255*std::sqrt(std::clamp(radiance/3.f,0.f,1.f))))<<(8*c);
        }
        pixels[(face*kPaintEnvironmentSize+y)*kPaintEnvironmentSize+x]=rgba;
    }
    return pixels;
}
inline std::array<float,16> PaintCubeCamera(size_t face,const std::array<float,3>& eye) {
    const auto f=PaintCubeDirection(face,0,0),r0=PaintCubeDirection(face,1,0),d0=PaintCubeDirection(face,0,1);
    std::array<float,3> right{},down{};
    for(size_t k=0;k<3;++k){right[k]=r0[k]*std::sqrt(2.f)-f[k];down[k]=d0[k]*std::sqrt(2.f)-f[k];}
    std::array<float,16> matrix{};
    for(size_t k=0;k<3;++k){matrix[4*k]=right[k];matrix[4*k+1]=down[k];matrix[4*k+3]=f[k];matrix[12]-=right[k]*eye[k];matrix[13]-=down[k]*eye[k];matrix[15]-=f[k]*eye[k];}
    matrix[14]=.15f;return matrix;
}
}
