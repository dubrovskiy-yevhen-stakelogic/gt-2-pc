#pragma once
#include <array>
#include <cmath>
namespace gt2view {
// Cylindrical billboard axis from position, shared by both eyes. Looking around
// without moving the viewer leaves the tree orientation unchanged.
inline std::array<float, 3> BillboardRight(const std::array<float, 3> &eye, const std::array<float, 3> &base, const std::array<float, 3> &fallback) {
    const float x = eye[0] - base[0], z = eye[2] - base[2], length = std::hypot(x, z);
    return length > 1e-5f ? std::array<float, 3>{z / length, 0, -x / length} : fallback;
}
} // namespace gt2view
