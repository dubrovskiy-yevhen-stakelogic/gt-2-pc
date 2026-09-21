#pragma once
#include "gt2formats/track.h"
#include <algorithm>
#include <cmath>
namespace gt2view {
inline void ExtendTrackEntries(std::vector<uint16_t>& entries,const gt2::Track& track,
                               const std::array<float,3>& eye,float distance) {
    if(distance==0) return;
    std::vector<bool> inRange(track.chunks.size(),false),listed(track.chunks.size(),false);
    for(size_t i=0;i<track.chunks.size();++i) {
        double squared=0;
        for(size_t k=0;k<3;++k) {
            const double d=track.chunks[i].centre[k]/65536.0-eye[k];squared+=d*d;
        }
        inRange[i]=distance<0 || squared<=double(distance)*distance;
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
// The extended distance applies to scenery independently of PS1 road-chunk masks.
inline bool ExtendedScenery(double distanceSquared, float extendedDistance, bool maxDetail) {
    return maxDetail || extendedDistance < 0 ||
        (extendedDistance > 0 && distanceSquared <= double(extendedDistance) * extendedDistance);
}
inline int SceneryEntry(const gt2::Track& track, const gt2::TrackSceneryInstance& instance,
    uint32_t mask, uint32_t lodMeasure,
    double distanceSquared, float extendedDistance, bool maxDetail) {
    const bool extended = ExtendedScenery(distanceSquared, extendedDistance, maxDetail);
    if (!extended && instance.list < 32 && !(mask & (1u << instance.list))) return -1;
    const auto& lods = track.sceneryLods.at(instance.lodList);
    if (lods.empty()) return -1;
    // With every detailed road chunk present, use the authored near representation.
    // An empty first LOD is intentional: that instance supplies distant course
    // geometry, already represented by the road chunks. Skipping it resurrects
    // lifted asphalt and simplified hills through barriers in stereo.
    if (extendedDistance < 0) return 0;
    return extended ? HighestSceneryLod(track,lods) : gt2::SceneryLodIndex(lods, lodMeasure);
}
}
