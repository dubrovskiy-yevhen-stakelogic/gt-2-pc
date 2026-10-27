// Owned-data corpus: every mapped GT4 car, eight outside angles and the cockpit.
// Capture completion is separate from visual/PS2 reference acceptance.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2formats/model_set.h"
#include <iostream>
#include <set>

int main(int argc,char** argv) {
    try {
        if(argc<4)throw std::runtime_error("gt2gt4visual disc pack output-under-work [ids ...]");
        const auto output=std::filesystem::absolute(argv[3]).lexically_normal();
        if(!output.string().starts_with(std::filesystem::absolute("work").string()+std::string(1,std::filesystem::path::preferred_separator)))
            throw std::runtime_error("output must be under work");
        std::filesystem::create_directories(output/"frames");
        gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];
        const auto manifest=gt2::json::ReadFile((std::filesystem::path(argv[2])/"manifest.json").string());
        const auto& cars=manifest.Require("cars");
        std::set<std::string> selected;
        for(int i=4;i<argc;++i)selected.insert(argv[i]);
        Device device;gt2view::VkContext context(device.existing);
        gt2view::VkSceneRenderer renderer(context,{640,640},VK_FORMAT_R8G8B8A8_UNORM);
        renderer.clearColor[0]=.10f;renderer.clearColor[1]=.70f;renderer.clearColor[2]=.90f;
        gt2view::RenderOptions options;options.msaa=1;options.smoothTextures=true;
        renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
        gt2view::ProceduralCockpit cockpit(renderer);
        auto report=gt2::json::Value::Array();size_t failures=0,index=0;
        for(const auto& [id,entry]:cars.Members()) {
            if(!selected.empty() && !selected.contains(id))continue;
            auto result=gt2::json::Value::Object();result.Set("id",gt2::json::Value::String(id));
            result.Set("name",gt2::json::Value::String(entry.Require("name").AsString()));
            result.Set("mesh",gt2::json::Value::String(entry.Require("mesh").AsString()));
            result.Set("gt4Model",gt2::json::Value::String(entry.Require("gt4Model").AsString()));
            try {
                renderer.ResetModelAtlasForAudit();
                gt2view::SceneAssets assets(renderer,vol,!std::getenv("GT2_AUDIT_PREVIEW"));assets.SetGt4Enabled(true);
                const auto loadStarted=std::chrono::steady_clock::now();
                const int slot=assets.UseCar(id);
                result.Set("loadMilliseconds",gt2::json::Value::Double(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-loadStarted).count()));
                if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("GT4 body did not load");
                const auto model=gt2::ReadGltf((std::filesystem::path(argv[2])/entry.Require("mesh").AsString()).string());
                const float length=model.boundsMax[2]-model.boundsMin[2];
                const float radius=std::max(4.5f,length*1.5f);
                const float targetY=(model.boundsMax[1]+model.boundsMin[1])*.45f;
                auto images=gt2::json::Value::Array();
                for(int pose=0;pose<8;++pose) {
                    constexpr float pi=3.14159265358979323846f;
                    const float yaw=float(pose)*45;
                    const std::array<float,3> eye{-std::sin(yaw*pi/180)*radius,targetY+radius*.24f,std::cos(yaw*pi/180)*radius};
                    auto matrix=View(eye,yaw,-13.5f);
                    // 35 degree lens, instead of the 95 degree cockpit lens.
                    const float zoom=std::tan(95.f*pi/360)/std::tan(35.f*pi/360);
                    for(size_t k=0;k<4;++k){matrix[k*4]*=zoom;matrix[k*4+1]*=zoom;}
                    std::vector<gt2view::DrawItem> items;
                    assets.AppendCarItems(items,slot,matrix.data(),0,0);
                    if(std::getenv("GT2_AUDIT_NO_GLOSS"))std::erase_if(items,[](const auto& item){return (item.blend==1 || item.blend==gt2view::kBlendStockSurfaceEnvironment);});
                    if(std::getenv("GT2_AUDIT_NO_WHEELS"))std::erase_if(items,[](const auto& item){return item.firstVertex<gt2view::SceneRenderer::kModelSetVertexBase && item.blend!=2;});
                    const std::string file=id+"-outside-"+std::to_string(pose)+".png";
                    renderer.Draw(items,(output/"frames"/file).string(),items.size());renderer.WaitFrame();
                    images.Push(gt2::json::Value::String(file));
                }
                if(!std::getenv("GT2_AUDIT_PREVIEW")) {
                const auto fit=assets.SlotCockpitFit(slot);auto eye=fit.eye;eye[2]+=.15f;
                const auto matrix=View(eye,0,-8);
                std::vector<gt2view::DrawItem> items;
                assets.AppendCockpitBodyItems(items,slot,matrix.data(),0);
                if(!assets.HasNativeGt4Interior(slot))cockpit.Append(items,fit,matrix.data(),0,0,850,1,true,0,.15f);
                renderer.Draw(items,(output/"frames"/(id+"-cockpit.png")).string(),items.size());renderer.WaitFrame();
                }
                result.Set("images",std::move(images));
                result.Set("exteriorVertices",gt2::json::Value::Int(assets.Gt4ExteriorVertices(slot)));
                result.Set("reflectionVertices",gt2::json::Value::Int(assets.Gt4ReflectionVertices(slot)));
                result.Set("authoredExteriorVertices",gt2::json::Value::Int(int64_t(model.TriangleCount()*3)));
                result.Set("removedExteriorSquareMetres",gt2::json::Value::Double(assets.Gt4ExteriorRemovedArea(slot)));
                if(std::getenv("GT2_AUDIT_REQUIRE_INTACT") && (std::abs(assets.Gt4ExteriorRemovedArea(slot))>1e-6 ||
                    assets.Gt4ExteriorVertices(slot)!=model.TriangleCount()*3))
                    throw std::runtime_error("runtime removed authored exterior surface");
                result.Set("captureComplete",gt2::json::Value::Bool(true));
                result.Set("ps2Compared",gt2::json::Value::Bool(false));
            }catch(const std::exception& e){++failures;result.Set("captureComplete",gt2::json::Value::Bool(false));result.Set("error",gt2::json::Value::String(e.what()));std::cerr<<id<<": "<<e.what()<<'\n';}
            report.Push(std::move(result));
            if(index++%20==0){gt2::json::WriteFile((output/"report.json").string(),report);std::cout<<"Captured "<<index<<"/"<<cars.Size()<<", failures "<<failures<<std::endl;}
        }
        gt2::json::WriteFile((output/"report.json").string(),report);
        std::cout<<"Captured "<<report.Size()<<" cars, failures "<<failures<<'\n';return failures?1:0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
