#pragma once
#include <chrono>

namespace gt2game {
// Every call to present waits for OpenXR, samples the new pose and submits a new
// image. Advance the 60 Hz game field only after its display-time deadline.
// Never add a wall-clock sleep here: predicted display time is ahead of now.
// Sleeping to the field deadline would cap rendering to 60 and repeat XR images.
template<class Present>
bool PresentXrField(std::chrono::steady_clock::time_point deadline, std::chrono::steady_clock::time_point lastDisplay, Present&& present) {
    // A slow GPU may have already presented beyond this game field. Advance its
    // simulation/side effects without forcing another image and slowing game time.
    if (lastDisplay >= deadline) return false;
    bool between = false;
    while (present(between) < deadline) between = true;
    return true;
}
}
