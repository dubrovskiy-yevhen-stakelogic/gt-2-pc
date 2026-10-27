#pragma once
#include "offscreen_car_audit.h"
#include "gt2view/cockpit_fit.h"
inline size_t MarkCockpitSeamHoles(const std::vector<uint8_t>& front,
    const std::vector<uint8_t>& uncut,const std::vector<uint8_t>& uncutGreen,
    std::vector<uint8_t>& marked,uint32_t size,const std::array<float,3>& eye,
    const gt2view::CockpitFit& fit) {
    size_t holes=0;
            auto background=[&](size_t p){return front[p]>250 && front[p+1]<3 && front[p+2]>250;};
            // A road/window region is open upward. A thin strip of outside
            // bounded above by the bonnet and below by the dash is a hole.
            for(size_t e=0;e<2;++e)for(size_t x=size/3;x<size*2/3;++x) {
                auto at=eye;at[0]+=(e?.032f:-.032f);const auto vp=View(at,0,-8);
                auto project=[&](const auto& p){float clip[4]{};for(size_t r=0;r<4;++r){clip[r]=vp[12+r];for(size_t k=0;k<3;++k)clip[r]+=vp[k*4+r]*p[k];}return std::array<float,2>{(clip[0]/clip[3]+1)*size*.5f,(clip[1]/clip[3]+1)*size*.5f};};
                float boundary=-1000;
                for(size_t i=1;i<fit.cowl.size();++i){const auto a=project(fit.cowl[i-1]),b=project(fit.cowl[i]);if(float(x)>=a[0] && float(x)<=b[0])boundary=a[1]+(b[1]-a[1])*(float(x)-a[0])/std::max(.001f,b[0]-a[0]);}
                const auto headerA=project(fit.frontHeader[0]),headerB=project(fit.frontHeader[1]);
                const float header=fit.openTop?-1000.f:headerA[1]+(headerB[1]-headerA[1])*
                    (float(x)-headerA[0])/std::max(.001f,headerB[0]-headerA[0]);
                bool solid=false;size_t run=0;
                for(size_t y=size/3;y<size*3/4;++y) {
                    const size_t p=((e*size+y)*size+x)*4;
                    if(background(p)){if(solid)++run;}
                    else {
                        // Roof/header is above this band. Its legitimate narrow
                        // windshield opening must not be mistaken for a bonnet slit.
                        if(solid && run>0 && run<size/16 && float(y-run)>header+4 && float(y-run)>=boundary-12 && float(y)<=boundary+6)
                            for(size_t yy=y-run;yy<y;++yy){const size_t q=((e*size+yy)*size+x)*4;
                                // Include the bonnet side of the cowl seam. A
                                // missing overlap is just above this edge.
                                if(float(yy)+.5f<boundary-4)continue;
                                // Ignore a one-pixel raster edge around authored
                                // wipers/window borders. A leak must contain a
                                // fully open interior pixel, not edge coverage.
                                if(!background(q-4) || !background(q+4) ||
                                   !background(q-size*4) || !background(q+size*4))continue;
                                // Gaps around authored wipers/mirrors also exist
                                // on the uncut car and are not missing bodywork.
                                const auto covered=[&](size_t pixel){return
                                    std::abs(int(uncut[pixel])-int(uncutGreen[pixel]))+
                                    std::abs(int(uncut[pixel+1])-int(uncutGreen[pixel+1]))+
                                    std::abs(int(uncut[pixel+2])-int(uncutGreen[pixel+2]))<4;};
                                // Erode the reference silhouette as well as the
                                // open target. A one-pixel distant lamp edge seen
                                // through an open window is not a bonnet panel.
                                if(covered(q) && covered(q-4) && covered(q+4) &&
                                   covered(q-size*4) && covered(q+size*4)){
                                    ++holes;marked[q]=marked[q+1]=255;marked[q+2]=0;
                                }
                            }
                        solid=true;run=0;
                    }
                }
            }
    return holes;
}
