#include "gt2vfs/inflate.h"

#include <array>
#include <stdexcept>

namespace gt2 {
namespace {

struct BitReader {
    std::span<const uint8_t> in;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;

    uint32_t Bits(int need) {
        uint32_t val = bitBuf;
        while (bitCount < need) {
            if (pos >= in.size()) throw std::runtime_error("inflate: unexpected end of input");
            val |= static_cast<uint32_t>(in[pos++]) << bitCount;
            bitCount += 8;
        }
        bitBuf = need == 32 ? 0 : val >> need;
        bitCount -= need;
        return need == 32 ? val : val & ((1u << need) - 1);
    }
};

constexpr int kMaxBits = 15;

struct Huffman {
    std::array<uint16_t, kMaxBits + 1> count{};
    std::array<uint16_t, 288> symbol{};

    void Build(const uint8_t* lengths, int n) {
        count.fill(0);
        for (int i = 0; i < n; i++) count[lengths[i]]++;
        int left = 1;
        for (int len = 1; len <= kMaxBits; len++) {
            left <<= 1;
            left -= count[len];
            if (left < 0) throw std::runtime_error("inflate: over-subscribed code");
        }
        std::array<uint16_t, kMaxBits + 1> offs{};
        for (int len = 1; len < kMaxBits; len++) offs[len + 1] = static_cast<uint16_t>(offs[len] + count[len]);
        for (int i = 0; i < n; i++)
            if (lengths[i] != 0) symbol[offs[lengths[i]]++] = static_cast<uint16_t>(i);
    }

    int Decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; len++) {
            code |= static_cast<int>(br.Bits(1));
            int c = count[len];
            if (code - c < first) return symbol[index + (code - first)];
            index += c;
            first += c;
            first <<= 1;
            code <<= 1;
        }
        throw std::runtime_error("inflate: invalid code");
    }
};

constexpr uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                   35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                   3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                    8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void InflateCodes(BitReader& br, std::vector<uint8_t>& out, const Huffman& lit, const Huffman& dist) {
    for (;;) {
        int sym = lit.Decode(br);
        if (sym < 256) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == 256) {
            return;
        } else {
            sym -= 257;
            if (sym >= 29) throw std::runtime_error("inflate: bad length symbol");
            size_t len = kLenBase[sym] + br.Bits(kLenExtra[sym]);
            int ds = dist.Decode(br);
            if (ds >= 30) throw std::runtime_error("inflate: bad distance symbol");
            size_t d = kDistBase[ds] + br.Bits(kDistExtra[ds]);
            if (d > out.size()) throw std::runtime_error("inflate: distance too far back");
            size_t from = out.size() - d;
            for (size_t i = 0; i < len; i++) out.push_back(out[from + i]);
        }
    }
}

} // namespace

