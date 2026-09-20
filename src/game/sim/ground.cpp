#include "game/sim/ground.h"

#include <cstring>

#include "game/sim/fixed.h"
#include "game/sim/drive_shafts.h"
#include "game/sim/trig.h"
#include "game/sim/tyres.h"

namespace gt2::sim {
namespace {

constexpr int32_t kGravityPerSecond = 0x9CCD;     // 9.8 m/s^2 in 1/4096 m/s^2
constexpr uint32_t kMaxDirtiness = 480000;
constexpr int32_t kRadiansToAngle = 0x28C;        // 4096 / (2 pi)
constexpr int32_t kSpeedReadoutScale = 0xDFBDD;   // speed (1/4096 m/s) * this >> 24 = readout units
constexpr int32_t kYawArmScale = 0x6488;          // yaw rate (angle units << 7 / s) -> m/s per m of arm (2 pi * 4096 / 4096 << 7)
constexpr int32_t kSlipScaleGain = 0x93, kSlipScaleFullSpeed = 0x1BC88;
constexpr int32_t kResetLiftHeight = 0x4CC;       // 0.3 m
constexpr int32_t kResetHeadingRate = 0x155;      // angle units per second of the reset heading correction
constexpr int32_t kViewPitchGain = 0x14000, kViewPitchDamping = 0xA000;
constexpr int32_t kViewFollowMinSpeed = 0x8E4;    // 2276 = 0.56 m/s: below this the view follows the heading

// The scratchpad work area is addressed by the named offsets of ground.h (kSuspensionWork*).
int32_t& ScratchS32(uint8_t* scratch, uint32_t offset) { return *reinterpret_cast<int32_t*>(scratch + offset); }
int32_t ScratchS32(const uint8_t* scratch, uint32_t offset) {
    int32_t v;
    std::memcpy(&v, scratch + offset, sizeof v);
    return v;
}

inline int32_t Wrap(uint32_t v) { return int32_t(v); }
inline int32_t Add(int32_t a, int32_t b) { return Wrap(uint32_t(a) + uint32_t(b)); }
inline int32_t Sub(int32_t a, int32_t b) { return Wrap(uint32_t(a) - uint32_t(b)); }
inline int32_t Neg(int32_t a) { return Wrap(0u - uint32_t(a)); }
inline int32_t MulLow(int32_t a, int32_t b) { return Wrap(uint32_t(a) * uint32_t(b)); } // MIPS mult + mflo
inline int32_t Half(int32_t a) { return Div(a, 2); }                                     // the compiler's signed / 2
inline int16_t AddS16(int16_t a, int32_t b) { return int16_t(uint32_t(uint16_t(a)) + uint32_t(b)); }

// The GTE's NCLIP on SXY0 = P, SXY1 = A, SXY2 = B: the doubled signed area of the triangle, MAC0 keeps the low
// 32 bits of the 64-bit sum (the interpreter's Gte::SetMac0).
inline int32_t Nclip(int16_t px, int16_t py, int16_t ax, int16_t ay, int16_t bx, int16_t by) {
    const int64_t value = int64_t(px) * ay + int64_t(ax) * by + int64_t(bx) * py - int64_t(px) * by - int64_t(ax) * py - int64_t(bx) * ay;
    return int32_t(uint32_t(uint64_t(value)));
}

} // namespace

// ================================================================ course surface lookup

bool PointPastChunkOrigin(const TrackChunk& chunk, const int32_t point[3]) {
    int64_t dot = 0;
    for (uint32_t i = 0; i < 3; i++) {
        const int32_t delta = Sub(point[i], chunk.origin[i]);
        dot += int64_t(chunk.direction[i]) * delta;
    }
    // 0x80081A78 returns (high << 20) | (low >> 12); the caller tests its sign = bit 43 of the sum.
    return ((uint64_t(dot) >> 43) & 1u) == 0;
}

uint32_t FindChunkAlongCourse(const Track& track, uint32_t hint, const int32_t point[3]) {
    if (hint >= track.chunks.size()) return hint; // the original would read outside the chunk pointer table
    uint32_t index = hint;
    // Backwards until the point is past the chunk's start plane.
    while (!PointPastChunkOrigin(track.chunks[index], point)) index = track.chunks[index].prev;
    // Forwards while the point is also past the next chunk's start plane (stop before wrapping around).
    const uint32_t start = index;
    for (;;) {
        const uint32_t next = track.chunks[index].next;
        if (!PointPastChunkOrigin(track.chunks[next], point) || next == start) return index;
        index = next;
    }
}

int32_t FindRoadPolygon(const TrackChunk& chunk, const SurfaceGrid& grid, int32_t localX, int32_t localZ) {
    const uint32_t cellX = uint32_t(Sub(localX, grid.originX) >> (grid.shiftX & 31));
    const uint32_t cellZ = uint32_t(Sub(localZ, grid.originZ) >> (grid.shiftZ & 31));
    if (cellX >= 4 || cellZ >= 4) return -1;
    const std::vector<uint16_t>& list = grid.cells[cellZ * 4 + cellX];
    const int16_t px = int16_t(localX), pz = int16_t(localZ); // SXY0 holds 16-bit coordinates
    for (uint16_t index : list) {
        const TrackPolygon& polygon = chunk.road.polygons[index];
        const std::vector<TrackVertex>& vertices = chunk.road.vertices;
        const TrackVertex& v0 = vertices[polygon.vertex[0]];
        const TrackVertex& v1 = vertices[polygon.vertex[1]];
        const TrackVertex& v2 = vertices[polygon.vertex[2]];
        const TrackVertex& v3 = polygon.IsQuad() ? vertices[polygon.vertex[3]] : v2; // word0 bit 30 = quad
        const int32_t side01 = Nclip(px, pz, v0.x, v0.y, v1.x, v1.y);
        const int32_t side21 = Nclip(px, pz, v2.x, v2.y, v1.x, v1.y);
        const int32_t side23 = Nclip(px, pz, v2.x, v2.y, v3.x, v3.y);
        const int32_t side03 = Nclip(px, pz, v0.x, v0.y, v3.x, v3.y);
        // Inside when side01 <= 0, side21 >= 0, side23 <= 0, side03 >= 0 (the OR of the sign bits of -a | b | -c | d).
        const uint32_t signs = uint32_t(Neg(side01)) | uint32_t(side21) | uint32_t(Neg(side23)) | uint32_t(side03);
        if (int32_t(signs) >= 0) return index;
    }
    return -1;
}

int32_t FindRoadPolygonAt(const TrackChunk& chunk, const SurfaceGrid& grid, const int32_t point[3]) {
    const uint32_t cellX = uint32_t(chunk.centre[0]) & 0xFFC00000u, cellZ = uint32_t(chunk.centre[2]) & 0xFFC00000u;
    return FindRoadPolygon(chunk, grid, Wrap(uint32_t(point[0]) - cellX) >> 10, Wrap(uint32_t(point[2]) - cellZ) >> 10);
}

int32_t InterpolateRoadHeight(const TrackChunk& chunk, const TrackPolygon& polygon, const int32_t point[3]) {
    const uint32_t cellX = uint32_t(chunk.centre[0]) & 0xFFC00000u, cellY = uint32_t(chunk.centre[1]) & 0xFFC00000u,
                   cellZ = uint32_t(chunk.centre[2]) & 0xFFC00000u;
    const int32_t localX16 = Wrap(uint32_t(point[0]) - cellX), localZ16 = Wrap(uint32_t(point[2]) - cellZ);
    const int32_t localX = localX16 >> 10, localZ = localZ16 >> 10;
    const std::vector<TrackVertex>& vertices = chunk.road.vertices;
    const TrackVertex& v0 = vertices[polygon.vertex[0]];
    const TrackVertex& v1 = vertices[polygon.vertex[1]];
    const TrackVertex& v2 = vertices[polygon.vertex[2]];
    TrackVertex base = v2, a = v0, b = v1; // triangle: plane through (v2, v0, v1)
    if (polygon.IsQuad()) {
        const TrackVertex& v3 = vertices[polygon.vertex[3]];
        const TrackVertex centroid{int16_t((v0.x + v1.x + v2.x + v3.x) >> 2), int16_t((v0.y + v1.y + v2.y + v3.y) >> 2),
                                   int16_t((v0.z + v1.z + v2.z + v3.z) >> 2)};
        const int32_t dx = localX - centroid.x, dz = localZ - centroid.y;
        // Which side of the spoke centroid -> corner is the point on? One bit per corner.
        uint32_t bits = 0;
        const TrackVertex* corners[4] = {&v0, &v1, &v2, &v3};
        for (uint32_t i = 0; i < 4; i++) {
            const int32_t cross = Sub(MulLow(dx, corners[i]->y - centroid.y), MulLow(dz, corners[i]->x - centroid.x));
            if (cross < 0) bits |= 1u << i;
        }
        base = centroid;
        switch (bits) { // jump table at 0x8002F1C4
        case 1: case 3: case 7: a = v3; b = v0; break;
        case 4: case 12: case 13: a = v1; b = v2; break;
        case 8: case 9: case 11: a = v2; b = v3; break;
        default: a = v0; b = v1; break;
        }
    }
    const int32_t ax = a.x - base.x, ay = a.y - base.y, az = a.z - base.z;
    const int32_t bx = b.x - base.x, by = b.y - base.y, bz = b.z - base.z;
    const int32_t denominator = Sub(MulLow(ax, by), MulLow(ay, bx)); // vertical component of the plane normal
    int32_t height = Wrap(uint32_t(base.z) << 10);                   // 1/64 m -> 16.16
    if (denominator != 0) {
        const int32_t dx16 = Sub(localX16, Wrap(uint32_t(base.x) << 10)), dz16 = Sub(localZ16, Wrap(uint32_t(base.y) << 10));
        const int32_t normalX = Sub(MulLow(az, by), MulLow(ay, bz)), normalZ = Sub(MulLow(ax, bz), MulLow(az, bx));
        const int64_t numerator = int64_t(uint64_t(int64_t(dx16) * normalX) + uint64_t(int64_t(dz16) * normalZ));
        height = Add(height, int32_t(uint64_t(Div64(numerator, denominator))));
    }
    return Wrap(cellY + uint32_t(height));
}

void QueryRoadSurface(const CourseSurface& course, uint32_t chunkIndex, const int32_t point[3], ContactQuery& query) {
    const Track& track = *course.track;
    uint32_t chunk = chunkIndex;
    int32_t polygon = FindRoadPolygonAt(track.chunks[chunk], course.grids[chunk], point);
    if (polygon < 0) {
        uint32_t back = track.chunks[chunk].prev, forward = track.chunks[chunk].next;
        for (int step = 0; step < 8 && polygon < 0; step++) {
            polygon = FindRoadPolygonAt(track.chunks[back], course.grids[back], point);
            if (polygon >= 0) { chunk = back; break; }
            polygon = FindRoadPolygonAt(track.chunks[forward], course.grids[forward], point);
            if (polygon >= 0) { chunk = forward; break; }
            back = track.chunks[back].prev;
            forward = track.chunks[forward].next;
        }
    }
    if (polygon < 0) {
        query.surface = 7;
        query.attribute1 = 4;
        query.attribute2 = 0;
        query.reserved03 = 0;
        query.surfaceHeight = kNoSurfaceHeight;
        return;
    }
    const TrackPolygon& p = track.chunks[chunk].road.polygons[size_t(polygon)];
    query.surface = uint8_t((p.flags >> 9) & 0xF);       // polygon word1 >> 28 (flags = word1 >> 19)
    query.attribute1 = uint8_t((p.flags >> 4) & 3);
    query.attribute2 = uint8_t((p.flags >> 6) & 3);
    query.reserved03 = 0;
    query.surfaceHeight = InterpolateRoadHeight(track.chunks[chunk], p, point);
}

void QueryContact(const CourseSurface& course, ContactQuery& query) {
    const int32_t point[3] = {query.x, query.height, Neg(query.y)}; // simulation y -> course Z
    const uint32_t chunk = FindChunkAlongCourse(*course.track, query.chunkIndex, point);
    query.chunkIndex = uint16_t(chunk);
    QueryRoadSurface(course, chunk, point, query);
}

// ================================================================ wheel geometry

void UpdateWheelOffsets(CarBody& body) { // 0x8004323C
    const int32_t fx = body.basis[kRowForward][0], fy = body.basis[kRowForward][1];
    const int32_t lx = body.basis[kRowLateral][0], ly = body.basis[kRowLateral][1];
    const int32_t front = body.axleOffset[0], rear = body.axleOffset[1];
    const int32_t frontTrack = body.halfTrack[0], rearTrack = body.halfTrack[1];
    const int16_t frontX = int16_t(Mul12(fx, front)), frontY = int16_t(Mul12(fy, front));
    const int16_t rearX = int16_t(Mul12(fx, rear)), rearY = int16_t(Mul12(fy, rear));
    const int16_t frontTrackX = int16_t(Mul12(lx, frontTrack)), frontTrackY = int16_t(Mul12(ly, frontTrack));
    const int16_t rearTrackX = int16_t(Mul12(lx, rearTrack)), rearTrackY = int16_t(Mul12(ly, rearTrack));
    body.wheels[0].offsetX = int16_t(frontX - frontTrackX);
    body.wheels[0].offsetY = int16_t(frontY - frontTrackY);
    body.wheels[2].offsetX = int16_t(-rearX - rearTrackX);
    body.wheels[1].offsetX = int16_t(frontTrackX + frontX);
    body.wheels[1].offsetY = int16_t(frontTrackY + frontY);
    body.wheels[2].offsetY = int16_t(-rearY - rearTrackY);
    body.wheels[3].offsetX = int16_t(rearTrackX - rearX);
    body.wheels[3].offsetY = int16_t(rearTrackY - rearY);
}

void UpdateWheelHeightOffsets(CarBody& body) { // 0x800431A0
    const int32_t forwardUp = body.basis[kRowForward][2], lateralUp = body.basis[kRowLateral][2];
    const int16_t front = int16_t(Mul12(forwardUp, body.axleOffset[0]));
    const int16_t rear = int16_t(Mul12(forwardUp, body.axleOffset[1]));
    const int16_t frontTrack = int16_t(Mul12(lateralUp, body.halfTrack[0]));
    const int16_t rearTrack = int16_t(Mul12(lateralUp, body.halfTrack[1]));
    body.wheels[0].offsetHeight = int16_t(front - frontTrack);
    body.wheels[1].offsetHeight = int16_t(frontTrack + front);
    body.wheels[2].offsetHeight = int16_t(-rear - rearTrack);
    body.wheels[3].offsetHeight = int16_t(rearTrack - rear);
}

void UpdateWheelGeometry(CarBody& body) { // 0x8004335C
    UpdateWheelHeightOffsets(body);
    UpdateWheelOffsets(body);
}

// ================================================================ contact passes

void PrepareContactQueries(Car* cars, int count, uint8_t* scratch) { // 0x80043388
    for (int car = 0; car < count; car++) UpdateWheelOffsets(cars[car].body);
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        for (uint32_t wheel = 0; wheel < 4; wheel++) {
            Wheel& w = body.wheels[wheel];
            int32_t factor = Mul12Floor(ApproxLength(w.contactForwardSpeed, w.contactLateralSpeed), 0x52);
            if (factor > 0x1000) factor = 0x1000;
            w.speedFactor = int16_t(factor);
            ContactQuery& query = ContactQueryAt(scratch, size_t(car), wheel);
            query.x = Wrap(uint32_t(Add(body.position[0], w.offsetX)) << 4);
            query.height = Wrap(uint32_t(Add(body.position[2], w.offsetHeight)) << 4);
            query.y = Wrap(uint32_t(Add(body.position[1], w.offsetY)) << 4);
            query.chunkIndex = uint16_t(body.chunkIndex);
        }
    }
}

