// Differential checks of src/game/sim/race_sim.* against the original (see guest.h for the harness): the course
// queries of the placement and the progress bookkeeping (0x80028288 nearest chunk, 0x80028900 ground height,
// 0x80028C6C course distance) on our parsed .tro plus its extras, the render transform (0x8001336C, the transform
// part of 0x800133F0) and the race order (0x80042568). With the .tro bytes the extras parsed from the file are
// also compared with the guest's course object.
#include <cstring>
#include <stdexcept>

#include "game/sim/field.h"
#include "game/sim/race_sim.h"
#include "game/sim/trig.h"
#include "guest.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kCourseObject = 0x800A9500u;      // the guest's course object (chunk pointer table at + 0xB544)
constexpr uint32_t kQueryScratch = kStack - 0x80;    // guest stack area (excluded from the comparison) for records
constexpr uint32_t kRaceOrder = 0x801C8578u;         // s8[6] race order table

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
sim::Car* CarsOf(uint8_t* ram) { return reinterpret_cast<sim::Car*>(At(ram, kCarBase)); }

struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
};

// The extras as the guest's course object holds them (same walk as verify_ground's BuildCourseSurface).
sim::CourseExtras ExtrasFromGuest(const uint8_t* ram, const Track& track) {
    static constexpr uint32_t kStride[8] = {12, 12, 20, 24, 12, 12, 20, 24};
    sim::CourseExtras extras;
    const uint32_t table = Get<uint32_t>(ram, D(kCourseObject) + 0xB544);
    extras.courseLength = Get<int32_t>(ram, table);
    const int16_t chunkCount = Get<int16_t>(ram, table + 4);
    if (size_t(chunkCount) != track.chunks.size()) throw std::runtime_error("race: the guest's course has a different chunk count than the parsed track");
    for (uint32_t c = 0; c < uint32_t(chunkCount); c++) {
        const uint32_t chunk = Get<uint32_t>(ram, table + 0xC + c * 4);
        extras.chunks.push_back({Get<int32_t>(ram, chunk + 0x10), Get<uint16_t>(ram, chunk + 0x14), Get<uint16_t>(ram, chunk + 0x16)});
        const uint32_t shape = chunk + 0xA4, grid = Get<uint32_t>(ram, chunk + 0x9C);
        uint32_t listOffset[8], cumulative[8], listCount[8], total = 0;
        for (uint32_t li = 0; li < 8; li++) {
            listOffset[li] = Get<uint32_t>(ram, shape + 4 + li * 4);
            listCount[li] = uint32_t(Get<int16_t>(ram, shape + 0x30 + li * 2));
            cumulative[li] = total;
            total += listCount[li];
        }
        sim::SurfaceGrid g;
        g.originX = Get<int16_t>(ram, grid);
        g.originZ = Get<int16_t>(ram, grid + 2);
        g.shiftX = Get<int16_t>(ram, grid + 4);
        g.shiftZ = Get<int16_t>(ram, grid + 6);
        for (uint32_t cell = 0; cell < 16; cell++) {
            const uint32_t n = Get<uint16_t>(ram, grid + 8 + cell * 2), list = Get<uint32_t>(ram, grid + 0x28 + cell * 4);
            for (uint32_t j = 0; j < n; j++) {
                const uint32_t polygon = Get<uint32_t>(ram, list + j * 4);
                bool found = false;
                for (uint32_t li = 0; li < 8 && !found; li++) {
                    if (polygon < listOffset[li] || polygon >= listOffset[li] + listCount[li] * kStride[li]) continue;
                    g.cells[cell].push_back(uint16_t(cumulative[li] + (polygon - listOffset[li]) / kStride[li]));
                    found = true;
                }
                if (!found) throw std::runtime_error("race: grid polygon pointer outside the chunk's road shape");
            }
        }
        extras.grids.push_back(std::move(g));
    }
    return extras;
}

bool SameExtras(const sim::CourseExtras& a, const sim::CourseExtras& b) {
    if (a.courseLength != b.courseLength || a.chunks.size() != b.chunks.size() || a.grids.size() != b.grids.size()) return false;
    for (size_t c = 0; c < a.chunks.size(); c++) {
        if (std::memcmp(&a.chunks[c], &b.chunks[c], sizeof(sim::ChunkExtra)) != 0) return false;
        const sim::SurfaceGrid& ga = a.grids[c];
        const sim::SurfaceGrid& gb = b.grids[c];
        if (ga.originX != gb.originX || ga.originZ != gb.originZ || ga.shiftX != gb.shiftX || ga.shiftZ != gb.shiftZ) return false;
        for (uint32_t cell = 0; cell < 16; cell++)
            if (ga.cells[cell] != gb.cells[cell]) return false;
    }
    return true;
}

