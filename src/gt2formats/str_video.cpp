#include "gt2formats/str_video.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>
#include <string>

#include "gt2formats/course_map.h"
#include "gt2formats/exe_profile.h"

namespace gt2 {
namespace {

uint16_t U16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

// Scan position -> raster position of the 8x8 block (the MPEG zigzag).
constexpr uint8_t kZigzag[64] = {0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18, 11, 4,  5,  12, 19, 26, 33, 40, 48,
                                 41, 34, 27, 20, 13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23,
                                 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

int32_t Signed10(uint32_t v) { return int32_t(v << 22) >> 22; }

void Require(bool ok, const std::string& what) {
    if (!ok) throw std::runtime_error("movie: " + what);
}

} // namespace

// ---------------------------------------------------------------- sectors / tables

std::optional<GtVideoSector> ParseGtVideoSector(const uint8_t* raw) {
    const uint8_t submode = raw[18];
    if (submode & 0x04) return std::nullopt; // XA audio
    const uint8_t* h = raw + 24;
    if (U32(h) != kGtVideoMagic) return std::nullopt;
    GtVideoSector s;
    s.chunk = U16(h + 4);
    s.chunkCount = U16(h + 6);
    s.frame = U32(h + 8);
    s.usedBytes = U32(h + 12);
    s.frameCount = U16(h + 16);
    s.flags = U16(h + 18);
    return s;
}

std::vector<StreamMovie> ReadStreamMovieTable(const GuestImage& input) {
    const auto& exe = input;
    Require(exe.profile && exe.profile->arcade,
            "the movie table exists on the Arcade disc only (executable " + exe.fileName + ")");
    std::vector<StreamMovie> movies(kArcadeMovieCount);
    for (int i = 0; i < kArcadeMovieCount; i++) {
        const uint32_t a = exe.Get<uint32_t>(UiAddress(exe, kArcadeMovieTableAddress, true) + uint32_t(i) * 4);
        const uint32_t b = exe.Get<uint32_t>(UiAddress(exe, kArcadeMovieTableAddress, true) + uint32_t(i + 1) * 4);
        Require(b > a + 25, "movie table entry " + std::to_string(i) + " is not increasing");
        movies[size_t(i)] = {a, b - a - 25};
    }
    return movies;
}

std::vector<uint16_t> ReadCourseMovieTable(const GuestImage& input) {
    const auto& arcadeMenus = input;
    std::vector<uint16_t> table;
    for (uint32_t a = UiAddress(arcadeMenus, kCourseMovieTableAddress, true); arcadeMenus.Contains(a, 2); a += 2) table.push_back(arcadeMenus.Get<uint16_t>(a));
    Require(!table.empty(), "the course movie table is outside the arcade menus overlay");
    return table;
}

uint32_t StreamFileLba(const DiscImage& disc) {
    const auto file = disc.FindRootFile("STREAM.DAT");
    Require(file.has_value(), "the disc has no STREAM.DAT (only the Arcade disc carries movies)");
    return file->lba;
}

GtVlcTable ReadGtVlcTable(const GuestImage& input, uint32_t address) {
    const auto& overlay = input;
    address = UiAddress(overlay, address, true);
    GtVlcTable t;
    for (int z = 1; z < GtVlcTable::kZeroCounts; z++) // zero count 0 is never looked up (its slot is code)
        for (int i = 0; i < 64; i++) t.entries[size_t(z * 64 + i)] = overlay.Get<uint32_t>(address + uint32_t(z) * 0x100 + uint32_t(i) * 4);
    return t;
}

// ---------------------------------------------------------------- bitstream (member 5 0x80010AC0)

MdecFrameCodes GtFrameToMdecCodes(const uint8_t* frame, size_t size, const GtVlcTable& table) {
    // Halfwords past the frame's bytes read as 0 (the original reads on into its buffer; they are never consumed).
    auto hw = [&](size_t off) -> uint32_t { return off + 1 < size ? uint32_t(frame[off] | (frame[off + 1] << 8)) : 0u; };
    Require(size >= 10, "frame shorter than its header");
    MdecFrameCodes f;
    const uint32_t words = hw(0);
    f.width = int(hw(4));
    f.height = int(hw(6));
    const uint32_t lzBytes = hw(8);
    Require(f.width > 0 && f.height > 0 && f.width <= 1024 && f.height <= 512, "frame size out of range");
    const int mbCount = ((f.width + 15) >> 4) * ((f.height + 15) >> 4);
    const size_t blocks = size_t(mbCount) * 6;
    const std::vector<uint8_t> dc = InflateGtZip(std::span<const uint8_t>(frame + 10, size - 10), blocks * 2); // 0x80083C1C

    f.codes.reserve(2 + size_t(words) * 2);
    f.codes.push_back(uint16_t(words));
    f.codes.push_back(uint16_t(hw(2)));
    size_t src = 10 + lzBytes;
    uint32_t window = (hw(src) << 16) | hw(src + 2); // a0: the next 32 bits, most significant first
    src += 4;
    int avail = 16;                                  // a1: bits beyond the top 16 still valid in the window
    auto consume = [&](int n) {                      // 0x80010CB8
        avail -= n;
        window <<= (n & 31);
        if (avail <= 0) {
            avail += 16;
            window |= hw(src) << ((16 - avail) & 31);
            src += 2;
        }
    };
    for (size_t b = 0; b < blocks; b++) {
        f.codes.push_back(uint16_t((dc[b] << 8) | dc[blocks + b])); // DC / scale word: high byte array, low byte array
        for (;;) {
            if (int32_t(window) < 0) {
                const uint32_t top3 = window >> 29;
                if (top3 < 6) { // "10": end of block
                    f.codes.push_back(0xFE00);
                    consume(2);
                    break;
                }
                f.codes.push_back(top3 == 6 ? 0x0001 : 0x03FF); // "11s": run 0, level +-1
                consume(3);
                continue;
            }
            int zeros = std::countl_zero(window); // the GTE LZCR of 0x80010BBC (the window is >= 0 here)
            const int zerosIndex = zeros;
            if ((window >> 23) == 0) { // nine or more leading zeros: skip nine first
                consume(9);
                zeros -= 9;
            } else if (zeros == 5) { // "000001": escape, 6-bit run + 10-bit level follow
                consume(6);
                f.codes.push_back(uint16_t(window >> 16));
                consume(16);
                continue;
            }
            const uint32_t next = (window >> ((23 - zeros) & 31)) & 0xFC;
            const size_t index = size_t(zerosIndex) * 64 + next / 4;
            Require(zerosIndex < GtVlcTable::kZeroCounts, "invalid AC code (" + std::to_string(zerosIndex) + " leading zeros)");
            const uint32_t entry = table.entries[index];
            f.codes.push_back(uint16_t(entry));
            Require((entry >> 26) > 0 && (entry >> 26) <= 16, "invalid AC table code length" );
            consume(int(entry >> 26));
        }
    }
    while (f.codes.size() < 2 + size_t(words) * 2) f.codes.push_back(0xFE00);
    return f;
}

// ---------------------------------------------------------------- MDEC

MdecCore::MdecCore() {
    quant_[0].fill(1);
    quant_[1].fill(1);
}

void MdecCore::SetQuantTable(const uint8_t* bytes, bool colour) {
    std::memcpy(quant_[0].data(), bytes, 64);
    if (colour) std::memcpy(quant_[1].data(), bytes + 64, 64);
}

void MdecCore::SetScaleTable(const int16_t* table) { std::memcpy(scale_.data(), table, sizeof(scale_)); }

void MdecCore::Idct(int16_t block[64]) const {
    int32_t src[64], tmp[64];
    for (int i = 0; i < 64; i++) src[i] = block[i];
    int32_t* in = src;
    int32_t* out = tmp;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                int32_t sum = 0;
                for (int z = 0; z < 8; z++) sum += in[y + z * 8] * (int32_t(scale_[size_t(x + z * 8)]) >> 3);
                out[x + y * 8] = (sum + 0xFFF) >> 13;
            }
        std::swap(in, out);
    }
    for (int i = 0; i < 64; i++) block[i] = int16_t(std::clamp(in[i], -32768, 32767));
}

