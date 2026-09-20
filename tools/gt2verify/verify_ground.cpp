// Differential checks of src/game/sim/ground.* (ground contact, course height query, suspension, attitude) against
// the original (see guest.h for the harness).
#include <cstring>
#include <stdexcept>

#include "game/sim/ground.h"
#include "guest.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kCourseObject = 0x800A9500u;      // the guest's course object (chunk pointer table at + 0xB544)
constexpr uint32_t kCourseTableEntry = 0x801E18E8u;  // course table of the game shell (0x80060E94), 24 bytes per course
constexpr uint32_t kCourseIndexAddress = 0x800AF230u;
constexpr uint32_t kRaceStatePointer = 0x801C8568u;  // -> + 0x18 = race grid list
constexpr uint32_t kRateAddress = 0x801C8570u, kViewModeAddress = 0x801C9990u, kRaceModeAddress = 0x801D5866u;
constexpr uint32_t kCollisionDisabledAddress = 0x800A9520u;
constexpr uint32_t kFlagA = 0x800A951Cu, kFlagB = 0x801C9995u, kFlagC = 0x800AF232u;
constexpr uint32_t kFreeRam = 0x801E8000u;           // scratch words for pure-function checks

// ---- random inputs: tier 1 realistic, 2 wide, 3 extreme ----
struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
    int32_t Spread(int tier, int32_t realistic, int32_t wide) {
        if (tier >= 3) {
            if (Chance(4)) {
                static constexpr int32_t kEdges[] = {0, 1, -1, 0x7FFFFFFF, int32_t(0x80000000), 0x1000, -0x1000, 0xFFFF, -0x10000, 0x7FFF, -0x8000};
                return kEdges[Next() % (sizeof(kEdges) / sizeof(kEdges[0]))];
            }
            return int32_t(Next());
        }
        const int32_t limit = tier == 1 ? realistic : wide;
        return Range(-limit, limit);
    }
    int32_t Positive(int tier, int32_t realistic, int32_t wide) {
        if (tier >= 3) return Chance(4) ? 0x7FFFFFFF : int32_t(Next() & 0x7FFFFFFF);
        return Range(0, tier == 1 ? realistic : wide);
    }
    int32_t S16(int tier, int32_t realistic) { return tier >= 3 ? int32_t(int16_t(Next())) : Range(-realistic, realistic); }
};

uint32_t BodyAddress(uint32_t car) { return kCarBase + car * kCarStride + kBodyOffset; }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
const uint8_t* At(const uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
sim::Car* CarsOf(uint8_t* ram) { return reinterpret_cast<sim::Car*>(At(ram, kCarBase)); }
sim::CarBody& BodyAt(uint8_t* ram, uint32_t address) { return *reinterpret_cast<sim::CarBody*>(At(ram, address)); }

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, At(ram, address), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* base, uint32_t offset, T value) { std::memcpy(base + offset, &value, sizeof(T)); }
void Put32(uint8_t* base, uint32_t offset, int32_t v) { Put<int32_t>(base, offset, v); }
void Put16(uint8_t* base, uint32_t offset, int32_t v) { Put<int16_t>(base, offset, int16_t(v)); }
void Put8(uint8_t* base, uint32_t offset, int32_t v) { Put<uint8_t>(base, offset, uint8_t(v)); }

// ---- the guest's course object -> our CourseSurface (grid lists mapped to polygon indices of the parsed track) ----
sim::CourseSurface BuildCourseSurface(const uint8_t* ram, const Track& track) {
    static constexpr uint32_t kStride[8] = {12, 12, 20, 24, 12, 12, 20, 24};
    sim::CourseSurface course;
    course.track = &track;
    const uint32_t table = Get<uint32_t>(ram, D(kCourseObject) + 0xB544);
    const int16_t chunkCount = Get<int16_t>(ram, table + 4);
    if (size_t(chunkCount) != track.chunks.size()) throw std::runtime_error("ground: the guest's course has a different chunk count than the parsed track");
    for (uint32_t c = 0; c < uint32_t(chunkCount); c++) {
        const uint32_t chunk = Get<uint32_t>(ram, table + 0xC + c * 4);
        const uint32_t shape = chunk + 0xA4, grid = Get<uint32_t>(ram, chunk + 0x9C);
        uint32_t listOffset[8], cumulative[8], listCount[8];
        uint32_t total = 0;
        for (uint32_t li = 0; li < 8; li++) {
            listOffset[li] = Get<uint32_t>(ram, shape + 4 + li * 4);
            listCount[li] = uint32_t(Get<int16_t>(ram, shape + 0x30 + li * 2));
            cumulative[li] = total;
            total += listCount[li];
        }
        if (total != track.chunks[c].road.polygons.size()) throw std::runtime_error("ground: polygon count differs from the parsed track");
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
                    if ((polygon - listOffset[li]) % kStride[li] != 0) throw std::runtime_error("ground: grid polygon pointer is not aligned to its list");
                    g.cells[cell].push_back(uint16_t(cumulative[li] + (polygon - listOffset[li]) / kStride[li]));
                    found = true;
                }
                if (!found) throw std::runtime_error("ground: grid polygon pointer outside the chunk's road shape");
            }
        }
        course.grids.push_back(std::move(g));
    }
    return course;
}

// ---- constants and globals as the (possibly randomised) RAM image holds them ----
sim::GroundConstants ReadConstants(const uint8_t* ram) {
    sim::GroundConstants k;
    for (uint32_t i = 0; i < 16; i++) {
        k.roughnessAmplitude[i] = Get<int16_t>(ram, D(0x80046F88u) + i * 2);
        k.roughnessFrequency[i] = Get<int16_t>(ram, D(0x80046F98u) + i * 2);
        k.roughnessSpeedScaled[i] = Get<uint8_t>(ram, D(0x80046FA8u) + i);
    }
    for (uint32_t i = 0; i < 2; i++) {
        k.viewYawGain[i] = Get<int32_t>(ram, D(0x80046C94u) + i * 4);
        k.viewYawDamping[i] = Get<int32_t>(ram, D(0x80046C9Cu) + i * 4);
    }
    std::memcpy(&k.effects, At(ram, D(0x80046EACu)), sizeof(k.effects));
    return k;
}

