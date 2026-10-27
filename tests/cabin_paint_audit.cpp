// GPU regressions over every selectable stock/replacement car. No headset or
// user settings are used. Geometry and images, rather than loading success,
// determine the result. Failure captures remain under work for inspection.
#include "offscreen_car_audit.h"
#include "car_audit_catalogue.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto ids=AuditCarCatalogue(vol,argc,argv);gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    Device device;VkContext context(device.existing);constexpr uint32_t size=256;
    VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::ofstream csv(out/"audit.csv");csv<<"car,path,native,rim,eye_x,wheel_x,removed_wheel_vertices,cabin_median,black_fraction,sky_chroma_delta,failures\n";
    size_t paths=0,failures=0;
    const auto cameras=NativeEnvironmentCameras({0,0,0});std::array<std::vector<DrawItem>,3> empty;
    for(const auto& id:ids)for(bool gt4:{false,true}){
        if(gt4 && !pack.Has(id))continue;
        if(!gt4 && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(gt4);const int slot=assets.UseCar(id);
        if(slot<0 || (gt4 && !assets.UsesGt4(slot)))throw std::runtime_error("missing model "+id);
        const auto fit=assets.SlotCockpitFit(slot);if(!fit.valid)throw std::runtime_error("invalid fit "+id);
        size_t errors=0;const bool native=assets.HasNativeGt4Interior(slot),rim=fit.nativeWheel[3]>0;
        if(native && rim && std::abs(fit.eye[0]-fit.nativeWheel[0])>.02f)++errors;
        if(native && !rim && std::abs(fit.eye[0])>.05f)++errors; // central dual-stick concepts have no round rim
        std::vector<DrawItem> authored,items;
        assets.AppendCockpitBodyItems(authored,slot,identity,0);
        assets.AppendCockpitBodyItems(items,slot,identity,0,nullptr,true);
        const auto count=[](const auto& list){size_t n=0;for(const auto& d:list)n+=d.vertexCount;return n;};
        const auto removed=count(authored)-count(items);
        if((native && rim) ? removed<12 : removed!=0)++errors;
        if(std::getenv("GT2_AUDIT_NO_LAMPS")) {
            std::vector<DrawItem> lamps;assets.AppendGt4LampItems(lamps,slot,identity,0,true);
            std::erase_if(items,[&](const auto& d){return std::any_of(lamps.begin(),lamps.end(),[&](const auto& l){return d.firstVertex==l.firstVertex;});});
        }
        ProceduralCockpit cabin(renderer);
        cabin.Append(items,fit,identity,0,0,0,1,false,0,gt2game::CockpitSeatBack({}),nullptr,!native);
        if(rim)for(size_t k=0;k<3;++k)if(std::abs(cabin.WheelCenter()[k]-fit.nativeWheel[k])>.001f)++errors;
        for(auto& item:items)item.space=kSpaceWorld;SortModelTransparency(items);
        gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,{});
        StereoViews views;for(int e=0;e<2;++e){auto vp=View({camera.eye[0]+(e?1:-1)*.032f,camera.eye[1],camera.eye[2]},0,-15);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
        renderer.SetStereoViews(views);renderer.clearColor[0]=.05f;renderer.clearColor[1]=.8f;renderer.clearColor[2]=1.f;
        renderer.SetNativeSceneLighting(NativeSceneLighting{});
        if(std::getenv("GT2_AUDIT_GEOMETRY")) {
            const auto name=id+(gt4?"-gt4":"-gt2");std::ofstream f(out/(name+"-geometry.csv"));f<<"x,y,z,flags,first\n";
            for(const auto& d:items)for(const auto& v:renderer.VerticesForAudit(d)) {
                for(size_t k=0;k<3;++k)f<<d.mvp[12+k]+d.mvp[k]*v.pos[0]+d.mvp[k+4]*v.pos[1]+d.mvp[k+8]*v.pos[2]<<',';
                f<<v.flags<<','<<d.firstVertex<<'\n';
            }
            std::ofstream e(out/(name+"-eye.txt"));e<<camera.eye[0]<<' '<<camera.eye[1]<<' '<<camera.eye[2];
        }
        const auto rgba=renderer.CaptureStereoForAudit(items,items.size());
        std::vector<int> values;size_t black=0;
        for(int e=0;e<2;++e)for(uint32_t y=size/2;y<size*9/10;++y)for(uint32_t x=size/5;x<size*4/5;++x){
            const size_t p=((size_t(e)*size+y)*size+x)*4;
            if(rgba[p]<30 && rgba[p+1]>195 && rgba[p+2]>245)continue;
            const int l=(54*rgba[p]+183*rgba[p+1]+19*rgba[p+2])/256;values.push_back(l);black+=l<5;
        }
        std::sort(values.begin(),values.end());const int median=values.empty()?0:values[values.size()/2];
        const double blackFraction=values.empty()?1:double(black)/double(values.size());
        if(median<10 || blackFraction>.45)++errors;
        // Equal-luminance grey/blue skies isolate tint from reflection strength.
        // Real opaque scenery stays in the separate coverage test.
        auto vp=View({2,1.4f,4},-25,0);for(int e=0;e<2;++e)std::copy(vp.begin(),vp.end(),views.worldVP[e]);renderer.SetStereoViews(views);
        renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=0;
        gt2::modernCarReflections=true;assets.UpdateCarReflection(slot,identity,{{{1,0,0},{0,-1,0},{0,0,-1}}});
        items.clear();assets.AppendCarItems(items,slot,identity,0,0);std::erase_if(items,[](const auto& d){return d.blend==2;});
        for(auto& d:items)d.space=kSpaceWorld;SortModelTransparency(items);
        NativeSceneLighting light;light.stockSky={.12f,.38f,.85f,1};renderer.SetNativeSceneLighting(light);renderer.QueueNativeEnvironment(empty,cameras);
        const auto blue=renderer.CaptureStereoForAudit(items,items.size());
        const float l=.12f*.2126f+.38f*.7152f+.85f*.0722f;light.stockSky={l,l,l,1};renderer.SetNativeSceneLighting(light);
        const auto grey=renderer.CaptureStereoForAudit(items,items.size());int chroma=0;
        for(size_t p=0;p<blue.size();p+=4)chroma=std::max(chroma,std::abs((int(blue[p+2])-int(blue[p+1]))-(int(grey[p+2])-int(grey[p+1]))));
        if(chroma>(gt4?0:8))++errors;
        csv<<id<<','<<(gt4?"gt4":"gt2")<<','<<native<<','<<rim<<','<<fit.eye[0]<<','<<fit.nativeWheel[0]<<','<<removed<<','<<median<<','<<blackFraction<<','<<chroma<<','<<errors<<'\n';csv.flush();
        if(errors || std::getenv("GT2_AUDIT_CAPTURE")){
            const auto name=id+(gt4?"-gt4":"-gt2");
            gt2::WritePngRgba((out/(name+"-cabin.png")).string(),size,size*2,rgba);
            gt2::WritePngRgba((out/(name+"-paint.png")).string(),size,size*2,blue);
        }
        failures+=errors;if(++paths%20==0)std::cout<<paths<<" paths, "<<failures<<" failures\n"<<std::flush;
    }
    std::cout<<paths<<" paths, "<<failures<<" failures\n";return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
