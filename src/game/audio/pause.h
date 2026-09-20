#pragma once
#include <atomic>

namespace gt2::audio {
// A modal PC overlay suspends every mixer while retaining voice and stream cursors.
inline std::atomic<unsigned> mixPauseDepth{0};
class ScopedMixPause {
public:
    ScopedMixPause() { ++mixPauseDepth; }
    ~ScopedMixPause() { --mixPauseDepth; }
    ScopedMixPause(const ScopedMixPause&) = delete;
    ScopedMixPause& operator=(const ScopedMixPause&) = delete;
};
}
