#pragma once
#include <algorithm>
#include <chrono>

namespace gt2game {
// Presentation owns a persistent deadline. Do not round it to a new future
// DWM blank every field: that can defer every image while physics keeps going.
inline bool DesktopFrameDue(std::chrono::steady_clock::time_point now,
                            std::chrono::steady_clock::time_point fieldDeadline,
                            std::chrono::steady_clock::time_point presentDeadline, bool first) {
    return std::max(now, presentDeadline) < fieldDeadline || (first && presentDeadline <= now);
}
inline std::chrono::steady_clock::time_point NextDesktopPresent(std::chrono::steady_clock::time_point slot,
                                                               std::chrono::steady_clock::time_point after,
                                                               std::chrono::steady_clock::duration period) {
    return period > period.zero() ? std::max(slot + period, after - period / 2) : after;
}
}
