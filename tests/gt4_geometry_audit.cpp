// Production geometry with independent road and animated tyre witnesses.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt4_cabin_witness.h"

#include "gt2export/png_writer.h"
#include "../tools/gt2game/cockpit_camera.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv) {try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [ids...]");
    if(!std::getenv("GT2_AUDIT_WHEEL_COLOR"))throw std::runtime_error("GT2_AUDIT_WHEEL_COLOR=1 is required");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    std::vector<std::string> ids;
    if(argc>4)for(int i=4;i<argc;++i)ids.emplace_back(argv[i]);
    else {const auto manifest=gt2::json::ReadFile((gt2::gt4ModelSetRoot/"manifest.json").string());for(const auto& [id,e]:manifest.Require("cars").Members()){(void)e;ids.push_back(id);}}
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{256,192},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.msaa=2;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({256,192},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const bool capture=std::getenv("GT2_AUDIT_CAPTURE")!=nullptr;
    std::ofstream csv(out/"geometry.csv");csv<<"car,view,steer,road_pixels,wheel_pixels,road_leaks,wheel_leaks\n";
    bool failed=false;

    for(const auto& id:ids) {
        SceneAssets assets(renderer,vol,std::getenv("GT2_AUDIT_MENU_ASSETS")==nullptr);assets.SetGt4Enabled(true);const int slot=assets.UseCar(id);
        if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("native car unavailable: "+id);
        const auto fit=assets.SlotCockpitFit(slot);
        std::cout<<id<<" eye "<<fit.eye[0]<<' '<<fit.eye[1]<<' '<<fit.eye[2]<<" floor "<<fit.floorY<<'\n';
        for(const auto& w:fit.wheelWells)std::cout<<" well "<<w[0]<<' '<<w[1]<<' '<<w[2]<<' '<<w[3]<<' '<<w[4]<<'\n';
        // A real horizontal road, independent of cabin boundaries. It extends
        // beyond the car and remains legitimately visible through the windows.
        std::vector<SceneVertex> road;
        float ground=1e9f;for(const auto& w:fit.wheelWells)ground=std::min(ground,w[1]-w[3]);
        for(const auto& p:std::array<std::array<float,2>,6>{{{-30,-30},{30,-30},{30,30},{-30,-30},{30,30},{-30,30}}}) {
            SceneVertex v{};v.pos[0]=p[0];v.pos[1]=ground-.01f;v.pos[2]=p[1];v.color[0]=v.color[1]=1;road.push_back(v);
        }
        renderer.SetVertices(0,road);
        const auto maskVertices=Witness(fit,true);
        renderer.SetVertices(uint32_t(road.size()),maskVertices);
        DrawItem mask;mask.firstVertex=uint32_t(road.size());mask.vertexCount=uint32_t(maskVertices.size());
        std::copy_n(identity,16,mask.mvp);mask.space=kSpaceWorld;
        DrawItem roadItem;roadItem.vertexCount=uint32_t(road.size());std::copy_n(identity,16,roadItem.mvp);
        ProceduralCockpit cabin(renderer);
        std::array<std::vector<uint8_t>,12> referenceImages;
        for(int steering=-3;steering<=3;++steering) {
            std::array<std::array<float,16>,4> wheels;
            for(size_t w=0;w<4;++w) {
                auto centre=assets.SlotWheelCentres(slot)[w];
                if(std::abs(steering)>1)centre[1]+=(std::abs(steering)==2?.08f:-.08f);
                gt2::WheelModelMatrix(int(w),centre.data(),int16_t(w<2?std::clamp(steering,-1,1)*384:0),0,512,wheels[w].data());
            }
            std::vector<DrawItem> seated;assets.AppendCockpitBodyItems(seated,slot,identity,0,&wheels);
            cabin.Append(seated,fit,identity,float(steering),0,0,1,true,0,.15f,nullptr,!assets.HasNativeGt4Interior(slot));
            seated.push_back(roadItem);
            if(std::getenv("GT2_AUDIT_INJECT_CABIN_GAP"))
                std::erase_if(seated,[](const auto& i){return i.firstVertex>=SceneRenderer::kCockpitVertexBase && i.firstVertex<SceneRenderer::kCockpitVertexBase+SceneRenderer::kCockpitVertexLimit;});
            for(int pose=0;pose<12;++pose) {
                auto eye=fit.eye;eye[2]+=gt2game::CockpitEyeBack(fit,gt2game::CockpitSettings{},pose>=4 && pose<8);
                float yaw=0,pitch=0;auto items=seated;
                if(pose<8){yaw=float(pose%4)*90;pitch=pose<4?-55.f:0.f;}
                else {
                    eye={pose==8?3.f:pose==9?-3.f:0.f,pose==11?.05f:1.1f,pose<10?0.f:4.f};
                    yaw=pose==8?-90.f:pose==9?90.f:0.f;pitch=0;
                    items.clear();assets.AppendCarItems(items,slot,identity,0,0,true,&wheels);
                    if(pose==11)std::erase_if(items,[](const auto& i){return i.firstVertex<SceneRenderer::kModelSetVertexBase;});
                    else items.push_back(roadItem);
                }
                for(auto& item:items)item.space=kSpaceWorld;SortModelTransparency(items);
                StereoViews views;for(int e=0;e<2;++e){auto p=eye;p[0]+=(e?1:-1)*.032f;auto vp=View(p,yaw,pitch);std::copy(vp.begin(),vp.end(),views.worldVP[e]);}
                renderer.SetStereoViews(views);std::fill_n(renderer.clearColor,3,.12f);
                auto rgba=renderer.CaptureStereoForAudit(items,items.size());
                auto& expected=referenceImages[size_t(pose)];
                if(expected.empty() && pose<8) expected=renderer.CaptureStereoForAudit({mask},1);
                else if(expected.empty()) {
                    std::vector<DrawItem> panels;assets.AppendGt4PanelItemsForAudit(panels,slot,std::min(pose-8,2),identity);
                    expected=renderer.CaptureStereoForAudit(panels,panels.size());
                    // Filtering the opposite side must not expose bright inner
                    // triangles hidden by a dark wheel-arch closure. Intersect
                    // the flank witness with the complete opaque shell's image.
                    panels.clear();assets.AppendGt4PanelItemsForAudit(panels,slot,3,identity);
                    const auto shell=renderer.CaptureStereoForAudit(panels,panels.size());
                    for(size_t p=0;p<expected.size();p+=4)
                        if(std::max({shell[p],shell[p+1],shell[p+2]})<=80)
                            std::fill_n(expected.begin()+ptrdiff_t(p),3,uint8_t(0));
                }
                size_t yellow=0,magenta=0;for(size_t p=0;p<rgba.size();p+=4){if(rgba[p]>230 && rgba[p+1]>230 && rgba[p+2]<16)++yellow;if(rgba[p]>230 && rgba[p+1]<16 && rgba[p+2]>230)++magenta;}
                size_t roadLeaks=0,wheelLeaks=0;
                auto wheelPixel=[&](size_t p){return rgba[p]>230 && rgba[p+1]<16 && rgba[p+2]>230;};
                auto panel=[&](size_t p) {
                    if(pose<8)return expected[p]>230 && expected[p+1]>230 && expected[p+2]>230;
                    return std::max({expected[p],expected[p+1],expected[p+2]})>80;
                };
                for(size_t e=0;e<2;++e)for(size_t y=2;y+2<192;++y)for(size_t x=2;x+2<256;++x) {
                    const size_t p=((e*192+y)*256+x)*4;
                    if(!panel(p)||!panel(p-4)||!panel(p+4)||!panel(p-256*4)||!panel(p+256*4))continue;
                    if(pose<8 && rgba[p]>230 && rgba[p+1]>230 && rgba[p+2]<16)++roadLeaks;
                    if(wheelPixel(p) && wheelPixel(p-4) && wheelPixel(p+4) && wheelPixel(p-256*4) && wheelPixel(p+256*4))++wheelLeaks;
                }
                failed|=roadLeaks!=0 || wheelLeaks!=0;
                csv<<id<<','<<pose<<','<<steering<<','<<yellow<<','<<magenta<<','<<roadLeaks<<','<<wheelLeaks<<'\n';
                if(capture || roadLeaks || wheelLeaks){const auto dest=out/id;std::filesystem::create_directories(dest);gt2::WritePngRgba((dest/(std::to_string(pose)+"-"+std::to_string(steering)+".png")).string(),256,384,rgba);
                    if(roadLeaks || wheelLeaks)gt2::WritePngRgba((dest/("mask-"+std::to_string(pose)+".png")).string(),256,384,expected);
                }
            }
        }
        csv.flush();std::cout<<id<<" captured\n"<<std::flush;
    }
    return failed?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
