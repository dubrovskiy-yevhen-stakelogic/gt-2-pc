// Owned-data course corpus: build the real scene and enforce its GPU budget.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include <iostream>
int main(int argc,char** argv){try{
    if(argc<3)throw std::runtime_error("disc output-under-work [course substring]");
    const auto output=std::filesystem::absolute(argv[2]).lexically_normal();
    if(!output.string().starts_with(std::filesystem::absolute("work").string()+std::string(1,std::filesystem::path::preferred_separator)))throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(output/"frames");
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);Device device;gt2view::VkContext context(device.existing);gt2view::VkSceneRenderer renderer(context,{256,256},VK_FORMAT_R8G8B8A8_UNORM);
    gt2view::RenderOptions options;options.msaa=1;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    auto report=gt2::json::Value::Array();size_t failures=0;
    for(const auto& file:vol.Files()){if(!file.path.ends_with(".tro.gz") && !file.path.ends_with(".tro"))continue;if(argc>3 && file.path.find(argv[3])==std::string::npos)continue;
        auto result=gt2::json::Value::Object();result.Set("track",gt2::json::Value::String(file.path));
        try{const auto track=gt2::ParseTrack(vol.Read(file));auto name=file.path.substr(7);name.resize(name.find(".tro"));gt2view::SceneAssets assets(renderer,vol,false);assets.UseTrack(name,track);
            result.Set("vertices",gt2::json::Value::Int(int64_t(assets.CourseVertexCount())));result.Set("droppedVertices",gt2::json::Value::Int(int64_t(assets.DroppedCourseVertices())));
            if(assets.DroppedCourseVertices())throw std::runtime_error("course geometry exceeded GPU vertex budget");
            gt2view::SceneAssets::TrackView view;for(size_t k=0;k<3;++k)view.eye[k]=float(track.startGrid[0][k]/65536.);view.eye[1]+=8;view.forward={0,-1,0};view.extendedDistance=-1;view.maxDetail=true;view.glows=false;
            const auto vp=gt2view::PaintCubeCamera(3,view.eye);std::vector<gt2view::DrawItem> opaque,blended;assets.AppendTrackItems(opaque,blended,vp.data(),view);opaque.insert(opaque.end(),blended.begin(),blended.end());
            if(opaque.empty())throw std::runtime_error("course emitted no geometry");renderer.Draw(opaque,(output/"frames"/(name+".png")).string());renderer.WaitFrame();result.Set("passed",gt2::json::Value::Bool(true));
        }catch(const std::exception& e){++failures;result.Set("passed",gt2::json::Value::Bool(false));result.Set("error",gt2::json::Value::String(e.what()));std::cerr<<file.path<<": "<<e.what()<<std::endl;}
        report.Push(std::move(result));std::cout<<"course audit "<<file.path<<std::endl;
    }
    if(report.Size()==0)throw std::runtime_error("no courses examined");gt2::json::WriteFile((output/"report.json").string(),report);return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 2;}}