void QueryContacts(const CourseSurface& course, int count, uint8_t* scratch) { // 0x800434DC
    for (int car = 0; car < count; car++)
        for (uint32_t wheel = 0; wheel < 4; wheel++) QueryContact(course, ContactQueryAt(scratch, size_t(car), wheel));
}

namespace {

// 0x80041A28: the car is a regular participant of a race on a paved course.
bool RaceParticipant(const CarBody& body, const GroundGlobals& globals) {
    if (body.controlClass != 0 || body.raceState != 0) return false;
    if (globals.raceMode != 3 && globals.raceMode != 6) return false;
    return !globals.dirtCourse;
}

} // namespace

bool InStartZone(int32_t distance, int32_t slot, const GroundGlobals& globals) { // 0x80036BDC
    const RaceGridInfo& grid = globals.grid;
    if (grid.count < 6) return false; // (count 0 also stands for "no list")
    if (distance < Half(globals.courseLength)) distance = Add(distance, globals.courseLength);
    const int32_t first = grid.distance.at(size_t(slot + 1)), last = grid.distance.at(size_t(grid.count - 2 - slot));
    return first < distance && distance < last;
}

void ApplyContacts(Car* cars, int count, uint8_t* scratch, const GroundConstants& constants, const GroundGlobals& globals) { // 0x80043578
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        body.chunkIndex = int32_t(uint32_t(ContactQueryAt(scratch, size_t(car), 0).chunkIndex));
        int noSurface = 0, offRoad = 0, loose = 0;
        for (uint32_t wheel = 0; wheel < 4; wheel++) {
            const ContactQuery& query = ContactQueryAt(scratch, size_t(car), wheel);
            Wheel& w = body.wheels[wheel];
            const int32_t height = query.surfaceHeight;
            if (height == kNoSurfaceHeight) {
                noSurface++;
                w.roughness = 0;
                continue;
            }
            const int32_t previous = w.surfaceHeight;
            w.surfaceHeight = height;
            w.surfaceHeightAvg = Add(height, previous) >> 5; // mean of the two, 16.16 -> 1/4096
            w.surface = query.surface;
            w.surfaceAttribute = query.attribute1;
            if (query.attribute2 & 2) offRoad++;
            if (w.surface > 1) loose++;
            if (w.surface == 6) {
                w.surface = 5;
                noSurface++;
            }
            const uint32_t surface = w.surface & 15; // 0..15 from the polygon; the tables overlap beyond 7
            const int32_t amplitude = constants.roughnessAmplitude[surface];
            if (amplitude == 0) {
                w.roughness = 0;
                continue;
            }
            const int32_t frequency = constants.roughnessFrequency[surface];
            const int32_t phaseX = Mul12Floor(frequency, query.x), phaseY = Mul12Floor(frequency, query.y);
            int32_t bump = Sin(uint32_t(phaseX)) + Cos(uint32_t(phaseY));
            if (constants.roughnessSpeedScaled[surface] != 0) bump = Mul12(bump, w.speedFactor);
            w.roughness = int16_t(MulLow(bump, amplitude) >> 12);
        }
        uint8_t flags = body.flags78D;
        const bool raceModeWithReset = globals.raceMode == 3 || globals.raceMode == 6; // 0x80041AB8 negated
        flags = (offRoad != 0 && !raceModeWithReset) ? uint8_t(flags | 0x10) : uint8_t(flags & 0xEF);
        body.flags78D = flags;
        if (RaceParticipant(body, globals) && body.resetState == 0) {
            bool stuck = false;
            if (noSurface == 4) {
                body.resetCounter = 0xFF;
                stuck = true;
            } else if ((offRoad == 4 && InStartZone(body.courseDistance, 0, globals)) || (globals.raceMode == 3 && loose == 4)) {
                body.resetCounter = uint8_t(body.resetCounter + 1);
                stuck = true;
            }
            if (!stuck) {
                body.resetCounter = 0;
            } else if (body.resetCounter != 0) {
                Car& target = cars[body.carIndex]; // 0x800156B8(slot, 2): car + 0xA92 = 120, car + 0xA8D = 2
                target.body.hudTimer3 = 120;
                target.body.hudMessageKind = 2;
                body.resetState = 2;
            }
        }
    }
}

