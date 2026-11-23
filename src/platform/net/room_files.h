#pragma once
#include "platform/net/private_transport.h"
#include "game/net/room_protocol.h"
#include <filesystem>
namespace gt2::net {
struct RoomInvitation {
    std::string address;
    uint16_t port = 29600;
    RoomSecurity security;
};
RoomInvitation ReadInvitation(const std::filesystem::path& path);
void WriteInvitation(const std::filesystem::path& path, const RoomInvitation& invitation);
RoomSecurity ReadHostKey(const std::filesystem::path& path);
void WriteHostKey(const std::filesystem::path& path, const RoomSecurity& security);
struct ResumeIdentity { uint64_t session = 0; uint8_t slot = 255; ResumeToken token{}; };
struct SavedResume {
    std::string address;
    uint16_t port = 0;
    std::array<uint8_t, 32> hostPublic{};
    ContentKey content{};
    ResumeIdentity identity;
};
SavedResume ReadResume(const std::filesystem::path& path);
void WriteResume(const std::filesystem::path& path, const SavedResume& value);
} // namespace gt2::net
