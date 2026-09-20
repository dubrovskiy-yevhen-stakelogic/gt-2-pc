#pragma once
#include <cstdint>

#include "gt2formats/track.h"

namespace gt2::sim {

// A sweep segment of one car corner: where the corner was and where it wants to be, in 16.16 metres of the
// simulation plane (x, y) - z unused. Layout mirrors the original's 44-byte record (verified on it).
#pragma pack(push, 1)
struct SweepSegment {
    int32_t x0, y0, z0;     // 0x00 start
    int32_t x1, y1, z1;     // 0x0C end
    int32_t fraction;       // 0x18 12-bit fraction of the sweep at the first wall hit; 0x1000 = no hit
    int16_t hitNormal0;     // 0x1C boundary normal at the hit: (n1, -n0)
    int16_t hitNormal1;     // 0x1E
    int16_t localX0, localY0, localY1, localX1; // 0x20 chunk-local 1/64 m coordinates (filled by the test)
    int16_t negDeltaY, deltaX;                  // 0x28 perpendicular of the sweep direction
};
#pragma pack(pop)
static_assert(sizeof(SweepSegment) == 44);

// Tests four sweep segments against the walls of one course chunk and records the earliest hit per segment
// (original: 0x80027FC4). Cell origin = chunk centre rounded down to the 64 m grid, like the geometry.
void TestSegmentsAgainstChunk(const TrackChunk& chunk, SweepSegment segments[4]);

// Sweeps the four segments against the car's chunk and its neighbours along the course (up to 8 chunks or
// 16 m in each direction) - original: 0x80028968. The simulation plane's y axis points the other way than the
// course z axis: the routine negates y on entry and on exit, including the recorded normal's y.
void SweepAgainstCourse(const Track& track, uint32_t chunkIndex, SweepSegment segments[4]);

} // namespace gt2::sim
