#pragma once
// Byte access to guest memory for native ports that work on several objects at once (the per-car orchestration of
// CORE.GT4 US 1.01: car object, spec block, table entries, per-car records behind car[+4], image globals).
// The resolver is supplied by the caller (game runtime: EE RAM; gt4verify: captured pages or random-case regions).
// A request the resolver cannot satisfy marks the view as `missing` and returns a zeroed scratch block, so a port
// can run to the end without branching on every access; callers must treat a missing view as a refused input and
// discard everything the port wrote.
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "game/guest/words.h"

namespace gt4::game {

class GuestBytes {
public:
    using Resolve = uint8_t* (*)(const void* ctx, uint32_t address, uint32_t length);

    GuestBytes(const void* ctx, Resolve resolve) : ctx_(ctx), resolve_(resolve) {}
    GuestBytes(const GuestBytes&) = delete;
    GuestBytes& operator=(const GuestBytes&) = delete;

    // Host pointer to [address, address + length).
    uint8_t* At(uint32_t address, uint32_t length) const {
        if (uint8_t* p = resolve_(ctx_, address, length)) return p;
        missing_ = true;
        spare_.emplace_back(static_cast<size_t>(length), uint8_t{0});
        return spare_.back().data();
    }
    uint32_t U32(uint32_t address) const {
        uint32_t v;
        std::memcpy(&v, At(address, 4), 4);
        return v;
    }
    void SetU32(uint32_t address, uint32_t v) const { std::memcpy(At(address, 4), &v, 4); }
    uint8_t U8(uint32_t address) const { return *At(address, 1); }
    void SetU8(uint32_t address, uint8_t v) const { *At(address, 1) = v; }
    uint16_t U16(uint32_t address) const {
        uint16_t v;
        std::memcpy(&v, At(address, 2), 2);
        return v;
    }
    void SetU16(uint32_t address, uint16_t v) const { std::memcpy(At(address, 2), &v, 2); }

    // Word reader view for ports written against GuestWords.
    GuestWords Words() const {
        return GuestWords{this, [](const void* ctx, uint32_t a) { return static_cast<const GuestBytes*>(ctx)->U32(a); }};
    }

    bool missing() const { return missing_; }

private:
    const void* ctx_;
    Resolve resolve_;
    mutable bool missing_ = false;
    mutable std::deque<std::vector<uint8_t>> spare_;
};

} // namespace gt4::game
