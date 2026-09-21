#include "gt2formats/hd_media.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <span>
#include "gt2formats/sha1.h"
#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#define STBI_ONLY_JPEG
#define STB_IMAGE_IMPLEMENTATION
#define STBI_MAX_DIMENSIONS 2048
#include "../../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace gt2::hd {
namespace {
std::filesystem::path root;
uint64_t generation=0;
bool enabled=true;
uint64_t Read(std::istream& f,int n) { uint64_t v=0; for(int i=0;i<n;++i) { const int b=f.get(); if(b<0) throw std::runtime_error("truncated HD movie"); v|=uint64_t(b)<<(i*8); } return v; }
void Write(std::ostream& f,uint64_t v,int n) { for(int i=0;i<n;++i) f.put(char(v>>(i*8))); }
void Dimensions(uint32_t w,uint32_t h) { if(!w||!h||w>2048||h>2048) throw std::runtime_error("invalid HD dimensions"); }
}
Movie::Movie(const std::string& path):file_(path,std::ios::binary) {
    if(!file_) throw std::runtime_error("cannot open HD movie");
    file_.seekg(0,std::ios::end); length_=uint64_t(file_.tellg()); file_.seekg(0);
    char magic[8]; file_.read(magic,8);
    if(!file_||std::memcmp(magic,"G2MEDIA1",8)) throw std::runtime_error("invalid HD movie signature");
    width=uint32_t(Read(file_,4));height=uint32_t(Read(file_,4));frames=uint32_t(Read(file_,4));
    fpsNum=uint32_t(Read(file_,4));fpsDen=uint32_t(Read(file_,4));const auto rate=Read(file_,4);
    index_=Read(file_,8);audio_=Read(file_,8);samples_=Read(file_,8);
    sourceWidth=uint32_t(Read(file_,4));sourceHeight=uint32_t(Read(file_,4));
    Dimensions(width,height);Dimensions(sourceWidth,sourceHeight);
    if(!frames||frames>120000||!fpsNum||!fpsDen||fpsNum>120000||fpsDen>10000||rate!=44100||samples_%2||samples_>44100ull*2*1800)
        throw std::runtime_error("invalid HD movie timing");
    if(uint64_t(frames)*fpsDen>1800ull*fpsNum) throw std::runtime_error("HD movie exceeds 30 minutes");
    const auto range=[&](uint64_t at,uint64_t bytes) { return at>=64&&at<=length_&&bytes<=length_-at; };
    if(!range(index_,uint64_t(frames)*16)||!range(audio_,samples_*2)) throw std::runtime_error("invalid HD movie offsets");
    file_.seekg(std::streamoff(index_)); entries_.reserve(frames);
    uint64_t end=64;
    for(uint32_t i=0;i<frames;++i) {
        const uint64_t at=Read(file_,8);const auto bytes=uint32_t(Read(file_,4));Read(file_,4);
        if(!bytes||bytes>32*1024*1024||at<end||!range(at,bytes)||at+bytes>audio_) throw std::runtime_error("invalid HD frame index");
        entries_.emplace_back(at,bytes);end=at+bytes;
    }
    if(audio_+samples_*2>index_) throw std::runtime_error("overlapping HD audio/index");
}
Picture Movie::Frame(uint32_t index) {
    if(index>=frames) throw std::runtime_error("HD frame out of range");
    const auto [offset,bytes]=entries_[index];std::vector<uint8_t> jpeg(bytes);
    file_.clear();file_.seekg(std::streamoff(offset));file_.read(reinterpret_cast<char*>(jpeg.data()),bytes);
    if(!file_) throw std::runtime_error("truncated HD frame");
    int w=0,h=0,c=0;
    if(!stbi_info_from_memory(jpeg.data(),int(jpeg.size()),&w,&h,&c)||w!=int(width)||h!=int(height)) throw std::runtime_error("HD frame size mismatch");
    auto* data=stbi_load_from_memory(jpeg.data(),int(jpeg.size()),&w,&h,&c,3);
    if(!data) throw std::runtime_error("invalid HD JPEG");
    Picture picture{width,height,{}};picture.rgb.assign(data,data+size_t(width)*height*3);stbi_image_free(data);return picture;
}
std::vector<int16_t> Movie::Audio() {
    std::vector<int16_t> result(static_cast<size_t>(samples_));file_.clear();file_.seekg(std::streamoff(audio_));
    file_.read(reinterpret_cast<char*>(result.data()),std::streamsize(samples_*2));
    if(!file_) throw std::runtime_error("truncated HD audio");return result;
}
MovieWriter::MovieWriter(const std::string& path,uint32_t w,uint32_t h,uint32_t sw,uint32_t sh,uint32_t num,uint32_t den):
    out_(path,std::ios::binary),width_(w),height_(h),sourceWidth_(sw),sourceHeight_(sh),fpsNum_(num),fpsDen_(den) {
    Dimensions(w,h);Dimensions(sw,sh);if(!out_||!num||!den) throw std::runtime_error("cannot create HD movie");
    std::array<char,64> empty{};out_.write(empty.data(),empty.size());
}
void MovieWriter::Add(const uint8_t* rgb,int quality) {
    if(!rgb || entries_.size()>=120000) throw std::runtime_error("invalid HD movie frame count");
    std::vector<uint8_t> jpeg;
    const auto callback=[](void* context,void* bytes,int count) { auto& b=*static_cast<std::vector<uint8_t>*>(context);const auto* p=static_cast<uint8_t*>(bytes);b.insert(b.end(),p,p+count); };
    if(!stbi_write_jpg_to_func(callback,&jpeg,int(width_),int(height_),3,rgb,quality)) throw std::runtime_error("JPEG encode failed");
    entries_.emplace_back(uint64_t(out_.tellp()),uint32_t(jpeg.size()));out_.write(reinterpret_cast<const char*>(jpeg.data()),std::streamsize(jpeg.size()));
    if(!out_) throw std::runtime_error("HD movie write failed");
}
void MovieWriter::Finish(const std::vector<int16_t>& audio) {
    if(entries_.empty() || audio.size()%2 || audio.size()>44100ull*2*1800) throw std::runtime_error("invalid HD movie audio or frames");
    const uint64_t audioAt=uint64_t(out_.tellp());out_.write(reinterpret_cast<const char*>(audio.data()),std::streamsize(audio.size()*2));
    const uint64_t indexAt=uint64_t(out_.tellp());for(const auto& [at,size]:entries_) { Write(out_,at,8);Write(out_,size,4);Write(out_,0,4); }
    out_.seekp(0);out_.write("G2MEDIA1",8);Write(out_,width_,4);Write(out_,height_,4);Write(out_,entries_.size(),4);
    Write(out_,fpsNum_,4);Write(out_,fpsDen_,4);Write(out_,44100,4);Write(out_,indexAt,8);Write(out_,audioAt,8);Write(out_,audio.size(),8);Write(out_,sourceWidth_,4);Write(out_,sourceHeight_,4);
    out_.flush();if(!out_) throw std::runtime_error("HD movie finalization failed");out_.close();
}
bool Enabled() { return enabled; }
void SetEnabled(bool value) { if(enabled!=value) {enabled=value; ++generation;} }
std::string StartupMovie() {
    if(root.empty()) return {};
    auto path=root/"startup-hd.gtm";
    if(!enabled || !std::filesystem::is_regular_file(path)) path=root/"startup.gtm";
    return std::filesystem::is_regular_file(path)?path.string():std::string();
}
uint64_t Generation() { return generation; }
void SetRoot(const std::string& discPath,const std::string& exeSha1) {
    ++generation; root.clear();auto p=std::filesystem::path(discPath);if(!std::filesystem::is_directory(p)) p=p.parent_path();
    p/="hd";std::ifstream stamp(p/"profile.txt");std::string actual;stamp>>actual;if(actual==exeSha1) root=p;
}
std::string Asset(const std::string& name) {
    if(!enabled||root.empty()||name.find("..")!=std::string::npos||name.find(':')!=std::string::npos||name.empty()||name[0]=='/'||name.find('\\')!=std::string::npos) return {};
    const auto p=root/name;return std::filesystem::is_regular_file(p)?p.string():std::string();
}
}
