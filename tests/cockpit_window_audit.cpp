#include "offscreen_car_audit.h"
#include "window_mask_audit.h"
#include "gt2view/scene_assets.h"
#include "car_audit_catalogue.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const bool list=argc==5 && std::string(argv[4])=="--list";
    const auto ids=AuditCarCatalogue(vol,list?4:argc,argv);gt2::ModelSet pack(gt2::gt4ModelSetRoot);
    if(list){
        std::ofstream catalogue(out/"catalogue.csv");catalogue<<"car,path\n";
        for(const auto& id:ids){
            if(!vol.IsExpansionAsset("carobj/"+id+".cdo"))catalogue<<id<<",gt2\n";
            if(pack.Has(id))catalogue<<id<<",gt4\n";
        }
        return 0;
    }
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{320,320},VK_FORMAT_R8G8B8A8_UNORM);
    // Nearest samples and one raster sample separate real tint from filtered
    // edge coverage. The same interior is rendered against two known skies.
    RenderOptions options;options.msaa=1;options.smoothTextures=false;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({320,320},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::ofstream csv(out/"windows.csv");csv<<"car,path,yaw,pitch,clear_pixels,tinted_pixels,largest_tint_core\n";size_t paths=0,failures=0,missing=0;
    std::ofstream errors(out/"errors.csv");errors<<"car,path,error\n";
    for(const auto& id:ids)for(bool gt4:{false,true}){
        if(gt4 && !pack.Has(id))continue;
        if(!gt4 && vol.IsExpansionAsset("carobj/"+id+".cdo"))continue;
        try {
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(gt4);const int slot=assets.UseCar(id);
        if(slot<0 || (gt4 && !assets.UsesGt4(slot)))throw std::runtime_error("missing car/model "+id);
        const auto fit=assets.SlotCockpitFit(slot);if(!fit.valid)throw std::runtime_error("invalid cockpit fit");
        ProceduralCockpit cabin(renderer);std::vector<DrawItem> items;
        assets.AppendCockpitBodyItems(items,slot,identity,0);
        // This gate measures glazing, not tyre or headlamp-lens coverage. The
        // separate surface audit keeps wheels and all six lamp states. Never
        // drop a complete lamp callback: some callbacks also carry windows.
        std::vector<DrawItem> wheels;assets.AppendCarWheels(wheels,slot,identity,0);
        if(items.size()<wheels.size())throw std::runtime_error("wheel audit prefix missing");
        for(size_t i=0;i<wheels.size();++i)if(items[i].firstVertex!=wheels[i].firstVertex || items[i].vertexCount!=wheels[i].vertexCount)
            throw std::runtime_error("wheel audit prefix changed");
        items.erase(items.begin(),items.begin()+ptrdiff_t(wheels.size()));
        cabin.Append(items,fit,identity,0,0,0,1,false,0,gt2game::CockpitSeatBack({}),nullptr,!assets.HasNativeGt4Interior(slot));
        SortModelTransparency(items);
        assets.AppendSideMirrorItems(items,slot,identity);
        for(auto& item:items)item.space=kSpaceWorld;
        if(std::getenv("GT2_AUDIT_GEOMETRY")){
            std::ofstream geometry(out/(id+(gt4?"-gt4":"-gt2")+"-geometry.csv"));
            geometry<<"draw,blend,x,y,z,flags,page,clut,u,v\n";size_t draw=0;
            for(const auto& item:items){
                for(const auto& vertex:renderer.VerticesForAudit(item)){
                    std::array<float,3> world{};
                    for(size_t k=0;k<3;++k)world[k]=item.mvp[k]*vertex.pos[0]+item.mvp[4+k]*vertex.pos[1]+item.mvp[8+k]*vertex.pos[2]+item.mvp[12+k];
                    geometry<<draw<<','<<item.blend<<','<<world[0]<<','<<world[1]<<','<<world[2]<<','<<vertex.flags<<','<<vertex.page<<','<<vertex.clut<<','<<vertex.texel[0]<<','<<vertex.texel[1]<<'\n';
                }++draw;
            }
        }
        auto mirrorSource=Checker(renderer);mirrorSource.space=kSpaceMirrorSource;items.push_back(mirrorSource);
        gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,{});
        // Put the controlled background just beyond the passenger cell. This
        // retains every submitted window triangle, while transparent nose/tail
        // lenses of open concepts are outside this glazing experiment. The
        // separate elevated surface gate retains the entire body and lamps.
        float front=fit.cowl[0][2];for(const auto& point:fit.cowl)front=std::min(front,point[2]);
        front=std::min(front-.30f,camera.eye[2]-.85f);
        std::vector<SceneVertex> backdrop;
        for(const auto& xy:std::array<std::array<float,2>,6>{{{-100,-100},{100,-100},{100,100},{-100,-100},{100,100},{-100,100}}}){
            SceneVertex v{};v.pos[0]=xy[0];v.pos[1]=xy[1];v.pos[2]=front;backdrop.push_back(v);
        }
        const float sill=std::min({fit.frontCowl[0][1],fit.frontCowl[1][1],fit.rearCowl[0][1],fit.rearCowl[1][1]})-.02f;
        for(const auto& xz:std::array<std::array<float,2>,6>{{{-100,-100},{100,-100},{100,100},{-100,-100},{100,100},{-100,100}}}){
            SceneVertex v{};v.pos[0]=xz[0];v.pos[1]=sill;v.pos[2]=xz[1];backdrop.push_back(v);
        }
        DrawItem background;background.firstVertex=mirrorSource.vertexCount;background.vertexCount=uint32_t(backdrop.size());background.space=kSpaceWorld;
        std::copy_n(identity,16,background.mvp);background.cabinDepth=1;
        items.insert(items.begin(),background);
        if(std::getenv("GT2_DEBUG_CABIN"))std::printf("window eye %s %s %.6f %.6f %.6f\n",id.c_str(),gt4?"gt4":"gt2",camera.eye[0],camera.eye[1],camera.eye[2]);
        for(int yaw:{0,-35,35,-70,70,-110,110})for(int pitch:{0,15}){
            StereoViews views;for(int e=0;e<2;++e){auto vp=View({camera.eye[0]+(e?1:-1)*.032f,camera.eye[1],camera.eye[2]},float(yaw),float(pitch));std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
            renderer.SetStereoViews(views);std::array<std::vector<uint8_t>,2> frames;
            for(size_t phase=0;phase<2;++phase){
                const float level=phase?.75f:.25f;std::fill_n(renderer.clearColor,3,level);
                for(auto& vertex:backdrop)std::fill_n(vertex.color,3,level);
                renderer.SetVertices(background.firstVertex,backdrop);
                frames[phase]=renderer.CaptureStereoForAudit(items,items.size());
            }
            size_t clear=0,tinted=0;std::vector<uint8_t> mask(320*640);
            for(size_t p=0;p<mask.size();++p){int low=255,high=0;
                for(size_t c=0;c<3;++c){const int d=int(frames[1][p*4+c])-int(frames[0][p*4+c]);low=std::min(low,d);high=std::max(high,d);}
                clear+=low>=125;mask[p]=high>4 && low<122;
            }
            const auto cores=WindowTintCores(mask,320,320);
            tinted=cores.total;
            const bool visibleTint=cores.largest>16;
            // A completely opaque door/cap used to pass the tint comparison:
            // both backgrounds were equally invisible. Side-facing views need
            // a measurable opening as well (0.25% of the two-eye frame).
            const bool blocked=std::abs(yaw)==70 && pitch==0 && clear<512;
            failures+=visibleTint || blocked;
            const auto* capturePose=std::getenv("GT2_AUDIT_CAPTURE_SCENE");
            if(visibleTint && capturePose && (std::string(capturePose)=="all" ||
               std::string(capturePose)==std::to_string(yaw)+","+std::to_string(pitch))) {
                auto captured=items;const auto vp=View({camera.eye[0]-.032f,camera.eye[1],camera.eye[2]},float(yaw),float(pitch));
                for(auto& item:captured){float m[16]{};for(int col=0;col<4;++col)for(int row=0;row<4;++row)for(int k=0;k<4;++k)m[col*4+row]+=vp[k*4+row]*item.mvp[col*4+k];std::copy_n(m,16,item.mvp);}
                renderer.SaveCapture((out/(id+(gt4?"-gt4-":"-gt2-")+std::to_string(yaw)+"-"+std::to_string(pitch)+".gt2cap")).string(),captured,captured.size());
            }
            if(visibleTint){auto marked=frames[1];for(size_t p=0;p<mask.size();++p)if(mask[p]){marked[p*4]=255;marked[p*4+1]=0;marked[p*4+2]=255;}
                gt2::WritePngRgba((out/(id+(gt4?"-gt4-":"-gt2-")+std::to_string(yaw)+"-"+std::to_string(pitch)+"-tint.png")).string(),320,640,marked);}
            csv<<id<<','<<(gt4?"gt4":"gt2")<<','<<yaw<<','<<pitch<<','<<clear<<','<<tinted<<','<<cores.largest<<'\n';
            if(std::getenv("GT2_AUDIT_CAPTURE") || visibleTint || blocked)gt2::WritePngRgba((out/(id+(gt4?"-gt4-":"-gt2-")+std::to_string(yaw)+"-"+std::to_string(pitch)+".png")).string(),320,640,frames[1]);
        }
        csv.flush();if(++paths%20==0)std::cout<<paths<<" paths\n"<<std::flush;
        }catch(const std::exception& e){
            // One unavailable import must not hide failures later in the catalogue.
            // It remains a failed, unrendered path, never a synthetic passing view.
            ++missing;++failures;std::string message=e.what();
            std::replace(message.begin(),message.end(),',',';');std::replace(message.begin(),message.end(),'\n',' ');
            errors<<id<<','<<(gt4?"gt4":"gt2")<<','<<message<<'\n';errors.flush();
            std::cerr<<"Unrendered "<<id<<": "<<message<<'\n';
        }
    }
    std::ofstream(out/"gate.json")<<"{\"paths\":"<<paths<<",\"failures\":"<<failures<<",\"missing_models\":"<<missing<<"}\n";
    std::cout<<paths<<" paths, "<<failures<<" tinted views\n";return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
