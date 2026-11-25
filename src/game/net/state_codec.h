#pragma once
#include "game/net/room_protocol.h"
#include <optional>

namespace gt2::net {
// Trusted-host recovery only. A matching compatibility key is mandatory. This
// schema does not deserialize native checkpoints or assets supplied by a peer.
class StateCodec {
public:
    static constexpr uint16_t kVersion = 1;
    static constexpr size_t kMaxBytes = 32 * 1024;
    static std::vector<uint8_t> Encode(const RaceSession& session, const ContentKey& content);
    static uint64_t Digest(const RaceSession& session, const ContentKey& content, const RaceSession::State& state);
    static void Restore(RaceSession& session, const ContentKey& expectedContent, std::span<const uint8_t> bytes,
        std::optional<Tick> expectedTick = {});
private:
    static std::vector<uint8_t> EncodeState(const RaceSession& session, const ContentKey& content, const RaceSession::State& state);
};
struct RoomState { Tick nextTick = 0; std::vector<uint8_t> bytes; };
std::vector<std::vector<uint8_t>> EncodeStateChunks(uint64_t session, Tick tick, std::span<const uint8_t> bytes);
class StateAssembler {
public:
    StateAssembler(uint64_t session, Tick tick) : session_(session), tick_(tick) {}
    void Feed(std::span<const uint8_t> bytes); // ordered reliable control lane; rejects without partial append
    bool Complete() const { return total_ && bytes_.size() == total_; }
    RoomState Take();
private:
    uint64_t session_;
    Tick tick_;
    uint32_t total_ = 0;
    std::vector<uint8_t> bytes_;
};
} // namespace gt2::net
