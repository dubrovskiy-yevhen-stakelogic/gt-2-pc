#pragma once
#include "frame_profiler.h"
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <string>

namespace gt2game {
struct ProfilerFrame {
    double now = 0, waitMs = -1, buildMs = -1, submitMs = -1, endMs = -1, previousGpuMs = -1;
    unsigned draws = 0, sceneDraws = 0, mirrorDraws = 0, vertices = 0;
    unsigned cached = 0, uncached = 0, tested = 0, culled = 0;
    unsigned width = 0, height = 0, msaa = 1;
    int stereo = 0, overlay = 0, foveation = 0, refresh = 0, drawDistance = 0;
    int cockpit = 0, mirror = 0, smooth = 0, hd = 0;
    int cockpitMirror = 1, cockpitMirrorScale = 100;
};

// Buffered frame records follow the visible profiler switch. Each enable starts
// a new file; disabling or normal shutdown closes it without erasing older runs.
class FrameProfilerLog {
public:
    ~FrameProfilerLog() { Close(); }
    FrameProfilerLog() = default;
    FrameProfilerLog(const FrameProfilerLog&) = delete;
    FrameProfilerLog& operator=(const FrameProfilerLog&) = delete;
    bool Enabled() const { return enabled_; }
    bool Failed() const { return failed_; }
    const std::filesystem::path& Path() const { return path_; }

    void SetEnabled(bool enabled, const std::filesystem::path& directory = {}) {
        if (enabled == enabled_) return;
        Close(); enabled_ = enabled;
        if (!enabled) return;
        failed_ = false;
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) { Fail(); return; }
        const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        path_ = directory / ("profiler-" + std::to_string(stamp) + ".csv");
        // Exclusive create: never truncate an earlier session, even on a clock collision.
        file_ = std::fopen(path_.string().c_str(), "wx");
        if (!file_) { Fail(); return; }
        std::setvbuf(file_, buffer_, _IOFBF, sizeof(buffer_));
        std::fputs("frame,time_ms,app_frame_ms,wait_begin_ms,build_ms,render_submit_ms,end_frame_ms,gpu_previous_ms,"
                   "app_fps,avg_frame_ms,max_frame_ms,low_1pct_fps,recent_max_ms,draws,scene_draws,mirror_source_draws,vertices,"
                   "cached_materials,uncached_materials,cull_tested,cull_rejected,width,height,msaa,stereo,overlay,"
                   "foveation,refresh_requested_hz,draw_distance,cockpit_enabled,mirror_enabled,smooth,hd,"
                   "cockpit_mirror_enabled,cockpit_mirror_scale\n", file_);
        if (std::fflush(file_) != 0) { Fail(); return; }
        std::printf("profiler: recording %s\n", path_.string().c_str());
        rows_ = 0; start_ = last_ = flushed_ = -1;
    }

    void Record(const ProfilerFrame& f, const FrameProfiler& stats) {
        if (!file_) return;
        if (start_ < 0) start_ = flushed_ = f.now;
        const double interval = last_ < 0 ? -1 : (f.now - last_) * 1000;
        last_ = f.now;
        const int result = std::fprintf(file_,
            "%llu,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,"
            "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
            static_cast<unsigned long long>(rows_++), (f.now-start_)*1000, interval,
            f.waitMs, f.buildMs, f.submitMs, f.endMs, f.previousGpuMs,
            stats.fps, stats.frameMs, stats.maxMs, stats.lowFps, stats.recentMaxMs,
            f.draws, f.sceneDraws, f.mirrorDraws, f.vertices, f.cached, f.uncached, f.tested, f.culled,
            f.width, f.height, f.msaa, f.stereo, f.overlay, f.foveation, f.refresh, f.drawDistance,
            f.cockpit, f.mirror, f.smooth, f.hd, f.cockpitMirror, f.cockpitMirrorScale);
        if (result < 0 || std::ferror(file_)) { Fail(); return; }
        // Keep at most one second of buffered data without a per-frame flush.
        if (f.now-flushed_ >= 1) {
            if (std::fflush(file_) != 0) { Fail(); return; }
            flushed_ = f.now;
        }
    }
private:
    void Close() {
        if (file_ && std::fclose(file_) != 0) {
            failed_ = true;
            std::fprintf(stderr, "profiler: cannot close CSV %s\n", path_.string().c_str());
        }
        file_ = nullptr;
    }
    void Fail() {
        std::fprintf(stderr, "profiler: cannot write CSV %s\n", path_.string().c_str());
        Close(); failed_ = true;
    }
    FILE* file_ = nullptr;
    char buffer_[16384]{};
    bool enabled_ = false, failed_ = false;
    uint64_t rows_ = 0;
    double start_ = -1, last_ = -1, flushed_ = -1;
    std::filesystem::path path_;
};
}
