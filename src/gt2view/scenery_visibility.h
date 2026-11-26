#pragma once
#include "gt2formats/track.h"
#include <algorithm>
#include <cmath>
namespace gt2view {
// 0x80020FD8, the chunk distance key: max + mid / 2 + min / 4 of the absolute centre-to-eye components. The original
// uses it to order the render list and, for the mirror, to skip a chunk beyond 0x63FFFF (about 100 m). It is a chunk
// measure, not a sphere: a large chunk can still reach past a key radius, exactly as on the console.
inline float ChunkDistanceKey(const gt2::TrackChunk& chunk, const std::array<float,3>& eye) {
    float d[3] = {std::abs(float(chunk.centre[0] / 65536.0) - eye[0]),
                  std::abs(float(chunk.centre[1] / 65536.0) - eye[1]),
                  std::abs(float(chunk.centre[2] / 65536.0) - eye[2])};
    std::sort(d, d + 3); // d[2] largest, d[0] smallest
    return d[2] + d[1] * 0.5f + d[0] * 0.25f;
}
// Distance from `eye` to the nearest point of a chunk's box (metres). A numeric Draw Distance uses this for submission
// culling, so a chunk that still reaches into the radius is kept; `bounds` 0 is the main view's box, 1 the mirror's
// (chunk + 0x6C).
inline float ChunkNearestDistance(const gt2::TrackChunk& chunk, const std::array<float,3>& eye, size_t bounds = 0) {
    const gt2::TrackChunkBounds& b = chunk.bounds[std::min<size_t>(bounds, 1)];
    float loX = 1e30f, hiX = -1e30f, loZ = 1e30f, hiZ = -1e30f;
    for (int k = 0; k < 4; ++k) {
        const float x = float(b.x[k] / 65536.0), z = float(b.z[k] / 65536.0);
        loX = std::min(loX, x); hiX = std::max(hiX, x);
        loZ = std::min(loZ, z); hiZ = std::max(hiZ, z);
    }
    if (loX == 0.0f && hiX == 0.0f && loZ == 0.0f && hiZ == 0.0f && b.yLow == 0 && b.yHigh == 0) { // no authored box
        const float dx = float(chunk.centre[0] / 65536.0) - eye[0], dy = float(chunk.centre[1] / 65536.0) - eye[1],
                    dz = float(chunk.centre[2] / 65536.0) - eye[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    const float loY = float(std::min(b.yLow, b.yHigh) / 65536.0), hiY = float(std::max(b.yLow, b.yHigh) / 65536.0);
    auto outside = [](float p, float lo, float hi) { return p < lo ? lo - p : p > hi ? p - hi : 0.0f; };
    const float dx = outside(eye[0], loX, hiX), dy = outside(eye[1], loY, hiY), dz = outside(eye[2], loZ, hiZ);
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
inline void ExtendTrackEntries(std::vector<uint16_t>& entries,const gt2::Track& track,
                               const std::array<float,3>& eye,float distance,size_t bounds=0) {
    if(distance==0) return;
    std::vector<bool> inRange(track.chunks.size(),false),listed(track.chunks.size(),false);
    for(size_t i=0;i<track.chunks.size();++i) {
        inRange[i]=distance<0 || ChunkNearestDistance(track.chunks[i],eye,bounds)<=distance;
    }
    for(auto& entry:entries) {
        const size_t index=entry&0x3fff;
        if(index>=listed.size()) continue;
        listed[index]=true;
        // A retail render list can already contain this chunk as lights only.
        // Extending its range must promote that entry as well as adding absent ones.
        if(inRange[index]) entry=uint16_t(index);
    }
    for(size_t i=0;i<track.chunks.size() && i<=0x3fff;++i)
        if(inRange[i] && !listed[i]) entries.push_back(uint16_t(i));
}
inline int HighestSceneryLod(const gt2::Track& track, const std::vector<gt2::TrackLodEntry>& lods) {
    for(size_t i=0;i<lods.size();++i) {
        const auto& model=track.sceneryModels.at(lods[i].model);
        if(!model.polygons.empty() || !model.billboards.empty() || !model.glows.empty()) return int(i);
    }
    return -1;
}
// The farthest authored representation: the last step of the LOD ladder, used only when a Draw Distance extends past
// the last authored threshold.
inline int FarthestSceneryLod(const gt2::Track& track, const std::vector<gt2::TrackLodEntry>& lods) {
    for(size_t i=lods.size();i-- > 0;) {
        const auto& model=track.sceneryModels.at(lods[i].model);
        if(!model.polygons.empty() || !model.billboards.empty() || !model.glows.empty()) return int(i);
    }
    return -1;
}
// The extended distance applies to scenery independently of PS1 road-chunk masks.
inline bool ExtendedScenery(double distanceSquared, float extendedDistance, bool maxDetail) {
    return maxDetail || extendedDistance < 0 ||
        (extendedDistance > 0 && distanceSquared <= double(extendedDistance) * extendedDistance);
}
// Conservative world radius of an instance's authored model, used so a numeric distance only drops objects that are
// entirely beyond it; the renderer's far clip then cuts the part that is past the limit.
inline double SceneryInstanceRadius(const gt2::Track& track, const gt2::TrackSceneryInstance& instance) {
    double radius = 0;
    for (const gt2::TrackLodEntry& lod : track.sceneryLods.at(instance.lodList)) {
        const gt2::TrackSceneryModel& model = track.sceneryModels.at(lod.model);
        const double mu = gt2::SceneryMetresPerUnit(model);
        const double scale = std::max({std::abs(instance.scale[0]), std::abs(instance.scale[1]), std::abs(instance.scale[2])}) / 4096.0;
        const double x = std::max(std::abs(double(model.boundsMin[0])), std::abs(double(model.boundsMax[0]))) * scale * mu;
        const double y = std::max(std::abs(double(model.boundsMin[1])), std::abs(double(model.boundsMax[1]))) * scale * mu;
        const double z = std::max(std::abs(double(model.boundsMin[2])), std::abs(double(model.boundsMax[2]))) * scale * mu;
        radius = std::max(radius, std::sqrt(x * x + y * y + z * z));
    }
    return radius;
}
// A positive graphics "Draw Distance" is a hard cap: nothing farther from the eye draws. `radius` keeps an object
// whose near edge is inside the radius; 0 = the original render list, < 0 = the entire course: neither is capped.
inline bool BeyondDrawDistance(double distanceSquared, float extendedDistance, double radius = 0) {
    return extendedDistance > 0 && std::sqrt(std::max(0.0, distanceSquared)) - radius > extendedDistance;
}
inline int SceneryEntry(const gt2::Track& track, const gt2::TrackSceneryInstance& instance,
    uint32_t mask, uint32_t lodMeasure,
    double distanceSquared, float extendedDistance, bool maxDetail) {
    const bool extended = ExtendedScenery(distanceSquared, extendedDistance, maxDetail);
    if (!extended && instance.list < 32 && !(mask & (1u << instance.list))) return -1;
    const auto& lods = track.sceneryLods.at(instance.lodList);
    if (lods.empty()) return -1;
    if (maxDetail) return HighestSceneryLod(track, lods);
    // The authored LOD ladder is the original culling behaviour. A Draw Distance only caps how far it may go; when it
    // reaches past the last threshold the instance keeps its farthest authored representation, never an upgraded one.
    // An empty first LOD is intentional: that instance supplies distant course geometry, already represented by the
    // road chunks. Skipping it resurrects lifted asphalt and simplified hills through barriers in stereo.
    if (extendedDistance < 0) return 0;
    const int authored = gt2::SceneryLodIndex(lods, lodMeasure);
    if (authored >= 0) return authored;
    return extended ? FarthestSceneryLod(track, lods) : -1;
}
}
