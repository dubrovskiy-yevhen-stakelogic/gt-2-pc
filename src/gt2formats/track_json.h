#pragma once
// The editable course file (docs/formats/track_json.md, format "gt2pc-track"): one JSON per course with everything the
// simulation and the renderer take from the course's .tro (chunks with their road shape, walls, road lookup grid, chunk
// chain and distances, render lists, mirror copies, billboards; the course UV table; scenery models, LOD lists and
// instances; the start grid), the race data inside the .tro (start lines, the seven race lists - AI lines and grid list),
// the trackside replay cameras (.tro header + 0x1C) and the course's .crsinfo entry (name, flags, backdrop), plus what a
// mod adds (external glTF objects, texture pack / course map / backdrop choices). A complete export reads back to the
// identical parsed data (CanonicalCourseBytes); a mod file may name only the blocks it replaces (baseCourse = the disc
// course the other blocks come from). No dependency beyond json.h, the parsers and the glTF reader.
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/course_data.h"
#include "gt2formats/track.h"

namespace gt2 {

class GtfsVolume;
struct GltfMesh;

// ---------------------------------------------------------------- trackside replay cameras (.tro header + 0x1C)

// One record of the camera list (the original's 0x800117C4 / 0x800109FC): u16 flags (bits 0-3 = kind: 0 path looking at
// the car, 1 fixed, 2 onboard, 3 path with zoom), u16 first / last lap, u16 chunk (search seed), s32 start / end course
// distance (16.16 m), then the kind's data (H values, position / angles or an inline path) up to the next record.
struct CourseCameraRecord {
    uint16_t flags = 0, firstLap = 0, lastLap = 0, chunk = 0;
    int32_t start = 0, end = 0;
    std::vector<uint8_t> data; // record + 0x10 .. the record's end, as stored
};

struct CourseCameras {
    bool present = false;       // header + 0x1C != 0
    uint16_t lapModulus = 0;    // list + 2
    std::vector<CourseCameraRecord> records;
    // The list re-laid out for the camera code (camera::ReplayCameraData{bytes, base, list = base}): the list header at
    // the start (u16 count, u16 modulus, u32 record pointers = base + offset), the records after it, 4-byte aligned.
    // Empty when !present. `base` must not be 0 (a list pointer of 0 means "no cameras").
    std::vector<uint8_t> Blob(uint32_t base) const;
};
// The camera list of a .tro (throws on a malformed list).
CourseCameras ParseCourseCameras(std::span<const uint8_t> tro);
// The camera list at pointer `list` of `bytes` whose first byte has the pointer value `base`.
CourseCameras ParseCameraList(std::span<const uint8_t> bytes, uint32_t base, uint32_t list);

// ---------------------------------------------------------------- mod objects

// An external glTF mesh placed in the course by a mod (drawn every frame, no collision). Positions in metres (world
// x, y up, z), angles in degrees (yaw about +Y, then pitch about +X, then roll about +Z), scale multiplies the mesh.
struct CourseObject {
    std::string mesh;                      // glTF 2.0 file relative to the JSON
    double scale = 1.0;
    std::array<double, 3> position{};
    std::array<double, 3> rotation{};      // degrees: yaw, pitch, roll
    bool shadow = false;                   // a soft ground shadow under the bounding box
    // Loaded by ResolveCourseFile.
    std::string meshPath;
    std::shared_ptr<GltfMesh> loaded;
};

// ---------------------------------------------------------------- the file

// Which blocks a file carries (a partial mod file names only some; the rest come from its base course).
struct CourseBlocks {
    bool info = false, start = false, uvTable = false, chunks = false, scenery = false, race = false, cameras = false;
    bool All() const { return info && start && uvTable && chunks && scenery && race && cameras; }
};

struct CourseInfo {
    std::string name;                 // display name (.crsinfo)
    uint16_t flags = 0;               // .crsinfo flags: bit 0 / 1 reflection map (crstim.arc 5 / 4), bit 2 dirt, bit 5 point to point, bit 6 no sponsor boards
    std::string backdrop;             // bgsobj/<name>.bso (+ .bsp); empty = none
    std::array<uint8_t, 12> tail{};   // .crsinfo entry bytes 12..23 (not decoded)
    bool hasEntry = false;            // false: the disc course has no .crsinfo entry
};

struct CourseFile {
    int version = 1;
    std::string course;               // course id (= the file's base name for mods; the .tro base name for exports)
    std::string baseCourse;           // disc course the missing blocks and the disc assets come from (default = course when it is a disc course)
    std::string meshFile;             // informational: the drawable geometry exported as glTF
    std::string texturePack;          // .trp-style TIM pack relative to the JSON; empty = textureCourse's disc pack
    std::string textureCourse;        // disc course whose crsobj/<name>.trp is used (default = baseCourse)
    std::string courseMap;            // disc course whose crsmap/ image is the HUD's course map (default = baseCourse)
    CourseBlocks blocks;
    CourseInfo info;
    struct { bool name = false, flags = false, backdrop = false, tail = false; } infoKeys; // which "info" keys the file names
    Track track;                      // start / uvTable / chunks / scenery blocks
    TrackRaceData race;
    CourseCameras cameras;
    std::vector<CourseObject> objects;
    std::vector<TrackSceneryInstance> addInstances; // "addSceneryInstances": appended to the resolved scenery (mods)
    std::vector<std::string> warnings; // unknown keys and other non-fatal findings of the reader
};

// Everything of a disc course (all blocks present). Throws when the course does not parse.
CourseFile MakeCourseFile(const GtfsVolume& vol, const std::string& course);

void WriteCourseFile(const std::string& path, const CourseFile& file);
// Throws std::runtime_error naming the path and the offending key on malformed input; checks every index against its
// table (ValidateCourse).
CourseFile ReadCourseFile(const std::string& path);

// The consistency rules the .tro parser enforces (index ranges, 9-bit / 10-bit vertex fields, polygon codes per list,
// s16 ranges). Throws std::runtime_error with the first violation.
void ValidateTrack(const Track& track);

// A course file resolved over the disc: missing blocks from the base course, the texture pack bytes, the objects'
// meshes loaded.
struct ResolvedCourse {
    std::string course, baseCourse;
    Track track;
    TrackRaceData race;
    CourseCameras cameras;
    CourseInfo info;
    int baseIndex = -1;                // .crsinfo entry of the base course (-1 none)
    std::vector<uint8_t> texturePack;  // TIM pack (u32 count + TIMs)
    std::string textureSource;         // where the pack came from (printed)
    std::string courseMap;             // disc course of the HUD's map
    std::vector<CourseObject> objects; // with meshes loaded
    std::vector<std::string> notes;
};
ResolvedCourse ResolveCourseFile(const GtfsVolume& vol, const CourseFile& file, const std::string& jsonPath);

// ---------------------------------------------------------------- canonical bytes / diff

// Every field of the parsed data in a fixed order (the round-trip comparison and its SHA-1).
std::vector<uint8_t> CanonicalTrackBytes(const Track& track);
std::vector<uint8_t> CanonicalRaceBytes(const TrackRaceData& race);
std::vector<uint8_t> CanonicalCameraBytes(const CourseCameras& cameras);
std::vector<uint8_t> CanonicalInfoBytes(const CourseInfo& info);

// Human-readable summary of what differs (per block; at most `limit` lines).
std::vector<std::string> DiffCourses(const Track& a, const TrackRaceData& ra, const Track& b, const TrackRaceData& rb, size_t limit = 40);

} // namespace gt2
