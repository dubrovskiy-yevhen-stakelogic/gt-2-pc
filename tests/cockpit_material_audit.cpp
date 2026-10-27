// Actual cockpit reflection and day/night lamp submissions on owned Arcade cars.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/car_info.h"
#include "gt2export/png_writer.h"
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<4)throw std::runtime_error("arcade disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    std::vector<std::string> ids;
    if(argc>4)ids={argv+4,argv+argc};else {auto data=gt2::ArcadeData::Load(vol);for(size_t i=0;i<data.PlayerCarCount();++i)ids.push_back(gt2::UnpackCarId(data.PlayerCarId(i)));}
    constexpr uint32_t size=256;Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::vector<SceneVertex> wall;
    for(auto xy:std::array<std::array<float,2>,6>{{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}}}){SceneVertex v{};v.pos[0]=xy[0];v.pos[1]=xy[1];v.pos[2]=.5f;std::fill_n(v.color,3,.8f);wall.push_back(v);}
    renderer.SetVertices(0,wall);DrawItem block;block.vertexCount=6;block.space=kSpaceMirrorSource;std::copy_n(identity,16,block.mvp);
    std::array<std::vector<DrawItem>,3> environment;for(auto& face:environment)face.push_back(block);
    const auto cameras=NativeEnvironmentCameras({0,0,0});
    auto render=[&](std::vector<DrawItem> items,const std::array<float,3>& eye,float yaw,float pitch){StereoViews views;
        for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;const auto vp=View(at,yaw,pitch);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
        renderer.SetStereoViews(views);for(auto& d:items)if(d.space!=kSpaceMirrorSource)d.space=kSpaceWorld;SortModelTransparency(items);return renderer.CaptureStereoForAudit(items,items.size());};
    std::ofstream csv(out/"materials.csv");csv<<"car,reflection_vertices,reflection_pixels,day_light,night_light,headlamp_expected,mirror_response,failures\n";
    size_t paths=0,failures=0;
    for(const auto& id:ids) {
        if(!pack.Has(id))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(true);const int slot=assets.UseCar(id);
        if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("missing native model "+id);
        const auto imported=pack.Read(id,false);const auto fit=assets.SlotCockpitFit(slot);
        for(auto& v:wall)std::fill_n(v.color,3,.8f);renderer.SetVertices(0,wall);
        renderer.SetNativeSceneLighting(NativeSceneLighting{});renderer.QueueNativeEnvironment(environment,cameras);
        std::vector<DrawItem> cabin;assets.AppendCockpitBodyItems(cabin,slot,identity,0);
        auto matte=cabin;std::erase_if(matte,[](const auto& d){return NativeEnvironmentBlend(d.blend) || (d.blend==1 || d.blend==gt2view::kBlendStockSurfaceEnvironment);});
        size_t reflections=0;for(const auto& d:cabin)if(NativeEnvironmentBlend(d.blend) || (d.blend==1 || d.blend==gt2view::kBlendStockSurfaceEnvironment))reflections+=d.vertexCount;
        const std::array<float,3> eye{0,fit.roofY+1.f,fit.windshieldZ+.6f};
        const auto bare=render(matte,eye,0,-65),shine=render(cabin,eye,0,-65);size_t changed=0;
        for(size_t p=0;p<bare.size();p+=4){int d=0;for(size_t c=0;c<3;++c)d+=std::abs(int(bare[p+c])-int(shine[p+c]));changed+=d>8;}
        renderer.QueueNativeEnvironment({},cameras);
        NativeSceneLighting night;night.ambient={.02f,.02f,.02f,0};night.diffuse={0,0,0,0};night.specular={0,0,0,0};renderer.SetNativeSceneLighting(night);
        std::vector<DrawItem> body,allLamps;assets.AppendCarItems(body,slot,identity,0,0);
        for(uint32_t state=0;state<6;++state)assets.AppendGt4LampItems(allLamps,slot,identity,state);
        std::erase_if(body,[&](const auto& d){return d.blend==2 || NativeEnvironmentBlend(d.blend) || (d.blend==1 || d.blend==gt2view::kBlendStockSurfaceEnvironment) || std::any_of(allLamps.begin(),allLamps.end(),[&](const auto& l){return d.firstVertex==l.firstVertex;});});
        std::array<uint64_t,2> light{};std::array<std::vector<uint8_t>,2> images;
        for(uint32_t on=0;on<2;++on){auto draws=body;assets.AppendGt4LampItems(draws,slot,identity,on*2);
            images[on]=render(draws,{0,(fit.roofY+fit.floorY)*.45f,fit.frontZ-1.5f},180,0);
            for(size_t p=0;p<images[on].size();p+=4)for(size_t c=0;c<3;++c)light[on]+=images[on][p+c];}
        bool expected=false;
        for(const auto& p:imported.lamps.primitives){const auto& m=imported.lamps.materials.at(size_t(p.material));
            if(!(m.lampStates&4) || (m.lampStates&1) || m.nativeGt4Values[12]<.1f)continue;
            for(auto index:p.indices)if(p.vertices.at(index).position[2]<-.5f)expected=true;}
        std::vector<DrawItem> mirrors;assets.AppendSideMirrorItems(mirrors,slot,identity);
        size_t mirrorResponse=0;
        if(!mirrors.empty()) {
            std::array<float,3> lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};
            for(const auto& d:mirrors)for(const auto& v:renderer.VerticesForAudit(d))for(size_t k=0;k<3;++k){lo[k]=std::min(lo[k],v.pos[k]);hi[k]=std::max(hi[k],v.pos[k]);}
            const std::array<float,3> at{(lo[0]+hi[0])*.5f,(lo[1]+hi[1])*.5f,hi[2]+std::max(1.5f,hi[0]-lo[0])};
            mirrors.insert(mirrors.begin(),block);
            std::array<std::vector<uint8_t>,2> frames;
            for(size_t phase=0;phase<2;++phase){for(auto& v:wall){std::fill_n(v.color,3,0.f);v.color[phase]=1;}renderer.SetVertices(0,wall);frames[phase]=render(mirrors,at,0,0);}
            for(size_t p=0;p<frames[0].size();p+=4)mirrorResponse+=frames[0][p]>200 && frames[0][p+1]<20 && frames[1][p]<20 && frames[1][p+1]>200;
            if(std::getenv("GT2_AUDIT_CAPTURE") || mirrorResponse<8)gt2::WritePngRgba((out/(id+"-live-mirrors.png")).string(),size,size*2,frames[1]);
        }
        const bool failed=(!imported.reflections.primitives.empty() && (!reflections || !changed)) || (expected && light[1]<=light[0]+100) || (!mirrors.empty() && mirrorResponse<8);

        csv<<id<<','<<reflections<<','<<changed<<','<<light[0]<<','<<light[1]<<','<<expected<<','<<mirrorResponse<<','<<failed<<'\n';csv.flush();failures+=failed;++paths;
        if(failed || std::getenv("GT2_AUDIT_CAPTURE")){
            gt2::WritePngRgba((out/(id+"-reflections.png")).string(),size,size*2,shine);
            gt2::WritePngRgba((out/(id+"-matte.png")).string(),size,size*2,bare);
            for(int on=0;on<2;++on)gt2::WritePngRgba((out/(id+(on?"-night.png":"-day.png"))).string(),size,size*2,images[on]);}
        std::cout<<id<<" materials "<<(failed?"FAIL":"PASS")<<'\n'<<std::flush;
    }
    std::ofstream(out/"gate.json")<<"{\"paths\":"<<paths<<",\"failures\":"<<failures<<"}\n";return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
