#pragma once
#include "race_common.h"
namespace gt2game {
int NetworkRaceCheck(const RaceData& data, size_t carCount);
int NetworkStateCheck(const RaceData& data, size_t carCount, const std::string& input = {}, const std::string& output = {});
int NetworkPeerCheck(const RaceData& data, size_t carCount, bool listen, const std::string& address, uint16_t port);
int NetworkRoomCheck(const RaceData& data);
int NetworkPrivateRoomCheck(const RaceData& data);
int NetworkInvitationCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& volume, const std::string& invitation, bool restart);
int NetworkRoomPeerCheck(const RaceData& data, size_t carCount, bool listen, const std::string& address, uint16_t port,
    bool longRecovery = false, bool hostPause = false, unsigned desyncs = 0);
}

namespace gt2game { int NetworkDirectoryCheck(const gt2::DiscImage&,const gt2::GtfsVolume&,const std::string&,const std::string&,unsigned); }
