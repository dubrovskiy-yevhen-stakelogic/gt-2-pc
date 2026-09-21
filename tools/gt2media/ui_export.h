#pragma once
#include "gt2formats/hd_ui.h"
#include "gt2formats/hud_assets.h"
#include <set>

inline void ExportUiMaps(const gt2::GtfsVolume& vol, const std::filesystem::path& out, std::set<std::string>& keys) {
    namespace fs=std::filesystem;
    fs::create_directories(out);
    size_t count=0;
    for(const auto& e:vol.Files()) if(e.path.starts_with("crsmap/")) {
        auto name=fs::path(e.path).filename();
        if(name.extension()==".gz") name=name.stem();
        if(name.extension()!=".tim") continue;
        const auto map=gt2::LoadCourseMap(vol,name.stem().string());
        std::vector<uint16_t> vram(1024*256);
        for(int y=0;y<96;++y) std::copy_n(map.words.data()+y*24,24,vram.data()+(144+y)*1024+576);
        const auto* words=vram.data()+576;
        const auto key=gt2::hd::UiPageKey(words);
        auto page=gt2::hd::EnlargeUiPage(words);
        const auto region=gt2::hd::EnlargeUiRegion(words,0,144,96);
        for(int y=0;y<384;++y) std::copy_n(region.data()+y*384,384,page.data()+(576+y)*1024);
        std::vector<uint8_t> rgba(page.size()*4,255);
        for(size_t i=0;i<page.size();++i) rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=page[i];
        gt2::WritePngRgbaCompressed((out/(key+".png")).string(),1024,1024,rgba);
        keys.insert(key); ++count;
    }
    std::cout<<"Prepared "<<count<<" course maps with isolated sprite boundaries\n";
}

inline void ExportUi(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::filesystem::path& out) {
    namespace fs=std::filesystem;
    fs::create_directories(out);
    std::set<std::string> keys;
    const auto save = [&](const std::vector<uint16_t>& vram, int page, uint32_t depth=0) {
        if((page%16)*64+(64u<<depth)>1024) return;
        const auto* words=vram.data()+(page/16)*256*1024+(page%16)*64;
        bool any=false;
        for(int y=0;y<256 && !any;++y) for(int x=0;x<int(64u<<depth);++x) if(words[y*1024+x]) {any=true;break;}
        if(!any) return;
        const auto key=gt2::hd::UiPageKey(words,1024,depth);
        if(!keys.insert(key).second) return;
        const auto indices=gt2::hd::EnlargeUiPage(words,depth);
        std::vector<uint8_t> rgba(indices.size()*4,255);
        for(size_t i=0;i<indices.size();++i) rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=indices[i];
        gt2::WritePngRgbaCompressed((out/(key+".png")).string(),1024,1024,rgba);
    };
    const auto& profile=gt2::ProfileOf(disc);
    const auto title=profile.arcade?gt2::TitleAssets::LoadArcade(disc,vol):gt2::TitleAssets::Load(disc,vol);
    for(int p=0;p<32;++p) for(uint32_t d=0;d<2;++d) save(title.vram.Words(),p,d);
    if(profile.arcade) {
        gt2::MenuVram v;
        v.UploadTimToPage(vol.Read("arcade/arc_panels_us.tim"),6);
        v.UploadTimToPage(vol.Read("arcade/arc_maker.tim"),0x0b);
        v.UploadTimToPage(vol.Read("arcade/arc_font.tim"),0x1e);
        v.UploadTimToPage(vol.Read("arcade/arc_other.tim"),0x0f);
        v.UploadTimToPage(vol.Read("arcade/arc_goodies_us.tim"),0x1c);
        for(int p=0;p<32;++p) for(uint32_t d=0;d<2;++d) save(v.Words(),p,d);
    }
    if(!profile.arcade) {
        const auto assets=gt2::MenuAssets::Load(disc,vol);
        gt2::MenuVram v;
        v.UploadTimToPage(assets.cursorTim,9); v.UploadTimToPage(assets.itemsTim,0x18); v.UploadTimToPage(assets.fontTim,10);
        v.Upload(704,0,64,256,assets.iconImage);
        for(int p=0;p<32;++p) for(uint32_t d=0;d<2;++d) save(v.Words(),p,d);
        const auto pages=gt2::MenuPages::Load(vol);
        for(uint32_t id=0;id<pages.Count();++id) {
            gt2::MenuVram composed;
            gt2::ComposeMenuVram(assets,pages.Page(id),composed);
            for(int p:{9,26,27}) save(composed.Words(),p);
        }
    }
    std::vector<uint16_t> v(1024*512);
    const auto upload=[&](int x,int y,int w,int h,const uint16_t* data) {
        for(int row=0;row<h;++row) std::copy_n(data+row*w,w,v.data()+(y+row)*1024+x);
    };
    const auto font=gt2::LoadRaceFont(vol,gt2::LoadExeImage(disc));
    const auto sheet=gt2::LoadHudSheet(vol);
    upload(384,0,128,256,font.words.data()); save(v,6);save(v,7);
    upload(512,0,64,234,sheet.sheet.data()); save(v,8);
    const auto tables=gt2::LoadHudTables(gt2::LoadOverlayImage(disc,0));
    for(const auto& dial:sheet.dials) {
        upload(512,0,64,234,sheet.sheet.data());
        upload(512,0,20,80,dial.words.data()); upload(512,228,16,1,dial.clut.data()); save(v,8);
        // PC split-screen combines two independent rev-limit dial faces.
        for(const auto& second:sheet.dials) {
            upload(512+tables.faces[1].u/4,tables.faces[1].v,20,80,second.words.data()); save(v,8);
        }
    }
    ExportUiMaps(vol,out,keys);
    std::ofstream index(out/"index.txt");index<<"indexed-scale4x-v2\n";
    for(const auto& key:keys) index<<key<<'\n';
    std::cout<<"Prepared "<<keys.size()<<" UI pages (4x edge scaling, original palettes)\n";
}

