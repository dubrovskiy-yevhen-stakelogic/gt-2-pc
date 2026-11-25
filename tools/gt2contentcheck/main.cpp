#include "network_setup.h"
#include "gt2vfs/gtfs.h"
#include "gt2formats/exe_profile.h"
#include <cstdio>
int main(int argc,char** argv){try{
    if(argc!=2)throw std::runtime_error("installed Arcade directory required");
    gt2::DiscImage disc(argv[1]);gt2::SetActiveProfile(gt2::ProfileOf(disc));
    gt2::GtfsVolume installed(disc),physics(disc,gt2::GtfsVolume::Source::DiscOnly);
    size_t changed=0;for(const auto& file:physics.Files())if(file.path.starts_with("carparam/")&&installed.Read(file)!=physics.Read(file)){
        ++changed;std::printf("installed catalogue changes %s\n",file.path.c_str());}
    gt2game::RaceData data;gt2game::RaceOptions options;
    gt2game::LoadRaceTrack(disc,physics,"seattle",{},data);gt2game::BuildCarRecords(physics,"us36n",6,{},data);
    const auto a=gt2game::NetworkContent(disc,installed,data,6,options,2),b=gt2game::NetworkContent(disc,physics,data,6,options,2);
    std::printf("changed catalogue files: %zu; installed fingerprint %.*s; stock fingerprint %.*s\n",changed,int(a.size()),a.data(),int(b.size()),b.data());
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
