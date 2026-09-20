// Differential checks of the disc-derived game data (gt2formats/overlay_data.*, course_data.*,
// game/sim/disc_data.*) against the dump of the original's race: the executable / overlay images, the course
// table, the course id hash, the tuning constants derived by 0x8003B7B8 / 0x8003BA64 and the race lists filled
// by 0x80038DA0 - every byte the simulation consumes must come out equal to what the original had in memory.
#include <cstring>
#include <stdexcept>
#include <string>

#include "game/sim/dev_dump_constants.h"
#include "game/sim/disc_data.h"
#include "gt2formats/course_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kCourseTable = 0x801E18E0u;    // .crsinfo loaded whole (entries at + 8)
constexpr uint32_t kCourseIdTable = 0x801E33F0u;  // the boot block's hashes of the crsmap file names
constexpr uint32_t kCourseIdCount = 0x801C93E4u;  // u16
constexpr uint32_t kTroBase = 0x800B4A34u;        // the course's .tro loaded whole
constexpr uint32_t kRaceObject = 0x801C8568u;     // -> the relocated race block of the .tro

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
const uint8_t* At(const uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }

void Row(const char* name, const char* where, size_t cases, size_t mismatches, int& failures) {
    std::printf("%-10s %-11s %zu cases, %zu mismatches  %s\n", name, where, cases, mismatches, mismatches ? "FAIL" : "ok");
    failures += mismatches ? 1 : 0;
}

// Byte ranges of the images that the original never writes (code and read-only tables), compared whole.
size_t CompareRange(const GuestImage& image, const uint8_t* ram, uint32_t begin, uint32_t end) {
    size_t bad = 0;
    for (uint32_t a = begin; a < end; a++) bad += image.Get<uint8_t>(a) != *At(ram, a);
    return bad;
}

} // namespace

