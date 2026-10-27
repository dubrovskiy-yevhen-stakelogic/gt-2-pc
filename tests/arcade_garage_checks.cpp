#include "game/arcade/expanded_garage.h"
#include "game/arcade/arcade_setup.h"
#include "game/arcade/arcade_menus.h"
#include "game/arcade/arcade_car_page.h"
#include "game/career/cheats.h"
#include "gt2vfs/gtfs.h"
#include "gt2formats/car_info.h"
#include <cstring>
#include <filesystem>
#include <iostream>
#include <set>
using namespace gt2;
static void Check(bool ok,const char* msg){if(!ok)throw std::runtime_error(msg);}
int main(int argc,char** argv){try {
    if(argc!=3)throw std::runtime_error("disc output-under-work");
    const auto output=std::filesystem::absolute(argv[2]).lexically_normal();
    const auto rel=output.lexically_relative(std::filesystem::absolute("work"));
    if(rel.empty() || *rel.begin()=="..")throw std::runtime_error("output must be under work");
    std::filesystem::create_directories(output);
    DiscImage disc(argv[1]);GtfsVolume vol(disc);
    auto data=career::CareerData::Load(disc,vol); data.strict=false;
    auto initial=std::make_unique<career::GarageBlock>(); initial->currentCar=-1;
    career::TuneSheet sheet{};std::array<uint8_t,0x230> scratch{};
    const auto catalogue=career::CheatCatalogue(data);
    Check(catalogue.size()>100,"catalogue must exercise expanded slots");
    const auto& first=data.cars.At(catalogue.front());
    Check(career::BuyCar(*initial,first.carId,first.paintIds[0],0,data,sheet,{scratch.data()})==1,"initial car");
    initial->cars[0].value=123456; // preserve existing ownership bytes, including tuning/value
    const auto original=*initial;
    const auto all=arcade::AddAllArcadeCars(std::span(reinterpret_cast<const uint8_t*>(initial.get()),sizeof(*initial)),data);
    Check(std::memcmp(initial.get(),&original,sizeof(original))==0,"retail save mutated");
    Check(std::memcmp(all.data()+4,&original.cars[0],sizeof(career::GarageCar))==0,"existing car lost");
    Check(arcade::ExpandedGarageCount(all)==int(catalogue.size()),"missing catalogue cars");
    Check(arcade::AddAllArcadeCars(all,data)==all,"duplicate cheat is not idempotent");
    auto tuned=all;auto& last=arcade::ExpandedGarageCar(tuned,int(catalogue.size()-1));last.value=765432;
    Check(arcade::GarageSummary(tuned).size()==catalogue.size(),"garage menu truncates cars");
    arcade::ArcadeRaceSetup setup;
    auto* selection=setup.menuRegion.data()+arcade::kSelectionAddress-arcade::kMenuRegionAddress;
    selection[arcade::Sel::kMode]=6;
    const int16_t garage=0,index=int16_t(catalogue.size()-1);
    const uint32_t garageFlag=1;
    std::memcpy(selection+arcade::Sel::kGarage,&garage,2);
    std::memcpy(selection+arcade::Sel::kGarageSlot,&index,2);
    std::memcpy(selection+arcade::Sel::kGarageFlag,&garageFlag,4);
    arcade::RebuildGarageEntry(setup,tuned,data);
    uint32_t racedCar;std::memcpy(&racedCar,setup.raceBlock.data()+0x5C,4);
    Check(racedCar==last.modelId,"last expanded car cannot enter race");
    const auto path=(output/"test.arcade-garage").string();arcade::SaveArcadeGarage(path,tuned);
    Check(arcade::ReadArcadeGarage(path)==tuned,"expanded garage reload lost tuning");
    auto corrupt=career::ReadFileBytes(path);corrupt.back()^=1;career::WriteFileBytes(path+".bad",corrupt);
    bool rejected=false;try{(void)arcade::ReadArcadeGarage(path+".bad");}catch(...){rejected=true;}
    Check(rejected,"corrupt save accepted");
    rejected=false;try{(void)arcade::ExpandedGarageCount(std::span(all).first(100));}catch(...){rejected=true;}
    Check(rejected,"truncated garage accepted");
    // Exercise the actual menu cheat, including its lazily loaded Arcade data.
    auto menuAssets=arcade::ArcadeMenuAssets::Load(disc,vol);
    // Inspect emitted glyph bounds for every purchasable record, rather than
    // accepting a screenshot whose first page happens to contain short names.
    size_t shortened=0, textualLogos=0;
    arcade::ArcNameLogo logo;
    const auto fontVram=menuAssets.vram;
    for(const auto catalogueIndex:catalogue) {
        const auto id=data.cars.At(catalogueIndex).carId;
        const auto name=menuAssets.CarName(id);
        Check(!name.empty(),"garage name is blank");
        for(int available:{140,155,170}) {
            const auto& font=menuAssets.FontAt(0x801234D0u);
            const auto fitted=arcade::FitText(font,name,available);
            shortened+=fitted!=name;
            std::vector<HudFontSprite> glyphs;
            font.Text(fitted,0,0,1,glyphs);
            Check(!glyphs.empty(),"garage name has no printable glyphs");
            for(const auto& g:glyphs)Check(g.x>=0 && g.x+g.w<=available,"name overlaps power column");
        }
        logo.Set(menuAssets,menuAssets.vram,-1,id);
        // Logo TIM uploads share VRAM with menu fonts. Width checks alone
        // cannot catch erased glyphs after switching a showroom car.
        MenuOtSlot nameSlot;arcade::TextCtx nameContext;
        nameContext.ot=&nameSlot;nameContext.font=&menuAssets.FontAt(0x801234D0u);nameContext.colour=0x808080;
        arcade::DrawText(nameContext,arcade::FitText(*nameContext.font,name,170),10,30,1);
        std::vector<MenuPrim> namePrimitives;nameSlot.Emit(namePrimitives,0);
        MenuCanvas expectedName,actualName;
        for(const auto& primitive:namePrimitives){expectedName.Draw(fontVram,primitive);actualName.Draw(menuAssets.vram,primitive);}
        Check(expectedName.Rgba()==actualName.Rgba(),"logo upload corrupted garage font");
        if(!logo.textNames[size_t(logo.slot)].empty()) {
            ++textualLogos;
            Check(logo.textNames[size_t(logo.slot)]==name,"missing logo selected another car");
            for(const unsigned char ch:name)if(ch!=' ') {
                std::vector<HudFontSprite> glyphs;logo.textFont->Glyph(ch,0,60,glyphs);
                Check(!glyphs.empty(),"fallback title font is missing a character");
                MenuOtSlot glyphSlot;arcade::TextCtx glyphContext;
                glyphContext.ot=&glyphSlot;glyphContext.font=logo.textFont;glyphContext.colour=0x808080;
                arcade::DrawGlyph(glyphContext,ch,100,60);
                std::vector<MenuPrim> glyphPrimitives;glyphSlot.Emit(glyphPrimitives,0);
                MenuCanvas glyphCanvas;for(const auto& p:glyphPrimitives)glyphCanvas.Draw(menuAssets.vram,p);
                const auto pixels=glyphCanvas.Rgba();size_t visible=0;
                for(size_t k=0;k<pixels.size();k+=4)visible+=pixels[k]||pixels[k+1]||pixels[k+2];
                Check(visible>0,"fallback title glyph has no visible pixels");
            }
            MenuOtSlot slot;logo.Draw(slot,id,256,100,128,false);
            std::vector<MenuPrim> primitives;slot.Emit(primitives,0);
            Check(!primitives.empty(),"fallback title did not render");
            for(const auto& p:primitives)for(int k=0;k<4;++k)
                Check(p.x[k]>=116 && p.x[k]<=396,"fallback title exceeds page width");
        }
    }
    Check(shortened>0,"long-name regression fixture not exercised");
    // A deliberately missing logo must never reuse the previous cached TIM.
    const auto absent=PackCarId("zzzzz");logo.Set(menuAssets,menuAssets.vram,-1,absent);
    Check(logo.textNames[size_t(logo.slot)]==menuAssets.CarName(absent),"stale cached logo");
    std::cout<<"Name layout: "<<catalogue.size()<<" cars, "<<textualLogos<<" textual titles, "<<shortened<<" bounded labels\n";
    auto arcadeData=ArcadeData::Load(vol);auto cars=CarInfoDirectory::Load(vol);
    auto state=career::NewCareer({});
    const auto stateBytes=std::span(reinterpret_cast<const uint8_t*>(&state),sizeof(state));
    arcade::ArcadeMenus menus(menuAssets,arcadeData,cars,stateBytes);
    menus.SetExpandedGaragePath((output/"menu.arcade-garage").string());menus.Reset();
    std::cout<<menus.AddAllCarsToGarage()<<'\n';
    Check(arcade::ExpandedGarageCount(menus.GarageBlock(0))==int(catalogue.size()),"menu cheat missed cars");
    Check(std::equal(stateBytes.begin(),stateBytes.end(),menus.Career().begin()),"menu cheat changed retail career");
    const auto filled=std::vector<uint8_t>(menus.GarageBlock(0).begin(),menus.GarageBlock(0).end());
    (void)menus.AddAllCarsToGarage();
    Check(std::equal(filled.begin(),filled.end(),menus.GarageBlock(0).begin()),"menu cheat duplicated cars");
    std::cout<<"Arcade garage passed: "<<catalogue.size()<<" cars; retail save preserved, repeat, slot > 100, reload and CRC verified\n";
    return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
