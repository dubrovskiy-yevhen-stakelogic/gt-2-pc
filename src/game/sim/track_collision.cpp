#include "game/sim/track_collision.h"

#include <utility>

namespace gt2::sim {
namespace {

// The original evaluates the 2D cross products on the GTE (MVMVA, no shift) and reads the MAC registers,
// i.e. the exact products summed in 64 bits and truncated to 32.
inline int32_t Dot2(int32_t ax, int32_t ay, int32_t bx, int32_t by) { return int32_t(int64_t(ax) * bx + int64_t(ay) * by); }

} // namespace

void TestSegmentsAgainstChunk(const TrackChunk& chunk, SweepSegment segments[4]) {
    if (chunk.boundaries.empty()) return;
    const uint32_t cellX = uint32_t(chunk.centre[0]) & 0xFFC00000u, cellZ = uint32_t(chunk.centre[2]) & 0xFFC00000u;

    for (int i = 0; i < 4; i++) {
        SweepSegment& s = segments[i];
        const int32_t x0 = int32_t(uint32_t(s.x0) - cellX) >> 10, y0 = int32_t(uint32_t(s.y0) - cellZ) >> 10;
        const int32_t x1 = int32_t(uint32_t(s.x1) - cellX) >> 10, y1 = int32_t(uint32_t(s.y1) - cellZ) >> 10;
        s.localX0 = int16_t(x0);
        s.localY0 = int16_t(y0);
        s.localY1 = int16_t(y1);
        s.localX1 = int16_t(x1);
        int16_t dx = int16_t(int16_t(x1) - int16_t(x0)), dy = int16_t(int16_t(y1) - int16_t(y0));
        const bool outOfRange = uint32_t(x0 + 0x7FFF) > 0xFFFE || x1 < -0x7FFF || x1 > 0x7FFF || y0 < -0x7FFF || y0 > 0x7FFF || y1 < -0x7FFF || y1 > 0x7FFF;
        if (outOfRange) dx = dy = 0;
        s.negDeltaY = int16_t(-dy);
        s.deltaX = dx;
    }

    for (const TrackBoundary& wall : chunk.boundaries) {
        const TrackVertex& a = chunk.road.vertices[wall.vertexA];
        const TrackVertex& b = chunk.road.vertices[wall.vertexB];
        // V0 = (by - ay, ax - bx): perpendicular of the wall, 16-bit like the GTE sees it.
        const int16_t wx = int16_t(b.y - a.y), wy = int16_t(a.x - b.x);
        const int32_t wallSide = Dot2(a.x, a.y, wx, wy);
        for (int i = 0; i < 4; i++) {
            SweepSegment& s = segments[i];
            // First MVMVA: which side of the wall line are the sweep's end points on?
            const int32_t startSide = int32_t(uint32_t(Dot2(s.localX0, s.localY0, wx, wy)) - uint32_t(wallSide));
            const int32_t endSide = int32_t(uint32_t(Dot2(s.localX1, s.localY1, wx, wy)) - uint32_t(wallSide));
            const int32_t span = int32_t(uint32_t(startSide) - uint32_t(endSide));
            if (!((startSide ^ endSide) < 0 && (startSide | span) >= 0)) continue;
            // Second MVMVA: are the wall's end points on opposite sides of the sweep line?
            const int32_t sweepSide = Dot2(s.localX1, s.localY1, s.negDeltaY, s.deltaX);
            const int32_t aSide = int32_t(uint32_t(Dot2(a.x, a.y, s.negDeltaY, s.deltaX)) - uint32_t(sweepSide));
            const int32_t bSide = int32_t(uint32_t(Dot2(b.x, b.y, s.negDeltaY, s.deltaX)) - uint32_t(sweepSide));
            if ((aSide ^ bSide) >= 0) continue;
            const int32_t fraction = int32_t((int64_t(startSide) << 12) / int64_t(span));
            if (fraction < s.fraction) {
                s.fraction = fraction;
                s.hitNormal0 = wall.normal1;
                s.hitNormal1 = int16_t(-wall.normal0);
            }
        }
    }
}

namespace {

// Distance metric of the original between two chunk origins: max + mid / 2 + min / 4 of the |axis deltas|.
uint32_t ChunkDistance(const TrackChunk& a, const TrackChunk& b) {
    uint32_t d[3];
    for (int i = 0; i < 3; i++) {
        const int32_t delta = int32_t(uint32_t(b.origin[size_t(i)]) - uint32_t(a.origin[size_t(i)]));
        d[i] = delta < 0 ? uint32_t(-delta) : uint32_t(delta);
    }
    uint32_t big = d[0], mid = d[1], small = d[2];
    if (big < mid) std::swap(big, mid);
    if (big < small) std::swap(big, small);
    if (mid < small) std::swap(mid, small);
    return big + (mid >> 1) + (small >> 2);
}

} // namespace

void SweepAgainstCourse(const Track& track, uint32_t chunkIndex, SweepSegment segments[4]) {
    for (int i = 0; i < 4; i++) {
        segments[i].fraction = 0x1000;
        segments[i].y0 = int32_t(0u - uint32_t(segments[i].y0));
        segments[i].y1 = int32_t(0u - uint32_t(segments[i].y1));
    }
    const TrackChunk* start = &track.chunks.at(chunkIndex);
    TestSegmentsAgainstChunk(*start, segments);
    const TrackChunk* back = &track.chunks[start->prev];
    const TrackChunk* forward = &track.chunks[start->next];
    uint32_t backDistance = 0, forwardDistance = 0;
    for (int step = 0; step < 8; step++) {
        bool idle = true;
        if (backDistance < 0x100000) {
            TestSegmentsAgainstChunk(*back, segments);
            const TrackChunk* next = &track.chunks[back->prev];
            backDistance += ChunkDistance(*back, *next);
            back = next;
            idle = false;
        }
        if (forwardDistance < 0x100000) {
            TestSegmentsAgainstChunk(*forward, segments);
            const TrackChunk* next = &track.chunks[forward->next];
            forwardDistance += ChunkDistance(*forward, *next);
            forward = next;
            idle = false;
        }
        if (idle) break;
    }
    for (int i = 0; i < 4; i++) {
        segments[i].y0 = int32_t(0u - uint32_t(segments[i].y0));
        segments[i].y1 = int32_t(0u - uint32_t(segments[i].y1));
        segments[i].hitNormal1 = int16_t(-segments[i].hitNormal1);
    }
}

} // namespace gt2::sim
