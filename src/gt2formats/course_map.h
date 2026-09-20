#pragma once
// The arcade course pictures: `arcade/course_mapinfo` + `arcade/course_map` of the US Arcade v1.1 disc (EXE SCUS_944.55
// SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95; ARCADE addresses). The COURSE SELECTION of the arcade menus (member 2,
// 0x80017AA8) inflates the course's picture with the EXE's 0x80083C1C into a 4-bit TIM (188 x 200 texels, CLUT 16 x 1) and
// uploads it to page (384, 256) (CLUT row 256, image rows 257..456); at the race load it sits at page (448, 256) the same
// way (CLUT (448, 256), image (448, 257)), where the arcade Time Trial's post-race menu (race overlay, Simulation 0x8004C5C8)
// draws it as a 188 x 200 sprite (tpage 0x37, CLUT 0x401C, uv (0, 1)). Evidence: gt2run watch of the inflated TIM (writer pc
// 0x80083C4C / 0x80083CF0, ra 0x80017AB0), our objdump of 0x80083C1C, the VRAM of gt2play --prims captures (work/play/
// arcade_menu/cap/course_4150, work/play/mode6/cap/c1_*: all 200 rows and the CLUT equal to the inflated picture).
//
// course_mapinfo: u32 count (78), u32 0, then count entries of 16 bytes {u32 packed size, u32 sector (x 2048 into
// course_map), u32 name offset (into this file), u32 flags (0x10 reverse, 0x08 two-player)}, the names (NUL-terminated
// course file names, "tahiti_t") behind them.
// course_map: per entry at sector * 2048 a GT-ZIP block: "@(#)GT-ZIP" + 2 zero bytes, u32 unpacked size (the TIM's, 18864),
// then the LZ stream 0x80083C1C reads (see InflateGtZip).
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace gt2 {

class GtfsVolume;

struct CourseMapEntry {
    std::string name;
    uint32_t packedSize = 0, sector = 0, flags = 0;
};
std::vector<CourseMapEntry> ParseCourseMapInfo(std::span<const uint8_t> info);

// EXE 0x80083C1C(dst, src, n): LSB-first flag bytes (one per 8 items); flag 0 = a literal byte, flag 1 = a match of 2 or 3
// bytes {length - 3, distance - 1 (7 bits; bit 7 set: 15 bits, the low byte follows)} copied from the output (overlapping
// copies repeat), the last match cut to the n bytes asked for. `src` = the block + 0x10.
std::vector<uint8_t> InflateGtZip(std::span<const uint8_t> src, size_t unpacked);

// The course's picture, the TIM's CLUT (16 colours) and 4-bit image (words x rows).
struct CoursePicture {
    std::vector<uint16_t> clut;   // 16 entries
    uint16_t words = 0, rows = 0; // 47 x 200
    std::vector<uint16_t> image;  // words * rows
};
// Reads the picture of `courseFile` (the .crsinfo / course list file name); throws when the course has none.
CoursePicture LoadCoursePicture(const GtfsVolume& vol, const std::string& courseFile);

} // namespace gt2