sim::GroundGlobals ReadGlobals(const uint8_t* ram) {
    sim::GroundGlobals g;
    g.rate = Get<int32_t>(ram, D(kRateAddress));
    g.viewMode = Get<uint8_t>(ram, D(kViewModeAddress));
    g.raceMode = Get<uint8_t>(ram, D(kRaceModeAddress));
    g.collisionDisabled = Get<uint16_t>(ram, D(kCollisionDisabledAddress)) != 0;
    const uint32_t courseEntry = D(kCourseTableEntry) + uint32_t(Get<uint8_t>(ram, D(kCourseIndexAddress))) * 24;
    g.dirtCourse = (Get<uint16_t>(ram, courseEntry + 8) & 4) != 0;
    g.flag800A951C = Get<uint8_t>(ram, D(kFlagA));
    g.flag801C9995 = Get<uint8_t>(ram, D(kFlagB));
    g.flag800AF232 = Get<uint8_t>(ram, D(kFlagC));
    g.wheelEffectsTail = gt2::ActiveProfile().wheelEffectsTail; // the Simulation build's tail of 0x800426F0 (ground.h)
    const uint32_t table = Get<uint32_t>(ram, D(kCourseObject) + 0xB544);
    g.courseLength = Get<int32_t>(ram, table);
    const uint32_t list = Get<uint32_t>(ram, Get<uint32_t>(ram, D(kRaceStatePointer)) + 0x18);
    g.grid.count = list ? Get<int32_t>(ram, list) : 0;
    for (uint32_t slot = 0; slot < 12; slot++) { // the dump's list has 12 records; the reset path indexes them without a count check
        g.grid.distance.push_back(Get<int32_t>(ram, list + 4 + slot * 0x28 + 0x14));
        g.grid.heading.push_back(Get<int32_t>(ram, list + 4 + slot * 0x28 + 0x24));
    }
    return g;
}

void RandomiseGlobals(Rng& rng, uint8_t* ram, int tier) {
    Put32(At(ram, D(kRateAddress)), 0, rng.Chance(2) ? 30 : rng.Range(1, tier == 1 ? 60 : 300));
    Put8(At(ram, D(kViewModeAddress)), 0, rng.Range(0, 1));
    static constexpr int32_t kModes[] = {2, 3, 6, 0, 1, 4, 5, 12};
    Put8(At(ram, D(kRaceModeAddress)), 0, kModes[rng.Next() % (rng.Chance(2) ? 3 : 8)]);
    Put16(At(ram, D(kCollisionDisabledAddress)), 0, rng.Chance(8) ? 1 : 0);
    Put8(At(ram, D(kFlagA)), 0, rng.Range(0, 1));
    Put8(At(ram, D(kFlagB)), 0, rng.Range(0, 1));
    Put8(At(ram, D(kFlagC)), 0, rng.Range(0, 1));
    const uint32_t courseEntry = D(kCourseTableEntry) + uint32_t(Get<uint8_t>(ram, D(kCourseIndexAddress))) * 24;
    Put16(At(ram, courseEntry + 8), 0, rng.Chance(2) ? 0 : 4);
    const uint32_t list = Get<uint32_t>(ram, Get<uint32_t>(ram, D(kRaceStatePointer)) + 0x18);
    Put32(At(ram, list), 0, rng.Range(2, 12)); // 0 / 1 make the original read outside its list
    if (tier >= 2)
        for (uint32_t slot = 0; slot < 12; slot++) {
            Put32(At(ram, list + 4 + slot * 0x28 + 0x14), 0, rng.Spread(tier, 300000000, 0x7FFFFFFF));
            Put32(At(ram, list + 4 + slot * 0x28 + 0x24), 0, rng.S16(tier, 0x1000));
        }
}

// ---- body inputs of the ground subsystem ----
struct RoadPoint { int32_t x, y, z; uint32_t chunk; }; // world 16.16 (X, height, Z)

// A random point on (or near) a drivable polygon of chunk `c` of the track, or anywhere nearby for tier 3.
RoadPoint RandomRoadPoint(Rng& rng, const Track& track, const sim::CourseSurface* course, uint32_t c, int tier) {
    const TrackChunk& chunk = track.chunks[c];
    RoadPoint p{chunk.centre[0], chunk.centre[1], chunk.centre[2], c};
    if (tier >= 3 && rng.Chance(2)) {
        p.x = int32_t(uint32_t(p.x) + rng.Next() % 0x2000000u - 0x1000000u); // +- 256 m
        p.z = int32_t(uint32_t(p.z) + rng.Next() % 0x2000000u - 0x1000000u);
        p.y = int32_t(uint32_t(p.y) + rng.Next() % 0x400000u - 0x200000u);
        return p;
    }
    std::vector<uint16_t> candidates;
    if (course)
        for (const auto& cell : course->grids[c].cells) candidates.insert(candidates.end(), cell.begin(), cell.end());
    if (candidates.empty() || rng.Chance(8))
        for (uint16_t i = 0; i < chunk.road.polygons.size(); i++) candidates.push_back(i);
    if (candidates.empty()) return p;
    const TrackPolygon& polygon = chunk.road.polygons[candidates[rng.Next() % candidates.size()]];
    const size_t corners = polygon.IsQuad() ? 4 : 3;
    // random convex combination of the corners, sometimes pushed outside
    double w[4] = {0, 0, 0, 0}, sum = 0;
    for (size_t i = 0; i < corners; i++) { w[i] = double(rng.Next() % 1000 + 1); sum += w[i]; }
    double x = 0, y = 0, z = 0;
    for (size_t i = 0; i < corners; i++) {
        const TrackVertex& v = chunk.road.vertices[polygon.vertex[i]];
        x += v.x * w[i] / sum; y += v.y * w[i] / sum; z += v.z * w[i] / sum;
    }
    if (rng.Chance(4)) { x += (rng.Next() % 200) - 100.0; y += (rng.Next() % 200) - 100.0; }
    if (rng.Chance(5)) { // exactly on a corner or an edge (ties between polygons)
        const TrackVertex& a = chunk.road.vertices[polygon.vertex[rng.Next() % corners]];
        const TrackVertex& b = chunk.road.vertices[polygon.vertex[rng.Next() % corners]];
        const double t = (rng.Next() % 5) / 4.0;
        x = a.x + (b.x - a.x) * t; y = a.y + (b.y - a.y) * t; z = a.z + (b.z - a.z) * t;
    }
    p.x = int32_t(chunk.cellOrigin[0] + int64_t(x * 1024)) + (rng.Chance(3) ? 0 : rng.Range(0, 1023));
    p.z = int32_t(chunk.cellOrigin[1] + int64_t(y * 1024)) + (rng.Chance(3) ? 0 : rng.Range(0, 1023));
    p.y = int32_t(int64_t(z * 1024)) + (rng.Chance(2) ? rng.Range(-0x40000, 0x40000) : 0);
    return p;
}

