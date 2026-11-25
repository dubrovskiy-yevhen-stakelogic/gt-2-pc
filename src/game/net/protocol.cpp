#include "game/net/protocol.h"
#include <stdexcept>

namespace gt2::net {
namespace {
void Put(std::vector<uint8_t>& b, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) b.push_back(uint8_t(v >> (i * 8)));
}
uint64_t Get(std::span<const uint8_t> b, size_t at, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(b[at + i]) << (i * 8);
    return v;
}
bool ValidHeader(const Packet& p) {
    return p.session != 0 && p.cars >= 1 && p.cars <= kSlots &&
        (p.kind == MessageKind::Authority || (p.kind == MessageKind::Input && p.cars == 1 && !p.stateDigest));
}
}
std::vector<uint8_t> EncodePacket(const Packet& p) {
    if (!ValidHeader(p)) throw std::invalid_argument("network packet: invalid header");
    std::vector<uint8_t> b;
    const size_t payload = size_t(p.cars) * kInputBytes + (p.stateDigest ? 8 : 0);
    b.reserve(kHeaderBytes + payload);
    Put(b, 0x504d3247, 4); // G2MP
    Put(b, kProtocolVersion, 2);
    Put(b, uint8_t(p.kind), 1);
    Put(b, p.cars, 1);
    Put(b, p.session, 8);
    Put(b, p.tick, 4);
    Put(b, payload, 2);
    Put(b, p.stateDigest ? 1 : 0, 2);
    for (size_t i = 0; i < p.cars; ++i) {
        const auto& input = p.inputs[i];
        if (!ValidInput(input)) throw std::invalid_argument("network packet: invalid control");
        Put(b, input.flags, 2);
        Put(b, uint16_t(input.steer), 2);
        Put(b, input.brake, 2);
        Put(b, input.throttle, 2);
        Put(b, uint8_t(input.shift), 1);
        Put(b, input.reverse, 1);
        Put(b, input.handbrake, 1);
        Put(b, input.reserved, 1);
    }
    if (p.stateDigest) Put(b, *p.stateDigest, 8);
    return b;
}
bool DecodePacket(std::span<const uint8_t> b, uint64_t expectedSession, Packet& out) {
    if (b.size() < kHeaderBytes || Get(b, 0, 4) != 0x504d3247 || Get(b, 4, 2) != kProtocolVersion || Get(b, 22, 2) > 1) return false;
    Packet p;
    p.kind = MessageKind(b[6]);
    p.cars = b[7];
    p.session = Get(b, 8, 8);
    p.tick = Tick(Get(b, 16, 4));
    const bool digest = Get(b, 22, 2) != 0;
    if (digest) p.stateDigest = 0;
    if (!ValidHeader(p) || p.session != expectedSession) return false;
    const size_t payload = size_t(p.cars) * kInputBytes + (digest ? 8 : 0);
    if (Get(b, 20, 2) != payload || b.size() != kHeaderBytes + payload) return false;
    for (size_t i = 0; i < p.cars; ++i) {
        auto& input = p.inputs[i];
        const size_t at = kHeaderBytes + i * kInputBytes;
        input.flags = uint16_t(Get(b, at, 2));
        input.steer = int16_t(uint16_t(Get(b, at + 2, 2)));
        input.brake = uint16_t(Get(b, at + 4, 2));
        input.throttle = uint16_t(Get(b, at + 6, 2));
        input.shift = int8_t(b[at + 8]);
        input.reverse = b[at + 9];
        input.handbrake = b[at + 10];
        input.reserved = b[at + 11];
        if (!ValidInput(input)) return false;
    }
    if (digest) p.stateDigest = Get(b, kHeaderBytes + size_t(p.cars) * kInputBytes, 8);
    out = p;
    return true;
}
void AuthorityHistory::Append(const Packet& p) {
    if (p.kind != MessageKind::Authority || !ValidHeader(p) || p.tick != next_ || next_ == UINT32_MAX ||
        (!frames_.empty() && (p.session != frames_.back().session || p.cars != frames_.back().cars)))
        throw std::invalid_argument("invalid authority history sequence");
    frames_.push_back(p); ++next_;
    if (frames_.size() > kCapacity) frames_.pop_front();
}
const Packet* AuthorityHistory::Find(Tick tick) const {
    if (frames_.empty() || tick < frames_.front().tick || tick >= next_) return nullptr;
    return &frames_[tick - frames_.front().tick];
}
bool AuthorityHistory::CanResume(Tick tick) const { return tick == next_ || Find(tick) != nullptr; }
} // namespace gt2::net
