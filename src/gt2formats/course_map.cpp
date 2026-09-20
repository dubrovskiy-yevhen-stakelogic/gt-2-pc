#include "gt2formats/course_map.h"

#include <cstring>
#include <stdexcept>

#include "gt2vfs/gtfs.h"

namespace gt2 {

namespace {
uint32_t U32(std::span<const uint8_t> b, size_t o) {
    if (o + 4 > b.size()) throw std::runtime_error("course map: short data");
    uint32_t v;
    std::memcpy(&v, b.data() + o, 4);
    return v;
}
uint16_t U16(std::span<const uint8_t> b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("course map: short data");
    return uint16_t(b[o] | b[o + 1] << 8);
}
} // namespace

std::vector<CourseMapEntry> ParseCourseMapInfo(std::span<const uint8_t> info) {
    const uint32_t count = U32(info, 0);
    if (count > 4096) throw std::runtime_error("course_mapinfo: implausible count");
    std::vector<CourseMapEntry> out;
    for (uint32_t i = 0; i < count; i++) {
        const size_t o = 8 + size_t(i) * 16;
        CourseMapEntry e;
        e.packedSize = U32(info, o);
        e.sector = U32(info, o + 4);
        const uint32_t name = U32(info, o + 8);
        e.flags = U32(info, o + 12);
        for (size_t k = name; k < info.size() && info[k] != 0; k++) e.name.push_back(char(info[k]));
        out.push_back(e);
    }
    return out;
}

std::vector<uint8_t> InflateGtZip(std::span<const uint8_t> src, size_t unpacked) { // EXE 0x80083C1C (Arcade)
    std::vector<uint8_t> out;
    out.reserve(unpacked);
    size_t p = 0;
    uint32_t flags = 0;
    int bit = 0;
    int32_t n = int32_t(unpacked);
    auto next = [&]() -> uint8_t {
        if (p >= src.size()) throw std::runtime_error("GT-ZIP: the stream ends early");
        return src[p++];
    };
    while (n > 0) {
        flags >>= 1;
        if (bit == 0) flags = next();
        if ((flags & 1) == 0) {
            out.push_back(next());
            n--;
        } else {
            int32_t length = int32_t(next()) + 3;
            const uint8_t b1 = next();
            int32_t distance = b1 & 0x7F;
            if (b1 & 0x80) distance = (distance << 8) | next();
            distance += 1;
            n -= length;
            if (n < 0) length += n;
            if (size_t(distance) > out.size()) throw std::runtime_error("GT-ZIP: a match before the start of the output");
            for (int32_t k = 0; k < length; k++) out.push_back(out[out.size() - size_t(distance)]);
        }
        bit = (bit + 1) & 7;
    }
    return out;
}

CoursePicture LoadCoursePicture(const GtfsVolume& vol, const std::string& courseFile) {
    const std::vector<uint8_t> info = vol.Read("arcade/course_mapinfo");
    const CourseMapEntry* entry = nullptr;
    const std::vector<CourseMapEntry> entries = ParseCourseMapInfo(info);
    for (const CourseMapEntry& e : entries)
        if (e.name == courseFile) entry = &e;
    if (!entry) throw std::runtime_error("course_mapinfo: no picture of " + courseFile);
    const std::vector<uint8_t> map = vol.Read("arcade/course_map");
    const size_t at = size_t(entry->sector) * 2048;
    if (at + 16 > map.size() || at + entry->packedSize > map.size()) throw std::runtime_error("course_map: the block lies outside the file");
    const std::span<const uint8_t> block(map.data() + at, entry->packedSize);
    if (std::memcmp(block.data(), "@(#)GT-ZIP", 10) != 0) throw std::runtime_error("course_map: not a GT-ZIP block");
    const std::vector<uint8_t> tim = InflateGtZip(block.subspan(16), U32(block, 12));
    // A 4-bit TIM with its CLUT: u32 0x10, u32 8, CLUT block {u32 length, u16 x, y, w, h, data}, image block likewise.
    if (U32(tim, 0) != 0x10 || U32(tim, 4) != 8) throw std::runtime_error("course_map: not a 4-bit TIM with CLUT");
    const uint32_t clutLength = U32(tim, 8);
    if (U16(tim, 16) != 16 || U16(tim, 18) != 1) throw std::runtime_error("course_map: unexpected CLUT size");
    CoursePicture pic;
    for (size_t i = 0; i < 16; i++) pic.clut.push_back(U16(tim, 20 + i * 2));
    const size_t image = 8 + clutLength;
    pic.words = U16(tim, image + 8);
    pic.rows = U16(tim, image + 10);
    if (U32(tim, image) < 12 + size_t(pic.words) * pic.rows * 2) throw std::runtime_error("course_map: short image block");
    for (size_t i = 0; i < size_t(pic.words) * pic.rows; i++) pic.image.push_back(U16(tim, image + 12 + i * 2));
    return pic;
}

} // namespace gt2