// ================================================================ suspension

int32_t SpringDamperForce(const CarBody& body, Wheel& wheel, int32_t height, const AxleSuspension& axle) { // 0x800438F0
    if (axle.droopLimit < height) { // in the air
        wheel.travel = axle.droopLimit;
        wheel.load = 0;
        wheel.damperForce = 0;
        wheel.springForce = -int32_t(axle.staticLoad);
        return -int32_t(axle.staticLoad);
    }
    const int32_t travel = height < axle.travelFloor ? axle.travelFloor : height;
    int32_t spring = Neg(Mul12(travel, axle.springRate));
    if (travel < axle.bumpStopStart) {
        const int32_t past = int16_t(uint16_t(axle.bumpStopStart) - uint16_t(travel));
        spring = Add(spring, Mul12Floor(MulLow(past, past) >> 12, axle.bumpStopRate));
    }
    const int32_t velocity = Mul12Wide(body.stepRate12, Sub(height, wheel.travel));
    const int32_t previousVelocity = wheel.travelVelocity;
    wheel.travelVelocity = int16_t(velocity);
    int32_t mean = Half(Add(velocity, previousVelocity));
    int32_t damper;
    if (mean < 0) {
        if (mean < -0x1000) mean = -0x1000;
        const int32_t root = SquareRoot(Sub(Neg(axle.bumpKnee), mean), 6);
        damper = Add(axle.bumpBase, Mul12Wide(axle.bumpGain, root));
    } else {
        if (mean > 0x1000) mean = 0x1000;
        const int32_t root = SquareRoot(Sub(mean, axle.reboundKnee), 6);
        damper = Sub(Neg(Mul12Wide(axle.reboundGain, root)), axle.reboundBase);
    }
    wheel.springForce = spring;
    wheel.travel = int16_t(travel);
    wheel.damperForce = damper;
    const int32_t total = Add(spring, damper);
    const int32_t load = Add(total, axle.staticLoad);
    wheel.load = load < 0 ? 0 : load;
    return total;
}

