// Shared HD picture storage must survive an event-menu -> save-screen -> menu round trip.
#include "offscreen_car_audit.h"
#include "gt2view/menu_view.h"
#include "gt2view/title_view.h"
#include "gt2formats/hd_media.h"
#include "gt2formats/png_reader.h"
#include "gt2export/png_writer.h"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,char** argv) { try {
    if(argc!=2)throw std::runtime_error("hd picture regression output-directory");
    const auto out=std::filesystem::absolute(argv[1]).lexically_normal();
    std::filesystem::create_directories(out/"fixture/hd/images");
    auto picture=[&](const char* name,int w,int h,int seed) {
        std::vector<uint8_t> rgba(size_t(w)*h*4,255);
        for(int y=0;y<h;++y)for(int x=0;x<w;++x) {
            const size_t p=size_t(y*w+x)*4;
            rgba[p]=uint8_t(30+(x/8*37+seed*51)%190);
            rgba[p+1]=uint8_t(25+(y/8*29+seed*17)%190);
            rgba[p+2]=uint8_t(20+((x+y)/8*11+seed*31)%190);
        }
        gt2::WritePngRgba((out/"fixture/hd/images"/name).string(),w,h,rgba);
    };
    picture("gt-background-1.png",256,240,1);
    picture("gt-background-2.png",128,120,2);
    picture("title.png",176,240,3);
    std::ofstream(out/"fixture/hd/profile.txt")<<"synthetic-hd-picture-regression\n";
    gt2::hd::SetRoot((out/"fixture").string(),"synthetic-hd-picture-regression");
    gt2::hd::SetEnabled(true);
    Device device;gt2view::VkContext context(device.existing);
    gt2view::VkSceneRenderer renderer(context,{512,480},VK_FORMAT_R8G8B8A8_UNORM);
    gt2view::MenuView first(renderer),second(renderer);
    gt2view::TitleView title(renderer);
    gt2::MenuVram vram;
    gt2::MenuFrame frame;frame.backgroundId=1;frame.backgroundPrims=1;
    gt2::MenuPrim background;background.kind=gt2::MenuPrim::kTile;background.w=512;background.h=480;
    frame.prims.push_back(background);
    auto menuShot=[&](gt2view::MenuView& view,int id,const char* name) {
        frame.backgroundId=id;std::vector<gt2view::DrawItem> items;
        view.Build(frame,512.f/480.f,items,true);
        renderer.Draw(items,(out/name).string(),0);
        return gt2::ReadPngFile((out/name).string()).rgba;
    };
    first.UploadVram(vram);
    const auto before=menuShot(first,1,"menu-before.png");
    const auto stableRevision=renderer.HdPictureRevision();
    if(menuShot(first,1,"menu-unchanged.png")!=before || renderer.HdPictureRevision()!=stableRevision)
        throw std::runtime_error("unchanged menu reuploaded or changed its HD picture");
    title.UploadVram(vram); // RunSaveGameScreen uploads title assets, including the shared HD picture.
    first.UploadVram(vram); // RunMenuSession restores PS1 VRAM but keeps its existing MenuView.
    const auto afterSave=menuShot(first,1,"menu-after-save.png");
    menuShot(second,2,"other-menu.png");
    const auto afterMenu=menuShot(first,1,"menu-after-other-menu.png");
    gt2::MenuPrim titleSprite;titleSprite.kind=gt2::MenuPrim::kSprite;
    titleSprite.w=352;titleSprite.h=480;titleSprite.tpage=0x86;titleSprite.clut=0x7fd8;
    auto titleShot=[&](const char* name) {
        std::vector<gt2view::DrawItem> items;
        title.Build({titleSprite},352,512.f/480.f,items,true);
        renderer.Draw(items,(out/name).string(),0);
        return gt2::ReadPngFile((out/name).string()).rgba;
    };
    gt2view::TitleView freshTitle(renderer);freshTitle.UploadVram(vram);
    const auto titleBefore=titleShot("title-before.png");
    gt2view::MenuView freshMenu(renderer);
    menuShot(freshMenu,1,"menu-before-title-return.png");
    const auto titleAfter=titleShot("title-after-menu.png");
    const auto stableTitleRevision=renderer.HdPictureRevision();
    if(titleShot("title-unchanged.png")!=titleAfter || renderer.HdPictureRevision()!=stableTitleRevision)
        throw std::runtime_error("unchanged title reuploaded or changed its HD picture");
    auto differences=[](const auto& a,const auto& b){size_t n=0;for(size_t i=0;i<a.size();i+=4)if(!std::equal(a.begin()+i,a.begin()+i+4,b.begin()+i))++n;return n;};
    const size_t saveDiff=differences(before,afterSave),menuDiff=differences(before,afterMenu),titleDiff=differences(titleBefore,titleAfter);
    std::ofstream(out/"report.json")<<"{\"menu_after_save_pixels\":"<<saveDiff<<",\"menu_after_other_menu_pixels\":"<<menuDiff<<",\"title_after_menu_pixels\":"<<titleDiff<<"}\n";
    std::cout<<"Different pixels after save/menu/title: "<<saveDiff<<'/'<<menuDiff<<'/'<<titleDiff<<'\n';
    if(saveDiff || menuDiff || titleDiff)throw std::runtime_error("shared HD picture was not restored");
    std::cout<<"PASS: HD menu and title pictures survive shared-slot replacement\n";
    return 0;
} catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;} }
