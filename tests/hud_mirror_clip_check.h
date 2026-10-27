#pragma once
#include "gt2export/png_writer.h"
#include <filesystem>
// Imported cars in the hidden-car HUD mirror must clip in its own plane.
inline size_t CheckHudMirrorClip(gt2view::VkSceneRenderer& renderer,uint32_t size,const std::filesystem::path& out) {
    using namespace gt2view;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    StereoViews views;for(int e=0;e<2;++e){std::copy_n(identity,16,views.worldVP[e]);std::copy_n(identity,16,views.hudVP[e]);}
    renderer.SetStereoViews(views);
    std::vector<SceneVertex> quad;
    for(const auto& p:std::array<std::array<float,2>,6>{{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}}}) {
        SceneVertex v{};v.pos[0]=p[0];v.pos[1]=p[1];v.pos[2]=.5f;v.color[0]=1;quad.push_back(v);
    }
    renderer.SetVertices(SceneRenderer::kModelSetVertexBase,quad);
    DrawItem item;item.firstVertex=SceneRenderer::kModelSetVertexBase;item.vertexCount=6;item.space=kSpaceScreen;
    std::copy_n(identity,16,item.mvp);item.scissor[0]=.25f;item.scissor[1]=.125f;item.scissor[2]=.75f;item.scissor[3]=.25f;
    renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=0;
    const auto clipped=renderer.CaptureStereoForAudit({item},1);
    auto outside=[&](const auto& image){size_t n=0;for(size_t e=0;e<2;++e)for(uint32_t y=0;y<size;++y)for(uint32_t x=0;x<size;++x)
        if(x<size/4 || x>=size*3/4 || y<size/8 || y>=size/4)n+=image[((e*size+y)*size+x)*4]>200;return n;};
    size_t inside=0;for(size_t e=0;e<2;++e)for(uint32_t y=size/8;y<size/4;++y)for(uint32_t x=size/4;x<size*3/4;++x)
        inside+=clipped[((e*size+y)*size+x)*4]>200;
    if(inside!=size_t(size)*size/8)throw std::runtime_error("mirror clip erased pixels inside its rectangle");
    const size_t leaks=outside(clipped);
    std::fill_n(item.scissor,4,0.f);const auto broken=renderer.CaptureStereoForAudit({item},1);
    if(outside(broken)<size*size)throw std::runtime_error("mirror clipping negative control did not escape");
    gt2::WritePngRgba((out/"hud-mirror-clip.png").string(),size,size*2,clipped);
    std::cout<<"HUD mirror outside pixels: "<<leaks<<'\n';return leaks;
}
