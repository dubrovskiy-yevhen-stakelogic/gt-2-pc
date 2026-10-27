// Optional owned-data audit: every expansion car uses the production renderer.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2formats/model_set.h"
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
    try {
        if(argc<4 || argc>5)throw std::runtime_error("gt2expansionrender disc model-set output-under-work [id]");
        const auto output=std::filesystem::absolute(argv[3]).lexically_normal();
        const auto rel=output.lexically_relative(std::filesystem::absolute("work").lexically_normal());
        if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
        std::filesystem::create_directories(output/"frames");
        gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];
        const auto bytes=vol.Read("carparam/gt4_expansion.json");
        const auto doc=gt2::json::Parse(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size()));
        const auto& cars=doc.Require("cars");
        Device device;gt2view::VkContext context(device.existing);
        gt2view::VkSceneRenderer renderer(context,{320,320},VK_FORMAT_R8G8B8A8_UNORM);
        gt2view::RenderOptions options;options.msaa=1;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
        const auto checker=Checker(renderer);gt2view::ProceduralCockpit cockpit(renderer);
        auto report=gt2::json::Value::Array();int failures=0;
        for(size_t i=0;i<cars.Size();++i) {
            const auto id=cars.At(i).Require("id").AsString();if(argc==5 && id!=argv[4])continue;
            auto result=gt2::json::Value::Object();result.Set("id",gt2::json::Value::String(id));
            try {
                gt2view::SceneAssets assets(renderer,vol); // fresh slots and texture cache per car
                const int slot=assets.UseCar(id); // replacement toggle deliberately remains OFF
                if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("mandatory GT4 model did not load");
                if(argc==5) {
                    const int ghost=assets.UseCar(id,"ghost:"+id);
                    if(ghost<0 || !assets.UsesGt4(ghost))throw std::runtime_error("expansion ghost lost its GT4 body");
                }
                const auto fit=assets.SlotCockpitFit(slot);
                auto eye=fit.eye;eye[2]+=.15f;
                for(int pose=0;pose<3;++pose) {
                    auto matrix=pose==0?View({3.4f,2.2f,4.8f},-35,-16):View(eye,pose==1?0.f:75.f,pose==1?-8.f:-20.f);
                    std::vector<gt2view::DrawItem> items{checker};
                    if(pose==0)assets.AppendCarItems(items,slot,matrix.data(),0,0);
                    else {
                        assets.AppendCockpitBodyItems(items,slot,matrix.data(),0);
                        if(!assets.HasNativeGt4Interior(slot))cockpit.Append(items,fit,matrix.data(),0,0,850,1,true,0,.15f);
                    }
                    renderer.Draw(items,(output/"frames"/(id+"-"+std::to_string(pose)+".png")).string(),items.size());renderer.WaitFrame();
                }
                result.Set("passed",gt2::json::Value::Bool(true));
            }catch(const std::exception& e){++failures;result.Set("error",gt2::json::Value::String(e.what()));std::cerr<<id<<": "<<e.what()<<'\n';}
            report.Push(std::move(result));gt2::json::WriteFile((output/"report.json").string(),report);
            if(i%20==0)std::cout<<"Render "<<i+1<<"/"<<cars.Size()<<", failures "<<failures<<std::endl;
        }
        std::cout<<"Rendered "<<report.Size()<<" cars; failures "<<failures<<'\n';return failures?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
