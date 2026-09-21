#include "gt2formats/hd_media.h"
#include "game/shell/shared_vr_settings.h"
#include "game/shell/title_options.h"
#include "gt2formats/hd_ui.h"
#include "gt2formats/ui_contours.h"
#include "gt2formats/hd_video_filter.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <chrono>

namespace fs = std::filesystem;
void Require(bool ok) { if (!ok) throw std::runtime_error("HD media check failed"); }
template<class F> void Reject(F f) { bool rejected=false; try { f(); } catch(const std::exception&) { rejected=true; } Require(rejected); }
int main() {
    const auto dir=fs::temp_directory_path()/ ("gt2-hd-check-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    try {
        for(const auto* newline:{"\n","\r\n"}) {
            std::istringstream index(std::string("palette-contours4x-v1")+newline+"9a61b7e14cf3319e-3e70"+newline+"9a61b7e14cf3319e-3e37"+newline);
            const auto entries=gt2::hd::ReadFontIndex(index);
            Require(entries.size()==2 && entries[0]=="9a61b7e14cf3319e-3e70" && entries[1]=="9a61b7e14cf3319e-3e37");
        }
        std::istringstream wrong("unsupported\r\n9a61b7e14cf3319e-3e70\r\n");
        Require(gt2::hd::ReadFontIndex(wrong).empty());
        std::istringstream bad("palette-contours4x-v1\n../../../../evil-xxxx\n9a61b7e14cf3319e-3e70\n");
        Require(gt2::hd::ReadFontIndex(bad).size()==1);
        {
            std::array<uint16_t,16> palette{};palette[1]=0x8000;palette[2]=0xffff;
            const auto coverage=[&](const std::vector<uint8_t>& image,int at) {
                const float w=image[size_t(at)*4+2]/255.f;
                return (image[size_t(at)*4]==2?1.f:0.f)*(1-w)+(image[size_t(at)*4+1]==2?1.f:0.f)*w;
            };
            std::vector<uint8_t> pattern(64,1);
            auto smooth=gt2::hd::SmoothUiContours4x(pattern,palette,8);
            for(int i=0;i<1024;++i) Require(coverage(smooth,i)==0);
            for(int y=0;y<8;++y) for(int x=4;x<8;++x) pattern[size_t(y*8+x)]=2;
            smooth=gt2::hd::SmoothUiContours4x(pattern,palette,8);
            for(int y=0;y<32;++y) for(int x=0;x<32;++x) Require(coverage(smooth,y*32+x)==(x>=16?1.f:0.f));
            for(int y=0;y<8;++y) for(int x=0;x<8;++x) pattern[size_t(y*8+x)]=x>=y?2:1;
            smooth=gt2::hd::SmoothUiContours4x(pattern,palette,8);
            int antialiased=0;for(int i=0;i<1024;++i) {float c=coverage(smooth,i);if(c>0 && c<1) ++antialiased;}
            Require(antialiased>0);
            Reject([&]{gt2::hd::SmoothUiContours4x({16},palette,1);});
        }
        std::vector<uint8_t> source(4*4*4,100), neural(16*16*4,255);
        gt2::hd::ConstrainMovieFrame(neural,16,16,source,4,4);
        for(size_t i=0;i<neural.size();i+=4) Require(neural[i]==110 && neural[i+3]==255);
        std::fill(neural.begin(),neural.end(),uint8_t(0));
        gt2::hd::ConstrainMovieFrame(neural,16,16,source,4,4);
        for(size_t i=0;i<neural.size();i+=4) Require(neural[i]==90);
        Reject([&]{gt2::hd::ConstrainMovieFrame(neural,2049,16,source,4,4);});
        std::vector<uint16_t> atlas(1024*256,0x1111);
        const auto key=gt2::hd::UiPageKey(atlas.data());
        const auto enlarged=gt2::hd::EnlargeUiPage(atlas.data());
        Require(enlarged.size()==1024*1024 && std::all_of(enlarged.begin(),enlarged.end(),[](uint8_t c){return c==1;}));
        atlas[63]=0x2222; Require(key!=gt2::hd::UiPageKey(atlas.data()));
        Require(gt2::hd::UiPageKey(atlas.data())!=gt2::hd::UiPageKey(atlas.data(),1024,1));
        const auto eight=gt2::hd::EnlargeUiPage(atlas.data(),1);Require(eight[0]==17);
        const auto edge=gt2::hd::ScaleUi2x({0,0,0,0,1,1,0,1,1},3);
        Require(edge[2*6+2]==0 && edge[3*6+3]==1); // diagonal rounds without inventing indices
        std::vector<uint16_t> mapPage(1024*256);
        for(int y=144;y<240;++y) std::fill_n(mapPage.data()+y*1024,24,uint16_t(0x1111));
        const auto isolated=gt2::hd::EnlargeUiRegion(mapPage.data(),0,144,96);
        Require(isolated.size()==384*384 && std::all_of(isolated.begin(),isolated.end(),[](uint8_t v){return v==1;}));
        const auto whole=gt2::hd::EnlargeUiPage(mapPage.data());
        Require(whole[576*1024+383]!=isolated[383]); // neighbouring index zero used to leak into the corner
        Reject([&]{gt2::hd::EnlargeUiRegion(mapPage.data(),0,240,96);});
        const auto path=(dir/"test.gtm").string();
        const std::vector<int16_t> pcm={-32768,32767,-128,128,0,0};
        {
            gt2::hd::MovieWriter out(path,16,16,8,8,30,1);
            std::vector<uint8_t> rgb(16*16*3,64); out.Add(rgb.data());
            std::fill(rgb.begin(),rgb.end(),uint8_t(192)); out.Add(rgb.data()); out.Finish(pcm);
        }
        {
            gt2::hd::Movie movie(path); Require(movie.frames==2 && movie.sourceWidth==8 && movie.Audio()==pcm);
            Require(movie.Frame(0).rgb[0]>=62 && movie.Frame(1).rgb[0]>=190);
            Reject([&]{movie.Frame(2);});
        }
        fs::copy_file(path,dir/"truncated.gtm"); fs::resize_file(dir/"truncated.gtm",65);
        Reject([&]{gt2::hd::Movie bad((dir/"truncated.gtm").string());});
        {
            std::fstream corrupt(path,std::ios::in|std::ios::out|std::ios::binary);
            corrupt.seekp(8); const char huge[4]={-1,-1,-1,127};corrupt.write(huge,4);
        }
        Reject([&]{gt2::hd::Movie bad(path);});
        fs::create_directory(dir/"hd"); std::ofstream(dir/"hd/profile.txt")<<"profile-a";
        std::ofstream(dir/"hd/title.png")<<"present";
        gt2::hd::SetRoot(dir.string(),"profile-b"); Require(gt2::hd::Asset("title.png").empty());
        gt2::hd::SetRoot(dir.string(),"profile-a"); Require(!gt2::hd::Asset("title.png").empty());
        const auto generation=gt2::hd::Generation();
        std::ofstream(dir/"hd/startup.gtm")<<"original";
        std::ofstream(dir/"hd/startup-hd.gtm")<<"enhanced";
        Require(fs::path(gt2::hd::StartupMovie()).filename()=="startup-hd.gtm");
        gt2::hd::SetEnabled(false);
        Require(gt2::hd::Generation()!=generation && gt2::hd::Asset("title.png").empty());
        Require(fs::path(gt2::hd::StartupMovie()).filename()=="startup.gtm");
        const auto disabled=gt2::hd::Generation(); gt2::hd::SetEnabled(false); Require(disabled==gt2::hd::Generation());
        gt2::hd::SetEnabled(true); Require(!gt2::hd::Asset("title.png").empty());
        fs::remove(dir/"hd/startup-hd.gtm"); Require(fs::path(gt2::hd::StartupMovie()).filename()=="startup.gtm");
        const auto saves=dir/"saves";
        gt2::shell::WritePreferences(saves/"arcade/settings.txt","vr_world_scale=110\nunits=kmh\nmusic_volume=10\n");
        gt2::shell::WritePreferences(saves/"arcade/settings.txt.overlay","vr_render_scale=120\nhd_assets=0\nunlock_cars=1\n");
        gt2::shell::WritePreferences(saves/"simulation/settings.txt.overlay","vr_render_scale=175\nunlock_courses=1\n");
        fs::last_write_time(saves/"simulation/settings.txt.overlay",fs::last_write_time(saves/"arcade/settings.txt.overlay")-std::chrono::hours(1));
        gt2::shell::InitializeSharedVrSettings(saves,"vr_stereo=1\n", "vr_render_scale=150\n");
        const auto shared=gt2::shell::ReadPreferences(gt2::shell::sharedVrSettingsPath);
        Require(shared.find("vr_render_scale=120")!=std::string::npos && shared.find("hd_assets=0")!=std::string::npos);
        Require(shared.find("unlock")==std::string::npos && shared.find("music_volume")==std::string::npos);
        gt2::shell::WritePreferences(saves/"arcade/settings.txt.overlay","vr_render_scale=50\n");
        gt2::shell::InitializeSharedVrSettings(saves,"vr_stereo=0\n", "vr_render_scale=50\n");
        Require(gt2::shell::ReadPreferences(gt2::shell::sharedVrSettingsPath)==shared);
        Require(gt2::shell::ReadPreferences(saves/"simulation/settings.txt.overlay").find("unlock_courses=1")!=std::string::npos);
        const auto arcade=gt2::shell::PcSettings::Load((saves/"arcade/settings.txt").string());
        const auto simulation=gt2::shell::PcSettings::Load((saves/"simulation/settings.txt").string());
        Require(arcade.vr.renderScale==120 && simulation.vr.renderScale==120);
        Require(arcade.vr.worldScale==110 && simulation.vr.worldScale==110);
        Require(arcade.options.musicVolume==10 && simulation.options.musicVolume!=10);
        gt2::shell::sharedVrSettingsPath.clear();
        Require(gt2::hd::Asset("../test.gtm").empty()); Require(gt2::hd::Asset("C:/test").empty());
        fs::remove_all(dir); std::cout<<"HD media checks passed\n"; return 0;
    } catch(const std::exception& e) { fs::remove_all(dir); std::cerr<<e.what()<<'\n';return 1; }
}
