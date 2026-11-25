#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace gt2::net {
// A scalar-only archive: no native layouts, padding, pointers or length-driven
// allocation. Aggregate fields must be explicitly listed by the schema.
class StateWriter {
public:
    std::vector<uint8_t> bytes;
    template<class T> requires std::is_integral_v<T>
    void operator()(T value) {
        if constexpr (std::is_same_v<T, bool>) bytes.push_back(value ? 1 : 0);
        else {
            static_assert(sizeof(T) <= 8);
            const auto bits = std::bit_cast<std::make_unsigned_t<T>>(value);
            for (size_t i = 0; i < sizeof(T); ++i) bytes.push_back(uint8_t(bits >> (i * 8)));
        }
    }
    template<class T, size_t N> void operator()(const T (&values)[N]) { for (const auto& value : values) (*this)(value); }
    template<class T, size_t N> void operator()(const std::array<T, N>& values) { for (const auto& value : values) (*this)(value); }
    template<class T> requires (!std::is_integral_v<T>)
    void operator()(const T& value) { Fields(*this, value); }
    template<class... T> void FieldsOf(const T&... values) { ((*this)(values), ...); }
};
class StateReader {
public:
    explicit StateReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool Done() const { return at_ == bytes_.size(); }
    template<class T> requires std::is_integral_v<T>
    void operator()(T& value) {
        if constexpr (std::is_same_v<T, bool>) {
            uint8_t v = 0; (*this)(v);
            if (v > 1) throw std::invalid_argument("state: noncanonical boolean");
            value = v != 0;
        } else {
            static_assert(sizeof(T) <= 8);
            if (sizeof(T) > bytes_.size() - at_) throw std::invalid_argument("state: truncated scalar");
            uint64_t bits = 0;
            for (size_t i = 0; i < sizeof(T); ++i) bits |= uint64_t(bytes_[at_++]) << (8 * i);
            value = std::bit_cast<T>(std::make_unsigned_t<T>(bits));
        }
    }
    template<class T, size_t N> void operator()(T (&values)[N]) { for (auto& value : values) (*this)(value); }
    template<class T, size_t N> void operator()(std::array<T, N>& values) { for (auto& value : values) (*this)(value); }
    template<class T> requires (!std::is_integral_v<T>)
    void operator()(T& value) { Fields(*this, value); }
    template<class... T> void FieldsOf(T&... values) { ((*this)(values), ...); }
private:
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
};
} // namespace gt2::net
