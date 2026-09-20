#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "game/sim/car_body.h"
#include "gt2formats/track.h"

// Ground contact and suspension: wheel contact points, course height / surface query, suspension loads, body
// attitude (original: 0x80043388, 0x800434DC, 0x80043578, 0x8003E8E4 and callees).
// Ported from the US Simulation v1.2 executable and verified bit for bit by tools/gt2verify (verify_ground.cpp).
//
// Units: positions 1/4096 m in the body (simulation plane x, y, height) and 16.16 m in the course (world X,
// height Y, Z = -simulation y); angles 4096 per turn; factors 0x1000 = 1.0. Offsets in comments are relative to
// the body (car + 0x2C), a wheel record (body + 0x460 + wheel * 0x68) or the per-axle suspension block
// (body + 0x12C + axle * 0x34).
//
// The scratchpad (0x1F800000) is the step's work area. This subsystem uses:
//   +0x000            s32 stepTime of the car being processed (written by the pass drivers)
//   +0x004 + car*0x60 + wheel*0x18   ContactQuery (24 bytes), written by 0x80043388, answered by 0x800434DC,
//                     consumed by 0x80043578 and 0x80043AE0
//   +0x000 + car*0x34 SuspensionWork (see below), private to 0x80043AE0 (note the 0x34 stride: +0x34 of a
//                     car is +0x00 of the next; the original only ever reads +0 for the current car after
//                     writing it)
namespace gt2::sim {

// ---------------------------------------------------------------- course surface (road polygon) lookup

// Road-surface lookup grid of one course chunk: the original's chunk object keeps a pointer to it at
// chunk + 0x9C ({ s16 originX, originZ, shiftX, shiftZ; u16 count[16]; polygon* list[16]; then the lists }):
// a 4 x 4 grid in chunk-local 1/64 m over the DRIVABLE polygons of the chunk's road shape (a subset chosen
// offline: kerbs and details are left out), each cell listing its polygons in the file's order (the first hit
// wins on shared edges). In the .tro the same block sits at the file offset stored at chunk + 0x9C, with file
// offsets of the polygons (inside the road shape's lists) instead of pointers (checked on seattle.tro: 5874
// entries, all inside the chunk's own road shape). Not parsed by gt2::Track yet; the verifier builds it from
// the guest's course object. Polygon numbers index TrackChunk::road.polygons.
struct SurfaceGrid {
    int16_t originX = 0, originZ = 0;   // chunk-local 1/64 m (same frame as the road vertices)
    int16_t shiftX = 0, shiftZ = 0;     // cell size = 1 << shift
    std::vector<uint16_t> cells[16];    // row-major: cell = x + z * 4
};

struct CourseSurface {
    const Track* track = nullptr;
    std::vector<SurfaceGrid> grids;     // one per chunk
};

// One wheel's contact query record in the scratchpad (0x1F800004 + car * 0x60 + wheel * 0x18).
#pragma pack(push, 1)
struct ContactQuery {
    uint8_t surface;         // +0x00  out: polygon word1 >> 28 (surface type, indexes the per-surface tables); 7 = no road found
    uint8_t attribute1;      // +0x01  out: (word1 >> 23) & 3; 4 = no road found. Copied to wheel + 0x15.
    uint8_t attribute2;      // +0x02  out: (word1 >> 25) & 3; bit 1 = "off road" (counted by 0x80043578)
    uint8_t reserved03;      // +0x03  out: 0
    uint16_t chunkIndex;     // +0x04  in: chunk hint (the body's chunk); out: the chunk found along the course
    uint16_t reserved06;     //        untouched
    int32_t x;               // +0x08  in: world X of the wheel, 16.16 m
    int32_t height;          // +0x0C  in: world height, 16.16 m (only copied around)
    int32_t y;               // +0x10  in: simulation-plane y (= -world Z), 16.16 m
    int32_t surfaceHeight;   // +0x14  out: road height under the wheel, 16.16 m; 0x7FFFFFFF = no road found
};
#pragma pack(pop)
static_assert(sizeof(ContactQuery) == 0x18);

constexpr uint32_t kContactQueryBase = 0x004, kContactQueryCarStride = 0x60, kContactQueryWheelStride = 0x18;
constexpr int32_t kNoSurfaceHeight = 0x7FFFFFFF;

inline ContactQuery& ContactQueryAt(uint8_t* scratch, size_t car, size_t wheel) {
    return *reinterpret_cast<ContactQuery*>(scratch + kContactQueryBase + car * kContactQueryCarStride + wheel * kContactQueryWheelStride);
}

// Is `point` (world 16.16: X, height, Z) at or past the chunk's start plane? dot(point - origin, direction) is
// summed in 64 bits and tested on bit 43 like the original (0x80027BBC with the GTE-free dot 0x80081A78).
bool PointPastChunkOrigin(const TrackChunk& chunk, const int32_t point[3]);

// The chunk whose span along the course contains `point`, starting from `hint`: walks backwards while the
// point is before the chunk's start plane, then forwards while it is past the next chunk's plane (0x80028394).
uint32_t FindChunkAlongCourse(const Track& track, uint32_t hint, const int32_t point[3]);

// Drivable polygon of `chunk` under the chunk-local point (1/64 m), or -1. Point-in-polygon by four GTE NCLIP
// orientation tests against the ring v0 v1 v2 (v3), edges inclusive; the point is truncated to 16 bits like the
// GTE's SXY register (0x800279E8).
int32_t FindRoadPolygon(const TrackChunk& chunk, const SurfaceGrid& grid, int32_t localX, int32_t localZ);

// Same for a world point (0x80027C1C): local = (world - cell) >> 10 with cell = centre & 0xFFC00000.
int32_t FindRoadPolygonAt(const TrackChunk& chunk, const SurfaceGrid& grid, const int32_t point[3]);

// Height of the polygon's surface under the world point, 16.16 m (0x80027C70). A quad is split into four
// triangles around its centroid (average of the corners), a triangle uses (v2, v0, v1); the plane through the
// three corners is evaluated with 32-bit cross products and one 64-bit division.
int32_t InterpolateRoadHeight(const TrackChunk& chunk, const TrackPolygon& polygon, const int32_t point[3]);

// Surface type and height under the world point (X, height, Z) in chunk `chunkIndex` or its neighbours (up to
// 8 chunks each way, previous before next) - 0x80028470. Writes surface, attribute1, attribute2, reserved03 and
// surfaceHeight of `query`.
void QueryRoadSurface(const CourseSurface& course, uint32_t chunkIndex, const int32_t point[3], ContactQuery& query);

// Full contact query of one wheel (0x80028830): finds the chunk from the hint, then the road under the wheel.
// The simulation y is negated on the way in to become the course Z.
void QueryContact(const CourseSurface& course, ContactQuery& query);

// ---------------------------------------------------------------- suspension data

// Per-axle suspension block: CarBody::suspension[axle] (car_body.h AxleSuspension, body + 0x12C + axle * 0x34).
constexpr uint32_t kAxleSuspensionOffset = offsetof(CarBody, suspension);

inline AxleSuspension& AxleSuspensionOf(CarBody& body, size_t axle) { return body.suspension[axle]; }
inline const AxleSuspension& AxleSuspensionOf(const CarBody& body, size_t axle) { return body.suspension[axle]; }

// The suspension pass's per-car work block (0x1F800000 + car * 0x34).
constexpr uint32_t kSuspensionWorkStride = 0x34;
constexpr uint32_t kSuspensionWorkRatio = 0x04;     // s32 ground-to-body up-vector ratio
constexpr uint32_t kSuspensionWorkTravel = 0x08;    // s32[4] wheel travel
constexpr uint32_t kSuspensionWorkSpring = 0x18;    // s32[4] spring + anti-roll + aero + load-transfer forces
constexpr uint32_t kSuspensionWorkDamper = 0x28;    // s32[4] damper forces (+0x34 overlaps the next car's +0)

// Constants of the overlay read by this subsystem (the verifier copies them from the dump so the check stays exact).
#pragma pack(push, 1)
struct WheelEffectConstants { // bytes 0x80046EAC .. 0x80046ECB (0x800426F0)
    uint8_t slipSmokeGain;          // 0x80046EAC  lateral slip speed -> smoke
    uint8_t slipSmokeCap;           // 0x80046EAD
    uint8_t spinSmokeGainDrive;     // 0x80046EAE  slip ratio (driving) -> smoke
    uint8_t spinSmokeGainBrake;     // 0x80046EAF  slip ratio (braking) -> smoke
    uint8_t spinSmokeCap;           // 0x80046EB0
    uint8_t spinSmokeBase;          // 0x80046EB1
    uint8_t smokeCap;               // 0x80046EB2
    uint8_t smokeThreshold;         // 0x80046EB3  below this the smoke level is 0
    uint8_t dirtSmokeSpeedGain;     // 0x80046EB4  speed factor (wheel + 0x40) -> dust on loose surfaces
    uint8_t reserved[3];            // 0x80046EB5
    uint8_t skidAngleGain[2];       // 0x80046EB8  [0] tarmac / kerb, [1] loose surface
    uint8_t skidAngleCap[2];        // 0x80046EBA
    uint8_t skidRatioGainDrive[2];  // 0x80046EBC
    uint8_t skidRatioGainBrake[2];  // 0x80046EBE
    uint8_t skidRatioCap[2];        // 0x80046EC0
    uint8_t skidRatioBase[2];       // 0x80046EC2
    uint8_t skidCap[2];             // 0x80046EC4
    uint8_t skidThreshold[2];       // 0x80046EC6
    uint8_t skidDelay[2];           // 0x80046EC8  wheel + 0x1E counts up to this before the skid is reported
    uint8_t dirtSkidSpeedGain;      // 0x80046ECA
    uint8_t dirtSkidSpeedCap;       // 0x80046ECB
};
#pragma pack(pop)
static_assert(sizeof(WheelEffectConstants) == 0x20);

struct GroundConstants {
    // Per surface type (polygon word1 >> 28, 0..15). The original's tables have 8 entries each and are indexed
    // by the whole surface byte: types 8..15 read the following table (amplitude[8..15] = frequency[0..7] and so
    // on), so 16 entries are kept here, copied from the contiguous block 0x80046F88..0x80046FC7.
    int16_t roughnessAmplitude[16] = {};   // 0x80046F88  0 = smooth
    int16_t roughnessFrequency[16] = {};   // 0x80046F98  spatial frequency of the sin/cos bumps
    uint8_t roughnessSpeedScaled[16] = {}; // 0x80046FA8  non-zero: amplitude scaled by the wheel's speed factor (wheel + 0x40)
    int32_t viewYawGain[2] = {};          // 0x80046C94  per view mode (0x801C9990, 0 or 1): view yaw spring
    int32_t viewYawDamping[2] = {};       // 0x80046C9C  (the original indexes both by mode * 4; only modes 0 and 1 have data)
    WheelEffectConstants effects{};       // 0x80046EAC
};

// Race-grid records of the game shell (0x801C8568 -> +0x18: { s32 count; { ..., s32 distance at +0x14, ...,
// s32 heading at +0x24 } [count] }, 0x28 bytes per record). Used by the reset animation (target heading of the
// car's grid slot, body + 0x6B2) and by the "stuck in the run-off" test (0x80036BDC).
struct RaceGridInfo {
    int32_t count = 0;                 // 0 = no list
    std::vector<int32_t> distance;     // course distance of each slot (16.16 m)
    std::vector<int32_t> heading;      // heading of each slot (angle units)
};

struct GroundGlobals {
    int32_t rate = 30;                 // 0x801C8570  simulation frames per second
    uint8_t viewMode = 0;              // 0x801C9990  selects the view yaw constants; 1 = follow the velocity
    uint8_t raceMode = 2;              // 0x801D5866  3 / 6 = race modes with the stuck-car reset
    bool collisionDisabled = false;    // 0x800A9520  skips the suspension pass
    bool dirtCourse = false;           // course table entry (0x80060E94(0x800AF230)) + 8 & 4
    uint8_t flag800A951C = 0;          // 0x800A951C  \  the effects of a car of type 2 (body + 0x45E) are cleared
    uint8_t flag801C9995 = 0;          // 0x801C9995   > when 0x800A951C == 0 and (0x801C9995 == 0 or 0x800AF232 == 0)
    uint8_t flag800AF232 = 0;          // 0x800AF232  /
    // The clear above exists only in the US Simulation build (0x800426F0); the US Arcade build's UpdateWheelEffects
    // (0x8004269C) returns after the effect levels (gt2formats/exe_profile.h ExeProfile::wheelEffectsTail).
    bool wheelEffectsTail = true;
    int32_t courseLength = 0;          // chunk table + 0 (s32, 16.16 m) of the course object (0x80036BDC)
    RaceGridInfo grid;
};

// ---------------------------------------------------------------- wheel geometry

// Planar wheel offsets (wheel + 0x24 x, + 0x26 y, 1/4096 m) from the attitude rows (body + 0x668 forward,
// + 0x670 lateral) and the axle positions (body + 0x0C front, + 0x0E rear) and half tracks (+ 0x18, + 0x1A)
// (original: 0x8004323C).
void UpdateWheelOffsets(CarBody& body);

// Height offsets of the wheels (wheel + 0x28) from the rows' vertical components (body + 0x66C, + 0x674)
// (original: 0x800431A0).
void UpdateWheelHeightOffsets(CarBody& body);

// Both (original: 0x8004335C).
void UpdateWheelGeometry(CarBody& body);

// ---------------------------------------------------------------- the passes after the move pass

// Pass 1 (0x80043388): wheel offsets, the wheel speed factor (wheel + 0x40 = min(1.0, |contact velocity| * 0x52 / 0x1000))
// and the contact query records for every wheel: world position of the wheel * 16 and the body's chunk.
void PrepareContactQueries(Car* cars, int count, uint8_t* scratch);

// Pass 2 (0x800434DC): answers every record.
void QueryContacts(const CourseSurface& course, int count, uint8_t* scratch);

// Pass 3 (0x80043578): applies the answers to the wheels (surface height wheel + 0x04 (16.16), the average of
// the last two in wheel + 0x00 (1/4096 m), surface type wheel + 0x14 / + 0x15, roughness wheel + 0x42), the body's
// chunk, the off-road flag (body + 0x78D bit 4) and the stuck-car reset: when all four wheels are off the road
// in a race the reset counter (body + 0x6FB) runs and a reset (body + 0x6FA = 2) is requested through the game
// shell (0x800156B8 writes car + 0xA92 = 120, car + 0xA8D = 2 of the car slot body + 0x45C).
void ApplyContacts(Car* cars, int count, uint8_t* scratch, const GroundConstants& constants, const GroundGlobals& globals);

// Is the car's course distance (16.16 m, wrapped by the course length when in the first half) strictly between
// the distances of grid slots `slot` + 1 and count - 2 - `slot`? Only answered for lists of at least 6 slots;
// used by the stuck-car reset with slot 0 (original: 0x80036BDC).
bool InStartZone(int32_t distance, int32_t slot, const GroundGlobals& globals);

// Spring and damper force of one wheel for the travel `height` (0x800438F0). Writes wheel + 0x10 (travel, s16),
// + 0x12 (travel velocity, s16), + 0x48 spring force, + 0x4C damper force, + 0x08 load (>= 0). Returns spring + damper.
int32_t SpringDamperForce(const CarBody& body, Wheel& wheel, int32_t height, const AxleSuspension& axle);

// Lift of the reset animation for the reset timer `state` (body + 0x78E, frames) (0x800357C8): phase 0 for the
// first rate/2 frames (lift 0), phase 2 until 4.5 * rate (lift 0x4CC), phase 1 afterwards (lift falling to 0 at 5 * rate).
int32_t ResetLift(int32_t state, int32_t rate, int32_t& phase);

// Puts the body at the ground height (body + 0x688, from the visual pose) plus `lift` if that is above its
// current height, zeroing the attitude and the vertical speed; returns whether it did (0x80043AA4).
bool LiftBodyToGround(CarBody& body, int32_t lift);

// Pass 4a (0x80043AE0): the suspension proper. Travel per wheel from the surface heights, spring / damper
// forces, anti-roll bars, downforce and load transfer; integrates the pitch / roll rates and angles (body + 0x650,
// + 0x652, + 0x644, + 0x646), the attitude rows, the velocity (forces along the up row, gravity) and finally lifts
// bodies whose wheels went below the travel floor. Cars with body + 0x786 == 7 are skipped; a body in the reset
// animation (body + 0x78E > 1) is lifted instead.
void SuspensionPass(Car* cars, int count, uint8_t* scratch, const GroundGlobals& globals);

// Pass 4b (0x800426F0): wheel effect levels (wheel + 0x1C skid, + 0x1D smoke, + 0x1E skid delay, + 0x1F dust),
// tyre load sound (body + 0x762), brake flag (body + 0x763), engine visual bytes (body + 0x757, + 0x758) and the
// car's dirtiness (body + 0x658). `stepTime` is the scratchpad's +0 (16.16 s).
void UpdateWheelEffects(CarBody& body, int32_t stepTime, const GroundConstants& constants, const GroundGlobals& globals);

// Pass 4c: eases the view yaw / pitch (body + 0x748 .. + 0x752) towards the heading and pitch (or the direction of
// travel in view mode 1) and records the offsets (body + 0x73A, + 0x73C). Inline in 0x8003E8E4; `stepTime` is the
// scratchpad's +0 as left by the previous pass.
void SmoothViewAttitude(CarBody& body, int32_t stepTime, const GroundConstants& constants, const GroundGlobals& globals);

// Rotation rows from pitch, roll and heading (0x80044EA4). row0 = forward, row1 = lateral, row2 = up, each
// (x, y, height) with 0x1000 = 1.0. In 0x8003E8E4 the rows are body + 0x668 / + 0x670 / + 0x678 from
// body + 0x644 / + 0x646 / + 0x648.
void BuildAttitudeMatrix(int16_t row0[3], int16_t row1[3], int16_t row2[3], int32_t pitch, int32_t roll, int32_t heading);

// Pass 4e (0x8003E7EC): the ground-following visual pose (body + 0x680 position with the wheels' mean surface
// height, + 0x6F4 pitch, + 0x6F6 roll, rows + 0x68C / + 0x694 / + 0x69C from the heading).
void UpdateVisualPose(CarBody& body);

// Pass 4f (0x800304DC): forward / lateral speed from the velocity on the visual rows, the speed readout
// (body + 0x6AE, max in + 0x6F8), the contact patch velocities of the wheels (wheel + 0x2C, + 0x30 including the
// yaw rate) and the low-speed slip scale (wheel + 0x2A).
void UpdateBodyFrameSpeeds(CarBody& body);

// Pass 4f for every car plus (0x800306C0): rim speed (wheel + 0x18) of wheels without contact flags, axle speeds
// (body + 0x634) from the wheel speeds and the wheel rotation angles (wheel + 0x20). `stepTime` is the scratchpad's +0.
void UpdateWheelRotation(Car* cars, int count, int32_t stepTime);

// Pass 4 (0x8003E8E4): the suspension pass (unless collision is disabled), the wheel effects, the view attitude,
// the wheel geometry, the attitude rows, the visual pose and the wheel rotation for every car.
void GroundPass(Car* cars, int count, uint8_t* scratch, const GroundConstants& constants, const GroundGlobals& globals);

} // namespace gt2::sim
