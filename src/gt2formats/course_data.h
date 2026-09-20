#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace gt2 {

// Course-side race data of GT2 as it is stored on the disc. Facts (US Simulation v1.2, SCUS_944.88 SHA-1
// 3030aa27...), established with tools/gt2run `watch` on the race load and the disassembly of the writers:
//
// 1. `.crsinfo` (GT2.VOL root, 4037 bytes): the course table. Header "CRS\0", u16 version (2), u16 count (126);
//    then `count` entries of 24 bytes: u32 offset of the NUL-terminated display name within the file, u32 course
//    file id (see CourseFileId), u16 flags (bit 2 = dirt course, tested by 0x800418E8 / 0x8003BA64), 14 bytes not
//    decoded. The boot block loads the file whole at 0x801E18E0 (0x80011C70: 0x8005D8A0(6, 0x801E18E0), then
//    turns the name offsets into pointers), and 0x80060E94(index) returns 0x801E18E8 + index * 24.
//
// 2. The course file id: 0x80083004 hashes a name as h = rotl32(h, 6) + c per byte. The boot block (0x80011B70)
//    hashes the base name (up to the first '.') of every file of the VOL's "crsmap" directory into the table
//    0x801E33F0, and 0x80060FB0(id) turns an id back into that file's index; the same base name is the course's
//    "crsobj/<name>.tro". So the course with file name "seattle" is the .crsinfo entry whose id is
//    CourseFileId("seattle") = 0xA2D762AE (entry 86, "Seattle Circuit Full Course").
//
// 3. The race data of a course sits inside its .tro (loaded whole at 0x800B4A34 in the race, i.e. the header
//    fields 0x10..0x20 become pointers): header + 0x24 = s32 count of start lines, + 0x28 = s32[count] course
//    distances (16.16 m) (read by 0x80033384 / 0x8003C70C / 0x8003CF94 through 0x800B4A58); header + 0x20 = offset
//    of the race block: { s32 size; u32 listCount (7); u32 listOffset[listCount] } followed by the lists, each
//    { s32 count; TrackRaceRecord[count] }. The race loader 0x80038DA0 relocates the list offsets, computes the
//    "filled by the loader" fields of every record from the course geometry and negates the size word.

// One entry of a race list (0x28 bytes). Only the fields marked [file] carry data on the disc; the others are
// zero in the file and are produced by the loader (see game/sim/disc_data.h).
#pragma pack(push, 1)
struct TrackRaceRecord {
    int32_t type;        // +0x00  [file] 0 = straight, 1 = corner (the loader rewrites corners: 1 = right, 2 = left)
    int32_t x;           // +0x04  [file] simulation-plane position, 1/4096 m
    int32_t y;           // +0x08  [file]
    int32_t height;      // +0x0C  road height at (x, y), 1/4096 m
    uint16_t chunk;      // +0x10  course chunk containing (x, y)
    int8_t surface;      // +0x12  road surface type under (x, y) (polygon word1 >> 28)
    int8_t attribute;    // +0x13  road attribute 1 under (x, y)
    int32_t distance;    // +0x14  course distance of (x, y), 16.16 m
    int32_t radius;      // +0x18  [file] signed corner radius, 1/4096 m (0 on straights); RaceSection::curvature
    int32_t slopeAcross; // +0x1C  angle of the road across the heading (from two probes 2 m to the sides)
    int32_t slopeAlong;  // +0x20  angle of the road along the heading (from two probes 2 m fore / aft)
    int32_t heading;     // +0x24  [file] angle units
};
#pragma pack(pop)
static_assert(sizeof(TrackRaceRecord) == 0x28);

struct TrackRaceData {
    std::vector<int32_t> startLineDistances;          // header + 0x24 / + 0x28
    std::array<bool, 7> present{};                    // list offset non-zero
    std::array<std::vector<TrackRaceRecord>, 7> lists; // in file order; list 4 is the grid list (0x80038DA0)
    uint32_t listCount = 0;                           // as stored (7 on all but one course of the US v1.2 VOL)
};

// Parses the race data of a .tro (the whole file, as gt2::ParseTrack takes it). Throws std::runtime_error.
TrackRaceData ParseTrackRaceData(std::span<const uint8_t> tro);

// 0x80083004.
uint32_t CourseFileId(std::string_view baseName);

struct CourseInfoEntry {
    std::string name;                 // display name
    uint32_t fileId = 0;              // CourseFileId of the course file's base name
    uint16_t flags = 0;               // bit 2 = dirt course
    std::array<uint8_t, 14> rest{};   // the undecoded tail of the entry (bytes 10..23)
    bool IsDirt() const { return (flags & 4) != 0; }
};

struct CourseInfoTable {
    std::vector<CourseInfoEntry> entries;
    // Index of the first entry with the id of `courseFileName` (e.g. "seattle"), -1 when absent.
    int FindByFileName(std::string_view courseFileName) const;
};

// Parses `.crsinfo`. Throws std::runtime_error.
CourseInfoTable ParseCourseInfo(std::span<const uint8_t> data);

} // namespace gt2
