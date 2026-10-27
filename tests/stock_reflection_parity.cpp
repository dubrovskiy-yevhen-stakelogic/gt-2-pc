// Compare the same original car and reflection map through mono and stereo.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include <filesystem>
#include <iostream>
#include "gt2formats/png_reader.h"
#include <fstream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("disc output-under-work");
    const auto out=std::filesystem::absolute(argv[2]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{512,512},VK_FORMAT_R8G8B8A8_UNORM);
    // A neutral, nonuniform PS1 reflection map. Identical data in both paths.
    std::vector<uint16_t> env(512*1024);
    for(int y=0;y<128;++y)for(int x=0;x<128;++x){
        const int index=1+(x+y)*14/254;
        env[size_t(y)*1024+576+x/4]|=uint16_t(index<<((x%4)*4));
    }
    for(int i=1;i<16;++i)env[511*1024+368+i]=uint16_t(i*2|((i*2)<<5)|((i*2)<<10));
    renderer.UploadVram(0,512,env.data());
    SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(false);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const auto vp=View({2,1.4f,4},-25,0);
    renderer.CreateStereoTarget({512,512},VK_FORMAT_R8G8B8A8_UNORM,true);
    size_t failures=0;std::ofstream results(out/"results.csv");results<<"case,wheel_covered,reflection_pixels,stereo_changed\n";
    for(const char* id:{"buc9n","dvpgn"}){
        const int slot=assets.UseCar(id);if(slot<0)throw std::runtime_error("car unavailable");
        gt2::modernCarReflections=false;
        assets.UpdateCarReflection(slot,identity,{{{1,0,0},{0,-1,0},{0,0,-1}}});
        for(bool smooth:{false,true})for(bool cache:{false,true})for(int msaa:{1}){
            RenderOptions options;options.msaa=msaa;options.smoothTextures=smooth;renderer.SetOptions(options);renderer.EnableDecodedTextures(cache);
            const auto name=std::string(id)+(smooth?"-smooth":"-nearest")+(cache?"-cache":"-raw")+"-msaa"+std::to_string(msaa);
            std::vector<DrawItem> mono;assets.AppendCarItems(mono,slot,vp.data(),0,0);
            std::erase_if(mono,[](const auto& d){return d.blend==2;});
            auto bare=mono;std::erase_if(bare,[](const auto& d){return d.blend!=kBlendOpaque;});
            renderer.Draw(bare,(out/(name+"-bare.png")).string(),bare.size());renderer.WaitFrame();
            renderer.Draw(mono,(out/(name+"-mono.png")).string(),mono.size());renderer.WaitFrame();
            StereoViews views;for(int e=0;e<2;++e)std::copy(vp.begin(),vp.end(),views.worldVP[e]);renderer.SetStereoViews(views);
            std::vector<DrawItem> stereo;assets.AppendCarItems(stereo,slot,identity,0,0);
            std::erase_if(stereo,[](const auto& d){return d.blend==2;});for(auto& d:stereo)d.space=kSpaceWorld;
            renderer.DrawStereo(stereo,(out/(name+"-stereo.png")).string(),stereo.size());renderer.WaitFrame();
            const auto base=gt2::ReadPngFile((out/(name+"-bare.png")).string());
            const auto flat=gt2::ReadPngFile((out/(name+"-mono.png")).string());
            const auto xr=gt2::ReadPngFile((out/(name+"-stereo.png")).string());
            size_t covered=0,shine=0,different=0;
            for(size_t at=0;at<base.rgba.size();at+=4){
                const bool wheel=base.rgba[at]>250 && base.rgba[at+1]<3 && base.rgba[at+2]>250;
                bool changed=false,eyeChanged=false;int added=0;
                for(size_t c=0;c<3;++c){
                    changed|=std::abs(int(base.rgba[at+c])-int(flat.rgba[at+c]))>3;
                    eyeChanged|=std::abs(int(xr.rgba[at+c])-int(flat.rgba[at+c]))>3;
                    added+=int(flat.rgba[at+c])-int(base.rgba[at+c]);
                }
                covered+=wheel && changed;shine+=!wheel && added>8;different+=eyeChanged;
            }
            // Different matrix multiplication orders can affect triangle edges.
            results<<name<<','<<covered<<','<<shine<<','<<different<<'\n';
            failures+=(std::getenv("GT2_AUDIT_WHEEL_COLOR") && covered!=0) || shine<50 || different>256;

        }
    }
    std::cout<<"Original reflection parity failures: "<<failures<<"\n";return failures?2:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
