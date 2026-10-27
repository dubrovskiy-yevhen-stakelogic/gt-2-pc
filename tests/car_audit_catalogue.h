#pragma once
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/car_info.h"
#include "gt2formats/model_set.h"
#include <set>
inline std::vector<std::string> AuditCarCatalogue(const gt2::GtfsVolume& vol,int argc,char** argv) {
    if(argc>4)return {argv+4,argv+argc};
    std::set<std::string> ids;
    const auto info=gt2::CarInfoDirectory::Load(vol);
    // Exclude deleted slots and unnamed menu objects (logos and trophies).
    // They have no selectable car name and no occupied driving compartment.
    for(size_t i=0;i<info.Count();++i)if(info.At(i).RegionMask()!=15 && !info.At(i).name.empty())
        ids.insert(gt2::UnpackCarId(info.At(i).carId));
    const auto doc=gt2::json::ReadFile((gt2::gt4ModelSetRoot/"manifest.json").string());
    for(const auto& [id,e]:doc.Require("cars").Members()){(void)e;ids.insert(id);}
    return {ids.begin(),ids.end()};
}
