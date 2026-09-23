#pragma once
#include <array>
#include <cstdint>

namespace gt2view {
// Exact RGB555 colour shared by textured body lining and the procedural shell.
inline constexpr uint16_t kCockpitTrimColor = 3 | (3 << 5) | (3 << 10);
inline constexpr std::array<float,3> kCockpitInteriorRgb{3.f/31,3.f/31,3.f/31};
}
