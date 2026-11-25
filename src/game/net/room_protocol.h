#pragma once
#include "game/net/race_session.h"
#include <span>
#include <string>

namespace gt2::net {
using ContentKey = std::array<uint8_t, 32>;
using ResumeToken = std::array<uint8_t, 16>;
enum class RoomMessage : uint8_t { Hello = 1, Welcome, Ready, Status, Start, End, Reject, Resume, Resumed, ReplayAck, StateBegin, StateAck, Emote };
struct RoomPacket {
    RoomMessage kind = RoomMessage::Hello;
    uint8_t cars = 6, humans = 2, slot = 255, connected = 0, ready = 0;
    uint64_t session = 0;
    uint64_t expectedSession = 0; // Hello only: optional pin from the setup query
    Tick tick = 0;
    ContentKey content{};
    std::array<std::string, kSlots> names{};
    uint8_t emote=0, actor=255;
    uint32_t emoteSerial=0;
    ResumeToken token{}; // private slot ticket, never broadcast or logged
};
constexpr size_t kRoomPacketBytes = 88 + 24 * kSlots;
std::vector<uint8_t> EncodeRoomPacket(const RoomPacket& packet);
bool DecodeRoomPacket(std::span<const uint8_t> bytes, RoomPacket& packet);
Roster RoomRoster(uint8_t cars, uint8_t humans);

// Stock-race description, sent before asset loading and normal admission.
// Strings are restricted asset identifiers, never filenames or paths.
struct RoomSetup {
    uint8_t cars = 6, humans = 2, laps = 2;
    uint8_t botDifficulty = 1;
    bool manual = false, countdown = true, arcadeCars = false;
    std::string track;
    std::array<std::string, kSlots> carIds{};
    std::array<uint32_t, kSlots> paints{};
    bool operator==(const RoomSetup&) const = default;
};
enum class SetupMessage : uint8_t { Request = 1, Offer };
struct SetupPacket {
    SetupMessage kind = SetupMessage::Request;
    uint64_t session = 0;
    ContentKey content{};
    RoomSetup setup;
    bool running = false; // request: allow querying active room; offer: race has started
    bool operator==(const SetupPacket&) const = default;
};
constexpr size_t kSetupPacketBytes = 192;
bool ValidRoomSetup(const RoomSetup& setup);
std::vector<uint8_t> EncodeSetupPacket(const SetupPacket& packet);
bool DecodeSetupPacket(std::span<const uint8_t> bytes, SetupPacket& packet);
} // namespace gt2::net
