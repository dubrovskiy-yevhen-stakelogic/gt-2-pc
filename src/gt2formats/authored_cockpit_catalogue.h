#pragma once
#include "gt2formats/model_set.h"
#include "gt2formats/car_info.h"
#include <set>
namespace gt2 {
// Read installed authored manifests, not a hard-coded model-family whitelist.
inline std::set<uint32_t> AuthoredCockpitCars(){
    const auto root=std::getenv("GT2_AUTHORED_CAR_SET")?std::filesystem::path(std::getenv("GT2_AUTHORED_CAR_SET")):gt4ModelSetRoot/"authored";
    std::set<uint32_t> cars;std::error_code ec;
    for(std::filesystem::directory_iterator it(root,ec),end;!ec&&it!=end;it.increment(ec)){
        if(!it->is_directory(ec)||it->is_symlink(ec))continue;
        const auto id=it->path().filename().string();if(id.size()!=5)continue;
        try{const auto doc=json::ReadFile((it->path()/"manifest.json").string());
            if(doc.StringOr("format","")!="gt2-authored-car"||doc.IntOr("version",0)!=1||doc.StringOr("car","")!=id)continue;
            if(std::filesystem::is_regular_file(ModelSetAsset(it->path(),doc.Require("mesh").AsString())))cars.insert(PackCarId(id));
        }catch(const std::exception&){continue;}
    }return cars;
}
}
