// Optional owned-disc integration test. No game data is embedded in this test.
#include "game/career/garage.h"
#include "game/menu/menu_runtime.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/json.h"
#include "gt2vfs/gtfs.h"
#include "game/sim/disc_data.h"
#include <cstring>
#include <iostream>
#include <set>
#include <memory>

using namespace gt2;
using namespace gt2::career;
static void Check(bool ok,const std::string& what) { if(!ok)throw std::runtime_error(what); }
static void Put(std::vector<uint8_t>& b,size_t at,uint32_t v,int n) { for(int i=0;i<n;++i)b[at+size_t(i)]=uint8_t(v>>(i*8)); }
static void OffsetCheck() {
    std::vector<uint8_t> bytes(0x12020);
    std::memcpy(bytes.data(),"CAR\0",4);Put(bytes,4,1,2);
    Put(bytes,8,PackCarId("z0001"),4);Put(bytes,12,0x12000,4);
    bytes[0x12002]='0';std::memcpy(bytes.data()+0x12004,"Synthetic expansion",20);
    const std::array<uint8_t,1> paint{'0'};
    Check(CarInfo(bytes).Lookup("z0001",paint)->name=="Synthetic expansion","18-bit car name offset");
    Check(CarInfoDirectory(bytes).At(0).name=="Synthetic expansion","directory/name parsers agree");
}
int main(int argc,char** argv) {
    try {
        OffsetCheck();
        if(argc<2) { std::cout<<"Expanded car-info offset passed\n";return 0; }
        DiscImage disc(argv[1]);GtfsVolume vol(disc);
        auto data=CareerData::Load(disc,vol);data.strict=false;
        const auto source=vol.Read("carparam/gt4_expansion.json");
        const auto meta=json::Parse(std::string_view(reinterpret_cast<const char*>(source.data()),source.size()));
        const auto& cars=meta.Require("cars");
        const auto names=ParseUniStrDb(vol.Read("carparam/usa_unistrdb.dat"));
        const auto colors=CarColorNames::Load(vol);
        CarInfo info(vol.Read(".carinfoa"));
        auto sheet=std::make_unique<TuneSheet>();std::array<uint8_t,0x400> scratch{};
        const bool driveCheck=argc>2 && std::string_view(argv[2])=="--drive";
        const auto tro=vol.Read("crsobj/maxspeed.tro");const auto track=ParseTrack(tro);
        const auto extras=sim::BuildCourseExtras(track,tro);const sim::NativeCourse course(track,extras);
        const auto overlay=LoadOverlayImage(disc,0);
        const auto constants=sim::LoadSimConstants(overlay,data.exe,{},{},false);
        const auto courseData=sim::BuildRaceCourseData(track,extras,ParseTrackRaceData(tro),overlay,false);
        int driveFailures=0;
        std::set<uint32_t> expected;
        for(size_t i=0;i<cars.Size();++i) {
            const auto& c=cars.At(i);const auto id=c.Require("id").AsString();const auto packed=PackCarId(id);expected.insert(packed);
            const auto prefix=id+": ";
            auto cfg=CatalogueCarConfig(data.tables,packed);Check(bool(cfg),prefix+"catalogue config");
            const auto params=BuildCarParams(vol,data.tables,*cfg);
            Check(params.weightKg==c.IntAt("mass"),prefix+"mass from GT4");
            Check(params.height==c.IntAt("height") && params.width==c.IntAt("width"),prefix+"GT4 dimensions");
            Check(params.driveType==c.IntAt("drive"),prefix+"GT4 drive layout");
            Check(params.gearCount==c.Require("gears").Size() && params.gearCount>=1 && params.gearCount<=7,prefix+"gear count");
            Check(params.torquePointCount>=2 && params.torquePointCount<=16,prefix+"engine curve count");
            for(int j=1;j<params.torquePointCount;++j) Check(params.torqueRpm100[j]>params.torqueRpm100[j-1],prefix+"engine curve order");
            const auto model=ParseCarModel(vol.Read("carobj/"+id+".cdo"));Check(!model.lods.empty(),prefix+"body scaffold");
            const auto texture=ParseCarTexture(vol.Read("carobj/"+id+".cdp"));
            Check(texture.paints.size()==1,prefix+"paint count");
            const std::array<uint8_t,1> paint{texture.paints[0].id};
            Check(info.Lookup(id,paint)->name==c.Require("name").AsString(),prefix+"display name");
            const auto color=colors.NameIndices(size_t(data.cars.IndexOf(packed)),1);Check(!colors.Name(color[0]).empty(),prefix+"colour name");
            const auto cat=CarCatalogueAt(data.tables,*FindCatalogueRow(data.tables,packed));
            Check(Utf16ToUtf8(names.at(cat.modelName))==c.Require("name").AsString(),prefix+"shop/garage name");
            CareerSave save;save.header=BuildSaveHeader(data.exe);save.state=NewCareer({});save.state.garage.money=kMoneyLimit;
            const int price=CataloguePrice(data.tables,packed);
            Check(price>0 && price==c.IntAt("price"),prefix+"purchase price");
            Check(BuyCar(save.state.garage,packed,paint[0],price,data,*sheet,BuildScratch{scratch.data()})==1,prefix+"purchase");
            Check(save.state.garage.count==1 && save.state.garage.money==kMoneyLimit-price,prefix+"money and garage");
            const auto loaded=LoadCareerSaveFile(BuildCareerSaveFile(save));
            Check(loaded.CrcOk() && std::memcmp(&loaded.state,&save.state,sizeof(CareerState))==0,prefix+"save reload");
            auto raceCfg=loaded.state.garage.cars[0].config;
            const auto race=BuildCarParams(vol,data.tables,raceCfg);
            Check(race.weightKg==params.weightKg && race.gearCount==params.gearCount,prefix+"saved race configuration");
            auto driving=race;
            if(driving.gearAutoSet) {
                sim::SetupConstants gearConstants;gearConstants.gearAutoTable=data.exe.At(0x800923E2,48);
                sim::GenerateGearRatios(driving,int(driving.gearAutoFinal)*10,false,gearConstants);
            }
            for(int j=1;j<=driving.gearCount;++j)Check(driving.gearRatio[j]>0,prefix+"usable driving ratio");
            Check(driving.finalDrive>0,prefix+"usable final drive");
            if(driveCheck) {
                const auto body=BodyDimensionsOf(model);
                auto slot=sim::SlotFromTrackGrid(track,course,0,sim::CarNoseOffset(body.bboxFront,body.scaleShift));
                slot.entryKind=sim::kEntryPlayer1;slot.controlClass=0;
                sim::RaceShellOptions options;options.countdown=false;
                auto simulation=std::make_unique<sim::RaceSim>();
                simulation->Setup(track,extras,constants,courseData,{race},{slot},options);
                sim::PadRecord pad{};pad.throttle=1;int32_t speed=0;
                for(int frame=0;frame<300;++frame){
                    simulation->Step(&pad);const auto t=simulation->Telemetry(0);speed=std::max(speed,t.forwardSpeed);
                    if(argc>3 && id==argv[3] && frame%30==0)std::cerr<<id<<" frame "<<frame<<" rpm "<<t.rpm<<" gear "<<int(t.gear)<<" throttle "<<t.throttle<<" speed "<<t.forwardSpeed<<" clutch "<<simulation->CarAt(0).body.clutchState<<'\n';
                }
                if(speed<1138){++driveFailures;std::cerr<<prefix<<"no usable acceleration, peak "<<speed<<'\n';}
                std::cout<<"Drive "<<id<<" peak-kmh "<<double(speed)*3.6/4096<<'\n';
            }
            if(i%50==0)std::cout<<"Checked "<<i+1<<" / "<<cars.Size()<<"\n";
        }
        if(argc>2 && std::string_view(argv[2])=="--catalog-only") {
            std::cout<<cars.Size()<<" catalogue, purchase and save round trips passed\n";return 0;
        }
        const auto pages=MenuPages::Load(vol);std::set<uint32_t> visited,found;std::vector<uint32_t> todo{0x10000};
        uint32_t firstDealer=0;
        while(!todo.empty()) {
            const auto id=todo.back();todo.pop_back();if(!visited.insert(id).second)continue;
            const auto p=pages.Page(id);Check(p.items.size()<=64,"showroom item limit");
            for(const auto& it:p.items) {
                if(it.Type()==6) {found.insert(it.CarId());if(!firstDealer)firstDealer=id;}
                if(it.Has(menu_item_flag::kAction) && it.Target()>=0x10000 && it.Target()<0x80000000)todo.push_back(it.Target());
            }
        }
        Check(found==expected,"every new car is reachable through manufacturer pagination");
        auto menuData=menu::MenuData::Load(disc,vol);auto state=NewCareer({});state.garage.money=kMoneyLimit;
        menu::CareerMenuActions actions(state,menuData);menu::MenuRuntime runtime(pages,menuData,actions);
        auto settle=[&]{for(int i=0;i<40;++i)runtime.Update({});};
        runtime.Start(firstDealer);settle();const auto selected=runtime.Page().DefaultItem()->CarId();
        menu::PadState choose{};choose.pressed=menu::pad::kCross;runtime.Update(choose);settle();
        Check(runtime.Car().carId==selected && runtime.CurrentTransaction().carId==selected,"dealer selection previews the purchased car");
        runtime.Update(choose);settle();Check(runtime.PageId()&0x80000000u,"buy enters normal confirmation flow");
        auto transaction=runtime.CurrentTransaction();actions.RunTransaction(transaction);
        Check(state.garage.count==1 && state.garage.cars[0].carId==selected,"normal menu transaction buys expansion car");
        Check(!driveFailures,std::to_string(driveFailures)+" cars failed acceleration");
        Check(CanBuy(state.garage,kMoneyLimit)==-1,"insufficient credits refused");
        state.garage.count=kGarageCapacity;Check(CanBuy(state.garage,0)==-2,"full garage refused");
        std::cout<<cars.Size()<<" purchases, save round trips, race records and reachable shop entries passed\n";
        return 0;
    }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