int32_t ResetLift(int32_t state, int32_t rate, int32_t& phase) { // 0x800357C8
    const int32_t halfRate = Half(rate);
    if (state < rate * 5 - halfRate) {
        if (state < halfRate) {
            phase = 0;
            return 0;
        }
        phase = 2;
        return kResetLiftHeight;
    }
    phase = 1;
    return Div(MulLow(rate * 5 - state, kResetLiftHeight), halfRate);
}

bool LiftBodyToGround(CarBody& body, int32_t lift) { // 0x80043AA4
    const int32_t target = Add(body.visualPosition[2], lift);
    if (target < body.position[2]) return false;
    body.position[2] = target;
    body.rollRate = 0;
    body.pitchRate = 0;
    body.roll = 0;
    body.pitch = 0;
    body.velocity[2] = 0;
    return true;
}

namespace {

// Wheel travel of one car into its work block (0x80043AE0, first pass and the re-evaluation after the lift):
// travel = (body height + wheel height offset - surface height - roughness) * ratio + (ride reference - radius).
void ComputeTravel(CarBody& body, uint8_t* work, int32_t ratio) {
    int32_t rideReference[4];
    for (uint32_t axle = 0; axle < 2; axle++)
        rideReference[axle * 2] = rideReference[axle * 2 + 1] = AxleSuspensionOf(body, axle).rideReference - body.wheelRadius[axle];
    for (uint32_t wheel = 0; wheel < 4; wheel++) {
        const Wheel& w = body.wheels[wheel];
        const int32_t clearance = Sub(Sub(Add(body.position[2], w.offsetHeight), w.surfaceHeightAvg), w.roughness);
        ScratchS32(work, kSuspensionWorkTravel + uint32_t(wheel) * 4) = Add(Mul12(clearance, ratio), rideReference[wheel]);
    }
}

uint32_t TravelFloorMask(const CarBody& body, const uint8_t* work) {
    uint32_t mask = 0;
    for (uint32_t wheel = 0; wheel < 4; wheel++)
        if (ScratchS32(work, kSuspensionWorkTravel + uint32_t(wheel) * 4) < AxleSuspensionOf(body, wheel >> 1).travelFloor)
            mask |= 1u << wheel;
    return mask;
}

// Rate integration with the zero-crossing stop and the +-0x4000 clamp of 0x80043AE0.
int16_t IntegrateRate(int16_t rate, int32_t delta) {
    const int32_t before = rate, after = Add(before, delta);
    if ((before >= 1 && after <= -1) || (before <= -1 && after >= 1)) return 0;
    if (after >= 0x4000) return 0x3FFF;
    if (after < -0x4000) return -0x4000;
    return int16_t(after);
}

// `angleField` / `rateField` are the body's pitch / pitchRate or roll / rollRate.
void IntegrateAngle(int16_t& angleField, int16_t& rateField, int32_t stepTime) {
    const int16_t angle = AddS16(angleField, Mul16ShiftWide(stepTime, rateField, 4));
    angleField = angle;
    if (angle < -0x200 || angle > 0x200) {
        angleField = int16_t(angle < -0x200 ? -0x200 : 0x200);
        rateField = 0;
    }
}

void AttitudeRows(CarBody& body) {
    BuildAttitudeMatrix(body.basis[kRowForward], body.basis[kRowLateral], body.basis[kRowUp], body.pitch, body.roll, body.heading);
}

// Penetration half-depth of the lower of two wheels of an axle; adds the atan of the difference to `angle`
// (the roll / pitch correction of the lift in 0x80043AE0). `leftWord` / `rightWord` are the 32-bit sums whose
// low 16 bits are the two depths (the original keeps them in registers and takes the sign of the 16-bit value).
uint32_t LiftOfPair(int16_t& angle, int32_t lever, int32_t leftWord, int32_t rightWord) {
    const int32_t left = int16_t(leftWord), right = int16_t(rightWord);
    const uint32_t leftSign = uint32_t(leftWord << 16) >> 31, rightSign = uint32_t(rightWord << 16) >> 31;
    if (left == 0 && right == 0) return 0;
    if (right < left) {
        angle = AddS16(angle, Atan2(left - right, lever));
        return (uint32_t(right) + rightSign) >> 1;
    }
    if (left < right) {
        angle = AddS16(angle, Atan2(left - right, lever));
        return (uint32_t(left) + leftSign) >> 1;
    }
    return (uint32_t(left) + leftSign) >> 1;
}

} // namespace

