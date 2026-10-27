// Real owned course/cars, fixed cameras and native stereo GPU timings; no XR session.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2view/menu_view.h"
#include "gt2formats/car_info.h"
#include <chrono>
#include <thread>
#include <fstream>
#include <iostream>
using namespace gt2view;
namespace {
std::array<float,16> Mul(const float* a,const float* b) {
    std::array<float,16> c{};
    for(int x=0;x<4;++x)for(int y=0;y<4;++y)for(int k=0;k<4;++k)c[x*4+y]+=a[k*4+y]*b[x*4+k];
    return c;
}
}
int main(int argc,char** argv) {try {
    if(argc!=4)throw std::runtime_error("disc pack output-under-work");
    const auto output=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=output.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(output);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    Device device;VkContext context(device.existing);
    VkSceneRenderer renderer(context,{512,512},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);
    renderer.EnableDecodedTextures(true);renderer.SetFoveation(2);
    const float renderScale=std::getenv("GT2_AUDIT_RENDER_SCALE")?std::strtof(std::getenv("GT2_AUDIT_RENDER_SCALE"),nullptr):1.f;
    if(renderScale<.5f || renderScale>2.f)throw std::runtime_error("render scale out of range");
    renderer.CreateStereoTarget({uint32_t(2184*renderScale),uint32_t(2288*renderScale)},VK_FORMAT_R8G8B8A8_UNORM,true);
    // The production bug accumulates while browsing, before the race starts.
    {MenuCarView menu(renderer,vol);for(const auto* id:{"dvpgn","an16n","n24vn","k2zzn","bmmgn","gv4gn","umcon","brmcn","fcx8n","gotin"}) {
        if(!menu.Use(gt2::PackCarId(id)))throw std::runtime_error("menu warmup failed");
        gt2::MenuItem viewport;viewport.x0=32;viewport.y0=190;viewport.x1=420;viewport.y1=448;
        const auto projection=gt2::menu::MenuCarProject(viewport,gt2::menu::MenuCarCamera{},true);
        std::vector<DrawItem> items;menu.Append(items,projection,0,4.f/3.f,false);renderer.Draw(items);renderer.WaitFrame();}}
    std::string trackName=std::getenv("GT2_AUDIT_TRACK")?std::getenv("GT2_AUDIT_TRACK"):"roma_night";
    if(trackName=="find-rock") {
        bool found=false;
        const auto courses=gt2::ParseCourseInfo(vol.Read(".crsinfo"));
        for(const auto& e:vol.Files())if(e.path.starts_with("crsobj/") && e.path.find(".tro")!=std::string::npos) {
            const auto name=e.path.substr(7,e.path.find(".tro")-7);
            const int row=courses.FindByFileName(name);
            if(row>=0 && courses.entries[size_t(row)].name.find("Red Rock")!=std::string::npos) {trackName=name;found=true;break;}
        }
        if(!found)throw std::runtime_error("rock course missing from owned disc");
    }
    const auto track=gt2::ParseTrack(vol.Read("crsobj/"+trackName+".tro"));
    SceneAssets assets(renderer,vol);assets.UseTrack(trackName,track);assets.SetGt4Enabled(true);
    // Follow the owned course line across actual roof boundaries. Classification
    // and cached map selection must agree in the same frame on entry and exit.
    std::ofstream coverage(output/"coverage.csv");coverage<<"track,chunk,step,covered,probe,selected_covered\n";
    size_t samples=0,transitions=0,coveredSamples=0;bool previous=false;
    for(size_t i=0;i<track.chunks.size();++i) {
        const auto& a=track.chunks[i].origin;const auto& b=track.chunks[(i+1)%track.chunks.size()].origin;
        const float dx=float((double(b[0])-a[0])/65536.),dz=float((double(b[2])-a[2])/65536.);
        const int steps=std::max(1,int(std::ceil(std::sqrt(dx*dx+dz*dz)/2.f)));
        for(int step=0;step<steps;++step) {
            std::array<float,3> eye{};for(size_t k=0;k<3;++k)eye[k]=float((a[k]+(double(b[k])-a[k])*step/steps)/65536.);eye[1]+=.8f;
            const bool covered=assets.PaintCoveredForAudit(eye);
            if(!samples || covered!=previous) {
                assets.UpdatePaintProbe(eye);const bool selected=assets.SelectedPaintProbeCoveredForAudit();
                coverage<<trackName<<','<<i<<','<<step<<','<<covered<<','<<assets.PaintProbeKeyForAudit()<<','<<selected<<'\n';
                if(covered!=selected)throw std::runtime_error("roof boundary selects opposite cached environment");
                if(samples)++transitions;
            }
            previous=covered;++samples;if(covered)++coveredSamples;
        }
    }
    std::cout<<"Coverage "<<trackName<<": "<<samples<<" samples, "<<coveredSamples<<" covered, "<<transitions<<" immediate transitions\n";
    if(std::getenv("GT2_AUDIT_COVERAGE_ONLY"))return 0;
    std::vector<int> slots;for(const auto* id:{"bmmgn","gv4gn","umcon","brmcn","fcx8n","gotin"})slots.push_back(assets.UseCar(id));
    for(int slot:slots)if(slot<0)throw std::runtime_error("race load failed");
    if(!assets.UsesGt4(slots[0]))throw std::runtime_error("MGF GT4 load failed");
    ProceduralCockpit cockpit(renderer);
    std::ofstream csv(output/"timings.csv");csv<<"mode,chunk,repeat,gpu_ms,wait_ms,draws,vertices,cached,uncached,environment_gpu_ms\n";
    bool failed=false;
    const int repeats=std::getenv("GT2_AUDIT_REPEATS")?std::atoi(std::getenv("GT2_AUDIT_REPEATS")):12;
    for(const std::string mode:{"cockpit-last","cockpit-first","chase"})for(size_t ci=0;ci<track.chunks.size();ci+=8) {
        const auto& chunk=track.chunks[ci];
        const float yaw=std::atan2(float(chunk.direction[0]),-float(chunk.direction[2]));
        std::array<float,16> model{};model[0]=model[10]=std::cos(yaw);model[2]=-std::sin(yaw);model[8]=std::sin(yaw);model[5]=model[15]=1;
        for(int k=0;k<3;++k)model[12+k]=float(chunk.origin[k]/65536.);
        const auto& fit=assets.SlotCockpitFit(slots[0]);
        const std::array<float,3> local=mode=="chase"?std::array<float,3>{0,2,6}:fit.eye;
        std::array<float,3> eye{};for(int k=0;k<3;++k)eye[k]=model[12+k]+model[k]*local[0]+model[4+k]*local[1]+model[8+k]*local[2];
        std::array<float,16> vp{};vp[0]=vp[5]=vp[10]=vp[15]=1;
        for(size_t k=0;k<3;++k)vp[12+k]=-eye[k];
        const auto carVp=Mul(vp.data(),model.data());
        StereoViews views;for(int e=0;e<2;++e) {
            std::array<float,3> shifted{};shifted[0]=(e?1:-1)*.032f*model[0];shifted[2]=(e?1:-1)*.032f*model[2];
            const auto m=View(shifted,-yaw*180.f/3.14159265359f,0),sky=View({0,0,0},-yaw*180.f/3.14159265359f,0);
            std::copy(m.begin(),m.end(),views.worldVP[e]);std::copy(sky.begin(),sky.end(),views.skyVP[e]);
        }
        renderer.SetStereoViews(views);
        assets.UpdatePaintProbe(eye);
        std::vector<DrawItem> items,blend,cabin;
        float skyModel[16]{};const float origin[3]{};SceneAssets::BackdropModel(origin,skyModel);
        const auto sky=Mul(vp.data(),skyModel);assets.AppendBackdropItems(items,blend,sky.data());
        for(auto& item:items)item.space=kSpaceSky;for(auto& item:blend)item.space=kSpaceSky;
        SceneAssets::TrackView view;view.eye=eye;view.forward={-model[8],0,-model[10]};view.right={model[0],0,model[2]};view.extendedDistance=-1;view.maxDetail=true;
        const auto world=items.size(),worldBlend=blend.size();assets.AppendTrackItems(items,blend,vp.data(),view);
        for(size_t i=1;i<slots.size();++i) {auto other=model;other[12]+=float((i&1)?-3:3);other[14]-=float(i*8);const auto m=Mul(vp.data(),other.data());assets.AppendCarItems(items,slots[i],m.data(),0,0);}
        if(mode!="chase") {
            assets.AppendCockpitBodyItems(cabin,slots[0],carVp.data(),0);
            cockpit.Append(cabin,fit,carVp.data(),0,80,4000,3,true,0,0,nullptr,!assets.HasNativeGt4Interior(slots[0]));
        } else assets.AppendCarItems(items,slots[0],carVp.data(),0,0);
        for(size_t i=world;i<items.size();++i)items[i].space=kSpaceWorld;
        for(size_t i=worldBlend;i<blend.size();++i)blend[i].space=kSpaceWorld;
        for(auto& item:cabin)item.space=kSpaceWorld;
        if(mode=="cockpit-first")items.insert(items.begin(),cabin.begin(),cabin.end());
        else items.insert(items.end(),cabin.begin(),cabin.end());
        SortModelTransparency(items);
        items.insert(items.end(),blend.begin(),blend.end());
        if(std::getenv("GT2_AUDIT_NO_REFLECTIONS"))items.erase(std::remove_if(items.begin(),items.end(),[](const auto& item){return NativeEnvironmentBlend(item.blend);}),items.end());
        std::copy(assets.SkyColor().begin(),assets.SkyColor().end(),renderer.clearColor);
        auto frameTime=std::chrono::steady_clock::now();
        const int pace=std::getenv("GT2_AUDIT_FRAME_US")?std::atoi(std::getenv("GT2_AUDIT_FRAME_US")):0;
        for(int r=0;r<repeats;++r) {
            if(pace>0) {std::this_thread::sleep_until(frameTime);frameTime+=std::chrono::microseconds(pace);}
            const auto start=std::chrono::steady_clock::now();
            const bool capture=r==repeats-1 && !std::getenv("GT2_AUDIT_NO_CAPTURE");
            if(!std::getenv("GT2_AUDIT_STATIC_ENV"))assets.UpdatePaintProbe({model[12],model[13]+.8f,model[14]});
            renderer.DrawStereo(items,capture?(output/(mode+"-"+std::to_string(ci)+".png")).string():"",items.size());renderer.WaitFrame();
            const double wait=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
            size_t vertices=0;for(const auto& item:items)vertices+=item.vertexCount;
            csv<<mode<<','<<ci<<','<<r<<','<<renderer.GpuMilliseconds()<<','<<wait<<','<<items.size()<<','<<vertices<<','<<renderer.CachedMaterials()<<','<<renderer.UncachedMaterials()<<','<<renderer.EnvironmentGpuMilliseconds()<<'\n';
            if(r>2 && !capture && (wait>100 || renderer.GpuMilliseconds()<0))failed=true;
        }
    }
    return failed?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
