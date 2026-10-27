// Owned-data showroom audit: body/wheel alignment and native overlay coverage.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "car_audit_catalogue.h"
#include "gt2export/png_writer.h"
#include "gt2formats/png_reader.h"
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto ids=AuditCarCatalogue(vol,argc,argv);gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    constexpr uint32_t size=512;Device device;VkContext context(device.existing);
    VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);
    RenderOptions options;options.msaa=1;options.smoothTextures=true;renderer.SetOptions(options);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::ofstream report(out/"geometry.csv");report<<"car,wheel,x,y,z,radius,width\n";
    std::ofstream coverage(out/"coverage.csv");coverage<<"car,pose,reflected_pixels,unsupported_pixels\n";
    size_t failures=0,paths=0;
    for(const auto& id:ids) {
        if(!pack.Has(id))continue;
        SceneAssets assets(renderer,vol,false);assets.SetGt4Enabled(true);const int slot=assets.UseCar(id);
        if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("missing native car "+id);
        const auto fit=assets.SlotCockpitFit(slot);
        auto source=pack.Read(id,false);ApplyNativeBodyPose(source);++paths;
        if(source.nativeWheelPose) {
            const float scale=fit.wheelWells[0][3]/source.wheelWells[0][3];
            std::array<float,3> offset{};for(size_t k=0;k<3;++k)offset[k]=fit.wheelWells[0][k]-source.wheelWells[0][k]*scale;
            for(size_t w=0;w<4;++w)for(size_t k=0;k<5;++k)
                if(std::abs(fit.wheelWells[w][k]-(source.wheelWells[w][k]*scale+(k<3?offset[k]:0.f)))>.0001f)
                    throw std::runtime_error("runtime distorted native wheel pose: "+id);
        }
        for(size_t w=0;w<4;++w){report<<id<<','<<w;for(float n:fit.wheelWells[w])report<<','<<n;report<<'\n';}
        for(int pose=0;pose<3;++pose) {
            const auto view=pose==0?View({3,1.3f,4},-37,-7):pose==1?View({3.5f,.5f,0},-90,0):View({0,2.8f,3.5f},0,-35);
            std::vector<DrawItem> full;assets.AppendCarItems(full,slot,view.data(),0,0);
            std::erase_if(full,[](const auto& d){return d.blend==2;});
            // Transparent lenses legitimately have a reflection without an
            // opaque base. Retain those in the reference; only the matched
            // opaque-body coating must stay inside depth-writing coverage.
            auto bare=full;std::erase_if(bare,[](const auto& d){return d.blend==kBlendNativeSurfaceEnvironment;});
            if(std::getenv("GT2_AUDIT_LEGACY_REFLECTION_COVERAGE"))
                for(auto& d:full)if(d.blend==kBlendNativeSurfaceEnvironment)d.blend=kBlendNativeEnvironment;
            renderer.clearColor[0]=.24f;renderer.clearColor[1]=.27f;renderer.clearColor[2]=.30f;
            for(int pass=0;pass<2;++pass) {
                renderer.Draw(pass?full:bare,(out/(id+"-"+std::to_string(pose)+(pass?"-full.png":"-bare.png"))).string(),pass?full.size():bare.size());
                renderer.WaitFrame();
            }
            const auto prefix=out/(id+"-"+std::to_string(pose));
            const auto a=gt2::ReadPngFile(prefix.string()+"-bare.png"),b=gt2::ReadPngFile(prefix.string()+"-full.png");
            const auto background=[&](size_t p){return a.rgba[p]==a.rgba[0] && a.rgba[p+1]==a.rgba[1] && a.rgba[p+2]==a.rgba[2];};
            size_t changed=0,unsupported=0;
            for(size_t y=1;y+1<size;++y)for(size_t x=1;x+1<size;++x) {
                const size_t p=(y*size+x)*4;
                const int delta=std::abs(int(a.rgba[p])-int(b.rgba[p]))+std::abs(int(a.rgba[p+1])-int(b.rgba[p+1]))+std::abs(int(a.rgba[p+2])-int(b.rgba[p+2]));
                changed+=delta>3;
                unsupported+=delta>6 && background(p) && background(p-4) && background(p+4) && background(p-size*4) && background(p+size*4);
            }
            coverage<<id<<','<<pose<<','<<changed<<','<<unsupported<<'\n';coverage.flush();
            if(unsupported>8){++failures;std::cout<<id<<" pose "<<pose<<": reflections fill "<<unsupported<<" empty pixels\n";}
            // Keep failure evidence and named visual witnesses, not thousands
            // of identical-size successful captures on the build volume.
            if(unsupported<=8 && id!="t2h8r" && id!="z0428" && id!="z0740" && !std::getenv("GT2_AUDIT_CAPTURE")) {
                std::filesystem::remove(prefix.string()+"-bare.png");
                std::filesystem::remove(prefix.string()+"-full.png");
            }
        }
        report.flush();std::cout<<id<<" captured\n"<<std::flush;
    }
    std::cout<<paths<<" showroom paths, "<<failures<<" coverage failures\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
