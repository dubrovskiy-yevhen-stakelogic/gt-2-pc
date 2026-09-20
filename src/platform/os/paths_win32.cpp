// The Windows implementation of the OS services of paths.h.
#include "platform/os/paths.h"

#include <windows.h>

namespace gt2::os {

std::filesystem::path ExecutableDir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path();
}

std::filesystem::path DataRoot() { return ExecutableDir(); }

std::filesystem::path SavesDir() { return DataRoot() / "saves"; }

std::filesystem::path LogPath() { return DataRoot() / "gt2game.log"; }

uint32_t TickCountMs() { return uint32_t(GetTickCount()); }

} // namespace gt2::os
