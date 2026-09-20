// The Android implementation of the OS services of paths.h (docs/research/vr_port_plan.md, M6).
//
// Android gives an application two directories and no working directory worth the name:
//   - the app-specific EXTERNAL files directory (/sdcard/Android/data/<package>/files) - readable and writable over
//     adb without any permission, which is where the user's disc image (and later the prepared pack) is pushed. It is
//     the game's DataRoot.
//   - the INTERNAL data directory (/data/data/<package>/files) - not visible to the shell, which is where the memory
//     cards, settings.txt and the log belong (SavesDir / LogPath).
// android_main hands both to SetAndroidDirs before the game starts; nothing here guesses a path.
#include "platform/os/paths.h"

#include <sys/stat.h>
#include <time.h>

#include <stdexcept>
#include <string>

namespace gt2::os {
namespace {

std::string g_external, g_internal;

void MakeDir(const std::string& path) {
    if (path.empty()) return;
    ::mkdir(path.c_str(), 0770);
}

const std::string& External() {
    if (g_external.empty()) throw std::runtime_error("gt2::os: the Android directories were never set (android_main must call SetAndroidDirs)");
    return g_external;
}

const std::string& Internal() {
    if (g_internal.empty()) throw std::runtime_error("gt2::os: the Android directories were never set (android_main must call SetAndroidDirs)");
    return g_internal;
}

} // namespace

void SetAndroidDirs(const char* externalFiles, const char* internalData) {
    g_external = externalFiles ? externalFiles : "";
    g_internal = internalData ? internalData : "";
    MakeDir(g_external);
    MakeDir(g_internal);
    MakeDir(g_internal + "/saves");
}

// There is no executable directory on Android: the code lives in libgt2game.so inside the APK. The data root is the
// closest thing the game's own code means by it (mods and the disc image are looked for there).
std::filesystem::path ExecutableDir() { return std::filesystem::path(External()); }

std::filesystem::path DataRoot() { return std::filesystem::path(External()); }

std::filesystem::path SavesDir() { return std::filesystem::path(Internal()) / "saves"; }

std::filesystem::path LogPath() { return std::filesystem::path(Internal()) / "gt2game.log"; }

uint32_t TickCountMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint32_t(uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec) / 1000000u);
}

} // namespace gt2::os
