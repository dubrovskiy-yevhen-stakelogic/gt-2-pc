#pragma once
// The OS services the game needs besides its window, its renderer and its sound device: where its files live, and a
// monotonic millisecond counter. One implementation per platform - paths_win32.cpp today; the Quest build adds
// paths_android.cpp, where the data root is the application's external files directory (the installer pushes the
// prepared pack there) and the saves live in the internal data directory.
#include <cstdint>
#include <filesystem>

namespace gt2::os {

// The directory the running executable sits in.
std::filesystem::path ExecutableDir();
// Where the game's data lives (disc image, mods): the executable's directory on Windows.
std::filesystem::path DataRoot();
// Where the memory cards and settings.txt live: <data root>/saves.
std::filesystem::path SavesDir();
// Where a log file belongs: <data root>/gt2game.log (Android: the internal data directory, next to the saves).
std::filesystem::path LogPath();
// Milliseconds since the system started (GetTickCount on Windows): the seed of the runs that are not deterministic.
uint32_t TickCountMs();

#if defined(__ANDROID__)
// Android only: the two directories the operating system hands the activity. Called once by android_main before
// anything else runs - `externalFiles` is ANativeActivity::externalDataPath (the data root the installer pushes the
// disc / the pack into), `internalData` is ANativeActivity::internalDataPath (saves and the log). Both are created.
void SetAndroidDirs(const char* externalFiles, const char* internalData);
#endif

} // namespace gt2::os
