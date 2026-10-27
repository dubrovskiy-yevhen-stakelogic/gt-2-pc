// Actual GPU capture coverage: empty sky and opaque black scenery must differ
// for GT2, while GT4 retains its original black environment background.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("disc pack output-under-work");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{192,192},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({192,192},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::vector<SceneVertex> wall;
    for(const auto xy:std::array<std::array<float,2>,6>{{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}}}){
        SceneVertex v{};v.pos[0]=xy[0];v.pos[1]=xy[1];v.pos[2]=.5f;wall.push_back(v);
    }
    renderer.SetVertices(0,wall);DrawItem block;block.vertexCount=6;block.space=kSpaceMirrorSource;std::copy_n(identity,16,block.mvp);
    const auto cameras=NativeEnvironmentCameras({0,0,0});
    std::ofstream csv(out/"environment.csv");csv<<"car,path,sky_changed,wall_delta\n";
    for(const char* id:{"t2vzn","dvpgn","vst6n"})for(bool gt4:{false,true}){
        SceneAssets assets(renderer,vol,false);assets.SetGt4Enabled(gt4);const int slot=assets.UseCar(id);
        if(slot<0 || (gt4 && !assets.UsesGt4(slot)))throw std::runtime_error("missing test car");
        const auto vp=View({2,1.4f,4},-25,0);StereoViews views;
        for(int e=0;e<2;++e)std::copy(vp.begin(),vp.end(),views.worldVP[e]);renderer.SetStereoViews(views);
        gt2::modernCarReflections=true;assets.UpdateCarReflection(slot,identity,{{{1,0,0},{0,-1,0},{0,0,-1}}});
        std::vector<DrawItem> items;assets.AppendCarItems(items,slot,identity,0,0);
        std::erase_if(items,[](const auto& d){return d.blend==2;});for(auto& item:items)item.space=kSpaceWorld;
        auto body=items;std::erase_if(body,[](const auto& d){return d.blend!=kBlendOpaque;});
        const auto bare=renderer.CaptureStereoForAudit(body,body.size());
        std::array<std::vector<DrawItem>,3> empty,closed;for(auto& face:closed)face.push_back(block);
        renderer.QueueNativeEnvironment(empty,cameras);
        const auto sky=renderer.CaptureStereoForAudit(items,items.size());
        const auto emptyMap=renderer.NativeEnvironmentForAudit();
        for(size_t p=0;p<emptyMap.size();p+=4)if(emptyMap[p] || emptyMap[p+1] || emptyMap[p+2] || emptyMap[p+3])throw std::runtime_error("empty environment must retain black RGB and zero coverage");
        renderer.QueueNativeEnvironment(closed,cameras);
        const auto wallImage=renderer.CaptureStereoForAudit(items,items.size());
        const auto blackMap=renderer.NativeEnvironmentForAudit();
        for(size_t p=0;p<blackMap.size();p+=4)if(blackMap[p] || blackMap[p+1] || blackMap[p+2] || blackMap[p+3]!=255)throw std::runtime_error("opaque black scenery must retain full coverage");
        size_t changed=0,wallDelta=0;
        for(size_t p=0;p<sky.size();p+=4){
            int difference=0;for(size_t c=0;c<3;++c){difference+=std::abs(int(sky[p+c])-int(wallImage[p+c]));wallDelta+=std::abs(int(wallImage[p+c])-int(bare[p+c]))>2;}
            changed+=difference>8;
        }
        csv<<id<<','<<(gt4?"gt4":"gt2")<<','<<changed<<','<<wallDelta<<'\n';csv.flush();
        if(gt4 ? changed!=0 : changed<50)throw std::runtime_error("sky coverage does not distinguish GT2 from native GT4");
        // The capture covers directions inside its three native frusta; the
        // far sky outside those frusta is intentionally still available.
        gt2::WritePngRgba((out/(std::string(id)+(gt4?"-gt4":"-gt2")+"-sky.png")).string(),192,384,sky);
        gt2::WritePngRgba((out/(std::string(id)+(gt4?"-gt4":"-gt2")+"-wall.png")).string(),192,384,wallImage);
    }
    std::cout<<"PASS: native capture coverage, visible GT2 sky, unchanged GT4 background\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
