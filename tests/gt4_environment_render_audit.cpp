// Actual owned course maps, actual native car passes, fresh GPU pixels at every
// sampled position and both sides of roof transitions. No XR session is opened.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace gt2view;
int main(int argc,char** argv){try {
    if(argc<4)throw std::runtime_error("disc pack output-under-work [course...]");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];gt2::gt4ModelSetEnabled=true;
    const auto table=gt2::ParseCourseInfo(vol.Read(".crsinfo"));std::vector<std::string> names;
    if(argc>4)for(int i=4;i<argc;++i)names.emplace_back(argv[i]);
    else for(const auto& file:vol.Files())if(file.path.starts_with("crsobj/") && file.path.ends_with(".tro.gz")) {
        const auto name=file.path.substr(7,file.path.size()-14);if(table.FindByFileName(name)>=0)names.push_back(name);
    }
    if(names.empty())throw std::runtime_error("no owned courses discovered");
    Device device;VkContext context(device.existing);VkSceneRenderer renderer(context,{512,384},VK_FORMAT_R8G8B8A8_UNORM);
    RenderOptions options;options.smoothTextures=true;renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({512,384},VK_FORMAT_R8G8B8A8_UNORM,true);
    std::ofstream csv(out/"environments.csv");csv<<"course,chunk,step,covered,map_sha1,nonblack_pixels,green_gain,frame_gpu_ms\n";
    size_t courseCount=0,total=0,transitions=0;bool failed=false;
    for(const auto& name:names) {
        const auto track=gt2::ParseTrack(vol.Read("crsobj/"+name+".tro"));
        if(track.chunks.empty())throw std::runtime_error("empty course "+name);
        // Audit placement uses ray/triangle intersections in the actual render
        // road. A physics contact query needs a valid moving-car chunk and can
        // walk forever for an interpolated point outside a licence course.
        std::vector<std::array<std::array<float,3>,3>> groundTriangles;
        for(const auto& chunk:track.chunks) {
            std::vector<bool> physical(chunk.road.polygons.size(),false);
            for(const auto& cell:chunk.surfaceGrid.cells)for(auto pi:cell)if(pi<physical.size())physical[pi]=true;
            for(size_t pi=0;pi<physical.size();++pi)if(physical[pi]) {
                const auto& p=chunk.road.polygons[pi];std::array<std::array<float,3>,4> v{};
                for(size_t k=0;k<(p.IsQuad()?4u:3u);++k)v[k]=gt2::TrackVertexToWorld(chunk,chunk.road.vertices.at(p.vertex[k]));
                groundTriangles.push_back({v[0],v[1],v[p.IsQuad()?3:2]});if(p.IsQuad())groundTriangles.push_back({v[1],v[2],v[3]});
            }
        }
        const auto dest=out/name;std::filesystem::create_directories(dest);
        SceneAssets assets(renderer,vol,false);assets.SetGt4Enabled(true);assets.UseTrack(name,track);
        const int slot=assets.UseCar("dvpgn");if(slot<0 || !assets.UsesGt4(slot))throw std::runtime_error("native Viper missing");
        bool first=true,previous=false;std::string previousHash;size_t dynamic=0,samples=0;
        for(size_t ci=0;ci<track.chunks.size();++ci) {
            // License/open courses contain extrapolated approach/exit chunks
            // with no drivable geometry. They are not positions in the race.
            if(track.chunks[ci].road.polygons.empty())continue;
            const auto& a=track.chunks[ci].origin;const auto& b=track.chunks.at(track.chunks[ci].next).origin;
            const float dx=float((double(b[0])-a[0])/65536.),dz=float((double(b[2])-a[2])/65536.);
            const int steps=std::max(1,int(std::ceil(std::sqrt(dx*dx+dz*dz)/2.f)));
            for(int step=0;step<steps;++step) {
                std::array<float,3> car{};for(size_t k=0;k<3;++k)car[k]=float((a[k]+(double(b[k])-a[k])*step/steps)/65536.);
                float bestHeight=car[1],bestError=1e30f;
                for(const auto& v:groundTriangles) {
                    const double ax=v[1][0]-v[0][0],az=v[1][2]-v[0][2],bx=v[2][0]-v[0][0],bz=v[2][2]-v[0][2];
                    const double determinant=ax*bz-az*bx;if(std::abs(determinant)<1e-9)continue;
                    const double x=car[0]-v[0][0],z=car[2]-v[0][2],u=(x*bz-z*bx)/determinant,w=(ax*z-az*x)/determinant;
                    if(u<-.001 || w<-.001 || u+w>1.001)continue;
                    const float h=float(v[0][1]+u*(v[1][1]-v[0][1])+w*(v[2][1]-v[0][1]));
                    const float error=std::abs(h-car[1]);if(error<bestError){bestError=error;bestHeight=h;}
                }
                car[1]=bestHeight;
                auto probe=car;probe[1]+=.8f;const bool covered=assets.PaintCoveredForAudit(probe);
                const bool boundary=!first && covered!=previous;
                const bool capture=first || boundary || (ci%16==0 && step==0);
                if(capture && std::getenv("GT2_DEBUG_NATIVE_ENV"))std::printf("Course %s chunk %zu car %.3f %.3f %.3f groundError %.3f\n",name.c_str(),ci,car[0],car[1],car[2],bestError);
                previous=covered;if(!capture)continue;
                const float yaw=std::atan2(float(track.chunks[ci].direction[0]),-float(track.chunks[ci].direction[2]));
                const std::array<float,3> camera{car[0]+std::cos(yaw)*3.2f,car[1]+1.6f,car[2]-std::sin(yaw)*3.2f};
                std::array<float,16> model{};model[0]=model[10]=std::cos(yaw);model[2]=-std::sin(yaw);model[8]=std::sin(yaw);model[5]=model[15]=1;
                for(size_t k=0;k<3;++k)model[12+k]=car[k]-camera[k];
                const auto vp=View({0,0,0},-90.f-yaw*180.f/3.14159265359f,-8.f);
                StereoViews views;for(int e=0;e<2;++e)std::copy(vp.begin(),vp.end(),views.worldVP[e]);renderer.SetStereoViews(views);
                std::vector<DrawItem> items;assets.AppendCarItems(items,slot,model.data(),0,0);
                for(auto& item:items)item.space=kSpaceWorld;SortModelTransparency(items);
                assets.UpdatePaintProbe(probe);
                const auto image=dest/(std::to_string(ci)+"-"+std::to_string(step));
                renderer.clearColor[0]=renderer.clearColor[1]=renderer.clearColor[2]=.05f;
                renderer.DrawStereo(items,image.string()+"-on.png",items.size());renderer.WaitFrame();
                const double gpu=renderer.GpuMilliseconds();const auto pixels=renderer.NativeEnvironmentForAudit();
                const auto hash=gt2::Sha1Hex(pixels);size_t nonblack=0;
                for(size_t i=0;i<pixels.size();i+=4)if(pixels[i] || pixels[i+1] || pixels[i+2])++nonblack;
                if(!nonblack) {
                    size_t f=0;for(auto face:renderer.NativeEnvironmentItemsForAudit()) {
                        for(auto& item:face)item.space=kSpaceScreen;
                        renderer.Draw(face,(dest/("empty-"+std::to_string(ci)+"-face-"+std::to_string(f++)+".png")).string(),face.size());renderer.WaitFrame();
                    }throw std::runtime_error("empty local environment "+name);
                }
                if(!previousHash.empty() && previousHash!=hash)++dynamic;
                if(boundary){++transitions;if(hash==previousHash)throw std::runtime_error("unchanged actual environment at roof transition "+name);}
                previousHash=hash;first=false;
                std::vector<uint8_t> sheet(size_t(64)*3*128*4);
                for(size_t f=0;f<3;++f)for(size_t y=0;y<128;++y)
                    std::copy_n(pixels.data()+(f*64*128+y*64)*4,64*4,sheet.data()+(y*192+f*64)*4);
                gt2::WritePngRgba(image.string()+"-environment.png",192,128,sheet);
                auto diffuse=items;diffuse.erase(std::remove_if(diffuse.begin(),diffuse.end(),[](const auto& item){return NativeEnvironmentBlend(item.blend);}),diffuse.end());
                renderer.DrawStereo(diffuse,image.string()+"-off.png",diffuse.size());renderer.WaitFrame();
                const auto on=gt2::ReadPngFile(image.string()+"-on.png"),off=gt2::ReadPngFile(image.string()+"-off.png");
                double green=0;size_t body=0;
                for(size_t i=0;i<on.rgba.size();i+=4)if(off.rgba[i]>35 && off.rgba[i+1]>35 && off.rgba[i+2]>35) {
                    const auto excess=[](const auto* p){return double(p[1])-.5*(p[0]+p[2]);};
                    green+=excess(on.rgba.data()+i)-excess(off.rgba.data()+i);++body;
                }
                const double gain=body?green/(255.*double(body)):0;
                if(!body || gain>.08){std::cerr<<"Excess car tint "<<name<<' '<<gain<<'\n';failed=true;}
                csv<<name<<','<<ci<<','<<step<<','<<covered<<','<<hash<<','<<nonblack<<','<<gain<<','<<gpu<<'\n';csv.flush();
                ++samples;++total;
            }
        }
        if(!samples)throw std::runtime_error("no drivable course samples "+name);
        if(samples>1 && !dynamic)throw std::runtime_error("environment remains static "+name);
        ++courseCount;std::cout<<"Environment "<<name<<": "<<samples<<" actual GPU maps, "<<dynamic<<" changes\n"<<std::flush;
    }
    std::cout<<courseCount<<" courses, "<<total<<" actual environments, "<<transitions<<" roof transitions\n";return failed?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
