#pragma once
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace gt2::hd {
struct Picture { uint32_t width=0, height=0; std::vector<uint8_t> rgb; };
// Indexed JPEG frames and 44.1 kHz stereo PCM. All multibyte fields are little-endian.
class Movie {
public:
    explicit Movie(const std::string& path);
    uint32_t width=0, height=0, sourceWidth=0, sourceHeight=0, frames=0, fpsNum=0, fpsDen=0;
    Picture Frame(uint32_t index);
    std::vector<int16_t> Audio();
private:
    std::ifstream file_;
    uint64_t length_=0, index_=0, audio_=0, samples_=0;
    std::vector<std::pair<uint64_t,uint32_t>> entries_;
};
class MovieWriter {
public:
    MovieWriter(const std::string& path, uint32_t width, uint32_t height,
                uint32_t sourceWidth, uint32_t sourceHeight, uint32_t fpsNum, uint32_t fpsDen);
    void Add(const uint8_t* rgb, int quality=93);
    void Finish(const std::vector<int16_t>& audio);
private:
    std::ofstream out_;
    uint32_t width_,height_,sourceWidth_,sourceHeight_,fpsNum_,fpsDen_;
    std::vector<std::pair<uint64_t,uint32_t>> entries_;
};
bool Enabled();
void SetEnabled(bool enabled);
std::string StartupMovie();
uint64_t Generation();
void SetRoot(const std::string& discPath, const std::string& exeSha1);
std::string Asset(const std::string& name);
}
