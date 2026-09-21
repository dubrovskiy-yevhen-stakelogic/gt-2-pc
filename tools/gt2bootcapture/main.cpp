// Offline libretro host. Firmware and the emulator core are supplied separately.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../third_party/libretro/libretro.h"
#include "gt2formats/hd_media.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <stdexcept>

namespace {
std::string systemDir,saveDir;
std::map<std::string,std::string> options;
retro_pixel_format format=RETRO_PIXEL_FORMAT_0RGB1555;
std::vector<int16_t> audio;
std::unique_ptr<gt2::hd::MovieWriter> writer;
std::vector<uint8_t> previous(640*480*3);
uint32_t frames=0,whiteAt=0,logoAt=0,finishedAt=0;
bool sawWhite=false,sawLogo=false;
void Log(enum retro_log_level,const char* message,...) {va_list args;va_start(args,message);std::vfprintf(stderr,message,args);va_end(args);}
bool Environment(unsigned command,void* data) {
    switch(command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:*static_cast<const char**>(data)=systemDir.c_str();return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:*static_cast<const char**>(data)=saveDir.c_str();return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:static_cast<retro_log_callback*>(data)->log=Log;return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:format=*static_cast<retro_pixel_format*>(data);return format!=RETRO_PIXEL_FORMAT_UNKNOWN;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:*static_cast<bool*>(data)=true;return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:*static_cast<unsigned*>(data)=0;return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:*static_cast<unsigned*>(data)=RETRO_LANGUAGE_ENGLISH;return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:*static_cast<bool*>(data)=false;return true;
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        auto& variable=*static_cast<retro_variable*>(data);auto it=options.find(variable.key);variable.value=it==options.end()?nullptr:it->second.c_str();return variable.value!=nullptr;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        for(auto* v=static_cast<retro_variable*>(data);v->key;++v) {
            std::string text=v->value?v->value:"";const auto start=text.find("; ");if(start==std::string::npos) continue;
            text=text.substr(start+2);text=text.substr(0,text.find('|'));options.try_emplace(v->key,text);
            if(std::string(v->key).find("skip_bios")!=std::string::npos) options[v->key]="disabled";
        }
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:return true;
    default:return false;
    }
}
void Video(const void* pixels,unsigned width,unsigned height,size_t pitch) {
    if(pixels==RETRO_HW_FRAME_BUFFER_VALID) throw std::runtime_error("capture needs a software-rendering core");
    if(pixels&&width&&height) {
        for(unsigned y=0;y<480;++y) for(unsigned x=0;x<640;++x) {
            const auto* row=static_cast<const uint8_t*>(pixels)+(size_t(y)*height/480)*pitch;unsigned r,g,b;
            if(format==RETRO_PIXEL_FORMAT_XRGB8888) {const uint32_t v=reinterpret_cast<const uint32_t*>(row)[size_t(x)*width/640];r=(v>>16)&255;g=(v>>8)&255;b=v&255;}
            else {const uint16_t v=reinterpret_cast<const uint16_t*>(row)[size_t(x)*width/640];const bool six=format==RETRO_PIXEL_FORMAT_RGB565;r=((v>>(six?11:10))&31)*255/31;g=((v>>5)&(six?63:31))*255/(six?63:31);b=(v&31)*255/31;}
            const size_t at=size_t(y*640+x)*3;previous[at]=uint8_t(r);previous[at+1]=uint8_t(g);previous[at+2]=uint8_t(b);
        }
    }
    writer->Add(previous.data(),95);++frames;
    unsigned white=0,red=0,green=0,blue=0;
    for(size_t i=0;i<previous.size();i+=12) {const auto r=previous[i],g=previous[i+1],b=previous[i+2];white+=r>140&&g>140&&b>140&&std::abs(int(r)-int(g))<8&&std::abs(int(g)-int(b))<8;red+=r>100&&r>g*2&&r>b*2;green+=g>65&&g>r*3/2&&g>b*7/10;blue+=b>65&&b>r*3/2&&b>g*4/5;}
    if(!sawWhite&&white>40000) {sawWhite=true;whiteAt=frames;std::printf("White Sony screen at frame %u\n",frames);}
    if(sawWhite&&!sawLogo&&white<18000&&red>80&&green>30&&blue>30) {sawLogo=true;logoAt=frames;std::printf("PlayStation logo at frame %u\n",frames);}
    if(sawLogo&&frames>logoAt+60&&red<8&&green<8&&blue<8&&!finishedAt) finishedAt=frames;
}
void AudioSample(int16_t l,int16_t r) {audio.push_back(l);audio.push_back(r);}
size_t AudioBatch(const int16_t* data,size_t count) {audio.insert(audio.end(),data,data+count*2);return count;}
void Poll() {}
int16_t Input(unsigned,unsigned,unsigned,unsigned) {return 0;}
template<class T> T Function(HMODULE dll,const char* name) {auto f=reinterpret_cast<T>(GetProcAddress(dll,name));if(!f) throw std::runtime_error(std::string("missing libretro function: ")+name);return f;}
}
int main(int argc,char** argv) {
    try {
        if(argc!=5) throw std::runtime_error("gt2bootcapture <core.dll> <prepared-system-dir> <disc.cue> <output.gtm>");
        systemDir=std::filesystem::absolute(argv[2]).string();saveDir=systemDir;
        const HMODULE dll=LoadLibraryW(std::filesystem::absolute(argv[1]).c_str());if(!dll) throw std::runtime_error("cannot load capture core");
#define API(name) Function<decltype(&retro_##name)>(dll,"retro_" #name)
        API(set_environment)(Environment);API(set_video_refresh)(Video);API(set_audio_sample)(AudioSample);API(set_audio_sample_batch)(AudioBatch);API(set_input_poll)(Poll);API(set_input_state)(Input);API(init)();
        const std::string disc=std::filesystem::absolute(argv[3]).string();retro_game_info game{disc.c_str(),nullptr,0,nullptr};
        if(!API(load_game)(&game)) throw std::runtime_error("core could not boot disc with supplied BIOS");
        retro_system_av_info av{};API(get_system_av_info)(&av);
        writer=std::make_unique<gt2::hd::MovieWriter>(argv[4],640,480,640,480,uint32_t(av.timing.fps*1000+.5),1000);
        const auto run=API(run);
        while(frames<1200&&!finishedAt) run();
        API(unload_game)();API(deinit)();FreeLibrary(dll);
        if(av.timing.sample_rate!=44100) throw std::runtime_error("unsupported BIOS audio rate");
        writer->Finish(audio);writer.reset();
        if(!sawWhite||!sawLogo||!finishedAt) throw std::runtime_error("could not identify both complete BIOS screens; capture rejected");
        gt2::hd::Movie verify(argv[4]);verify.Frame(whiteAt);verify.Frame(logoAt);
        std::printf("Captured original BIOS intro: %u frames, white=%u, PS=%u, end=%u\n",frames,whiteAt,logoAt,finishedAt);return 0;
    } catch(const std::exception& e) {std::fprintf(stderr,"gt2bootcapture: %s\n",e.what());return 1;}
}

