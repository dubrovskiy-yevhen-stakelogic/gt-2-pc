#include "gt2formats/course_data.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gt2 {
namespace {

struct Reader {
    std::span<const uint8_t> d;
    void Need(size_t off, size_t n, const char* what) const {
        if (off > d.size() || n > d.size() - off) throw std::runtime_error(std::string(what) + ": read out of bounds at 0x" + std::to_string(off));
    }
    uint16_t U16(size_t o, const char* what) const { Need(o, 2, what); return uint16_t(d[o] | (d[o + 1] << 8)); }
    uint32_t U32(size_t o, const char* what) const {
        Need(o, 4, what);
        return uint32_t(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (uint32_t(d[o + 3]) << 24));
    }
    int32_t S32(size_t o, const char* what) const { return int32_t(U32(o, what)); }
};

} // namespace

TrackRaceData ParseTrackRaceData(std::span<const uint8_t> tro) {
    static constexpr const char* kWhat = "track race data";
    const Reader r{tro};
    if (tro.size() < 0x1A0 || std::memcmp(tro.data(), "@(#)GT-PS", 9) != 0) throw std::runtime_error("track race data: bad magic");
    TrackRaceData data;
    const int32_t startLines = r.S32(0x24, kWhat);
    if (startLines < 0 || startLines > 12) throw std::runtime_error("track race data: bad start line count");
    for (int32_t i = 0; i < startLines; i++) data.startLineDistances.push_back(r.S32(0x28 + size_t(i) * 4, kWhat));

    const size_t block = r.U32(0x20, kWhat);
    const int32_t size = r.S32(block, kWhat);
    data.listCount = r.U32(block + 4, kWhat);
    if (size <= 0 || size_t(size) > tro.size() - block) throw std::runtime_error("track race data: bad block size");
    if (data.listCount > 7) throw std::runtime_error("track race data: more than 7 lists");
    for (uint32_t li = 0; li < data.listCount; li++) {
        const uint32_t offset = r.U32(block + 8 + li * 4, kWhat);
        if (offset == 0) continue;
        const size_t list = block + offset;
        const int32_t count = r.S32(list, kWhat);
        if (count < 0 || count > 4096) throw std::runtime_error("track race data: bad list count");
        r.Need(list + 4, size_t(count) * sizeof(TrackRaceRecord), kWhat);
        data.present[li] = true;
        data.lists[li].resize(size_t(count));
        if (count) std::memcpy(data.lists[li].data(), tro.data() + list + 4, size_t(count) * sizeof(TrackRaceRecord));
    }
    return data;
}

uint32_t CourseFileId(std::string_view baseName) { // 0x80083004
    uint32_t h = 0;
    for (const char c : baseName) h = ((h << 6) | (h >> 26)) + uint32_t(uint8_t(c));
    return h;
}

int CourseInfoTable::FindByFileName(std::string_view courseFileName) const {
    const uint32_t id = CourseFileId(courseFileName);
    for (size_t i = 0; i < entries.size(); i++)
        if (entries[i].fileId == id) return int(i);
    return -1;
}

CourseInfoTable ParseCourseInfo(std::span<const uint8_t> data) {
    static constexpr const char* kWhat = "crsinfo";
    const Reader r{data};
    if (data.size() < 8 || std::memcmp(data.data(), "CRS\0", 4) != 0) throw std::runtime_error("crsinfo: bad magic");
    if (r.U16(4, kWhat) != 2) throw std::runtime_error("crsinfo: unexpected version");
    const uint32_t count = r.U16(6, kWhat);
    CourseInfoTable table;
    for (uint32_t i = 0; i < count; i++) {
        const size_t e = 8 + size_t(i) * 24;
        CourseInfoEntry entry;
        const size_t nameOffset = r.U32(e, kWhat);
        r.Need(nameOffset, 1, kWhat);
        const size_t nameEnd = std::find(data.begin() + std::ptrdiff_t(nameOffset), data.end(), uint8_t(0)) - data.begin();
        entry.name.assign(reinterpret_cast<const char*>(data.data() + nameOffset), nameEnd - nameOffset);
        entry.fileId = r.U32(e + 4, kWhat);
        entry.flags = r.U16(e + 8, kWhat);
        r.Need(e + 10, 14, kWhat);
        std::memcpy(entry.rest.data(), data.data() + e + 10, 14);
        table.entries.push_back(std::move(entry));
    }
    return table;
}

} // namespace gt2