void SuspensionPass(Car* cars, int count, uint8_t* scratch, const GroundGlobals& globals) { // 0x80043AE0
    auto workOf = [&](int car) { return scratch + uint32_t(car) * kSuspensionWorkStride; };
    auto skip = [&](const CarBody& body) { return body.raceState == 7; };

    // 1. wheel travel and the "below the travel floor" mask
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        if (skip(body)) continue;
        const int32_t ratio = Div(int32_t(body.visualBasis[kRowUp][2]) << 12, body.basis[kRowUp][2]);
        ScratchS32(workOf(car), kSuspensionWorkRatio) = ratio;
        ComputeTravel(body, workOf(car), ratio);
        body.contactMask = uint8_t(TravelFloorMask(body, workOf(car)));
    }
    // 2. spring and damper forces
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        if (skip(body)) continue;
        for (uint32_t wheel = 0; wheel < 4; wheel++)
            SpringDamperForce(body, body.wheels[wheel], ScratchS32(workOf(car), kSuspensionWorkTravel + uint32_t(wheel) * 4), AxleSuspensionOf(body, wheel >> 1));
    }
    // 3. per-wheel force sums: spring + anti-roll bar + half the axle's downforce + load transfer; damper copies
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        if (skip(body)) continue;
        uint8_t* work = workOf(car);
        int32_t antiRoll[2];
        for (uint32_t axle = 0; axle < 2; axle++) {
            const int32_t difference = int32_t(body.wheels[axle * 2].travel) - body.wheels[axle * 2 + 1].travel;
            const int32_t force = Mul12Floor(AxleSuspensionOf(body, axle).antiRollRate, MulLow(difference, difference) >> 12);
            antiRoll[axle] = difference < 0 ? Neg(force) : force;
        }
        const int32_t lateral = Mul12(body.rollLever, Neg(body.lateralAccel));
        const int32_t longitudinal = Mul12(body.pitchLever, Sub(body.netLongitudinalForce, body.externalLongForce));
        const int32_t halfDownforce[2] = {Half(body.downforceForce[0]), Half(body.downforceForce[1])};
        const int32_t spring[4] = {body.wheels[0].springForce, body.wheels[1].springForce, body.wheels[2].springForce, body.wheels[3].springForce};
        ScratchS32(work, kSuspensionWorkSpring + 0) = Add(Add(Sub(Sub(spring[0], antiRoll[0]), halfDownforce[0]), longitudinal), lateral);
        ScratchS32(work, kSuspensionWorkSpring + 4) = Sub(Add(Sub(Add(spring[1], antiRoll[0]), halfDownforce[0]), longitudinal), lateral);
        ScratchS32(work, kSuspensionWorkSpring + 8) = Add(Sub(Sub(Sub(spring[2], antiRoll[1]), halfDownforce[1]), longitudinal), lateral);
        ScratchS32(work, kSuspensionWorkSpring + 12) = Sub(Sub(Sub(Add(spring[3], antiRoll[1]), halfDownforce[1]), longitudinal), lateral);
        for (uint32_t wheel = 0; wheel < 4; wheel++)
            ScratchS32(work, kSuspensionWorkDamper + uint32_t(wheel) * 4) = body.wheels[wheel].damperForce;
    }
    // 4. integrate the attitude rates / angles and the velocity
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        if (skip(body)) continue;
        const uint8_t* work = workOf(car);
        if (body.penaltyFrames >= 2) {
            int32_t phase = 0;
            const int32_t lift = ResetLift(body.penaltyFrames, globals.rate, phase);
            if (LiftBodyToGround(body, lift)) {
                if (phase == 2) {
                    const int32_t target = globals.grid.heading.at(size_t(body.gridSlot));
                    const int32_t difference = AngleDifference(target, body.heading);
                    if (difference != 0) {
                        const int32_t step = MulLow(ScratchS32(scratch, 0), kResetHeadingRate) >> 16; // stepTime as left by the callers
                        if (step < difference) body.heading = AddS16(body.heading, step);
                        else if (difference < -step) body.heading = AddS16(body.heading, -step);
                        else body.heading = int16_t(target);
                    }
                }
                AttitudeRows(body);
                continue;
            }
        }
        const int32_t stepTime = body.stepTime;
        ScratchS32(scratch, 0) = stepTime;
        const int32_t s0 = ScratchS32(work, kSuspensionWorkSpring), s1 = ScratchS32(work, kSuspensionWorkSpring + 4);
        const int32_t s2 = ScratchS32(work, kSuspensionWorkSpring + 8), s3 = ScratchS32(work, kSuspensionWorkSpring + 12);
        const int32_t d0 = ScratchS32(work, kSuspensionWorkDamper), d1 = ScratchS32(work, kSuspensionWorkDamper + 4);
        const int32_t d2 = ScratchS32(work, kSuspensionWorkDamper + 8), d3 = ScratchS32(work, kSuspensionWorkDamper + 12);
        const int32_t pitchFront = body.inversePitchInertia[0], pitchRear = body.inversePitchInertia[1];
        const int32_t rollFront = body.inverseRollInertia[0], rollRear = body.inverseRollInertia[1];
        // pitch: springs, then dampers with the zero-crossing stop
        int16_t pitchRate = AddS16(body.pitchRate, Mul16(Sub(Mul12(Add(s0, s1), pitchFront), Mul12(Add(s2, s3), pitchRear)), stepTime));
        body.pitchRate = pitchRate;
        pitchRate = IntegrateRate(pitchRate, Mul16(Sub(Mul12(Add(d0, d1), pitchFront), Mul12(Add(d2, d3), pitchRear)), stepTime));
        body.pitchRate = pitchRate;
        IntegrateAngle(body.pitch, body.pitchRate, stepTime);
        // roll (the damper term uses the pitch gains - as in the original)
        int16_t rollRate = AddS16(body.rollRate, Mul16(Add(Mul12(Sub(s0, s1), rollFront), Mul12(Sub(s2, s3), rollRear)), stepTime));
        body.rollRate = rollRate;
        rollRate = IntegrateRate(rollRate, Mul16(Add(Mul12(Sub(d0, d1), pitchFront), Mul12(Sub(d2, d3), pitchRear)), stepTime));
        body.rollRate = rollRate;
        IntegrateAngle(body.roll, body.rollRate, stepTime);
        // airborne flag from the wheel loads
        const int32_t loadSum = Add(Add(Add(body.wheels[0].load, body.wheels[1].load), body.wheels[2].load), body.wheels[3].load);
        if (body.airborne == 0) {
            if (loadSum == 0) body.airborne = 1;
        } else if (loadSum != 0) {
            body.airborne = 0;
        }
        AttitudeRows(body);
        // velocity along the up row: springs, gravity, dampers; a damper that reverses the vertical speed stops it
        const int32_t response = body.inverseSprungMass;
        const int32_t springImpulse = Mul16(Mul12Wide(Add(Add(Add(s0, s1), s2), s3), response), stepTime);
        for (uint32_t i = 0; i < 3; i++) body.velocity[i] = Add(body.velocity[i], Mul12(body.basis[kRowUp][i], springImpulse));
        const int32_t afterGravity = Sub(body.velocity[2], MulLow(stepTime, kGravityPerSecond) >> 16);
        body.velocity[2] = afterGravity;
        const int32_t damperImpulse = Mul16(Mul12Wide(Add(Add(Add(d0, d1), d2), d3), response), stepTime);
        for (uint32_t i = 0; i < 3; i++) body.velocity[i] = Add(body.velocity[i], Mul12(body.basis[kRowUp][i], damperImpulse));
        if ((body.velocity[2] ^ afterGravity) < 0) body.velocity[2] = 0;
    }
    // 5. lift bodies whose wheels went below the travel floor, then floor the vertical speed by the ground slope
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        if (skip(body) || body.contactMask == 0) continue;
        uint8_t* work = workOf(car);
        UpdateWheelHeightOffsets(body);
        ComputeTravel(body, work, ScratchS32(work, kSuspensionWorkRatio));
        const uint8_t mask = uint8_t((uint32_t(body.contactMask) << 4) | TravelFloorMask(body, work));
        body.contactMask = mask;
        if (mask & 0xF) {
            int16_t depth[4];
            for (uint32_t wheel = 0; wheel < 4; wheel++) {
                const int16_t d = int16_t(uint16_t(AxleSuspensionOf(body, wheel >> 1).travelFloor) - uint16_t(ScratchS32(work, kSuspensionWorkTravel + uint32_t(wheel) * 4)));
                depth[wheel] = d < 0 ? int16_t(0) : d;
            }
            const int32_t frontSum = depth[0] + depth[1], rearSum = depth[2] + depth[3];
            const uint16_t frontMean = uint16_t((uint32_t(frontSum) + (uint32_t(frontSum) >> 31)) >> 1);
            const uint32_t rearMean = (uint32_t(rearSum) + (uint32_t(rearSum) >> 31)) >> 1;
            const int32_t blend = body.rearWeightFraction;
            const int32_t leftWord = Add(uint16_t(depth[0]), Mul12(depth[2] - depth[0], blend));
            const int32_t rightWord = Add(uint16_t(depth[1]), Mul12(depth[3] - depth[1], blend));
            uint32_t lift = LiftOfPair(body.roll, body.meanTrack, leftWord, rightWord);
            lift += LiftOfPair(body.pitch, body.wheelbase, int32_t(frontMean), int32_t(rearMean));
            body.position[2] = Add(body.position[2], int16_t(lift));
        }
        int32_t forward = 0, lateral = 0;
        for (uint32_t i = 0; i < 3; i++) {
            forward = Add(forward, Mul12Floor(body.visualBasis[kRowForward][i], body.velocity[i]));
            lateral = Add(lateral, MulLow(body.visualBasis[kRowLateral][i], body.velocity[i]) >> 12);
        }
        int32_t floor = Add(Mul12(forward, body.visualBasis[kRowForward][2]), Mul12(lateral, body.visualBasis[kRowLateral][2]));
        if (floor > 0) floor = 0;
        if (body.velocity[2] < floor) body.velocity[2] = floor;
    }
}

