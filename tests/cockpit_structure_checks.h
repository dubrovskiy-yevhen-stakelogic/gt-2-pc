#pragma once
#include "gt2view/cockpit_body.h"
template<class Check> void CockpitStructureChecks(Check check){
    using namespace gt2view;
    gt2::CarModel model;model.lods.resize(1);auto& lod=model.lods[0];lod.scale=16;
    auto vertex=[](float x,float y,float z){return gt2::CarVertex{int16_t(std::lround(x*4096)),int16_t(std::lround(y*4096)),int16_t(std::lround(z*4096)),0};};
    lod.vertices={vertex(.8f,.6f,.05f),vertex(.8f,.8f,.05f),vertex(1.f,.8f,.05f),vertex(1.f,.6f,.05f),
                  vertex(.799f,.3f,-.2f),vertex(.799f,.65f,-.2f),vertex(.799f,.65f,.3f),vertex(.799f,.3f,.3f)};
    gt2::CarPolygon mirror;mirror.primCode=0x2d;mirror.vertex={0,1,2,3};mirror.u={0,0,31,31};mirror.v={0,31,31,0};
    gt2::CarPolygon body;body.primCode=0x28;body.vertex={4,5,6,7};lod.polygons={mirror,body};
    gt2::CarTexture texture;texture.paints.resize(2);texture.indices.assign(256*224,1);
    for(auto& paint:texture.paints)paint.cluts[0][1]=uint16_t(4|(4<<5)|(4<<10));
    CockpitFit fit;fit.valid=true;fit.halfWidth=.8f;fit.windshieldY=.65f;fit.windshieldZ=-.4f;fit.eye={-.3f,.9f,.4f};
    check(cockpit_structure::MirrorSupports(model,texture,fit).empty(),"intact authored mirror stalk needs no additional support");
    for(int y=0;y<32;++y)for(int x=3;x<9;++x)texture.indices[size_t(y)*256+size_t(x)]=0;
    const auto supports=cockpit_structure::MirrorSupports(model,texture,fit);
    check(supports.size()==1,"a texture alpha break in a body-seated mirror stalk is detected");
    check(cockpit_structure::Length(cockpit_structure::Sub(supports[0].tip,supports[0].root))<.12f && supports[0].radius<=.008f,
        "mirror repair remains a short narrow stalk, not a body-spanning wedge");
    std::vector<gt2::CarMeshVertex> mountMesh;cockpit_structure::AppendSupport(supports[0],mountMesh);
    std::map<std::pair<cockpit_structure::Point,cockpit_structure::Point>,size_t> edges;
    for(size_t i=0;i<mountMesh.size();i+=3)for(size_t j=0;j<3;++j){auto a=cockpit_structure::Position(mountMesh[i+j]),b=cockpit_structure::Position(mountMesh[i+(j+1)%3]);
        if(b<a)std::swap(a,b);++edges[{a,b}];}
    check(std::all_of(edges.begin(),edges.end(),[](const auto& e){return e.second==2;}),
          "mirror support is watertight at its root and housing ends, including grazing views");
    const auto saved=lod.vertices;
    for(size_t i=4;i<8;++i)lod.vertices[i].x-=2048;
    check(cockpit_structure::MirrorSupports(model,texture,fit).empty(),"a floating accessory far from the body cannot acquire an invented mount");
    lod.vertices=saved;
    for(auto& paint:texture.paints)paint.cluts[0][1]=31;
    check(cockpit_structure::MirrorSupports(model,texture,fit).empty(),"a coloured exterior accessory is not a mirror");
    for(auto& paint:texture.paints)paint.cluts[0][1]=uint16_t(4|(4<<5)|(4<<10));
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    check(cockpit_structure::MirrorSupports(model,texture,fit).empty(),"an entirely transparent surface cannot anchor the other end of a stalk");
    gt2::CarModel roofModel;roofModel.lods.resize(1);auto& roofLod=roofModel.lods[0];roofLod.scale=16;
    roofLod.vertices={vertex(-.8f,1.2f,0),vertex(.8f,1.2f,0),vertex(.8f,1.2f,1.2f),vertex(-.8f,1.2f,1.2f)};
    roofLod.polygons={mirror};roofLod.polygons[0].u={0,31,31,0};roofLod.polygons[0].v={0,0,31,31};
    fit.eye={0,.9f,.4f};fit.roofFrontZ=0;fit.roofRearZ=1.2f;fit.roofFrontY=fit.roofRearY=fit.roofY=1.2f;
    fit.rearWindowZ=1.4f;fit.rearWindowY=.65f;fit.frontCowl={{{-.8f,.65f,-.4f},{.8f,.65f,-.4f}}};
    fit.rearCowl={{{-.8f,.65f,1.4f},{.8f,.65f,1.4f}}};
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(1));
    for(int y=8;y<24;++y)for(int x=8;x<24;++x)texture.indices[size_t(y)*256+size_t(x)]=0;
    const auto roof=BuildCockpitBody(roofModel,texture,fit);
    check(roof.windowTriangles==0 && roof.vertices.size()==6 &&
          std::all_of(roof.vertices.begin(),roof.vertices.end(),[](const auto& v){return !v.textured;}),
          "livery alpha on an authored roof cannot cut holes through its inner lining");
    roofLod.vertices[0].y=roofLod.vertices[1].y=int16_t(.65f*4096);
    check(BuildCockpitBody(roofModel,texture,fit).windowTriangles>0,
          "sloping glazing keeps its opening instead of becoming an opaque roof");
    const cockpit_structure::Point pillarRoot{.8f,.65f,-.4f},pillarTop{.7f,1.2f,0};
    check(cockpit_detail::CabinPillarEdge(pillarRoot,pillarTop,fit),"authored front pillar remains inside the passenger aperture");
    auto shiftedRoot=pillarRoot,shiftedTop=pillarTop;shiftedRoot[2]-=1.4f;shiftedTop[2]-=1.4f;
    check(!cockpit_detail::CabinPillarEdge(shiftedRoot,shiftedTop,fit),"front lamp edges cannot become cabin pillars even with glazing colours");
    shiftedRoot[2]+=3.5f;shiftedTop[2]+=3.5f;
    check(!cockpit_detail::CabinPillarEdge(shiftedRoot,shiftedTop,fit),"rear lamp edges cannot become cabin pillars");
    shiftedRoot=pillarRoot;shiftedTop=pillarTop;shiftedRoot[1]-=.4f;shiftedTop[1]-=.4f;
    check(!cockpit_detail::CabinPillarEdge(shiftedRoot,shiftedTop,fit),"low body trim cannot become a window pillar");
}
