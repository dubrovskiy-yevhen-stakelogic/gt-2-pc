#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gt2::hd {
// Bound the neural residual against a deterministic reconstruction of the source.
// No previous frame is mixed in, so cuts and fast motion cannot leave trails.
inline void ConstrainMovieFrame(std::vector<uint8_t>& enhanced, int width, int height,
                                const std::vector<uint8_t>& source, int sw, int sh) {
    if(sw<=0 || sh<=0 || width<sw || height<sh || width>2048 || height>2048 ||
       enhanced.size()!=size_t(width)*height*4 || source.size()!=size_t(sw)*sh*4)
        throw std::runtime_error("invalid movie reconstruction dimensions");
    const auto cubic=[](float x) {
        x=std::abs(x);
        return x<=1 ? (1.5f*x-2.5f)*x*x+1 : x<2 ? ((-0.5f*x+2.5f)*x-4)*x+2 : 0;
    };
    struct Taps { int at[4]; float weight[4]; };
    const auto taps=[&](int count,int input) {
        std::vector<Taps> out(count);
        for(int i=0;i<count;++i) {
            const float p=(float(i)+0.5f)*float(input)/float(count)-0.5f;
            const int b=int(std::floor(p));
            for(int k=0;k<4;++k) {out[i].at[k]=std::clamp(b+k-1,0,input-1);out[i].weight[k]=cubic(p-float(b+k-1));}
        }
        return out;
    };
    const auto xs=taps(width,sw),ys=taps(height,sh);
    std::vector<float> rows(size_t(width)*sh*3);
    for(int y=0;y<sh;++y) for(int x=0;x<width;++x) for(int c=0;c<3;++c) {
        float sum=0;
        for(int k=0;k<4;++k) sum+=xs[x].weight[k]*source[(size_t(y)*sw+xs[x].at[k])*4+c];
        rows[(size_t(y)*width+x)*3+c]=sum;
    }
    for(int y=0;y<height;++y) for(int x=0;x<width;++x) {
        const size_t i=(size_t(y)*width+x)*4;
        for(int c=0;c<3;++c) {
            float base=0;
            for(int k=0;k<4;++k) base+=ys[y].weight[k]*rows[(size_t(ys[y].at[k])*width+x)*3+c];
            base=std::clamp(base,0.f,255.f);
            const float residual=std::clamp(float(enhanced[i+c])-base,-20.f,20.f)*0.5f;
            enhanced[i+c]=uint8_t(std::clamp(std::lround(base+residual),0l,255l));
        }
        enhanced[i+3]=255;
    }
}
}
