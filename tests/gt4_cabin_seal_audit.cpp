// Coloured witnesses behind closed cabin panels. Uses the production seated
// body, procedural lining, texture alpha, MSAA and both eye layers.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "../tools/gt2game/cockpit_camera.h"
#include "gt2export/png_writer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
#include "gt4_cabin_witness.h"
int main(int argc,char** argv) {try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    std::vector<std::string> ids;
    if(argc>4)for(int i=4;i<argc;++i)ids.emplace_back(argv[i]);
    else {const auto manifest=gt2::json::ReadFile((gt2::gt4ModelSetRoot/"manifest.json").string());for(const auto& [id,e]:manifest.Require("cars").Members()){(void)e;ids.push_back(id);}}
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{256,192},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({256,192},VK_FORMAT_R8G8B8A8_UNORM,true);
    std::ofstream csv(out/"seals.csv");csv<<"car,native_interior,pose,offset,eye,yellow_pixels\n";
    bool failed=false;size_t cars=0,viewsCount=0;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const std::array<std::array<float,2>,8> poses{{{0,-55},{-70,-40},{70,-40},{180,-40},{0,65},{-70,35},{70,35},{180,35}}};
    for(const auto& id:ids) {
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(true);const int slot=assets.UseCar(id);
        if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("native car unavailable: "+id);
        const auto fit=assets.SlotCockpitFit(slot);if(!fit.valid)throw std::runtime_error("invalid fit: "+id);
        if(!fit.openTop && std::min(fit.rearHeader[0][2],fit.rearHeader[1][2])<fit.eye[2]+.14f)throw std::runtime_error("roof ends ahead of seat: "+id);
        ProceduralCockpit cabin(renderer);std::vector<DrawItem> items;
        assets.AppendCockpitBodyItems(items,slot,identity,0);
        const bool native=assets.HasNativeGt4Interior(slot);
        const gt2game::CockpitSettings settings;
        gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,settings);
        cabin.Append(items,fit,identity,0,0,0,1,false,gt2game::CockpitSeatHeight(fit,settings),gt2game::CockpitSeatBack(settings),nullptr,!native);
        if(std::getenv("GT2_AUDIT_INJECT_CABIN_GAP"))items.erase(std::remove_if(items.begin(),items.end(),[](const auto& d){return d.firstVertex>=SceneRenderer::kCockpitVertexBase && d.firstVertex<SceneRenderer::kCockpitVertexBase+SceneRenderer::kCockpitVertexLimit;}),items.end());
        const auto witness=Witness(fit);renderer.SetVertices(0,witness);
        const auto reference=Witness(fit,true);renderer.SetVertices(uint32_t(witness.size()),reference);
        DrawItem sentinel;sentinel.vertexCount=uint32_t(witness.size());std::copy_n(identity,16,sentinel.mvp);items.push_back(sentinel);
        for(auto& item:items)item.space=kSpaceWorld;SortModelTransparency(items);
        DrawItem closed=sentinel;closed.firstVertex=uint32_t(witness.size());closed.vertexCount=uint32_t(reference.size());closed.space=kSpaceWorld;
        for(size_t pose=0;pose<poses.size();++pose)for(int shift=0;shift<3;++shift) {
            std::array<float,3> eye{camera.eye[0],camera.eye[1],camera.eye[2]};
            if(shift==1){eye[1]-=.10f;eye[2]-=.08f;}
            if(shift==2){eye[0]+=.12f;eye[2]+=.08f;}
            StereoViews views;
            for(int e=0;e<2;++e){auto position=eye;position[0]+=(e?1:-1)*.032f;const auto vp=View(position,poses[pose][0],poses[pose][1]);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
            renderer.SetStereoViews(views);std::fill_n(renderer.clearColor,3,0.f);
            const auto rgba=renderer.CaptureStereoForAudit(items,items.size());
            const auto expected=renderer.CaptureStereoForAudit({closed},1);
            constexpr size_t width=256,height=192;
            if(rgba.size()!=width*height*2*4)throw std::runtime_error("invalid two-eye readback");
            for(size_t e=0;e<2;++e) {
                auto yellow=[&](size_t x,size_t y){const size_t p=((y+e*height)*width+x)*4;return rgba[p]>230 && rgba[p+1]>230 && rgba[p+2]<16;};
                // The independent closed-panel mask excludes legitimate window
                // views of the witness silhouette. Inset one raster pixel so
                // displaced proxy edges cannot masquerade as cabin holes.
                auto panel=[&](size_t x,size_t y){const size_t p=((y+e*height)*width+x)*4;return expected[p]>230 && expected[p+1]>230 && expected[p+2]>230;};
                size_t holes=0;
                for(size_t y=2;y+2<height;++y)for(size_t x=2;x+2<width;++x)
                    if(panel(x,y) && panel(x-1,y) && panel(x+1,y) && panel(x,y-1) && panel(x,y+1) && yellow(x,y) && yellow(x-1,y) && yellow(x+1,y) && yellow(x,y-1) && yellow(x,y+1))++holes;
                csv<<id<<','<<native<<','<<pose<<','<<shift<<','<<e<<','<<holes<<'\n';
                if(holes){failed=true;const auto dest=out/id;std::filesystem::create_directories(dest);gt2::WritePngRgba((dest/("gap-"+std::to_string(pose)+"-"+std::to_string(shift)+".png")).string(),int(width),int(height*2),rgba);}
                ++viewsCount;
            }
        }
        csv.flush();++cars;if(cars%20==0)std::cout<<"Seal corpus "<<cars<<'/'<<ids.size()<<", "<<viewsCount<<" eye views\n"<<std::flush;
    }
    std::cout<<cars<<" cars, "<<viewsCount<<" eye views, closed panel gate "<<(failed?"FAIL":"PASS")<<'\n';
    return failed?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