int VerifyData(Guest&, const std::vector<uint8_t>& pristine, std::mt19937&, const GtfsVolume* vol, const DiscImage* disc) {
    if (!vol || !disc) return 0;
    int failures = 0;
    const uint8_t* ram = pristine.data();
    sim::dev::DumpRace dump;
    std::string error;
    if (!sim::dev::LoadRaceFromImage(pristine, dump, error)) throw std::runtime_error("data: " + error);

    // ---- the images: the executable's code and tables, the race overlay's code.
    const GuestImage exe = LoadExeImage(*disc);
    const GuestImage ovl = LoadOverlayImage(*disc, kRaceOverlayIndex);
    {
        size_t bad = 0, cases = 0;
        for (auto [begin, end] : {std::pair<uint32_t, uint32_t>{D(0x8005D600u), D(0x8008DFC4u)}, {D(0x8008E020u), D(0x8009113Cu)}}) { // resident code, rodata
            bad += CompareRange(exe, ram, begin, end);
            cases += end - begin;
        }
        for (uint32_t table : {D(0x800923E2u), D(0x800925A4u)}) { bad += CompareRange(exe, ram, table, table + 64); cases += 64; }
        Row("ExeImage", ActiveProfile().exeName, cases, bad, failures); // section bounds translated to the dump's build
    }
    {
        size_t bad = 0, cases = 0;
        for (auto [begin, end] : {std::pair<uint32_t, uint32_t>{0x80010000u, D(0x8002EE98u)}, {D(0x8002FA00u), D(0x80046BACu)}, {D(0x80047000u), D(0x8005A77Cu)}}) {
            bad += CompareRange(ovl, ram, begin, end);
            cases += end - begin;
        }
        // The raw byte tables the derivations read and the plain tables are untouched by the race.
        for (auto [begin, end] : {std::pair<uint32_t, uint32_t>{D(0x80046C94u), D(0x80046CA4u)}, {D(0x80046D7C), D(0x80046DB0u)}, {D(0x80046DC8u), D(0x80046E00u)},
                                  {D(0x80046E20u), D(0x80046EACu)}, {D(0x80046EACu), D(0x80046EF0u)}}) {
            bad += CompareRange(ovl, ram, begin, end);
            cases += end - begin;
        }
        Row("OvlImage", "GT2.OVL[0]", cases, bad, failures);
    }

    // ---- the course table (.crsinfo at 0x801E18E0 with relocated name pointers) and the course id hash.
    const CourseInfoTable info = ParseCourseInfo(vol->Read(".crsinfo"));
    {
        size_t bad = 0;
        const uint32_t count = Get<uint16_t>(ram, D(kCourseTable) + 6);
        if (count != info.entries.size()) bad++;
        for (size_t i = 0; i < info.entries.size() && i < count; i++) {
            const uint32_t e = D(kCourseTable) + 8 + uint32_t(i) * 24;
            const CourseInfoEntry& entry = info.entries[i];
            const uint32_t namePointer = Get<uint32_t>(ram, e);
            const std::string guestName(reinterpret_cast<const char*>(At(ram, namePointer)));
            const bool same = guestName == entry.name && Get<uint32_t>(ram, e + 4) == entry.fileId && Get<uint16_t>(ram, e + 8) == entry.flags &&
                              std::memcmp(At(ram, e + 10), entry.rest.data(), 14) == 0;
            if (!same && bad++ < 3) std::printf("    MISMATCH crsinfo[%zu]: guest \"%s\" id %08X flags %04X, ours \"%s\" id %08X flags %04X\n", i, guestName.c_str(),
                                                Get<uint32_t>(ram, e + 4), Get<uint16_t>(ram, e + 8), entry.name.c_str(), entry.fileId, entry.flags);
        }
        Row("CrsInfo", ".crsinfo", info.entries.size(), bad, failures);
    }
    std::string courseName;
    {
        // The boot block's table = CourseFileId of the base name of every crsmap file, in directory order.
        size_t bad = 0, cases = 0;
        const uint32_t count = Get<uint16_t>(ram, D(kCourseIdCount));
        std::vector<std::string> crsmap;
        for (const GtfsEntry& f : vol->Files())
            if (f.path.rfind("crsmap/", 0) == 0) crsmap.push_back(f.path.substr(7, f.path.find('.') - 7));
        if (crsmap.size() != count) { bad++; std::printf("    MISMATCH course id table: guest has %u ids, the VOL's crsmap directory %zu files\n", count, crsmap.size()); }
        for (size_t i = 0; i < crsmap.size() && i < count; i++) {
            cases++;
            const uint32_t guestId = Get<uint32_t>(ram, D(kCourseIdTable) + uint32_t(i) * 4), ours = CourseFileId(crsmap[i]);
            if (guestId != ours && bad++ < 3) std::printf("    MISMATCH CourseFileId(\"%s\"): guest %08X, ours %08X\n", crsmap[i].c_str(), guestId, ours);
            if (dump.courseIndex < info.entries.size() && info.entries[dump.courseIndex].fileId == ours) courseName = crsmap[i];
        }
        if (courseName.empty() || info.FindByFileName(courseName) < 0) { bad++; std::printf("    MISMATCH the dump's course %u has no crsmap file with its id\n", dump.courseIndex); }
        Row("CourseId", "0x80083004", cases, bad, failures);
    }
    if (courseName.empty()) return failures + 1;
    const bool dirt = info.entries[dump.courseIndex].IsDirt();

    // ---- the tuning constants (0x8003B7B8 / 0x8003BA64 / 0x8001523C on the pristine overlay and the dump's settings).
    {
        const sim::SimConstants ours = sim::LoadSimConstants(ovl, exe, dump.settings, dump.shell, dirt);
        std::vector<std::string> diff = sim::dev::DiffSimConstants(ours, dump.constants);
        for (const std::string& line : sim::dev::DiffShellState(ours, dump.constants)) diff.push_back(line);
        for (size_t i = 0; i < diff.size() && i < 5; i++) std::printf("    MISMATCH %s\n", diff[i].c_str());
        Row("SimConst", "0x8003BA64", 40, diff.size(), failures);
    }

    // ---- the course's .tro: start lines and the race lists (0x80038DA0) against the relocated block in RAM.
    {
        const std::vector<uint8_t> tro = vol->Read("crsobj/" + courseName + ".tro");
        const Track track = ParseTrack(tro);
        const sim::CourseExtras extras = sim::BuildCourseExtras(track, tro);
        const TrackRaceData file = ParseTrackRaceData(tro);
        size_t bad = 0, cases = 0;
        // Plain bytes of the header (the start lines and the grid) as loaded at 0x800B4A34.
        for (uint32_t o = 0x24; o < 0x118; o += 4) { cases++; bad += std::memcmp(tro.data() + o, At(ram, D(kTroBase) + o), 4) != 0; }
        Row("TroHeader", "0x800B4A34", cases, bad, failures);

        const sim::RaceCourseData ours = sim::BuildRaceCourseData(track, extras, file, ovl, dirt);
        const std::vector<std::string> diff = sim::dev::DiffRaceCourseData(ours, dump.course);
        for (size_t i = 0; i < diff.size() && i < 5; i++) std::printf("    MISMATCH %s\n", diff[i].c_str());
        size_t records = 0;
        for (const auto& list : ours.sections) records += list.size();
        Row("RaceData", "0x80038DA0", records + ours.startLineDistances.size() + 2, diff.size(), failures);

        // Whole records (the loader-computed height / chunk / attribute too) against the guest's relocated block.
        const uint32_t object = Get<uint32_t>(ram, D(kRaceObject));
        bad = 0; cases = 0;
        auto fileS32 = [&](size_t offset) { int32_t v; std::memcpy(&v, tro.data() + offset, 4); return v; };
        if (Get<int32_t>(ram, object) != -fileS32(size_t(fileS32(0x20)))) bad++; // the loader negates the block's size word
        for (uint32_t li = 0; li < 7; li++) {
            const uint32_t list = Get<uint32_t>(ram, object + 8 + li * 4);
            if ((list != 0) != file.present[li]) { bad++; continue; }
            if (!list) continue;
            const int32_t count = Get<int32_t>(ram, list);
            if (count != int32_t(ours.sections[li].size())) { bad++; continue; }
            // Recompute the records like BuildRaceCourseData did: the RaceSection copy holds all 0x28 bytes.
            for (int32_t k = 0; k < count; k++) {
                cases++;
                const uint8_t* guest = At(ram, list + 4 + uint32_t(k) * 0x28);
                if (std::memcmp(guest, &ours.sections[li][size_t(k)], 0x28) != 0 && bad++ < 3) {
                    std::printf("    MISMATCH list %u record %d: guest", li, k);
                    for (int b = 0; b < 0x28; b++) std::printf("%s%02X", b % 4 ? "" : " ", guest[b]);
                    std::printf("\n                             ours ");
                    const uint8_t* mine = reinterpret_cast<const uint8_t*>(&ours.sections[li][size_t(k)]);
                    for (int b = 0; b < 0x28; b++) std::printf("%s%02X", b % 4 ? "" : " ", mine[b]);
                    std::printf("\n");
                }
            }
        }
        Row("RaceRecs", "0x801C8568", cases, bad, failures);
    }
    return failures;
}

} // namespace gt2::verify
