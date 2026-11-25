#pragma once
#include "race_common.h"
#include "game/net/room_protocol.h"
namespace gt2game {
void BuildNetworkCarRecords(const gt2::GtfsVolume& volume,const std::vector<std::string>& ids,RaceData& data);
gt2::net::RoomSetup DescribeNetworkSetup(const RaceData& data, size_t cars, const RaceOptions& options, uint8_t humans);
gt2::net::ContentKey NetworkContent(const gt2::DiscImage& disc, const gt2::GtfsVolume& volume,
    const RaceData& data, size_t cars, const RaceOptions& options, uint8_t humans);
} // namespace gt2game
