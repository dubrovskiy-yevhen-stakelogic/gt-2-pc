#pragma once
#include "../tools/gt2game/frame_profiler_log.h"
#include <fstream>
#include <iterator>

template<class Check>
void FrameProfilerLogChecks(Check check) {
    namespace fs = std::filesystem;
    const auto root = fs::temp_directory_path() / ("gt2-profiler-check-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    auto read = [](const fs::path& path) {
        std::ifstream in(path);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    gt2game::FrameProfilerLog log;
    gt2game::ProfilerFrame frame; frame.now = 10; frame.previousGpuMs = 7.5; frame.mirrorDraws = 43;
    frame.cockpitMirror = 0; frame.cockpitMirrorScale = 25;
    gt2game::FrameProfiler stats;
    log.SetEnabled(false, root); log.Record(frame, stats);
    check(!fs::exists(root), "disabled profiler creates no file or directory");
    log.SetEnabled(true, root);
    const auto first = log.Path();
    check(!log.Failed() && fs::exists(first), "enabling creates a session CSV");
    log.Record(frame, stats); frame.now = 11.1; log.Record(frame, stats);
    auto contents = read(first);
    check(std::count(contents.begin(), contents.end(), '\n') == 3, "active log flushes within one second");
    check(contents.find("gpu_previous_ms") != std::string::npos && contents.find("7.500") != std::string::npos,
          "CSV identifies delayed GPU timing and records measured values");
    check(contents.find("cockpit_mirror_enabled,cockpit_mirror_scale\n") != std::string::npos &&
          contents.find(",0,25\n") != std::string::npos,
          "CSV records cockpit mirror preferences separately from the HUD master setting");
    frame.now = 11.2; log.Record(frame, stats); log.SetEnabled(false);
    contents = read(first);
    check(std::count(contents.begin(), contents.end(), '\n') == 4, "disabling flushes the last buffered frame");
    log.Record(frame, stats);
    check(read(first) == contents, "disabled logger stops recording");
    log.SetEnabled(true, root);
    const auto second = log.Path();
    check(second != first && read(first) == contents, "re-enabling preserves the previous session");
    frame.now = 20; log.Record(frame, stats); log.SetEnabled(false);
    check(read(second).find("0,0.000,-1.000") != std::string::npos, "new session resets its frame clock");
    log.SetEnabled(true, first / "not-a-directory");
    check(log.Failed(), "unwritable destination is visible instead of silently dropping logs");
    log.SetEnabled(false);
    fs::remove(first); fs::remove(second); fs::remove(root);
}
