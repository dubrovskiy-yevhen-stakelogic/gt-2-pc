#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace gt2 {

// .carinfoa/.carinfoe/.carinfoj ("CAR\0"): car display names and paint chip colours.
// Layout derived from real bytes (US v1.2), see docs/formats/car_info.md:
//   0x00 "CAR\0", 0x04 u16 count, u16 0
//   0x08 count x { u32 packedCarId, u16 entryOffset, u16 flags(unknown) }
//   entry: u16 chipColor[n] (PS1 15-bit), u8 paintId[n], u8 code(unknown), pad 0x7F to even,
//          NUL-terminated name. n is NOT stored - it is the paint count of the car's .cdp.
struct CarInfoEntry {
    std::string name;
    std::vector<uint16_t> chipColors;
    std::vector<uint8_t> paintIds;
    uint8_t code = 0;
    uint16_t flags = 0;
};

// 5 characters, 6 bits each, first character most significant: '-' = 0, '0'-'9' = 1-10, 'a'-'z' = 11-36.
uint32_t PackCarId(const std::string& id);
std::string UnpackCarId(uint32_t packed);

class CarInfo {
public:
    explicit CarInfo(std::vector<uint8_t> data);

    size_t Count() const { return count_; }
    uint32_t PackedIdAt(size_t index) const;

    // Returns nullopt when the car is not listed. Throws when the entry's paint ids do not
    // equal `cdpPaintIds` (the two files must agree).
    std::optional<CarInfoEntry> Lookup(const std::string& carId, std::span<const uint8_t> cdpPaintIds) const;

private:
    std::vector<uint8_t> data_;
    size_t count_ = 0;
};

} // namespace gt2
