#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try {
    if(argc!=4)throw std::runtime_error("disc pack output-under-work");
    const auto output=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=output.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(output);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{1024,768},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    std::ofstream report(output/"draws.csv");report<<"car,brake,draws,vertices\n";
    for(const auto* id:{"dvpgn","an16n","bmmgn","gv4gn","brmcn"}) {
        SceneAssets assets(renderer,vol,false);assets.SetGt4Enabled(true);const int slot=assets.UseCar(id);
        if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("GT4 model failed to load");
        if(assets.Gt4ExteriorRemovedArea(slot)>1e-7)throw std::runtime_error("exterior geometry removed");
        const auto vp=View({0,1.1f,4.2f},0,0);
        size_t offVertices=0;
        for(uint32_t brake:{0u,1u}) {
            std::vector<DrawItem> items;assets.AppendCarItems(items,slot,vp.data(),0,brake);
            size_t vertices=0;for(const auto& item:items)vertices+=item.vertexCount;
            if(!brake)offVertices=vertices;
            if(brake && vertices<=offVertices)throw std::runtime_error(std::string(id)+" native brake branch absent");
            renderer.Draw(items,(output/(std::string(id)+(brake?"-brake-on.png":"-brake-off.png"))).string(),items.size());renderer.WaitFrame();
            report<<id<<','<<brake<<','<<items.size()<<','<<vertices<<'\n';
        }
    }
    gt2::gt4ModelSetEnabled=false;gt2::modernCarReflections=true;
    SceneAssets stock(renderer,vol,false);const int slot=stock.UseCar("dvpgn");
    const float world[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    stock.UpdateCarReflection(slot,world,{{{1,0,0},{0,-1,0},{0,0,-1}}});
    auto vp=View({2,1.4f,4},-25,0);std::vector<DrawItem> items;stock.AppendCarItems(items,slot,vp.data(),0,1);
    renderer.Draw(items,(output/"gt2-modern.png").string(),items.size());renderer.WaitFrame();
    std::cout<<"GT4 body geometry preserved; original brake callback visible for five cars; modern GT2 shader rendered\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