void RandomiseAxle(Rng& rng, uint8_t* body, uint32_t axle, int tier) {
    uint8_t* a = body + sim::kAxleSuspensionOffset + axle * 0x34;
    if (tier == 1 && rng.Chance(2)) return; // the dump's values
    Put16(a, 0x00, rng.S16(tier, 2000));
    Put16(a, 0x02, rng.S16(tier, 2500));
    Put16(a, 0x04, rng.S16(tier, 1000));
    Put16(a, 0x08, rng.S16(tier, 1000));
    Put32(a, 0x0C, rng.Positive(tier, 100000, 10000000));
    Put32(a, 0x10, rng.Spread(tier, 4000, 100000));
    Put32(a, 0x14, rng.Spread(tier, 20000, 1000000));
    Put32(a, 0x18, rng.Spread(tier, 40000, 1000000));
    Put32(a, 0x1C, rng.Spread(tier, 4000, 100000));
    Put32(a, 0x20, rng.Spread(tier, 40000, 1000000));
    Put32(a, 0x24, rng.Spread(tier, 40000, 1000000));
    Put32(a, 0x28, rng.Positive(tier, 10000, 1000000));
    Put32(a, 0x2C, rng.Positive(tier, 40000, 1000000));
    Put16(a, 0x32, rng.S16(tier, 2000));
}

void RandomiseWheel(Rng& rng, uint8_t* w, int tier, int32_t bodyHeight) {
    Put32(w, 0x00, tier == 1 ? bodyHeight - rng.Range(-0x800, 0x1000) : rng.Spread(tier, 0x100000, 0x7FFFFFFF));
    Put32(w, 0x04, rng.Spread(tier, 0x1000000, 0x7FFFFFFF));
    Put32(w, 0x08, rng.Chance(6) ? 0 : rng.Positive(tier, 0x8000, 0x7FFFFFFF));
    Put16(w, 0x0C, rng.S16(tier, 0x300));
    Put16(w, 0x10, rng.S16(tier, 2000));
    Put16(w, 0x12, rng.S16(tier, 0x2000));
    Put8(w, 0x14, rng.Chance(3) ? rng.Range(0, 7) : rng.Range(0, tier >= 2 ? 255 : 7));
    Put8(w, 0x15, int32_t(rng.Next() & 0xFF));
    Put32(w, 0x18, rng.Spread(tier, 200000, 2000000));
    for (uint32_t o = 0x1C; o < 0x20; o++) Put8(w, o, int32_t(rng.Next() & 0xFF));
    Put16(w, 0x20, int32_t(rng.Next() & 0xFFFF));
    Put16(w, 0x24, rng.S16(tier, 0x2000));
    Put16(w, 0x26, rng.S16(tier, 0x2000));
    Put16(w, 0x28, rng.S16(tier, 0x800));
    Put16(w, 0x2A, rng.S16(tier, 0x1000));
    Put32(w, 0x2C, rng.Chance(8) ? 0 : rng.Spread(tier, 200000, 2000000));
    Put32(w, 0x30, rng.Chance(8) ? 0 : rng.Spread(tier, 50000, 2000000));
    Put16(w, 0x40, tier >= 2 ? rng.S16(tier, 0x2000) : rng.Range(0, 0x1000));
    Put16(w, 0x42, rng.S16(tier, 200));
    Put16(w, 0x44, rng.Chance(4) ? rng.Range(-0x1000, 0x1000) : rng.S16(tier, 0x1000));
    Put32(w, 0x48, rng.Spread(tier, 0x8000, 0x7FFFFFFF));
    Put32(w, 0x4C, rng.Spread(tier, 0x8000, 0x7FFFFFFF));
    Put16(w, 0x50, rng.S16(tier, 0x800));
    Put32(w, 0x58, rng.Spread(tier, 200000, 20000000));
    Put32(w, 0x5C, rng.Spread(tier, 200000, 20000000));
    Put8(w, 0x63, rng.Chance(2) ? 0 : int32_t(rng.Next() & 0xFF));
}

