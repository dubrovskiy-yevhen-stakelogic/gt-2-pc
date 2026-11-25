#pragma once
#include "platform/net/lobby.h"
#include "gt2formats/arcade_menu_data.h"
#include <algorithm>
namespace gt2::net {
inline std::vector<std::string> LobbyClassCars(const ArcadeMenuData& menu,uint8_t cls){
    if(cls>3&&cls!=6)return {};auto cars=menu.classes.at(cls).cars;
    if(cls==6&&cars.size()>4)cars.resize(4); // standard Rally Car entries; bonus cars are another ruleset
    return cars;
}
inline bool LobbyCarAllowed(const ArcadeMenuData& menu,const LobbyOptions& options,const std::string& car){
    if(LobbyTrackDirt(options.track)&&options.carClass!=6)return false;
    const auto cars=LobbyClassCars(menu,options.carClass);
    return std::find(cars.begin(),cars.end(),car)!=cars.end()&&(options.carRule!=0||car==options.fixedCar);
}
inline void SelectLobbyTrack(const ArcadeMenuData& menu,LobbyOptions& options,uint8_t track,LobbyOptions& road) {
    const bool wasDirt=LobbyTrackDirt(options.track),dirt=LobbyTrackDirt(track);
    if(!wasDirt&&dirt)road=options;
    if(wasDirt&&!dirt){options.carClass=road.carClass;options.fixedCar=road.fixedCar;options.carRule=road.carRule;options.laps=road.laps;}
    options.track=track;
    if(dirt)options.carClass=6;
    const auto allowed=LobbyClassCars(menu,options.carClass);
    if(std::find(allowed.begin(),allowed.end(),options.fixedCar)==allowed.end())options.fixedCar=allowed.front();
    if(LobbyTrackPointToPoint(track))options.laps=1;
}

}
