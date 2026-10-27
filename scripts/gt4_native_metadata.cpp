// Export CAR4/MDLS draw metadata through gt4formats. Owned data only.
#include "gt4formats/car4.h"
#include "gt4_attachment_trace.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <tuple>
#include <cmath>
using namespace gt4fmt;
namespace fs=std::filesystem;
std::vector<uint8_t> read(const fs::path& p) {
    std::ifstream f(p,std::ios::binary);if(!f)throw FormatError("input missing");
    return {(std::istreambuf_iterator<char>(f)),{}};
}
int main(int argc,char** argv) {try {
    if(argc<3 || argc>4)throw FormatError("owned CAR4 file, output directory, optional external Pat0");
    const fs::path source=argv[1],out=argv[2];fs::create_directories(out);
    auto bytes=read(source);const auto car=parseCar4(bytes);const auto body=car.slot(kSlotBody);
    const auto ms=parseModelSet(bytes,body);
    std::optional<Pat0> pat;
    std::vector<uint8_t> external;
    if(car.pats.contains(kSlotBodyPat))pat=car.pats.at(kSlotBodyPat);
    else {const fs::path ep=argc==4?fs::path(argv[3]):fs::path(source.string()+".pat");if(fs::is_regular_file(ep)){external=read(ep);pat=parsePat0(external,0);}}
    std::ofstream f(out/"draws.json");f.precision(9);
    f<<"{\"version\":8,\"materials\":[";
    const auto mats=[&](const ModelSet& set) {
        f<<'[';for(size_t m=0;m<set.materials.size();++m) {
            if(m)f<<',';const auto& mt=set.materials[m];f<<'[';
            for(int k=0;k<16;++k){if(k)f<<',';f<<mt.colors[k];}f<<','<<mt.f40<<']';
        }f<<']';
    };
    mats(ms);f<<"],\"paintMaterials\":[";
    // Pat0 patches both Tex1 and material rows. Texture-only deduplication must
    // not erase a native colour whose material changes instead of its CLUT.
    const unsigned colours=pat?pat->colors:unsigned(ms.variations.size());
    for(unsigned color=0;color<colours;++color) {
        if(color)f<<',';
        if(pat) {auto image=bytes;pat->apply(image,body,ms.size,int(color));mats(parseModelSet(image,body,false));}
        else {auto variant=ms;variant.materials=ms.variations.at(color);mats(variant);}
    }
    f<<"],\"states\":[";
    std::set<std::tuple<uint32_t,size_t,int,int,int,int,bool,std::array<float,16>>> geometry;
    std::set<std::pair<int,int>> images;
    for(int state=0;state<6;++state) {
        if(state)f<<',';f<<'[';bool comma=false;int cb[0x30]{};cb[0]=state&1;cb[7]=state>>1;
        int set=-1,tex=-1,material=-1;
        const auto draws=drawList(bytes,ms,0,0,0,0,false,cb);
        const auto attachments=CarAttachments(bytes,ms,cb);
        if(draws.size()!=attachments.size())throw FormatError("attachment trace draw count differs");
        size_t drawIndex=0;
        for(const auto& d:draws) {
            const auto attachment=attachments.at(drawIndex++);
            if(attachment.shape!=d.shape)throw FormatError("attachment trace shape differs");
            const auto& shape=ms.shapes.at(d.shape);const unsigned type=shape.flags8&31;
            if(type!=0 && type!=1 && type!=2 && type!=4 && type!=5)throw FormatError("unknown car vertex routine");
            for(size_t pi=0;pi<shape.packets.size();++pi) {
                const auto& pk=shape.packets[pi];
                if(pk.tex==0x1ff){set=-1;tex=-1;}else if(pk.tex && d.texset>=0){set=d.texset;tex=pk.tex-1;}
                if(pk.mat)material=pk.mat-1;
                const bool env=type==2,highlight=type==4;
                const bool textured=((pk.gif[1]>>15)&0x10)!=0 && !env && !highlight;
                if(textured && (set<0 || tex<0 || ms.texsets.empty() || size_t(set)>=ms.texsets[0].size()))continue;
                // Alpha-only destination masks are not colour geometry.
                if(d.fbmsk==0x00ffffff || d.fbmsk==0xffffffff)continue;
                if(comma)f<<',';comma=true;
                const bool blend=((pk.gif[1]>>15)&0x40)!=0;
                f<<"{\"shape\":"<<d.shape<<",\"packet\":"<<pi<<",\"material\":"<<material
                 <<",\"wheel\":"<<attachment.wheel<<",\"spinning\":"<<(attachment.spinning?"true":"false")
                 <<",\"type\":"<<type<<",\"texture\":"<<(textured?tex:-1)<<",\"texset\":"<<(textured?set:-1)
                 <<",\"blend\":"<<(blend?"true":"false")<<",\"blendEquation\":"<<d.blend0
                 <<",\"alphaFunc\":"<<(d.alphaTest?d.alphaFunc:-1)<<",\"alphaRef\":"<<d.alphaRef
                 // ModelSet2::begin (CORE.GT4 US 1.01, 00455370) enables
                 // capability 0x0f, exactly like command 0x2d. The parser's
                 // -1 means untouched, not disabled. Only 0x2e turns it off.
                 <<",\"alphaFail\":"<<d.alphaFail<<",\"cull\":"<<(d.cull!=0?"true":"false")
                 <<",\"depthWrite\":"<<(d.depthMask?"true":"false");
                if(textured) {
                    const auto& tr=ms.texsets[0].at(set)->textures.at(tex);const auto cl=tr.clamp;
                    const auto tf=tex0Fields(tr.tex0);
                    f<<",\"address\":["<<(cl&3)<<','<<((cl>>2)&3)<<','<<((cl>>4)&1023)<<','<<((cl>>14)&1023)<<','<<((cl>>24)&1023)<<','<<((cl>>34)&1023)<<"],\"textureAlpha\":"<<tf.tcc<<",\"textureFunction\":"<<tf.tfx;
                }
                std::array<float,16> matrix{};
                f<<",\"matrix\":[";
                for(size_t k=0;k<16;++k) {
                    matrix[k]=d.matrix[k];if(!std::isfinite(matrix[k]))throw FormatError("non-finite car instance matrix");
                    if(k)f<<',';f<<matrix[k];
                }
                f<<"],\"vertices\":[";
                // Vertices below are already transformed. Two uses of one
                // shape (e.g. left/right seats) are distinct geometry instances.
                const auto key=std::tuple{d.shape,pi,material,textured?set:-1,textured?tex:-1,attachment.wheel,attachment.spinning,matrix};
                bool vc=false;
                if(geometry.insert(key).second)for(const auto& tri:stripTriangles(pk.strips))for(const auto i:tri) {
                    const auto v=interpretVertex(pk.verts.at(i),shape.scale());if(!v.hasPos)throw FormatError("missing car position");
                    if(vc)f<<',';vc=true;f<<'[';
                    for(int k=0;k<3;++k){if(k)f<<',';float x=0;for(int j=0;j<3;++j)x+=v.pos[j]*d.matrix[4*j+k];f<<x+d.matrix[12+k];}
                    for(int k=0;k<3;++k){float x=0;for(int j=0;j<3;++j)x+=(v.hasNrm?v.nrm[j]:(j==1?1.f:0.f))*d.matrix[4*j+k];f<<','<<x;}
                    for(int k=0;k<2;++k)f<<','<<(v.hasUv?v.uv[k]:0.f);
                    for(int k=0;k<4;++k)f<<','<<(v.hasCol?v.col[k]/128.f:1.f);
                    f<<']';
                }
                f<<"]}";
                if(textured && images.emplace(set,tex).second) {
                    const auto& ts=ms.texsets[0].at(set);if(!ts)throw FormatError("missing car texture");
                    const auto im=decodeTexture(buildGs(*ts),ts->variantTex0(tex,0));
                    std::ofstream raw(out/(std::to_string(set)+"."+std::to_string(tex)+".rgba"),std::ios::binary);
                    raw.write(reinterpret_cast<const char*>(&im.w),4);raw.write(reinterpret_cast<const char*>(&im.h),4);
                    raw.write(reinterpret_cast<const char*>(im.rgba.data()),im.rgba.size()*4);
                }
            }
        }f<<']';
    }f<<"],\"paintImages\":[";
    for(unsigned c=0;c<colours;++c) {
        if(c)f<<',';f<<'{' ;bool comma=false;
        auto image=bytes;if(pat)pat->apply(image,body,ms.size,int(c));
        const auto variant=parseModelSet(image,body,false);
        for(const auto& [set,tex]:images) {
            const auto& ts=variant.texsets[0].at(set);if(!ts)throw FormatError("missing paint texture");
            const auto im=decodeTexture(buildGs(*ts),ts->variantTex0(tex,0));
            const std::string name=std::to_string(set)+"."+std::to_string(tex);
            const auto base=read(out/(name+".rgba"));
            if(base.size()==8+im.rgba.size()*4 && !std::memcmp(base.data()+8,im.rgba.data(),im.rgba.size()*4))continue;
            const std::string fn="paint-"+std::to_string(c)+"-"+name+".rgba";
            std::ofstream raw(out/fn,std::ios::binary);raw.write(reinterpret_cast<const char*>(&im.w),4);raw.write(reinterpret_cast<const char*>(&im.h),4);
            raw.write(reinterpret_cast<const char*>(im.rgba.data()),im.rgba.size()*4);
            if(comma)f<<',';comma=true;f<<'"'<<name<<"\":\""<<fn<<'"';
        }f<<'}';
    }f<<"]}\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 2;}}