std::vector<uint8_t> Inflate(std::span<const uint8_t> deflate, size_t* consumed) {
    BitReader br{deflate};
    std::vector<uint8_t> out;
    out.reserve(deflate.size() * 3);

    bool last = false;
    while (!last) {
        last = br.Bits(1) != 0;
        uint32_t type = br.Bits(2);
        if (type == 0) {
            br.bitBuf = 0;
            br.bitCount = 0;
            if (br.pos + 4 > deflate.size()) throw std::runtime_error("inflate: truncated stored block");
            uint16_t len = static_cast<uint16_t>(deflate[br.pos] | (deflate[br.pos + 1] << 8));
            uint16_t nlen = static_cast<uint16_t>(deflate[br.pos + 2] | (deflate[br.pos + 3] << 8));
            br.pos += 4;
            if (len != static_cast<uint16_t>(~nlen)) throw std::runtime_error("inflate: stored length mismatch");
            if (br.pos + len > deflate.size()) throw std::runtime_error("inflate: truncated stored block");
            out.insert(out.end(), deflate.begin() + br.pos, deflate.begin() + br.pos + len);
            br.pos += len;
        } else if (type == 1) {
            uint8_t lengths[288];
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            Huffman lit, dist;
            lit.Build(lengths, 288);
            uint8_t dl[30];
            for (auto& v : dl) v = 5;
            dist.Build(dl, 30);
            InflateCodes(br, out, lit, dist);
        } else if (type == 2) {
            int nlen = static_cast<int>(br.Bits(5)) + 257;
            int ndist = static_cast<int>(br.Bits(5)) + 1;
            int ncode = static_cast<int>(br.Bits(4)) + 4;
            if (nlen > 286 || ndist > 30) throw std::runtime_error("inflate: bad counts");
            static constexpr uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            uint8_t lengths[320] = {};
            for (int i = 0; i < ncode; i++) lengths[order[i]] = static_cast<uint8_t>(br.Bits(3));
            Huffman lenCode;
            lenCode.Build(lengths, 19);
            int index = 0;
            uint8_t all[320] = {};
            while (index < nlen + ndist) {
                int sym = lenCode.Decode(br);
                if (sym < 16) {
                    all[index++] = static_cast<uint8_t>(sym);
                } else {
                    uint8_t prev = 0;
                    int rep;
                    if (sym == 16) {
                        if (index == 0) throw std::runtime_error("inflate: repeat without previous length");
                        prev = all[index - 1];
                        rep = 3 + static_cast<int>(br.Bits(2));
                    } else if (sym == 17) {
                        rep = 3 + static_cast<int>(br.Bits(3));
                    } else {
                        rep = 11 + static_cast<int>(br.Bits(7));
                    }
                    if (index + rep > nlen + ndist) throw std::runtime_error("inflate: too many lengths");
                    while (rep--) all[index++] = prev;
                }
            }
            Huffman lit, dist;
            lit.Build(all, nlen);
            dist.Build(all + nlen, ndist);
            InflateCodes(br, out, lit, dist);
        } else {
            throw std::runtime_error("inflate: invalid block type");
        }
    }
    if (consumed) *consumed = br.pos;
    return out;
}

uint32_t Crc32(std::span<const uint8_t> data, uint32_t crc) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (uint8_t b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

bool IsGzip(std::span<const uint8_t> data) {
    return data.size() >= 18 && data[0] == 0x1F && data[1] == 0x8B && data[2] == 8;
}

std::vector<uint8_t> Gunzip(std::span<const uint8_t> gz) {
    if (!IsGzip(gz)) throw std::runtime_error("gunzip: not a gzip stream");
    uint8_t flags = gz[3];
    size_t pos = 10;
    if (flags & 0x04) {
        if (pos + 2 > gz.size()) throw std::runtime_error("gunzip: truncated header");
        pos += 2 + (gz[pos] | (gz[pos + 1] << 8));
    }
    for (uint8_t bit : {uint8_t{0x08}, uint8_t{0x10}}) {
        if (flags & bit) {
            while (pos < gz.size() && gz[pos] != 0) pos++;
            pos++;
        }
    }
    if (flags & 0x02) pos += 2;
    if (pos + 8 > gz.size()) throw std::runtime_error("gunzip: truncated header");

    size_t consumed = 0;
    auto out = Inflate(gz.subspan(pos, gz.size() - pos), &consumed);

    // Some VOL entries pad a gzip member to a much larger fixed-size slot. The trailer
    // follows the DEFLATE stream, not the end of that slot. Never skip checksum validation.
    uint32_t crc = Crc32(out);
    uint32_t isize = static_cast<uint32_t>(out.size());
    const size_t t = pos + consumed;
    if (t <= gz.size() && gz.size() - t >= 8) {
        auto u32 = [&](size_t o) {
            return static_cast<uint32_t>(gz[o] | (gz[o + 1] << 8) | (gz[o + 2] << 16) | (static_cast<uint32_t>(gz[o + 3]) << 24));
        };
        if (u32(t) == crc && u32(t + 4) == isize) return out;
    }
    throw std::runtime_error("gunzip: CRC32/ISIZE trailer mismatch");
}

} // namespace gt2
