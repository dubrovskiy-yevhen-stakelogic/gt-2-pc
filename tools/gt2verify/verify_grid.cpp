// Differential checks of the start grid (src/game/sim/race_sim.* PlaceOnGrid / GridSlotOfEntry / CarNoseOffset): the
// per-car race setup 0x80012CD4 runs on the guest up to its call of 0x80033384 and the arguments it passes (the chunk
// hint, the position in the simulation plane, sin / cos of the heading, the control class, the car index, the grid
// offset) are compared with ours.
//   GridArgs: random grids / start angles / entries / body models / game modes on the dump's course, the native side
//             reading the same bytes of the image (the .tro at 0x800B4A34, the model's LOD 0 block);
//   GridDisc: the dump's own race, the native side built like gt2game builds it - the course parsed from the disc's
//             .tro, the body model from the disc's .cdo of each car's configuration (licence tables in game mode 3).
#include <cstring>
#include <stdexcept>

#include "game/sim/race_sim.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_params.h"
#include "gt2formats/course_data.h"
#include "gt2formats/license_data.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kTro = 0x800B4A34u;                        // the course file (+0x54 start angle, +0x58 grid, 12 bytes per slot)
constexpr uint32_t kEntries = 0x801D5944u, kEntryStride = 0xD0; // race entries: +1 grid slot, +2 kind, +3 transmission
constexpr uint32_t kConfigs = 0x801D58C0u;                    // CarConfig of each car slot (0x801D58B8 + 8)
constexpr uint32_t kRecords = 0x801DE8BAu, kRecordStride = 0x1C0; // car records (0x801C98E0 + 0x14FDA)
constexpr uint32_t kModels = 0x800A9504u;                     // the body model object of each car (-> car + 0x878)
constexpr uint32_t kGameMode = 0x801D5866u, kCourseIndex = 0x800AF230u, kCourseTable = 0x801E18E8u;

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }

struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
};

// The arguments of 0x80033384 (a0 body, a1 record, a2 chunk, a3 x, then sp + 0x10..0x30).
struct StartArgs {
    int32_t chunk = 0, x = 0, y = 0, sinH = 0, cosH = 0;
    uint32_t controlClass = 0, transmission = 0, contact = 0, byte1C = 0, carIndex = 0, gridOffset = 0;
    bool operator==(const StartArgs&) const = default;
};

std::string Describe(const StartArgs& a) {
    char text[200];
    std::snprintf(text, sizeof(text), "chunk %d x %d y %d sin %d cos %d class %u gear %u contact %u car %u offset %d", a.chunk, a.x, a.y, a.sinH, a.cosH,
                  a.controlClass, a.transmission, a.contact, a.carIndex, int32_t(a.gridOffset));
    return text;
}

// The original: 0x80012CD4(car, index, record) until it calls 0x80033384.
bool OriginalArgs(Guest& guest, uint32_t car, StartArgs& a) {
    if (!guest.CallUntil(0x80012CD4u, 0x80033384u, kCarBase + car * kCarStride, car, D(kRecords) + car * kRecordStride)) return false;
    const uint8_t* ram = guest.Ram();
    const uint32_t sp = guest.Reg(29);
    a.chunk = int32_t(guest.Reg(6));
    a.x = int32_t(guest.Reg(7));
    a.y = Get<int32_t>(ram, sp + 0x10);
    a.sinH = Get<int32_t>(ram, sp + 0x14);
    a.cosH = Get<int32_t>(ram, sp + 0x18);
    a.controlClass = Get<uint32_t>(ram, sp + 0x1C) & 0xFF;
    a.transmission = Get<uint32_t>(ram, sp + 0x20) & 0xFF;
    a.contact = Get<uint32_t>(ram, sp + 0x24) & 0xFF;
    a.byte1C = Get<uint32_t>(ram, sp + 0x28) & 0xFF;
    a.carIndex = Get<uint32_t>(ram, sp + 0x2C) & 0xFF;
    a.gridOffset = Get<uint32_t>(ram, sp + 0x30);
    return true;
}

