#pragma once
#include <algorithm>
#include <array>
#include <cmath>
namespace gt2game {
// New application frames only, not repeated compositor submissions or physics steps.
class FrameProfiler {
public:
    void Reset() { *this = {}; }
    void Record(double now) {
        if (!started_) { last_ = now; started_ = true; return; }
        const double interval = now - last_;
        last_ = now;
        if (interval <= 0 || interval > 1.0) { Reset(); last_ = now; started_ = true; return; }
        recent_[recentNext_] = interval;
        recentNext_ = (recentNext_ + 1) % recent_.size();
        recentCount_ = std::min(recentCount_ + 1, recent_.size());
        elapsed_ += interval; ++intervals_;
        peak_ = std::max(peak_, interval);
        if (elapsed_ >= 0.5) {
            fps = double(intervals_) / elapsed_;
            frameMs = elapsed_ * 1000 / double(intervals_);
            maxMs = peak_ * 1000;
            auto sorted = recent_;
            std::sort(sorted.begin(), sorted.begin() + recentCount_);
            const size_t tail = std::max(size_t(1), (recentCount_ + 99) / 100);
            double slowSum = 0;
            for (size_t i = recentCount_ - tail; i < recentCount_; ++i) slowSum += sorted[i];
            lowFps = double(tail) / slowSum;
            recentMaxMs = sorted[recentCount_ - 1] * 1000;
            elapsed_ = peak_ = 0; intervals_ = 0;
        }
    }
    double fps = 0, frameMs = 0, maxMs = 0, lowFps = 0, recentMaxMs = 0;
    // Recent 256 application intervals; isolated hitches remain visible beyond the half-second FPS update.
private:
    std::array<double, 256> recent_{};
    size_t recentNext_ = 0, recentCount_ = 0;
    bool started_ = false;
    double last_ = 0, elapsed_ = 0, peak_ = 0;
    unsigned intervals_ = 0;
};
}
