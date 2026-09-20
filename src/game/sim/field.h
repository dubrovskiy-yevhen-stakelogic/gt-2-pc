#pragma once
#include <cstdint>
#include <cstring>

// Typed access to a field of an original-layout object by byte offset. The simulation itself uses the named
// members of CarBody / Wheel / Car (car_body.h); this remains for the verification tools (tools/gt2verify), which
// poke randomised values into snapshots of the original's memory at the offsets the original's code uses.
namespace gt2::sim {

template <typename T>
inline T Field(const void* object, uint32_t offset) {
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(object) + offset, sizeof(T));
    return v;
}

template <typename T>
inline void SetField(void* object, uint32_t offset, T value) {
    std::memcpy(static_cast<uint8_t*>(object) + offset, &value, sizeof(T));
}

} // namespace gt2::sim
