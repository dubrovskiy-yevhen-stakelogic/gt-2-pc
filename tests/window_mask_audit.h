#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

struct WindowTintCore {size_t total=0,largest=0;};
inline WindowTintCore WindowTintCores(std::span<const uint8_t> mask,size_t width,size_t eyeHeight) {
    if(width<3 || eyeHeight<3 || mask.size()!=width*eyeHeight*2)throw std::runtime_error("invalid stereo window mask");
    WindowTintCore result;std::vector<uint8_t> cores(mask.size());
    // Erode each eye independently. Count connected cores, not the sum of
    // disconnected texture-edge samples spread across the dashboard/frame.
    // A single visible pane/triangle is still a failure at the same 16-pixel
    // threshold; scattered one-pixel texture fringes cannot form a pane.
    for(size_t eye=0;eye<2;++eye)for(size_t y=1;y+1<eyeHeight;++y)for(size_t x=1;x+1<width;++x){
        const size_t at=(eye*eyeHeight+y)*width+x;bool core=true;
        for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx)core=core && mask[size_t(ptrdiff_t(at)+dy*ptrdiff_t(width)+dx)];
        cores[at]=uint8_t(core);result.total+=core;
    }
    for(size_t at=0;at<cores.size();++at)if(cores[at]){
        std::vector<size_t> pending{at};cores[at]=0;
        for(size_t i=0;i<pending.size();++i)for(ptrdiff_t offset:{-ptrdiff_t(width),ptrdiff_t(width),ptrdiff_t(-1),ptrdiff_t(1)}){
            const auto next=size_t(ptrdiff_t(pending[i])+offset);
            if(next<cores.size() && cores[next]){cores[next]=0;pending.push_back(next);}
        }
        result.largest=std::max(result.largest,pending.size());
    }
    return result;
}
