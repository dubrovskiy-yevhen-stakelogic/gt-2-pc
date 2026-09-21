#pragma once
#include "gt2formats/hd_ui.h"
#include "gt2formats/ui_contours.h"
#include <set>

inline std::string FontKey(const std::string& page,uint16_t clut) {
    char tail[8]; std::snprintf(tail,sizeof(tail),"-%04x",unsigned(clut)); return page+tail;
}
inline void ExportFonts(const gt2::DiscImage& disc,const gt2::GtfsVolume& vol,const std::filesystem::path& out) {
    namespace fs=std::filesystem; fs::create_directories(out); std::set<std::string> keys;
    const auto save=[&](const std::vector<uint16_t>& v,int page,uint16_t clut) {
        const int px=(page&15)*64,py=(page&16)*16,cx=(clut&63)*16,cy=clut>>6;
        const auto* palette=v.data()+cy*1024+cx; std::set<uint16_t> colours(palette,palette+16);
        if(colours.size()<2) return;
        const auto key=FontKey(gt2::hd::UiPageKey(v.data()+py*1024+px),clut);
        if(!keys.insert(key).second) return;
        if(fs::is_regular_file(out/(key+".png"))) {
            try {const auto cached=gt2::ReadPngFile((out/(key+".png")).string());if(cached.width==1024 && cached.height==1024) return;}
            catch(const std::exception&) {}
        }
        std::vector<uint8_t> indices(256*256);
        for(int y=0;y<256;++y) for(int x=0;x<256;++x)
            indices[size_t(y*256+x)]=uint8_t((v[(py+y)*1024+px+x/4]>>((x%4)*4))&15);
        std::array<uint16_t,16> coloursArray{};std::copy_n(palette,16,coloursArray.begin());
        gt2::WritePngRgbaCompressed((out/(key+".png")).string(),1024,1024,gt2::hd::SmoothUiContours4x(indices,coloursArray));
    };
    const auto tim=[&](const std::vector<uint16_t>& v,const std::vector<uint8_t>& bytes,int page) {
        const auto u32=[&](size_t at) {return uint32_t(bytes.at(at))|uint32_t(bytes.at(at+1))<<8|uint32_t(bytes.at(at+2))<<16|uint32_t(bytes.at(at+3))<<24;};
        const size_t block=8+((u32(4)&8)?u32(8):0); const int w=int(u32(block+8)&65535),h=int(u32(block+8)>>16);
        for(int y=0;y<h;++y) if(y<2 || y>=h-8) for(int x=0;x<w;x+=16) {
            const auto clut=uint16_t((((page&16)*16+y)<<6)|(((page&15)*64+x)/16));
            for(int offset=0;offset<(w+63)/64;++offset) save(v,page+offset,clut);
        }
    };
    const auto& profile=gt2::ProfileOf(disc);
    auto title=profile.arcade?gt2::TitleAssets::LoadArcade(disc,vol):gt2::TitleAssets::Load(disc,vol);
    for(const auto& entry: {std::pair{"arcade/title_item.tim",12}, {"arcade/arc_font.tim",30}, {"arcade/topmenu_panels_us.tim",14}})
        tim(title.vram.Words(),vol.Read(entry.first),entry.second);
    if(profile.arcade) {
        gt2::MenuVram v;
        for(const auto& entry:{std::pair{"arcade/arc_panels_us.tim",6}, {"arcade/arc_maker.tim",11}, {"arcade/arc_font.tim",30}, {"arcade/arc_other.tim",15}, {"arcade/arc_goodies_us.tim",28}})
            v.UploadTimToPage(vol.Read(entry.first),uint16_t(entry.second));
        for(const auto& entry:{std::pair{"arcade/arc_panels_us.tim",6}, {"arcade/arc_font.tim",30}, {"arcade/arc_other.tim",15}, {"arcade/arc_goodies_us.tim",28}})
            tim(v.Words(),vol.Read(entry.first),entry.second);
    } else {
        const auto assets=gt2::MenuAssets::Load(disc,vol);gt2::MenuVram v;
        v.UploadTimToPage(assets.fontTim,10);v.UploadTimToPage(assets.itemsTim,24);v.UploadTimToPage(assets.cursorTim,9);
        tim(v.Words(),assets.fontTim,10);tim(v.Words(),assets.itemsTim,24);
    }
    std::vector<uint16_t> v(1024*512);
    const auto upload=[&](int x,int y,int w,int h,const uint16_t* data) {for(int row=0;row<h;++row) std::copy_n(data+row*w,w,v.data()+(y+row)*1024+x);};
    // The tightly packed nine-pixel caption font retains its indexed fallback.
    const auto sheet=gt2::LoadHudSheet(vol);const auto tables=gt2::LoadHudTables(gt2::LoadOverlayImage(disc,0));
    std::set<uint16_t> cluts;
    for(const auto& d:tables.speedDigits) cluts.insert(d.clut);
    for(const auto& d:tables.strip) cluts.insert(d.clut);
    for(const auto& d:tables.badges) cluts.insert(d.clut);
    cluts.insert(tables.unitKmh.clut);cluts.insert(tables.unitMph.clut);
    for(const auto& dial:sheet.dials) {
        upload(512,0,64,234,sheet.sheet.data());upload(512,0,20,80,dial.words.data());upload(512,228,16,1,dial.clut.data());
        for(auto clut:cluts) save(v,8,clut);
    }
    std::ofstream index(out/"index.txt",std::ios::binary);index<<"palette-contours4x-v1\n";for(const auto& key:keys) index<<key<<'\n';
    std::cout<<"Prepared "<<keys.size()<<" contour/palette pairs\n";
}