// Randomises everything the ground subsystem reads. With a track the car is put on a random road polygon.
void RandomiseBody(Rng& rng, uint8_t* ram, uint32_t address, int tier, const Track* track, const sim::CourseSurface* course) {
    uint8_t* body = At(ram, address);
    sim::CarBody& b = BodyAt(ram, address);
    if (track && !(tier >= 3 && rng.Chance(2))) {
        const RoadPoint p = RandomRoadPoint(rng, *track, course, uint32_t(rng.Next() % track->chunks.size()), tier);
        b.position[0] = p.x >> 4;
        b.position[1] = int32_t(0u - uint32_t(p.z)) >> 4;
        b.position[2] = (p.y >> 4) + rng.Range(0, 0x1000);
        b.chunkIndex = rng.Chance(4) ? int32_t(rng.Next() % track->chunks.size()) : int32_t(p.chunk);
    } else {
        for (int32_t& v : b.position) v = rng.Spread(tier, 0x2000000, 0x7FFFFFFF);
        b.chunkIndex = track ? int32_t(rng.Next() % track->chunks.size()) : b.chunkIndex;
    }
    for (int32_t& v : b.velocity) v = rng.Chance(6) ? 0 : rng.Spread(tier, 300000, 3000000);
    b.heading = int16_t(tier >= 3 ? rng.Next() : rng.Next() % 0x1000);
    b.yawRate = rng.Spread(tier, 0x100000, 0x7FFFFFFF);
    b.forwardSpeed = rng.Spread(tier, 300000, 3000000);
    b.lateralSpeed = rng.Spread(tier, 100000, 3000000);
    b.stepTime = int16_t(rng.Chance(2) ? 2184 : rng.Range(0, tier >= 2 ? 0x7FFF : 0x2000));
    b.stepRate12 = rng.Chance(2) ? 0x1E000 : rng.Spread(tier, 0x40000, 0x7FFFFFFF);
    b.downforceForce[0] = rng.Spread(tier, 0x100000, 0x7FFFFFFF);
    b.downforceForce[1] = rng.Spread(tier, 0x100000, 0x7FFFFFFF);
    b.wallImpact = int16_t(rng.Chance(2) ? 0 : rng.S16(tier, 0x1000));
    b.controlClass = uint8_t(rng.Chance(4) ? 1 : 0);
    b.raceState = uint8_t(rng.Chance(6) ? 7 : rng.Chance(4) ? 1 : 0);
    // geometry and response parameters
    Put16(body, 0x0C, rng.Chance(2) ? 5000 : rng.S16(tier, 8000));
    Put16(body, 0x0E, rng.Chance(2) ? -5000 : rng.S16(tier, 8000));
    Put16(body, 0x10, rng.Chance(4) ? 0 : rng.S16(tier, 8000));
    Put16(body, 0x12, rng.Chance(4) ? 0 : rng.S16(tier, 8000));
    Put16(body, 0x14, rng.Chance(2) ? 0x800 : rng.S16(tier, 0x2000));
    Put16(body, 0x18, rng.S16(tier, 4000));
    Put16(body, 0x1A, rng.S16(tier, 4000));
    Put16(body, 0x1E, rng.S16(tier, 0x2000));
    Put16(body, 0x20, rng.S16(tier, 0x2000));
    Put16(body, 0x2C, rng.S16(tier, 0x100));
    for (uint32_t o = 0x6C; o <= 0x78; o += 4) Put32(body, o, rng.Spread(tier, 0x2000, 0x7FFFFFFF));
    Put8(body, 0x372, rng.Range(0, 2));
    Put8(body, 0x37A, rng.Range(0, 7));
    Put8(body, 0x37B, rng.Range(0, 7));
    for (uint32_t gear = 0; gear < 8; gear++) Put32(body, 0x3A4 + gear * 4, rng.Spread(tier, 20000, 0x7FFFFFFF));
    if (Get<int32_t>(ram, address + 0x3A8) == 0) Put32(body, 0x3A8, 1); // divisor of the effects' gear factor
    for (uint32_t axle = 0; axle < 2; axle++) {
        Put16(body, 0x3E4 + axle * 2, rng.Range(800, 2000));
        Put16(body, 0x3E8 + axle * 2, rng.S16(tier, 0x4000));
        RandomiseAxle(rng, body, axle, tier);
        Put32(body, 0x634 + axle * 4, rng.Spread(tier, 500000, 5000000));
    }
    Put8(body, 0x45C, rng.Range(0, int32_t(kCarCount) - 1));
    Put8(body, 0x45E, rng.Chance(3) ? 2 : rng.Range(0, 3));
    Put32(body, 0x604, rng.Spread(tier, 300000000, 0x7FFFFFFF));
    Put8(body, 0x618, rng.Range(0, 7));
    Put8(body, 0x619, rng.Range(0, 3));
    Put16(body, 0x61E, rng.Range(-0x800, 0x1800));
    Put16(body, 0x644, rng.Chance(2) ? rng.Range(-0x200, 0x200) : rng.S16(tier, 0x800));
    Put16(body, 0x646, rng.Chance(2) ? rng.Range(-0x200, 0x200) : rng.S16(tier, 0x800));
    Put16(body, 0x650, rng.Chance(4) ? 0 : rng.S16(tier, 0x4000));
    Put16(body, 0x652, rng.Chance(4) ? 0 : rng.S16(tier, 0x4000));
    Put32(body, 0x658, rng.Chance(2) ? rng.Range(0, 480000) : int32_t(rng.Next()));
    // attitude rows: consistent with the angles, or random
    if (rng.Chance(2)) {
        sim::BuildAttitudeMatrix(reinterpret_cast<int16_t*>(body + 0x668), reinterpret_cast<int16_t*>(body + 0x670), reinterpret_cast<int16_t*>(body + 0x678),
                                 Get<int16_t>(ram, address + 0x644), Get<int16_t>(ram, address + 0x646), b.heading);
    } else {
        for (uint32_t o = 0x668; o < 0x680; o += 2) Put16(body, o, rng.S16(tier, 0x1000));
    }
    for (uint32_t o = 0x680; o < 0x68C; o += 4) Put32(body, o, rng.Spread(tier, 0x2000000, 0x7FFFFFFF));
    if (rng.Chance(2)) {
        sim::BuildAttitudeMatrix(reinterpret_cast<int16_t*>(body + 0x68C), reinterpret_cast<int16_t*>(body + 0x694), reinterpret_cast<int16_t*>(body + 0x69C),
                                 rng.Range(-0x100, 0x100), rng.Range(-0x100, 0x100), b.heading);
    } else {
        for (uint32_t o = 0x68C; o < 0x6A4; o += 2) Put16(body, o, rng.S16(tier, 0x1000));
    }
    Put16(body, 0x6AE, int32_t(rng.Next() & 0xFFFF));
    Put8(body, 0x6B1, rng.Range(0, 1));
    Put8(body, 0x6B2, rng.Range(0, 11));
    Put8(body, 0x718, rng.Chance(3) ? 1 : 0);
    Put16(body, 0x6F4, rng.S16(tier, 0x800));
    Put16(body, 0x6F6, rng.S16(tier, 0x800));
    Put16(body, 0x6F8, int32_t(rng.Next() & 0xFFFF));
    Put8(body, 0x6FA, rng.Chance(3) ? rng.Range(0, 3) : 0);
    Put8(body, 0x6FB, rng.Chance(2) ? 0 : int32_t(rng.Next() & 0xFF));
    Put16(body, 0x738, int32_t(rng.Next() & 0xFFFF));
    Put16(body, 0x73E, rng.Chance(3) ? 30 : rng.S16(tier, 100));
    Put8(body, 0x719, int32_t(rng.Next() & 0xFF));
    for (uint32_t o = 0x724; o <= 0x734; o += 4) Put32(body, o, rng.Spread(tier, 0x400000, 0x7FFFFFFF));
    for (uint32_t o = 0x757; o <= 0x759; o++) Put8(body, o, int32_t(rng.Next() & 0xFF));
    for (uint32_t o = 0x762; o <= 0x763; o++) Put8(body, o, int32_t(rng.Next() & 0xFF));
    Put16(body, 0x73A, rng.S16(tier, 0x800));
    Put16(body, 0x73C, rng.S16(tier, 0x800));
    Put16(body, 0x748, rng.S16(tier, 0x800));
    Put16(body, 0x74A, rng.S16(tier, 0x1000));
    Put16(body, 0x74C, rng.S16(tier, 0x800));
    Put16(body, 0x74E, rng.S16(tier, 0x1000));
    Put16(body, 0x752, rng.S16(tier, 0x1000));
    Put8(body, 0x78D, int32_t(rng.Next() & 0xFF));
    Put16(body, 0x78E, rng.Chance(2) ? rng.Range(0, 1) : rng.Range(0, tier >= 2 ? 400 : 160));
    for (uint32_t wheel = 0; wheel < 4; wheel++) RandomiseWheel(rng, body + 0x460 + wheel * 0x68, tier, b.position[2]);
}

