#include "offscreen_car_audit.h"
#include <cstdio>
#include <fstream>
#include <chrono>
#include "gt2view/generated_world.h"
#include "gt2export/png_writer.h"
#include <cstdlib>
#include "gt2view/generated_mip_atlas.h"
using namespace gt2view;
int main() { try {
    Device device; VkContext context(device.existing);
    VkSceneRenderer renderer(context,{64,64},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);
    {
        namespace fs=std::filesystem;
        const auto folder=fs::temp_directory_path()/("gt2-texture-pack-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        struct Cleanup {fs::path path;~Cleanup(){std::error_code ec;fs::remove_all(path,ec);}} cleanup{folder};
        auto require=[](bool ok,const char* why){if(!ok)throw std::runtime_error(why);};
        fs::create_directories(folder/"arcade/generated-world");
        gt2::generated::SetRoot((folder/"arcade/disc.raw2352").string());require(gt2::generated::root==folder/"arcade/generated-world","legacy pack path changed");
        fs::create_directories(folder/"textures/arcade/test");
        gt2::generated::SetRoot((folder/"arcade/disc.raw2352").string());require(gt2::generated::root==folder/"textures/arcade","common textures folder not found");
        const std::array<uint8_t,4> source{1,2,3,4};const auto dir=gt2::generated::root/"test";
        std::ofstream(dir/"pack.txt")<<"GT2-GENERATED-WORLD-2 "<<gt2::Sha1Hex(source)<<"\n0 0 0 0 4 4 custom.png road\n0 0 8 0 4 4 missing.png road\n";
        std::array<uint8_t,4*4*4> art{};for(size_t i=0;i<art.size();i+=4){art[i]=255;art[i+3]=255;}
        gt2::WritePngRgba((dir/"custom.png").string(),4,4,art);
        GeneratedWorld world;auto load=[&]{uint32_t cursor=SceneRenderer::kGeneratedWorldTexelBase;world.Load("test",source,renderer,cursor);};
        auto triangle=[](float x){std::vector<SceneVertex> v(3);for(auto& p:v){p.flags=kTextured;p.texel[0]=x;p.texel[1]=1;}return v;};
        load();auto replaced=triangle(1),missing=triangle(9);require(world.Apply(replaced)==1,"PNG replacement not applied");require(world.Apply(missing)==0,"missing PNG replaced original");
        gt2::generated::SetEnabled(false);auto disabled=triangle(1);require(world.Apply(disabled,gt2::generated::ActiveMask())==0,"disabled pack replaced original");
        fs::remove(dir/"custom.png");load();auto deleted=triangle(1);require(world.Apply(deleted)==0,"deleted PNG remained cached");
        std::ofstream(dir/"custom.png")<<"invalid PNG";load();auto corrupt=triangle(1);require(world.Apply(corrupt)==0,"corrupt PNG replaced original");
        gt2::WritePngRgba((dir/"custom.png").string(),4,4,art);load();auto restored=triangle(1);require(world.Apply(restored)==1,"restored PNG not reloaded");
        require(!gt2::generated::BuiltinTextureManifest("seatt_s","bb3a919b5115803ea33fee89b24d1761a3b01a35").empty(),"built-in Seattle mapping missing");
        require(gt2::generated::BuiltinTextureManifest("seatt_s","wrong-profile").empty(),"wrong disc mapping accepted");
        gt2::generated::root.clear();
        std::puts("PASS: texture folder, partial pack, off, deleted/corrupt/restored PNG, profile binding");
    }

    StereoViews views{};
    for(int eye=0;eye<2;++eye)for(auto* matrix:{views.worldVP[eye],views.skyVP[eye],views.hudVP[eye]})
        matrix[0]=matrix[5]=matrix[10]=matrix[15]=1;
    renderer.SetStereoViews(views);
    std::array<uint32_t,16> pixels;pixels.fill(0xff40c020);
    renderer.BeginGeneratedTextures();
    renderer.UploadGeneratedTexture(SceneRenderer::kGeneratedWorldTexelBase,4,4,pixels.data());
    const float xy[6][2]={{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,-.8f},{.8f,.8f},{-.8f,.8f}};
    std::vector<SceneVertex> vertices(6);
    for(size_t i=0;i<vertices.size();++i) {
        auto& v=vertices[i];v.pos[0]=xy[i][0];v.pos[1]=xy[i][1];v.pos[2]=.5f;
        v.color[0]=v.color[1]=v.color[2]=1;v.texel[0]=v.texel[1]=2;
        v.page=SceneRenderer::kGeneratedWorldTexelBase;v.clut=4u|(4u<<16);
        v.flags=kTextured|kExternalTexture|kGeneratedMipTexture|kModelClampU|kModelClampV;
    }
    renderer.SetVertices(0,vertices);
    DrawItem item;item.vertexCount=6;item.space=kSpaceWorld;item.mvp[0]=item.mvp[5]=item.mvp[10]=item.mvp[15]=1;
    for(const bool smooth:{false,true})for(const unsigned samples:{1u,4u}) {
        RenderOptions options;options.smoothTextures=smooth;options.msaa=samples;renderer.SetOptions(options);
        renderer.CreateStereoTarget({64,64},VK_FORMAT_R8G8B8A8_UNORM,true);
        const auto rgba=renderer.CaptureStereoForAudit({item},1);
#ifdef _WIN32
        _putenv_s("GT2_AUDIT_UNFOVEATED_GENERATED","1");
#else
        setenv("GT2_AUDIT_UNFOVEATED_GENERATED","1",1);
#endif
        const auto reference=renderer.CaptureStereoForAudit({item},1);
#ifdef _WIN32
        _putenv_s("GT2_AUDIT_UNFOVEATED_GENERATED","");
#else
        unsetenv("GT2_AUDIT_UNFOVEATED_GENERATED");
#endif
        if(rgba.size()!=reference.size())throw std::runtime_error("world specialization capture size");
        for(size_t p=0;p<rgba.size();++p)if(std::abs(int(rgba[p])-int(reference[p]))>1)
            throw std::runtime_error("world specialization changed unfoveated texture pixels");
        for(size_t eye=0;eye<2;++eye) {
            const size_t p=(eye*64*64+32*64+32)*4;
            if(rgba[p+1]<150 || rgba[p]>80 || rgba[p+2]>110)
                throw std::runtime_error("generated clamp-U flag entered the nearest PS1 HUD sampler");
        }
    }
    // Asphalt detail must vary across the road in both eyes. A constant
    // outPaintView previously collapsed the entire aggregate to one texel.
    renderer.BeginGeneratedTextures();
    const uint32_t aggregate=SceneRenderer::kGeneratedWorldTexelBase;
    std::vector<uint32_t> stone(512*512);
    for(unsigned y=0;y<512;++y)for(unsigned x=0;x<512;++x) {
        const uint32_t c=32+x*192/511;stone[y*512+x]=0xff000000u|c|(c<<8)|(c<<16);
    }
    renderer.UploadGeneratedTexture(aggregate,512,512,stone.data());
    const uint32_t road=aggregate+512*512+GeneratedMipAtlas::kLevels+1;
    renderer.UploadExternalTexture(road-1,1,&aggregate);
    pixels.fill(0xff808080);
    renderer.UploadGeneratedTexture(road,4,4,pixels.data());
    for(auto& v:vertices){v.page=road;v.flags|=kClampTextureRect;}
    renderer.SetVertices(0,vertices);
    for(const unsigned samples:{1u,2u,4u}) {
        RenderOptions options;options.smoothTextures=true;options.msaa=samples;renderer.SetOptions(options);
        renderer.CreateStereoTarget({64,64},VK_FORMAT_R8G8B8A8_UNORM,true);
        const auto rgba=renderer.CaptureStereoForAudit({item},1);
#ifdef _WIN32
        _putenv_s("GT2_AUDIT_UNFOVEATED_GENERATED","1");
#else
        setenv("GT2_AUDIT_UNFOVEATED_GENERATED","1",1);
#endif
        const auto reference=renderer.CaptureStereoForAudit({item},1);
#ifdef _WIN32
        _putenv_s("GT2_AUDIT_UNFOVEATED_GENERATED","");
#else
        unsetenv("GT2_AUDIT_UNFOVEATED_GENERATED");
#endif
        if(rgba.size()!=reference.size())throw std::runtime_error("world specialization capture size");
        for(size_t p=0;p<rgba.size();++p)if(std::abs(int(rgba[p])-int(reference[p]))>1)
            throw std::runtime_error("world specialization changed unfoveated texture pixels");
        for(size_t eye=0;eye<2;++eye) {
            const size_t a=(eye*64*64+32*64+20)*4,b=(eye*64*64+32*64+44)*4;
            if(std::abs(int(rgba[a])-int(rgba[b]))<40)
                throw std::runtime_error("asphalt aggregate lost its spatial coordinates");
        }
    }
    std::puts("PASS: asphalt detail varies across the road in both eyes, MSAA 1/2/4");
    std::puts("PASS: generated/clamped external textures use their own sampler in both eyes, nearest/smooth, MSAA 1/4");
} catch(const std::exception& e) {std::fprintf(stderr,"%s\n",e.what());return 1;} }
