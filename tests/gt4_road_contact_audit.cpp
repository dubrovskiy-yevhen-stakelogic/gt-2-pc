// Catalogue-wide contact check against the owned Seattle road and real GT2 setup.
#include "offscreen_car_audit.h"
#include "gt2view/race_road_contact.h"
#include "gt2formats/car_params.h"
#include "gt2formats/car_info.h"
#include "gt2formats/gtmode_tables.h"
#include "game/sim/disc_data.h"
#include <fstream>
#include <iostream>
#include <memory>
#include <chrono>
using namespace gt2;
using namespace gt2view;
namespace {
std::array<float,16> Matrix(const sim::CarPose& p) {
    std::array<float,16> m{};m[15]=1;
    for(size_t r=0;r<3;++r){m[12+r]=p.worldPosition[r]/65536.f;for(size_t c=0;c<3;++c)m[c*4+r]=p.rotation[r][c]/4096.f;}
    return m;
}
std::array<float,3> Transform(const float* m,const float* p) {
    std::array<float,3> q{};for(size_t r=0;r<3;++r)q[r]=m[12+r]+m[r]*p[0]+m[4+r]*p[1]+m[8+r]*p[2];return q;
}
}
int main(int argc,char** argv){try{
    if(argc<4)throw std::runtime_error("disc pack work-output [cars...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    DiscImage disc(argv[1]);GtfsVolume vol(disc);gt4ModelSetRoot=argv[2];
    const auto manifest=json::ReadFile((gt4ModelSetRoot/"manifest.json").string());
    const auto nativeRoot=std::getenv("GT2_GT4_NATIVE")?std::filesystem::path(std::getenv("GT2_GT4_NATIVE")):gt4ModelSetRoot/"native-v16";
    const auto nativeWheels=json::ReadFile((nativeRoot/"wheels.json").string());
    const bool stockModels=std::getenv("GT2_AUDIT_STOCK")!=nullptr;
    std::vector<std::string> ids;
    if(argc>4)for(int i=4;i<argc;++i)ids.emplace_back(argv[i]);
    else if(stockModels){
        const auto cars=CarInfoDirectory::Load(vol);
        for(size_t i=0;i<cars.Count();++i){
            const auto id=UnpackCarId(cars.BaseAt(i).carId);
            if(!vol.IsExpansionAsset("carobj/"+id+".cdo"))ids.push_back(id);
        }
    }else for(const auto& [id,value]:manifest.Require("cars").Members()){(void)value;ids.push_back(id);}
    const auto tables=CarParamTables::Load(vol);const auto exe=LoadExeImage(disc),overlay=LoadOverlayImage(disc,0);
    const auto arcadeTables=CarParamTables::Load(vol,"carparam/usa_arcade_data.dat");
    const auto tro=vol.Read("crsobj/seattle.tro");const auto track=ParseTrack(tro);
    const auto extras=sim::BuildCourseExtras(track,tro);const sim::NativeCourse course(track,extras);
    sim::ShellState shellState;shellState.gameMode=2;
    const auto constants=sim::LoadSimConstants(overlay,exe,{},shellState,false);
    const auto courseData=sim::BuildRaceCourseData(track,extras,ParseTrackRaceData(tro),overlay,false);
    const auto templates=LoadWheelTemplates(exe);const auto dishes=LoadWheelDishDepths(exe);
    // Pick actual ascending/descending course sections, not a plane inferred
    // from the model's own tyres (which would make burial pass by construction).
    size_t up=0,down=0;double highest=0,lowest=0;
    for(size_t i=0;i+1<track.chunks.size();++i){
        const auto& a=track.chunks[i].origin;const auto& b=track.chunks[i+1].origin;
        const double length=std::hypot(double(b[0])-a[0],double(b[2])-a[2]);
        if(length<8*65536. || length>60*65536.)continue;
        const double grade=(double(b[1])-a[1])/length;
        if(grade>highest){highest=grade;up=i;}if(grade<lowest){lowest=grade;down=i;}
    }
    if(highest<.05 || lowest>-.05)throw std::runtime_error("Seattle hill witnesses missing");
    std::cout<<"Seattle slopes: "<<up<<" "<<highest<<", "<<down<<" "<<lowest<<'\n';
    const bool captures=std::getenv("GT2_AUDIT_ROAD_SHOTS")!=nullptr;
    const bool benchmark=std::getenv("GT2_AUDIT_ROAD_BENCH")!=nullptr;
    const bool driving=std::getenv("GT2_AUDIT_ROAD_DRIVE")!=nullptr;
    const size_t driveSamples=std::getenv("GT2_AUDIT_DRIVE_SAMPLES")?std::stoul(std::getenv("GT2_AUDIT_DRIVE_SAMPLES")):600;
    const uint32_t size=captures?768u:256u;
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    if(captures)renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    std::ofstream timing(out/"fit-timing.csv");timing.exceptions(std::ios::badbit|std::ios::failbit);timing<<"car,section,phase,mean_cpu_us\n";
    std::ofstream csv(out/"contacts.csv"),errors(out/"errors.csv"),shadows(out/"shadows.csv"),unconfigured(out/"unconfigured.csv");
    csv.exceptions(std::ios::badbit|std::ios::failbit);errors.exceptions(std::ios::badbit|std::ios::failbit);
    csv<<"car,section,phase,wheel,min_clearance,max_clearance,body_min,ride_height,physics_radius,native_radius,airborne,raw_min\n";
    errors<<"car,error\n";size_t failures=0,checked=0;
    shadows<<"car,section,phase,min_clearance,max_clearance,samples\n";
    unconfigured<<"car,reason\n";size_t noConfiguration=0;
    for(const auto& id:ids){try{
        CarConfig config{};
        const CarParamTables* selectedTables=&tables;
        if(stockModels){
            bool found=false;
            for(const auto* source:{&arcadeTables,&tables}){
                for(size_t row=0;row<source->RowCount(kTableRacingModify);++row){
                    const auto& rm=source->RowAs<RacingModifyRow>(kTableRacingModify,row);
                    if(rm.modelId!=PackCarId(id))continue;
                    try{config=StockCarConfig(*source,rm.carId);}catch(const std::runtime_error&){continue;}
                    config.racingModify=uint16_t(row);selectedTables=source;
                    config.downforce[0]=rm.downforceFront;config.downforce[1]=rm.downforceRear;found=true;break;
                }
                if(found)break;
            }
            if(!found){
                if(argc>4)throw std::runtime_error("no USA Arcade/Simulation physics configuration for model");
                unconfigured<<id<<",no USA Arcade/Simulation physics configuration\n";++noConfiguration;continue;
            }
        }else config=StockCarConfig(tables,PackCarId(id));
        auto params=BuildCarParams(vol,*selectedTables,config);
        const auto stock=ParseCarModel(vol.Read("carobj/"+id+".cdo"));const auto dimensions=BodyDimensionsOf(stock);
        SceneAssets assets(renderer,vol,false);assets.SetGt4Enabled(!stockModels);int slot=-1;
        if(captures)assets.UseTrack("seattle",track);
        size_t flightSamples=0,landings=0;bool wasFlying=false;
        for(size_t section=0;section<(driving?1u:5u);++section){
            sim::RaceSlot grid;
            if(section==0)grid=sim::SlotFromTrackGrid(track,course,0,sim::CarNoseOffset(dimensions.bboxFront,dimensions.scaleShift));
            else {
                const size_t index=section==2?down:up;const auto& a=track.chunks[index].origin;const auto& b=track.chunks[index+1].origin;
                sim::GridPlacement placement;
                for(size_t k=0;k<3;++k)placement.position[k]=placement.pole[k]=int32_t((int64_t(a[k])+b[k])/2);
                if(section>=3)for(size_t k=0;k<3;++k)placement.position[k]=placement.pole[k]=(section==3?a:b)[k];
                placement.startAngle=int32_t(std::lround(std::atan2(double(a[0])-b[0],double(a[2])-b[2])*4096/(2*3.141592653589793)));
                grid=sim::PlaceOnGrid(track,course,placement);
            }
            grid.entryKind=sim::kEntryPlayer1;grid.controlClass=driving?2:0;
            sim::RaceShellOptions options;options.countdown=false;
            auto race=std::make_unique<sim::RaceSim>();race->Setup(track,extras,constants,courseData,{params},{grid},options);
            sim::PadRecord pad{};pad.brake=1;
            for(size_t tick=0;tick<90;++tick)race->Step(&pad);
            for(size_t phase=0;phase<(driving?driveSamples:5u);++phase){
            // Settle, accelerate, then brake: exercise both suspension pitch
            // directions while staying inside this straight road witness.
            if(driving){for(size_t tick=0;tick<2;++tick)race->Step(&pad);}
            else if(phase && phase<3){pad.brake=phase==2;pad.throttle=phase==1;for(size_t tick=0;tick<30;++tick)race->Step(&pad);}
            const auto& body=race->CarAt(0).body;
            if(slot<0){
                SceneAssets::RaceWheelGeometry geometry;std::array<WheelAxleDims,2> dims;
                for(size_t axle=0;axle<2;++axle){
                    geometry.radius[axle]=body.wheelRadius[axle]/4096.f;geometry.rideHeight[axle]=body.rideHeight[axle]/4096.f;
                    dims[axle]=RaceWheelDims(body.wheelRadius[axle],body.rimRadius[axle],body.tyreHeight[axle],WheelDishDepth(config.word00,dishes));
                }
                const auto area=GenerateWheelArea(dims,templates);slot=assets.UseCar(id,"",&area,&geometry);
                if(slot<0 || assets.UsesGt4(slot)==stockModels)throw std::runtime_error("requested model unavailable");
            }
            auto model=Matrix(race->Pose(0));std::array<std::array<float,16>,4> wheels{};
            const auto visual=race->Wheels(0);
            for(size_t w=0;w<4;++w){
                const float centre[]{float((w&1)?body.halfTrack[w>>1]:-body.halfTrack[w>>1])/4096.f,visual[w].verticalOffset/4096.f,stock.wheels[w].x/4096.f};
                WheelModelMatrix(int(w),centre,visual[w].steerAngle,body.camber[w>>1],visual[w].rotation,wheels[w].data());
            }
            // Deterministic presentation stress cases: a 30 cm compression
            // with the raw tyre contact retained, and a one-metre airborne gap.
            // These supplement settled/accelerating/braking simulation states.
            if(!driving && phase==3){for(size_t r=0;r<3;++r)model[12+r]-=model[4+r]*.3f;for(auto& w:wheels)w[13]+=.3f;}
            if(!driving && phase==4)model[13]+=1.f;
            bool flying=true;for(const auto& wheel:body.wheels)flying&=(wheel.contactFlags&1)!=0;
            const bool justLanded=wasFlying && !flying;
            flightSamples+=flying;if(justLanded)++landings;wasFlying=flying;
            std::array<float,4> rawBottom{1e9f,1e9f,1e9f,1e9f};
            {
                std::vector<DrawItem> raw;assets.AppendCarWheels(raw,slot,model.data(),0,&wheels);
                size_t w=0;
                for(const auto& item:raw)if(item.firstVertex<SceneRenderer::kModelSetVertexBase){
                    for(const auto& v:renderer.VerticesForAudit(item))rawBottom[w]=std::min(rawBottom[w],RaceRoadClearance(course,uint16_t(body.chunkIndex),Transform(item.mvp,v.pos)));
                    ++w;
                }
            }
            if(benchmark){
                const auto start=std::chrono::steady_clock::now();
                for(int repeat=0;repeat<128;++repeat){auto m=model;auto w=wheels;FitRaceCarRoadContact(assets,slot,course,uint16_t(body.chunkIndex),m.data(),w);}
                const double us=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count()/128;
                timing<<id<<','<<section<<','<<phase<<','<<us<<'\n';
            }
            if(!std::getenv("GT2_AUDIT_NO_ROAD_FIT"))
                FitRaceCarRoadContact(assets,slot,course,uint16_t(body.chunkIndex),model.data(),wheels);
            auto clearance=[&](const std::array<float,3>& p){
                sim::ContactQuery query{};query.chunkIndex=uint16_t(body.chunkIndex);query.x=int32_t(std::lround(p[0]*65536));query.height=int32_t(std::lround(p[1]*65536));query.y=-int32_t(std::lround(p[2]*65536));
                sim::QueryContact(course.Surface(),query);
                if(query.surfaceHeight==INT32_MAX)throw std::runtime_error("road witness outside course section="+std::to_string(section)+" phase="+std::to_string(phase)+" point="+std::to_string(p[0])+":"+std::to_string(p[1])+":"+std::to_string(p[2]));
                return p[1]-query.surfaceHeight/65536.f;
            };
            auto ground=Matrix(race->VisualPose(0));GroundShadowMatrix(ground.data(),assets.SlotShadowHeight(slot));
            if(!std::getenv("GT2_AUDIT_NO_ROAD_SHADOW"))FitRaceCarRoadShadow(assets,slot,course,uint16_t(body.chunkIndex),ground.data());
            std::vector<DrawItem> items;assets.AppendCarItems(items,slot,model.data(),0,0,true,&wheels,ground.data());
            float shadowMin=1e9f,shadowMax=-1e9f;size_t shadowSamples=0;
            for(const auto& item:items)if(item.blend==2){
                const auto vertices=renderer.VerticesForAudit(item);
                for(size_t v=0;v+2<vertices.size();v+=3){
                    for(const auto& weights:std::array<std::array<float,3>,4>{{{1,0,0},{0,1,0},{0,0,1},{1.f/3,1.f/3,1.f/3}}}){
                        float p[3]{};for(size_t k=0;k<3;++k)for(size_t j=0;j<3;++j)p[k]+=vertices[v+j].pos[k]*weights[j];
                        const float h=clearance(Transform(item.mvp,p));shadowMin=std::min(shadowMin,h);shadowMax=std::max(shadowMax,h);++shadowSamples;
                    }
                }
            }
            shadows<<id<<','<<section<<','<<phase<<','<<shadowMin<<','<<shadowMax<<','<<shadowSamples<<'\n';
            float bodyMin=1e9f;
            for(const auto& item:items)if((stockModels?item.firstVertex==SceneAssets::kCarVertexBase:item.firstVertex>=SceneRenderer::kModelSetVertexBase && std::equal(model.begin(),model.end(),item.mvp)) && item.blend==kBlendOpaque)
                for(const auto& v:renderer.VerticesForAudit(item))if(v.pos[1]<.35f)bodyMin=std::min(bodyMin,clearance(Transform(item.mvp,v.pos)));
            items.clear();assets.AppendCarWheels(items,slot,model.data(),0,&wheels);
            std::erase_if(items,[](const auto& d){return d.firstVertex>=SceneRenderer::kModelSetVertexBase;});
            if(items.size()!=4)throw std::runtime_error("expected four presentation wheel slots");
            bool fail=bodyMin<-.025f || !shadowSamples || shadowMin<-.005f || shadowMax>.12f;
            for(size_t w=0;w<4;++w){
                float bottom=1e9f,top=-1e9f;for(const auto& v:renderer.VerticesForAudit(items[w])){const float h=clearance(Transform(items[w].mvp,v.pos));bottom=std::min(bottom,h);top=std::max(top,h);}
                const float radius=assets.SlotCockpitFit(slot).wheelWells[w][3];
                const bool airborne=(!driving && phase==4) || (body.wheels[w].contactFlags&1)!=0;
                csv<<id<<','<<section<<','<<phase<<','<<w<<','<<bottom<<','<<top<<','<<bodyMin<<','<<body.rideHeight[w>>1]/4096.f<<','<<body.wheelRadius[w>>1]/4096.f<<','<<radius<<','<<airborne<<','<<rawBottom[w]<<'\n';
                // Dynamic contact flags describe the simulation tyre, while
                // the visible pose may still be rebounding. Retain existing
                // positive clearance; reject any newly introduced floating.
                const float gapLimit=std::max(.06f,rawBottom[w]+.006f);
                fail|=bottom<-.025f || (!airborne && bottom>gapLimit);
                if(!driving && phase==4)fail|=bottom<.8f;
                // A missing regional GT4 pose must retain the real GT2 tyre,
                // not shrink it to conceal a body placed too low. Native poses
                // have their own source tyre dimensions instead of donor sizes.
                if(!stockModels && !nativeWheels.Require("cars").Get(id.c_str()))
                    fail|=std::abs(radius-body.wheelRadius[w>>1]/4096.f)>.01f;
            }
            if(fail){++failures;std::cout<<"CONTACT FAIL "<<id<<" section "<<section<<" phase "<<phase<<" body "<<bodyMin<<'\n';}
            if(captures && (phase==0 || (driving && justLanded))){
                const float eyeLocal[]{3.3f,1.1f,2.2f},targetLocal[]{0,.35f,0};
                const auto eye=Transform(model.data(),eyeLocal),target=Transform(model.data(),targetLocal);
                const float dx=target[0]-eye[0],dy=target[1]-eye[1],dz=target[2]-eye[2];
                const float yaw=std::atan2(dx,-dz)*180.f/3.141592653589793f,pitch=std::atan2(dy,std::hypot(dx,dz))*180.f/3.141592653589793f;
                const auto vp=View(eye,yaw,pitch);
                const std::array<float,16> identity{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
                const std::array<std::array<float,3>,3> axes{{{1,0,0},{0,-1,0},{0,0,-1}}};
                assets.UpdateCarReflection(slot,model.data(),axes);
                SceneAssets::TrackView tv;tv.eye=eye;tv.cameraChunk=body.chunkIndex;tv.extendedDistance=120;
                for(bool stereo:{false,true}){
                    std::vector<DrawItem> draws,blended;
                    assets.AppendTrackItems(draws,blended,stereo?identity.data():vp.data(),tv);
                    std::array<float,16> mvp{};
                    for(size_t c=0;c<4;++c)for(size_t r=0;r<4;++r)for(size_t k=0;k<4;++k)mvp[c*4+r]+=vp[k*4+r]*model[c*4+k];
                    std::array<float,16> shadowMvp{};
                    for(size_t c=0;c<4;++c)for(size_t r=0;r<4;++r)for(size_t k=0;k<4;++k)shadowMvp[c*4+r]+=vp[k*4+r]*ground[c*4+k];
                    assets.AppendCarItems(draws,slot,stereo?model.data():mvp.data(),2,0,true,&wheels,stereo?ground.data():shadowMvp.data());
                    draws.insert(draws.end(),blended.begin(),blended.end());SortModelTransparency(draws);
                    const auto path=(out/(id+"-section"+std::to_string(section)+(driving?"-step"+std::to_string(phase):"")+(stereo?"-stereo.png":"-mono.png"))).string();
                    if(stereo){
                        StereoViews views;for(auto& v:views.worldVP)std::copy(vp.begin(),vp.end(),v);
                        renderer.SetStereoViews(views);for(auto& d:draws)d.space=kSpaceWorld;
                        renderer.DrawStereo(draws,path,draws.size());
                    }else renderer.Draw(draws,path,draws.size());
                    renderer.WaitFrame();
                }
            }
            }
        }
        if(driving)std::cout<<"Flight "<<id<<": samples="<<flightSamples<<", landings="<<landings<<'\n';
        ++checked;csv.flush();std::cout<<"Checked "<<checked<<'/'<<ids.size()<<" "<<id<<'\n'<<std::flush;
    }catch(const std::exception& e){++failures;errors<<id<<','<<e.what()<<'\n';std::cout<<"ERROR "<<id<<": "<<e.what()<<'\n'<<std::flush;}}
    std::cout<<"Road catalogue: "<<checked<<'/'<<ids.size()<<", no USA configuration="<<noConfiguration<<", failures="<<failures<<'\n';return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
