#pragma once
#include "gt2view/scene_types.h"
template<class Check> void BackdropOrderChecks(Check check) {
    using namespace gt2view;
    DrawItem sky,road,mask,glass,shadow;
    sky.space=kSpaceSky;sky.firstVertex=1;sky.vertexCount=3;
    road.space=kSpaceWorld;road.firstVertex=2;road.vertexCount=3;
    mask=road;mask.firstVertex=3;mask.stpPass=1;
    glass=road;glass.firstVertex=4;glass.blend=kBlendAlpha;
    shadow=road;shadow.firstVertex=5;shadow.blend=1;
    std::vector<DrawItem> items{sky,road,mask,shadow,glass};
    check(OpaqueDrawsAfterBackdrop(items)==2,"sky-first negative control exposes redundant opaque overdraw");
    DeferOpaqueBackdrop(items);
    check(OpaqueDrawsAfterBackdrop(items)==0,"sky cannot precede depth-writing world draws");
    for(size_t i=0;i<items.size();++i)check(items[i].firstVertex==std::array<uint32_t,5>{2,3,1,5,4}[i],"opaque world precedes sky and all transparency retains its order");
    items={sky,road,glass};items[1].clearDepth=1;DeferOpaqueBackdrop(items);
    check(items[0].firstVertex==1 && items[1].firstVertex==2,"sky optimization cannot cross a mirror depth clear");
}