// ================================================================ wheel effects

void UpdateWheelEffects(CarBody& body, int32_t stepTime, const GroundConstants& constants, const GroundGlobals& globals) { // 0x800426F0
    const WheelEffectConstants& c = constants.effects;
    // wheel spin and brake heat of the front axle -> tyre load sound
    const int32_t slip0 = body.wheels[0].slipRatio, slip1 = body.wheels[1].slipRatio;
    const int32_t slipSquares = Add(MulLow(slip0, slip0) >> 12, MulLow(slip1, slip1) >> 12);
    int32_t spin = Mul12Floor(Add(body.wheels[0].driveForce, body.wheels[1].driveForce), Half(slipSquares) + 0x199);
    if (spin < 0) spin = Neg(spin);
    spin = Div(Wrap(uint32_t(spin) << 1), 3);
    if (spin > 0x555) spin = 0x555;
    const int32_t brakeMean = Div(Add(body.wheels[0].brakeForce, body.wheels[1].brakeForce), 12);
    int32_t brake = Mul12Floor(brakeMean, brakeMean);
    if (brake > 0x555) brake = 0x555;
    // wall impact hold
    const int32_t impactTimer = body.impactTimer;
    if (impactTimer == globals.rate || int32_t(body.impactHold) < body.wallImpact) body.impactHold = uint16_t(body.wallImpact);
    else if (impactTimer < Div(globals.rate * 3, 5)) body.impactHold = 0;
    const int32_t impactLevel = body.impactHold != 0 ? int32_t(body.impactHold) * 8 + 0x400 : 0;

    int32_t loadLevel = 0, skidLevel = 0;
    uint32_t dirtLevel = 0;
    for (uint32_t wheel = 0; wheel < 4; wheel++) {
        Wheel& w = body.wheels[wheel];
        if (body.scriptedControl == 0) {
            const int32_t damper = w.damperForce;
            if (damper > 0) {
                int32_t v = Div(damper, 12);
                if (v > 0x1000) v = 0x1000;
                loadLevel = Add(loadLevel, v);
            }
            if (wheel < 2 && w.surface == 1) {
                int32_t v = Mul12Wide(Div(w.load, 8), Div(body.forwardSpeed, 64));
                if (v < 0) v = Neg(v);
                if (v > 0x400) v = 0x400;
                skidLevel = Add(skidLevel, v);
            }
        }
        if (w.load == 0) {
            w.smokeLevel = 0;
            w.skidLevel = 0;
            w.dustLevel = 0;
            w.skidDelay = 0xFF;
            continue;
        }
        const uint32_t surface = w.surface;
        const uint32_t loose = surface < 6 ? (surface >= 2 ? 1u : 0u) : 0u;
        const int32_t slipRatio = w.slipRatio;
        int32_t ratio;
        if (slipRatio < 0) {
            ratio = Neg(slipRatio) >> 5;
            if (ratio < -0x80) ratio = -0x80;
        } else {
            ratio = slipRatio >> 5;
            if (ratio > 0x7F) ratio = 0x7F;
        }
        ratio = int16_t(ratio);
        const uint32_t steer = uint32_t(uint16_t(w.steerAngle));
        const int32_t slipSpeed = Add(Mul12Wide(Cos(steer), w.contactLateralSpeed), Mul12Wide(Sin(steer), w.contactForwardSpeed));
        const int32_t speedFactor = w.speedFactor;
        if (surface == 1) { // kerb: smoke from the load, dust from the speed
            int32_t smoke = (w.load >> 5) - 0x40;
            if (smoke < 0) smoke = 0;
            w.smokeLevel = uint8_t(smoke > 0xFF ? 0xFF : smoke);
            int32_t forward = w.contactForwardSpeed;
            if (forward < 0) forward = Neg(forward);
            const int32_t dust = (forward >> 10) - 0x20;
            w.dustLevel = uint8_t(dust < 0 ? 0 : dust > 0xFF ? 0xFF : dust);
        } else {
            int32_t slipTerm = MulLow(int32_t(c.slipSmokeGain), slipSpeed) >> 12;
            slipTerm = MulLow(slipTerm, slipTerm) >> 12;
            if (slipTerm > c.slipSmokeCap) slipTerm = c.slipSmokeCap;
            int32_t spinTerm;
            if (ratio <= 0) {
                spinTerm = Div(MulLow(Neg(ratio), c.spinSmokeGainBrake), 100) + c.spinSmokeBase;
                if (spinTerm > c.spinSmokeCap) spinTerm = c.spinSmokeCap;
            } else {
                spinTerm = 0;
                if (uint32_t(w.contactForwardSpeed) + 0x8E4u > 0x11C8u) { // |forward| > 0.56 m/s
                    if (ratio != 0x7F) {
                        spinTerm = Div(MulLow(ratio, c.spinSmokeGainDrive), 100) + c.spinSmokeBase;
                        if (spinTerm > c.spinSmokeCap) spinTerm = c.spinSmokeCap;
                    } else {
                        spinTerm = c.spinSmokeCap;
                    }
                }
            }
            int32_t smoke = Add(slipTerm, spinTerm);
            if (loose) smoke = Add(smoke, Div(MulLow(speedFactor >> 4, c.dirtSmokeSpeedGain), 100));
            if (smoke > c.smokeCap) smoke = c.smokeCap;
            else if (smoke < c.smokeThreshold) smoke = 0;
            w.smokeLevel = uint8_t(smoke);
            w.dustLevel = 0;
            if (loose) {
                const int32_t dust = (speedFactor >> 5) + 0x40;
                w.dustLevel = uint8_t(dust > 0xFF ? 0xFF : dust);
            }
        }
        // skid: slip angle folded into 0..0x400, capped, squared; plus the slip ratio term
        int32_t angle = w.slipAngle;
        if (angle < -0x400) angle += 0x800;
        else if (angle < 0) angle = -angle;
        else if (angle > 0x400) angle = 0x800 - angle;
        if (angle > 0x2AA) angle = 0x2AA;
        angle = MulLow(int32_t(c.skidAngleGain[loose]), angle);
        if (angle < 0) angle += 15;
        angle >>= 4;
        int32_t angleTerm = MulLow(angle, angle) >> 12;
        if (angleTerm > c.skidAngleCap[loose]) angleTerm = c.skidAngleCap[loose];
        int32_t ratioTerm;
        if (ratio == 0x7F) {
            ratioTerm = c.skidRatioCap[loose];
        } else {
            ratioTerm = ratio <= 0 ? MulLow(Neg(ratio), c.skidRatioGainBrake[loose]) : MulLow(ratio, c.skidRatioGainDrive[loose]);
            ratioTerm = Div(ratioTerm, 100) + c.skidRatioBase[loose];
            if (ratioTerm > c.skidRatioCap[loose]) ratioTerm = c.skidRatioCap[loose];
        }
        int32_t skid = Add(angleTerm, ratioTerm);
        if (skid > c.skidCap[loose]) skid = c.skidCap[loose];
        if (loose) {
            int32_t speedTerm = Div(MulLow(speedFactor >> 4, c.dirtSkidSpeedGain), 100);
            if (speedTerm > c.dirtSkidSpeedCap) speedTerm = c.dirtSkidSpeedCap;
            skid = Add(skid, speedTerm);
            if (skid > 0xFF) skid = 0xFF;
            dirtLevel = globals.dirtCourse ? uint32_t(skid) >> 1 : uint32_t(skid);
        }
        if (skid < c.skidThreshold[loose]) skid = 0;
        if (skid == 0) {
            w.skidDelay = 0xFF;
        } else {
            const int32_t delay = int32_t(w.skidDelay) + (MulLow(stepTime, speedFactor) >> 16);
            if (delay < c.skidDelay[loose]) {
                skid = 0;
                w.skidDelay = uint8_t(delay);
            } else {
                w.skidDelay = 0;
            }
        }
        w.skidLevel = uint8_t(skid);
    }
    uint32_t dirt = body.dirtiness + (uint32_t(MulLow(stepTime, int32_t(dirtLevel))) >> 16);
    body.dirtiness = dirt;
    if (dirt > kMaxDirtiness) body.dirtiness = kMaxDirtiness;
    int32_t level = Add(loadLevel, skidLevel);
    if (level < Add(spin, brake)) level = Add(spin, brake);
    if (level < impactLevel) level = impactLevel;
    level >>= 4;
    if (level > 0xFF) level = 0xFF;
    body.loadSoundLevel = uint8_t(level);
    body.impactSoundFlag = 0;
    if (Div(globals.rate << 2, 5) < impactTimer) body.impactSoundFlag = 1;
    if (body.forwardGears != 1) { // read as the engine control mode here (car_body.h)
        int32_t load = body.engineLoad;
        int32_t deviation = load - 0x600;
        if (load > 0x1000) {
            load = 0x1000;
            deviation = 0xA00;
        }
        if (deviation < 0) deviation = -deviation;
        int32_t gearFactor = 0x800;
        if (body.clutchState == 1) {
            const int32_t ratio = body.gearRatio[body.gear];
            int32_t q = MulLow(int32_t(uint64_t(Div64(int64_t(ratio) << 12, int64_t(body.gearRatio[1])))), 7);
            if (q < 0) q += 7;
            gearFactor = (q >> 3) + 0x200;
            if (gearFactor > 0x1000) gearFactor = 0x1000;
        }
        body.engineVisual1 = uint8_t(Half(int32_t(body.engineVisual1) + ((load * 3 + 0x1000) >> 7)));
        body.engineVisual0 = uint8_t(Half(int32_t(body.engineVisual0) + (MulLow(deviation + 0x600, gearFactor) >> 17)));
    }
    // US Simulation only; the US Arcade build ends above (GroundGlobals::wheelEffectsTail).
    if (globals.wheelEffectsTail && body.contactType == 2 && globals.flag800A951C == 0 && (globals.flag801C9995 == 0 || globals.flag800AF232 == 0)) {
        for (Wheel& w : body.wheels) {
            w.smokeLevel = 0;
            w.skidLevel = 0;
            w.dustLevel = 0;
        }
        body.engineVisual1 = 0;
        body.exhaustFlame = 0;
        body.engineVisual0 = 0;
    }
}

