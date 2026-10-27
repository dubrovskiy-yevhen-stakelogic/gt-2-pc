// Render contracts use the production native pipelines, including MSAA.
#include "offscreen_car_audit.h"
#include "gt2formats/png_reader.h"
#include "gt2view/particles.h"
#include <filesystem>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv) {try {
    if(argc!=2)throw std::runtime_error("output-under-work");
    const auto out=std::filesystem::absolute(argv[1]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{128,128},VK_FORMAT_R8G8B8A8_UNORM);
    std::vector<SceneVertex> vertices;
    for(const auto& xy:std::array<std::array<float,2>,6>{{{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,-.8f},{.8f,.8f},{-.8f,.8f}}}) {
        SceneVertex v{};v.pos[0]=xy[0];v.pos[1]=xy[1];v.pos[2]=.5f;
        std::fill_n(v.color,3,1.f);v.flags=kModelNative;vertices.push_back(v);
    }
    const auto base=SceneRenderer::kModelSetVertexBase;
    renderer.SetVertices(base,vertices);
    std::vector<NativeModelVertex> attrs(6);for(auto& a:attrs){a.normal={0,1,0};a.materialAlpha=16384u<<16;}
    renderer.SetNativeModelVertices(0,attrs);
    NativeModelMaterial material;material.values[0]=material.values[1]=material.values[2]=1;
    material.values[16]=0;material.values[18]=-1;material.values[23]=1;
    renderer.SetNativeModelMaterials(0,{material});
    DrawItem item;item.firstVertex=base;item.vertexCount=6;item.mvp[0]=item.mvp[5]=item.mvp[10]=item.mvp[15]=1;
    bool failed=false;
    for(uint32_t samples:{1u,2u,4u}) {
        RenderOptions options;options.msaa=samples;renderer.SetOptions(options);
        // Compare actual GPU pixels for STP-only smoke and a modded opaque
        // palette. Dropping only the empty pass must preserve both cases.
        auto smoke=vertices;
        for(auto& v:smoke){v.flags=kTextured|kSemiTransparent;v.page=0;v.clut=352u|(510u<<16);v.texel[0]=v.texel[1]=.5f;}
        renderer.SetVertices(0,smoke);
        for(bool opaqueColour:{false,true}) {
            std::vector<uint16_t> vram(1024*512);vram[0]=0x1111;
            std::array<uint16_t,16> palette{};palette[1]=uint16_t(0x421|(opaqueColour?0:0x8000));
            std::copy(palette.begin(),palette.end(),vram.begin()+510*1024+352);
            renderer.UploadVram(0,512,vram.data());
            DrawItem solid;solid.vertexCount=6;solid.stpPass=1;solid.mvp[0]=solid.mvp[5]=solid.mvp[10]=solid.mvp[15]=1;
            DrawItem add=solid;add.stpPass=2;add.blend=1;
            const bool needed=SmokeOpaquePassRequired(smoke,{SmokePaletteHasOpaque(palette),true});
            failed|=needed!=opaqueColour;
            auto unknown=smoke;unknown[0].clut=0;
            failed|=!SmokeOpaquePassRequired(unknown,{false,false});
            const auto prefix="smoke-"+std::to_string(samples)+"-"+std::to_string(opaqueColour);
            std::fill_n(renderer.clearColor,3,0.f);
            renderer.Draw({solid,add},(out/(prefix+"-before.png")).string(),2);renderer.WaitFrame();
            std::vector<DrawItem> after;if(needed)after.push_back(solid);after.push_back(add);
            renderer.Draw(after,(out/(prefix+"-after.png")).string(),after.size());renderer.WaitFrame();
            const auto a=gt2::ReadPngFile((out/(prefix+"-before.png")).string()),b=gt2::ReadPngFile((out/(prefix+"-after.png")).string());
            failed|=a.rgba!=b.rgba || a.rgba[(64*128+64)*4]==0;
            std::cout<<"Smoke MSAA "<<samples<<", opaque palette "<<opaqueColour<<": identical="<<(a.rgba==b.rgba)<<", opaque pass="<<needed<<'\n';
            // Compare cached stereo blending against the general shader. The
            // opaque blocker exercises early depth while transparent texels must
            // keep their coverage holes, in both multiview and two-pass stereo.
            auto blockerVertices=vertices;
            for(auto& v:blockerVertices){v.flags=0;v.pos[0]=v.pos[0]*.45f-.4f;v.pos[2]=.8f;v.color[0]=.3f;v.color[1]=v.color[2]=0;}
            renderer.SetVertices(16,blockerVertices);
            DrawItem blocker;blocker.firstVertex=16;blocker.vertexCount=6;blocker.space=kSpaceWorld;
            blocker.mvp[0]=blocker.mvp[5]=blocker.mvp[10]=blocker.mvp[15]=1;
            auto stereoSolid=solid,stereoAdd=add;stereoSolid.space=stereoAdd.space=kSpaceWorld;
            StereoViews views;for(auto& matrix:views.worldVP)for(int i=0;i<4;++i)matrix[i*5]=1;
            renderer.SetStereoViews(views);
            for(bool multiview:{false,true}) {
                renderer.CreateStereoTarget({128,128},VK_FORMAT_R8G8B8A8_UNORM,multiview);
                const std::vector<DrawItem> draws{blocker,stereoSolid,stereoAdd};
                renderer.EnableDecodedTextures(false);const auto reference=renderer.CaptureStereoForAudit(draws,draws.size());
                renderer.EnableDecodedTextures(true);const auto cached=renderer.CaptureStereoForAudit(draws,draws.size());
                size_t different=0;int maximum=0;
                for(size_t i=0;i<reference.size();++i){int delta=std::abs(int(reference[i])-int(cached[i]));maximum=std::max(maximum,delta);different+=delta>1;}
                failed|=different!=0 || reference.empty();
                std::cout<<"Smoke cached stereo MSAA "<<samples<<", multiview "<<multiview<<", opaque palette "<<opaqueColour<<": differences="<<different<<", max="<<maximum<<'\n';
                auto soft=draws;soft.back().softParticle=1;
                const auto coarse=renderer.CaptureStereoForAudit(soft,soft.size());
                failed|=coarse!=cached;
                // The following ordinary draw must restore its original rate.
                const auto restored=renderer.CaptureStereoForAudit(draws,draws.size());
                failed|=restored!=cached;
                std::cout<<"Soft smoke uniform colour/depth and following draw: identical="<<(coarse==cached && restored==cached)<<'\n';
            }
        }
        for(size_t sceneItems:{0u,1u}) {
            const auto prefix="coverage-"+std::to_string(samples)+"-"+std::to_string(sceneItems);
            std::fill_n(renderer.clearColor,3,0.f);renderer.Draw({item},(out/(prefix+"-black.png")).string(),sceneItems);renderer.WaitFrame();
            std::fill_n(renderer.clearColor,3,1.f);renderer.Draw({item},(out/(prefix+"-white.png")).string(),sceneItems);renderer.WaitFrame();
            const auto a=gt2::ReadPngFile((out/(prefix+"-black.png")).string()),b=gt2::ReadPngFile((out/(prefix+"-white.png")).string());
            size_t leaks=0;
            for(size_t y=32;y<96;++y)for(size_t x=32;x<96;++x)for(size_t c=0;c<3;++c)
                leaks+=std::abs(int(a.rgba[(y*128+x)*4+c])-int(b.rgba[(y*128+x)*4+c]))>1;
            std::cout<<"Opaque coverage, scene items "<<sceneItems<<", MSAA "<<samples<<": leaks="<<leaks<<'\n';
            failed|=leaks!=0;
        }
        const auto b=out/("opaque-black-"+std::to_string(samples)+".png");
        const auto w=out/("opaque-white-"+std::to_string(samples)+".png");
        std::fill_n(renderer.clearColor,3,0.f);renderer.Draw({item},b.string(),1);renderer.WaitFrame();
        std::fill_n(renderer.clearColor,3,1.f);renderer.Draw({item},w.string(),1);renderer.WaitFrame();
        const auto black=gt2::ReadPngFile(b.string()),white=gt2::ReadPngFile(w.string());
        size_t leaks=0;int maximum=0;
        for(size_t y=32;y<96;++y)for(size_t x=32;x<96;++x)for(size_t c=0;c<3;++c) {
            const size_t i=(y*128+x)*4+c;const int delta=std::abs(int(black.rgba[i])-int(white.rgba[i]));
            maximum=std::max(maximum,delta);if(delta>1)++leaks;
        }
        std::cout<<"MSAA "<<samples<<": opaque background leaks="<<leaks<<", max="<<maximum<<'\n';
        failed|=leaks!=0;
        // CAR4 begins with culling enabled; explicit 0x2e and the seated
        // cabin path remain two-sided. Exercise the native material state,
        // including the fragment fallback, rather than only kCullBack.
        for(unsigned mode=0;mode<4;++mode) {
            auto shell=vertices;
            if(mode==0)for(size_t i=0;i<shell.size();i+=3)std::swap(shell[i+1],shell[i+2]);
            if(mode==3)for(auto& v:shell)v.flags|=kCabinLining;
            material.values[22]=mode==2?0.f:1.f;
            renderer.SetVertices(base,shell);renderer.SetNativeModelMaterials(0,{material});
            const auto imagePath=out/("cull-"+std::to_string(samples)+"-"+std::to_string(mode)+".png");
            renderer.Draw({item},imagePath.string(),1);renderer.WaitFrame();
            const auto image=gt2::ReadPngFile(imagePath.string());
            const unsigned centre=image.rgba[(64*128+64)*4];
            const bool hidden=mode==1;
            failed|=hidden?centre<250:centre>240;
            std::cout<<"MSAA "<<samples<<": native cull case "<<mode<<", centre="<<centre<<'\n';
        }
        material.values[22]=0;renderer.SetVertices(base,vertices);renderer.SetNativeModelMaterials(0,{material});
        // Hidden inner panels must not shine through a nearer outer skin.
        // A genuinely nearer authored layer remains legal under GS GEQUAL;
        // requiring EQUAL here would silently remove valid clearcoat/glass.
        for(const auto blend:{kBlendNativeEnvironment,kBlendNativeSurfaceEnvironment})for(float depth:{.4f,.49999f,.5f,.6f}) {
            auto planes=vertices;
            for(auto& v:planes)std::fill_n(v.color,3,.15f);
            for(auto v:vertices){v.pos[2]=depth;v.flags|=kModelReflection;planes.push_back(v);}
            renderer.SetVertices(base,planes);
            auto attributes=attrs;attributes.insert(attributes.end(),attrs.begin(),attrs.end());
            renderer.SetNativeModelVertices(0,attributes);
            DrawItem coat=item;coat.firstVertex+=6;coat.blend=blend;
            const auto path=out/("overlay-depth-"+std::to_string(blend)+"-"+std::to_string(samples)+"-"+std::to_string(depth)+".png");
            renderer.Draw({item,coat},path.string(),2);renderer.WaitFrame();
            const auto pixels=gt2::ReadPngFile(path.string());const int value=pixels.rgba[(64*128+64)*4];
            const bool visible=blend==kBlendNativeSurfaceEnvironment?depth==.5f:depth>=.5f;
            failed|=visible?value<60:value>32;
            std::cout<<"MSAA "<<samples<<": overlay depth "<<depth<<", centre="<<value<<'\n';
        }
        renderer.SetVertices(base,vertices);renderer.SetNativeModelVertices(0,attrs);
    }

    // A physical +Y normal must receive more overhead studio light than -Y.
    // Moving the asymmetric viewport must not change that material light.
    material.values[16]=1;material.values[4]=material.values[5]=material.values[6]=1;
    renderer.SetNativeModelMaterials(0,{material});
    for(auto& v:vertices){v.pos[0]*=4;v.pos[1]*=4;v.pos[2]=-1;}
    renderer.SetVertices(base,vertices);
    RenderOptions lightingOptions;renderer.SetOptions(lightingOptions);
    int up=-1,down=-1;
    for(int direction:{1,-1})for(float shift:{0.f,.7f}) {
        for(auto& attr:attrs)attr.normal={0,float(direction),0};renderer.SetNativeModelVertices(0,attrs);
        const auto projection=View({0,0,0},0,0);std::copy(projection.begin(),projection.end(),item.mvp);
        item.mvp[8]+=shift*item.mvp[11];item.mvp[9]+=shift*item.mvp[11];
        const auto path=out/("light-"+std::to_string(direction)+"-"+std::to_string(shift)+".png");
        renderer.Draw({item},path.string(),1);renderer.WaitFrame();const auto image=gt2::ReadPngFile(path.string());
        const int value=image.rgba[(64*128+64)*4];
        std::cout<<"Normal Y="<<direction<<", viewport="<<shift<<", light="<<value<<'\n';
        int& baseline=direction==1?up:down;
        if(shift==0)baseline=value;else failed|=std::abs(value-baseline)>1;
    }
    failed|=up<=down+25;
    // More than 16 palettes sharing page X / CLUT X must remain cached and
    // produce the same GPU colours in mono, multiview and two-pass stereo.
    {
        std::vector<uint16_t> vram(1024*512);vram[576]=0x1111;
        std::vector<SceneVertex> strips;
        for(unsigned row=0;row<32;++row){
            vram[(480+row)*1024+353]=uint16_t((row+1)%32 | (7u<<5) | (15u<<10));
            const float left=-.96f+float(row)*.06f;
            for(const auto& xy:std::array<std::array<float,2>,6>{{{0,-.8f},{.06f,-.8f},{.06f,.8f},{0,-.8f},{.06f,.8f},{0,.8f}}}){
                SceneVertex v{};v.pos[0]=left+xy[0];v.pos[1]=xy[1];v.pos[2]=.5f;
                v.texel[0]=v.texel[1]=.5f;v.page=576;v.clut=352u|((480+row)<<16);v.flags=kTextured|kRawTexture;
                std::fill_n(v.color,3,1.f);strips.push_back(v);
            }
        }
        renderer.UploadVram(0,512,vram.data());renderer.SetVertices(0,strips);
        RenderOptions options;options.msaa=1;renderer.SetOptions(options);
        DrawItem draw;draw.vertexCount=uint32_t(strips.size());draw.space=kSpaceWorld;
        for(int i=0;i<4;++i)draw.mvp[i*5]=1;
        StereoViews views;for(auto& matrix:views.worldVP)for(int i=0;i<4;++i)matrix[i*5]=1;renderer.SetStereoViews(views);
        for(bool multiview:{false,true}){
            renderer.CreateStereoTarget({128,128},VK_FORMAT_R8G8B8A8_UNORM,multiview);
            renderer.EnableDecodedTextures(false);const auto reference=renderer.CaptureStereoForAudit({draw},1);
            renderer.EnableDecodedTextures(true);const auto cached=renderer.CaptureStereoForAudit({draw},1);
            size_t different=0;for(size_t i=0;i<reference.size();++i)different+=std::abs(int(reference[i])-int(cached[i]))>1;
            failed|=different!=0 || renderer.UncachedMaterials()!=0 || reference.empty();
            std::cout<<"Palette row stereo "<<multiview<<": differences="<<different<<", misses="<<renderer.UncachedMaterials()<<'\n';
        }
        renderer.EnableDecodedTextures(false);renderer.Draw({draw},(out/"palette-rows-general.png").string(),1);renderer.WaitFrame();
        renderer.EnableDecodedTextures(true);renderer.Draw({draw},(out/"palette-rows-cached.png").string(),1);renderer.WaitFrame();
        const auto general=gt2::ReadPngFile((out/"palette-rows-general.png").string()),cached=gt2::ReadPngFile((out/"palette-rows-cached.png").string());
        failed|=general.rgba!=cached.rgba || renderer.UncachedMaterials()!=0;
        std::cout<<"Palette row mono: identical="<<(general.rgba==cached.rgba)<<", misses="<<renderer.UncachedMaterials()<<'\n';
    }
    return failed?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}

