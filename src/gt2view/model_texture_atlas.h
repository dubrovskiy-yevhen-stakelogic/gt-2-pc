#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace gt2view {
// GT4 textures keep native resolution. A one-pixel repeating border lets the
// hardware bilinear sampler filter independently of neighbouring materials.
class ModelTextureAtlas {
public:
    // The live eight-car texture budget is 8M texels, before tile borders.
    static constexpr uint32_t kSide=4096;
    std::vector<uint32_t> pixels;
    bool dirty=false;
    bool Insert(uint32_t key,uint32_t width,uint32_t height,const std::vector<uint32_t>& rgba,uint32_t& origin,bool nativeColour=false) {
        if (!width || !height || width+2>kSide || height+2>kSide || rgba.size()!=size_t(width)*height) return false;
        const uint64_t id=(uint64_t(key)<<32)|(height<<16)|width;
        const auto previous=entries_.find(id);
        if (previous!=entries_.end() && !invalid_.count(id)) { origin=previous->second;return true; }
        uint32_t x=x_,y=y_,row=row_;
        if (previous!=entries_.end()) { x=(previous->second&65535u)-1;y=(previous->second>>16)-1; }
        else if (x+width+2>kSide) { x=0;y+=row;row=0; }
        if (y+height+2>kSide) return false;
        if (pixels.empty()) pixels.resize(size_t(kSide)*kSide);
        // Native indexed textures contain low-contrast ordered colour dithering.
        // Filter that colour once at upload; alpha/reflectivity and hard edges
        // remain byte-exact. No extra per-frame shader samples are needed.
        std::vector<uint32_t> filtered;
        if(nativeColour) {
            filtered=rgba;
            for(uint32_t py=0;py<height;++py)for(uint32_t px=0;px<width;++px) {
                const auto centre=rgba[size_t(py)*width+px];uint32_t sums[3]{},weight=0;
                for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx) {
                    const auto sx=uint32_t(std::clamp(int(px)+dx,0,int(width)-1));
                    const auto sy=uint32_t(std::clamp(int(py)+dy,0,int(height)-1));
                    const auto sample=rgba[size_t(sy)*width+sx];bool similar=true;
                    for(int c=0;c<3;++c)if(std::abs(int((sample>>(c*8))&255u)-int((centre>>(c*8))&255u))>16)similar=false;
                    if(!similar)continue;
                    const uint32_t w=dx==0 && dy==0?4u:1u;weight+=w;
                    for(int c=0;c<3;++c)sums[c]+=w*((sample>>(c*8))&255u);
                }
                uint32_t colour=centre & 0xff000000u;
                for(int c=0;c<3;++c)colour|=((sums[c]+weight/2)/weight)<<(c*8);
                filtered[size_t(py)*width+px]=colour;
            }
        }
        const auto& sampled=nativeColour?filtered:rgba;
        for (uint32_t v=0;v<height+2;++v) for (uint32_t u=0;u<width+2;++u)
            pixels[size_t(y+v)*kSide+x+u]=sampled[size_t((v+height-1)%height)*width+(u+width-1)%width];
        origin=(x+1)|((y+1)<<16);entries_.emplace(id,origin);
        if (previous==entries_.end()) { x_=x+width+2;y_=y;row_=(std::max)(row,height+2); }
        invalid_.erase(id);dirty=true;return true;
    }
    void Invalidate(uint32_t key) {
        for (const auto& entry:entries_) if (uint32_t(entry.first>>32)==key) invalid_.insert(entry.first);
    }
    bool Source(uint32_t origin,uint32_t& key) const {
        for (const auto& entry:entries_) if (entry.second==origin) { key=uint32_t(entry.first>>32);return true; }
        return false;
    }
private:
    uint32_t x_=0,y_=0,row_=0;
    std::map<uint64_t,uint32_t> entries_;
    std::set<uint64_t> invalid_;
};
}