// ================================================================ view attitude, rows, visual pose

void SmoothViewAttitude(CarBody& body, int32_t stepTime, const GroundConstants& constants, const GroundGlobals& globals) {
    const uint32_t mode = globals.viewMode;
    const int32_t gain = constants.viewYawGain[mode & 1], damping = constants.viewYawDamping[mode & 1];
    int32_t target = body.heading;
    if (mode == 1 && ApproxLength(body.velocity[0], body.velocity[1]) >= kViewFollowMinSpeed) target = Atan2(Neg(body.velocity[0]), body.velocity[1]);
    const int32_t difference = AngleDifference(target, body.viewYaw);
    int32_t accel = Add(MulLow(difference, gain), MulLow(body.viewYawRate, Neg(damping)));
    if (accel < 0) accel += 0xFFF;
    accel >>= 12;
    const int32_t rateDelta = Mul16(stepTime, Half(int32_t(body.viewYawAccel) + accel));
    body.viewYawAccel = int16_t(accel);
    body.viewYawRate = AddS16(body.viewYawRate, rateDelta);
    int16_t yaw = AddS16(body.viewYaw, Mul16(body.viewYawRate, stepTime));
    yaw = int16_t(WrapAngle(yaw));
    body.viewYaw = yaw;
    body.viewYawOffset = int16_t(AngleDifference(body.heading, yaw));
    const int32_t pitchDifference = AngleDifference(body.pitch, body.viewPitch);
    const int32_t pitchAccel = Sub(Mul12(pitchDifference, kViewPitchGain), Mul12(body.viewPitchRate, kViewPitchDamping));
    const int16_t pitchRate = AddS16(body.viewPitchRate, Mul16(pitchAccel, stepTime));
    body.viewPitchRate = pitchRate;
    int16_t pitch = AddS16(body.viewPitch, Mul16(pitchRate, stepTime));
    pitch = int16_t(WrapAngle(pitch));
    body.viewPitch = pitch;
    body.viewPitchOffset = int16_t(AngleDifference(body.pitch, pitch));
}

