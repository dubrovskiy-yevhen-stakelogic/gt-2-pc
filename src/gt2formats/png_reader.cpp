#include "gt2formats/png_reader.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "gt2vfs/inflate.h"

namespace gt2 {
namespace {

uint32_t U32Be(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }

int Paeth(int a, int b, int c) {
    const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return pb <= pc ? b : c;
}

} // namespace

PngImage DecodePng(std::span<const uint8_t> file) {
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (file.size() < 8 || std::memcmp(file.data(), kSignature, 8) != 0) throw std::runtime_error("png: bad signature");

    uint32_t width = 0, height = 0;
    int bitDepth = 0, colorType = 0, interlace = 0;
    bool haveHeader = false;
    std::vector<uint8_t> palette, trns, zlib;
    for (size_t pos = 8; pos + 8 <= file.size();) {
        const uint32_t length = U32Be(&file[pos]);
        if (pos + 12 + size_t(length) > file.size()) throw std::runtime_error("png: truncated chunk");
        const std::span<const uint8_t> data = file.subspan(pos + 8, length);
        const char* type = reinterpret_cast<const char*>(&file[pos + 4]);
        if (std::memcmp(type, "IHDR", 4) == 0) {
            if (length != 13) throw std::runtime_error("png: bad IHDR");
            width = U32Be(&data[0]);
            height = U32Be(&data[4]);
            bitDepth = data[8];
            colorType = data[9];
            interlace = data[12];
            haveHeader = true;
        } else if (std::memcmp(type, "PLTE", 4) == 0) {
            palette.assign(data.begin(), data.end());
        } else if (std::memcmp(type, "tRNS", 4) == 0) {
            trns.assign(data.begin(), data.end());
        } else if (std::memcmp(type, "IDAT", 4) == 0) {
            zlib.insert(zlib.end(), data.begin(), data.end());
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + size_t(length);
    }
    if (!haveHeader) throw std::runtime_error("png: no IHDR");
    if (interlace != 0) throw std::runtime_error("png: interlaced images are not supported");
    if (width == 0 || height == 0 || width > 16384 || height > 16384) throw std::runtime_error("png: unsupported image size");
    int channels;
    switch (colorType) {
    case 0: channels = 1; break; // grey
    case 2: channels = 3; break; // RGB
    case 3: channels = 1; break; // palette
    case 4: channels = 2; break; // grey + alpha
    case 6: channels = 4; break; // RGBA
    default: throw std::runtime_error("png: bad colour type");
    }
    const bool depthOk = colorType == 3 ? (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8)
                         : colorType == 0 ? (bitDepth == 1 || bitDepth == 2 || bitDepth == 4 || bitDepth == 8 || bitDepth == 16)
                                          : (bitDepth == 8 || bitDepth == 16);
    if (!depthOk) throw std::runtime_error("png: bad bit depth");
    if (colorType == 3 && palette.empty()) throw std::runtime_error("png: palette image without PLTE");
    if (zlib.size() < 6) throw std::runtime_error("png: no image data");

    // zlib wrapper: 2-byte header, raw deflate, adler32 (not verified).
    if ((zlib[0] & 0x0F) != 8 || ((zlib[0] << 8) | zlib[1]) % 31 != 0) throw std::runtime_error("png: bad zlib header");
    const std::vector<uint8_t> raw = Inflate(std::span<const uint8_t>(zlib).subspan(2, zlib.size() - 6));

    const size_t bitsPerPixel = size_t(channels) * size_t(bitDepth);
    const size_t stride = (size_t(width) * bitsPerPixel + 7) / 8;
    const size_t bpp = bitsPerPixel < 8 ? 1 : bitsPerPixel / 8; // filter byte distance
    if (raw.size() < (stride + 1) * height) throw std::runtime_error("png: image data too short");

    std::vector<uint8_t> scan(stride * height);
    std::vector<uint8_t> previous(stride, 0);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t filter = raw[y * (stride + 1)];
        const uint8_t* src = &raw[y * (stride + 1) + 1];
        uint8_t* dst = &scan[y * stride];
        for (size_t x = 0; x < stride; x++) {
            const int a = x >= bpp ? dst[x - bpp] : 0, b = previous[x], c = x >= bpp ? previous[x - bpp] : 0;
            int v;
            switch (filter) {
            case 0: v = src[x]; break;
            case 1: v = src[x] + a; break;
            case 2: v = src[x] + b; break;
            case 3: v = src[x] + ((a + b) >> 1); break;
            case 4: v = src[x] + Paeth(a, b, c); break;
            default: throw std::runtime_error("png: bad filter type");
            }
            dst[x] = uint8_t(v);
        }
        std::memcpy(previous.data(), dst, stride);
    }

    PngImage image;
    image.width = int(width);
    image.height = int(height);
    image.rgba.resize(size_t(width) * height * 4);
    // Sample fetch: value scaled to 8 bits (16-bit samples keep the high byte; sub-byte samples are stretched).
    auto sample = [&](const uint8_t* row, size_t index) -> int {
        if (bitDepth == 8) return row[index];
        if (bitDepth == 16) return row[index * 2];
        const size_t bit = index * size_t(bitDepth);
        const int v = (row[bit / 8] >> (8 - bitDepth - int(bit % 8))) & ((1 << bitDepth) - 1);
        return colorType == 3 ? v : v * 255 / ((1 << bitDepth) - 1);
    };
    // Full-precision sample for the tRNS colour-key comparison.
    auto sampleRaw = [&](const uint8_t* row, size_t index) -> int {
        if (bitDepth == 16) return (row[index * 2] << 8) | row[index * 2 + 1];
        if (bitDepth == 8) return row[index];
        const size_t bit = index * size_t(bitDepth);
        return (row[bit / 8] >> (8 - bitDepth - int(bit % 8))) & ((1 << bitDepth) - 1);
    };
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* row = &scan[y * stride];
        for (uint32_t x = 0; x < width; x++) {
            uint8_t* out = &image.rgba[(size_t(y) * width + x) * 4];
            int r, g, b, a = 255;
            switch (colorType) {
            case 0: {
                r = g = b = sample(row, x);
                if (trns.size() >= 2 && sampleRaw(row, x) == ((trns[0] << 8) | trns[1])) a = 0;
                break;
            }
            case 2: {
                r = sample(row, x * 3);
                g = sample(row, x * 3 + 1);
                b = sample(row, x * 3 + 2);
                if (trns.size() >= 6 && sampleRaw(row, x * 3) == ((trns[0] << 8) | trns[1]) && sampleRaw(row, x * 3 + 1) == ((trns[2] << 8) | trns[3]) &&
                    sampleRaw(row, x * 3 + 2) == ((trns[4] << 8) | trns[5]))
                    a = 0;
                break;
            }
            case 3: {
                const size_t index = size_t(sample(row, x));
                if (index * 3 + 2 >= palette.size()) throw std::runtime_error("png: palette index out of range");
                r = palette[index * 3];
                g = palette[index * 3 + 1];
                b = palette[index * 3 + 2];
                if (index < trns.size()) a = trns[index];
                break;
            }
            case 4: {
                r = g = b = sample(row, x * 2);
                a = sample(row, x * 2 + 1);
                break;
            }
            default: {
                r = sample(row, x * 4);
                g = sample(row, x * 4 + 1);
                b = sample(row, x * 4 + 2);
                a = sample(row, x * 4 + 3);
                break;
            }
            }
            out[0] = uint8_t(r);
            out[1] = uint8_t(g);
            out[2] = uint8_t(b);
            out[3] = uint8_t(a);
        }
    }
    return image;
}

PngImage ReadPngFile(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("png: cannot open " + path);
    std::vector<uint8_t> data;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    try {
        return DecodePng(data);
    } catch (const std::exception& e) {
        throw std::runtime_error(path + ": " + e.what());
    }
}

} // namespace gt2
