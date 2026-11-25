#pragma once
#include "gt2view/scene_assets.h"
#include "gt2export/png_writer.h"
#include <fstream>

// Exercise the real loaded glass, depth, UV crops and stereo render path.
// The same body with mirrors disabled is the negative control: changing the
// rear target must not recolour the dashboard, housing or other cars.
inline void CheckAuthoredMirrors(gt2view::VkSceneRenderer& renderer,gt2view::SceneAssets& assets,
    int slot,const gt2view::AuthoredCar& car,uint32_t width,uint32_t height,const std::filesystem::path& out) {
    using namespace gt2view;
    auto require=[](bool ok,const char* message){if(!ok)throw std::runtime_error(message);};
    require(car.mirrors.size()==3,"three authored live mirror surfaces required");
    const float identity[]{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    std::vector<DrawItem> live,disabled,center,sides,body,exterior;
    assets.AppendAuthoredMirrors(live,slot,identity);
    assets.AppendAuthoredMirrors(disabled,slot,identity,false,false);
    assets.AppendAuthoredMirrors(center,slot,identity,true,false);
    assets.AppendAuthoredMirrors(sides,slot,identity,false,true);
    require(live.size()==3 && disabled.empty() && center.size()==1 && sides.size()==2,"mirror settings/range selection failed");
    assets.AppendCockpitBodyItems(body,slot,identity,0);assets.AppendAuthoredControls(body,slot,identity,0,0,0);
    assets.AppendCarItems(exterior,slot,identity,0,0,true);
    for(const auto& item:exterior)for(const auto& v:renderer.VerticesForAudit(item))
        require(!(v.flags&kMirrorTexture),"exterior mirror samples its own render attachment");
    std::ofstream report(out/"mirror-checks.json");report<<"{\"surfaces\":[";
    bool first=true;
    for(const auto& glass:live){
        std::array<float,3> lo{100,100,100},hi{-100,-100,-100};
        for(const auto& v:renderer.VerticesForAudit(glass)){
            require(v.flags==kMirrorTexture,"authored glass is not a live mirror");
            for(size_t k=0;k<3;++k){lo[k]=std::min(lo[k],v.pos[k]);hi[k]=std::max(hi[k],v.pos[k]);}
        }
        std::array<float,3> target{};for(size_t k=0;k<3;++k)target[k]=(lo[k]+hi[k])*.5f;
        const std::string name=target[0]<-.5f?"left":target[0]>.5f?"right":"center";
        const float dx=target[0]-car.fit.eye[0],dy=target[1]-car.fit.eye[1],dz=target[2]-car.fit.eye[2];
        const float yaw=std::atan2(dx,-dz)*180/3.14159265f,pitch=std::atan2(dy,std::hypot(dx,dz))*180/3.14159265f;
        StereoViews views;
        for(int eye=0;eye<2;++eye){auto at=car.fit.eye;at[0]+=eye?.032f:-.032f;auto vp=View(at,yaw,pitch);
            const float focal=std::tan(47.5f*3.14159265f/180)/std::tan(27.5f*3.14159265f/180);
            for(size_t c=0;c<4;++c){vp[c*4]*=focal*float(height)/float(width);vp[c*4+1]*=focal;}
            std::copy(vp.begin(),vp.end(),views.worldVP[eye]);}renderer.SetStereoViews(views);
        auto capture=[&](int phase,bool enabled){
            constexpr std::array<std::array<float,2>,6> corners{{{0,0},{1,0},{1,1},{0,0},{1,1},{0,1}}};
            std::vector<SceneVertex> pattern;
            for(int band=0;band<3;++band)for(const auto& uv:corners){SceneVertex v{};
                v.pos[0]=-1+(float(band)+uv[0])*2/3;v.pos[1]=-1+uv[1]*2;v.pos[2]=.5f;
                v.color[(band+phase)%3]=1;pattern.push_back(v);}
            renderer.SetVertices(0,pattern);DrawItem rear;rear.firstVertex=0;rear.vertexCount=uint32_t(pattern.size());
            rear.space=kSpaceMirrorSource;std::copy_n(identity,16,rear.mvp);
            auto draws=body;if(enabled)draws.push_back(glass);
            for(auto& d:draws)d.space=kSpaceWorld;SortModelTransparency(draws);draws.push_back(rear);
            return renderer.CaptureStereoForAudit(draws,draws.size());
        };
        const auto a=capture(0,true),b=capture(1,true),offA=capture(0,false),offB=capture(1,false);
        require(offA==offB,"rear image leaks onto cabin with mirrors disabled");
        size_t changed[2]{},colors[2][3]{};
        for(size_t e=0;e<2;++e)for(size_t i=e*width*height*4;i<(e+1)*width*height*4;i+=4){
            if(std::abs(int(a[i])-int(b[i]))+std::abs(int(a[i+1])-int(b[i+1]))+std::abs(int(a[i+2])-int(b[i+2]))<100)continue;
            ++changed[e];for(size_t c=0;c<3;++c)if(a[i+c]>120 && a[i+c]>a[i+(c+1)%3]*2 && a[i+c]>a[i+(c+2)%3]*2)++colors[e][c];
        }
        gt2::WritePngRgba((out/("mirror-"+name+"-a.png")).string(),int(width),int(height*2),a);
        gt2::WritePngRgba((out/("mirror-"+name+"-b.png")).string(),int(width),int(height*2),b);
        std::cout<<"Mirror "<<name<<": changed "<<changed[0]<<','<<changed[1]<<"; RGB "<<colors[0][0]<<','<<colors[0][1]<<','<<colors[0][2]<<'\n';
        for(size_t e=0;e<2;++e){require(changed[e]>30,"live mirror does not update in both eyes");
            if(name=="left")require(colors[e][0]>30 && colors[e][2]==0,"left mirror has wrong rear-view crop");
            if(name=="right")require(colors[e][2]>30 && colors[e][0]==0,"right mirror has wrong rear-view crop");
            if(name=="center")require(colors[e][0]>10 && colors[e][1]>10 && colors[e][2]>10,"center mirror loses rear-view coverage");}
        if(!first)report<<',';first=false;
        report<<"{\"name\":\""<<name<<"\",\"changedPixels\":["<<changed[0]<<','<<changed[1]<<"],\"disabledUnchanged\":true}";
    }
    report<<"],\"exteriorNoFeedback\":true,\"pass\":true}\n";
}
