#pragma once
namespace gt2view {
// The extended distance applies to scenery independently of PS1 road-chunk masks.
inline bool ExtendedScenery(double distanceSquared, float extendedDistance, bool maxDetail) {
    return maxDetail || extendedDistance < 0 ||
        (extendedDistance > 0 && distanceSquared <= double(extendedDistance) * extendedDistance);
}
}