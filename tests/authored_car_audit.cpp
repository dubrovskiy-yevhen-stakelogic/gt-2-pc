#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include "gt2view/vr_driving_visuals.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <fstream>
#include <iostream>
#include "authored_mirror_audit.h"
using namespace gt2view;
static void Require(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
int main(int argc,char** argv){try {
    if(argc!=4 && argc!=5)throw std::runtime_error("disc gt4-pack output [car-id]");
    const std::string carId=argc==5?argv[4]:"nr32n";
    const std::filesystem::path out=argv[3];std::filesystem::create_directories(out);
    gt2::gt4ModelSetRoot=argv[2];gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    const auto source=ReadAuthoredCar(AuthoredCarManifest(carId),carId);
    Require(source.fit.eye[0]<0 && source.controls[0].center[0]<0,"R32 must retain the donor's left-hand driving position");
    {
        const auto& wheel=source.controls[0];const auto m=AuthoredControlMatrix(wheel,-.4f);
        auto top=wheel.center;top[1]+=.15f;
        const float x=m[12]+m[0]*top[0]+m[4]*top[1]+m[8]*top[2];
        Require(x>wheel.center[0],"right steering input must move the upper rim right");
    }
    Require(source.mesh.TriangleCount()*3<=SceneRenderer::kModelSetVertexStride,"game LOD exceeds vertex budget");
    Device device;VkContext context(device.existing);constexpr uint32_t width=960,height=640;
    VkSceneRenderer renderer(context,{width,height},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);renderer.CreateStereoTarget({width,height},VK_FORMAT_R8G8B8A8_UNORM,true);
    {
        VrDrivingVisuals visuals(renderer);gt2::vr::DrivingController driving;gt2::vr::TrackedControllers tracking;
        gt2::vr::DrivingSettings settings;settings.mode=1;
        const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};std::vector<DrawItem> draws;
        visuals.Append(draws,tracking,driving,settings,identity);
        Require(draws.size()==1,"generated VR wheel missing for ordinary cars");
        draws.clear();settings.authoredWheel=true;settings.cockpitWheelAxis=source.controls[0].axis;
        visuals.Append(draws,tracking,driving,settings,identity);
        Require(draws.empty(),"generated VR wheel overlaps authored steering wheel");
        tracking.gripValid[0]=true;tracking.gripPose[0].orientation[3]=1;
        visuals.Append(draws,tracking,driving,settings,identity);
        Require(draws.size()==1 && draws[0].firstVertex>=SceneRenderer::kVrDrivingVertexBase+8192,"authored wheel suppresses tracked hand visual");
        // Sweep the actual render matrices, including the authored wheel tilt.
        // A dot product against an orthogonal axis previously flipped their
        // handedness unpredictably as floating-point roundoff changed sign.
        size_t handSamples=0;
        for(const auto axis:{std::array<float,3>{0,0,1},source.controls[0].axis}) {
            settings.cockpitWheelAxis=axis;settings.cockpitWheelCenter=source.controls[0].center;
            const auto basis=gt2::vr::DrivingWheelBasis(settings);const auto center=gt2::vr::DrivingWheelCenter(settings);
            auto pose=[&](float angle){for(int h=0;h<2;++h){tracking.gripValid[h]=true;tracking.grip[h]=1;
                for(size_t k=0;k<3;++k)tracking.gripPose[h].position[k]=center[k]+(h?1.f:-1.f)*.18f*(basis[0][k]*std::cos(angle)+basis[1][k]*std::sin(angle));}};
            driving.Reset();pose(0);driving.Update(tracking,settings,true,0);
            std::array<std::array<float,16>,2> previous{};bool havePrevious=false;
            for(int step=0;step<=640;++step){const float angle=1.39f*std::sin(float(step)*6.283185307f/640);
                pose(angle);driving.Update(tracking,settings,true,1.f/90);draws.clear();visuals.Append(draws,tracking,driving,settings,identity);
                Require(draws.size()==2 && driving.Grabbed(0) && driving.Grabbed(1),"hand sweep lost wheel grip");
                for(size_t h=0;h<2;++h){const auto& m=draws[h].mvp;
                    const float determinant=m[0]*(m[5]*m[10]-m[6]*m[9])-m[4]*(m[1]*m[10]-m[2]*m[9])+m[8]*(m[1]*m[6]-m[2]*m[5]);
                    Require(std::isfinite(determinant) && std::abs(determinant-1)<.0001f,"wheel hand reflected/upside-down during turn");
                    if(havePrevious)for(size_t col=0;col<3;++col){float dot=0;for(size_t k=0;k<3;++k)dot+=m[col*4+k]*previous[h][col*4+k];Require(dot>.999f,"wheel hand orientation jumps between adjacent poses");}
                    std::copy(m,m+16,previous[h].begin());++handSamples;
                }havePrevious=true;
            }
        }
        std::ofstream(out/"hand-continuity.json")<<"{\"pass\":true,\"handMatrices\":"<<handSamples<<",\"wheelTilts\":2,\"bothHands\":true}\n";
    }
    SceneAssets assets(renderer,vol,true);
    if(std::getenv("GT2_AUDIT_AUTHORED_COURSE")) {
        const auto track=gt2::ParseTrack(vol.Read("crsobj/seatt_s.tro"));
        assets.UseTrack("seatt_s",track);
    }
assets.SetGt4Enabled(true);const int slot=assets.UseCar(carId);
    Require(slot>=0 && assets.HasAuthoredCar(slot),"authored runtime path not selected");
    const auto fit=assets.SlotCockpitFit(slot);Require(fit.eye==source.fit.eye,"authored eye was refitted");
    gt2::camera::CameraProjection camera{};const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    gt2game::PlaceCockpitEye(camera,identity,fit,{},true);
    for(size_t i=0;i<3;++i)Require(std::abs(camera.eye[i]-fit.eye[i])<1e-6f,"default camera shifted the authored eye");
    ProceduralCockpit generated(renderer);std::vector<DrawItem> forbidden;
    generated.Append(forbidden,fit,identity,0,0,0,0,true);generated.AppendMirror(forbidden,identity);
    Require(forbidden.empty(),"procedural geometry was appended to a frozen car");
    std::map<std::array<float,3>,int> expected,actual;
    for(const auto& p:source.mesh.primitives) {
        const auto lamps=p.material>=0?source.mesh.materials.at(size_t(p.material)).lampStates:0;
        if(lamps && !(lamps&1u))continue;
        for(auto i:p.indices)++expected[p.vertices.at(i).position];
    }
    std::vector<DrawItem> items;assets.AppendCockpitBodyItems(items,slot,identity,0);assets.AppendAuthoredControls(items,slot,identity,0,0,0);
    const auto base=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride;
    for(const auto& draw:items)if(draw.firstVertex>=base && draw.firstVertex<base+SceneRenderer::kModelSetVertexStride)
        for(const auto& v:renderer.VerticesForAudit(draw))++actual[{v.pos[0],v.pos[1],v.pos[2]}];
    Require(expected==actual,"runtime clipped, moved, duplicated or generated cockpit vertices");
    // Exterior glass replaces attributes of the same triangles; the cabin must
    // retain its original material and the exterior must not add a second layer.
    for(const auto& draw:items)for(const auto& v:renderer.VerticesForAudit(draw))
        Require((v.flags & (kModelReflection|kModelBlend))!=(kModelReflection|kModelBlend),"exterior glazing leaked into cabin");
    std::vector<DrawItem> outside;assets.AppendCarItems(outside,slot,identity,0,0,true);
    size_t glazed=0;
    for(const auto& draw:outside)for(const auto& v:renderer.VerticesForAudit(draw))
        if((v.flags & (kModelReflection|kModelBlend))==(kModelReflection|kModelBlend)) {
            ++glazed;Require(draw.blend==kBlendAlpha && draw.stpPass==3,"glass was submitted as opaque");
            Require(expected.contains({v.pos[0],v.pos[1],v.pos[2]}),"glass geometry moved or was generated");
        }
    Require(glazed>0,"exterior glazing was not submitted");

    for(const auto& control:source.controls)for(float angle:{-.7f,0.f,1.2f}) {
        const auto m=AuthoredControlMatrix(control,angle);
        for(size_t r=0;r<3;++r){float value=m[12+r];for(size_t k=0;k<3;++k)value+=m[k*4+r]*control.center[k];Require(std::abs(value-control.center[r])<1e-5f,"control pivot moved during rotation");}
    }
    std::vector<uint8_t> rest,exteriorRest;
    CheckAuthoredMirrors(renderer,assets,slot,source,width,height,out);
    Require(!source.controls[1].nodes.empty() && !source.controls[2].nodes.empty(),"working speedometer and tachometer required");
    bool glass=false,lamps=false;
    for(const auto& material:source.mesh.materials){glass|=material.alphaBlend;lamps|=material.lampStates!=0;}
    Require(glass && lamps,"restored glazing and lamp states required");
    VrDrivingVisuals handVisuals(renderer);
    for(int view=0;view<13;++view) {
        const bool exterior=view==5 || view==6 || view==9;auto eye=fit.eye;float yaw=0,pitch=-12;
        if(view==2){yaw=-70;pitch=-8;}if(view==3){yaw=70;pitch=-8;}if(view==4){yaw=180;pitch=-8;}
        if(view==10){yaw=35;pitch=28;}
        if(exterior){eye={-3.4f,2.2f,4.6f};yaw=35;pitch=-18;}
        if(view==6){eye={-3.4f,2.2f,-4.6f};yaw=145;pitch=-18;}
        StereoViews views;
        for(int e=0;e<2;++e){auto at=eye;at[0]+=e?.032f:-.032f;if(exterior && std::getenv("GT2_AUDIT_AUTHORED_COURSE"))for(size_t k=0;k<3;++k)at[k]-=eye[k];auto vp=View(at,yaw,pitch);
            const float framing=std::tan(47.5f*3.14159265f/180)/std::tan(27.5f*3.14159265f/180);
            for(size_t c=0;c<4;++c){vp[c*4]*=framing*float(height)/width;vp[c*4+1]*=framing;}
            std::copy(vp.begin(),vp.end(),views.worldVP[e]);}renderer.SetStereoViews(views);
        std::fill_n(renderer.clearColor,3,.36f);std::vector<DrawItem> draws;
        if(exterior)assets.AppendCarItems(draws,slot,identity,0,view==9?1u:0u,true);
        else {assets.AppendCockpitBodyItems(draws,slot,identity,0);assets.AppendAuthoredControls(draws,slot,identity,(view==1 || view==12)?.6f:0,(view==1 || view==7)?160.f:0,(view==1 || view==8)?5000.f:0);}
        if(view>=11){gt2::vr::DrivingSettings settings;settings.mode=1;settings.authoredWheel=true;settings.cockpitWheelAxis=source.controls[0].axis;settings.cockpitWheelCenter=source.controls[0].center;settings.wheelRadiusCm=int(std::lround(source.fit.nativeWheel[3]*100));
            const auto basis=gt2::vr::DrivingWheelBasis(settings);gt2::vr::TrackedControllers tracking;gt2::vr::DrivingController driving;
            auto setPose=[&](float angle){for(int h=0;h<2;++h){tracking.gripValid[h]=true;tracking.grip[h]=1;tracking.gripPose[h].orientation[3]=1;for(size_t k=0;k<3;++k)tracking.gripPose[h].position[k]=source.controls[0].center[k]+(h?1.f:-1.f)*source.fit.nativeWheel[3]*(basis[0][k]*std::cos(angle)+basis[1][k]*std::sin(angle));}};
            setPose(0);driving.Update(tracking,settings,true,0);setPose(view==12?.6f:0);driving.Update(tracking,settings,true,0);handVisuals.Append(draws,tracking,driving,settings,identity);
        }
        for(auto& draw:draws) {
            draw.space=kSpaceWorld;
            if(exterior && std::getenv("GT2_AUDIT_AUTHORED_COURSE"))for(size_t k=0;k<3;++k)draw.mvp[12+k]-=eye[k];
        }
        SortModelTransparency(draws);
        auto frame=renderer.CaptureStereoForAudit(draws,draws.size());
        if(view==0)rest=frame;
        if(view==5)exteriorRest=frame;
        if(view==1){size_t changed=0;for(size_t i=0;i<frame.size();i+=4)changed+=frame[i]!=rest[i]||frame[i+1]!=rest[i+1]||frame[i+2]!=rest[i+2];Require(changed>100,"wheel/instrument animation did not change the render");}
        if(view==7 || view==8){size_t changed=0;for(size_t i=0;i<frame.size();i+=4)changed+=frame[i]!=rest[i]||frame[i+1]!=rest[i+1]||frame[i+2]!=rest[i+2];Require(changed>12 && changed<25000,"individual instrument must move independently");}
        if(view==9){size_t changed=0;for(size_t i=0;i<frame.size();i+=4)changed+=frame[i]!=exteriorRest[i]||frame[i+1]!=exteriorRest[i+1]||frame[i+2]!=exteriorRest[i+2];Require(changed>12,"restored brake lamps did not change state");}
        gt2::WritePngRgba((out/("view-"+std::to_string(view)+".png")).string(),int(width),int(height*2),frame);
    }
    std::ofstream(out/"checks.json")<<"{\"immutableGeometry\":true,\"explicitLeftEye\":true,\"noProceduralFurniture\":true,\"controlPivotsStable\":true,\"independentSpeedAndRpm\":true,\"brakeLamps\":true,\"stereoViews\":13,\"sourceTriangles\":"<<source.mesh.TriangleCount()<<",\"headsetTested\":false}\n";
    std::cout<<"PASS authored R32 runtime, geometry unchanged, independent instruments, brake lamps, thirteen stereo views\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
