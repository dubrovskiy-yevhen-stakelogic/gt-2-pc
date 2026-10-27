// Owned-data regression: changing a GT4 body colour must not erase donor rims.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "car_audit_catalogue.h"
#include "gt2export/png_writer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv) {try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto ids=AuditCarCatalogue(vol,argc,argv);
    gt2::ModelSet modelSet(gt2::gt4ModelSetRoot);
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{384,384},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=1;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({384,384},VK_FORMAT_R8G8B8A8_UNORM,true);
    std::ofstream csv(out/"wheels.csv");csv<<"car,path,paint,side,reference_pixels,missing_pixels\n";
    size_t failures=0,views=0,variants=0;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(const auto& id:ids)for(bool native:{false,true}) {
        if(native && !modelSet.Has(id))continue;
        if(!native && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,false);assets.SetGt4Enabled(native);const int slot=assets.UseCar(id);
        if(slot<0 || (native && !assets.UsesGt4(slot)))throw std::runtime_error("car unavailable: "+id);
        const auto texture=gt2::ParseCarTexture(vol.Read("carobj/"+id+".cdp"));
        const size_t paints=native?gt2::ModelSet(gt2::gt4ModelSetRoot).Read(id,false).paints.size():texture.paints.size();
        std::array<std::vector<uint8_t>,2> reference;
        // Read back the actual shader. The wheel-only pass removes body colour
        // from the comparison; brightness may vary, but opaque coverage may not.
        for(size_t paint=0;paint<paints;++paint)for(size_t side=0;side<2;++side) {
            const auto& centre=assets.SlotWheelCentres(slot)[side];
            const auto view=View({side? -3.f:3.f,centre[1],0},side?90.f:-90.f,0);
            StereoViews stereo;for(int eye=0;eye<2;++eye)std::copy(view.begin(),view.end(),stereo.worldVP[eye]);renderer.SetStereoViews(stereo);
            std::vector<DrawItem> full;assets.AppendCarItems(full,slot,identity,uint32_t(paint),0);
            std::vector<DrawItem> wheels;assets.AppendCarWheels(wheels,slot,identity,uint32_t(paint));
            // Donor rims only: native brake attachments can have paint-dependent
            // materials and must not hide loss of the rim in this reference.
            wheels.erase(std::remove_if(wheels.begin(),wheels.end(),[](const auto& item){return item.firstVertex>=SceneRenderer::kModelSetVertexBase;}),wheels.end());
            for(auto& item:wheels)item.space=kSpaceWorld;
            renderer.clearColor[0]=1;renderer.clearColor[1]=0;renderer.clearColor[2]=1;
            auto rgba=renderer.CaptureStereoForAudit(wheels,wheels.size());
            auto covered=[](const auto& image,size_t p){return !(image[p]>250 && image[p+1]<3 && image[p+2]>250);};
            if(!paint)reference[side]=rgba;
            size_t expected=0,missing=0;
            for(size_t p=0;p<rgba.size();p+=4)if(covered(reference[side],p)){
                ++expected;
                const auto& ref=reference[side];
                const bool vanished=std::max({ref[p],ref[p+1],ref[p+2]})>48 && std::max({rgba[p],rgba[p+1],rgba[p+2]})<8;
                if(!covered(rgba,p) || (native && vanished))++missing;
            }
            const bool fail=missing>2;
            failures+=fail;++views;if(!side)++variants;
            csv<<id<<','<<(native?"gt4":"gt2")<<','<<paint<<','<<side<<','<<expected<<','<<missing<<'\n';
            if(fail || (std::getenv("GT2_AUDIT_CAPTURE") && id==ids.front())) {
                const auto name=id+"-"+(native?"gt4":"gt2")+"-"+std::to_string(paint)+"-"+std::to_string(side);
                gt2::WritePngRgba((out/(name+".png")).string(),384,768,rgba);
            }
        }
        csv.flush();std::cout<<id<<' '<<(native?"gt4":"gt2")<<" paints "<<paints<<" failures "<<failures<<'\n'<<std::flush;
    }
    std::ofstream(out/"gate.json")<<"{\"cars\":"<<ids.size()<<",\"variants\":"<<variants<<",\"views\":"<<views<<",\"failures\":"<<failures<<"}\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
