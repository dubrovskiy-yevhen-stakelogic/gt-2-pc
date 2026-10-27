// Production wheel/HUD/mirror composition and cowl continuity regressions.
#include "offscreen_car_audit.h"
#include "car_audit_catalogue.h"
#include "gt2view/scene_assets.h"
#include "gt2view/vr_driving_visuals.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include "mirror_copy_check.h"
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<4 || !std::getenv("GT2_AUDIT_COCKPIT"))throw std::runtime_error("GT2_AUDIT_COCKPIT=1; disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto relative=out.lexically_relative(std::filesystem::absolute("work"));
    if(relative.empty() || *relative.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    const auto ids=AuditCarCatalogue(vol,argc,argv);
    constexpr uint32_t size=384;
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    CheckMirrorCopy(renderer,size,out);
    VrDrivingVisuals wheel(renderer);gt2::vr::TrackedControllers tracking;gt2::vr::DrivingController driving;
    gt2::vr::DrivingSettings drivingSettings;drivingSettings.mode=1;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::vector<SceneVertex> ui;
    for(int y=0;y<5;++y)for(int x=0;x<5;++x)for(auto p:std::array<std::array<float,2>,6>{{{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}}}){
        SceneVertex v{};v.pos[0]=-.9f+x*.4f+p[0]*.12f;v.pos[1]=-.9f+y*.4f+p[1]*.12f;v.pos[2]=1;v.color[0]=1;v.flags=kOverlay;ui.push_back(v);
    }
    renderer.SetVertices(0,ui);DrawItem hud;hud.vertexCount=uint32_t(ui.size());hud.space=kSpaceScreen;std::copy_n(identity,16,hud.mvp);
    size_t failures=0,cases=0;
    std::ofstream csv(out/"presentation.csv");csv<<"car,path,native_interior,cowl_spike,join_spike,mirror_vertices,mirror_depth,wheel_reference,wheel_visible,hud_reference,hud_visible,cowl_added_pixels,mirror_reference,mirror_visible,seal_rise,cowl_bump\n";
    for(const auto& id:ids)for(bool native:{false,true}){
        if(native && !pack.Has(id))continue;
        if(!native && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(native);const int slot=assets.UseCar(id);
        if(slot<0 || (native && !assets.UsesGt4(slot)))throw std::runtime_error("missing car: "+id);
        const auto fit=assets.SlotCockpitFit(slot);if(!fit.valid)throw std::runtime_error("invalid fit: "+id);
        // Honda Z ACT uses an open race-car physics donor. Its donor eye is
        // inside the bonnet; measured native windshield begins near Z=-.53 m.
        if(native && id=="z0352" && (fit.eye[1]<.80f || fit.windshieldZ<-.80f))++failures;
        if(native && !assets.HasNativeGt4Interior(slot) && fit.eye[1]<fit.windshieldY+.06f)++failures;
        auto spike=[](const auto& points){float worst=0;for(size_t i=1;i+1<points.size();++i)
            worst=std::max(worst,points[i][1]-std::max(points[i-1][1],points[i+1][1]));return worst;};
        const float cowlSpike=spike(fit.cowl),joinSpike=fit.bonnetJoinValid?spike(fit.bonnetJoin):0;
        std::vector<DrawItem> mirrors;assets.AppendSideMirrorItems(mirrors,slot,identity);
        size_t mirrorVertices=0;bool mirrorDepth=true;for(const auto& d:mirrors){mirrorVertices+=d.vertexCount;mirrorDepth&=d.cabinDepth!=0;}
        size_t wheelReference=0,wheelVisible=0,hudReference=0,hudVisible=0,cowlAdded=0,mirrorReference=0,mirrorVisible=0,mirrorRasterized=0;
        int cowlBump=0;
        gt2game::CockpitSettings settings;
        if(const char* back=std::getenv("GT2_AUDIT_SEAT_BACK"))settings.seatBackCm=std::atoi(back);
        ProceduralCockpit furniture(renderer);
        std::vector<DrawItem> cabin;assets.AppendCockpitBodyItems(cabin,slot,identity,0);
        const size_t bodyItems=cabin.size();
        furniture.Append(cabin,fit,identity,0,0,0,1,false,gt2game::CockpitSeatHeight(fit,settings),gt2game::CockpitSeatBack(settings),nullptr,!assets.HasNativeGt4Interior(slot));
        for(auto& d:cabin)d.space=kSpaceWorld;
        float sealRise=0;
        bool linerAboveWindow=false;
        for(const auto& v:furniture.WheelLinersForAudit()){
            const float boundary=v.pos[2]<fit.eye[2]?std::min({fit.windshieldY,fit.frontCowl[0][1],fit.frontCowl[1][1]}):
                std::min({fit.rearWindowY,fit.rearCowl[0][1],fit.rearCowl[1][1]});
            linerAboveWindow|=v.pos[1]>boundary+.012f;
        }
        auto heightAt=[](const auto& curve,float x){for(size_t i=1;i<curve.size();++i)if(x<=curve[i][0] || i+1==curve.size()){
            const auto& a=curve[i-1];const auto& b=curve[i];const float t=std::clamp((x-a[0])/std::max(.001f,b[0]-a[0]),0.f,1.f);return a[1]+(b[1]-a[1])*t;}return 0.f;};
        if(!assets.HasNativeGt4Interior(slot))for(const auto& v:furniture.CowlSealForAudit()){
            const float envelope=std::max(heightAt(fit.cowl,v.pos[0]),heightAt(fit.bonnetJoin,v.pos[0]));
            sealRise=std::max(sealRise,v.pos[1]-envelope);
        }
        const bool render=argc>4 || std::getenv("GT2_AUDIT_PRESENTATION_RENDER_ALL");
        if(render){
            gt2::camera::CameraProjection camera{};
            gt2game::PlaceCockpitEye(camera,identity,fit,settings);
            const std::array<float,3> eye{camera.eye[0],camera.eye[1],camera.eye[2]};
            StereoViews views;for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;const auto vp=View(at,0,-25);
                std::copy(vp.begin(),vp.end(),views.worldVP[e]);std::copy_n(identity,16,views.hudVP[e]);}
            renderer.SetStereoViews(views);renderer.clearColor[0]=1;renderer.clearColor[1]=0;renderer.clearColor[2]=1;
            const float rigSeat[]{0,0,0};const auto controls=gt2game::CockpitControlsLocal(fit,settings,1,rigSeat);
            drivingSettings.cockpitWheelCenter=gt2game::CockpitWheelTrackingCenter(furniture.WheelCenter(),controls);
            std::vector<DrawItem> wheelItems;
            // Exactly the shared Append invoked by the Quest Android backend.
            wheel.Append(wheelItems,tracking,driving,drivingSettings,controls.data());
            const auto fixedHub=furniture.WheelCenter();
            for(int back:{-20,0,40})for(int height:{-20,0,20})for(float scale:{.8f,1.f,1.2f}){
                auto moved=settings;moved.seatBackCm=back;moved.seatHeightCm=height;
                std::vector<DrawItem> furnitureProbe;
                furniture.Append(furnitureProbe,fit,identity,0,0,0,1,false,gt2game::CockpitSeatHeight(fit,moved),
                                 gt2game::CockpitSeatBack(moved),nullptr,!assets.HasNativeGt4Interior(slot));
                if(furniture.WheelCenter()!=fixedHub)throw std::runtime_error("seat rebuild relocated dashboard wheel: "+id);
                const float calibratedSeat[]{.04f,-.03f,.06f};
                const auto parent=gt2game::CockpitControlsLocal(fit,moved,scale,calibratedSeat);
                auto anchored=drivingSettings;anchored.cockpitWheelCenter=gt2game::CockpitWheelTrackingCenter(fixedHub,parent);
                // Saved free-wheel offsets must be irrelevant in the cockpit.
                anchored.wheelHeightCm=0;anchored.wheelDistanceCm=75;
                std::vector<DrawItem> draws;wheel.Append(draws,tracking,driving,anchored,parent.data());
                if(draws.empty())throw std::runtime_error("missing anchored wheel");
                for(size_t axis=0;axis<3;++axis)if(std::abs(draws.front().mvp[12+axis]-fixedHub[axis])>1e-6f)
                    throw std::runtime_error("wheel moved relative to dashboard: "+id);
            }
            // Negative control: the rejected player-relative route must move
            // with seating, so this test cannot pass by ignoring the transform.
            auto unanchored=drivingSettings;unanchored.cockpitWheelCenter.reset();
            auto shifted=settings;shifted.seatHeightCm+=20;shifted.seatBackCm+=40;
            const auto shiftedParent=gt2game::CockpitControlsLocal(fit,shifted,1,rigSeat);
            std::vector<DrawItem> beforeShift,afterShift;
            wheel.Append(beforeShift,tracking,driving,unanchored,controls.data());
            wheel.Append(afterShift,tracking,driving,unanchored,shiftedParent.data());
            if(std::abs(afterShift.front().mvp[13]-beforeShift.front().mvp[13]-.20f)>1e-6f ||
               std::abs(afterShift.front().mvp[14]-beforeShift.front().mvp[14]-.40f)>1e-6f)
                throw std::runtime_error("wheel attachment negative control did not reproduce drift: "+id);
            std::cout<<id<<(native?" gt4":" gt2")<<" dashboard anchor: 27 settings fixed; old-route drift 20/40 cm\n";
            // Rebuild default furniture after probing seat travel: the renderer
            // owns one shared cabin vertex range.
            cabin.resize(bodyItems);
            furniture.Append(cabin,fit,identity,0,0,0,1,false,gt2game::CockpitSeatHeight(fit,settings),
                             gt2game::CockpitSeatBack(settings),nullptr,!assets.HasNativeGt4Interior(slot));
            for(auto& d:cabin)d.space=kSpaceWorld;
            const auto reference=renderer.CaptureStereoForAudit(wheelItems,wheelItems.size());
            auto complete=cabin;complete.insert(complete.end(),wheelItems.begin(),wheelItems.end());
            const auto composed=renderer.CaptureStereoForAudit(complete,complete.size());
            for(size_t p=0;p<reference.size();p+=4)if(!(reference[p]>250 && reference[p+1]<3 && reference[p+2]>250)){
                ++wheelReference;int error=0;for(size_t k=0;k<3;++k)error+=std::abs(int(reference[p+k])-int(composed[p+k]));if(error<8)++wheelVisible;
            }
            const auto uiReference=renderer.CaptureStereoForAudit({hud},0);
            complete.push_back(hud);const auto withHud=renderer.CaptureStereoForAudit(complete,complete.size()-1);
            for(size_t p=0;p<uiReference.size();p+=4)if(uiReference[p]>250 && uiReference[p+1]<3 && uiReference[p+2]<3){++hudReference;if(withHud[p]>250 && withHud[p+1]<3 && withHud[p+2]<3)++hudVisible;}
            const auto stem=id+(native?"-gt4":"-gt2");
            const bool saveFrames=std::getenv("GT2_AUDIT_CAPTURE") || id=="dvpgn" || id=="bucrn" || id=="bmmgn" ||
                wheelReference<1000 || wheelVisible*100<wheelReference*85 || hudReference==0 || hudVisible!=hudReference;
            if(saveFrames){
            gt2::WritePngRgba((out/(stem+"-wheel.png")).string(),size,size*2,composed);
            gt2::WritePngRgba((out/(stem+"-hud.png")).string(),size,size*2,withHud);
            auto forwardItems=cabin;
            furniture.AppendMirror(forwardItems,identity,1.f);
            forwardItems.insert(forwardItems.end(),wheelItems.begin(),wheelItems.end());
            for(auto& d:forwardItems)d.space=kSpaceWorld;
            for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;const auto forwardVP=View(at,0,0);
                std::copy(forwardVP.begin(),forwardVP.end(),views.worldVP[e]);}
            renderer.SetStereoViews(views);
            gt2::WritePngRgba((out/(stem+"-dashboard-forward.png")).string(),size,size*2,
                renderer.CaptureStereoForAudit(forwardItems,forwardItems.size()));
            }
            // Compare the windshield opening with the uncut authored body.
            // New trim may overlap the seam by a pixel, but must not grow
            // several pixels into an otherwise open view above the bonnet.
            for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;const auto vp=View(at,0,-8);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
            renderer.SetStereoViews(views);
            std::vector<DrawItem> source;assets.AppendCarItems(source,slot,identity,0,0);
            for(auto& d:source)d.space=kSpaceWorld;
            const auto exterior=renderer.CaptureStereoForAudit(source,source.size());
            const auto closure=renderer.CaptureStereoForAudit(cabin,cabin.size());
            // Isolate the generated cowl closure. Pillars, wipers and the
            // dashboard project into the same band but are not bonnet humps.
            const auto& sealVertices=furniture.CowlSealForAudit();
            renderer.SetVertices(1024,sealVertices);
            DrawItem seal;seal.firstVertex=1024;seal.vertexCount=uint32_t(sealVertices.size());seal.space=kSpaceWorld;seal.cabinDepth=1;std::copy_n(identity,16,seal.mvp);
            const auto sealImage=renderer.CaptureStereoForAudit({seal},1);
            if(std::getenv("GT2_AUDIT_CAPTURE")){
                const std::vector<DrawItem> body(cabin.begin(),cabin.begin()+bodyItems);
                const std::vector<DrawItem> trim(cabin.begin()+bodyItems,cabin.end());
                gt2::WritePngRgba((out/(stem+"-body.png")).string(),size,size*2,renderer.CaptureStereoForAudit(body,body.size()));
                gt2::WritePngRgba((out/(stem+"-trim.png")).string(),size,size*2,renderer.CaptureStereoForAudit(trim,trim.size()));
            }
            auto isOpen=[](const auto& pixels,size_t p){return pixels[p]>250 && pixels[p+1]<3 && pixels[p+2]>250;};
            auto marked=closure;
            std::array<std::array<int,size>,2> addedColumns{};
            for(size_t e=0;e<2;++e)for(size_t x=size/4;x<size*3/4;++x){
                auto project=[&](const auto& point){float clip[4]{};for(size_t r=0;r<4;++r){clip[r]=views.worldVP[e][12+r];for(size_t k=0;k<3;++k)clip[r]+=views.worldVP[e][k*4+r]*point[k];}return std::array<float,2>{(clip[0]/clip[3]+1)*size*.5f,(clip[1]/clip[3]+1)*size*.5f};};
                float boundary=-1000;
                for(size_t i=1;i<fit.cowl.size();++i){const auto a=project(fit.cowl[i-1]),b=project(fit.cowl[i]);if(float(x)>=a[0] && float(x)<=b[0])boundary=a[1]+(b[1]-a[1])*(float(x)-a[0])/std::max(.001f,b[0]-a[0]);}
                for(size_t y=4;y+4<size;++y){
                    if(float(y)<boundary-20 || float(y)>boundary+2)continue;
                    bool pillar=false;
                    for(size_t side=0;side<2;++side){const auto a=project(fit.frontCowl[side]),b=project(fit.frontHeader[side]);
                        const float dx=b[0]-a[0],dy=b[1]-a[1],t=std::clamp(((float(x)-a[0])*dx+(float(y)-a[1])*dy)/std::max(.001f,dx*dx+dy*dy),0.f,1.f);
                        pillar|=std::hypot(float(x)-a[0]-t*dx,float(y)-a[1]-t*dy)<8.f;}
                    if(pillar)continue; // the restored A-pillar is not bonnet trim
                    const size_t p=((e*size+y)*size+x)*4;
                    if(isOpen(closure,p) || isOpen(sealImage,p))continue;
                    bool open=true;for(int dy=-3;dy<=3;++dy)for(int dx=-3;dx<=3;++dx)open&=isOpen(exterior,size_t(ptrdiff_t(p)+(dy*int(size)+dx)*4));
                    if(open){++cowlAdded;++addedColumns[e][x];marked[p]=255;marked[p+1]=255;marked[p+2]=0;}
                }
            }
            // A continuous flat overlap is allowed (its physical thickness is
            // checked independently above). Detect new local humps relative
            // to the authored silhouette instead of rejecting every seam pixel.
            for(const auto& columns:addedColumns)for(size_t x=14;x+14<size;++x){
                auto supported=[&](size_t at){return std::min({columns[at-1],columns[at],columns[at+1]});};
                cowlBump=std::max(cowlBump,supported(x)-std::max(supported(x-12),supported(x+12)));
            }
            if(saveFrames || cowlSpike>.035f || joinSpike>.035f || sealRise>.012f || linerAboveWindow){
                gt2::WritePngRgba((out/(stem+"-cowl.png")).string(),size,size*2,marked);
                gt2::WritePngRgba((out/(stem+"-source.png")).string(),size,size*2,exterior);
            }
            if(mirrorVertices){
                std::vector<SceneVertex> witness;
                for(auto p:std::array<std::array<float,2>,6>{{{-1,-1},{1,-1},{1,1},{-1,-1},{1,1},{-1,1}}}){SceneVertex v{};v.pos[0]=p[0];v.pos[1]=p[1];v.pos[2]=.9f;v.color[0]=1;witness.push_back(v);}
                DrawItem rear;rear.firstVertex=512;rear.vertexCount=6;rear.space=kSpaceMirrorSource;std::copy_n(identity,16,rear.mvp);
                for(auto& d:mirrors)d.space=kSpaceWorld;
                for(const auto pose:std::array<std::array<float,2>,4>{{{-50,0},{50,0},{-50,.08f},{50,.08f}}}){
                    const float yaw=pose[0];if(pose[1]>0 && mirrorVisible>=64)break;
                    for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;at[1]+=fit.openTop?pose[1]:std::min(pose[1],std::max(0.f,fit.roofY-.08f-eye[1]));const auto vp=View(at,yaw,-8);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}renderer.SetStereoViews(views);
                    auto isolated=mirrors;isolated.push_back(rear);auto combined=cabin;combined.insert(combined.end(),isolated.begin(),isolated.end());
                    auto body=std::vector<DrawItem>(cabin.begin(),cabin.begin()+bodyItems);body.insert(body.end(),isolated.begin(),isolated.end());
                    for(auto& v:witness){v.color[0]=1;v.color[1]=0;}renderer.SetVertices(512,witness);
                    const auto refRed=renderer.CaptureStereoForAudit(isolated,isolated.size());const auto fullRed=renderer.CaptureStereoForAudit(combined,combined.size());
                    const auto bodyRed=renderer.CaptureStereoForAudit(body,body.size());
                    for(auto& v:witness){v.color[0]=0;v.color[1]=1;}renderer.SetVertices(512,witness);
                    const auto refGreen=renderer.CaptureStereoForAudit(isolated,isolated.size());const auto fullGreen=renderer.CaptureStereoForAudit(combined,combined.size());
                    const auto bodyGreen=renderer.CaptureStereoForAudit(body,body.size());
                    if(std::getenv("GT2_AUDIT_CAPTURE")) {
                        gt2::WritePngRgba((out/(stem+(yaw<0?"-mirror-left-body.png":"-mirror-right-body.png"))).string(),size,size*2,bodyRed);
                        gt2::WritePngRgba((out/(stem+(yaw<0?"-mirror-left.png":"-mirror-right.png"))).string(),size,size*2,fullRed);
                        gt2::WritePngRgba((out/(stem+(yaw<0?"-mirror-left-reference.png":"-mirror-right-reference.png"))).string(),size,size*2,refRed);
                    }
                    for(size_t p=0;p<refRed.size();p+=4)if(refRed[p]>240 && refRed[p+1]<10 && refGreen[p]<10 && refGreen[p+1]>240){
                        ++mirrorRasterized;
                        if(bodyRed[p]>240 && bodyRed[p+1]<10 && bodyGreen[p]<10 && bodyGreen[p+1]>240){++mirrorReference;if(fullRed[p]>240 && fullRed[p+1]<10 && fullGreen[p]<10 && fullGreen[p+1]>240)++mirrorVisible;}
                    }
                }
            }
        }
        // Owned-disc witnesses with identifiable side-glass geometry that was
        // absent from the hand-written rearView material profiles in 0.8.5.
        const bool missingMirror=native && (id=="dvpgn" || id=="bucrn" || id=="z0322" || id=="z0352") && mirrorVertices==0;
        // Pixel differences remain diagnostics: wipers break the reference
        // silhouette and create false local peaks. The shape gate measures
        // the added geometry against the fitted body/window in world metres.
        const bool bad=missingMirror || linerAboveWindow || cowlSpike>.035f || joinSpike>.035f || sealRise>.012f || !mirrorDepth ||
            (render && (wheelReference<1000 || wheelVisible*100<wheelReference*85 || hudReference==0 || hudVisible!=hudReference ||
                // The isolated mirror mesh includes the far side hidden by
                // the car. Require a live visible patch, not visibility of
                // back-facing/occluded glass on the opposite side.
                (mirrorVertices && (mirrorRasterized<10 || (mirrorReference>=10 && mirrorVisible<std::min(size_t(64),std::max(size_t(8),mirrorReference/4)))))));
        failures+=bad;++cases;
        csv<<id<<','<<(native?"gt4":"gt2")<<','<<assets.HasNativeGt4Interior(slot)<<','<<cowlSpike<<','<<joinSpike<<','<<mirrorVertices<<','<<mirrorDepth<<','<<wheelReference<<','<<wheelVisible<<','<<hudReference<<','<<hudVisible<<','<<cowlAdded<<','<<mirrorReference<<','<<mirrorVisible<<','<<sealRise<<','<<cowlBump<<'\n';csv.flush();
        std::cout<<id<<(native?" gt4":" gt2")<<" failures "<<failures<<'\n'<<std::flush;
    }
    std::ofstream(out/"gate.json")<<"{\"cases\":"<<cases<<",\"failures\":"<<failures<<"}\n";
    return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
