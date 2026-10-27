#include "game/menu/menu_runtime.h"
#include "gt2formats/model_set.h"
#include "gt2formats/car_info.h"
#include "gt2vfs/gtfs.h"
#include "gt2export/png_writer.h"
#include "gt2view/cockpit_badge.h"
#include <iostream>
#include <set>
int main(int argc,char** argv){try{
    if(argc!=4)throw std::runtime_error("disc pack output-under-work");
    const auto out=std::filesystem::absolute(argv[3]).lexically_normal();
    const auto rel=out.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(out);
    for(bool native:{false,true}) {
        std::vector<gt2::MenuPrim> badge;gt2view::AppendCockpitBadge(badge,native,56,208);
        if(!native){if(!badge.empty())throw std::runtime_error("generated interior must have no badge");continue;}
        if(badge.size()<100)throw std::runtime_error("interior badge missing lettering");
        for(const auto& p:badge)if(p.x[0]<48 || p.x[0]+p.w>303 || p.y[0]<180 || p.y[0]+p.h>240)
            throw std::runtime_error("interior badge overlaps car specifications");
    }
    gt2::DiscImage disc(argv[1]);gt2::GtfsVolume vol(disc);
    gt2::gt4ModelSetRoot=argv[2];
    const auto manifest=gt2::json::ReadFile((gt2::gt4ModelSetRoot/"manifest.json").string());
    const auto assets=gt2::MenuAssets::Load(disc,vol);const auto pages=gt2::MenuPages::Load(vol);
    auto data=gt2::menu::MenuData::Load(disc,vol);auto career=gt2::career::NewCareer({});
    gt2::menu::CareerMenuActions actions(career,data);gt2::menu::MenuRuntime runtime(pages,data,actions);
    runtime.Start(0);auto state=runtime.RenderState(assets);state.cursor=false;
    size_t cars=0,native=0,shopItems=0,badgedItems=0;
    for(const auto& [id,entry]:manifest.Require("cars").Members()) {
        const bool expected=entry.BoolOr("nativeInterior",false);++cars;native+=expected;
        if(state.carHasNativeCockpit(gt2::PackCarId(id))!=expected)throw std::runtime_error("badge metadata mismatch: "+id);
    }
    if(state.carHasNativeCockpit(gt2::PackCarId("zzzzz")))throw std::runtime_error("unknown car badge");
    std::set<uint32_t> visited;std::vector<uint32_t> todo{0,0x10000};
    std::ofstream nativePages(out/"shop-native-pages.csv");nativePages<<"page,item,car,target\n";
    bool captured=false;
    while(!todo.empty()) {
        const auto id=todo.back();todo.pop_back();if(!visited.insert(id).second)continue;
        const auto page=pages.Page(id);size_t badges=0;
        for(size_t index=0;index<page.items.size();++index){
            const auto& it=page.items[index];
            if(it.Type()==6){
                ++shopItems;const bool nativeCockpit=state.carHasNativeCockpit(it.CarId());badges+=nativeCockpit;
                if(nativeCockpit)nativePages<<id<<','<<index<<','<<gt2::UnpackCarId(it.CarId())<<','<<it.Target()<<'\n';
            }
            if(it.Has(gt2::menu_item_flag::kAction) && it.Target()<0x80000000)todo.push_back(it.Target());
        }
        auto plain=state;plain.carHasNativeCockpit={};
        const auto without=gt2::BuildMenuFrame(assets,page,plain),with=gt2::BuildMenuFrame(assets,page,state);
        if(with.prims.size()-without.prims.size()!=badges*7)throw std::runtime_error("badge glyph count mismatch on page "+std::to_string(id));
        badgedItems+=badges;
        if(badges && !captured){
            const auto rendered=gt2::RenderMenuPage(assets,page,state);
            gt2::WritePngRgba((out/"shop-cockpit.png").string(),512,480,rendered.canvas.Rgba());captured=true;
        }
    }
    if(!captured || !shopItems)throw std::runtime_error("no native cockpit reached in shops");
    std::ofstream(out/"gate.json")<<"{\"cars\":"<<cars<<",\"native_cockpits\":"<<native<<",\"shop_items\":"<<shopItems<<",\"marked_shop_items\":"<<badgedItems<<"}\n";
    std::cout<<cars<<" metadata rows; "<<shopItems<<" shop entries; "<<badgedItems<<" cockpit badges passed\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
