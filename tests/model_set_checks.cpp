#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "gt2formats/model_set.h"
#include "game/shell/shared_vr_settings.h"
#include "gt2view/model_texture_atlas.h"
#include "gt2view/gt4_cabin_geometry.h"
#include "gt2view/model_window_cut.h"
#include "gt2view/model_glazing.h"
#include "gt2view/native_cockpit_fit.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <source_location>

void Check(bool condition,const std::source_location where=std::source_location::current()) {
    if (!condition) throw std::runtime_error("model set check failed at line "+std::to_string(where.line()));
}
template<class F> void Reject(F call) {
    bool rejected=false;try { call(); } catch (const std::exception&) { rejected=true; }
    Check(rejected);
}
int main() {try {
    {
        gt2::GltfMaterial spokes;spokes.nativeGt4Type=1;spokes.nativeAlphaFunc=6;spokes.nativeAlphaRef=127;
        spokes.nativeAlphaFail=3;
        Check(!gt2view::NativeAlphaAcceptsZero(spokes) && gt2view::NativeAlphaBlendsZero(spokes));
        spokes.nativeAlphaFail=1;Check(gt2view::NativeAlphaBlendsZero(spokes));
        spokes.nativeAlphaFail=0;Check(!gt2view::NativeAlphaBlendsZero(spokes));
        spokes.nativeAlphaFail=2;Check(!gt2view::NativeAlphaBlendsZero(spokes));
        spokes.nativeAlphaRef=32;spokes.nativeAlphaFail=0;
        Check(gt2view::NativeTextureAlphaDiscard(spokes,0));
        Check(gt2view::NativeTextureAlphaDiscard(spokes,64));
        Check(!gt2view::NativeTextureAlphaDiscard(spokes,65));
        spokes.nativeAlphaFail=3;Check(!gt2view::NativeTextureAlphaDiscard(spokes,64));
        spokes.nativeAlphaFail=0;spokes.nativeTextureFunction=1;
        Check(!gt2view::NativeTextureAlphaDiscard(spokes,64));
    }
    {
        const uint8_t gray[]{88,88,88,90},frame[]{16,16,16,254},paint[]{98,20,0,160},clear[]{146,146,146,0};
        Check(gt2view::ModelNeutralGlassPixel(gray));
        const uint8_t darkLamp[]{28,11,11,198};
        Check(!gt2view::ModelNeutralGlassPixel(darkLamp) && !gt2view::ModelBlendGlassPixel(darkLamp));
        const uint8_t tealGlass[]{8,16,16,100},tealFrame[]{8,16,16,255};
        Check(gt2view::ModelGlassPixel(tealFrame));
        Check(!gt2view::IsWindowLayerNormal({.0552f,.9920f,-.1135f}));
        Check(gt2view::IsWindowLayerNormal({.2075f,.7077f,-.6754f}));
        Check(gt2view::ModelBlendGlassPixel(tealGlass) && !gt2view::ModelBlendGlassPixel(tealFrame));
        const uint8_t brightGlass[]{134,153,153,200},blueGlass[]{8,8,16,226};
        Check(gt2view::ModelBlendColourGlassPixel(brightGlass) && gt2view::ModelBlendColourGlassPixel(blueGlass));
        const uint8_t warmGlass[]{16,13,8,220};
        Check(gt2view::ModelNeutralGlassPixel(warmGlass));
        const uint8_t cyanRamp[]{26,53,53,194};
        Check(gt2view::ModelBlendColourGlassPixel(cyanRamp));
        const uint8_t whiteGlass[]{255,255,255,80},whitePaint[]{255,255,255,255},shadow[]{0,0,0,80};
        Check(gt2view::ModelBrightGlassPixel(whiteGlass));
        Check(!gt2view::ModelBrightGlassPixel(whitePaint) && !gt2view::ModelBrightGlassPixel(shadow));
        const gt2::PngImage flatPane{2,2,{70,70,70,100,70,70,70,100,164,164,164,200,0,0,0,0}};
        Check(gt2view::ClassifyNeutralOverlay(flatPane).flat);
        const gt2::PngImage shadowRamp{4,1,{30,30,30,100,40,40,40,70,50,50,50,40,60,60,60,10}};
        Check(gt2view::ClassifyNeutralOverlay(shadowRamp).translucent && !gt2view::ClassifyNeutralOverlay(shadowRamp).flat);
        auto solidFrame=flatPane;solidFrame.rgba[3]=255;
        Check(!gt2view::ClassifyNeutralOverlay(solidFrame).translucent);
        Check(!gt2view::ModelBlendColourGlassPixel(gray));
        Check(!gt2view::ModelNeutralGlassPixel(frame) && !gt2view::ModelNeutralGlassPixel(paint) && !gt2view::ModelNeutralGlassPixel(clear));
    }
    {
        std::array<gt2::GltfVertex,3> pane{};
        pane[0].position={0,0,0};pane[1].position={1,0,0};pane[2].position={0,1,0};
        pane[0].uv={0,0};pane[1].uv={1,0};pane[2].uv={0,1};
        const std::vector<gt2view::ModelGlazingOpening> opening{gt2view::GlazingOpening(pane)};
        auto reflection=pane;for(auto& v:reflection){v.position[2]+=.001f;v.uv={17,29};}
        Check(gt2view::CutGlazingLayers(reflection,opening).empty());
        for(auto& v:reflection)v.position[2]+=.02f;
        Check(gt2view::CutGlazingLayers(reflection,opening).size()==3);
        auto frame=pane;for(auto& v:frame)v.position[0]+=1;
        Check(gt2view::CutGlazingLayers(frame,opening).size()==3);
        auto opposite=pane;std::swap(opposite[1],opposite[2]);
        Check(gt2view::CutGlazingLayers(opposite,opening).empty());
        gt2view::ModelWindowHoles holes{{{.2f,.2f},{.4f,.2f},{.4f,.4f},{.2f,.4f}}};
        const auto glass=gt2view::GlazingInside(pane,holes);Check(glass.size()==6);
        for(const auto& v:glass){Check(v.position[0]>=.19999f && v.position[0]<=.40001f);Check(v.position[1]>=.19999f && v.position[1]<=.40001f);}
        std::vector<gt2view::ModelGlazingOpening> aperture;
        for(size_t i=0;i<glass.size();i+=3)aperture.push_back(gt2view::GlazingOpening({glass[i],glass[i+1],glass[i+2]}));
        const auto retained=gt2view::CutGlazingLayers(pane,aperture);Check(!retained.empty());
        double area=0;for(size_t i=0;i<retained.size();i+=3){const auto& a=retained[i].position;const auto& b=retained[i+1].position;const auto& c=retained[i+2].position;area+=std::abs((b[0]-a[0])*(c[1]-a[1])-(b[1]-a[1])*(c[0]-a[0]))*.5;}
        Check(std::abs(area-.46)<1e-5);
        const auto projectedArea=[](const std::vector<gt2::GltfVertex>& mesh){double sum=0;
            for(size_t i=0;i<mesh.size();i+=3){const auto& a=mesh[i].position;const auto& b=mesh[i+1].position;const auto& c=mesh[i+2].position;
                sum+=std::abs(double(b[0]-a[0])*(c[1]-a[1])-double(b[1]-a[1])*(c[0]-a[0]))*.5;}return sum;};
        // A shallow opaque cowl/wiper face intersects the glass prism, but
        // is not a duplicate pane. Keep its full area; blend layers still cut.
        auto trim=pane;for(auto& v:trim)v.position[2]=.04f*v.position[0]-.001f;
        Check(std::abs(projectedArea(gt2view::CutGlazingLayers(trim,opening))-.5)<1e-5);
        Check(projectedArea(gt2view::CutGlazingLayers(trim,opening,.025f,true))<.5);
        // A differently triangulated curved reflection crosses the aperture
        // slab even though every corner lies outside its plane tolerance.
        auto curved=pane;for(auto& v:curved)v.position[2]=.08f*v.position[0]-.04f;
        const auto partial=gt2view::CutGlazingLayers(curved,opening,.025f,true);
        Check(std::abs(projectedArea(partial)-.1875)<1e-5);
        // Transparent source pixels can mask a reflection without cutting
        // lettering or a pillar from the opaque diffuse frame.
        auto reflectionOnly=opening;reflectionOnly[0].reflectionOnly=true;
        Check(gt2view::CutGlazingLayers(pane,reflectionOnly).size()==3);
        Check(gt2view::CutGlazingLayers(pane,reflectionOnly,.003f,true).empty());
        // Many neighbouring mask cells must not tessellate the whole car into
        // thousands of pieces. Both the opening and the retained area matter.
        std::vector<gt2view::ModelGlazingOpening> strips;
        for(int i=0;i<16;++i){const float x=.2f+float(i)*.0125f;
            const gt2view::ModelWindowHoles strip{{{x,.2f},{x+.0125f,.2f},{x+.0125f,.4f},{x,.4f}}};
            const auto clipped=gt2view::GlazingInside(pane,strip);
            for(size_t j=0;j<clipped.size();j+=3)strips.push_back(gt2view::GlazingOpening({clipped[j],clipped[j+1],clipped[j+2]}));
        }
        const auto frameParts=gt2view::CutGlazingLayers(pane,strips);
        if(frameParts.size()>48 || std::abs(projectedArea(frameParts)-.46)>=1e-5)
            std::cerr<<"Adjacent glazing masks: "<<frameParts.size()<<" vertices, area "<<projectedArea(frameParts)<<'\n';
        Check(frameParts.size()<=48 && std::abs(projectedArea(frameParts)-.46)<1e-5);

        // A curved seating boundary is a union of adjacent vertical strips.
        // Do not re-triangulate the retained bonnet after each strip.
        std::vector<gt2view::ModelClipVolume> cowlVolumes;
        for(int i=0;i<16;++i){const float x=.2f+float(i)*.0125f;
            cowlVolumes.push_back({{1,0,0,x+.0125f},{-1,0,0,-x},{0,1,0,.4f},{0,-1,0,-.2f}});
        }
        const auto bonnet=gt2view::SubtractModelVolumes(pane,cowlVolumes);
        Check(bonnet.size()<=48 && std::abs(projectedArea(bonnet)-.46)<1e-5);

        // Transparent surround must not swallow an opaque mirror island.
        gt2::PngImage maskImage;maskImage.width=8;maskImage.height=8;
        std::vector<uint8_t> mask(64,1);for(int y=2;y<6;++y)for(int x=2;x<6;++x)mask[size_t(y)*8+x]=0;
        const auto rim=gt2view::ExactModelMask(maskImage,mask);
        double rimArea=0;for(const auto& h:rim)rimArea+=(h[1].x-h[0].x)*(h[2].y-h[1].y);
        Check(std::abs(rimArea-.75)<1e-6);
        const auto partition=gt2view::ReflectionMaskParts(maskImage,mask);double partitionArea=0;
        for(const auto& h:partition)for(size_t i=1;i+1<h.size();++i)partitionArea+=gt2view::cockpit_window_detail::Cross(h[0],h[i],h[i+1])*.5;
        Check(std::abs(partitionArea-.75)<1e-6);
        gt2::PngImage tiny;tiny.width=1;tiny.height=1;
        Check(gt2view::ModelGlassHoles(tiny,{1}).size()==1);

        // Clamp-to-edge and repeated coordinates must open the same physical
        // pane, including degenerate UVs used by flat tint passes.
        gt2view::ModelWindowHoles edgeHole{{{.8f,0},{1,0},{1,1},{.8f,1}}};
        auto beyond=pane;for(auto& v:beyond)v.uv={1.2f,.5f};
        Check(gt2view::GlazingInside(beyond,edgeHole,true,true).size()==3);
        Check(gt2view::GlazingInside(beyond,edgeHole,false,true).empty());
        for(auto& v:beyond)v.uv={2.9f,.5f};
        Check(gt2view::GlazingInside(beyond,edgeHole,false,true).size()==3);
        gt2view::ModelWindowHoles full{{{0,0},{1,0},{1,1},{0,1}}};
        beyond[0].uv={-8,-3};beyond[1].uv={9,4};beyond[2].uv={2,3};
        Check(gt2view::GlazingInside(beyond,full,false,false).size()==3);
    }
    {
        gt2::GltfMaterial m;m.nativeGt4Type=1;m.nativeAlphaFunc=-1;
        Check(gt2view::NativeAlphaAcceptsZero(m));
        m.nativeAlphaFunc=6;m.nativeAlphaRef=32;Check(!gt2view::NativeAlphaAcceptsZero(m));
        m.nativeAlphaFunc=5;m.nativeAlphaRef=0;Check(gt2view::NativeAlphaAcceptsZero(m));
        m.nativeGt4Type=-1;Check(!gt2view::NativeAlphaAcceptsZero(m));
        // Independent of the profile's seating side. Reject a dashboard,
        // longitudinal column and road-wheel attachment as steering rims.
        for(float side:{-1.f,1.f}) {
            gt2::GltfMesh mesh;mesh.materials.push_back({});gt2::GltfPrimitive p;p.material=0;
            for(int i=0;i<24;++i)for(float radius:{.16f,.18f}) {
                const float a=float(i)*6.2831853f/24;
                gt2::GltfVertex v;v.position={side*.35f+radius*std::cos(a),.65f+radius*std::sin(a),-.35f+.08f*std::sin(a)};
                p.indices.push_back(uint32_t(p.vertices.size()));p.vertices.push_back(v);
            }
            mesh.primitives.push_back(p);const auto wheel=gt2view::FindNativeCockpitWheel(mesh);
            Check(wheel.valid && std::abs(wheel.center[0]-side*.35f)<.001f && wheel.primitives[0]);
            mesh.materials[0].wheelAttachment=0;Check(!gt2view::FindNativeCockpitWheel(mesh).valid);
            mesh.materials[0].wheelAttachment=-1;
            for(auto& v:mesh.primitives[0].vertices)v.position[0]*=3;
            Check(!gt2view::FindNativeCockpitWheel(mesh).valid);
            mesh.primitives[0]=p;for(auto& v:mesh.primitives[0].vertices)v.position[2]*=5;
            Check(!gt2view::FindNativeCockpitWheel(mesh).valid);
        }
    }
    {
        gt2::PngImage image;image.width=16;image.height=8;image.rgba.resize(16*8*4);
        std::vector<uint8_t> mask(16*8);
        // Two teal panes separated by a full opaque pillar, with opaque borders.
        for(int y=1;y<7;++y)for(int x=1;x<15;++x)if(x<7 || x>8)mask[size_t(y)*16+x]=1;
        const auto holes=gt2view::ModelGlassHoles(image,mask);Check(holes.size()==2);
        gt2view::CockpitFit fit;fit.halfWidth=1;fit.frontCowl={{{-1,0,-1},{1,0,-1}}};fit.rearCowl={{{-1,0,1},{1,0,1}}};
        std::array<gt2::GltfVertex,3> pane{};
        pane[0].position={.9f,.1f,-.8f};pane[1].position={.9f,.9f,-.8f};pane[2].position={.9f,.9f,.8f};
        pane[0].uv={0,0};pane[1].uv={0,1};pane[2].uv={1,1};
        auto retained=gt2view::CutModelWindows(pane,holes,fit,1,{});Check(!retained.empty() && retained.size()%3==0);
        for(size_t i=0;i<retained.size();i+=3){
            float u=0,v=0;for(size_t j=0;j<3;++j){u+=retained[i+j].uv[0]/3;v+=retained[i+j].uv[1]/3;Check(std::abs(retained[i+j].position[0]-.9f)<1e-5f);}
            Check(v<=.125f || v>=.875f || u<=.0625f || u>=.9375f || (u>=.4375f && u<=.5625f));
        }
        for(auto& v:pane)v.position[1]-=2;
        const auto below=gt2view::CutModelWindows(pane,holes,fit,1,{});
        Check(below.size()==3 && below[0].position==pane[0].position && below[2].uv==pane[2].uv);
    }
    {
        std::array<std::array<float,3>,17> cowl{};
        for(size_t i=0;i<cowl.size();++i)cowl[i]={float(i)/8-1,0,-.5f};
        std::array<gt2::GltfVertex,3> triangle{};
        triangle[0].position={-.8f,0,-1};triangle[1].position={.8f,0,-1};triangle[2].position={0,0,.5f};
        for(auto& v:triangle){v.uv={v.position[0],v.position[2]};v.normal={0,1,0};}
        const auto split=gt2view::SplitModelCowl(triangle,cowl,1,{});
        auto curved=cowl;
        for(auto& p:curved)p[2]+=.1f*std::abs(p[0]);
        auto exterior=triangle;
        exterior[0].position={.7f,0,-.43f};exterior[1].position={.8f,0,-.43f};exterior[2].position={.75f,0,-.44f};
        // Inside the global Z range, but wholly ahead of the local cowl.
        Check(gt2view::SplitModelCowl(exterior,curved,1,{}).size()==3);
        float area=0;bool ahead=false,behind=false;
        for(size_t i=0;i+2<split.size();i+=3) {
            const auto& a=split[i].position;const auto& b=split[i+1].position;const auto& c=split[i+2].position;
            area+=std::abs((b[0]-a[0])*(c[2]-a[2])-(b[2]-a[2])*(c[0]-a[0]))*.5f;
            Check(std::min({a[2],b[2],c[2]})>=-.47501f || std::max({a[2],b[2],c[2]})<=-.47499f);
            for(size_t j=0;j<3;++j){const auto& v=split[i+j];Check(std::abs(v.uv[0]-v.position[0])<1e-5f && std::abs(v.uv[1]-v.position[2])<1e-5f);ahead|=v.position[2]<-.5f;behind|=v.position[2]>0;}
        }
        Check(std::abs(area-1.2f)<1e-4f && ahead && behind);
    }
    std::array<gt2::GltfVertex,3> backing{};
    gt2::GltfMaterial black;black.baseColorFactor={.02f,.02f,.02f,1.f};
    Check(gt2view::ModelWheelBacking(backing,&black,nullptr));
    gt2::GltfMaterial paint;paint.baseColorFactor={.5f,.1f,.1f,1.f};
    Check(!gt2view::ModelWheelBacking(backing,&paint,nullptr));
    black.reflection=true;
    Check(!gt2view::ModelWheelBacking(backing,&black,nullptr));
    std::vector<std::array<float,3>> arch;
    for(int i=0;i<12;++i) {
        const float a=float(i)*3.14159265f/12.f,b=float(i+1)*3.14159265f/12.f;
        const std::array<float,3> p{.9f,.35f+.32f*std::sin(a),1.f+.32f*std::cos(a)};
        const std::array<float,3> q{.9f,.35f+.32f*std::sin(b),1.f+.32f*std::cos(b)};
        auto r=q;r[1]+=.2f;auto s=p;s[1]+=.2f;
        for(const auto& v:{p,q,r,p,r,s})arch.push_back(v);
    }
    const std::array<float,5> donorWell{.85f,.35f,1.f,.35f,.24f};
    const auto fitted=gt2view::FitModelWheel(arch,donorWell);
    Check(fitted[0]+fitted[4]*.5f<.9f && fitted[3]<donorWell[3]);
    Check(std::abs((fitted[1]-fitted[3])-(donorWell[1]-donorWell[3]))<1e-5f);
    // Sparse trim alone cannot resize a wheel, nor may fitting enlarge it.
    Check(gt2view::FitModelWheel(std::span(arch).first(1),donorWell)[3]==donorWell[3]);
    // A triangle crossing from black backing into a painted wing is preserved.
    backing[0].color={.01f,.01f,.01f,1.f};backing[1].color=backing[0].color;
    Check(!gt2view::ModelWheelBacking(backing,nullptr,nullptr));
    namespace fs=std::filesystem;
    const auto dir=fs::temp_directory_path()/("gt2-model-set-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    try {
        gt2view::ModelTextureAtlas atlas;
        uint32_t at=0,again=0; const uint32_t side=gt2view::ModelTextureAtlas::kSide;
        Check(atlas.Insert(1,2,2,{1,2,3,4},at));
        const auto pixel=[&](int x,int y){ return atlas.pixels[(size_t(at>>16)+y)*side+(at&65535u)+x]; };
        Check(pixel(0,0)==1 && pixel(1,1)==4 && pixel(-1,0)==2 && pixel(0,-1)==3 && pixel(-1,-1)==4);
        atlas.dirty=false; Check(atlas.Insert(1,2,2,{1,2,3,4},again) && at==again && !atlas.dirty);
        // Reloading races must reuse the allocation, and refresh its border.
        for (int i=0;i<3000;++i) { atlas.Invalidate(1); Check(atlas.Insert(1,2,2,{5,6,7,8},again) && at==again); }
        Check(pixel(-1,-1)==8 && pixel(0,0)==5);
        Check(!atlas.Insert(2,0,1,{},again)); Check(!atlas.Insert(2,2,2,{1},again));
        // Colour dithering is reduced without changing glass/cutout alpha or
        // mixing opposite sides of a high-contrast feature.
        std::vector<uint32_t> checker(64);
        for(size_t y=0;y<8;++y)for(size_t x=0;x<8;++x) {
            const uint32_t rgb=((x+y)&1)?0x909090u:0x808080u;
            checker[y*8+x]=rgb|(uint32_t((x*31+y*17)&255)<<24);
        }
        Check(atlas.Insert(40,8,8,checker,again,true));
        const auto filtered=[&](size_t x,size_t y){return atlas.pixels[(size_t(again>>16)+y)*side+(again&65535u)+x];};
        for(size_t y=0;y<8;++y)for(size_t x=0;x<8;++x)Check((filtered(x,y)>>24)==(checker[y*8+x]>>24));
        Check(std::abs(int(filtered(3,3)&255u)-int(filtered(4,3)&255u))<8);
        for(size_t y=0;y<8;++y)for(size_t x=0;x<8;++x)checker[y*8+x]=x<4?0xff000000u:0xffffffffu;
        atlas.Invalidate(40);Check(atlas.Insert(40,8,8,checker,again,true));
        Check(filtered(3,3)==0xff000000u && filtered(4,3)==0xffffffffu);
        gt2view::ModelTextureAtlas full;
        std::vector<uint32_t> large(size_t(side-2)*(side-2),1);
        Check(full.Insert(1,side-2,side-2,large,at));
        Check(!full.Insert(2,1,1,{1},again)); // caller retains the SSBO fallback
        gt2::ModelSet missing(dir); Check(!missing.Has("dvpgn")); Check(!missing.HasNativeInterior("dvpgn"));
        for (const auto* path : {"../outside.gltf", "a/../../outside.gltf", "C:/outside.gltf", "/outside.gltf", ""})
            Reject([&]{gt2::ModelSetAsset(dir,path);});
        Check(gt2::ModelSetAsset(dir,"car/body.gltf")==dir/"car/body.gltf");
        std::ofstream(dir/"manifest.json")<<R"({"format":"gt2-model-set","version":99,"id":"gt4","cars":{}})";
        gt2::ModelSet badVersion(dir); Reject([&]{badVersion.Has("dvpgn");});
        std::ofstream(dir/"manifest.json")<<R"({"format":"gt2-model-set","version":1,"id":"gt4","cars":{"dvpgn":{},"../bad":{}}})";
        gt2::ModelSet atomic(dir); Reject([&]{atomic.Has("dvpgn");}); Check(!atomic.Has("dvpgn"));
        std::ofstream(dir/"manifest.json")<<R"({"format":"gt2-model-set","version":1,"id":"gt4","cars":{"dvpgn":{"mesh":"absent.gltf","cockpitMesh":"absent.gltf"}}})";
        gt2::ModelSet incomplete(dir); Check(incomplete.Has("dvpgn")); Check(!incomplete.Has("other")); Check(!incomplete.HasNativeInterior("dvpgn")); Check(!incomplete.HasNativeInterior("other"));
        Reject([&]{incomplete.Read("dvpgn");});
        const float triangle[9]={0,0,0,1,0,0,0,1,0};
        std::ofstream meshBuffer(dir/"mesh.bin",std::ios::binary);
        meshBuffer.write(reinterpret_cast<const char*>(triangle),sizeof(triangle)); meshBuffer.close();
        std::ofstream(dir/"mesh.gltf")<<R"({"asset":{"version":"2.0"},"buffers":[{"uri":"mesh.bin","byteLength":36}],"bufferViews":[{"buffer":0,"byteLength":36}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"}],"materials":[{"extras":{"gt2pc":{"rearView":true}}}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
        const auto nativeCar=[&](const std::string& extra) {
            std::ofstream(dir/"manifest.json")<<R"({"format":"gt2-model-set","version":1,"id":"gt4","cars":{"an16n":{"mesh":"mesh.gltf","cockpitMesh":"mesh.gltf","nativeInterior":true,"eye":[0,1,0],"mirror":[0,1,-0.2])"<<extra<<"}}}";
            return gt2::ModelSet(dir).Read("an16n");
        };
        Check(nativeCar("").speedometer[3]==0 && nativeCar("").openTop); // previous packs remain readable
        Check(!nativeCar(R"(,"openTop":false)").openTop); // authored closed cabins are not convertibles
        const auto native=nativeCar(R"(,"speedometer":[0.3,0.7,-0.2,0.14])");
        Check(native.speedometer[3]==.14f && native.cockpit.materials[0].rearView);
        Reject([&]{nativeCar(R"(,"speedometer":[0,0,0,-1])");});
        Reject([&]{nativeCar(R"(,"speedometer":[0,4,0,0.14])");});
        Reject([&]{nativeCar(R"(,"speedometer":[0,0,0])");});
        std::ofstream(dir/"manifest.json")<<R"({"format":"gt2-model-set","version":1,"id":"gt4","cars":{"an16n":{"mesh":"mesh.gltf","cockpitMesh":"missing-cockpit.gltf"}}})";
        Check(gt2::ModelSet(dir).Read("an16n",false).exterior.TriangleCount()==1);
        Reject([&]{gt2::ModelSet(dir).Read("an16n");}); // a race still requires the cockpit
        // Authored brake parts must not be merged back into the fixed body.
        fs::create_directories(dir/"native-v16");
        auto attachmentMesh=gt2::json::ReadFile((dir/"mesh.gltf").string());
        auto materials=gt2::json::Value::Array();
        auto bodyMaterial=attachmentMesh.Require("materials").At(0);
        auto bodyExtras=bodyMaterial.Require("extras").Require("gt2pc");
        auto program=gt2::json::Value::Object();program.Set("type",gt2::json::Value::Int(1));
        auto values=gt2::json::Value::Array();for(int k=0;k<17;++k)values.Push(gt2::json::Value::Int(k<8?1:0));
        program.Set("values",values);bodyExtras.Set("nativeGt4",program);
        auto extras=gt2::json::Value::Object();extras.Set("gt2pc",bodyExtras);bodyMaterial.Set("extras",extras);
        materials.Push(bodyMaterial);
        auto brakeMaterial=bodyMaterial;
        auto brakeExtras=bodyExtras;brakeExtras.Set("wheelAttachment",gt2::json::Value::Int(2));
        extras.Set("gt2pc",brakeExtras);brakeMaterial.Set("extras",extras);
        materials.Push(brakeMaterial);
        auto lampMaterial=bodyMaterial;auto lampExtras=bodyExtras;
        lampExtras.Set("lampStates",gt2::json::Value::Int(4));lampExtras.Set("reflection",gt2::json::Value::Bool(true));
        extras.Set("gt2pc",lampExtras);lampMaterial.Set("extras",extras);materials.Push(lampMaterial);
        attachmentMesh.Set("materials",materials);
        auto primitives=attachmentMesh.Require("meshes").At(0).Require("primitives");
        auto brakePrimitive=primitives.At(0);brakePrimitive.Set("material",gt2::json::Value::Int(1));primitives.Push(brakePrimitive);
        auto lampPrimitive=primitives.At(0);lampPrimitive.Set("material",gt2::json::Value::Int(2));primitives.Push(lampPrimitive);
        auto mesh=gt2::json::Value::Object();mesh.Set("primitives",primitives);
        auto meshes=gt2::json::Value::Array();meshes.Push(mesh);attachmentMesh.Set("meshes",meshes);
        gt2::json::WriteFile((dir/"native-v16/body.gltf").string(),attachmentMesh);
        fs::copy_file(dir/"mesh.bin",dir/"native-v16/mesh.bin");
        std::ofstream(dir/"native-v16/manifest.json")<<R"({"format":"gt4-native-overlay","version":1,"cars":{"an16n":"body.gltf"}})";
        std::ofstream(dir/"native-v16/paints.json.gz")<<R"({"format":"gt4-native-paints","colors":[]})";
        const auto attached=gt2::ModelSet(dir).Read("an16n",false);
        Check(attached.exterior.TriangleCount()==1 && attached.attachments.TriangleCount()==1);
        Check(attached.attachments.materials.at(1).wheelAttachment==2);
        Check(attached.lamps.TriangleCount()==1 && attached.reflections.TriangleCount()==0);
        Check(attached.lamps.materials.at(2).lampStates==4 && attached.lamps.materials.at(2).reflection);
        std::ofstream(dir/"native-v16/interiors.json")<<R"({"format":"gt4-native-interiors","version":1,"cars":{"an16n":{"mesh":"body.gltf","nativeInterior":true,"openTop":false,"eye":[0.3,1,0.4],"mirror":[0,1,-0.2]}}})";
        gt2::ModelSet discovered(dir);
        Check(discovered.HasNativeInterior("an16n"));
        const auto closedCabin=discovered.Read("an16n");
        Check(closedCabin.nativeInterior && !closedCabin.openTop && closedCabin.eye[0]==.3f);
        // The race shell's old interior cap must remain outside only, while
        // authored donor furniture uses the same material/palette namespace.
        auto shellExtras=bodyExtras;shellExtras.Set("exteriorOnly",gt2::json::Value::Bool(true));
        extras.Set("gt2pc",shellExtras);auto shellMaterial=bodyMaterial;shellMaterial.Set("extras",extras);
        auto donorExtras=bodyExtras;donorExtras.Set("interiorOnly",gt2::json::Value::Bool(true));
        extras.Set("gt2pc",donorExtras);auto donorMaterial=bodyMaterial;donorMaterial.Set("extras",extras);
        auto cabinMaterials=gt2::json::Value::Array();cabinMaterials.Push(shellMaterial);cabinMaterials.Push(brakeMaterial);cabinMaterials.Push(lampMaterial);cabinMaterials.Push(donorMaterial);
        auto cabinPrimitives=primitives;auto donorPrimitive=primitives.At(0);donorPrimitive.Set("material",gt2::json::Value::Int(3));cabinPrimitives.Push(donorPrimitive);
        auto cabinMesh=attachmentMesh;cabinMesh.Set("materials",cabinMaterials);mesh.Set("primitives",cabinPrimitives);meshes=gt2::json::Value::Array();meshes.Push(mesh);cabinMesh.Set("meshes",meshes);
        gt2::json::WriteFile((dir/"native-v16/body.gltf").string(),cabinMesh);
        const auto separated=gt2::ModelSet(dir).Read("an16n");
        Check(separated.exterior.TriangleCount()==1 && separated.exterior.primitives[0].material==0);
        Check(separated.cockpit.TriangleCount()==1 && separated.cockpit.primitives[0].material==3);
        gt2::json::WriteFile((dir/"native-v16/body.gltf").string(),attachmentMesh);
        std::ofstream(dir/"native-v16/interiors.json")<<R"({"format":"gt4-native-interiors","version":1,"cars":{"an16n":{"mesh":"wrong-body.gltf","nativeInterior":true,"openTop":true}}})";
        Reject([&]{gt2::ModelSet(dir).HasNativeInterior("an16n");});
        fs::remove(dir/"native-v16/interiors.json");
        Check(!gt2::ModelSet(dir).HasNativeInterior("an16n")); // removing the overlay restores the old source decision
        brakeExtras.Set("wheelAttachment",gt2::json::Value::Int(4));
        extras.Set("gt2pc",brakeExtras);brakeMaterial.Set("extras",extras);
        materials=gt2::json::Value::Array();materials.Push(bodyMaterial);materials.Push(brakeMaterial);
        attachmentMesh.Set("materials",materials);
        gt2::json::WriteFile((dir/"native-v16/body.gltf").string(),attachmentMesh);
        Reject([&]{gt2::ModelSet(dir).Read("an16n",false);});
        Check(gt2::shell::SharedVrKey("gt4_model_set"));
        fs::remove_all(dir);
        std::cout<<"Model set manifest, atomic fallback, missing assets, paths and shared setting passed\n";
    } catch (...) { fs::remove_all(dir);throw; }
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}