// A world point (16.16: X, height, Z) near the course: around a random chunk's centre, sometimes far away.
void RandomWorldPoint(Rng& rng, const Track& track, int32_t point[3], uint32_t& chunk) {
    chunk = uint32_t(rng.Next() % track.chunks.size());
    const TrackChunk& c = track.chunks[chunk];
    const int32_t spread = rng.Chance(4) ? 0x4000000 : rng.Chance(2) ? 0x800000 : 0x100000; // 1024 m, 128 m, 16 m
    point[0] = c.centre[0] + rng.Range(-spread, spread);
    point[1] = c.centre[1] + rng.Range(-0x100000, 0x100000);
    point[2] = c.centre[2] + rng.Range(-spread, spread);
    if (rng.Chance(8)) chunk = uint32_t(rng.Next() % track.chunks.size());
}

} // namespace

int VerifyRace(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt, const Track* track, std::span<const uint8_t> tro) {
    int failures = 0;
    Rng rng{mt};
    if (!track) {
        std::puts("Race       0x80028588  skipped (needs the course: gt2verify <ram> <disc> <course>)");
        return failures;
    }
    // ---- the course extras: from the .tro against the guest's course object
    sim::CourseExtras extras = ExtrasFromGuest(pristine.data(), *track);
    if (!tro.empty()) {
        const sim::CourseExtras fromFile = sim::BuildCourseExtras(*track, tro);
        const bool same = SameExtras(fromFile, extras);
        std::printf("%-10s .tro        %zu chunks, grids + distance fields %s  %s\n", "Extras", extras.chunks.size(), same ? "identical to the guest's" : "DIFFER from the guest's",
                    same ? "ok" : "FAIL");
        failures += same ? 0 : 1;
    }
    sim::NativeCourse course(*track, extras);
    std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
    const uint32_t table = Get<uint32_t>(pristine.data(), D(kCourseObject) + 0xB544);

    // ---- 0x80028288: nearest chunk (table, point*)
    {
        size_t cases = 0, bad = 0;
        for (int i = 0; i < 3000; i++) {
            int32_t point[3];
            uint32_t hint;
            RandomWorldPoint(rng, *track, point, hint);
            for (uint32_t k = 0; k < 3; k++) Put<int32_t>(guest.Ram(), kQueryScratch + k * 4, point[k]);
            const int32_t original = int32_t(guest.Call(0x80028288u, table, kQueryScratch));
            const int32_t ours = course.NearestChunk(point);
            cases++;
            if (original != ours && bad++ < 3) std::printf("    MISMATCH NearestChunk(%d, %d, %d): original %d, ours %d\n", point[0], point[1], point[2], original, ours);
        }
        Report("NearChunk", 0x80028288u, cases, bad, failures);
    }
    // ---- 0x80028900: ground height under (x, y) with the chunk in / out
    {
        size_t cases = 0, bad = 0, found = 0;
        for (int i = 0; i < 3000; i++) {
            int32_t point[3];
            uint32_t hint;
            RandomWorldPoint(rng, *track, point, hint);
            const int32_t x = point[0] >> 4, y = int32_t(0u - uint32_t(point[2])) >> 4;
            int32_t chunkOriginal = int32_t(hint), chunkOurs = int32_t(hint);
            Put<int32_t>(guest.Ram(), kQueryScratch, chunkOriginal);
            Put<uint32_t>(guest.Ram(), kStack + 0x10, kQueryScratch);
            const int32_t original = int32_t(guest.Call(0x80028900u, D(kCourseObject), uint32_t(x), uint32_t(y), 0x64000));
            chunkOriginal = Get<int32_t>(guest.Ram(), kQueryScratch);
            const int32_t ours = course.GroundHeight(x, y, chunkOurs);
            cases++;
            found += original != sim::kNoSurfaceHeight;
            if ((original != ours || chunkOriginal != chunkOurs) && bad++ < 3)
                std::printf("    MISMATCH GroundHeight(%d, %d, chunk %u): original %d (chunk %d), ours %d (chunk %d)\n", x, y, hint, original, chunkOriginal, ours, chunkOurs);
        }
        std::printf("%-10s 0x80028900  %zu cases (%zu on the road), %zu mismatches  %s\n", "GroundHt", cases, found, bad, bad ? "FAIL" : "ok");
        failures += bad ? 1 : 0;
    }
    // ---- 0x80028C6C: course distance of a simulation point (course, chunk, point*)
    {
        size_t cases = 0, bad = 0;
        for (int i = 0; i < 3000; i++) {
            int32_t point[3];
            uint32_t hint;
            RandomWorldPoint(rng, *track, point, hint);
            const int32_t x16 = point[0], h16 = point[1], y16 = int32_t(0u - uint32_t(point[2]));
            Put<int32_t>(guest.Ram(), kQueryScratch + 0x10, x16);
            Put<int32_t>(guest.Ram(), kQueryScratch + 0x14, h16);
            Put<int32_t>(guest.Ram(), kQueryScratch + 0x18, y16);
            const int32_t original = int32_t(guest.Call(0x80028C6Cu, D(kCourseObject), hint, kQueryScratch + 0x10));
            const int32_t ours = course.CourseDistance(int32_t(hint), x16, h16, y16);
            cases++;
            if (original != ours && bad++ < 3) std::printf("    MISMATCH CourseDistance(chunk %u, %d, %d, %d): original %d, ours %d\n", hint, x16, h16, y16, original, ours);
        }
        Report("CourseDist", 0x80028C6Cu, cases, bad, failures);
    }
    // ---- 0x800133F0: the render transform of a car (transform part; the sound / particle branches are disabled by the inputs)
    {
        std::vector<uint32_t> cars;
        for (uint32_t car = 0; car < kCarCount; car++) cars.push_back(kCarBase + car * kCarStride);
        const StatefulResult r = VerifyStateful(
            guest, pristine, 0x800133F0u, cars, 200,
            [&](uint8_t* ram, uint8_t*, uint32_t car, size_t variant) {
                uint8_t* body = At(ram, car + kBodyOffset);
                Put<int16_t>(ram, car + 0x18, 0);        // no live pad: skips the vibration block
                Put<uint8_t>(ram, car + 0x0E, uint8_t(rng.Range(0, 1)));
                Put<uint8_t>(ram, car + 0x0F, uint8_t(rng.Next()));
                for (uint32_t w = 0; w < 4; w++) {
                    uint8_t* wheel = body + 0x460 + w * 0x68;
                    wheel[0x1C] &= 0x7F;                 // no skid particle spawn
                    if (variant % 4) sim::SetField<int16_t>(wheel, 0x10, int16_t(rng.Range(-3000, 3000)));
                }
                if (variant % 4 == 0) return;
                sim::CarBody& b = *reinterpret_cast<sim::CarBody*>(body);
                for (int32_t& v : b.position) v = rng.Range(-0x8000000, 0x8000000);
                for (uint32_t o = 0x680; o < 0x68C; o += 4) sim::SetField<int32_t>(body, o, rng.Range(-0x8000000, 0x8000000));
                const int32_t pitch = rng.Range(-0x200, 0x200), roll = rng.Range(-0x200, 0x200), heading = rng.Range(-0xFFF, 0xFFF);
                sim::BuildAttitudeMatrix(reinterpret_cast<int16_t*>(body + 0x668), reinterpret_cast<int16_t*>(body + 0x670), reinterpret_cast<int16_t*>(body + 0x678), pitch, roll, heading);
                sim::BuildAttitudeMatrix(reinterpret_cast<int16_t*>(body + 0x68C), reinterpret_cast<int16_t*>(body + 0x694), reinterpret_cast<int16_t*>(body + 0x69C), rng.Range(-0x200, 0x200),
                                         rng.Range(-0x200, 0x200), heading);
                if (variant % 4 == 3)
                    for (uint32_t o = 0x668; o < 0x6A4; o += 2) sim::SetField<int16_t>(body, o, int16_t(rng.Next()));
                sim::SetField<int16_t>(body, 0x02E, int16_t(rng.Range(-0x2000, 0x2000)));
                for (uint32_t axle = 0; axle < 2; axle++) sim::SetField<int16_t>(body, 0x12C + axle * 0x34 + 8, int16_t(rng.Range(-3000, 3000)));
                for (uint32_t w = 0; w < 4; w++)
                    for (uint32_t o = 0x7C4; o < 0x7D4; o += 2) sim::SetField<int16_t>(At(ram, car), o + w * 0x10, int16_t(rng.Next()));
            },
            [&](uint8_t* ram, uint8_t*, uint32_t car) {
                uint8_t* record = At(ram, car);
                const sim::CarBody& body = *reinterpret_cast<const sim::CarBody*>(record + kBodyOffset);
                if (record[0x0E] != 0) record[0x0F] = 0;
                sim::SetField<int32_t>(record, 0x14, body.chunkIndex);
                int32_t previous[3];
                std::memcpy(previous, record + 0x830, sizeof(previous));
                std::memcpy(record + 0x85C, previous, sizeof(previous));
                const sim::CarPose physics = sim::PhysicsPoseOf(body), visual = sim::VisualPoseOf(body);
                auto store = [&](uint32_t at, const sim::CarPose& pose) {
                    for (uint32_t r = 0; r < 3; r++)
                        for (uint32_t k = 0; k < 3; k++) sim::SetField<int16_t>(record, at + (r * 3 + k) * 2, pose.rotation[r][k]);
                    for (uint32_t k = 0; k < 3; k++) sim::SetField<int32_t>(record, at + 0x14 + k * 4, pose.worldPosition[k]);
                };
                store(0x81C, physics);
                store(0x83C, visual);
                // Bytes the original leaves in the padding (+0x12) and the delta record are not part of the port's
                // output; keep the guest's delta arithmetic: delta = position (before the offset) - previous.
                for (uint32_t k = 0; k < 3; k++) {
                    const int32_t unshifted = int32_t(uint32_t(physics.worldPosition[k]) - uint32_t((int32_t(physics.rotation[k][2]) * int32_t(sim::Field<int16_t>(&body, 0x02E))) >> 8));
                    sim::SetField<int32_t>(record, 0x868 + k * 4, int32_t(uint32_t(unshifted) - uint32_t(previous[k])));
                }
                for (uint32_t w = 0; w < 4; w++) {
                    uint8_t* wheelRecord = record + 0x7C4 + w * 0x10;
                    sim::SetField<int32_t>(wheelRecord, 0x0, sim::Field<int32_t>(wheelRecord, 0x8));
                    sim::SetField<int32_t>(wheelRecord, 0x4, sim::Field<int32_t>(wheelRecord, 0xC));
                    const int16_t travel = sim::Field<int16_t>(&body.wheels[w], 0x10);
                    const int16_t reference = sim::AxleSuspensionOf(body, w >> 1).rideReference;
                    sim::SetField<int16_t>(wheelRecord, 0x2, int16_t(reference - travel));
                }
            });
        Report("RenderXf", 0x800133F0u, r.cases, r.mismatches, failures);
    }
    // ---- 0x80042568: race order (cars, count)
    {
        size_t cases = 0, bad = 0;
        for (int i = 0; i < 1500; i++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            uint8_t* ram = guest.Ram();
            int8_t order[kMaxCars];
            for (uint32_t k = 0; k < kCarCount; k++) order[k] = int8_t(k);
            for (uint32_t k = kCarCount - 1; k > 0; k--) std::swap(order[k], order[rng.Next() % (k + 1)]);
            for (uint32_t k = 0; k < kCarCount; k++) Put<int8_t>(ram, D(kRaceOrder) + k, order[k]);
            for (uint32_t car = 0; car < kCarCount; car++) {
                uint8_t* body = At(ram, kCarBase + car * kCarStride + kBodyOffset);
                sim::SetField<int16_t>(body, 0x608, int16_t(rng.Range(0, 3)));
                sim::SetField<uint8_t>(body, 0x78D, uint8_t(rng.Next()));
                sim::SetField<int32_t>(body, 0x604, rng.Chance(3) ? rng.Range(0, 3) * 0x1000000 : rng.Range(0, 0x10000000));
                sim::SetField<uint8_t>(body, 0x6FC, uint8_t(rng.Range(0, 2)));
                sim::SetField<uint8_t>(body, 0x750, uint8_t(rng.Next()));
            }
            std::vector<uint8_t> ours(ram, ram + Bus::kRamSize);
            guest.Call(0x80042568u, kCarBase, kCarCount);
            sim::UpdateRaceOrder(CarsOf(ours.data()), int(kCarCount), reinterpret_cast<int8_t*>(At(ours.data(), D(kRaceOrder))));
            cases++;
            const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000;
            if (std::memcmp(ours.data(), ram, stackLow) != 0 && bad++ < 3) {
                std::printf("    MISMATCH race order variant %d: original", i);
                for (uint32_t k = 0; k < kCarCount; k++) std::printf(" %d", Get<int8_t>(ram, D(kRaceOrder) + k));
                std::printf(", ours");
                for (uint32_t k = 0; k < kCarCount; k++) std::printf(" %d", Get<int8_t>(ours.data(), D(kRaceOrder) + k));
                std::printf("\n");
            }
        }
        Report("RaceOrder", 0x80042568u, cases, bad, failures);
    }
    return failures;
}

} // namespace gt2::verify
