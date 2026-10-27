// Shared helpers of the GT4 asset parsers (little-endian reads with bounds checks, byte writer, SHA-256).
// Formats: docs/formats/car4.md, mdls.md, tex1.md.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace gt4fmt {

using Bytes = std::span<const std::uint8_t>;

struct FormatError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

inline void need(Bytes b, std::size_t off, std::size_t n, const char* what) {
    if (off > b.size() || n > b.size() - off) throw FormatError(std::string(what) + " past end of data");
}
inline std::uint8_t rd8(Bytes b, std::size_t o) {
    need(b, o, 1, "u8");
    return b[o];
}
inline std::uint16_t rd16(Bytes b, std::size_t o) {
    need(b, o, 2, "u16");
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
inline std::uint32_t rd32(Bytes b, std::size_t o) {
    need(b, o, 4, "u32");
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
           (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}
inline std::uint64_t rd64(Bytes b, std::size_t o) {
    return static_cast<std::uint64_t>(rd32(b, o)) | (static_cast<std::uint64_t>(rd32(b, o + 4)) << 32);
}
inline float bitsToFloat(std::uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
inline float rdf32(Bytes b, std::size_t o) { return bitsToFloat(rd32(b, o)); }

class ByteWriter {
public:
    std::vector<std::uint8_t> buf;
    void u8(std::uint32_t v) { buf.push_back(static_cast<std::uint8_t>(v)); }
    void u16(std::uint32_t v) {
        u8(v);
        u8(v >> 8);
    }
    void u32(std::uint32_t v) {
        u16(v);
        u16(v >> 16);
    }
    void u64(std::uint64_t v) {
        u32(static_cast<std::uint32_t>(v));
        u32(static_cast<std::uint32_t>(v >> 32));
    }
    void bytes(const void* p, std::size_t n) {
        const auto* c = static_cast<const std::uint8_t*>(p);
        buf.insert(buf.end(), c, c + n);
    }
    void str(const char* s) { bytes(s, std::strlen(s)); }
};

// SHA-256 (FIPS 180-4), used for the canonical-stream digests shared with the Python scouts.
class Sha256 {
public:
    Sha256();
    void update(const void* data, std::size_t n);
    std::array<std::uint8_t, 32> finish();
    static std::string hex(Bytes data);

private:
    void block(const std::uint8_t* p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t len_ = 0;
    std::uint64_t total_ = 0;
};

}  // namespace gt4fmt
