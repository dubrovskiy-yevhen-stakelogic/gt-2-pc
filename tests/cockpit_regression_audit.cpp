// Tests occupied geometry and framebuffer holes, excluding silhouette AA edges.
#include "offscreen_car_audit.h"
#include "cockpit_seam_check.h"
#include "gt2view/scene_assets.h"
#include "car_audit_catalogue.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv) {try {
    if(argc<4 || !std::getenv("GT2_AUDIT_COCKPIT"))throw std::runtime_error("GT2_AUDIT_COCKPIT=1; disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto ids=AuditCarCatalogue(vol,argc,argv);
    gt2::ModelSet modelSet(gt2::gt4ModelSetRoot);
    constexpr uint32_t size=512;
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    // A real rendered aperture exercises the same classifier as the car corpus.
    // A missing optional seal is not a valid negative control when the original
    // body still covers it. Here the opening is explicitly bounded on all sides.
    {
        CockpitFit fit;fit.openTop=true;
        for(size_t i=0;i<fit.cowl.size();++i)fit.cowl[i]={float(i)/8.f-1.f,0,-1};
        const std::array<float,3> eye{0,.5f,0};
        StereoViews stereo;for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;const auto vp=View(at,0,-8);std::copy(vp.begin(),vp.end(),stereo.worldVP[e]);}
        renderer.SetStereoViews(stereo);
        auto draw=[&](bool aperture,bool green) {
            std::vector<SceneVertex> vertices;
            auto quad=[&](float left,float right,float bottom,float top){
                for(const auto& xy:std::array<std::array<float,2>,6>{{{left,bottom},{right,bottom},{right,top},{left,bottom},{right,top},{left,top}}}) {
                    SceneVertex v{};v.pos[0]=xy[0];v.pos[1]=xy[1];v.pos[2]=-1;
                    v.color[0]=v.color[1]=v.color[2]=.3f;vertices.push_back(v);
                }
            };
            if(aperture){quad(-.5f,-.12f,-.15f,.15f);quad(.12f,.5f,-.15f,.15f);quad(-.12f,.12f,-.15f,0);quad(-.12f,.12f,.03f,.15f);}
            else quad(-.5f,.5f,-.15f,.15f);
            renderer.SetVertices(0,vertices);DrawItem item;item.vertexCount=uint32_t(vertices.size());item.space=kSpaceWorld;std::copy_n(identity,16,item.mvp);
            renderer.clearColor[0]=renderer.clearColor[2]=green?0.f:1.f;renderer.clearColor[1]=green?1.f:0.f;
            return renderer.CaptureStereoForAudit({item},1);
        };
        const auto solid=draw(false,false),solidGreen=draw(false,true),aperture=draw(true,false);
        auto marked=aperture;const auto detected=MarkCockpitSeamHoles(aperture,solid,solidGreen,marked,size,eye,fit);
        auto intact=solid;
        if(!detected || MarkCockpitSeamHoles(solid,solid,solidGreen,intact,size,eye,fit))throw std::runtime_error("rendered seam fixture failed");
        std::cout<<"Rendered seam fixture: "<<detected<<" missing pixels detected; intact panel passes\n";
    }
    size_t failures=0,views=0,cars=0,seatFailures=0,seamFailures=0,groundFailures=0;
    std::ofstream csv(out/"cockpits.csv");csv<<"car,path,setting,eye_y,eye_z,floor_y,rear_z,seat_width,seat_height,cushion_drop,headrest_back,holes,ground_pixels\n";
    for(const auto& id:ids)for(bool native:{false,true}) {
        if(native && !modelSet.Has(id))continue;
        if(!native && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(native);const int slot=assets.UseCar(id);
        if(slot<0 || (native && !assets.UsesGt4(slot)))throw std::runtime_error("car unavailable: "+id);
        const auto fit=assets.SlotCockpitFit(slot);
        if(!fit.valid){++failures;csv<<id<<','<<(native?"gt4":"gt2")<<",invalid,"<<fit.eye[1]<<','<<fit.eye[2]<<','<<fit.floorY<<','<<fit.rearWindowZ<<",0,0,0,0,0,0\n";csv.flush();std::cout<<"Invalid cockpit "<<id<<" eye "<<fit.eye[1]<<" dash "<<fit.dashboardY<<" roof "<<fit.roofY<<'\n';continue;}
        ProceduralCockpit cabin(renderer);++cars;
        for(int setting=0;setting<5;++setting) {
            gt2game::CockpitSettings settings;
            if(setting==1)settings.seatBackCm=-20; // exact headset repro
            if(setting==2)settings.seatHeightCm=-20;
            if(setting==3)settings.seatHeightCm=20;
            if(setting==4)settings.seatBackCm=40;
            gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,settings);
            const std::array<float,3> eye{camera.eye[0],camera.eye[1],camera.eye[2]};
            std::vector<DrawItem> items;assets.AppendCockpitBodyItems(items,slot,identity,0);
            const size_t bodyEnd=items.size();
            cabin.Append(items,fit,identity,0,0,0,1,false,gt2game::CockpitSeatHeight(fit,settings),gt2game::CockpitSeatBack(settings),nullptr,!assets.HasNativeGt4Interior(slot));
            std::array<float,3> lo{1e9f,1e9f,1e9f},hi{-1e9f,-1e9f,-1e9f};
            for(const auto& v:cabin.DriverSeatForAudit())for(size_t k=0;k<3;++k){lo[k]=std::min(lo[k],v.pos[k]);hi[k]=std::max(hi[k],v.pos[k]);}
            const float width=hi[0]-lo[0],height=hi[1]-lo[1];
            // Cushion surface is the highest near-horizontal seat triangle.
            float cushion=-1e9f,headrest=1e9f;
            const auto& seat=cabin.DriverSeatForAudit();
            for(size_t i=0;i+2<seat.size();i+=3) {
                const auto* a=seat[i].pos;const auto* b=seat[i+1].pos;const auto* c=seat[i+2].pos;
                if(std::abs(a[1]-b[1])<1e-5f && std::abs(a[1]-c[1])<1e-5f && a[1]<lo[1]+.20f)cushion=std::max(cushion,a[1]);
                for(size_t j=0;j<3;++j)if(seat[i+j].pos[1]>hi[1]-.12f)headrest=std::min(headrest,seat[i+j].pos[2]);
            }
            const float drop=eye[1]-cushion,back=headrest-eye[2];
            // The generated eye moved back 10 cm; furniture remains fixed.
            bool badSeat=!assets.HasNativeGt4Interior(slot) && (width<.52f || height<.95f || height>1.02f || drop<.60f || drop>1.02f || back<.50f || back>.62f);
            for(auto& item:items)item.space=kSpaceWorld;
            auto render=[&](const auto& draws,float pitch) {
                StereoViews stereo;
                for(int e=0;e<2;++e){auto at=eye;at[0]+=(e?.032f:-.032f);const auto vp=View(at,0,pitch);std::copy(vp.begin(),vp.end(),stereo.worldVP[e]);}
                renderer.SetStereoViews(stereo);return renderer.CaptureStereoForAudit(draws,draws.size());
            };
            renderer.clearColor[0]=1;renderer.clearColor[1]=0;renderer.clearColor[2]=1;
            const auto front=render(items,-8);size_t holes=0;
            std::vector<DrawItem> source;
            // Closed LODs have a black interior cap behind the windshield.
            // It must not make legitimate gaps around wipers look like missing
            // bonnet geometry. Retain only authored triangles in front of the
            // seating volume, with their real alpha/material/depth behaviour.
            if(native)assets.AppendGt4BonnetItemsForAudit(source,slot,identity);
            else assets.AppendCockpitBodyItems(source,slot,identity,0);
            for(auto& item:source)item.space=kSpaceWorld;
            if(std::getenv("GT2_AUDIT_GEOMETRY")) {
                const auto prefix=out/(id+(native?"-gt4-":"-gt2-")+std::to_string(setting));
                std::ofstream data(prefix.string()+"-source.csv");data<<"x,y,z,flags\n";
                for(const auto& d:source)for(const auto& v:renderer.VerticesForAudit(d))data<<v.pos[0]<<','<<v.pos[1]<<','<<v.pos[2]<<','<<v.flags<<'\n';
                std::ofstream cameraData(prefix.string()+"-camera.txt");cameraData<<eye[0]<<' '<<eye[1]<<' '<<eye[2]<<'\n';
                for(const auto& p:fit.cowl)cameraData<<p[0]<<' '<<p[1]<<' '<<p[2]<<'\n';
            }
            const auto uncut=render(source,-8);
            renderer.clearColor[0]=0;renderer.clearColor[1]=1;renderer.clearColor[2]=0;
            const auto uncutGreen=render(source,-8);
            renderer.clearColor[0]=1;renderer.clearColor[1]=0;renderer.clearColor[2]=1;
            auto marked=front;
            holes=MarkCockpitSeamHoles(front,uncut,uncutGreen,marked,size,eye,fit);
            // The curb/grass can rise through the body during suspension travel.
            // Even then it must never cover occupied interior surfaces.
            std::vector<SceneVertex> ground;
            const float groundY=fit.floorY+.12f;
            for(const auto& p:std::array<std::array<float,3>,6>{{{-8,groundY,-8},{8,groundY,-8},{8,groundY,8},{-8,groundY,-8},{8,groundY,8},{-8,groundY,8}}}) {
                SceneVertex v{};std::copy(p.begin(),p.end(),v.pos);v.color[1]=1;ground.push_back(v);
            }
            renderer.SetVertices(0,ground);DrawItem road;road.vertexCount=6;road.space=kSpaceWorld;std::copy_n(identity,16,road.mvp);
            auto withRoad=items;withRoad.push_back(road);
            const std::vector<DrawItem> interior(items.begin()+bodyEnd,items.end());
            const auto down=render(withRoad,-70),reference=render(interior,-70);size_t grass=0;
            for(size_t p=0;p<down.size();p+=4)if(down[p]<3 && down[p+1]>250 && down[p+2]<3 &&
                !(reference[p]>250 && reference[p+1]<3 && reference[p+2]>250))++grass;
            seatFailures+=badSeat;seamFailures+=holes!=0;groundFailures+=grass!=0;
            failures+=badSeat || holes || grass;views+=4;
            csv<<id<<','<<(native?"gt4":"gt2")<<','<<setting<<','<<eye[1]<<','<<eye[2]<<','<<fit.floorY<<','<<fit.rearWindowZ<<','<<width<<','<<height<<','<<drop<<','<<back<<','<<holes<<','<<grass<<'\n';
            if(badSeat || holes || grass || std::getenv("GT2_AUDIT_CAPTURE")) {
                const auto stem=id+"-"+(native?"gt4":"gt2")+"-"+std::to_string(setting);
                gt2::WritePngRgba((out/(stem+"-front.png")).string(),size,size*2,front);
                gt2::WritePngRgba((out/(stem+"-floor.png")).string(),size,size*2,down);
                if(holes){gt2::WritePngRgba((out/(stem+"-marked.png")).string(),size,size*2,marked);gt2::WritePngRgba((out/(stem+"-uncut.png")).string(),size,size*2,uncut);}
            }
            (void)bodyEnd;
        }
        csv.flush();std::cout<<id<<' '<<(native?"gt4":"gt2")<<" failures "<<failures<<'\n'<<std::flush;
    }
    std::ofstream(out/"gate.json")<<"{\"cockpits\":"<<cars<<",\"eye_views\":"<<views<<",\"failures\":"<<failures<<",\"seat_failures\":"<<seatFailures<<",\"seam_failures\":"<<seamFailures<<",\"ground_failures\":"<<groundFailures<<"}\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