void BuildAttitudeMatrix(int16_t row0[3], int16_t row1[3], int16_t row2[3], int32_t pitch, int32_t roll, int32_t heading) { // 0x80044EA4
    const int32_t sinPitch = Sin(uint32_t(pitch)), cosPitch = Cos(uint32_t(pitch));
    const int32_t sinRoll = Sin(uint32_t(roll)), cosRoll = Cos(uint32_t(roll));
    const int32_t sinHeading = Sin(uint32_t(heading)), cosHeading = Cos(uint32_t(heading));
    auto roundedShift = [](int32_t v) { return int16_t((v < 0 ? v + 0xFFF : v) >> 12); };
    const int32_t cosHeadingSinRoll = Mul12(cosHeading, sinRoll), sinHeadingSinRoll = Mul12(sinHeading, sinRoll);
    row1[0] = int16_t(Mul12(cosHeading, cosRoll));
    row1[1] = int16_t(Mul12(sinHeading, cosRoll));
    row1[2] = int16_t(-sinRoll);
    row0[0] = roundedShift(Sub(MulLow(cosHeadingSinRoll, sinPitch), MulLow(sinHeading, cosPitch)));
    row0[1] = roundedShift(Add(MulLow(sinHeadingSinRoll, sinPitch), MulLow(cosHeading, cosPitch)));
    row0[2] = int16_t(Mul12(cosRoll, sinPitch));
    row2[0] = roundedShift(Add(MulLow(cosHeadingSinRoll, cosPitch), MulLow(sinHeading, sinPitch)));
    row2[1] = roundedShift(Sub(MulLow(sinHeadingSinRoll, cosPitch), MulLow(cosHeading, sinPitch)));
    row2[2] = int16_t(Mul12(cosRoll, cosPitch));
}

void UpdateVisualPose(CarBody& body) { // 0x8003E7EC
    const int32_t blend = body.rearWeightFraction;
    const int32_t h0 = body.wheels[0].surfaceHeightAvg, h1 = body.wheels[1].surfaceHeightAvg;
    const int32_t h2 = body.wheels[2].surfaceHeightAvg, h3 = body.wheels[3].surfaceHeightAvg;
    const int32_t left = Add(h0, Mul12(Sub(h2, h0), blend)), right = Add(h1, Mul12(Sub(h3, h1), blend));
    body.visualPosition[0] = body.position[0];
    body.visualPosition[1] = body.position[1];
    body.visualPosition[2] = Half(Add(left, right));
    const int16_t pitch = int16_t(Atan2(Half(Sub(Add(h0, h1), Add(h2, h3))), body.wheelbase));
    body.visualPitch = pitch;
    const int16_t roll = int16_t(Atan2(Sub(left, right), body.meanTrack));
    body.visualRoll = roll;
    BuildAttitudeMatrix(body.visualBasis[kRowForward], body.visualBasis[kRowLateral], body.visualBasis[kRowUp], pitch, roll, body.heading);
}

void UpdateBodyFrameSpeeds(CarBody& body) { // 0x800304DC
    int32_t forward = 0, lateral = 0;
    for (uint32_t i = 0; i < 3; i++) {
        forward = Add(forward, Mul12Floor(body.visualBasis[kRowForward][i], body.velocity[i]));
        lateral = Add(lateral, MulLow(body.visualBasis[kRowLateral][i], body.velocity[i]) >> 12);
    }
    body.forwardSpeed = forward;
    body.lateralSpeed = lateral;
    const uint16_t readout = uint16_t(Mul12ShiftFloor(forward < 0 ? Neg(forward) : forward, kSpeedReadoutScale, 12));
    body.speedReadout = readout;
    if (body.maxSpeedReadout < readout) body.maxSpeedReadout = readout;
    const int32_t yawArm = Mul12ShiftWide(body.yawRate, kYawArmScale, 7);
    const int32_t frontLateral = Neg(Mul12(body.axleOffset[0], yawArm)), rearLateral = Mul12(body.axleOffset[1], yawArm);
    const int32_t frontForward = Mul12(body.halfTrack[0], yawArm), rearForward = Mul12(body.halfTrack[1], yawArm);
    const int32_t lateralTerm[4] = {frontLateral, frontLateral, rearLateral, rearLateral};
    const int32_t forwardTerm[4] = {Neg(frontForward), frontForward, Neg(rearForward), rearForward};
    for (uint32_t wheel = 0; wheel < 4; wheel++) {
        Wheel& w = body.wheels[wheel];
        const int32_t contactForward = Add(forward, forwardTerm[wheel]);
        w.contactForwardSpeed = contactForward;
        w.contactLateralSpeed = Add(lateral, lateralTerm[wheel]);
        const int32_t magnitude = contactForward < 0 ? Neg(contactForward) : contactForward;
        int16_t scale = 0x1000;
        if (magnitude < kSlipScaleFullSpeed) scale = int16_t((Wrap(uint32_t(Mul12Wide(magnitude, kSlipScaleGain)) << 11) >> 12) + 0x800);
        w.slipScale = scale;
    }
}

void UpdateWheelRotation(Car* cars, int count, int32_t stepTime) { // 0x800306C0
    for (int car = 0; car < count; car++) {
        CarBody& body = cars[car].body;
        UpdateBodyFrameSpeeds(body);
        bool freeAxle[2] = {false, false};
        for (uint32_t wheel = 0; wheel < 4; wheel++) {
            Wheel& w = body.wheels[wheel];
            if (w.contactFlags != 0) continue;
            const uint32_t steer = uint32_t(uint16_t(w.steerAngle));
            w.rimSpeed = Sub(Mul12Wide(Cos(steer), w.contactForwardSpeed), Mul12Wide(Sin(steer), w.contactLateralSpeed));
            freeAxle[wheel >> 1] = true;
        }
        for (uint32_t axle = 0; axle < 2; axle++) {
            if (!freeAxle[axle] && body.axleDiffType[axle] != 0) continue;
            const int32_t meanRimSpeed = Half(Add(body.wheels[axle * 2].rimSpeed, body.wheels[axle * 2 + 1].rimSpeed));
            body.axleSpeed[axle] = Mul12Wide(body.inverseWheelRadius[axle], meanRimSpeed);
        }
        for (uint32_t wheel = 0; wheel < 4; wheel++) {
            Wheel& w = body.wheels[wheel];
            const int32_t distance = Mul16Floor(stepTime, w.rimSpeed);
            const int32_t delta = Mul12(MulLow(distance, body.inverseWheelRadius[wheel >> 1]) >> 12, kRadiansToAngle);
            int32_t rotation = Add(w.rotation, delta);
            while (int16_t(rotation) > 0xFFF) rotation -= 0x1000;
            while (int16_t(rotation) < 0) rotation += 0x1000;
            w.rotation = uint16_t(rotation);
        }
    }
}

void GroundPass(Car* cars, int count, uint8_t* scratch, const GroundConstants& constants, const GroundGlobals& globals) { // 0x8003E8E4
    if (!globals.collisionDisabled) SuspensionPass(cars, count, scratch, globals);
    for (int car = 0; car < count; car++) {
        ScratchS32(scratch, 0) = cars[car].body.stepTime;
        UpdateWheelEffects(cars[car].body, ScratchS32(scratch, 0), constants, globals);
    }
    for (int car = 0; car < count; car++) SmoothViewAttitude(cars[car].body, ScratchS32(scratch, 0), constants, globals);
    for (int car = 0; car < count; car++) UpdateWheelGeometry(cars[car].body);
    for (int car = 0; car < count; car++) AttitudeRows(cars[car].body);
    for (int car = 0; car < count; car++) UpdateVisualPose(cars[car].body);
    UpdateWheelRotation(cars, count, ScratchS32(scratch, 0));
}

} // namespace gt2::sim
