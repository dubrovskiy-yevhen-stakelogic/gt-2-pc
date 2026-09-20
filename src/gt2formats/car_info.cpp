#include "gt2formats/car_info.h"

#include <cstring>
#include <stdexcept>

namespace gt2 {
namespace {

constexpr size_t kTableOffset = 8;
constexpr size_t kRecordSize = 8;

} // namespace

uint32_t PackCarId(const std::string& id) {
    if (id.size() != 5) throw std::runtime_error("car id must have 5 characters: " + id);
    uint32_t packed = 0;
    for (char c : id) {
        uint32_t v;
        if (c == '-') v = 0;
        else if (c >= '0' && c <= '9') v = uint32_t(c - '0') + 1;
        else if (c >= 'a' && c <= 'z') v = uint32_t(c - 'a') + 11;
        else throw std::runtime_error("car id has an unsupported character: " + id);
        packed = (packed << 6) | v;
    }
    return packed;
}

std::string UnpackCarId(uint32_t packed) {
    std::string id(5, '?');
    for (int i = 4; i >= 0; i--, packed >>= 6) {
        uint32_t v = packed & 0x3F;
        id[size_t(i)] = v == 0 ? '-' : v <= 10 ? char('0' + v - 1) : v <= 36 ? char('a' + v - 11) : '?';
    }
    return id;
}

CarInfo::CarInfo(std::vector<uint8_t> data) : data_(std::move(data)) {
    if (data_.size() < kTableOffset || std::memcmp(data_.data(), "CAR\0", 4) != 0)
        throw std::runtime_error("carinfo: bad magic");
    count_ = size_t(data_[4] | (data_[5] << 8));
    if (kTableOffset + count_ * kRecordSize > data_.size()) throw std::runtime_error("carinfo: table out of bounds");
}

uint32_t CarInfo::PackedIdAt(size_t index) const {
    const uint8_t* r = &data_.at(kTableOffset + index * kRecordSize);
    return uint32_t(r[0] | (r[1] << 8) | (r[2] << 16) | (uint32_t(r[3]) << 24));
}

std::optional<CarInfoEntry> CarInfo::Lookup(const std::string& carId, std::span<const uint8_t> cdpPaintIds) const {
    const uint32_t key = PackCarId(carId);
    for (size_t i = 0; i < count_; i++) {
        if (PackedIdAt(i) != key) continue;
        const uint8_t* r = &data_[kTableOffset + i * kRecordSize];
        size_t pos = size_t(r[4] | (r[5] << 8));
        const size_t n = cdpPaintIds.size();

        CarInfoEntry e;
        e.flags = uint16_t(r[6] | (r[7] << 8));
        if (pos + 3 * n + 2 > data_.size()) throw std::runtime_error("carinfo: entry out of bounds for " + carId);
        for (size_t p = 0; p < n; p++, pos += 2) e.chipColors.push_back(uint16_t(data_[pos] | (data_[pos + 1] << 8)));
        e.paintIds.assign(data_.begin() + pos, data_.begin() + pos + n);
        pos += n;
        if (!std::equal(e.paintIds.begin(), e.paintIds.end(), cdpPaintIds.begin()))
            throw std::runtime_error("carinfo: paint ids disagree with the .cdp for " + carId);
        e.code = data_[pos++];
        if (pos & 1) pos++;
        while (pos < data_.size() && data_[pos] != 0) e.name.push_back(char(data_[pos++]));
        return e;
    }
    return std::nullopt;
}

} // namespace gt2
