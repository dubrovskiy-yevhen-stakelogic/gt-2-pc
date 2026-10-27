#pragma once
#include "gt2view/side_mirror_fit.h"
#include "gt2export/png_writer.h"
#include <filesystem>

// A patterned rear image detects UV damage that uniform red/green witnesses miss.
inline void CheckMirrorCopy(gt2view::VkSceneRenderer& renderer, uint32_t size,
                            const std::filesystem::path& output) {
    using namespace gt2view;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    StereoViews views;
    for(int eye=0;eye<2;++eye)std::copy_n(identity,16,views.worldVP[eye]);
    renderer.SetStereoViews(views);
    std::vector<SceneVertex> source,central,side;
    constexpr std::array<std::array<float,2>,6> corners{{{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}}};
    constexpr float colors[4][3]={{1,0,0},{0,1,0},{0,0,1},{1,1,0}};
    for(int y=0;y<2;++y)for(int x=0;x<2;++x)for(const auto& uv:corners){
        SceneVertex v{};v.pos[0]=-1+x+uv[0];v.pos[1]=-1+y+uv[1];v.pos[2]=.5f;
        std::copy_n(colors[y*2+x],3,v.color);source.push_back(v);
    }
    for(const auto& uv:corners){
        SceneVertex v{};v.pos[0]=-.8f+1.6f*uv[0];v.pos[1]=-.8f+1.6f*uv[1];v.pos[2]=.5f;
        v.texel[0]=uv[0];v.texel[1]=uv[1];v.flags=kMirrorTexture;central.push_back(v);
        v.texel[0]=v.texel[1]=.5f;v.color[0]=uv[0];v.color[1]=uv[1];
        v.clut=1|(1u<<16);v.flags=LiveSideMirrorFlags(kTextured|kExternalTexture|kModelNative);
        side.push_back(v);
    }
    renderer.BeginModelTextures();const uint32_t white=0xffffffffu;renderer.UploadExternalTexture(0,1,&white);
    renderer.SetVertices(0,source);renderer.SetVertices(64,central);
    std::vector<NativeModelVertex> attrs(6);for(auto& a:attrs){a.normal={0,1,0};a.materialAlpha=16384u<<16;}
    renderer.SetNativeModelVertices(0,attrs);
    NativeModelMaterial material{};material.values[0]=material.values[1]=material.values[2]=1;
    material.values[16]=1;material.values[18]=-1;material.values[23]=1;
    renderer.SetNativeModelMaterials(0,{material});
    auto item=[&](uint32_t first,uint32_t count,uint32_t space){DrawItem d;d.firstVertex=first;d.vertexCount=count;d.space=space;std::copy_n(identity,16,d.mvp);return d;};
    const auto rear=item(0,uint32_t(source.size()),kSpaceMirrorSource);
    const auto reference=renderer.CaptureStereoForAudit({rear,item(64,6,kSpaceWorld)},2);
    auto capture=[&](bool broken){auto vertices=side;if(broken)for(auto& v:vertices)v.flags|=kModelNative;
        renderer.SetVertices(SceneRenderer::kModelSetVertexBase,vertices);
        return renderer.CaptureStereoForAudit({rear,item(SceneRenderer::kModelSetVertexBase,6,kSpaceWorld)},2);};
    const auto fixed=capture(false),broken=capture(true);
    size_t differences=0,negative=0;
    for(size_t eye=0;eye<2;++eye)for(size_t y=size/4;y<size*3/4;++y)for(size_t x=size/4;x<size*3/4;++x){
        const size_t at=((eye*size+y)*size+x)*4;
        for(size_t c=0;c<3;++c){differences+=std::abs(int(reference[at+c])-int(fixed[at+c]))>2;
            negative+=std::abs(int(reference[at+c])-int(broken[at+c]))>2;}
    }
    gt2::WritePngRgba((output/"mirror-central.png").string(),size,size*2,reference);
    gt2::WritePngRgba((output/"mirror-side-copy.png").string(),size,size*2,fixed);
    gt2::WritePngRgba((output/"mirror-old-lighting-error.png").string(),size,size*2,broken);
    std::cout<<"Mirror patterned copy: differences="<<differences<<", old-lighting negative="<<negative<<'\n';
    if(differences || negative<100)throw std::runtime_error("central/side mirror copy contract failed");
}
