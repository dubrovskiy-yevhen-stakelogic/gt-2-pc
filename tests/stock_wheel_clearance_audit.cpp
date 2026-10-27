// Owned-data stock wheel articulation captures; production geometry in stereo.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<4)throw std::runtime_error("disc output-under-work car...");
    if(!std::getenv("GT2_AUDIT_WHEEL_COLOR"))throw std::runtime_error("GT2_AUDIT_WHEEL_COLOR=1 required for the pixel gate");
    const auto out=std::filesystem::absolute(argv[2]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    const char* pack=std::getenv("GT2_AUDIT_GT4_ROOT");
    if(pack){gt2::gt4ModelSetRoot=pack;gt2::gt4ModelSetEnabled=true;}
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{512,512},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({512,512},VK_FORMAT_R8G8B8A8_UNORM,true);
    std::ofstream csv(out/"wheels.csv");csv<<"car,state,wheel,x,y,z,radius,width,bump_limit\n";
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    size_t leaks=0;
    std::ofstream gate(out/"leaks.csv");gate<<"car,state,pose,pixels\n";
    for(int a=3;a<argc;++a){
        const std::string id=argv[a];SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(pack!=nullptr);int slot=assets.UseCar(id);
        if(slot<0 || (pack && !assets.UsesGt4(slot)))throw std::runtime_error("stock car unavailable");
        const auto fit=assets.SlotCockpitFit(slot);ProceduralCockpit cabin(renderer);
        gt2::CarMeshOptions mesh;mesh.wheels=false;
        std::vector<std::array<float,3>> shell;for(const auto& v:gt2::BuildCarMesh(assets.SlotModel(slot),mesh))shell.push_back({v.pos[0],v.pos[1],v.pos[2]});
        for(int state=0;state<6;++state){
            const int steer=state%3-1;const float bump=state>=3?.10f:0.f;
            std::array<std::array<float,16>,4> wheels;
            for(size_t w=0;w<4;++w){auto centre=assets.SlotWheelCentres(slot)[w];centre[1]+=bump;
                gt2::WheelModelMatrix(int(w),centre.data(),int16_t(w<2?steer*384:0),0,512,wheels[w].data());
                const auto& well=fit.wheelWells[w];csv<<id<<','<<state<<','<<w<<','<<centre[0]<<','<<centre[1]<<','<<centre[2]<<','<<well[3]<<','<<well[4]<<','<<ModelWheelBumpStop(shell,well)<<'\n';
            }
            for(int pose=0;pose<4;++pose){
                std::vector<DrawItem> items;auto eye=fit.eye;float yaw=0,pitch=-35;
                if(pose<2){assets.AppendCarItems(items,slot,identity,0,0,true,&wheels);eye=pose==0?std::array<float,3>{3,.4f,0}:std::array<float,3>{2,1.5f,-2.5f};yaw=pose==0?-90.f:-140.f;pitch=pose==0?0.f:-15.f;}
                else{assets.AppendCockpitBodyItems(items,slot,identity,0,&wheels);cabin.Append(items,fit,identity,0,0,0,1);yaw=pose==2?0.f:-70.f;}
                for(auto& i:items)i.space=kSpaceWorld;
                StereoViews views;for(int e=0;e<2;++e){auto p=eye;p[0]+=(e?1:-1)*.032f;auto vp=View(p,yaw,pitch);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
                renderer.SetStereoViews(views);std::fill_n(renderer.clearColor,3,.18f);
                auto rgba=renderer.CaptureStereoForAudit(items,items.size());
                std::vector<DrawItem> wheelItems;assets.AppendCarWheels(wheelItems,slot,identity,0,&wheels);
                auto panels=items;std::erase_if(panels,[&](const auto& i){return std::any_of(wheelItems.begin(),wheelItems.end(),[&](const auto& w){return i.firstVertex==w.firstVertex;});});
                const auto shellImage=renderer.CaptureStereoForAudit(panels,panels.size());
                size_t overlap=0;
                if(std::getenv("GT2_AUDIT_WHEEL_COLOR"))for(int y=2;y<1022;++y)for(int x=2;x<510;++x){
                    const size_t pixel=size_t(y*512+x)*4;
                    if(rgba[pixel]<230 || rgba[pixel+1]>16 || rgba[pixel+2]<230)continue;
                    bool solid=true;
                    for(int dy=-2;dy<=2;++dy)for(int dx=-2;dx<=2;++dx){const size_t at=size_t((y+dy)*512+x+dx)*4;
                        if(std::max({shellImage[at],shellImage[at+1],shellImage[at+2]})<80)solid=false;}
                    if(solid)++overlap;
                }
                gate<<id<<','<<state<<','<<pose<<','<<overlap<<'\n';leaks+=overlap;
                gt2::WritePngRgba((out/(id+"-"+std::to_string(state)+"-"+std::to_string(pose)+".png")).string(),512,1024,rgba);
            }
        }
        std::cout<<id<<" captured"<<std::endl;
    }
    std::cout<<"Wheel pixels crossing solid body: "<<leaks<<std::endl;
    return leaks?2:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
