#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try{
    if(argc<5)throw std::runtime_error("disc pack output car...");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto relative=out.lexically_relative(std::filesystem::absolute("work"));
    if(relative.empty() || *relative.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);gt2::gt4ModelSetRoot=argv[2];
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{1024,1024},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.smoothTextures=true;renderer.SetOptions(options);
    renderer.CreateStereoTarget({1024,1024},VK_FORMAT_R8G8B8A8_UNORM,true);
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    bool failed=false;
    std::ofstream contact(out/"contacts.csv");contact<<"car,ride_height,travel,rotation,camber,wheel,old_bottom,new_bottom\n";
    for(int arg=4;arg<argc;++arg){
        const auto stock=gt2::ParseCarModel(vol.Read("carobj/"+std::string(argv[arg])+".cdo"));
        for(float height:{.075f,.15f,.25f}) {
            SceneAssets contactAssets(renderer,vol,false);contactAssets.SetGt4Enabled(true);
            SceneAssets::RaceWheelGeometry geometry{{stock.wheelRadiusFront/4096.f,stock.wheelRadiusRear/4096.f},{height,height}};
            const int oldSlot=contactAssets.UseCar(argv[arg],"before");
            const int newSlot=contactAssets.UseCar(argv[arg],"after",nullptr,&geometry);
            if(oldSlot<0 || newSlot<0 || !contactAssets.UsesGt4(newSlot))throw std::runtime_error("contact fixture fallback");
            const auto anchors=contactAssets.SlotWheelCentres(newSlot);
            for(float travel:{-.06f,0.f,.06f})for(unsigned rotation:{0u,128u,256u,512u})for(int camber:{0,32}) {
                std::array<std::array<float,16>,4> models;
                for(size_t w=0;w<4;++w) {
                    auto centre=anchors[w];centre[1]=geometry.radius[w>>1]-height+travel;
                    gt2::WheelModelMatrix(int(w),centre.data(),0,int16_t(camber),uint16_t(rotation),models[w].data());
                }
                std::array<std::array<float,4>,2> bottoms{};
                for(size_t variant=0;variant<2;++variant) {
                    std::vector<DrawItem> draws;contactAssets.AppendCarWheels(draws,variant?newSlot:oldSlot,identity,0,&models);
                    std::erase_if(draws,[](const auto& d){return d.firstVertex>=SceneRenderer::kModelSetVertexBase;});
                    for(size_t w=0;w<4;++w) {
                        const auto& d=draws.at(w);float bottom=1e9f;
                        for(const auto& v:renderer.VerticesForAudit(d))bottom=std::min(bottom,d.mvp[1]*v.pos[0]+d.mvp[5]*v.pos[1]+d.mvp[9]*v.pos[2]+d.mvp[13]);
                        bottoms[variant][w]=bottom+height-travel;
                    }
                }
                for(size_t w=0;w<4;++w) {
                    const float radius=contactAssets.SlotCockpitFit(newSlot).wheelWells[w][3];
                    // The original low-poly tyre is inscribed in its radius;
                    // its polygon sag may leave a small gap, never penetration.
                    failed|=bottoms[1][w]<-.0001f || bottoms[1][w]>radius*.09f;
                    contact<<argv[arg]<<','<<height<<','<<travel<<','<<rotation<<','<<camber<<','<<w<<','<<bottoms[0][w]<<','<<bottoms[1][w]<<'\n';
                }
            }
        }
        SceneAssets assets(renderer,vol,true);assets.SetGt4Enabled(true);const int slot=assets.UseCar(argv[arg]);
        if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("GT4 reflection fixture unavailable");
        const auto track=gt2::ParseTrack(vol.Read("crsobj/seattle.tro"));assets.UseTrack("seattle",track);
        const std::array<float,3> eye{2.4f,1.0f,1.4f};
        const auto view=View(eye,-60.f,-10.f);
        const std::array<std::array<float,3>,3> axes{{{1,0,0},{0,-1,0},{0,0,-1}}};
        for(int yaw:{0,90,180}) {
            const float angle=float(yaw)*3.14159265358979323846f/180.f;
            std::array<float,16> world{};world[0]=world[10]=std::cos(angle);world[2]=-std::sin(angle);world[8]=std::sin(angle);world[5]=world[15]=1;
            assets.UpdateCarReflection(slot,world.data(),axes);
            std::array<float,16> mvp{};
            for(size_t c=0;c<4;++c)for(size_t r=0;r<4;++r)for(size_t k=0;k<4;++k)mvp[c*4+r]+=view[k*4+r]*world[c*4+k];
            auto model=world;for(size_t k=0;k<3;++k)model[12+k]=-eye[k];
            for(unsigned samples:{1u,4u}) {
                options.msaa=samples;renderer.SetOptions(options);
                const auto prefix=std::string(argv[arg])+"-yaw"+std::to_string(yaw)+"-msaa"+std::to_string(samples);
                std::vector<DrawItem> items;assets.AppendCarItems(items,slot,mvp.data(),2,0);SortModelTransparency(items);
                if(yaw==0 && samples==1) {
                    renderer.Draw(items,(out/(std::string(argv[arg])+"-full.png")).string(),items.size());renderer.WaitFrame();
                }
                // Compare the native body/clearcoat. The separate GT2 rim uses
                // nearest alpha masks, whose edge texel selection can change
                // under the two equivalent matrix multiplication orders.
                std::erase_if(items,[](const auto& item){return item.firstVertex<SceneRenderer::kModelSetVertexBase;});
                const auto monoPath=out/(prefix+"-mono.png");
                renderer.Draw(items,monoPath.string(),items.size());renderer.WaitFrame();
                const auto mono=gt2::ReadPngFile(monoPath.string());
                StereoViews views;const auto vp=View({0,0,0},-60.f,-10.f);for(auto& v:views.worldVP)std::copy(vp.begin(),vp.end(),v);
                renderer.SetStereoViews(views);items.clear();assets.AppendCarItems(items,slot,model.data(),2,0);
                std::erase_if(items,[](const auto& item){return item.firstVertex<SceneRenderer::kModelSetVertexBase;});
                for(auto& item:items)item.space=kSpaceWorld;SortModelTransparency(items);
                for(bool multiview:{false,true}) {
                    renderer.CreateStereoTarget({1024,1024},VK_FORMAT_R8G8B8A8_UNORM,multiview);
                    const auto stereoPath=out/(prefix+(multiview?"-multiview.png":"-two-pass.png"));
                    renderer.DrawStereo(items,stereoPath.string(),items.size());renderer.WaitFrame();
                    const auto stereo=gt2::ReadPngFile(stereoPath.string());
                    if(mono.rgba.size()!=stereo.rgba.size())throw std::runtime_error("mismatched capture dimensions");
                    size_t different=0;int maximum=0;double total=0;
                    for(size_t i=0;i<mono.rgba.size();i+=4) {
                        int delta=0;for(size_t c=0;c<3;++c){const int d=std::abs(int(mono.rgba[i+c])-int(stereo.rgba[i+c]));delta=std::max(delta,d);total+=d;}
                        different+=delta>3;maximum=std::max(maximum,delta);
                    }
                    const double mean=total/(mono.rgba.size()/4*3);
                    // Subpixel edge coverage can differ between one composed
                    // matrix and two shader multiplies. Interior shading cannot.
                    failed|=different>128 || mean>.02;
                    std::cout<<prefix<<" multiview="<<multiview<<" pixels>3="<<different<<" max="<<maximum<<" mean="<<mean<<'\n'<<std::flush;
                }
            }
        }
    }
    std::cout<<"GT4 contact and reflection gate: "<<(failed?"FAIL":"PASS")<<'\n';return failed?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
