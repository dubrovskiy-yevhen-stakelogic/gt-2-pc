// Exercise the actual menu car loader, slot reuse and mono rendering path.
#include "offscreen_car_audit.h"
#include "gt2view/menu_view.h"
#include "gt2formats/model_set.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_info.h"
#include "gt2formats/hd_media.h"
#include "gt2view/title_view.h"
#include "game/arcade/arcade_car_page.h"
#include "gt2vfs/gtfs.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>

int main(int argc,char** argv) {
    try {
        if(argc<5)throw std::runtime_error("menu model audit disc pack output ids ...");
        const auto output=std::filesystem::absolute(argv[3]).lexically_normal();
        if(!output.string().starts_with(std::filesystem::absolute("work").string()+std::string(1,std::filesystem::path::preferred_separator)))throw std::runtime_error("output must be under work");
        std::filesystem::create_directories(output);
        gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
        gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=std::getenv("GT2_AUDIT_STOCK")==nullptr;
        gt2::modernCarReflections=std::getenv("GT2_AUDIT_MODERN")!=nullptr;
        Device device;gt2view::VkContext context(device.existing);
        gt2view::VkSceneRenderer renderer(context,{1280,960},VK_FORMAT_R8G8B8A8_UNORM);
        gt2view::RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);
        renderer.EnableDecodedTextures(true);
        // VR car selection is mono, but the race targets already exist at boot.
        // Exercise that memory/driver condition without opening an XR session.
        if(std::getenv("GT2_AUDIT_VR_TARGETS")){
            renderer.SetFoveation(2);
            renderer.CreateStereoTarget({2184,2288},VK_FORMAT_R8G8B8A8_UNORM,true);
        }
        gt2view::MenuCarView car(renderer,vol);
        car.LoadMenuReflection();
        const bool realUi=std::getenv("GT2_AUDIT_REAL_UI")!=nullptr;
        if(realUi)car.SetFrameSize(352,480);
        std::optional<gt2::arcade::ArcadeMenuAssets> menuAssets;
        std::optional<gt2::ArcadeData> arcadeData;
        std::optional<gt2::CarInfoDirectory> carInfo;
        std::unique_ptr<gt2::arcade::ArcadeCarPage> page;
        gt2view::TitleView ui(renderer);
        gt2::MenuVram menuVram;
        gt2::arcade::TextCtx text;
        std::array<uint8_t,512> selection{};
        if(realUi){
            std::string profile;
            std::ifstream(std::filesystem::path(argv[1]).parent_path()/"hd/profile.txt")>>profile;
            gt2::hd::SetRoot(argv[1],profile);
            if(std::getenv("GT2_AUDIT_NO_HD"))gt2::hd::SetEnabled(false);
            menuAssets=gt2::arcade::ArcadeMenuAssets::Load(disc,vol);
            arcadeData=gt2::ArcadeData::Load(vol);carInfo=gt2::CarInfoDirectory::Load(vol);
            std::ofstream classS(output/"class-s.txt");for(const auto& id:menuAssets->data.classes[0].cars)classS<<id<<'\n';
            page=std::make_unique<gt2::arcade::ArcadeCarPage>(*menuAssets,*arcadeData,*carInfo);
        }
        std::vector<gt2view::SceneVertex> background;
        for(const auto& p:std::array<std::array<float,2>,6>{{{-1,-1},{1,-1},{-1,1},{-1,1},{1,-1},{1,1}}}){gt2view::SceneVertex vertex{};vertex.pos[0]=p[0];vertex.pos[1]=p[1];vertex.pos[2]=1;vertex.color[0]=vertex.color[1]=vertex.color[2]=.03f;background.push_back(vertex);}
        if(std::getenv("GT2_AUDIT_GREY_BACKGROUND"))for(auto& vertex:background)vertex.color[0]=vertex.color[1]=vertex.color[2]=.3f;
        renderer.SetVertices(gt2view::MenuView::kVertexBase,background);
        gt2::MenuItem viewport;viewport.x0=32;viewport.y0=190;viewport.x1=420;viewport.y1=448;
        const int frames=std::getenv("GT2_AUDIT_FRAMES")?std::atoi(std::getenv("GT2_AUDIT_FRAMES")):180;
        const int cycles=std::getenv("GT2_AUDIT_CYCLES")?std::atoi(std::getenv("GT2_AUDIT_CYCLES")):4;
        if(frames<6 || frames>8192 || cycles<1 || cycles>100)throw std::runtime_error("invalid audit frame/cycle count");
        auto report=gt2::json::Value::Array();
        bool passed=true;
        for(int cycle=0;cycle<cycles;++cycle)for(int i=4;i<argc;++i){
            const std::string id=argv[i];
            if(std::getenv("GT2_AUDIT_DEFER"))renderer.BeginFrameUploads();
            if(!car.Use(gt2::PackCarId(id)))throw std::runtime_error("menu load failed: "+id);
            if(realUi){
                bool found=false;
                for(size_t cls=0;cls<menuAssets->data.classes.size() && !found;++cls){
                    const auto& ids=menuAssets->data.classes[cls].cars;
                    for(size_t at=0;at<ids.size();++at)if(ids[at]==id){
                        std::vector<uint8_t> available(ids.size(),0);available[at]=1;
                        menuVram=menuAssets->vram;
                        page->Enter(int(cls),available,false,1,menuVram,text);
                        for(int warmup=0;warmup<120;++warmup){std::vector<int> sounds;page->Update(nullptr,selection.data(),sounds,menuVram,text);page->ModelTick();}
                        ui.UploadVram(menuVram);found=true;break;
                    }
                }
                if(!found || !page->CarShown() || page->CarId()!=gt2::PackCarId(id))throw std::runtime_error("real car page setup failed: "+id);
            }
            gt2::menu::MenuCarCamera camera;
            const auto started=std::chrono::steady_clock::now();
            std::ofstream timings(output/(id+"-timings-"+std::to_string(cycle)+".csv"));
            timings<<"frame,submitAndWaitMs,gpuMs\n";
            double maxWaitMs=0;
            int stalls=0,missingQueries=0;
            for(int frame=0;frame<frames;++frame){
                if(std::getenv("GT2_AUDIT_DEFER"))renderer.BeginFrameUploads();
                camera.yaw=int16_t(frame*4096/frames);
                auto projection=gt2::menu::MenuCarProject(viewport,camera,true);
                std::vector<gt2view::DrawItem> items;
                std::vector<gt2::MenuPrim> realPrims;
                size_t modelAt=0;
                if(realUi){
                    std::vector<int> sounds;page->Update(nullptr,selection.data(),sounds,menuVram,text);page->ModelTick();
                    gt2::arcade::ViewOt ot;page->Draw(ot,text);
                    gt2::arcade::AddArcadeHeader(ot.slot[4],ot.slot[6],menuAssets->fonts[0],"CAR SELECTION",0x2084B6,128);
                    std::vector<gt2::MenuPrim> prims={gt2::arcade::Tile(0,0,352,480,0)};
                    const auto floor=page->Floor();prims.insert(prims.end(),floor.begin(),floor.end());ot.Emit(prims,0);
                    modelAt=1+floor.size();realPrims=prims;
                    ui.Build(prims,352,4.f/3,items,false);projection=page->Projection(true);
                }else{
                    gt2view::DrawItem backdrop;backdrop.firstVertex=gt2view::MenuView::kVertexBase;backdrop.vertexCount=6;for(int k=0;k<4;++k)backdrop.mvp[k*5]=1;items.push_back(backdrop);
                }
                const size_t firstCar=items.size();
                const int paint=std::getenv("GT2_AUDIT_PAINT")?std::atoi(std::getenv("GT2_AUDIT_PAINT")):0;
                car.Append(items,projection,paint,4.f/3,false);
                if(items.size()==firstCar)throw std::runtime_error("menu car produced no draw items");
                items[firstCar].clearDepth=1; // the real car page clears depth after its UI background
                if(frame==0 && std::getenv("GT2_AUDIT_DRAW_STAGES")){
                    for(size_t end=firstCar+1;end<=items.size();++end){
                        const auto& d=items[end-1];
                        std::cout<<"stage "<<(end-firstCar)<<" first "<<d.firstVertex<<" count "<<d.vertexCount<<" blend "<<d.blend<<std::endl;
                        renderer.Draw(std::vector<gt2view::DrawItem>(items.begin(),items.begin()+end),(output/(id+"-stage-"+std::to_string(end-firstCar)+".png")).string(),0);
                    }
                }
                if(std::getenv("GT2_AUDIT_NO_SHADOW")){items.erase(items.begin()+firstCar);items.at(firstCar).clearDepth=1;}
                if(std::getenv("GT2_AUDIT_NO_GLOSS"))std::erase_if(items,[](const auto& item){return (item.blend==1 || item.blend==gt2view::kBlendStockSurfaceEnvironment) || gt2view::NativeEnvironmentBlend(item.blend);});
                if(std::getenv("GT2_AUDIT_NO_CAR"))items.resize(firstCar);
                if(std::getenv("GT2_AUDIT_NO_UI"))items.erase(items.begin(),items.begin()+firstCar);
                if(realUi && !std::getenv("GT2_AUDIT_NO_UI") && !std::getenv("GT2_AUDIT_OLD_UI_ORDER")) {
                    const std::vector<gt2view::DrawItem> layer(items.begin()+std::min(firstCar,items.size()),items.end());
                    items.clear();ui.Build(realPrims,352,4.f/3,items,false,modelAt,layer);
                }
                const auto shot=!std::getenv("GT2_AUDIT_NO_CAPTURE") && cycle==0 && frame%(frames/6)==0?(output/(id+"-"+std::to_string(frame)+".png")).string():std::string();
                const auto drawStarted=std::chrono::steady_clock::now();
                renderer.Draw(items,shot,0);
                if(std::getenv("GT2_AUDIT_TIMING")) {
                    renderer.WaitFrame();
                    const double waitMs=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-drawStarted).count();
                    timings<<frame<<','<<waitMs<<','<<renderer.GpuMilliseconds()<<'\n';
                    if(frame>=10 && shot.empty()){
                        maxWaitMs=std::max(maxWaitMs,waitMs);
                        if(renderer.GpuMilliseconds()<0)++missingQueries;
                        if(waitMs>100){++stalls;timings.flush();std::cout<<id<<" GPU stall frame "<<frame<<" wait_ms "<<waitMs<<" gpu_ms "<<renderer.GpuMilliseconds()<<std::endl;}
                    }
                }
                if(frame%256==0){renderer.WaitFrame();std::cout<<id<<" frame "<<frame<<" draws "<<items.size()<<" elapsed_ms "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()<<std::endl;}
                if(!std::getenv("GT2_AUDIT_NO_CAPTURE") && !std::getenv("GT2_AUDIT_NO_SCENE_CAPTURE") && cycle==0 && frame==0)renderer.SaveCapture((output/(id+".gt2cap")).string(),items,0);
                if(std::getenv("GT2_AUDIT_PACE"))std::this_thread::sleep_until(started+std::chrono::microseconds(int64_t(frame+1)*13889));
            }
            renderer.WaitFrame(); // Report success only after the final GPU fence passed.
            const bool carPassed=stalls==0 && missingQueries==0;passed=passed && carPassed;
            auto row=gt2::json::Value::Object();row.Set("id",gt2::json::Value::String(id));row.Set("cycle",gt2::json::Value::Int(cycle));row.Set("frames",gt2::json::Value::Int(frames));row.Set("stalls",gt2::json::Value::Int(stalls));row.Set("missingQueries",gt2::json::Value::Int(missingQueries));row.Set("maxWaitMs",gt2::json::Value::Double(maxWaitMs));row.Set("passed",gt2::json::Value::Bool(carPassed));report.Push(std::move(row));
            gt2::json::WriteFile((output/"report.json").string(),report);std::cout<<"Menu cycle "<<cycle<<" car "<<id<<(carPassed?" passed":" FAILED: GPU stalls")<<std::endl;
        }
        return passed?0:1;
    }catch(const std::exception& e){std::cerr<<e.what()<<std::endl;return 1;}
}