void RandomiseAllBodies(Rng& rng, uint8_t* ram, int tier, const Track* track, const sim::CourseSurface* course) {
    for (uint32_t car = 0; car < kCarCount; car++) RandomiseBody(rng, ram, BodyAddress(car), tier, track, course);
}

// Contact query records of every wheel, as 0x80043388 would leave them (positions next to the body).
void RandomiseContactRecords(Rng& rng, const uint8_t* ram, uint8_t* scratch, int tier) {
    for (uint32_t car = 0; car < kCarCount; car++) {
        const sim::CarBody& b = BodyAt(const_cast<uint8_t*>(ram), BodyAddress(car));
        for (uint32_t wheel = 0; wheel < 4; wheel++) {
            sim::ContactQuery& q = sim::ContactQueryAt(scratch, car, wheel);
            q.surface = uint8_t(rng.Next() & 15); // what the polygon flags can produce (the original indexes 8-entry tables with it)
            q.attribute1 = uint8_t(rng.Next());
            q.attribute2 = uint8_t(rng.Next());
            q.reserved03 = uint8_t(rng.Next());
            q.chunkIndex = uint16_t(b.chunkIndex);
            q.reserved06 = uint16_t(rng.Next());
            q.x = int32_t(uint32_t(b.position[0] + rng.Range(-0x2000, 0x2000)) << 4);
            q.height = int32_t(uint32_t(b.position[2] + rng.Range(-0x800, 0x800)) << 4);
            q.y = int32_t(uint32_t(b.position[1] + rng.Range(-0x2000, 0x2000)) << 4);
            q.surfaceHeight = rng.Chance(4) ? sim::kNoSurfaceHeight : tier >= 3 ? int32_t(rng.Next()) : int32_t(uint32_t(b.position[2] - rng.Range(0, 0x1000)) << 4);
        }
    }
}

int TierOf(size_t variant) { return variant % 4 == 0 ? 0 : variant % 4 == 1 ? 1 : variant % 4 == 2 ? 2 : 3; } // 0 = unmodified dump

// Like VerifyStateful, but the arguments come from `prepare` (per variant) and the return value can be checked.
struct CallArgs { uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0; };
template <typename Prepare, typename Native>
StatefulResult VerifyCalls(Guest& guest, const std::vector<uint8_t>& pristine, uint32_t function, size_t variants, Prepare prepare, Native native, bool compareReturn) {
    StatefulResult result;
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    for (size_t variant = 0; variant < variants; variant++) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
        const CallArgs args = prepare(guest.Ram(), guest.Scratch(), variant);
        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
        std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
        const uint32_t original = guest.Call(function, args.a0, args.a1, args.a2, args.a3);
        const uint32_t mine = native(ours.data(), ourScratch.data(), args);
        result.cases++;
        const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
        bool equal = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                     std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0 &&
                     std::memcmp(ourScratch.data(), guest.Scratch(), kScratchSize) == 0;
        if (compareReturn && original != mine) equal = false;
        if (!equal && result.mismatches++ < 3) {
            bool shown = false;
            if (compareReturn && original != mine) {
                std::printf("    MISMATCH variant %zu: return value original %d ours %d\n", variant, int32_t(original), int32_t(mine));
                shown = true;
            }
            for (uint32_t i = 0; i < Bus::kRamSize && !shown; i++)
                if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                    std::printf("    MISMATCH variant %zu: first differing byte at 0x%08X (a0 + 0x%X): original %02X ours %02X\n", variant, 0x80000000u + i,
                                i - (args.a0 & 0x1FFFFF), guest.Ram()[i], ours[i]);
                    shown = true;
                }
            for (uint32_t i = 0; i < kScratchSize && !shown; i++)
                if (ourScratch[i] != guest.Scratch()[i]) {
                    std::printf("    MISMATCH variant %zu: first differing scratchpad byte at 0x1F800000 + 0x%X: original %02X ours %02X\n", variant, i,
                                guest.Scratch()[i], ourScratch[i]);
                    shown = true;
                }
        }
    }
    return result;
}

std::vector<uint32_t> AllBodies() {
    std::vector<uint32_t> bodies;
    for (uint32_t car = 0; car < kCarCount; car++) bodies.push_back(BodyAddress(car));
    return bodies;
}

} // namespace