// Ours for car `car` of the image: the entry's slot, the grid of the image's .tro, the nose of `noseZ`.
StartArgs NativeArgs(const uint8_t* ram, const Track& track, const sim::NativeCourse& course, uint32_t car, uint16_t courseFlags, int32_t noseZ,
                     const std::array<std::array<int32_t, 3>, 16>& grid, int32_t startAngle) {
    const uint32_t entry = D(kEntries) + car * kEntryStride;
    const uint8_t mode = Get<uint8_t>(ram, D(kGameMode));
    const uint8_t slot = sim::GridSlotOfEntry(Get<uint8_t>(ram, entry + 1), mode, courseFlags);
    sim::GridPlacement g;
    g.pole = grid[0];
    g.position = grid[slot & 15];
    g.startAngle = startAngle;
    g.noseZ = noseZ;
    const sim::RaceSlot s = sim::PlaceOnGrid(track, course, g);
    StartArgs a;
    a.chunk = s.chunkHint;
    a.x = s.x;
    a.y = s.y;
    a.sinH = s.headingSin;
    a.cosH = s.headingCos;
    a.controlClass = sim::EntryControlClass(Get<uint8_t>(ram, entry + 2));
    a.transmission = Get<uint8_t>(ram, entry + 3);
    a.contact = sim::EntryContactType(Get<uint8_t>(ram, entry + 2), Get<uint8_t>(ram, D(kGameMode)), Get<uint8_t>(ram, D(0x800A951Cu)) != 0); // the ghost of mode 6: 2
    a.byte1C = 0;
    a.carIndex = car;
    a.gridOffset = 0; // 0x80039040's offset: only player entries in game mode 0 with 0x801D5862 != 0 (not ported)
    return a;
}

std::array<std::array<int32_t, 3>, 16> GridOf(const uint8_t* ram) {
    std::array<std::array<int32_t, 3>, 16> grid{};
    for (uint32_t i = 0; i < 16; i++)
        for (uint32_t k = 0; k < 3; k++) grid[i][k] = Get<int32_t>(ram, D(kTro) + 0x58 + i * 12 + k * 4);
    return grid;
}

int32_t NoseOfModel(const uint8_t* ram, uint32_t model) { // 0x80017E74 on the model object (+0x870 -> LOD 0 block)
    if (model == 0) return 0;
    const uint32_t lod = Get<uint32_t>(ram, model + 0x870);
    return sim::CarNoseOffset(Get<int16_t>(ram, lod + 0x40), Get<int16_t>(ram, lod + 0x4C));
}

uint16_t CourseFlags(const uint8_t* ram) { return Get<uint16_t>(ram, D(kCourseTable) + uint32_t(Get<uint8_t>(ram, D(kCourseIndex))) * 24 + 8); }

} // namespace

