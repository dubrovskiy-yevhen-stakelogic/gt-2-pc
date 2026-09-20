#pragma once
#include <cstdint>
namespace gt2view {
// Full-rate central ellipse, 2x2 shading outside. UI uses a separate full-rate pipeline.
inline uint8_t FoveationRate(uint32_t x, uint32_t y, uint32_t width, uint32_t height, int level) {
    if (level <= 0) return 0;
    const float nx = (float(x) + 0.5f) * 2 / float(width) - 1;
    const float ny = (float(y) + 0.5f) * 2 / float(height) - 1;
    const float radius = level == 1 ? 0.85f : level == 2 ? 0.65f : 0.45f;
    return nx * nx + ny * ny > radius * radius ? 5 : 0;
}
}