// Export material metadata through the gt4formats parser.
// Reads owned CAR4 data; emits original GS draw states and lamp-only geometry.
#include "gt4formats/car4.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <cmath>
#include <tuple>
using namespace gt4fmt;
namespace fs=std::filesystem;
int main(int argc,char** argv) {try {
    if(argc!=3)throw FormatError("owned CAR4 file, output directory");
    std::ifstream in(argv[1],std::ios::binary);if(!in)throw FormatError("input missing");
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),{});
    const auto car=parseCar4(bytes);auto ms=parseModelSet(bytes,car.slot(kSlotBody));
    const fs::path out=argv[2];fs::create_directories(out);
    std::ofstream f(out/"draws.json");f<<"{\"states\":[";
    std::set<std::pair<int,int>> images;
    std::set<std::tuple<uint32_t,size_t,int,int,int>> geometry;
    f.precision(9);
    for(int state=0;state<6;++state) {
        if(state)f<<',';f<<'[';bool comma=false;
        int cb[0x30]{};cb[0]=state&1;cb[7]=state>>1;
        int texset=-1,tex=-1,material=-1;
        for(const auto& d:drawList(bytes,ms,0,0,0,0,false,cb)) {
            const auto& shape=ms.shapes.at(d.shape);
            if((shape.flags8&31)!=0 && (shape.flags8&31)!=1)continue;
            for(size_t pi=0;pi<shape.packets.size();++pi) {
                const auto& p=shape.packets[pi];
                if(p.tex==0x1ff){texset=-1;tex=-1;}
                else if(p.tex && d.texset>=0){texset=d.texset;tex=p.tex-1;}
                if(p.mat)material=p.mat-1;
                const bool textured=((p.gif[1]>>15)&0x10)!=0;
                if(textured && (texset<0 || ms.texsets.empty() || size_t(texset)>=ms.texsets[0].size()))continue;
                if(d.fbmsk==0x00ffffff)continue;
                if(comma)f<<',';comma=true;
                const bool blend=((p.gif[1]>>15)&0x40)!=0;
                f<<"{\"shape\":"<<d.shape<<",\"material\":"<<material<<",\"texture\":"<<(textured?tex:-1)
                  <<",\"texset\":"<<texset<<",\"blend\":"<<(blend?"true":"false")
                  <<",\"blendEquation\":"<<d.blend0<<",\"alphaTest\":"<<(d.alphaTest?"true":"false")
                  <<",\"alphaFunc\":"<<d.alphaFunc<<",\"alphaRef\":"<<d.alphaRef<<",\"colors\":[";
                for(int k=0;k<16;++k){if(k)f<<',';f<<(material>=0 && size_t(material)<ms.materials.size()?ms.materials[material].colors[k]:((k%4)==3?1.f:(k<8?1.f:0.f)));}
                f<<"],\"packet\":"<<pi<<",\"vertices\":[";bool vc=false;
                const bool emit=geometry.emplace(d.shape,pi,material,texset,tex).second;
                if(emit)for(const auto& tri:stripTriangles(p.strips))for(auto i:tri) {
                    const auto v=interpretVertex(p.verts.at(i),shape.scale());
                    if(!v.hasPos)throw FormatError("lamp vertex position missing");
                    if(vc)f<<',';vc=true;f<<'[';
                    for(int k=0;k<3;++k){if(k)f<<',';f<<v.pos[k];}
                    for(int k=0;k<3;++k)f<<','<<(v.hasNrm?v.nrm[k]:(k==1?1:0));
                    for(int k=0;k<2;++k)f<<','<<(v.hasUv?v.uv[k]:0.f);
                    for(int k=0;k<4;++k)f<<','<<(v.hasCol?v.col[k]/128.f:1.f);
                    f<<']';
                }
                f<<"]}";
                if(textured && tex>=0 && images.emplace(texset,tex).second) {
                    const auto& ts=ms.texsets[0].at(texset);if(!ts)throw FormatError("texture set missing");
                    const auto im=decodeTexture(buildGs(*ts),ts->variantTex0(tex,0));
                    std::ofstream raw(out/(std::to_string(texset)+"."+std::to_string(tex)+".rgba"),std::ios::binary);
                    raw.write(reinterpret_cast<const char*>(&im.w),4);raw.write(reinterpret_cast<const char*>(&im.h),4);
                    raw.write(reinterpret_cast<const char*>(im.rgba.data()),im.rgba.size()*4);
                }
            }
        }
        f<<']';
    }
    f<<"]}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
