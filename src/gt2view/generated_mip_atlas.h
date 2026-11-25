#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gt2view {
// Each independently padded mip is packed into one hardware-filtered image.
// Never downsample the combined atlas: that would mix neighbouring materials.
class GeneratedMipAtlas {
public:
    static constexpr uint32_t kSide=4096, kLevels=11;
    std::vector<uint32_t> pixels;
    bool dirty=false;
    std::array<uint32_t,kLevels> Insert(uint32_t w,uint32_t h,const uint32_t* rgba) {
        if(!w || !h || w>1024 || h>1024) throw std::runtime_error("invalid generated mip dimensions");
        std::vector<uint32_t> level(size_t(w)*h);
        // Premultiplied colour prevents black transparent borders bleeding into leaves.
        for(size_t i=0;i<level.size();++i) {
            const uint32_t a=rgba[i]>>24;uint32_t c=a<<24;
            for(unsigned s=0;s<24;s+=8)c|=((((rgba[i]>>s)&255)*a+127)/255)<<s;
            level[i]=c;
        }
        std::array<uint32_t,kLevels> origins{};
        for(unsigned mip=0;mip<kLevels;++mip) {
            // Keep compression blocks private to one mip; fill the entire
            // rounded rectangle with edge texels, including its padding.
            const uint32_t paddedW=(w+5)&~3u,paddedH=(h+5)&~3u;
            if(x_+paddedW>kSide){x_=0;y_+=row_;row_=0;}
            if(y_+paddedH>kSide)throw std::runtime_error("generated mip atlas exhausted");
            if(pixels.empty())pixels.resize(size_t(kSide)*kSide);
            for(uint32_t y=0;y<paddedH;++y)for(uint32_t x=0;x<paddedW;++x) {
                const auto sx=std::clamp(int(x)-1,0,int(w)-1),sy=std::clamp(int(y)-1,0,int(h)-1);
                pixels[size_t(y_+y)*kSide+x_+x]=level[size_t(sy)*w+sx];
            }
            origins[mip]=(x_+1)|((y_+1)<<16);x_+=paddedW;row_=std::max(row_,paddedH);
            if(w==1 && h==1){std::fill(origins.begin()+mip+1,origins.end(),origins[mip]);break;}
            const uint32_t nw=std::max(1u,w/2),nh=std::max(1u,h/2);
            std::vector<uint32_t> next(size_t(nw)*nh);
            for(uint32_t y=0;y<nh;++y)for(uint32_t x=0;x<nw;++x){
                // Area bins also include the last row/column of odd-sized images.
                uint32_t sums[4]{},n=0;
                for(uint32_t sy=y*h/nh;sy<(y+1)*h/nh;++sy)for(uint32_t sx=x*w/nw;sx<(x+1)*w/nw;++sx){
                    const auto c=level[size_t(sy)*w+sx];for(unsigned k=0;k<4;++k)sums[k]+=(c>>(k*8))&255;++n;
                }
                uint32_t c=0;for(unsigned k=0;k<4;++k)c|=((sums[k]+n/2)/n)<<(k*8);next[size_t(y)*nw+x]=c;
            }
            level=std::move(next);w=nw;h=nh;
        }
        dirty=true;return origins;
    }
private:
    uint32_t x_=0,y_=0,row_=0;
};
}
