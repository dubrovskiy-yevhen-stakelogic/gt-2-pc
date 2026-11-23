#include "gt2formats/authored_cockpit_catalogue.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2vfs/gtfs.h"
#include <cstdio>
#include <stdexcept>
using namespace gt2;
void Require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("simulation-disc gt4-model-root");
    gt4ModelSetRoot=argv[2];const auto models=AuthoredCockpitCars();Require(models.size()>1,"multiple restored cockpit models required");
    DiscImage disc(argv[1]);GtfsVolume volume(disc);const auto assets=MenuAssets::Load(disc,volume);const auto names=MenuListNames::Load(volume,assets.ovl4);
    MenuPopupList list(MenuListKind::kGarage,assets,names);auto it=models.begin();
    std::vector<MenuGarageRow> rows(4);rows[0].modelId=PackCarId("zzzzz");rows[1].modelId=*it++;rows[2].modelId=PackCarId("yyyyy");rows[3].modelId=*it;
    list.LoadGarage(nullptr,rows,0);list.widget.selection=2;Require(list.ToggleCockpitFilter()&&list.count==2,"filter lost a restored cockpit model");
    for(int n=0;n<90;++n)list.Update(nullptr,true);list.widget.selection=1;MenuListPad pad{};pad.pressed=menu_list_pad::kCross;
    Require(list.Update(&pad,true)==3,"filtered selection chose wrong garage slot");
    Require(list.ToggleCockpitFilter()&&list.count==4&&list.widget.selection==2,"unfilter lost cursor");
    Require(list.garageRows.size()==rows.size()&&list.garageRows[3].modelId==rows[3].modelId,"filter changed garage data");
    rows.resize(1);rows[0].modelId=PackCarId("zzzzz");list.LoadGarage(nullptr,rows,0);
    Require(!list.ToggleCockpitFilter()&&list.count==1,"no-match filter trapped the player");
    std::printf("cockpit filter PASS: %zu installed models; multiple rows, native slot mapping, cursor restoration, no-match\n",models.size());return 0;
}catch(const std::exception& e){std::fprintf(stderr,"cockpit filter: %s\n",e.what());return 1;}}
