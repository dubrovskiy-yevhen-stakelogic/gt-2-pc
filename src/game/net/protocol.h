#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "game/net/race_session.h"

namespace gt2::net {
// Wire version is independent of the product release. Fixed-width little-endian
// fields; no pointers, host structs, padding, floating point or platform handles.
constexpr uint16_t kProtocolVersion = 2;
constexpr size_t kHeaderBytes = 24;
constexpr size_t kInputBytes = 12;
enum class MessageKind : uint8_t { Input = 1, Authority = 2 };
struct Packet {
    MessageKind kind = MessageKind::Input;
    uint64_t session = 0;
    Tick tick = 0;
    uint8_t cars = 1;
    Inputs inputs{};
    std::optional<uint64_t> stateDigest; // canonical state AFTER this authority tick
};
std::vector<uint8_t> EncodePacket(const Packet& packet);
// Unauthenticated decoding only. Callers must authenticate the connection and
// bind its owner before InputInbox::Submit. Failure leaves `out` unchanged.
bool DecodePacket(std::span<const uint8_t> bytes, uint64_t expectedSession, Packet& out);
// A bounded authoritative input log for resuming a live client's existing state.
class AuthorityHistory {
public:
    static constexpr Tick kCapacity = 900;
    void Append(const Packet& packet);
    const Packet* Find(Tick tick) const;
    bool CanResume(Tick nextTick) const;
    Tick NextTick() const { return next_; }
private:
    Tick next_ = 0;
    std::deque<Packet> frames_;
};
} // namespace gt2::net
