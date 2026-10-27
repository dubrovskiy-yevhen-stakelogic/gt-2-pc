// Inspect generated cabin shape in the production occupied draw path.
#include "offscreen_car_audit.h"
#include "car_audit_catalogue.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <fstream>
#include <iostream>
using namespace gt2view;
// A generated fascia may follow a curved windshield, but must not grow
// hand-width wings into the seating volume on either side.
std::array<float,2> SideSetback(std::span<const SceneVertex> fascia){
    float front=std::numeric_limits<float>::infinity();
    for(const auto& v:fascia)front=std::min(front,v.pos[2]);
    std::array<float,2> result{};
    for(const auto& v:fascia)result[size_t(v.pos[0]>=0)]=std::max(result[size_t(v.pos[0]>=0)],v.pos[2]-front);
    return result;
}
int main(int argc,char** argv){try {
    if(argc<4 || !std::getenv("GT2_AUDIT_COCKPIT"))throw std::runtime_error("GT2_AUDIT_COCKPIT=1; disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;gt2::ModelSet pack(argv[2]);
    const auto ids=AuditCarCatalogue(vol,argc,argv);
    constexpr uint32_t size=512;Device device;VkContext context(device.existing);
    VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);
    renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    constexpr float maxSetback=.20f;
    const bool capture=std::getenv("GT2_AUDIT_CAPTURE")!=nullptr;
    // Exercise the classifier with real generated fascia triangles, including
    // a watertight two-wing deformation. A hole detector would accept both.
    {
        CockpitFit f;f.valid=true;f.eye={-.3f,1.0f,.3f};f.windshieldZ=-.6f;f.windshieldY=.7f;
        f.frontCowl={{{-.8f,.7f,-.6f},{.8f,.7f,-.6f}}};f.rearCowl={{{-.8f,.7f,1.f},{.8f,.7f,1.f}}};
        for(size_t i=0;i<f.cowl.size();++i)f.cowl[i]={-.8f+float(i)*.1f,.7f,-.6f};
        for(bool deformed:{false,true}){
            if(deformed)for(auto& p:f.cowl)p[2]+=.7f*std::pow(std::abs(p[0])/.8f,4.f);
            ProceduralCockpit cabin(renderer);std::vector<DrawItem> draw;cabin.Append(draw,f,identity,0,0,0,1);
            auto witness=cabin.FasciaForAudit();
            if(deformed)for(auto& v:witness)v.pos[2]+=.5f*std::pow(std::abs(v.pos[0])/.8f,4.f);
            const auto setbacks=SideSetback(witness);
            if((setbacks[0]>maxSetback && setbacks[1]>maxSetback)!=deformed)
                throw std::runtime_error("fascia deformation positive/negative controls failed");
        }
    }
    std::ofstream summary(out/"shape.csv");summary<<"car,path,native_interior,left_setback_m,right_setback_m,eye_drift_m,wheel_drift_m,bad\n";
    size_t paths=0,failures=0,poses=0;
    for(const auto& id:ids)for(bool native:{false,true}){
        if(native && !pack.Has(id))continue;
        if(!native && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(native);const int slot=assets.UseCar(id);
        if(slot<0 || (native && !assets.UsesGt4(slot)))throw std::runtime_error("missing model "+id);
        auto fit=assets.SlotCockpitFit(slot);const auto stem=id+(native?"-gt4":"-gt2");
        auto original=fit;
        if(!native){
            original=FitCockpit(assets.SlotModel(slot),{},false);
            const auto texture=gt2::ParseCarTexture(vol.Read("carobj/"+id+".cdp"));
            if(capture){
                const auto structure=BuildCockpitBody(assets.SlotModel(slot),texture,fit);
                std::cout<<"Structure "<<id<<" mirrors "<<structure.mirrorSupports.size()<<" seams "<<structure.pillarSeams.size()<<'\n';
                gt2::CarMeshOptions mo;mo.wheels=false;const auto source=gt2::BuildCarMesh(assets.SlotModel(slot),mo);
                size_t offset=0;std::ofstream faces(out/(stem+"-faces.csv"));faces<<"first,count,glass,end,side\n";
                for(const auto& p:assets.SlotModel(slot).lods[0].polygons){const size_t n=p.IsQuad()?6:3;const auto face=std::span(source).subspan(offset,n);
                    float glass=0;for(size_t t=0;t<n;t+=3)glass=std::max(glass,cockpit_detail::GlassCoverage(face.subspan(t,3),texture,structure.glassMasks));
                    faces<<offset/3<<','<<n/3<<','<<glass<<','<<IsCockpitEndWindowFace(face)<<','<<CockpitSideOpening(face.first(3),fit)<<'\n';offset+=n;
                }
            }
            if(capture)for(size_t palette=0;palette<16;++palette){std::vector<uint8_t> rgba;
                for(auto index:texture.indices){const auto color=texture.paints[0].cluts[palette][index];
                    for(int shift:{0,5,10})rgba.push_back(uint8_t(((color>>shift)&31)*255/31));rgba.push_back(color?255:0);}
                gt2::WritePngRgba((out/(stem+"-texture-"+std::to_string(palette)+".png")).string(),256,224,rgba);}
            const auto body=BuildCockpitBody(assets.SlotModel(slot),texture,original);
            FitCockpitSideSills(original,body.windowOpenings);
            RefineCockpitEye(original,body.vertices,texture,body.glassMasks);
            FitCockpitMirror(original,assets.SlotModel(slot));
        }
        if(!native && std::getenv("GT2_AUDIT_LEGACY_COWL"))fit=original;
        if(capture && !native){gt2::CarMeshOptions rawOptions;rawOptions.wheels=false;
            std::ofstream raw(out/(stem+"-source.csv"));raw<<"x,y,z,r,g,b,palette,u,v\n";
            for(const auto& v:gt2::BuildCarMesh(assets.SlotModel(slot),rawOptions))
                raw<<v.pos[0]<<','<<v.pos[1]<<','<<v.pos[2]<<','<<v.color[0]<<','<<v.color[1]<<','<<v.color[2]<<','<<int(v.palette)<<','<<v.texel[0]<<','<<v.texel[1]<<'\n';}
        std::ofstream data;if(capture)data.open(out/(stem+"-fit.csv"));data<<"group,index,x,y,z\n";
        auto curve=[&](const char* name,const auto& points){size_t i=0;for(const auto& p:points)data<<name<<','<<i++<<','<<p[0]<<','<<p[1]<<','<<p[2]<<'\n';};
        curve("eye",std::array<std::array<float,3>,1>{fit.eye});curve("cowl",fit.cowl);curve("front",fit.frontCowl);curve("rear",fit.rearCowl);
        curve("left",fit.sideSill[0]);curve("right",fit.sideSill[1]);curve("wheels",fit.wheelWells);
        std::array<float,3> oldEye{},oldWheel{};
        if(!native){
            oldEye=original.eye;
            ProceduralCockpit previous(renderer);std::vector<DrawItem> ignored;
            previous.Append(ignored,original,identity,0,0,0,1,true,0,gt2game::CockpitSeatBack({}));oldWheel=previous.WheelCenter();
        }
        ProceduralCockpit furniture(renderer);std::vector<DrawItem> items;
        assets.AppendCockpitBodyItems(items,slot,identity,0);
        if(capture){std::ofstream bodyMesh(out/(stem+"-occupied.csv"));bodyMesh<<"x,y,z,r,g,b,flags,u,v,clut\n";
            for(const auto& d:items)for(const auto& v:renderer.VerticesForAudit(d))
                bodyMesh<<v.pos[0]<<','<<v.pos[1]<<','<<v.pos[2]<<','<<v.color[0]<<','<<v.color[1]<<','<<v.color[2]<<','<<v.flags<<','<<v.texel[0]<<','<<v.texel[1]<<','<<v.clut<<'\n';}
        furniture.Append(items,fit,identity,0,0,0,1,true,0,gt2game::CockpitSeatBack({}),nullptr,!assets.HasNativeGt4Interior(slot));
        const auto setbacks=SideSetback(furniture.FasciaForAudit());
        float eyeDrift=0,wheelDrift=0;
        if(!native)for(size_t axis=0;axis<3;++axis){eyeDrift=std::max(eyeDrift,std::abs(fit.eye[axis]-oldEye[axis]));wheelDrift=std::max(wheelDrift,std::abs(furniture.WheelCenter()[axis]-oldWheel[axis]));}
        const bool bad=!assets.HasNativeGt4Interior(slot) && (setbacks[0]>maxSetback || setbacks[1]>maxSetback);
        ++paths;failures+=bad;
        summary<<id<<','<<(native?"gt4":"gt2")<<','<<assets.HasNativeGt4Interior(slot)<<','<<setbacks[0]<<','<<setbacks[1]<<','<<eyeDrift<<','<<wheelDrift<<','<<bad<<'\n';summary.flush();
        for(auto& d:items)d.space=kSpaceWorld;SortModelTransparency(items);
        std::ofstream mesh;if(capture)mesh.open(out/(stem+"-furniture.csv"));mesh<<"x,y,z,r,g,b\n";
        if(capture)for(const auto& d:items)if(d.firstVertex>=SceneRenderer::kCockpitVertexBase && d.firstVertex<SceneRenderer::kCockpitVertexBase+SceneRenderer::kCockpitVertexLimit)
            for(const auto& v:renderer.VerticesForAudit(d))mesh<<v.pos[0]<<','<<v.pos[1]<<','<<v.pos[2]<<','<<v.color[0]<<','<<v.color[1]<<','<<v.color[2]<<'\n';
        gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,{});
        for(int pose=0;pose<6;++pose){
            const float yaw=pose<3?float(pose-1)*55.f:float(pose-4)*55.f;
            const float pitch=pose<3?-30.f:-55.f;
            StereoViews views;for(int e=0;e<2;++e){std::array<float,3> eye{camera.eye[0]+(e?.032f:-.032f),camera.eye[1],camera.eye[2]};
                const auto vp=View(eye,yaw,pitch);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
            renderer.SetStereoViews(views);std::fill_n(renderer.clearColor,3,.6f);
            const auto frame=renderer.CaptureStereoForAudit(items,items.size());
            ++poses;
            if(capture || bad)gt2::WritePngRgba((out/(stem+"-"+std::to_string(pose)+".png")).string(),size,size*2,frame);
        }
    }
    std::ofstream(out/"gate.json")<<"{\"paths\":"<<paths<<",\"poses\":"<<poses<<",\"failures\":"<<failures<<",\"deformation_control\":true}\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
