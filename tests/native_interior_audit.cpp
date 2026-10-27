// Owned-source cabin inventory and production-path stereo inspection.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<5)throw std::runtime_error("disc pack output-under-work ids...");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::ModelSet pack(argv[2]);
    std::ofstream csv(out/"interiors.csv");csv<<"car,native,open,wheel,x,y,z,triangles,primitives\n";
    for(int i=4;i<argc;++i){const auto car=pack.Read(argv[i]);const auto wheel=FindNativeCockpitWheel(car.cockpit);
        csv<<argv[i]<<','<<car.nativeInterior<<','<<car.openTop<<','<<wheel.valid;
        for(float v:wheel.center)csv<<','<<v;
        csv<<','<<car.cockpit.TriangleCount()<<','<<car.cockpit.primitives.size()<<'\n';
    }csv.close();
    if(std::getenv("GT2_AUDIT_INVENTORY_ONLY"))return 0;
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    Device device;VkContext context(device.existing);constexpr uint32_t size=384;
    VkSceneRenderer renderer(context,{size,size},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({size,size},VK_FORMAT_R8G8B8A8_UNORM,true);
    renderer.clearColor[0]=.65f;renderer.clearColor[1]=.75f;renderer.clearColor[2]=.85f;
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    for(int i=4;i<argc;++i){
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(true);const int slot=assets.UseCar(argv[i]);
        if(slot<0 || !assets.UsesGt4(slot) || !assets.HasNativeGt4Interior(slot))throw std::runtime_error(std::string("authored cabin not selected: ")+argv[i]);
        const auto fit=assets.SlotCockpitFit(slot);if(!fit.valid)throw std::runtime_error("invalid cabin fit");
        ProceduralCockpit furniture(renderer);std::vector<DrawItem> items;
        assets.AppendCockpitBodyItems(items,slot,identity,0);
        auto authored=items;
        furniture.Append(items,fit,identity,0,0,0,1,false,0,gt2game::CockpitSeatBack({}),nullptr,false);
        assets.AppendSideMirrorItems(items,slot,identity);for(auto& item:items)item.space=kSpaceWorld;
        assets.AppendSideMirrorItems(authored,slot,identity);for(auto& item:authored)item.space=kSpaceWorld;
        SortModelTransparency(authored);
        SortModelTransparency(items);
        gt2::camera::CameraProjection camera{};gt2game::PlaceCockpitEye(camera,identity,fit,{});
        std::cout<<argv[i]<<" actual eye "<<camera.eye[0]<<' '<<camera.eye[1]<<' '<<camera.eye[2]<<" roof "<<fit.roofY<<" open "<<fit.openTop<<'\n';
        if(fit.nativeWheel[3]>0){
            const float height=camera.eye[1]-fit.nativeWheel[1],reach=camera.eye[2]-fit.nativeWheel[2];
            if(height<.16f || height>.85f || reach<.20f || reach>.80f)throw std::runtime_error(std::string("eye/control calibration outside usable range: ")+argv[i]);
        }
        const auto source=pack.Read(argv[i]);
        const auto sourceWheel=FindNativeCockpitWheel(source.cockpit);
        bool failedAlphaCutout=false;
        for(size_t p=0;p<source.cockpit.primitives.size();++p)if(sourceWheel.primitives[p]){
            const auto& m=source.cockpit.materials.at(size_t(source.cockpit.primitives[p].material));
            if(m.alphaBlend && (m.nativeAlphaFail==1 || m.nativeAlphaFail==3) && m.baseColorImage>=0){
                const auto& image=source.cockpit.images.at(size_t(m.baseColorImage));
                size_t zero=0;for(size_t a=3;a<image.rgba.size();a+=4)zero+=image.rgba[a]==0;
                failedAlphaCutout|=zero*40>image.rgba.size();
            }
        }
        if(failedAlphaCutout){
            std::vector<DrawItem> original,replaced,controls;
            assets.AppendCockpitBodyItems(original,slot,identity,0,nullptr,false);
            assets.AppendCockpitBodyItems(replaced,slot,identity,0,nullptr,true);
            for(auto item:original)if(std::none_of(replaced.begin(),replaced.end(),[&](const auto& other){return item.firstVertex==other.firstVertex && item.vertexCount==other.vertexCount;})){item.space=kSpaceWorld;controls.push_back(item);}
            if(controls.empty())throw std::runtime_error("native controls were not replaceable");
            StereoViews views;for(int eye=0;eye<2;++eye){const auto vp=View({camera.eye[0]+(eye?.032f:-.032f),camera.eye[1],camera.eye[2]},0,-20);std::copy(vp.begin(),vp.end(),views.worldVP[eye]);}renderer.SetStereoViews(views);
            std::vector<SceneVertex> solid;
            for(const auto& item:controls)for(auto vertex:renderer.VerticesForAudit(item)){vertex.flags=0;vertex.color[0]=1;vertex.color[1]=vertex.color[2]=0;solid.push_back(vertex);}
            renderer.SetVertices(0,solid);DrawItem mask;mask.vertexCount=uint32_t(solid.size());mask.space=kSpaceWorld;std::copy_n(identity,16,mask.mvp);
            std::fill_n(renderer.clearColor,3,.2f);const auto reference=renderer.CaptureStereoForAudit({mask},1);
            const auto dark=renderer.CaptureStereoForAudit(controls,controls.size());
            std::fill_n(renderer.clearColor,3,.8f);const auto light=renderer.CaptureStereoForAudit(controls,controls.size());
            size_t samples=0,holes=0;
            for(size_t p=0;p<reference.size();p+=4)if(reference[p]>250 && reference[p+1]<3 && reference[p+2]<3){++samples;holes+=int(light[p+1])-int(dark[p+1])>140;}
            gt2::WritePngRgba((out/(std::string(argv[i])+"-control-alpha.png")).string(),size,size*2,light);
            std::cout<<argv[i]<<" alpha cutout samples "<<samples<<" holes "<<holes<<'\n';
            if(samples<100 || holes*100<samples*5)throw std::runtime_error("authored steering cutouts became opaque");
            renderer.clearColor[0]=.65f;renderer.clearColor[1]=.75f;renderer.clearColor[2]=.85f;
        }
        // A closed native cabin must not expose the world below its window
        // perimeter. The old seam audit observes only the centre of the cowl;
        // this mask also covers door/dashboard joints in side views.
        std::vector<SceneVertex> closedMask;
        if(!source.openTop) {
            std::vector<std::array<float,3>> perimeter;
            for(auto p:fit.cowl){p[1]-=.08f;perimeter.push_back(p);}
            for(int n=1;n<=16;++n){auto p=CockpitSideSillAt(fit,1,fit.frontCowl[1][2]+(fit.rearCowl[1][2]-fit.frontCowl[1][2])*n/16.f);p[1]-=.08f;perimeter.push_back(p);}
            auto rear=fit.rearCowl[0];rear[1]-=.08f;perimeter.push_back(rear);
            for(int n=15;n>=0;--n){auto p=CockpitSideSillAt(fit,0,fit.frontCowl[0][2]+(fit.rearCowl[0][2]-fit.frontCowl[0][2])*n/16.f);p[1]-=.08f;perimeter.push_back(p);}
            for(size_t n=0;n<perimeter.size();++n){
                const auto a=perimeter[n],b=perimeter[(n+1)%perimeter.size()];
                auto lowA=a,lowB=b;lowA[1]=lowB[1]=fit.floorY+.10f;
                for(const auto p:std::array<std::array<float,3>,6>{lowA,lowB,b,lowA,b,a}){
                    SceneVertex v{};std::copy(p.begin(),p.end(),v.pos);v.color[0]=1;closedMask.push_back(v);
                }
            }
        }
        renderer.SetVertices(0,closedMask);DrawItem closedWall;closedWall.vertexCount=uint32_t(closedMask.size());closedWall.space=kSpaceWorld;std::copy_n(identity,16,closedWall.mvp);
        size_t openSamples=0,blockedOpen=0,closedSamples=0,closedHoles=0;
        for(const auto pose:std::array<std::array<float,2>,9>{{{0,0},{0,-20},{-65,-15},{65,-15},{0,55},{0,-65},{180,-20},{-120,0},{120,0}}}){
            StereoViews views;for(int eye=0;eye<2;++eye){const auto vp=View({camera.eye[0]+(eye?.032f:-.032f),camera.eye[1],camera.eye[2]},pose[0],pose[1]);std::copy(vp.begin(),vp.end(),views.worldVP[eye]);}
            renderer.SetStereoViews(views);
            const auto frame=renderer.CaptureStereoForAudit(items,items.size());
            gt2::WritePngRgba((out/(std::string(argv[i])+"-"+std::to_string(int(pose[0]))+"-"+std::to_string(int(pose[1]))+".png")).string(),size,size*2,frame);
            if(source.openTop && pose[0]==0 && pose[1]==0) {
                const auto reference=renderer.CaptureStereoForAudit(authored,authored.size());
                size_t visible=0,blocked=0;
                for(size_t eye=0;eye<2;++eye)for(size_t y=size/10;y<size/3;++y)for(size_t x=size/10;x<size*9/10;++x) {
                    const size_t p=((eye*size+y)*size+x)*4;
                    if(std::abs(int(reference[p])-166)>2 || std::abs(int(reference[p+1])-191)>2 || std::abs(int(reference[p+2])-217)>2)continue;
                    ++visible;
                    blocked+=std::abs(int(frame[p])-int(reference[p]))+std::abs(int(frame[p+1])-int(reference[p+1]))+std::abs(int(frame[p+2])-int(reference[p+2]))>30;
                }
                std::cout<<argv[i]<<" open forward view "<<visible<<" blocked "<<blocked<<'\n';
                if(visible<1000 || blocked>visible/100)throw std::runtime_error("open cockpit forward view was enclosed");
            }
            if(!source.openTop) {
                const auto mask=renderer.CaptureStereoForAudit({closedWall},1);
                const auto covered=[&](size_t p){return mask[p]>250 && mask[p+1]<3 && mask[p+2]<3;};
                const auto background=[&](size_t p){return std::abs(int(frame[p])-166)<=2 && std::abs(int(frame[p+1])-191)<=2 && std::abs(int(frame[p+2])-217)<=2;};
                auto marked=frame;size_t holes=0;
                for(size_t eye=0;eye<2;++eye)for(size_t y=1;y+1<size;++y)for(size_t x=1;x+1<size;++x){
                    const size_t p=((eye*size+y)*size+x)*4;
                    if(!covered(p)||!covered(p-4)||!covered(p+4)||!covered(p-size*4)||!covered(p+size*4))continue;
                    ++closedSamples;
                    if(background(p)&&background(p-4)&&background(p+4)&&background(p-size*4)&&background(p+size*4)){
                        ++holes;marked[p]=marked[p+1]=255;marked[p+2]=0;
                    }
                }
                closedHoles+=holes;
                if(holes)gt2::WritePngRgba((out/(std::string(argv[i])+"-seam-"+std::to_string(int(pose[0]))+"-"+std::to_string(int(pose[1]))+".png")).string(),size,size*2,marked);
            }
            if(source.openTop && source.speedometer[3]==0) {
                // No dashboard exists to support added furniture on these
                // authored open chassis. Preserve the source's visible gaps.
                const auto reference=renderer.CaptureStereoForAudit(authored,authored.size());
                const auto background=[&](size_t p){return std::abs(int(reference[p])-166)<=2 &&
                    std::abs(int(reference[p+1])-191)<=2 && std::abs(int(reference[p+2])-217)<=2;};
                for(size_t eye=0;eye<2;++eye)for(size_t y=1;y+1<size;++y)for(size_t x=1;x+1<size;++x){
                    const size_t p=((eye*size+y)*size+x)*4;
                    if(!background(p)||!background(p-4)||!background(p+4)||!background(p-size*4)||!background(p+size*4))continue;
                    ++openSamples;int difference=0;for(size_t c=0;c<3;++c)difference+=std::abs(int(frame[p+c])-int(reference[p+c]));
                    blockedOpen+=difference>30;
                }
            }
        }
        if(source.openTop && source.speedometer[3]==0){
            std::cout<<argv[i]<<" authored open pixels "<<openSamples<<" blocked "<<blockedOpen<<'\n';
            if(openSamples<100 || blockedOpen>16)throw std::runtime_error("authored open chassis gaps were enclosed");
        }
        if(!source.openTop){
            std::cout<<argv[i]<<" closed wall samples "<<closedSamples<<" holes "<<closedHoles<<'\n';
            if(closedSamples<100 || closedHoles>16)throw std::runtime_error("closed native cabin has open door/dashboard seams");
        }
    }return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
