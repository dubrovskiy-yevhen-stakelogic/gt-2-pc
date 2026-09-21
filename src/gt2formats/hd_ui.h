#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <istream>
#include <stdexcept>
#include <vector>

namespace gt2::hd {
inline std::vector<std::string> ReadFontIndex(std::istream& input) {
    std::string key;
    std::vector<std::string> keys;
    if(!(input>>key) || key!="palette-contours4x-v1") return keys;
    while(input>>key) if(key.size()==21 && key[16]=='-' &&
        key.find_first_not_of("0123456789abcdef",0)==16 &&
        key.find_first_not_of("0123456789abcdef",17)==std::string::npos) keys.push_back(key);
    return keys;
}
// Palette-independent keys let the original game retain its fades, colours and STP classes.
inline std::string UiPageKey(const uint16_t* words, uint32_t stride = 1024, uint32_t depth = 0) {
    uint64_t hash = 14695981039346656037ull ^ depth;
    for (uint32_t y = 0; y < 256; ++y) for (uint32_t x = 0; x < (64u << depth); ++x) {
        const uint16_t w = words[y * stride + x];
        hash = (hash ^ (w & 255)) * 1099511628211ull;
        hash = (hash ^ (w >> 8)) * 1099511628211ull;
    }
    char key[32]; std::snprintf(key, sizeof(key), "%016llx", static_cast<unsigned long long>(hash));
    return key;
}
inline std::vector<uint8_t> ScaleUi2x(const std::vector<uint8_t>& src, int size) {
    std::vector<uint8_t> dst(size_t(size) * size * 4);
    const auto at = [&](int x, int y) { return src[size_t(std::clamp(y, 0, size-1)) * size + std::clamp(x, 0, size-1)]; };
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const auto b=at(x,y-1), d=at(x-1,y), e=at(x,y), f=at(x+1,y), h=at(x,y+1);
        const size_t p = size_t(y*2) * size*2 + x*2;
        dst[p] = b!=h && d!=f && d==b ? d:e;
        dst[p+1] = b!=h && d!=f && b==f ? f:e;
        dst[p+size*2] = b!=h && d!=f && d==h ? d:e;
        dst[p+size*2+1] = b!=h && d!=f && h==f ? f:e;
    }
    return dst;
}
inline std::vector<uint8_t> EnlargeUiPage(const uint16_t* words, uint32_t depth=0) {
    std::vector<uint8_t> pixels(256*256);
    for (uint32_t y=0;y<256;++y) for(uint32_t x=0;x<256;++x)
        pixels[y*256+x] = uint8_t((words[y*1024+(x>>(2-depth))] >> ((x&((1u<<(2-depth))-1))*(4u<<depth))) & ((1u<<(4u<<depth))-1));
    return ScaleUi2x(ScaleUi2x(pixels,256),512);
}
inline std::vector<uint8_t> EnlargeUiRegion(const uint16_t* words, int x, int y, int size) {
    if(x<0 || y<0 || size<=0 || x+size>256 || y+size>256) throw std::runtime_error("invalid UI region");
    std::vector<uint8_t> pixels(size_t(size)*size);
    for(int row=0;row<size;++row) for(int col=0;col<size;++col) {
        const int u=x+col;
        pixels[size_t(row)*size+col]=uint8_t((words[(y+row)*1024+u/4] >> ((u%4)*4))&15);
    }
    return ScaleUi2x(ScaleUi2x(pixels,size),size*2);
}

}
