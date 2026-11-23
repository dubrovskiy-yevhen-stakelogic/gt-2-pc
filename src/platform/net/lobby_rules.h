#pragma once
#include "platform/net/lobby.h"
#include <algorithm>
namespace gt2::net {
inline unsigned LobbyHumanCount(const LobbyRoom& room) {
    return unsigned(std::count_if(room.members.begin(),room.members.end(),[](const auto& m){return m.occupied;}));
}
inline bool LobbyCanStart(const LobbyRoom& room) {
    return LobbyHumanCount(room)>=2 && room.phase==0 && room.hostSlot<room.options.players && room.members[room.hostSlot].occupied &&
        std::all_of(room.members.begin(),room.members.end(),[](const auto& m){return !m.occupied||m.ready;});
}
inline bool LobbyAllReturned(const LobbyRoom& room,const std::array<bool,6>& returned) {
    for(size_t i=0;i<room.members.size();++i)if(room.members[i].occupied&&!returned[i])return false;
    return true;
}
inline void ResetLobbyReady(LobbyRoom& room) {
    if(++room.revision==0)++room.revision;
    for(auto& member:room.members)member.ready=false;
}
inline uint8_t LobbyRaceSlot(const LobbyRoom& room,uint8_t slot) {
    if(slot>=room.options.players||!room.members[slot].occupied)return 255;
    uint8_t result=0;for(uint8_t i=0;i<slot;++i)if(room.members[i].occupied)++result;
    return result;
}
inline void ElectLobbyHost(LobbyRoom& room) {
    if(room.hostSlot<6&&room.members[room.hostSlot].occupied)return;
    room.hostSlot=255;for(uint8_t i=0;i<room.options.players;++i)if(room.members[i].occupied){room.hostSlot=i;break;}
}
}
