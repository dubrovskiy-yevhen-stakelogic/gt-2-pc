#pragma once
// Offline adaptation of Hyllian's MIT xBR-lv3. See third_party/xbr/LICENSE.txt.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gt2::hd {
inline std::vector<uint8_t> SmoothUiContours4x(const std::vector<uint8_t>& indices,
    const std::array<uint16_t,16>& palette,int size=256) {
    if(size<1 || size>256 || indices.size()!=size_t(size)*size ||
       std::any_of(indices.begin(),indices.end(),[](uint8_t i){return i>15;}))
        throw std::runtime_error("invalid contour atlas");
    std::array<float,16> luma{};
    std::array<std::array<float,3>,16> rgb{};
    for(int i=0;i<16;++i) {
        for(int c=0;c<3;++c) rgb[i][c]=float((palette[i]>>(5*c))&31)/31.f;
        luma[i]=palette[i]?48.f*(.299f*rgb[i][0]+.587f*rgb[i][1]+.114f*rgb[i][2]):-48.f;
    }
    const auto ramp=[](float value,float centre) {const float t=std::clamp((value-centre+.4f)/.8f,0.f,1.f);return t*t*(3-2*t);};
    std::vector<uint8_t> result(size_t(size*4)*size*4*4,255);
    for(int y=0;y<size;++y) for(int x=0;x<size;++x) {
        const int centre=indices[size_t(y*size+x)];
        struct Edge {bool active=false,left=false,up=false,left3=false,up3=false;int next=0;float distance=0;};
        Edge edges[4];
        for(int rotation=0;rotation<4;++rotation) {
            const auto index=[&](int dx,int dy) {
                for(int r=0;r<rotation;++r) {const int old=dx;dx=dy;dy=-old;}
                return int(indices[size_t(std::clamp(y+dy,0,size-1)*size+std::clamp(x+dx,0,size-1))]);
            };
            const auto sample=[&](int dx,int dy){return luma[index(dx,dy)];};
            const float e=luma[centre],b=sample(0,-1),c=sample(1,-1),d=sample(-1,0),f=sample(1,0),
                g=sample(-1,1),h=sample(0,1),i=sample(1,1),f4=sample(2,0),h5=sample(0,2),
                i4=sample(2,1),i5=sample(1,2),c1=sample(1,-2),g0=sample(-2,1),b1=sample(0,-2),d0=sample(-2,0);
            const auto eq=[](float a,float z){return std::abs(a-z)<10.f;};
            const bool restriction=e!=f && e!=h && ((!eq(f,b)&&!eq(f,c)) || (!eq(h,d)&&!eq(h,g)) ||
                (eq(e,i)&&((!eq(f,f4)&&!eq(f,i4))||(!eq(h,h5)&&!eq(h,i5)))) || eq(e,g)||eq(e,c));
            const float diagonal=std::abs(e-c)+std::abs(e-g)+std::abs(i-h5)+std::abs(i-f4)+4*std::abs(h-f);
            const float cross=std::abs(h-d)+std::abs(h-i5)+std::abs(f-i4)+std::abs(f-b)+4*std::abs(e-i);
            auto& edge=edges[rotation];edge.active=restriction && diagonal<cross;
            edge.left=2*std::abs(f-g)<=std::abs(h-c) && e!=g && d!=g;
            edge.up=std::abs(f-g)>=2*std::abs(h-c) && e!=c && b!=c;
            edge.left3=std::abs(g-g0)<2 && std::abs(d0-g0)>=2;
            edge.up3=std::abs(c-c1)<2 && std::abs(b1-c1)>=2;
            edge.next=std::abs(e-f)<=std::abs(e-h)?index(1,0):index(0,1);
            for(int channel=0;channel<3;++channel) edge.distance+=std::abs(rgb[centre][channel]-rgb[edge.next][channel]);
            if(!palette[centre] != !palette[edge.next]) edge.distance+=3;
        }
        for(int sy=0;sy<4;++sy) for(int sx=0;sx<4;++sx) {
            float best=-1,weight=0;int next=centre;
            for(int rotation=0;rotation<4;++rotation) {
                const auto& edge=edges[rotation];if(!edge.active) continue;
                float u=(sx+.5f)/4.f,v=(sy+.5f)/4.f;
                for(int r=0;r<rotation;++r) {const float old=u;u=1-v;v=old;}
                float w=ramp(v+u,1.5f);
                if(edge.left) {w=(std::max)(w,ramp(v+.5f*u,1.f));if(edge.left3) w=(std::max)(w,ramp(6*v+2*u,5.f));}
                if(edge.up) {w=(std::max)(w,ramp(v+2*u,2.f));if(edge.up3) w=(std::max)(w,ramp(2*v+6*u,5.f));}
                if(w*edge.distance>best) {best=w*edge.distance;weight=w;next=edge.next;}
            }
            const size_t at=size_t((y*4+sy)*size*4+x*4+sx)*4;
            result[at]=uint8_t(centre);result[at+1]=uint8_t(next);result[at+2]=uint8_t(weight*255+.5f);
        }
    }
    return result;
}
}
