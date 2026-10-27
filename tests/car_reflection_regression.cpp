#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "car_audit_catalogue.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto ids=AuditCarCatalogue(vol,argc,argv);gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    constexpr uint32_t size=192;
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=1;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const std::array<std::array<float,3>,3> axes{{{1,0,0},{0,-1,0},{0,0,-1}}};
    // Known nonzero original reflection map. Car palettes live above these rows.
    std::vector<uint16_t> env(512*1024);
    for(int y=0;y<128;++y)for(int x=576;x<608;++x)env[size_t(y)*1024+x]=0x1111;
    env[511*1024+369]=uint16_t(12|(18<<5)|(24<<10));renderer.UploadVram(0,512,env.data());
    std::ofstream csv(out/"results.csv");csv<<"car,path,context,mode,body_changed,darkened,reflection_pixels,bad_mirror_triangles,mirror_triangles,native_cockpit\n";
    size_t failures=0,cases=0,paths=0;
    auto capture=[&](std::vector<DrawItem> items,const std::array<float,16>& vp){
        StereoViews views;for(int e=0;e<2;++e)std::copy(vp.begin(),vp.end(),views.worldVP[e]);renderer.SetStereoViews(views);
        for(auto& item:items)item.space=kSpaceWorld;
        renderer.clearColor[0]=.08f;renderer.clearColor[1]=.12f;renderer.clearColor[2]=.16f;
        return renderer.CaptureStereoForAudit(items,items.size());
    };
    for(const auto& id:ids)for(bool gt4:{false,true}){
        if(gt4 && !pack.Has(id))continue;
        if(!gt4 && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(gt4);const int slot=assets.UseCar(id);
        if(slot<0 || (gt4 && !assets.UsesGt4(slot)))throw std::runtime_error("car unavailable "+id);
        ++paths;const auto fit=assets.SlotCockpitFit(slot);
        std::vector<DrawItem> mirrors;assets.AppendSideMirrorItems(mirrors,slot,identity);
        size_t badMirrors=0,mirrorTriangles=0;
        std::vector<DrawItem> disabled;assets.AppendSideMirrorItems(disabled,slot,identity,false);
        if(!disabled.empty())throw std::runtime_error("side mirror OFF submitted geometry: "+id);
        if(gt4 && pack.HasNativeInterior(id)!=fit.nativeInterior)throw std::runtime_error("shop cockpit badge disagrees with loaded model: "+id);
        for(const auto& item:mirrors){const auto vertices=renderer.VerticesForAudit(item);mirrorTriangles+=vertices.size()/3;
            for(size_t i=0;i+2<vertices.size();i+=3){
                std::array<std::array<float,3>,3> p;for(size_t j=0;j<3;++j)std::copy_n(vertices[i+j].pos,3,p[j].begin());
                bool outside=true;for(const auto& v:p)outside&=std::abs(v[0])>=fit.halfWidth*.65f && v[1]>=fit.windshieldY-.18f;
                if(!outside || !RearFacingMirrorTriangle(p[0],p[1],p[2]))++badMirrors;
            }
        }
        if(gt4){failures+=badMirrors!=0;++cases;csv<<id<<",gt4,mirror,shared,0,0,0,"<<badMirrors<<','<<mirrorTriangles<<','<<fit.nativeInterior<<'\n';}
        else for(bool cockpit:{false,true}){
            if(cockpit && !fit.valid)continue;
            auto vp=View({2,1.4f,4},-25,0);
            if(cockpit){gt2game::CockpitSettings settings;settings.seatBackCm=-20;gt2::camera::CameraProjection camera{};
                gt2game::PlaceCockpitEye(camera,identity,fit,settings);vp=View({camera.eye[0],camera.eye[1],camera.eye[2]},0,-8);}
            const auto paletteCount=std::min(size_t(16),gt2::ParseCarTexture(vol.Read("carobj/"+id+".cdp")).paints.size());
            for(uint32_t index=0;index<32;++index) {
                std::vector<DrawItem> submitted;
                if(cockpit)assets.AppendCockpitBodyItems(submitted,slot,identity,index);else assets.AppendCarItems(submitted,slot,identity,index,0);
                for(const auto& item:submitted)if(item.blend==kBlendOpaque && item.paint!=(index<paletteCount?index:0))
                    throw std::runtime_error("paint range check failed: "+id);
            }
            uint32_t paint=0;
            auto draws=[&](){std::vector<DrawItem> result;
                if(cockpit)assets.AppendCockpitBodyItems(result,slot,identity,paint);else assets.AppendCarItems(result,slot,identity,paint,0);
                std::erase_if(result,[](const auto& d){return d.blend==2;});
                return result;};
            auto body=draws();std::erase_if(body,[](const auto& d){return d.blend!=kBlendOpaque;});
            const auto before=capture(body,vp);
            // A GT4 paint can outlive a switch to its GT2 donor. Unknown original
            // palettes must fall back to colour zero, never transparent VRAM.
            paint=31;auto stalePaint=draws();std::erase_if(stalePaint,[](const auto& d){return d.blend!=kBlendOpaque;});
            const auto stale=capture(stalePaint,vp);size_t staleDiff=0;
            for(size_t p=0;p<before.size();p+=4)for(size_t c=0;c<3;++c)staleDiff+=std::abs(int(before[p+c])-int(stale[p+c]))>2;
            failures+=staleDiff!=0;++cases;csv<<id<<",gt2,"<<(cockpit?"cockpit":"exterior")<<",stale-paint,"<<staleDiff<<",0,0,0,"<<mirrorTriangles<<",0\n";
            if(staleDiff)gt2::WritePngRgba((out/(id+(cockpit?"-cockpit":"-exterior")+"-stale-paint.png")).string(),size,size*2,stale);
            paint=0;

            for(bool modern:{false,true,false}){
                gt2::modernCarReflections=modern;
                if(cockpit)assets.UpdateCockpitReflection(slot,identity,axes);else assets.UpdateCarReflection(slot,identity,axes);
                const auto after=capture(body,vp),full=capture(draws(),vp);
                size_t changed=0,dark=0,shine=0;
                for(size_t p=0;p<before.size();p+=4){int a=0,b=0;bool same=true;
                    for(size_t c=0;c<3;++c){same&=std::abs(int(before[p+c])-int(after[p+c]))<=2;a+=after[p+c];b+=full[p+c];}
                    changed+=!same;dark+=b+8<a;shine+=b>a+8;
                }
                const bool fail=changed || dark || badMirrors;
                failures+=fail;++cases;csv<<id<<",gt2,"<<(cockpit?"cockpit":"exterior")<<','<<(modern?"modern":"classic")<<','<<changed<<','<<dark<<','<<shine<<','<<badMirrors<<','<<mirrorTriangles<<",0\n";
                if(fail || std::getenv("GT2_AUDIT_CAPTURE")){
                    const auto stem=id+(cockpit?"-cockpit":"-exterior")+(modern?"-modern":"-classic");
                    gt2::WritePngRgba((out/(stem+"-body.png")).string(),size,size*2,after);
                    gt2::WritePngRgba((out/(stem+"-full.png")).string(),size,size*2,full);
                }
            }
            if(pack.Has(id)) {
                // Reproduce the reported transition on one loaded slot: a
                // GT4-only colour survives returning to the original model.
                assets.SetGt4Enabled(true);
                if(!assets.UsesGt4(slot))throw std::runtime_error("switch did not load GT4: "+id);
                paint=31;
                capture(draws(),vp);
                assets.SetGt4Enabled(false);
                if(assets.UsesGt4(slot))throw std::runtime_error("switch did not restore GT2: "+id);
                auto restored=draws();std::erase_if(restored,[](const auto& d){return d.blend!=kBlendOpaque;});
                const auto switched=capture(restored,vp);size_t different=0;
                for(size_t p=0;p<before.size();p+=4)for(size_t c=0;c<3;++c)
                    different+=std::abs(int(before[p+c])-int(switched[p+c]))>2;
                failures+=different!=0;++cases;
                csv<<id<<",gt2,"<<(cockpit?"cockpit":"exterior")<<",gt4-to-gt2,"<<different<<",0,0,0,"<<mirrorTriangles<<",0\n";
            }
        }
        csv.flush();std::cout<<id<<(gt4?" gt4":" gt2")<<" paths "<<paths<<" failures "<<failures<<'\n'<<std::flush;
    }
    std::ofstream(out/"gate.json")<<"{\"paths\":"<<paths<<",\"cases\":"<<cases<<",\"failures\":"<<failures<<"}\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
