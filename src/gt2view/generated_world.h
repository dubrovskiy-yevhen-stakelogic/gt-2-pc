#pragma once
#include "gt2formats/generated_world.h"
#include "gt2formats/generated_texture_catalogue.h"
#include "gt2formats/png_reader.h"
#include "gt2formats/sha1.h"
#include "gt2formats/sponsor_boards.h"
#include "gt2view/scene_renderer.h"
#include <fstream>
#include <map>
#include <sstream>
#include <cstdio>
#include <cstring>

namespace gt2view {
// Explicit authored source rectangles, bound to the course texture-pack hash.
// A material is selected by its original texture page, palette and UV bounds.
class GeneratedWorld {
    struct Region {
        uint32_t page, clut, depth;
        int32_t x, y;
        uint32_t width, height, base, size, category;
        bool asphalt=false;
        unsigned sash=0;
    };
    bool paletteAlpha_=false;
    std::map<std::pair<uint32_t,uint32_t>,std::vector<Region>> regions_;
public:
    void Load(const std::string& course, std::span<const uint8_t> source, SceneRenderer& renderer, uint32_t& cursor) {
        regions_.clear();
        size_t regionCount=0;
#ifndef __EMSCRIPTEN__
        if(cursor==SceneRenderer::kGeneratedWorldTexelBase)renderer.BeginGeneratedTextures((gt2::generated::root/"_gpu").string());
#endif
        if (course.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) return;
        const auto dir = gt2::generated::root / course;
        std::ifstream external(dir / "pack.txt");
        const auto sourceHash=gt2::Sha1Hex(source);
        std::istringstream builtin(std::string(gt2::generated::BuiltinTextureManifest(course,sourceHash)));
        std::istream& manifest=external?static_cast<std::istream&>(external):static_cast<std::istream&>(builtin);
        std::string magic, expected;
        if (!(manifest >> magic >> expected)) return;
        paletteAlpha_=magic=="GT2-GENERATED-WORLD-2";
        if ((!paletteAlpha_ && magic != "GT2-GENERATED-WORLD-1") || expected != sourceHash) {
            std::printf("Generated world: rejected pack for %s (source mismatch)\n", course.c_str()); return;
        }
        std::map<std::string,std::pair<uint32_t,uint32_t>> images;
        uint32_t aggregateBase=0, roomAtlas=0;
        std::string line; std::getline(manifest,line);
        while (std::getline(manifest,line)) {
            if (line.empty() || line[0]=='#') continue;
            std::istringstream row(line);
            uint32_t page,clut,x,y,w,h; std::string file;
            if (!(row >> page >> clut >> x >> y >> w >> h >> file) || page>511 || clut>32767 ||
                !w || !h || x>=256 || y>=256 || w>256-x || h>256-y ||
                file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=std::string::npos ||
                file.find("..")!=std::string::npos) continue;
            std::string group;
            if (!(row >> group)) {
                if (file.starts_with("asphalt")) group="road";
                else if (file=="tree.png" || file=="sapling.png" || file=="hedge.png") group="vegetation";
                else if (file=="brick.png" || file=="facade.png" || file=="garage.png") group="buildings";
                else group="trackside";
            }
            const auto category=std::find(gt2::generated::keys.begin(),gt2::generated::keys.end(),group);
            if(category==gt2::generated::keys.end())continue;
            std::string surface;
            const bool asphalt=(row >> surface) && surface=="asphalt" && group=="road";
            const bool hdRooms=surface=="rooms-hd-day" || surface=="rooms-hd-night";
            const bool rooms=group=="buildings" && (surface=="rooms-day" || surface=="rooms-night" || hdRooms);
            try {
                if(asphalt && !aggregateBase) {
                    const auto tile=gt2::ReadPngFile((gt2::generated::root/"_shared"/"seattle-aggregate-v4.png").string());
                    if(tile.width!=512 || tile.height!=512 || cursor+(512u*512u*4u-1u)/3u>SceneRenderer::kGeneratedHudBase)
                        throw std::runtime_error("invalid asphalt tile or exhausted texture budget");
                    std::vector<uint32_t> pixels(512*512);
                    std::memcpy(pixels.data(),tile.rgba.data(),pixels.size()*4);
                    // Box-filtered mip chain, consecutive largest to smallest.
                    for(unsigned side=512,offset=0;side>1;side/=2) {
                        const unsigned next=offset+side*side;
                        pixels.resize(next+side*side/4);
                        for(unsigned my=0;my<side/2;++my) for(unsigned mx=0;mx<side/2;++mx) {
                            const unsigned i=offset+2*my*side+2*mx;
                            uint32_t result=0xff000000;
                            for(unsigned shift=0;shift<24;shift+=8) {
                                unsigned sum=0;
                                for(unsigned delta:{0u,1u,side,side+1})
                                    sum+=(pixels[i+delta]>>shift)&255;
                                result|=((sum+2)/4)<<shift;
                            }
                            pixels[next+my*(side/2)+mx]=result;
                        }
                        offset=next;
                    }
                    aggregateBase=cursor;
#ifdef __EMSCRIPTEN__
                    renderer.UploadExternalTexture(cursor,uint32_t(pixels.size()),pixels.data());
                    cursor+=uint32_t(pixels.size());
#else
                    renderer.UploadGeneratedTexture(cursor,512,512,pixels.data());
                    cursor+=512*512+GeneratedMipAtlas::kLevels;
#endif
                }
                auto it=images.find(file);
                if (it==images.end()) {
                    const auto local=dir/file;
                    const auto imagePath=std::filesystem::is_regular_file(local)?local:gt2::generated::root/"_shared"/file;
                    if(!std::filesystem::is_regular_file(imagePath))continue;
                    const auto png=gt2::ReadPngFile(imagePath.string());
                    if (png.width<1 || png.height<1 || png.width>1024 || png.height>1024) throw std::runtime_error("image exceeds 1024x1024 budget");
                    const uint32_t count=uint32_t(png.width*png.height);
#ifdef __EMSCRIPTEN__
                    const uint32_t mipWords=0;
#else
                    const uint32_t mipWords=GeneratedMipAtlas::kLevels;
#endif
                    if (uint64_t(cursor)+count+mipWords+unsigned(asphalt)>SceneRenderer::kGeneratedHudBase) throw std::runtime_error("external texture budget exhausted");
                    std::vector<uint32_t> pixels(count);std::memcpy(pixels.data(),png.rgba.data(),size_t(count)*4);
                    if(rooms) {
                        const unsigned maskWidth=hdRooms ? 256u : w;
                        const unsigned maskHeight=hdRooms ? 256u : h;
                        // Final-art registrations use an independent grid; legacy masks use native UV resolution.
                        // Keep the room art shared across all buildings on a course.
                        const uint32_t atlasArea=192*192;
                        std::vector<uint32_t> boxes;
                        uint32_t box;
                        while(row>>box) {
                            unsigned l=box&255,t=(box>>8)&255,bw=(box>>16)&255,bh=box>>24;
                            if(!bw || !bh || l+bw>maskWidth || t+bh>maskHeight) throw std::runtime_error("invalid room bounds");
                            boxes.push_back(box);
                        }
                        // Four 8-bit indices per word. Keep the dense encoding for
                        // unusually large sheets with more than 255 openings.
                        const bool indexed=boxes.size()<=255;
                        std::vector<uint32_t> mask(indexed ? (maskWidth*maskHeight+3)/4 : maskWidth*maskHeight,0);
                        for(size_t i=0;i<boxes.size();++i) {
                            const uint32_t b=boxes[i];
                            const unsigned left=b&255,top=(b>>8)&255;
                            for(unsigned v=top;v<top+(b>>24);++v) {
                                for(unsigned u=left;u<left+((b>>16)&255);++u) {
                                    const unsigned at=v*maskWidth+u;
                                    if(indexed) {
                                        const unsigned shift=(at%4)*8;
                                        mask[at/4]=(mask[at/4]&~(255u<<shift))|(uint32_t(i+1)<<shift);
                                    } else mask[at]=b;
                                }
                            }
                        }
                        const uint32_t maskBase=cursor+(roomAtlas ? 0 : atlasArea);
                        const uint32_t boxesBase=maskBase+uint32_t(mask.size());
                        if(indexed) mask.insert(mask.end(),boxes.begin(),boxes.end());
                        if(uint64_t(maskBase)+count+mipWords+mask.size()+5>SceneRenderer::kGeneratedHudBase)
                            throw std::runtime_error("room texture budget exhausted");
                        if(!roomAtlas) {
                            auto atlas=gt2::ReadPngFile((gt2::generated::root/"_shared"/"room-atlas.png").string());
                            if(atlas.width!=192 || atlas.height!=192) throw std::runtime_error("invalid interior atlas");
                            std::vector<uint32_t> art(atlasArea);
                            std::memcpy(art.data(),atlas.rgba.data(),atlasArea*4);
                            renderer.UploadExternalTexture(cursor,atlasArea,art.data());
                            roomAtlas=cursor;
                            cursor+=atlasArea;
                        }
                        mask.push_back(maskBase);
                        mask.push_back(indexed ? boxesBase : 0);
                        mask.push_back(roomAtlas);
                        mask.push_back(maskWidth | (maskHeight<<16));
                        mask.push_back(surface=="rooms-night" || surface=="rooms-hd-night");
                        renderer.UploadExternalTexture(cursor,uint32_t(mask.size()),mask.data());
                        cursor+=uint32_t(mask.size());
                    }
                    if(asphalt) renderer.UploadExternalTexture(cursor++,1,&aggregateBase);
#ifdef __EMSCRIPTEN__
                    renderer.UploadExternalTexture(cursor,count,pixels.data());
#else
                    renderer.UploadGeneratedTexture(cursor,png.width,png.height,pixels.data());
#endif
                    if(std::getenv("GT2_TRACE_FACADE") && file.find("final-") != std::string::npos)
                        std::printf("Final facade %s base=%u size=%dx%d source=%s\n",file.c_str(),cursor,png.width,png.height,imagePath.string().c_str());
                    it=images.emplace(file,std::pair{cursor,uint32_t(png.width)|(uint32_t(png.height)<<16)}).first;
                    cursor+=count+mipWords;
                }
                const auto pageWord=(page&15)*64|(((page>>4)&1)*256<<16);
                const auto clutWord=(clut&63)*16|((clut>>6)<<16);
                regions_[{pageWord,clutWord}].push_back({pageWord,clutWord,(page>>7)&3,int32_t(x),int32_t(y),w,h,it->second.first,it->second.second,
                    1u<<unsigned(category-gt2::generated::keys.begin()),asphalt,rooms ? 3u : group=="buildings" ? (surface=="sash-small" ? 1u : surface=="sash-tall" ? 2u : 0u) : 0u});
                ++regionCount;
            } catch (const std::exception& e) { std::printf("Generated world: %s: %s\n",file.c_str(),e.what()); }
        }
        std::printf("Generated world: %s, %zu regions, %zu images\n",course.c_str(),regionCount,images.size());
    }
    void KeepLiveUploads(std::span<const uint16_t> before,std::span<const uint16_t> live) {
        for(auto& [key,regions]:regions_) {
            std::erase_if(regions,[&](const Region& r) {
                const auto wordChanged=[&](uint32_t x,uint32_t y){const size_t i=(y%512)*1024+(x%1024);return before[i]!=live[i];};
                if(r.depth<2)for(uint32_t c=0;c<(r.depth?256u:16u);++c)
                    if(wordChanged((r.clut&65535)+c,r.clut>>16))return true;
                const uint32_t pixelsPerWord=r.depth==0?4:r.depth==1?2:1;
                for(uint32_t y=r.y;y<r.y+r.height;++y)
                    for(uint32_t x=r.x/pixelsPerWord;x<=(r.x+r.width-1)/pixelsPerWord;++x)
                        if(wordChanged((r.page&65535)+x,(r.page>>16)+y))return true;
                return false;
            });
        }
    }
    // The race chooses its adverts after loading the TRP. Bind each selected TIM
    // to its actual VRAM slot. Reverse layouts may address a slot through a
    // different 4-bit texture page, so derive its UV origin from that page.
    bool BindSponsors(const std::vector<gt2::SponsorUpload>& uploads,SceneRenderer& renderer,uint32_t& cursor) {
        const auto dir=gt2::generated::root/"_sponsors";
        if(!paletteAlpha_ || regions_.empty() || !std::filesystem::is_directory(dir))return false;
        std::vector<Region> selected;
        std::map<std::string,std::pair<uint32_t,uint32_t>> images;
        for(const auto& u:uploads) {
            if(!u.tim)return false;
            const auto& tim=*u.tim;
            const uint32_t w=4u*tim.width,h=tim.height;
            std::vector<uint8_t> rgba;rgba.reserve(size_t(w)*h*4);
            for(uint16_t word:tim.image)for(unsigned shift=0;shift<16;shift+=4) {
                const auto color=tim.clut[(word>>shift)&15];
                for(unsigned channel=0;channel<3;++channel)rgba.push_back(uint8_t(((color>>(5*channel))&31)*255/31));
                rgba.push_back(color==0?0:(color&32768)?128:255);
            }
            const auto hash=gt2::Sha1Hex(rgba);
            auto it=images.find(hash);
            if(it==images.end()) {
                try {
                    const auto png=gt2::ReadPngFile((dir/(hash+".png")).string());
                    const bool validSize=(png.width==int(2*w) && png.height==int(2*h)) || (png.width==int(4*w) && png.height==int(4*h));
                    if(!validSize || cursor+uint64_t(png.width)*png.height+11>SceneRenderer::kGeneratedHudBase)
                        throw std::runtime_error("unexpected dimensions or exhausted race texture budget");
                    const uint32_t count=uint32_t(png.width*png.height);
                    std::vector<uint32_t> pixels(count);std::memcpy(pixels.data(),png.rgba.data(),count*4);
#ifdef __EMSCRIPTEN__
                    renderer.UploadExternalTexture(cursor,count,pixels.data());
#else
                    renderer.UploadGeneratedTexture(cursor,png.width,png.height,pixels.data());
#endif
                    it=images.emplace(hash,std::pair{cursor,uint32_t(png.width)|(uint32_t(png.height)<<16)}).first;
                    cursor+=count+11;
                } catch(const std::exception& e) {std::printf("Generated sponsor %s: %s\n",hash.c_str(),e.what());continue;}
            }
            const uint32_t palette=u.clutX|(uint32_t(u.clutY)<<16);
            for(auto& [key,regions]:regions_) {
                if(key.second!=palette)continue;
                const int x=4*(int(u.imageX)-int(key.first&65535)),y=int(u.imageY)-int(key.first>>16);
                if(x>=256 || y>=256 || x+int(w)<=0 || y+int(h)<=0)continue;
                selected.push_back({key.first,palette,0,x,y,w,h,it->second.first,it->second.second,1u<<gt2::generated::Signs});
            }
        }
        // A CLUT or VRAM word can also be shared with unrelated scenery.
        // Overlay just the advertised rectangles on the authored course material.
        for(const auto& r:selected) {
            auto& regions=regions_[{r.page,r.clut}];
            regions.insert(regions.begin(),r);
        }
        std::printf("Generated sponsors: %zu distinct images for %zu race slots\n",images.size(),uploads.size());
        return true;
    }
    bool Empty() const { return regions_.empty(); }
    size_t Apply(std::vector<SceneVertex>& vertices,unsigned mask=gt2::generated::allCategories) const {
        size_t changed=0;
        for (size_t i=0;i+2<vertices.size();i+=3) {
            const auto found=regions_.find({vertices[i].page,vertices[i].clut});
            if(found==regions_.end())continue;
            for (const auto& r:found->second) {
                // Contact-fan interpolation can overshoot a boundary texel centre by 0.06.
                bool match=true;
                for (size_t k=0;k<3;++k) {
                    const auto& v=vertices[i+k];
                    if (!(v.flags&kTextured) || (v.flags&(kExternalTexture|kCarPaint)) || ((v.flags&kSemiTransparent) && !paletteAlpha_) ||
                        v.page!=r.page || v.clut!=r.clut || ((v.flags>>8)&3)!=r.depth ||
                        v.texel[0]<float(r.x)+.40f || v.texel[1]<float(r.y)+.40f ||
                        v.texel[0]>(float(r.x)+float(r.width))-.40f || v.texel[1]>(float(r.y)+float(r.height))-.40f) match=false;
                }
                if (!match) continue;
                if(!(r.category & mask))break;
                for (size_t k=0;k<3;++k) {
                    auto& v=vertices[i+k];
                    v.texel[0]=(v.texel[0]-float(r.x))*float(r.size&65535)/float(r.width);
                    v.texel[1]=(v.texel[1]-float(r.y))*float(r.size>>16)/float(r.height);
                    for (auto& c:v.color) c=(v.flags&kRawTexture)?1.f:std::min(1.f,c*(255.f/128.f));
                    v.page=r.base;v.clut=r.size;
                    v.flags=(v.flags&~(kRawTexture|uint32_t(3<<8)))|kExternalTexture|kModelClampU|kModelClampV;
                    v.flags |= r.sash << 8; // external architectural material variant
#ifndef __EMSCRIPTEN__
                    v.flags|=kGeneratedMipTexture;
#endif
                    if(r.asphalt) v.flags|=kClampTextureRect; // external road: world-space aggregate plus UV lighting mask
                }
                ++changed;break;
            }
        }
        return changed;
    }
};
}
