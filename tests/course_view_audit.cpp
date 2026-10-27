// Owned-data stereo course sweep at driving height, including VR billboards.
#include "offscreen_car_audit.h"
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include <chrono>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc<4)throw std::runtime_error("disc output-under-work course [stride]");
    const auto out=std::filesystem::absolute(argv[2]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);const std::string name=argv[3];
    if(name=="--list"){
        std::ofstream list(out/"courses.csv");list<<"course,chunks,backdrop,content_key\n";
        const auto info=gt2::ParseCourseInfo(vol.Read(".crsinfo"));std::vector<std::string> backgrounds;
        for(const auto& f:vol.Files())if(f.path.starts_with("bgsobj/") && f.path.ends_with(".bso.gz"))backgrounds.push_back(f.path);
        for(const auto& file:vol.Files())if(file.path.starts_with("crsobj/") && (file.path.ends_with(".tro") || file.path.ends_with(".tro.gz"))){
            const auto course=file.path.substr(7,file.path.find(".tro")-7);const auto bytes=vol.Read(file);const auto data=gt2::ParseTrack(bytes);
            if(data.chunks.empty())continue;
            std::string backdrop,key=gt2::Sha1Hex(bytes)+gt2::Sha1Hex(vol.Read("crsobj/"+course+".trp"));
            const int entry=info.FindByFileName(course);
            if(entry>=0){backdrop=gt2::BackdropNameAt(backgrounds,gt2::BackdropIndexOf(info.entries[size_t(entry)].rest));
                key+=gt2::Sha1Hex(vol.Read("bgsobj/"+backdrop+".bso"))+gt2::Sha1Hex(vol.Read("bgsobj/"+backdrop+".bsp"));
            }
            list<<course<<','<<data.chunks.size()<<','<<backdrop<<','<<key<<'\n';
        }return 0;
    }
    const auto track=gt2::ParseTrack(vol.Read("crsobj/"+name+".tro"));
    Device device;gt2view::VkContext context(device.existing);gt2view::VkSceneRenderer renderer(context,{512,384},VK_FORMAT_R8G8B8A8_UNORM);
    gt2view::RenderOptions options;options.msaa=(std::getenv("GT2_COURSE_ORDER_CHECK") || std::getenv("GT2_COURSE_WALL_CHECK"))?1:2;options.smoothTextures=!std::getenv("GT2_COURSE_WALL_CHECK");renderer.SetOptions(options);renderer.EnableDecodedTextures(true);
    renderer.CreateStereoTarget({512,384},VK_FORMAT_R8G8B8A8_UNORM,true);
    gt2view::SceneAssets assets(renderer,vol,false);assets.UseTrack(name,track);
    if(assets.DroppedCourseVertices())throw std::runtime_error("course vertex overflow");
    const size_t stride=argc>4?std::stoul(argv[4]):1;
    std::ofstream csv(out/"views.csv");csv<<"chunk,mode,draws,vertices,billboards,cpu_ms,capture_ms,order_changed_pixels,order_max_delta,late_opaque_before,late_opaque_after,road_witness_pixels,road_occluded_pixels\n";
    const size_t begin=argc>5?std::stoul(argv[5]):0,lastChunk=argc>6?std::min(track.chunks.size(),size_t(std::stoul(argv[6]))):track.chunks.size();
    const float lateral=argc>7?std::stof(argv[7]):0,height=argc>8?std::stof(argv[8]):1.5f;
    size_t failures=0,poses=0;
    for(size_t i=begin;i<lastChunk;i+=stride)for(int mode=0;mode<2;++mode){
        gt2view::SceneAssets::TrackView view;view.cameraChunk=int(i);view.glows=false;view.positionalBillboards=mode==0;
        view.extendedDistance=std::getenv("GT2_COURSE_DISTANCE")?std::stof(std::getenv("GT2_COURSE_DISTANCE")):-1;view.fullDetail=false;
        view.maxDetail=std::getenv("GT2_COURSE_MAX_DETAIL")!=nullptr;
        const auto& at=track.chunks[i].centre;const auto& next=track.chunks[(i+1)%track.chunks.size()].centre;
        const float fraction=std::getenv("GT2_COURSE_FRACTION")?std::clamp(std::stof(std::getenv("GT2_COURSE_FRACTION")),0.f,1.f):0.f;
        for(size_t k=0;k<3;++k)view.eye[k]=float((double(at[k])+(double(next[k])-at[k])*fraction)/65536.);view.eye[1]+=height;
        const float yaw=std::atan2(float(next[0]-at[0]),float(at[2]-next[2]))*180.f/3.14159265f+(std::getenv("GT2_COURSE_LOOK_BACK")?180.f:0.f);
        const float lookYaw=yaw+(std::getenv("GT2_COURSE_YAW_OFFSET")?std::stof(std::getenv("GT2_COURSE_YAW_OFFSET")):0.f);
        const float pitch=std::getenv("GT2_COURSE_PITCH")?std::stof(std::getenv("GT2_COURSE_PITCH")):0.f;
        const float rad=yaw*3.14159265f/180;view.right={std::cos(rad),0,std::sin(rad)};view.forward={std::sin(rad),0,-std::cos(rad)};
        for(size_t k=0;k<3;++k)view.eye[k]+=view.right[k]*lateral;
        if(std::getenv("GT2_COURSE_AUDIT_CHASE")){view.eye[1]+=2;for(size_t k=0;k<3;++k)view.eye[k]-=view.forward[k]*6;}
        gt2view::StereoViews views;for(int e=0;e<2;++e){auto eye=view.eye;for(size_t k=0;k<3;++k)eye[k]+=view.right[k]*(e?.032f:-.032f);const auto vp=View(eye,lookYaw,pitch);std::copy(vp.begin(),vp.end(),views.worldVP[e]);std::copy(vp.begin(),vp.end(),views.skyVP[e]);}
        renderer.SetStereoViews(views);const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
        std::vector<gt2view::DrawItem> items,blend;const auto start=std::chrono::steady_clock::now();assets.AppendTrackItems(items,blend,identity,view);items.insert(items.end(),blend.begin(),blend.end());
        const auto built=std::chrono::steady_clock::now();size_t vertices=0;for(auto& item:items){vertices+=item.vertexCount;item.space=gt2view::kSpaceWorld;}
        size_t changedPixels=0,lateBefore=0,lateAfter=0;int maxDelta=0;std::vector<gt2view::DrawItem> baselineItems;
        if(std::getenv("GT2_COURSE_ORDER_CHECK")){
            float model[16];gt2view::SceneAssets::BackdropModel(view.eye.data(),model);
            std::vector<gt2view::DrawItem> sky,skyBlend;assets.AppendBackdropItems(sky,skyBlend,model);
            for(auto& item:sky)item.space=gt2view::kSpaceSky;
            for(auto& item:skyBlend)item.space=gt2view::kSpaceSky;
            sky.insert(sky.end(),items.begin(),items.end()-ptrdiff_t(blend.size()));
            sky.insert(sky.end(),skyBlend.begin(),skyBlend.end());
            sky.insert(sky.end(),items.end()-ptrdiff_t(blend.size()),items.end());items=std::move(sky);
            const auto before=renderer.CaptureStereoForAudit(items,items.size());
            baselineItems=items;lateBefore=gt2view::OpaqueDrawsAfterBackdrop(items);
            if(!std::getenv("GT2_COURSE_INJECT_SKY_FIRST"))gt2view::DeferOpaqueBackdrop(items);
            lateAfter=gt2view::OpaqueDrawsAfterBackdrop(items);
            const auto after=renderer.CaptureStereoForAudit(items,items.size());
            for(size_t p=0;p<before.size();p+=4){int delta=0;for(size_t c=0;c<3;++c)delta=std::max(delta,std::abs(int(before[p+c])-int(after[p+c])));changedPixels+=delta>1;maxDelta=std::max(maxDelta,delta);}
            // One UNORM unit covers rounding; geometry/coverage must not move.
            if(changedPixels || lateAfter){++failures;
                gt2::WritePngRgba((out/(std::to_string(i)+"-before.png")).string(),512,768,before);
                gt2::WritePngRgba((out/(std::to_string(i)+"-after.png")).string(),512,768,after);
            }
        }
        const auto frame=renderer.CaptureStereoForAudit(items,items.size());const auto end=std::chrono::steady_clock::now();
        size_t roadWitness=0,roadOccluded=0;
        if(std::getenv("GT2_COURSE_WALL_CHECK")) {
            auto authored=view;authored.extendedDistance=0;
            std::vector<gt2view::DrawItem> reference,transparent;
            assets.AppendTrackItems(reference,transparent,identity,authored);
            reference.insert(reference.end(),transparent.begin(),transparent.end());
            for(auto& d:reference)d.space=gt2view::kSpaceWorld;
            const auto expected=renderer.CaptureStereoForAudit(reference,reference.size());
            auto road=[](const auto& image,size_t p){return image[p]>250 && image[p+1]<4 && image[p+2]>250;};
            for(size_t eye=0;eye<2;++eye)for(size_t y=192;y<380;++y)for(size_t x=3;x<509;++x) {
                const size_t p=((eye*384+y)*512+x)*4;
                if(!road(expected,p) || !road(expected,p-4) || !road(expected,p+4) || !road(expected,p-512*4) || !road(expected,p+512*4))continue;
                ++roadWitness;roadOccluded+=!road(frame,p);
            }
            if(roadOccluded>8){++failures;
                const auto stem=std::to_string(i)+"-"+std::to_string(mode);
                gt2::WritePngRgba((out/(stem+"-road-reference.png")).string(),512,768,expected);
                gt2::WritePngRgba((out/(stem+"-road-extended.png")).string(),512,768,frame);
            }
        }
        if(std::getenv("GT2_COURSE_AUDIT_CAPTURE") && mode==0 && (std::getenv("GT2_COURSE_CAPTURE_EVERY") || i==0 || i==track.chunks.size()/3 || i==track.chunks.size()*2/3)){
            const auto vp=View(view.eye,lookYaw,pitch);auto captured=baselineItems.empty()?items:baselineItems;
            for(auto& item:captured){float m[16]{};for(int col=0;col<4;++col)for(int row=0;row<4;++row)for(int k=0;k<4;++k)m[col*4+row]+=vp[k*4+row]*item.mvp[col*4+k];std::copy_n(m,16,item.mvp);}
            renderer.SaveCapture((out/(std::to_string(i)+".gt2cap")).string(),captured,captured.size());
        }
        if(!std::getenv("GT2_COURSE_FAILURES_ONLY"))gt2::WritePngRgba((out/(std::to_string(i)+"-"+std::to_string(mode)+".png")).string(),512,768,frame);
        csv<<i<<','<<mode<<','<<items.size()<<','<<vertices<<','<<assets.LastTrackStats().billboards<<','<<std::chrono::duration<double,std::milli>(built-start).count()<<','<<std::chrono::duration<double,std::milli>(end-built).count()<<','<<changedPixels<<','<<maxDelta<<','<<lateBefore<<','<<lateAfter<<','<<roadWitness<<','<<roadOccluded<<'\n';csv.flush();
        ++poses;std::cout<<i<<'/'<<track.chunks.size()<<'\n'<<std::flush;
    }
    std::ofstream(out/"gate.json")<<"{\"poses\":"<<poses<<",\"failures\":"<<failures<<"}\n";return failures?1:0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