size_t MdecCore::DecodeBlock(const uint16_t* codes, size_t count, size_t pos, int quant, int16_t out[64]) const {
    while (pos < count && codes[pos] == 0xFE00) pos++;
    if (pos >= count) return count + 1;
    int16_t block[64] = {};
    const auto& qt = quant_[size_t(quant)];
    uint32_t n = codes[pos++];
    const int32_t qscale = int32_t((n >> 10) & 0x3F);
    int k = 0;
    int32_t value = Signed10(n & 0x3FF) * qt[0];
    for (;;) {
        if (qscale == 0) value = Signed10(n & 0x3FF) * 2;
        value = std::clamp(value, -0x400, 0x3FF);
        if (qscale > 0) block[kZigzag[k]] = int16_t(value);
        else block[k] = int16_t(value);
        if (pos >= count) return count + 1;
        n = codes[pos++];
        k += int((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        value = (Signed10(n & 0x3FF) * qt[size_t(k)] * qscale + 4) >> 3;
    }
    Idct(block);
    std::memcpy(out, block, sizeof(block));
    return pos;
}

void MdecCore::ColourMacroblock(const int16_t cr[64], const int16_t cb[64], const int16_t y[4][64], const Output& o,
                                std::vector<uint32_t>& words) const {
    uint8_t rgb[16 * 16][3];
    for (int py = 0; py < 16; py++)
        for (int px = 0; px < 16; px++) {
            const int c = (px >> 1) + (py >> 1) * 8;
            const int32_t vr = cr[c], vb = cb[c];
            const int32_t luma = y[(py >> 3) * 2 + (px >> 3)][(px & 7) + (py & 7) * 8];
            const int32_t r = std::clamp(luma + ((359 * vr + 128) >> 8), -128, 127);
            const int32_t g = std::clamp(luma + ((-88 * vb - 183 * vr + 128) >> 8), -128, 127);
            const int32_t b = std::clamp(luma + ((454 * vb + 128) >> 8), -128, 127);
            uint8_t* p = rgb[py * 16 + px];
            p[0] = uint8_t(o.isSigned ? r : r + 128);
            p[1] = uint8_t(o.isSigned ? g : g + 128);
            p[2] = uint8_t(o.isSigned ? b : b + 128);
        }
    if (o.depth == k24Bit) {
        const uint8_t* bytes = &rgb[0][0];
        for (int i = 0; i < 16 * 16 * 3; i += 4) words.push_back(U32(bytes + i));
    } else {
        for (int i = 0; i < 256; i += 2) {
            uint32_t pair = 0;
            for (int k = 0; k < 2; k++) {
                const uint8_t* p = rgb[i + k];
                const uint32_t v = uint32_t(p[0] >> 3) | uint32_t(p[1] >> 3) << 5 | uint32_t(p[2] >> 3) << 10 | (o.bit15 ? 0x8000u : 0u);
                pair |= v << (16 * k);
            }
            words.push_back(pair);
        }
    }
}

void MdecCore::MonoBlock(const int16_t y[64], const Output& o, std::vector<uint32_t>& words) const {
    uint8_t bytes[64];
    for (int i = 0; i < 64; i++) {
        const int32_t v = std::clamp(int32_t(y[i]), -128, 127);
        bytes[i] = uint8_t(o.isSigned ? v : v + 128);
    }
    if (o.depth == k8Bit) {
        for (int i = 0; i < 64; i += 4) words.push_back(U32(bytes + i));
    } else {
        for (int i = 0; i < 64; i += 8) {
            uint32_t w = 0;
            for (int k = 0; k < 8; k++) w |= uint32_t(bytes[i + k] >> 4) << (4 * k);
            words.push_back(w);
        }
    }
}

MdecCore MdecCoreFromExe(const GuestImage& input) {
    const auto& exe = input;
    Require(exe.Get<uint32_t>(UiAddress(exe, kLibpressQuantAddress, true)) == 0x40000001u && exe.Get<uint32_t>(UiAddress(exe, kLibpressScaleAddress, true)) == 0x60000000u,
            "the libpress MDEC tables are not at their Arcade v1.1 addresses in " + exe.fileName);
    MdecCore m;
    m.SetQuantTable(exe.At(UiAddress(exe, kLibpressQuantAddress, true) + 4, 128), true);
    int16_t scale[64];
    std::memcpy(scale, exe.At(UiAddress(exe, kLibpressScaleAddress, true) + 4, 128), 128);
    m.SetScaleTable(scale);
    return m;
}

namespace {

// The words the MDEC hands out for the frame (DecDCTinRaw sends (words >> 5) * 32 parameter words by DMA0).
std::vector<uint32_t> RunMdec(const MdecCore& mdec, const MdecFrameCodes& codes, MdecCore::Depth depth) {
    Require(codes.codes.size() >= 2, "no MDEC codes");
    const size_t sent = size_t(codes.codes[0] >> 5) * 32;
    const size_t count = std::min(codes.codes.size() - 2, sent * 2);
    const uint16_t* c = codes.codes.data() + 2;
    const int mbCount = ((codes.width + 15) >> 4) * ((codes.height + 15) >> 4);
    MdecCore::Output o;
    o.depth = depth;
    std::vector<uint32_t> words;
    words.reserve(size_t(mbCount) * 192);
    size_t pos = 0;
    int16_t cr[64], cb[64], y[4][64];
    for (int m = 0; m < mbCount; m++) {
        if ((pos = mdec.DecodeBlock(c, count, pos, 1, cr)) > count) break;
        if ((pos = mdec.DecodeBlock(c, count, pos, 1, cb)) > count) break;
        bool complete = true;
        for (int b = 0; b < 4 && complete; b++) complete = (pos = mdec.DecodeBlock(c, count, pos, 0, y[b])) <= count;
        if (!complete) break;
        mdec.ColourMacroblock(cr, cb, y, o, words);
    }
    return words;
}

} // namespace

MovieImage DecodeMovieFrame(const MdecCore& mdec, const MdecFrameCodes& codes, MdecCore::Depth depth) {
    Require(depth == MdecCore::k24Bit || depth == MdecCore::k15Bit, "movie frames are colour");
    const std::vector<uint32_t> words = RunMdec(mdec, codes, depth);
    MovieImage img;
    img.width = codes.width;
    img.height = codes.height;
    img.rgb.assign(size_t(img.width) * size_t(img.height) * 3, 0);
    const int mbRows = (codes.height + 15) >> 4;
    const size_t perMb = depth == MdecCore::k24Bit ? 192 : 128;
    for (size_t m = 0; (m + 1) * perMb <= words.size(); m++) {
        const int col = int(m) / mbRows, row = int(m) % mbRows;
        for (int py = 0; py < 16; py++)
            for (int px = 0; px < 16; px++) {
                const int x = col * 16 + px, y = row * 16 + py;
                if (x >= img.width || y >= img.height) continue;
                uint8_t* d = &img.rgb[(size_t(y) * size_t(img.width) + size_t(x)) * 3];
                if (depth == MdecCore::k24Bit) {
                    const size_t byte = size_t(py * 16 + px) * 3;
                    for (int k = 0; k < 3; k++) d[k] = uint8_t(words[m * perMb + (byte + size_t(k)) / 4] >> (8 * ((byte + size_t(k)) % 4)));
                } else {
                    const size_t i = size_t(py * 16 + px);
                    const uint32_t v = (words[m * perMb + i / 2] >> (16 * (i % 2))) & 0xFFFF;
                    d[0] = uint8_t((v & 31) << 3);
                    d[1] = uint8_t(((v >> 5) & 31) << 3);
                    d[2] = uint8_t(((v >> 10) & 31) << 3);
                }
            }
    }
    return img;
}

std::vector<uint16_t> DecodeMovieFrame15(const MdecCore& mdec, const MdecFrameCodes& codes) {
    const std::vector<uint32_t> words = RunMdec(mdec, codes, MdecCore::k15Bit);
    std::vector<uint16_t> out(size_t(codes.width) * size_t(codes.height), 0);
    const int mbRows = (codes.height + 15) >> 4;
    for (size_t m = 0; (m + 1) * 128 <= words.size(); m++) {
        const int col = int(m) / mbRows, row = int(m) % mbRows;
        for (int i = 0; i < 256; i++) {
            const int x = col * 16 + (i & 15), y = row * 16 + (i >> 4);
            if (x < codes.width && y < codes.height) out[size_t(y) * size_t(codes.width) + size_t(x)] = uint16_t(words[m * 128 + size_t(i) / 2] >> (16 * (i & 1)));
        }
    }
    return out;
}

// ---------------------------------------------------------------- reader

GtMovieReader::GtMovieReader(const DiscImage& disc, uint32_t streamLba, const StreamMovie& movie)
    : disc_(disc), lba_(streamLba + movie.first), count_(movie.sectors) {
    Require(lba_ + count_ <= disc.SectorCount(), "movie sectors past the end of the disc");
}

bool GtMovieReader::NextFrame(Frame& frame) {
    while (!AtEnd())
        if (ReadSector(frame)) return true;
    return false;
}

bool GtMovieReader::ReadSector(Frame& frame) {
    uint8_t raw[DiscImage::kRawSectorSize];
    if (!AtEnd()) {
        const uint32_t index = next_++;
        disc_.ReadRawSector(lba_ + index, raw);
        const XaSubheader sub = XaSubheaderOf(raw);
        if (sub.submode & 0x04) { // XA audio: the stream's filter is file 1, channel 7 (0x800100F8)
            if (IsXaAudio(sub) && sub.file == 1 && sub.channel == 7) {
                const XaFormat fmt = XaFormatOf(sub.coding);
                audioRate_ = fmt.sampleRate;
                audioStereo_ = fmt.stereo;
                DecodeXaSector(raw, xa_, audio_);
                audioSectors_++;
            }
            return false;
        }
        const auto s = ParseGtVideoSector(raw);
        if (!s) return false;
        // 0x800106EC: chunk 0 starts a frame; later chunks must follow in order within the same frame.
        if (s->chunk == 0) {
            dropping_ = false;
            buildingFrame_ = s->frame;
            received_ = 0;
            building_.clear();
        } else if (dropping_ || s->frame != buildingFrame_ || s->chunk != received_) {
            dropping_ = true;
            if (s->frame >= s->frameCount) ended_ = true;
            return false;
        }
        received_++;
        building_.insert(building_.end(), raw + 24 + kGtVideoHeaderBytes, raw + 24 + kGtVideoHeaderBytes + kGtVideoChunkBytes);
        if (s->chunk + 1u == s->chunkCount) {
            frame.number = s->frame;
            frame.frameCount = s->frameCount;
            frame.lastSector = index;
            frame.data = std::move(building_);
            building_.clear();
            received_ = 0;
            if (s->frame >= s->frameCount) ended_ = true;
            return true;
        }
    }
    return false;
}

} // namespace gt2
