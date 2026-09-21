#include "gt2formats/hd_media.h"
#include "gt2formats/hd_video_filter.h"
#include "gt2formats/boot_images.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/title_assets.h"
#include "gt2formats/png_reader.h"
#include "gt2formats/str_video.h"
#include "gt2export/png_deflate.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>

#include "ui_export.h"
#include "font_export.h"

namespace fs=std::filesystem;
void Png(const fs::path& path,int w,int h,const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> rgba(size_t(w)*h*4,255);
    for(size_t i=0;i<rgb.size()/3;++i) std::memcpy(rgba.data()+i*4,rgb.data()+i*3,3);
    gt2::WritePngRgbaCompressed(path.string(),w,h,rgba);
}
int main(int argc,char** argv) {
    try {
        if(argc<3) throw std::runtime_error("gt2media images <disc> <out> | movie <disc> <id> <out> | pack <frames> <out.gtm> [original-frames] | unpack <movie.gtm> <frames> | inspect <movie.gtm> | frame <movie.gtm> <index> <out.png>");
        const std::string command=argv[1];
        if(command=="fit-images") {
            for(const auto& file:fs::directory_iterator(argv[2])) {
                if(file.path().extension()!=".png") continue;
                auto p=gt2::ReadPngFile(file.path().string());
                if(p.width<=2048 && p.height<=2048) continue;
                if(p.width>4096 || p.height>4096 || p.width%2 || p.height%2) throw std::runtime_error("HD image too large");
                std::vector<uint8_t> reduced(size_t(p.width/2)*(p.height/2)*4);
                for(int y=0;y<p.height/2;++y) for(int x=0;x<p.width/2;++x) for(int c=0;c<4;++c) {
                    unsigned sum=0;
                    for(int dy=0;dy<2;++dy) for(int dx=0;dx<2;++dx) sum+=p.rgba[size_t((y*2+dy)*p.width+x*2+dx)*4+size_t(c)];
                    reduced[size_t(y*(p.width/2)+x)*4+size_t(c)]=uint8_t((sum+2)/4);
                }
                gt2::WritePngRgbaCompressed(file.path().string(),p.width/2,p.height/2,reduced);
            }
            return 0;
        }
        if(command=="unpack") {
            if(argc!=4) throw std::runtime_error("unpack arguments");
            gt2::hd::Movie movie(argv[2]); const fs::path out=argv[3]; fs::create_directories(out);
            for(uint32_t i=0;i<movie.frames;++i) {
                char name[40]; std::snprintf(name,sizeof(name),"frame_%06u.png",i);
                const auto frame=movie.Frame(i); Png(out/name,int(frame.width),int(frame.height),frame.rgb);
            }
            const auto audio=movie.Audio(); std::ofstream pcm(out/"audio.pcm",std::ios::binary);
            pcm.write(reinterpret_cast<const char*>(audio.data()),std::streamsize(audio.size()*2));
            if(!pcm) throw std::runtime_error("cannot export movie audio");
            std::ofstream meta(out/"movie.txt");
            meta<<movie.sourceWidth<<' '<<movie.sourceHeight<<' '<<movie.fpsNum<<' '<<movie.fpsDen<<' '<<movie.frames<<'\n';
            if(!meta) throw std::runtime_error("cannot export movie metadata");
            return 0;
        }
        if(command=="inspect"||command=="frame") {
            gt2::hd::Movie movie(argv[2]);
            if(command=="frame") { if(argc!=5) throw std::runtime_error("frame arguments"); auto p=movie.Frame(uint32_t(std::stoul(argv[3])));Png(argv[4],int(p.width),int(p.height),p.rgb); }
            else { movie.Frame(0);movie.Frame(movie.frames-1);std::cout<<movie.width<<'x'<<movie.height<<" frames="<<movie.frames<<" fps="<<movie.fpsNum<<'/'<<movie.fpsDen<<" audio_samples="<<movie.Audio().size()<<'\n'; }
            return 0;
        }
        if(command=="pack") {
            if(argc!=4 && argc!=5) throw std::runtime_error("pack arguments");
            const fs::path folder=argv[2];std::ifstream meta(folder/"movie.txt");uint32_t sw=0,sh=0,num=0,den=0,count=0;
            if(!(meta>>sw>>sh>>num>>den>>count)||!count) throw std::runtime_error("missing movie.txt");
            std::unique_ptr<gt2::hd::MovieWriter> writer; int expectedW=0,expectedH=0;
            for(uint32_t i=0;i<count;++i) {
                char name[40];std::snprintf(name,sizeof(name),"frame_%06u.png",i);
                auto p=gt2::ReadPngFile((folder/name).string());
                if (p.width > 2048 || p.height > 2048) {
                    if (p.width > 4096 || p.height > 4096 || p.width%2 || p.height%2) throw std::runtime_error("upscaled frame too large");
                    std::vector<uint8_t> reduced(size_t(p.width/2)*(p.height/2)*4);
                    for (int y=0;y<p.height/2;++y) for (int x=0;x<p.width/2;++x) for (int c=0;c<4;++c) {
                        unsigned sum=0;
                        for (int dy=0;dy<2;++dy) for (int dx=0;dx<2;++dx) sum+=p.rgba[size_t((y*2+dy)*p.width+x*2+dx)*4+size_t(c)];
                        reduced[size_t(y*(p.width/2)+x)*4+size_t(c)]=uint8_t((sum+2)/4);
                    }
                    p.width/=2; p.height/=2; p.rgba=std::move(reduced);
                }
                if(argc==5) {
                    const auto original=gt2::ReadPngFile((fs::path(argv[4])/name).string());
                    if(original.width!=int(sw) || original.height!=int(sh)) throw std::runtime_error("source movie dimensions changed");
                    gt2::hd::ConstrainMovieFrame(p.rgba,p.width,p.height,original.rgba,original.width,original.height);
                }
                std::vector<uint8_t> rgb(size_t(p.width)*p.height*3);
                for(size_t j=0;j<rgb.size()/3;++j) std::memcpy(rgb.data()+j*3,p.rgba.data()+j*4,3);
                if(writer && (p.width!=expectedW || p.height!=expectedH)) throw std::runtime_error("frame dimensions changed");
                expectedW=p.width; expectedH=p.height;
                if(!writer) writer=std::make_unique<gt2::hd::MovieWriter>(argv[3],uint32_t(p.width),uint32_t(p.height),sw,sh,num,den);
                writer->Add(rgb.data());
            }
            std::ifstream pcm(folder/"audio.pcm",std::ios::binary|std::ios::ate);std::vector<int16_t> audio;
            if(pcm) { const auto bytes=pcm.tellg();if(bytes<0||bytes%4||bytes>44100ll*4*1800) throw std::runtime_error("invalid PCM size");audio.resize(size_t(bytes)/2);pcm.seekg(0);pcm.read(reinterpret_cast<char*>(audio.data()),bytes); }
            writer->Finish(audio);gt2::hd::Movie check(argv[3]);check.Frame(check.frames-1);std::cout<<"Packed "<<check.frames<<" frames\n";return 0;
        }
        gt2::DiscImage disc(argv[2]);const auto& profile=gt2::ProfileOf(disc);gt2::GtfsVolume vol(disc);
        if(command=="fonts") { if(argc!=4) throw std::runtime_error("fonts arguments"); ExportFonts(disc,vol,argv[3]);return 0; }
        if(command=="ui-maps") {
            if(argc!=4) throw std::runtime_error("ui-maps arguments");
            const fs::path out=argv[3]; std::set<std::string> keys; std::string key;
            std::ifstream previous(out/"index.txt"); std::getline(previous,key);
            while(previous>>key) if(key.size()==16) keys.insert(key);
            previous.close(); ExportUiMaps(vol,out,keys);
            std::ofstream index(out/"index.txt"); index<<"indexed-scale4x-v2\n";
            for(const auto& entry:keys) index<<entry<<'\n';
            return 0;
        }
        if(command=="ui") { if(argc!=4) throw std::runtime_error("ui arguments"); ExportUi(disc,vol,argv[3]); return 0; }
        if(command=="images") {
            if(argc!=4) throw std::runtime_error("images arguments");const fs::path out=argv[3];fs::create_directories(out);
            const auto exe=gt2::LoadExeImage(disc);
            for(const auto* name:{"logo-scea.tim","notice.tim"}) {const auto p=gt2::LoadBootImage(exe,name);Png(out/(std::string(name)+".png"),p.width,p.height,p.rgb);}
            auto title=profile.arcade?gt2::TitleAssets::LoadArcade(disc,vol):gt2::TitleAssets::Load(disc,vol);
            std::vector<uint8_t> rgb(352*480*3);
            for(int y=0;y<480;++y) for(int x=0;x<352;++x) {
                const uint16_t page=uint16_t((x<256?0x86:0x88)+(y>=256?0x10:0));
                const auto color=title.vram.Sample(page,0x7fd8,uint8_t(x%256),uint8_t(y%256));
                for(int c=0;c<3;++c) { const auto v=(color>>(c*5))&31;rgb[size_t(y*352+x)*3+size_t(c)]=uint8_t((v<<3)|(v>>2)); }
            }
            Png(out/"title.png",352,480,rgb);
            if(!profile.arcade) {
                auto assets=gt2::MenuAssets::Load(disc,vol);
                for(uint32_t id=0;id<assets.commonIndex.Count();++id) {
                    const auto bg=assets.Background(id);gt2::MenuVram vram;std::vector<uint8_t> clut(bg.clut.size()*2);std::memcpy(clut.data(),bg.clut.data(),clut.size());
                    vram.Upload(0,504,512,8,clut);vram.Upload(768,0,256,bg.Rows(),bg.pixels);gt2::MenuCanvas canvas;
                    for(auto w:bg.tiles) { const auto t=gt2::DecodeBackgroundTile(w); if(t.flat) canvas.Fill(t.x,t.y,16,8,t.r,t.g,t.b);else canvas.Sprite(vram,t.x,t.y,16,8,t.u,t.v,t.tpage,t.clut); }
                    gt2::WritePngRgbaCompressed((out/("gt-background-"+std::to_string(id)+".png")).string(),512,480,canvas.Rgba());
                }
            }
            std::ofstream(out/"profile.txt")<<profile.exeSha1<<'\n';return 0;
        }
        if(command=="movie") {
            if(argc!=5) throw std::runtime_error("movie arguments");const int id=std::stoi(argv[3]);const fs::path out=argv[4];fs::create_directories(out);
            const auto exe=gt2::LoadExeImage(disc);const auto movies=gt2::ReadStreamMovieTable(exe);if(id<0||size_t(id)>=movies.size()) throw std::runtime_error("movie id out of range");
            const auto vlc=gt2::ReadGtVlcTable(gt2::LoadOverlayImage(disc,5),gt2::kMovieVlcTableAddress);const auto mdec=gt2::MdecCoreFromExe(exe);
            gt2::GtMovieReader reader(disc,gt2::StreamFileLba(disc),movies[size_t(id)]);gt2::GtMovieReader::Frame frame;uint32_t count=0,firstSector=0,lastSector=0;int width=0,height=0;
            while(reader.NextFrame(frame)) {if(!count) firstSector=frame.lastSector;lastSector=frame.lastSector;const auto p=gt2::DecodeMovieFrame(mdec,gt2::GtFrameToMdecCodes(frame.data.data(),frame.data.size(),vlc));char name[40];std::snprintf(name,sizeof(name),"frame_%06u.png",count++);Png(out/name,p.width,p.height,p.rgb);width=p.width;height=p.height;}
            if(count<2) throw std::runtime_error("movie has fewer than two frames"); const uint32_t sectorsPerFrame=(lastSector-firstSector+(count-1)/2)/(count-1); if(!sectorsPerFrame) throw std::runtime_error("invalid movie cadence"); const uint32_t fpsNum=150/std::gcd(150u,sectorsPerFrame),fpsDen=sectorsPerFrame/std::gcd(150u,sectorsPerFrame);
            std::ofstream(out/"movie.txt")<<width<<' '<<height<<' '<<fpsNum<<' '<<fpsDen<<' '<<count<<'\n';
            const auto& xa=reader.Audio();const size_t channels=reader.AudioStereo()?2:1;std::vector<int16_t> pcm;
            if(!xa.empty()) {
                const size_t inputFrames=xa.size()/channels;const size_t outputFrames=size_t(uint64_t(inputFrames)*44100/reader.AudioRate());pcm.resize(outputFrames*2);
                for(size_t i=0;i<outputFrames;++i) {const double at=double(i)*reader.AudioRate()/44100;const size_t a=std::min(size_t(at),inputFrames-1),b=std::min(a+1,inputFrames-1);const double f=at-double(a);for(size_t c=0;c<2;++c) pcm[i*2+c]=int16_t(double(xa[a*channels+(c%channels)])*(1-f)+double(xa[b*channels+(c%channels)])*f);}
            }
            std::ofstream audio(out/"audio.pcm",std::ios::binary);audio.write(reinterpret_cast<const char*>(pcm.data()),std::streamsize(pcm.size()*2));
            std::cout<<"Exported "<<count<<" frames "<<width<<'x'<<height<<'\n';return 0;
        }
        throw std::runtime_error("unknown command");
    } catch(const std::exception& e) {std::cerr<<"gt2media: "<<e.what()<<'\n';return 1;}
}
