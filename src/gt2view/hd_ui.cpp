#include "gt2view/vk_scene_renderer.h"
#include "gt2formats/hd_media.h"
#include "gt2formats/hd_ui.h"
#include "gt2formats/png_reader.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace gt2view {
void VkSceneRenderer::ApplyHdUi(std::vector<SceneVertex>& vertices) {
    if(hdUiGeneration_ != gt2::hd::Generation()) {
        hdUiGeneration_=gt2::hd::Generation(); hdUiPages_.clear(); hdUiSlots_.clear();
        hdUiAssets_.clear(); hdFontAssets_.clear();
        const auto index=gt2::hd::Asset("ui/index.txt");
        std::ifstream list(index); std::string key;
        std::getline(list,key);
        while(list>>key) if(key.size()==16) hdUiAssets_.insert(key);
        std::ifstream fonts(gt2::hd::Asset("fonts/index.txt"));
        for(const auto& font:gt2::hd::ReadFontIndex(fonts)) hdFontAssets_.insert(font);
        std::printf("HD UI: %zu reconstructed font/palette pairs available\n",hdFontAssets_.size());
        hdUiEnabled_=!hdUiAssets_.empty() || !hdFontAssets_.empty();
        if(std::getenv("GT2_HD_UI_TRACE")) std::printf("HD UI enabled=%d\n",int(hdUiEnabled_));
    }
    if(hdUiEnabled_ || options_.smoothTextures) {
        for(auto& v:vertices)
            if((v.flags & kTextured) && !(v.flags & (kOverlay|kExternalTexture|kHandTexture))) v.flags |= kSmoothUi;
    }
    if(!hdUiEnabled_ || vramShadow_.empty()) return;
    ++hdUiFrame_;
    // Protect every page referenced by this batch before selecting an eviction victim.
    for(const auto& v:vertices) {
        auto it=hdUiPages_.find(uint64_t(v.page | (((v.flags>>8)&1u)<<31)) | (uint64_t(v.clut)<<32));
        if(it!=hdUiPages_.end() && it->second >= 0) hdUiSlots_[size_t(it->second)].used=hdUiFrame_;
    }
    for(auto& v:vertices) {
        if(!(v.flags & kTextured) || (v.flags & (kOverlay|kExternalTexture|kHdUi|0x200u))) continue;
        const uint32_t depth=(v.flags>>8)&1u;
        const uint32_t x=v.page&65535, y=v.page>>16;
        if(x+(64u<<depth)>1024 || y+256>kVramRows) continue;
        auto [it,added]=hdUiPages_.try_emplace(uint64_t(v.page | (depth<<31)) | (uint64_t(v.clut)<<32),-1);
        if(added || it->second == -2) {
            it->second=-1;
            auto key=gt2::hd::UiPageKey(vramShadow_.data()+size_t(y)*1024+x,1024,depth);
            char suffix[8]; const uint32_t clut=((v.clut&65535u)/16u)|(((v.clut>>16)&511u)<<6);
            std::snprintf(suffix,sizeof(suffix),"-%04x",clut);
            const bool reconstructed=depth==0 && hdFontAssets_.contains(key+suffix);
            if(reconstructed) key+=suffix;
            for(size_t i=0;i<hdUiSlots_.size();++i) if(hdUiSlots_[i].key==key) {it->second=int(i);break;}
            if(it->second<0) {
                const auto path=reconstructed ? gt2::hd::Asset("fonts/"+key+".png") : hdUiAssets_.contains(key) ? gt2::hd::Asset("ui/"+key+".png") : std::string();
                if(path.empty() && std::getenv("GT2_HD_UI_TRACE")) std::printf("HD UI missing: %s page=%u,%u depth=%u\n",key.c_str(),x,y,depth);
                if(!path.empty()) try {
                    const auto png=gt2::ReadPngFile(path);
                    if(png.width!=1024 || png.height!=1024) continue;
                    size_t slot=hdUiSlots_.size();
                    if(slot>=kHdUiSlots) {
                        slot=0;
                        for(size_t i=1;i<hdUiSlots_.size();++i) if(hdUiSlots_[i].used<hdUiSlots_[slot].used) slot=i;
                        if(hdUiSlots_[slot].used==hdUiFrame_) continue;
                    }
                    std::vector<uint32_t> packed(1024*1024/(reconstructed?2:4));
                    bool valid=true;
                    for(size_t i=0;i<1024*1024;++i) {
                        const auto c=png.rgba[i*4];
                        if(reconstructed) {
                            const auto b=png.rgba[i*4+1],w=png.rgba[i*4+2];
                            if(c>15 || b>15) {valid=false;break;}
                            packed[i/2] |= (uint32_t(c)|uint32_t(b)<<4|uint32_t(w)<<8)<<((i%2)*16); continue;
                        }
                        if((!depth && c>15) || png.rgba[i*4+1]!=c || png.rgba[i*4+2]!=c) {valid=false;break;}
                        packed[i/4] |= uint32_t(c) << ((i%4)*8);
                    }
                    if(!valid) continue;
                    UploadExternalTexture(kHdUiTexelBase+uint32_t(slot)*kHdUiPageTexels,uint32_t(packed.size()),packed.data());
                    if(slot==hdUiSlots_.size()) hdUiSlots_.push_back({key,hdUiFrame_,reconstructed});
                    else {
                        for(auto& [page,old]:hdUiPages_) { (void)page; if(old==int(slot)) old=-2; }
                        hdUiSlots_[slot]={key,hdUiFrame_,reconstructed};
                    }
                    it->second=int(slot);
                    std::printf("HD UI: page %s loaded\n",key.c_str());
                } catch(const std::exception& e) {std::printf("HD UI: %s\n",e.what());}
            }
        }
        if(it->second<0) continue;
        hdUiSlots_[size_t(it->second)].used=hdUiFrame_;
        if(hdUiSlots_[size_t(it->second)].reconstructed) v.flags|=kReconstructedUi;
        v.page=kHdUiTexelBase+uint32_t(it->second)*kHdUiPageTexels;
        v.texel[0]*=4; v.texel[1]*=4; v.flags|=kHdUi;
    }
}
}
