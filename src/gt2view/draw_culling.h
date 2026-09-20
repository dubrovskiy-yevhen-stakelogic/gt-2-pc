#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace gt2view {
struct DrawBounds {
    float lo[3]{}, hi[3]{};
    bool valid = false;
};

// Cached in object coordinates; transforms and eye poses may change every frame.
class DrawBoundsCache {
public:
    template<class Vertex> const DrawBounds& Get(const Vertex* vertices, uint32_t first, uint32_t count) {
        const uint64_t key = (uint64_t(first) << 32) | count;
        auto [it, added] = bounds_.try_emplace(key);
        if (!added || !count) return it->second;
        auto& b = it->second;
        for (int k = 0; k < 3; ++k) b.lo[k] = b.hi[k] = vertices[first].pos[k];
        b.valid = true;
        for (uint32_t i = 0; i < count; ++i) for (int k = 0; k < 3; ++k) {
            const float v = vertices[first + i].pos[k];
            if (!std::isfinite(v)) b.valid = false;
            b.lo[k] = std::min(b.lo[k], v); b.hi[k] = std::max(b.hi[k], v);
        }
        return b;
    }
    void Invalidate(uint32_t first, uint32_t count) {
        if (!count) return;
        for (auto it = bounds_.begin(); it != bounds_.end();) {
            const uint32_t start = uint32_t(it->first >> 32), size = uint32_t(it->first);
            if (uint64_t(start) < uint64_t(first) + count && uint64_t(first) < uint64_t(start) + size)
                it = bounds_.erase(it);
            else ++it;
        }
    }
private:
    std::unordered_map<uint64_t, DrawBounds> bounds_;
};

// Vulkan side planes in object space. Deliberately retain near/far intersections:
// reversed-Z depth bias and the infinite far plane must not erase road decals.
inline bool BoundsInView(const DrawBounds& b, const float* eyeVP, const float* model) {
    if (!b.valid) return true;
    float clip[16]{};
    for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) clip[c * 4 + r] += eyeVP[k * 4 + r] * model[c * 4 + k];
    for (int axis = 0; axis < 2; ++axis) for (float sign : {-1.0f, 1.0f}) {
        float maximum = clip[15] + sign * clip[12 + axis];
        float tolerance = 1e-4f * (1 + std::abs(maximum));
        for (int k = 0; k < 3; ++k) {
            const float p = clip[k * 4 + 3] + sign * clip[k * 4 + axis];
            const float v = p * (p >= 0 ? b.hi[k] : b.lo[k]);
            maximum += v; tolerance += 1e-4f * std::abs(v);
        }
        if (maximum < -tolerance) return false;
    }
    return true;
}
inline bool BoundsInStereo(const DrawBounds& b, const float eyes[2][16], const float* model) {
    return BoundsInView(b, eyes[0], model) || BoundsInView(b, eyes[1], model);
}
}
