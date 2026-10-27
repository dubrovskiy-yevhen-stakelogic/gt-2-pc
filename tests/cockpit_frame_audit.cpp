#include "offscreen_car_audit.h"
#include "car_audit_catalogue.h"
#include "cockpit_structure_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    if(!std::getenv("GT2_AUDIT_COCKPIT"))throw std::runtime_error("GT2_AUDIT_COCKPIT=1 is required for frame diagnostics");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto ids=AuditCarCatalogue(vol,argc,argv);gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    constexpr uint32_t size=512;Device device;VkContext context(device.existing);
    VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    size_t paths=0,failures=0;
    // Intersecting inner/outer shell faces must not expose the back of the
    // dark inner shell outside. Retaining backfaces reproduces the black patch.
    auto layerImage=[&](bool culled){
        std::vector<SceneVertex> vertices;
        const std::array<std::array<float,3>,4> corners{{{-.7f,-.6f,0},{.7f,-.6f,0},{.7f,.6f,0},{-.7f,.6f,0}}};
        const std::array<unsigned,6> indices{0,1,2,0,2,3};
        for(size_t layer=0;layer<2;++layer)for(size_t i=0;i<6;++i){SceneVertex v{};
            const auto index=indices[layer?5-i:i];std::copy_n(corners[index].begin(),3,v.pos);
            v.pos[2]=layer?.03f:0.f;std::fill_n(v.color,3,layer?0.f:.7f);
            if(layer && culled)v.flags|=kCullBack;vertices.push_back(v);}
        renderer.SetVertices(0,vertices);StereoViews views;
        for(int e=0;e<2;++e){const auto vp=View({e?.032f:-.032f,0,2},0,0);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}renderer.SetStereoViews(views);
        std::fill_n(renderer.clearColor,3,.4f);DrawItem d;std::copy_n(identity,16,d.mvp);d.vertexCount=12;d.space=kSpaceWorld;
        return renderer.CaptureStereoForAudit({d},1);
    };
    const auto bad=layerImage(false),good=layerImage(true);size_t exposedBefore=0,exposedAfter=0;
    for(size_t i=0;i<good.size();i+=4){exposedBefore+=bad[i]<2 && bad[i+1]<2 && bad[i+2]<2;exposedAfter+=good[i]<2 && good[i+1]<2 && good[i+2]<2;}
    if(exposedBefore<100 || exposedAfter>8)throw std::runtime_error("backface raster control did not distinguish missing culling");
    {
        gt2::CarModel model;model.lods.resize(1);auto& lod=model.lods[0];lod.scale=16;
        lod.vertices={{-3277,4915,0,0},{3277,4915,0,0},{3277,4915,4915,0},{-3277,4915,4915,0}};
        gt2::CarPolygon roof;roof.primCode=0x28;roof.vertex={0,1,2,3};lod.polygons.push_back(roof);
        gt2::CarMeshOptions mo;mo.wheels=false;auto mesh=gt2::BuildCarMesh(model,mo);
        CockpitFit fit;fit.eye={0,.8f,.5f};fit.roofFrontZ=0;fit.roofRearZ=1.2f;
        gt2::CarTexture texture;CockpitBody expected;
        const auto intact=structure_audit::Inspect(model,texture,fit,expected,mesh);
        mesh.resize(3);const auto punctured=structure_audit::Inspect(model,texture,fit,expected,mesh);
        if(intact.roofSamples<4 || intact.roofMissing || !punctured.roofMissing)
            throw std::runtime_error("opaque roof hole positive/negative controls failed");
    }
    std::ofstream csv(out/"frames.csv");csv<<"car,path,upper_frame_cut_m2,vertices,roof_samples,roof_missing,pillar_samples,pillar_missing,mount_samples,mount_missing\n";
    for(const auto& id:ids)for(bool native:{false,true}){
        if(native && !pack.Has(id))continue;
        if(!native && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(native);const int slot=assets.UseCar(id);
        if(slot<0 || (native && !assets.UsesGt4(slot)))throw std::runtime_error("missing model "+id);
        const auto fit=assets.SlotCockpitFit(slot);
        structure_audit::Result structure;
        if(!native){
            const auto texture=gt2::ParseCarTexture(vol.Read("carobj/"+id+".cdp"));const auto& model=assets.SlotModel(slot);
            const auto expected=BuildCockpitBody(model,texture,FitCockpit(model));
            std::vector<DrawItem> draws;assets.AppendCockpitBodyItems(draws,slot,identity,0);
            std::vector<gt2::CarMeshVertex> actual;
            for(const auto& draw:draws)for(const auto& vertex:renderer.VerticesForAudit(draw)){
                gt2::CarMeshVertex v{};std::copy_n(vertex.pos,3,v.pos);std::copy_n(vertex.texel,2,v.texel);
                v.palette=uint8_t((vertex.clut&0xffffu)/16);v.textured=(vertex.flags&kTextured)!=0;actual.push_back(v);}
            structure=structure_audit::Inspect(model,texture,fit,expected,actual);
        }
        const auto loss=assets.Gt4UpperFrameCutArea(slot);const bool failed=loss>.0001 || structure.roofMissing || structure.pillarMissing || structure.mountMissing || (!native && !fit.authoredUpperShell);
        failures+=failed;++paths;csv<<id<<','<<(native?"gt4":"gt2")<<','<<loss<<','<<assets.Gt4VertexCount(slot)<<','<<structure.roofSamples<<','<<structure.roofMissing<<','<<structure.pillarSamples<<','<<structure.pillarMissing<<','<<structure.mountSamples<<','<<structure.mountMissing<<'\n';csv.flush();
        if(!failed && !std::getenv("GT2_AUDIT_CAPTURE"))continue;
        ProceduralCockpit furniture(renderer);
        for(int pose=0;pose<8;++pose){
            const bool exterior=pose>=4;
            const float yaw=exterior?float((pose-4)*90):float(pose<2?-55:55);
            auto eye=fit.eye;
            gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,{});std::copy_n(camera.eye,3,eye.begin());
            if(exterior){const float a=yaw*3.14159265f/180;eye={-std::sin(a)*2.5f,fit.roofY+.65f,std::cos(a)*2.5f};}
            else if(pose%2)eye[1]+=.2f;
            StereoViews views;for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;const auto vp=View(at,yaw,exterior?-20.f:0.f);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}renderer.SetStereoViews(views);
            std::fill_n(renderer.clearColor,3,.65f);
            std::vector<DrawItem> items;
            if(exterior)assets.AppendCarItems(items,slot,identity,0,0,true);
            else {assets.AppendCockpitBodyItems(items,slot,identity,0);furniture.Append(items,fit,identity,0,0,0,1,false,0,gt2game::CockpitSeatBack({}),nullptr,!assets.HasNativeGt4Interior(slot));assets.AppendSideMirrorItems(items,slot,identity);}
            for(auto& d:items)d.space=kSpaceWorld;SortModelTransparency(items);
            const auto frame=renderer.CaptureStereoForAudit(items,items.size());
            if(pose==4 && std::getenv("GT2_AUDIT_SAVE_CAPTURE")) {
                const auto vp=View(eye,yaw,-20.f);auto captured=items;
                for(auto& d:captured)std::copy(vp.begin(),vp.end(),d.mvp);
                renderer.SaveCapture((out/(id+(native?"-gt4":"-gt2")+".gt2cap")).string(),captured,captured.size());
            }
            gt2::WritePngRgba((out/(id+(native?"-gt4":"-gt2")+"-"+std::to_string(pose)+".png")).string(),size,size*2,frame);
        }
    }
    std::ofstream(out/"gate.json")<<"{\"paths\":"<<paths<<",\"failures\":"<<failures<<",\"backface_negative_pixels\":"<<exposedBefore<<",\"backface_corrected_pixels\":"<<exposedAfter<<"}\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
