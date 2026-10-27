// Owned-data corpus audit: actual menu/race shaders, paint selections, windows
// and seated geometry. Generated captures/reports are restricted to work.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/car_info.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv) {try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    const auto output=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=output.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(output);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto info=gt2::CarInfoDirectory::Load(vol);
    std::vector<std::string> ids;
    if(argc>4)for(int i=4;i<argc;++i)ids.emplace_back(argv[i]);
    else {const auto doc=gt2::json::ReadFile((gt2::gt4ModelSetRoot/"manifest.json").string());for(const auto& [id,e]:doc.Require("cars").Members()){(void)e;ids.push_back(id);}}
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{512,384},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=std::getenv("GT2_AUDIT_MSAA")?uint32_t(std::stoi(std::getenv("GT2_AUDIT_MSAA"))):1;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({512,384},VK_FORMAT_R8G8B8A8_UNORM,true);
    std::ofstream csv(output/"corpus.csv");csv<<"car,paints,exterior,cockpit,reflection,glass_pixels,paint_changes,window_pixels\n";
    const float model[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    size_t count=0,variants=0,windows=0;bool failed=false;
    for(const auto& id:ids) {
        const auto dest=output/id;std::filesystem::create_directories(dest);
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(true);const int slot=assets.UseCar(id);
        if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("native car unavailable: "+id);
        if(assets.Gt4ExteriorRemovedArea(slot)>1e-6)throw std::runtime_error("native exterior removed: "+id);
        const auto car=gt2::ModelSet(gt2::gt4ModelSetRoot).Read(id,false);
        const auto* record=info.Find(gt2::PackCarId(id));
        if(!record || record->PaintCount()!=car.paints.size())throw std::runtime_error("menu native paint count: "+id);
        for(size_t p=0;p<record->PaintCount();++p)if(record->PaintIndex(record->paintIds.at(p))!=int(p))throw std::runtime_error("paint save/race mapping: "+id);
        std::vector<std::vector<uint8_t>> baseline;size_t changed=0,glassPixels=0,windowPixels=0;
        const std::array<std::array<float,3>,3> eyes{{{3.5f,1.4f,0},{0,1.3f,4.1f},{-3.0f,1.4f,-2.5f}}};
        const std::array<float,3> yaw{-90,0,130};
        const size_t paintLimit=std::getenv("GT2_AUDIT_CABIN_ONLY")?0:
            std::getenv("GT2_AUDIT_BASE_PAINT_ONLY")?std::min<size_t>(1,car.paints.size()):car.paints.size();
        for(size_t paint=0;paint<paintLimit;++paint) {
            size_t changes=0;
            for(size_t angle=0;angle<3;++angle) {
                const auto vp=View(eyes[angle],yaw[angle],0);
                std::vector<DrawItem> items;assets.AppendCarItems(items,slot,vp.data(),uint32_t(paint),0);SortModelTransparency(items);
                const auto path=dest/("paint-"+std::to_string(paint)+"-"+std::to_string(angle)+".png");
                renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=.10f;
                renderer.Draw(items,path.string(),items.size());renderer.WaitFrame();
                const auto image=gt2::ReadPngFile(path.string());
                if(!paint)baseline.push_back(image.rgba);
                else for(size_t i=0;i<image.rgba.size();i+=4)if(std::abs(int(image.rgba[i])-int(baseline[angle][i]))+
                        std::abs(int(image.rgba[i+1])-int(baseline[angle][i+1]))+std::abs(int(image.rgba[i+2])-int(baseline[angle][i+2]))>3)++changes;
                if(paint && std::getenv("GT2_AUDIT_KEEP_PAINTS")==nullptr)std::filesystem::remove(path);
                if(!paint && !std::getenv("GT2_AUDIT_EXTERIOR_ONLY")) {
                    auto relative=std::array<float,16>{};std::copy_n(model,16,relative.begin());for(size_t k=0;k<3;++k)relative[12+k]-=eyes[angle][k];
                    std::vector<DrawItem> world;assets.AppendCarItems(world,slot,relative.data(),0,0);
                    for(auto& item:world)item.space=kSpaceWorld;SortModelTransparency(world);
                    const auto originView=View({0,0,0},yaw[angle],0);
                    StereoViews views;for(int e=0;e<2;++e)std::copy(originView.begin(),originView.end(),views.worldVP[e]);renderer.SetStereoViews(views);
                    renderer.DrawStereo(world,(dest/("race-"+std::to_string(angle)+".png")).string(),world.size());renderer.WaitFrame();
                    // A changed background reveals fractional coverage on the
                    // actual car's visible pixels, not on a synthetic glass quad.
                    renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=.9f;
                    const auto white=dest/"coverage.png";renderer.DrawStereo(world,white.string(),world.size());renderer.WaitFrame();
                    const auto black=gt2::ReadPngFile((dest/("race-"+std::to_string(angle)+".png")).string());
                    const auto light=gt2::ReadPngFile(white.string());
                    for(size_t i=0;i<light.rgba.size();i+=4) {
                        const int d=int(light.rgba[i])-int(black.rgba[i]);
                        if(d>4 && d<195)++glassPixels;
                    }std::filesystem::remove(white);
                    // Verify authored alpha geometry separately from opaque
                    // silhouettes and tyres. GT4 light callbacks also substitute
                    // glazing/trim, so include their active native draw ranges.
                    auto glazing=world;
                    const uint32_t base=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride;
                    const uint64_t end=uint64_t(base)+SceneRenderer::kModelSetVertexStride;
                    glazing.erase(std::remove_if(glazing.begin(),glazing.end(),[&](const auto& item) {
                        return item.blend!=kBlendAlpha || item.stpPass!=3 || item.firstVertex<base || uint64_t(item.firstVertex)+item.vertexCount>end;
                    }),glazing.end());
                    if(!glazing.empty()) {
                        const auto wb=dest/("windows-"+std::to_string(angle)+".png"),ww=dest/"window-coverage.png";
                        renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=.1f;
                        renderer.DrawStereo(glazing,wb.string(),glazing.size());renderer.WaitFrame();
                        renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=.9f;
                        renderer.DrawStereo(glazing,ww.string(),glazing.size());renderer.WaitFrame();
                        const auto b=gt2::ReadPngFile(wb.string()),w=gt2::ReadPngFile(ww.string());
                        auto fractional=[&](size_t pixel){const int d=int(w.rgba[pixel*4])-int(b.rgba[pixel*4]);return d>4 && d<195;};
                        for(size_t y=1;y+1<w.height;++y)for(size_t x=1;x+1<w.width;++x) {
                            const size_t pixel=y*w.width+x;
                            if(fractional(pixel) && fractional(pixel-1) && fractional(pixel+1) && fractional(pixel-w.width) && fractional(pixel+w.width))++windowPixels;
                        }
                        std::filesystem::remove(ww);
                    }
                }
                if(paint && changes)break;
            }
            if(paint && changes)++changed;
            if(paint && !changes) {std::cerr<<"Invisible paint "<<id<<' '<<paint<<'\n';failed=true;}
            ++variants;
        }
        // Look around inside each seated hull; visual sheets expose cut scraps,
        // transparent-window silhouettes and missing procedural joins.
        const auto& fit=assets.SlotCockpitFit(slot);ProceduralCockpit cabin(renderer);
        if(!std::getenv("GT2_AUDIT_EXTERIOR_ONLY"))for(float pitch:{-50.f,-25.f,0.f,35.f,65.f})for(float angle:{0.f,-60.f,60.f,160.f}) {
            const auto vp=View(fit.eye,angle,pitch);std::vector<DrawItem> items;
            assets.AppendCockpitBodyItems(items,slot,vp.data(),0);
            cabin.Append(items,fit,vp.data(),0,0,0,1,false,0,0,nullptr,!assets.HasNativeGt4Interior(slot));
            renderer.clearColor[0]=.05f;renderer.clearColor[1]=.8f;renderer.clearColor[2]=.9f;
            SortModelTransparency(items);renderer.Draw(items,(dest/("cabin-"+std::to_string(int(angle))+"-"+std::to_string(int(pitch))+".png")).string(),items.size());renderer.WaitFrame();
        }
        if(glassPixels)++windows;
        csv<<id<<','<<paintLimit<<','<<assets.Gt4ExteriorVertices(slot)<<','<<assets.Gt4CockpitVertices(slot)<<','
           <<assets.Gt4ReflectionVertices(slot)<<','<<glassPixels<<','<<changed<<','<<windowPixels<<'\n';csv.flush();
        ++count;if(count%20==0)std::cout<<"Native corpus "<<count<<'/'<<ids.size()<<" cars, "<<variants<<" paints\n"<<std::flush;
    }
    std::cout<<count<<" cars, "<<variants<<" actual rendered paints, "<<windows<<" fractional race-coverage cases\n";
    return failed?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
