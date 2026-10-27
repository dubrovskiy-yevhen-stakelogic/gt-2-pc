#pragma once
#include "gt2view/side_mirror_fit.h"
template<class Check> void SideMirrorChecks(Check check) {
    gt2view::CockpitFit fit;fit.halfWidth=.9f;fit.windshieldY=.8f;fit.windshieldZ=-.5f;fit.eye[2]=.2f;
    gt2::GltfPrimitive glass;
    for(const auto& p:std::array<std::array<float,3>,4>{{{.76f,.78f,-.2f},{.96f,.78f,-.2f},{.96f,.9f,-.2f},{.76f,.9f,-.2f}}}){gt2::GltfVertex v;v.position=p;glass.vertices.push_back(v);}
    glass.indices={0,1,2,0,2,3};
    const gt2::PngImage neutral{1,1,{24,32,32,255}},painted{1,1,{220,30,30,255}};
    const uint8_t mirrorTeal[]{8,16,16,0},transparentRim[]{8,24,20,0},opaqueFrame[]{0,0,0,255};
    check(gt2view::MirrorPixelCovered(mirrorTeal) && !gt2view::MirrorPixelCovered(transparentRim) &&
          gt2view::MirrorPixelCovered(opaqueFrame),"CPU mirror clipping agrees with the shader silhouette");
    auto detect=[&](const auto& p,const auto& texture){return gt2view::IsSideMirrorGlass(p,texture,1,{0,0,0},fit);};
    check(detect(glass,neutral),"rear-facing side glass is recognised without per-car profiles");
    check(!detect(glass,painted),"painted mirror housing is not reflective glass");
    auto window=glass;for(auto& v:window.vertices)v.position[1]+=(v.position[1]-.78f)*3;
    check(!detect(window,neutral),"full-height side window is not a mirror");
    auto roof=glass;for(auto& v:roof.vertices)std::swap(v.position[1],v.position[2]);
    check(!detect(roof,neutral),"horizontal body panels are not mirrors");
    auto inboard=glass;for(auto& v:inboard.vertices)v.position[0]-=.4f;
    check(!detect(inboard,neutral),"dashboard patches are not side mirrors");
    auto acrossCabin=glass;
    acrossCabin.vertices[0].position[0]=-.76f;acrossCabin.vertices[3].position[0]=-.96f;
    check(!detect(acrossCabin,neutral),"a cross-cabin strip is not two mirrors merely because its vertices are outboard");
    auto left=glass;for(auto& v:left.vertices)v.position[0]*=-1;
    check(detect(left,neutral),"left mirror supports reversed authored winding");
    auto fender=glass;for(auto& v:fender.vertices)v.position[2]-=.9f;
    check(detect(fender,neutral),"bonnet-mounted side mirrors are recognised ahead of the windshield");
    auto dashboardSpeaker=glass;for(auto& v:dashboardSpeaker.vertices)v.position[0]-=.24f;
    auto wideBody=fit;wideBody.halfWidth=1.1f;
    check(!gt2view::IsSideMirrorGlass(dashboardSpeaker,neutral,1,{0,0,0},wideBody),
          "small neutral dashboard speakers are not mirrors on wide sports cars");
    auto headlamp=glass;headlamp.hasNormals=true;for(auto& v:headlamp.vertices)v.normal={0,0,-1};
    check(!detect(headlamp,neutral),"forward-facing neutral headlamps are not mirrors");
    gt2::PngImage mask{4,4,std::vector<uint8_t>(64)};mask.rgba[0]=mask.rgba[1]=mask.rgba[2]=32;mask.rgba[3]=255;
    auto emptyUv=glass;emptyUv.hasUvs=true;for(auto& v:emptyUv.vertices)v.uv={.5f,.5f};
    check(!detect(emptyUv,mask),"opaque texels outside the drawn UV patch cannot create a phantom mirror");
    auto batched=glass;
    for(auto v:inboard.vertices){v.position[2]+=.8f;batched.vertices.push_back(v);}
    for(auto index:inboard.indices)batched.indices.push_back(index+4);
    check(!detect(batched,neutral),"mixed draw is not treated as one mirror");
    const auto patches=gt2view::SideMirrorPatches(batched,neutral,1,{0,0,0},fit);
    check(patches.size()==1 && patches.front().indices.size()==glass.indices.size(),
          "disconnected mirror glass survives batching with an unrelated interior panel");
    check(gt2view::SideMirrorPatches(batched,painted,1,{0,0,0},fit).empty(),
          "component splitting cannot turn painted body panels into mirrors");

}
