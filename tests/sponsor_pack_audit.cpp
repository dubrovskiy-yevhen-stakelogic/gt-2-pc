#include "offscreen_car_audit.h"
#include "gt2view/generated_world.h"
#include "gt2vfs/gtfs.h"
#include "gt2export/png_writer.h"
#include <fstream>
#include <set>
#include <sstream>
using namespace gt2view;
int main(int argc,char** argv) { try {
    const bool dynamicOnly=argc==4 && std::string(argv[3])=="--dynamic-only";
    if(argc!=3 && !dynamicOnly)throw std::runtime_error("usage: gt2sponsorpackaudit <disc> <isolated-marker-texture-root> [--dynamic-only]");
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::generated::root=argv[2];
    Device device;VkContext context(device.existing);
    VkSceneRenderer renderer(context,{64,64},VK_FORMAT_R8G8B8A8_UNORM);
    renderer.EnableDecodedTextures(true);
    StereoViews views{};
    for(int eye=0;eye<2;++eye)for(auto* m:{views.worldVP[eye],views.skyVP[eye],views.hudVP[eye]})m[0]=m[5]=m[10]=m[15]=1;
    renderer.SetStereoViews(views);
    renderer.CreateStereoTarget({64,64},VK_FORMAT_R8G8B8A8_UNORM,true);
    auto quad=[](uint32_t page,uint32_t clut,float u,float v) {
        const float xy[6][2]={{-.8f,-.8f},{.8f,-.8f},{.8f,.8f},{-.8f,-.8f},{.8f,.8f},{-.8f,.8f}};
        std::vector<SceneVertex> vertices(6);
        for(size_t i=0;i<6;++i){auto& a=vertices[i];a.pos[0]=xy[i][0];a.pos[1]=xy[i][1];a.pos[2]=.5f;
            a.color[0]=a.color[1]=a.color[2]=1;a.texel[0]=u;a.texel[1]=v;
            a.page=(page&15)*64|(((page>>4)&1)*256<<16);a.clut=(clut&63)*16|((clut>>6)<<16);a.flags=kTextured|(((page>>7)&3)<<8);}
        return vertices;
    };
    auto draw=[&](const std::vector<SceneVertex>& vertices) {
        renderer.SetVertices(0,vertices);DrawItem item;item.vertexCount=6;item.space=kSpaceWorld;
        item.mvp[0]=item.mvp[5]=item.mvp[10]=item.mvp[15]=1;
        const auto rgba=renderer.CaptureStereoForAudit({item},1);
        for(size_t eye=0;eye<2;++eye){const size_t at=(eye*64*64+32*64+32)*4;
            if(rgba[at]>20 || rgba[at+1]<230 || rgba[at+2]>20)throw std::runtime_error("sponsor marker not rendered in both eyes");}
    };
    size_t courses=0,bindings=0,captures=0;std::set<std::string> seen;
    if(!dynamicOnly)for(const auto& entry:vol.Files()) {
        const auto name=std::filesystem::path(entry.path).filename().string();
        const auto dot=name.find('.');if(dot==std::string::npos)continue;
        if(name.substr(dot)!=".trp" && name.substr(dot)!=".trp.gz" && name.substr(dot)!=".bsp" && name.substr(dot)!=".bsp.gz")continue;
        const auto course=(name.substr(dot).starts_with(".bsp") ? "sky_" : "")+name.substr(0,dot);const auto source=vol.Read(entry);
        const auto manifest=gt2::generated::BuiltinTextureManifest(course,gt2::Sha1Hex(source));if(manifest.empty())continue;
        uint32_t cursor=SceneRenderer::kGeneratedWorldTexelBase;GeneratedWorld world;world.Load(course,source,renderer,cursor);++courses;
        std::istringstream rows{std::string(manifest)};std::string line;std::getline(rows,line);
        while(std::getline(rows,line)) {
            uint32_t page,clut,x,y,w,h;std::string file,group;std::istringstream row(line);
            if(!(row>>page>>clut>>x>>y>>w>>h>>file>>group))continue;
            const auto path=gt2::generated::CourseImagePath(course,file);
            if(path.parent_path().filename()!="_sponsors")continue;
            auto vertices=quad(page,clut,float(x)+float(w)*.5f,float(y)+float(h)*.5f);
            if(world.Apply(vertices)!=2)throw std::runtime_error("unbound sponsor: "+course+"/"+file);
            ++bindings;
            // Render each distinct source variant; repeated course registrations still check binding.
            if(seen.insert(file).second){draw(vertices);++captures;}
        }
    }
    const auto table=gt2::ParseSponsorTable(vol.Read(".crstims.tsd"));
    size_t dynamic=0,absent=0;
    const auto fixture=gt2::generated::root/"audit-fixture";std::filesystem::create_directories(fixture);
    const std::array<uint8_t,4> source{1,2,3,4};const std::array<uint8_t,4> white{255,255,255,255};
    gt2::WritePngRgba((fixture/"blank.png").string(),1,1,white);
    std::vector<const gt2::SponsorLogoTim*> all;
    for(const auto& tim:table.templates)all.push_back(&tim);
    for(const auto& tim:table.logos)all.push_back(&tim);
    for(const auto* timPtr:all) {
        const auto& tim=*timPtr;
        std::vector<uint8_t> rgba;
        for(uint16_t word:tim.image)for(unsigned shift=0;shift<16;shift+=4){const auto c=tim.clut[(word>>shift)&15];
            for(unsigned channel=0;channel<3;++channel)rgba.push_back(uint8_t(((c>>(channel*5))&31)*255/31));
            rgba.push_back(c==0?0:(c&32768)?128:255);}
        const auto hash=gt2::Sha1Hex(rgba);
        if(!std::filesystem::is_regular_file(gt2::generated::root/"_sponsors"/(hash+".png"))){++absent;continue;}
        const uint32_t w=4u*tim.width,h=tim.height;
        {std::ofstream out(fixture/"pack.txt");out<<"GT2-GENERATED-WORLD-2 "<<gt2::Sha1Hex(source)<<"\n0 0 0 0 "<<w<<' '<<h<<" blank.png signs\n";}
        uint32_t cursor=SceneRenderer::kGeneratedWorldTexelBase;GeneratedWorld world;world.Load("audit-fixture",source,renderer,cursor);
        gt2::SponsorUpload upload;upload.tim=&tim;
        if(!world.BindSponsors({upload},renderer,cursor))throw std::runtime_error("dynamic sponsor rejected: "+hash);
        auto vertices=quad(0,0,float(w)*.5f,float(h)*.5f);
        if(world.Apply(vertices)!=2)throw std::runtime_error("dynamic sponsor unbound: "+hash);
        draw(vertices);
        // A fixed billboard and a race slot sharing a PNG must not spend the atlas budget twice.
        {std::ofstream out(fixture/"pack.txt");out<<"GT2-GENERATED-WORLD-2 "<<gt2::Sha1Hex(source)<<"\n0 0 0 0 "<<w<<' '<<h<<" 1x1-"<<hash<<"-none-signs.png signs\n";}
        cursor=SceneRenderer::kGeneratedWorldTexelBase;world.Load("audit-fixture",source,renderer,cursor);
        const auto before=cursor;
        if(!world.BindSponsors({upload},renderer,cursor) || cursor!=before)
            throw std::runtime_error("shared sponsor uploaded twice: "+hash);
        vertices=quad(0,0,float(w)*.5f,float(h)*.5f);world.Apply(vertices);draw(vertices);
        ++dynamic;std::printf("DYNAMIC %s PASS\n",hash.c_str());
    }
    {
        std::vector<gt2::SponsorUpload> uploads;
        {std::ofstream out(fixture/"pack.txt");out<<"GT2-GENERATED-WORLD-2 "<<gt2::Sha1Hex(source)<<"\n";
            for(size_t i=0;i<all.size();++i) {
                const auto& tim=*all[i];
                out<<"0 "<<i*64<<" 0 0 "<<4u*tim.width<<' '<<tim.height<<" blank.png signs\n";
                gt2::SponsorUpload u;u.tim=&tim;u.clutY=uint16_t(i);uploads.push_back(u);
            }
        }
        uint32_t cursor=SceneRenderer::kGeneratedWorldTexelBase;GeneratedWorld world;world.Load("audit-fixture",source,renderer,cursor);
        cursor=SceneRenderer::kGeneratedHudBase-12000;
        if(!world.BindSponsors(uploads,renderer,cursor) || cursor>SceneRenderer::kGeneratedHudBase)
            throw std::runtime_error("sponsor pressure budget overflow");
        for(size_t i=0;i<all.size();++i) {
            auto vertices=quad(0,uint32_t(i*64),float(4u*all[i]->width)*.5f,float(all[i]->height)*.5f);
            if(world.Apply(vertices)!=2)throw std::runtime_error("pressure sponsor not bound");
            draw(vertices);
        }
        std::printf("PASS simultaneous pressure slots=%zu\n",all.size());
    }
    std::printf("PASS courses=%zu static_bindings=%zu rendered_variants=%zu dynamic_logos=%zu missing_dynamic=%zu\n",courses,bindings,captures,dynamic,absent);
    return 0;
} catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
