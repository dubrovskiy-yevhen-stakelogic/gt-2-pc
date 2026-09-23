#pragma once
#include "gt2view/cockpit_window_cut.h"
#include <cmath>

template<class Check>
void CockpitWindowChecks(Check check) {
    using gt2::CarMeshVertex;
    using namespace gt2view;
    auto vertex=[](float u,float v) {
        CarMeshVertex p{};
        p.pos[0]=u*.1f; p.pos[1]=v*.2f; p.pos[2]=u*.03f+v*.04f;
        p.texel[0]=u; p.texel[1]=v;
        p.color[0]=u/16; p.color[1]=v/16; p.color[2]=.75f;
        p.palette=2; p.textured=true; p.rawTexture=true; p.cullBack=true;
        return p;
    };
    auto area=[](const std::vector<CarMeshVertex>& vertices) {
        float sum=0;
        for (size_t i=0;i+2<vertices.size();i+=3)
            sum+=cockpit_window_detail::Area(std::span(vertices).subspan(i,3));
        return sum;
    };
    auto attributes=[&](const std::vector<CarMeshVertex>& vertices) {
        bool valid=true;
        for (const auto& p:vertices) {
            valid&=std::abs(p.pos[0]-p.texel[0]*.1f)<1e-5f;
            valid&=std::abs(p.pos[1]-p.texel[1]*.2f)<1e-5f;
            valid&=std::abs(p.pos[2]-p.texel[0]*.03f-p.texel[1]*.04f)<1e-5f;
            valid&=std::abs(p.color[0]-p.texel[0]/16)<1e-5f;
            valid&=std::abs(p.color[1]-p.texel[1]/16)<1e-5f;
            valid&=p.palette==2 && p.textured && p.rawTexture && p.cullBack;
        }
        check(valid,"window cut preserves surface coordinates, colour, UV and material flags");
    };
    const auto a=vertex(.5f,.5f),b=vertex(15.5f,.5f),c=vertex(15.5f,15.5f),d=vertex(.5f,15.5f);
    const std::vector<CarMeshVertex> face{a,b,c,a,c,d},alternate{a,b,d,b,c,d};
    gt2::CarTexture texture;
    texture.indices.resize(256*224);
    std::array<uint16_t,16> masks{}; masks[2]=2;
    for (int y=3;y<13;++y) for (int x=3;x<13;++x) texture.indices[size_t(y)*256+size_t(x)]=1;
    auto cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-125.f)<1e-4f,"rectangular glass is removed with the exact retained frame area");
    check(std::abs(area(cut.opening)-100.f)<1e-4f &&
          std::abs(area(cut.opening)+area(cut.frame)-area(face))<1e-4f,
          "removed window surface is retained for fitting with no gap or overlap in the source face");
    attributes(cut.opening);
    attributes(cut.frame);
    auto other=CutCockpitWindows(alternate,texture,masks);
    check(other.openings==1 && std::abs(area(cut.frame)-area(other.frame))<1e-4f,
          "a whole-face window has the same opening for either quad diagonal");

    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    for (int y=3;y<13;++y) for (int x=2;x<14;++x)
        if (x<6 || x>=10) texture.indices[size_t(y)*256+size_t(x)]=1;
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==2 && std::abs(area(cut.frame)-145.f)<1e-4f,"separate glass components preserve the painted pillar between them");
    attributes(cut.frame);
    std::vector<CarMeshVertex> collapsed{vertex(4.5f,4.5f),vertex(4.5f,4.5f),vertex(4.5f,4.5f)};
    cut=CutCockpitWindows(collapsed,texture,masks);
    check(cut.openings==1 && cut.frame.empty(),"a UV point wholly in glass removes the face");
    collapsed[1].texel[0]=12.5f; collapsed[2].texel[0]=12.5f;
    cut=CutCockpitWindows(collapsed,texture,masks);
    check(cut.openings==0 && cut.frame.size()==3,"a collapsed UV line crossing a painted pillar remains intact");

    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    for (int y=3;y<13;++y) for (int x=y;x<13;++x) texture.indices[size_t(y)*256+size_t(x)]=1;
    cut=CutCockpitWindows(face,texture,masks); other=CutCockpitWindows(alternate,texture,masks);
    check(cut.openings==1 && cut.frame.size()<90 && std::abs(area(cut.frame)-area(other.frame))<1e-4f,
          "a stair-step reflection mask becomes one smooth convex opening without a diagonal seam");
    attributes(cut.frame);
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==0 && cut.frame.size()==6,"a face without glass is unchanged");
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(1));
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==1 && cut.frame.empty(),"uniform authored glass needs no artificial perimeter frame");

    texture.paints.resize(1);
    texture.paints[0].cluts[2][0]=uint16_t(1|(1<<5)|(1<<10));
    texture.paints[0].cluts[2][1]=uint16_t(1|(2<<5)|(2<<10));
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    texture.indices[3*256+3]=1;
    cut=CutCockpitWindows(face,texture,masks);
    check(IsCockpitEndWindowFace(face) && cut.openings==1 && cut.frame.empty(),
          "a dominant black windscreen can extend from its small authored teal reflection");
    texture.paints[0].cluts[2][0]=uint16_t(1|(2<<5)|(1<<10));
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==1 && cut.frame.empty(),"invariant dark green windscreen glass uses the same spatial restriction");
    auto trim=face;
    for (auto& p:trim) for (float& component:p.pos) component*=.025f;
    cut=CutCockpitWindows(trim,texture,masks);
    check(!IsCockpitEndWindowFace(trim) && std::abs(area(cut.frame)-224.f)<1e-4f,
          "a teal fleck on a small black trim face does not remove its black surface");
    texture.paints.push_back(texture.paints[0]);
    texture.paints[1].cluts[2][0]=uint16_t(2|(2<<5)|(2<<10));
    cut=CutCockpitWindows(face,texture,masks);
    check(std::abs(area(cut.frame)-224.f)<1e-4f,"paint-dependent dark body colour is never treated as invariant glass");
    texture.paints.resize(1);
    texture.indices[3*256+3]=0;
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==0 && cut.frame.size()==6,"black alone cannot create a new window without a teal seed");
    auto side=face;
    for (auto& p:side) { p.pos[0]=0; p.pos[1]=p.texel[1]*.06f; p.pos[2]=p.texel[0]*.05f; }
    for (int y=3;y<13;++y) for (int x=2;x<14;++x)
        if (x<5 || x>=11) texture.indices[size_t(y)*256+size_t(x)]=1;
    cut=CutCockpitWindows(side,texture,masks);
    check(!IsCockpitEndWindowFace(side) && cut.openings==2 && std::abs(area(cut.frame)-165.f)<1e-4f,
          "dominant near-black side trim and B-pillar do not merge the two side windows");
    cut=CutCockpitWindows(side,texture,masks,true);
    check(cut.openings==1 && cut.frame.empty(),
          "a separate painted frame permits removing the full dark side-glass layer");
    texture.paints[0].cluts[2][2]=uint16_t(20|(20<<5)|(20<<10));
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    for (int y=0;y<16;++y) for (int x=0;x<16;++x)
        texture.indices[size_t(y)*256+size_t(x)]=uint8_t(x<3?2:x>=12?1:0);
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-37.5f)<1e-4f,
          "a mostly black windscreen with a broad teal reflection clears its pane but keeps its bright border");
    for (int y=0;y<16;++y) for (int x=0;x<16;++x)
        texture.indices[size_t(y)*256+size_t(x)]=uint8_t(x<4?2:x>=11?1:0);
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-157.5f)<1e-4f,
          "a mixed header with less than half dark glazing does not extend through its black trim");

    texture.paints[0].cluts[2][0]=uint16_t(1|(1<<5)|(2<<10));
    for (int y=0;y<16;++y) for (int x=0;x<16;++x)
        texture.indices[size_t(y)*256+size_t(x)]=uint8_t(x<1?2:x<6?0:1);
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-7.5f)<1e-4f,
          "a teal-dominated windshield opens its connected dark reflection while keeping its painted edge");
    for (int y=0;y<16;++y) for (int x=0;x<16;++x)
        texture.indices[size_t(y)*256+size_t(x)]=uint8_t(y>=6 && y<9?2:1);
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==2 && std::abs(area(cut.frame)-45.f)<1e-4f,
          "an opaque colored sponsor band crossing real windshield glass is retained");
    for (int y=0;y<16;++y) for (int x=0;x<16;++x)
        texture.indices[size_t(y)*256+size_t(x)]=uint8_t(x>=6 && x<10?0:1);
    cut=CutCockpitWindows(side,texture,masks);
    check(cut.openings==2 && std::abs(area(cut.frame)-60.f)<1e-4f,
          "teal-dominated side windows keep their near-black pillar without a separate backing");
    const std::vector<CarMeshVertex> largeScreen{
        vertex(.5f,.5f),vertex(31.5f,.5f),vertex(31.5f,31.5f),
        vertex(.5f,.5f),vertex(31.5f,31.5f),vertex(.5f,31.5f)};
    for (int y=0;y<32;++y) for (int x=0;x<32;++x)
        texture.indices[size_t(y)*256+size_t(x)]=uint8_t(x<1?2:x<7?0:1);
    cut=CutCockpitWindows(largeScreen,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-15.5f)<1e-4f,
          "an almost entirely glazed windshield opens a small blue shadow but retains its painted border");
    texture.paints[0].cluts[2][0]=uint16_t(2|(2<<5)|(3<<10));
    cut=CutCockpitWindows(largeScreen,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-15.5f)<1e-4f,
          "the next dark blue reflection shade does not leave an opaque wedge in the windshield");
    texture.paints[0].cluts[2][0]=uint16_t(3|(3<<5)|(4<<10));
    cut=CutCockpitWindows(largeScreen,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-201.5f)<1e-4f,
          "brighter blue decoration does not extend the dark glass shadow ramp");
    texture.paints[0].cluts[2][0]=uint16_t(1|(1<<5)|(1<<10));
    cut=CutCockpitWindows(largeScreen,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-201.5f)<1e-4f,
          "the small-blue-shadow rule does not extend through an equally sized neutral black trim strip");

    texture.paints.clear();
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    const std::vector<CarMeshVertex> wide{
        vertex(.5f,.5f),vertex(31.5f,.5f),vertex(31.5f,31.5f),
        vertex(.5f,.5f),vertex(31.5f,31.5f),vertex(.5f,31.5f)};
    for (int y=4;y<22;++y) for (int x=4;x<22;++x) texture.indices[size_t(y)*256+size_t(x)]=1;
    for (int x=25;x<31;++x) texture.indices[5*256+size_t(x)]=1;
    for (int i=0;i<4;++i) texture.indices[size_t(8+i)*256+size_t(26+i)]=1;
    cut=CutCockpitWindows(wide,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-637.f)<1e-4f,
          "tiny detached reflection chains do not perforate the frame beside a large pane");
    for (int y=15;y<18;++y) for (int x=26;x<29;++x) texture.indices[size_t(y)*256+size_t(x)]=1;
    for (int y=23;y<25;++y) for (int x=26;x<28;++x) texture.indices[size_t(y)*256+size_t(x)]=1;
    cut=CutCockpitWindows(wide,texture,masks);
    check(cut.openings==3 && std::abs(area(cut.frame)-624.f)<1e-4f,
          "a separate quarter window and a tiny pane with a solid core retain their painted dividers");
    std::fill(texture.indices.begin(),texture.indices.end(),uint8_t(0));
    texture.indices[5*256+5]=1;
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==1 && std::abs(area(cut.frame)-224.f)<1e-4f,
          "an authored face with a sole one-texel window keeps its opening");
    for (int y=4;y<12;++y) for (int x=4;x<14;++x) texture.indices[size_t(y)*256+size_t(x)]=1;
    for (int x=4;x<7;++x) texture.indices[1*256+size_t(x)]=1;
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==2 && std::abs(area(cut.frame)-142.f)<1e-4f,
          "a small component above the relative cutoff is not silently joined or filled");
    texture.indices.clear();
    cut=CutCockpitWindows(face,texture,masks);
    check(cut.openings==0 && cut.frame.size()==6,"missing texture data preserves the authored face");
}