int VerifyGrid(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt, const Track* track, std::span<const uint8_t> tro, const GtfsVolume* vol) {
    int failures = 0;
    if (!track || tro.empty()) {
        std::puts("GridArgs   0x80012CD4  skipped (needs the course: gt2verify <ram> <disc> <course>)");
        return failures;
    }
    Rng rng{mt};
    const sim::CourseExtras extras = sim::BuildCourseExtras(*track, tro);
    const sim::NativeCourse course(*track, extras);
    std::vector<uint32_t> models;
    for (uint32_t i = 0; i < kMaxCars; i++)
        if (const uint32_t m = Get<uint32_t>(pristine.data(), D(kModels) + i * 4); m != 0) models.push_back(m);

    // ---- GridArgs: random variants on the image
    {
        size_t cases = 0, mismatches = 0, shown = 0;
        for (size_t variant = 0; variant < 600; variant++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            uint8_t* ram = guest.Ram();
            const uint32_t car = uint32_t(variant % kCarCount);
            if (variant % 4 != 0) {
                Put<uint32_t>(ram, D(kTro) + 0x54, rng.Next() % 3 ? rng.Next() & 0xFFFF : rng.Next());
                for (uint32_t i = 0; i < 16; i++) { // around random chunks of the course (a few metres off the road too)
                    if (rng.Chance(3)) continue;
                    const TrackChunk& c = track->chunks[rng.Next() % track->chunks.size()];
                    for (uint32_t k = 0; k < 3; k++) Put<int32_t>(ram, D(kTro) + 0x58 + i * 12 + k * 4, c.centre[k] + rng.Range(-0x100000, 0x100000));
                }
                const uint32_t entry = D(kEntries) + car * kEntryStride;
                Put<uint8_t>(ram, entry + 1, uint8_t(rng.Range(0, 15)));
                Put<uint8_t>(ram, entry + 3, uint8_t(rng.Range(0, 1)));
                static constexpr uint8_t kModes[] = {0, 1, 2, 3, 4, 6};
                const uint8_t mode = kModes[rng.Next() % 6];
                Put<uint8_t>(ram, D(kGameMode), mode);
                static constexpr uint8_t kKinds[] = {1, 3, 4};
                Put<uint8_t>(ram, entry + 2, mode == 6 ? uint8_t(rng.Range(1, 3)) : kKinds[rng.Next() % 3]); // mode 6: AI, the ghost (contact 2), player 1
                const uint32_t flagsAddress = D(kCourseTable) + uint32_t(Get<uint8_t>(ram, D(kCourseIndex))) * 24 + 8;
                if (rng.Chance(2)) Put<uint16_t>(ram, flagsAddress, uint16_t(Get<uint16_t>(ram, flagsAddress) ^ 0x20));
                if (mode != 0) Put<uint8_t>(ram, D(0x801D5862u), uint8_t(rng.Next())); // zeroed by the mode test
                else Put<uint8_t>(ram, D(0x801D5862u), 0);
                if (!models.empty()) Put<uint32_t>(ram, D(kModels) + car * 4, rng.Chance(8) ? 0u : models[rng.Next() % models.size()]);
            }
            const std::vector<uint8_t> image(ram, ram + Bus::kRamSize);
            StartArgs original;
            if (!OriginalArgs(guest, car, original)) throw std::runtime_error("grid: 0x80012CD4 did not reach 0x80033384");
            const StartArgs ours = NativeArgs(image.data(), *track, course, car, CourseFlags(image.data()), NoseOfModel(image.data(), Get<uint32_t>(image.data(), D(kModels) + car * 4)),
                                              GridOf(image.data()), int32_t(Get<uint32_t>(image.data(), D(kTro) + 0x54)));
            cases++;
            if (!(ours == original)) {
                mismatches++;
                if (shown++ < 3) std::printf("    MISMATCH variant %zu car %u:\n      original %s\n      ours     %s\n", variant, car, Describe(original).c_str(), Describe(ours).c_str());
            }
        }
        Report("GridArgs", 0x80012CD4u, cases, mismatches, failures);
    }

    // ---- GridDisc: the dump's race from the disc's course and body models (gt2game's path)
    if (vol) {
        size_t cases = 0, mismatches = 0;
        const uint8_t mode = Get<uint8_t>(pristine.data(), D(kGameMode));
        const CourseInfoTable info = ParseCourseInfo(vol->Read(".crsinfo"));
        const uint32_t courseIndex = Get<uint8_t>(pristine.data(), D(kCourseIndex));
        if (courseIndex >= info.entries.size()) throw std::runtime_error("grid: the dump's course index is not in .crsinfo");
        const uint16_t flags = info.entries[courseIndex].flags;
        // The race's car tables: the licence file in mode 3; the arcade car tables on the arcade disc (its shell loads
        // carparam/usa_arcade_data.dat, docs/research/arcade_disc.md section 3); the GT-mode tables otherwise.
        const CarParamTables tables = mode == 3 ? LicenseData::Load(*vol).Tables()
                                      : ActiveProfile().arcade ? CarParamTables::Load(*vol, "carparam/usa_arcade_data.dat")
                                                               : CarParamTables::Load(*vol);
        for (uint32_t car = 0; car < kCarCount; car++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            StartArgs original;
            if (!OriginalArgs(guest, car, original)) throw std::runtime_error("grid: 0x80012CD4 did not reach 0x80033384");
            CarConfig config;
            std::memcpy(&config, pristine.data() + ((D(kConfigs) + car * kEntryStride) & 0x1FFFFF), sizeof(config));
            const RacingModifyRow& rm = tables.RowAs<RacingModifyRow>(kTableRacingModify, config.racingModify);
            const std::string modelId = UnpackCarId(rm.modelId);
            const CarBodyDimensions body = BodyDimensionsOf(ParseCarModel(vol->Read("carobj/" + modelId + ".cdo")));
            const int32_t nose = sim::CarNoseOffset(body.bboxFront, body.scaleShift);
            const StartArgs ours = NativeArgs(pristine.data(), *track, course, car, flags, nose, track->startGrid, track->startAngle);
            cases++;
            const bool same = ours == original;
            mismatches += same ? 0 : 1;
            std::printf("    car %u %-6s grid slot %u nose %.3f m: %s%s%s\n", car, modelId.c_str(), unsigned(Get<uint8_t>(pristine.data(), D(kEntries) + car * kEntryStride + 1)),
                        nose / 65536.0, Describe(ours).c_str(), same ? " = original" : "\n      original ", same ? "" : Describe(original).c_str());
        }
        Report("GridDisc", 0x80012CD4u, cases, mismatches, failures);
    }
    return failures;
}

} // namespace gt2::verify