int VerifyGround(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt, const Track* track) {
    int failures = 0;
    Rng rng{mt};
    sim::CourseSurface course;
    if (track) course = BuildCourseSurface(pristine.data(), *track);
    const sim::CourseSurface* coursePtr = track ? &course : nullptr;

    // ---- the course height / surface query, record by record (0x80028830 on the guest's course object)
    if (track) {
        constexpr uint32_t kRecord = 0x1F800004u;
        size_t cases = 0, mismatches = 0, hits = 0;
        for (uint32_t c = 0; c < track->chunks.size(); c++)
            for (int variant = 0; variant < 24; variant++) {
                const int tier = variant < 6 ? 1 : variant < 18 ? 2 : 3;
                const RoadPoint p = RandomRoadPoint(rng, *track, &course, c, tier);
                sim::ContactQuery record{};
                record.surface = uint8_t(rng.Next());
                record.attribute1 = uint8_t(rng.Next());
                record.attribute2 = uint8_t(rng.Next());
                record.reserved03 = uint8_t(rng.Next());
                record.reserved06 = uint16_t(rng.Next());
                record.surfaceHeight = int32_t(rng.Next());
                const uint32_t n = uint32_t(track->chunks.size());
                record.chunkIndex = uint16_t(rng.Chance(2) ? c : rng.Chance(2) ? (c + n + uint32_t(rng.Range(-12, 12))) % n : rng.Next() % n);
                record.x = p.x;
                record.height = p.y;
                record.y = int32_t(0u - uint32_t(p.z));
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                std::memset(guest.Scratch(), 0, kScratchSize);
                std::memcpy(guest.Scratch() + (kRecord & 0x3FF), &record, sizeof(record));
                guest.Call(0x80028830u, D(kCourseObject), kRecord);
                sim::ContactQuery ours = record;
                sim::QueryContact(course, ours);
                const sim::ContactQuery& original = *reinterpret_cast<const sim::ContactQuery*>(guest.Scratch() + (kRecord & 0x3FF));
                cases++;
                if (original.surface != 7) hits++;
                if (std::memcmp(&original, &ours, sizeof(ours)) != 0 && mismatches++ < 5)
                    std::printf("    MISMATCH chunk %u variant %d: point (%d, %d, %d) hint %u: original chunk %u surface %u/%u/%u height %d; ours chunk %u surface %u/%u/%u height %d\n",
                                c, variant, p.x, p.y, p.z, record.chunkIndex, original.chunkIndex, original.surface, original.attribute1, original.attribute2,
                                original.surfaceHeight, ours.chunkIndex, ours.surface, ours.attribute1, ours.attribute2, ours.surfaceHeight);
            }
        std::printf("%-10s 0x80028830  %zu cases (%zu on the road), %zu mismatches  %s\n", "Contact", cases, hits, mismatches, mismatches ? "FAIL" : "ok");
        failures += mismatches ? 1 : 0;
    } else {
        std::puts("Contact    0x80028830  skipped (no disc image: the course is needed)");
    }

    // ---- wheel geometry
    StatefulResult r = VerifyStateful(
        guest, pristine, 0x8004323Cu, AllBodies(), 200,
        [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) { if (TierOf(variant)) RandomiseBody(rng, ram, object, TierOf(variant), track, coursePtr); },
        [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::UpdateWheelOffsets(BodyAt(ram, object)); });
    Report("WheelOffs", 0x8004323Cu, r.cases, r.mismatches, failures);
    r = VerifyStateful(
        guest, pristine, 0x800431A0u, AllBodies(), 200,
        [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) { if (TierOf(variant)) RandomiseBody(rng, ram, object, TierOf(variant), track, coursePtr); },
        [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::UpdateWheelHeightOffsets(BodyAt(ram, object)); });
    Report("WheelHOffs", 0x800431A0u, r.cases, r.mismatches, failures);
    r = VerifyStateful(
        guest, pristine, 0x8004335Cu, AllBodies(), 200,
        [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) { if (TierOf(variant)) RandomiseBody(rng, ram, object, TierOf(variant), track, coursePtr); },
        [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::UpdateWheelGeometry(BodyAt(ram, object)); });
    Report("WheelGeom", 0x8004335Cu, r.cases, r.mismatches, failures);

    // ---- pass 1: contact query records
    r = VerifyStateful(
        guest, pristine, 0x80043388u, {kCarBase}, 400,
        [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
            for (uint32_t i = 0; i < kScratchSize; i++) scratch[i] = uint8_t(rng.Next());
            if (TierOf(variant)) RandomiseAllBodies(rng, ram, TierOf(variant), track, coursePtr);
        },
        [&](uint8_t* ram, uint8_t* scratch, uint32_t) { sim::PrepareContactQueries(CarsOf(ram), int(kCarCount), scratch); }, kCarCount);
    Report("ContactPrep", 0x80043388u, r.cases, r.mismatches, failures);

    // ---- pass 2: the queries of all cars on the course
    if (track) {
        size_t hits = 0;
        r = VerifyStateful(
            guest, pristine, 0x800434DCu, {kCarBase}, 400,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
                if (TierOf(variant)) RandomiseAllBodies(rng, ram, TierOf(variant), track, coursePtr);
                RandomiseContactRecords(rng, ram, scratch, TierOf(variant));
            },
            [&](uint8_t* ram, uint8_t* scratch, uint32_t) {
                sim::QueryContacts(course, int(kCarCount), scratch);
                (void)ram;
                for (uint32_t car = 0; car < kCarCount; car++)
                    for (uint32_t wheel = 0; wheel < 4; wheel++) hits += sim::ContactQueryAt(scratch, car, wheel).surface != 7;
            },
            kCarCount);
        std::printf("%-10s 0x800434DC  %zu cases (%zu wheels on the road), %zu mismatches  %s\n", "ContactQry", r.cases, hits, r.mismatches, r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
    } else {
        std::puts("ContactQry 0x800434DC  skipped (no disc image: the course is needed)");
    }

    // ---- pass 3: applying the records
    {
        size_t resets = 0;
        r = VerifyStateful(
            guest, pristine, 0x80043578u, {kCarBase}, 600,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
                const int tier = TierOf(variant);
                if (tier) {
                    RandomiseAllBodies(rng, ram, tier, track, coursePtr);
                    RandomiseGlobals(rng, ram, tier);
                }
                RandomiseContactRecords(rng, ram, scratch, tier);
                if (rng.Chance(3)) // make the stuck-car paths reachable: all four records of a car agree
                    for (uint32_t car = 0; car < kCarCount; car++) {
                        const int32_t kind = rng.Range(0, 2);
                        for (uint32_t wheel = 0; wheel < 4; wheel++) {
                            sim::ContactQuery& q = sim::ContactQueryAt(scratch, car, wheel);
                            if (kind == 0) q.surfaceHeight = sim::kNoSurfaceHeight;
                            if (kind == 1) { q.attribute2 |= 2; q.surfaceHeight = 0; }
                            if (kind == 2) { q.surface = uint8_t(2 + rng.Range(0, 13)); q.surfaceHeight = 0; }
                        }
                    }
            },
            [&](uint8_t* ram, uint8_t* scratch, uint32_t) {
                sim::ApplyContacts(CarsOf(ram), int(kCarCount), scratch, ReadConstants(ram), ReadGlobals(ram));
                for (uint32_t car = 0; car < kCarCount; car++) resets += Get<uint8_t>(ram, BodyAddress(car) + 0x6FA) == 2;
            },
            kCarCount);
        std::printf("%-10s 0x80043578  %zu cases (%zu cars in reset state afterwards), %zu mismatches  %s\n", "ContactApp", r.cases, resets, r.mismatches, r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
    }

    // ---- the start-zone test of the stuck-car logic (pure)
    r = VerifyCalls(
        guest, pristine, 0x80036BDCu, 600,
        [&](uint8_t* ram, uint8_t*, size_t variant) {
            const int tier = TierOf(variant);
            if (tier) RandomiseGlobals(rng, ram, tier);
            const uint32_t list = Get<uint32_t>(ram, Get<uint32_t>(ram, D(kRaceStatePointer)) + 0x18);
            const int32_t slot = rng.Range(0, 1);
            if (rng.Chance(2)) Put32(At(ram, list), 0, rng.Range(6, 12)); // the list of the dump has 12 records
            if (rng.Chance(2) && tier >= 2) { // put the distance between two records so that both outcomes occur
                const int32_t count = Get<int32_t>(ram, list);
                if (count >= 6) {
                    const int32_t a = Get<int32_t>(ram, list + 4 + uint32_t(slot + 1) * 0x28 + 0x14), b = Get<int32_t>(ram, list + 4 + uint32_t(count - 2 - slot) * 0x28 + 0x14);
                    const int32_t length = Get<int32_t>(ram, Get<uint32_t>(ram, D(kCourseObject) + 0xB544));
                    return CallArgs{uint32_t(int32_t(uint32_t(a) + (uint32_t(b) - uint32_t(a)) / 2) - (rng.Chance(2) ? length : 0)), uint32_t(slot), 0, 0};
                }
            }
            return CallArgs{uint32_t(tier ? rng.Spread(tier, 300000000, 0x7FFFFFFF) : 200000000), uint32_t(slot), 0, 0};
        },
        [&](uint8_t* ram, uint8_t*, const CallArgs& args) { return uint32_t(sim::InStartZone(int32_t(args.a0), int32_t(args.a1), ReadGlobals(ram))); }, true);
    Report("StartZone", 0x80036BDCu, r.cases, r.mismatches, failures);

    // ---- spring / damper force of one wheel: 0x800438F0(body, wheel, travel, axle block)
    r = VerifyCalls(
        guest, pristine, 0x800438F0u, 1200,
        [&](uint8_t* ram, uint8_t*, size_t variant) {
            const int tier = TierOf(variant);
            const uint32_t car = uint32_t(variant % kCarCount), wheel = uint32_t(variant / kCarCount) % 4;
            const uint32_t body = BodyAddress(car);
            if (tier) RandomiseBody(rng, ram, body, tier, track, coursePtr);
            const int32_t travel = tier == 0 ? rng.Range(-2000, 500) : tier == 1 ? rng.Range(-3000, 1000) : rng.Spread(tier, 0x8000, 0x7FFFFFFF);
            return CallArgs{body, body + 0x460 + wheel * 0x68, uint32_t(travel), body + sim::kAxleSuspensionOffset + (wheel >> 1) * 0x34};
        },
        [&](uint8_t* ram, uint8_t*, const CallArgs& args) {
            sim::CarBody& body = BodyAt(ram, args.a0);
            const size_t wheel = (args.a1 - args.a0 - 0x460) / 0x68;
            return uint32_t(sim::SpringDamperForce(body, body.wheels[wheel], int32_t(args.a2), sim::AxleSuspensionOf(body, wheel >> 1)));
        },
        true);
    Report("SpringDamp", 0x800438F0u, r.cases, r.mismatches, failures);

    // ---- reset lift: 0x800357C8(state, &phase)
    r = VerifyCalls(
        guest, pristine, 0x800357C8u, 600,
        [&](uint8_t* ram, uint8_t*, size_t variant) {
            const int tier = TierOf(variant);
            if (tier) RandomiseGlobals(rng, ram, tier);
            Put32(At(ram, kFreeRam), 0, int32_t(rng.Next()));
            return CallArgs{uint32_t(tier >= 2 ? rng.Range(0, 0xFFFF) : rng.Range(0, 200)), kFreeRam, 0, 0};
        },
        [&](uint8_t* ram, uint8_t*, const CallArgs& args) {
            int32_t phase = 0;
            const int32_t lift = sim::ResetLift(int32_t(args.a0), Get<int32_t>(ram, D(kRateAddress)), phase);
            Put32(At(ram, kFreeRam), 0, phase);
            return uint32_t(lift);
        },
        true);
    Report("ResetLift", 0x800357C8u, r.cases, r.mismatches, failures);

    // ---- lift to the ground: 0x80043AA4(body, lift)
    r = VerifyCalls(
        guest, pristine, 0x80043AA4u, 600,
        [&](uint8_t* ram, uint8_t*, size_t variant) {
            const int tier = TierOf(variant);
            const uint32_t body = BodyAddress(uint32_t(variant % kCarCount));
            if (tier) RandomiseBody(rng, ram, body, tier, track, coursePtr);
            if (rng.Chance(2)) Put32(At(ram, body + 0x688), 0, Get<int32_t>(ram, body + 0x664) - rng.Range(-0x800, 0x800));
            return CallArgs{body, uint32_t(tier >= 2 ? rng.Spread(tier, 0x8000, 0x7FFFFFFF) : rng.Range(0, 0x4CC)), 0, 0};
        },
        [&](uint8_t* ram, uint8_t*, const CallArgs& args) { return uint32_t(sim::LiftBodyToGround(BodyAt(ram, args.a0), int32_t(args.a1))); }, true);
    Report("LiftBody", 0x80043AA4u, r.cases, r.mismatches, failures);

    // ---- pass 4a: the suspension
    {
        size_t lifted = 0, animating = 0;
        r = VerifyStateful(
            guest, pristine, 0x80043AE0u, {kCarBase}, 600,
            [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
                const int tier = TierOf(variant);
                for (uint32_t i = 0; i < kScratchSize; i++) scratch[i] = uint8_t(rng.Next());
                if (tier) {
                    RandomiseAllBodies(rng, ram, tier, track, coursePtr);
                    RandomiseGlobals(rng, ram, tier);
                }
            },
            [&](uint8_t* ram, uint8_t* scratch, uint32_t) {
                for (uint32_t car = 0; car < kCarCount; car++) animating += Get<uint16_t>(ram, BodyAddress(car) + 0x78E) >= 2;
                sim::SuspensionPass(CarsOf(ram), int(kCarCount), scratch, ReadGlobals(ram));
                for (uint32_t car = 0; car < kCarCount; car++) lifted += (Get<uint8_t>(ram, BodyAddress(car) + 0x719) & 0xF) != 0;
            },
            kCarCount);
        std::printf("%-10s 0x80043AE0  %zu cases (%zu cars lifted, %zu in the reset animation), %zu mismatches  %s\n", "Suspension", r.cases, lifted, animating,
                    r.mismatches, r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
    }

    // ---- pass 4b: wheel effects (per body, step time in the scratchpad)
    r = VerifyStateful(
        guest, pristine, 0x800426F0u, AllBodies(), 200,
        [&](uint8_t* ram, uint8_t* scratch, uint32_t object, size_t variant) {
            const int tier = TierOf(variant);
            if (tier) {
                RandomiseBody(rng, ram, object, tier, track, coursePtr);
                RandomiseGlobals(rng, ram, tier);
            }
            Put32(scratch, 0, tier >= 2 ? rng.Spread(tier, 0x7FFF, 0x7FFFFFFF) : Get<int16_t>(ram, object + 0x6FE));
        },
        [&](uint8_t* ram, uint8_t* scratch, uint32_t object) { sim::UpdateWheelEffects(BodyAt(ram, object), Get<int32_t>(scratch, 0), ReadConstants(ram), ReadGlobals(ram)); });
    Report("Effects", 0x800426F0u, r.cases, r.mismatches, failures);

    // ---- attitude rows: 0x80044EA4(row0, row1, row2, pitch, roll, heading) - the last two on the stack
    r = VerifyCalls(
        guest, pristine, 0x80044EA4u, 1200,
        [&](uint8_t* ram, uint8_t*, size_t variant) {
            const int tier = TierOf(variant);
            const uint32_t body = BodyAddress(uint32_t(variant % kCarCount));
            if (tier) RandomiseBody(rng, ram, body, tier, track, coursePtr);
            const int32_t pitch = tier >= 2 ? int32_t(rng.Next()) : rng.Range(-0x200, 0x200), roll = tier >= 2 ? int32_t(rng.Next()) : rng.Range(-0x200, 0x200);
            Put32(At(ram, kStack + 0x10), 0, roll);
            Put32(At(ram, kStack + 0x14), 0, tier >= 2 ? int32_t(rng.Next()) : int32_t(Get<int16_t>(ram, body + 0x648)));
            return CallArgs{body + 0x668, body + 0x670, body + 0x678, uint32_t(pitch)};
        },
        [&](uint8_t* ram, uint8_t*, const CallArgs& args) {
            sim::BuildAttitudeMatrix(reinterpret_cast<int16_t*>(At(ram, args.a0)), reinterpret_cast<int16_t*>(At(ram, args.a1)), reinterpret_cast<int16_t*>(At(ram, args.a2)),
                                     int32_t(args.a3), Get<int32_t>(ram, kStack + 0x10), Get<int32_t>(ram, kStack + 0x14));
            return 0u;
        },
        false);
    Report("Attitude", 0x80044EA4u, r.cases, r.mismatches, failures);

    // ---- visual pose, body-frame speeds
    r = VerifyStateful(
        guest, pristine, 0x8003E7ECu, AllBodies(), 200,
        [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) { if (TierOf(variant)) RandomiseBody(rng, ram, object, TierOf(variant), track, coursePtr); },
        [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::UpdateVisualPose(BodyAt(ram, object)); });
    Report("VisualPose", 0x8003E7ECu, r.cases, r.mismatches, failures);
    r = VerifyStateful(
        guest, pristine, 0x800304DCu, AllBodies(), 200,
        [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) { if (TierOf(variant)) RandomiseBody(rng, ram, object, TierOf(variant), track, coursePtr); },
        [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::UpdateBodyFrameSpeeds(BodyAt(ram, object)); });
    Report("FrameSpeed", 0x800304DCu, r.cases, r.mismatches, failures);
    r = VerifyStateful(
        guest, pristine, 0x800306C0u, {kCarBase}, 400,
        [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
            const int tier = TierOf(variant);
            if (tier) RandomiseAllBodies(rng, ram, tier, track, coursePtr);
            Put32(scratch, 0, tier >= 2 ? rng.Spread(tier, 0x7FFF, 0x7FFFFFFF) : 2184);
        },
        [&](uint8_t* ram, uint8_t* scratch, uint32_t) { sim::UpdateWheelRotation(CarsOf(ram), int(kCarCount), Get<int32_t>(scratch, 0)); }, kCarCount);
    Report("WheelRot", 0x800306C0u, r.cases, r.mismatches, failures);

    // ---- pass 4 as a whole
    r = VerifyStateful(
        guest, pristine, 0x8003E8E4u, {kCarBase}, 400,
        [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
            const int tier = TierOf(variant);
            for (uint32_t i = 0; i < kScratchSize; i++) scratch[i] = uint8_t(rng.Next());
            if (tier) {
                RandomiseAllBodies(rng, ram, tier, track, coursePtr);
                RandomiseGlobals(rng, ram, tier);
            }
        },
        [&](uint8_t* ram, uint8_t* scratch, uint32_t) { sim::GroundPass(CarsOf(ram), int(kCarCount), scratch, ReadConstants(ram), ReadGlobals(ram)); }, kCarCount);
    Report("GroundPass", 0x8003E8E4u, r.cases, r.mismatches, failures);

    return failures;
}

} // namespace gt2::verify
