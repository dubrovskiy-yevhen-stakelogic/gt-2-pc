#include "platform/os/paths.h"
#include <SDL.h>
#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace gt2::os {
namespace {
std::filesystem::path TakePath(char* raw) {
    if (!raw) throw std::runtime_error(SDL_GetError());
    const std::filesystem::path result(raw);
    SDL_free(raw);
    return result;
}
}
std::filesystem::path ExecutableDir() { return TakePath(SDL_GetBasePath()); }
std::filesystem::path DataRoot() {
    if (const char* path = std::getenv("GT2_DATA_ROOT"); path && *path) return std::filesystem::absolute(path);
    return TakePath(SDL_GetPrefPath("", "GT2"));
}
std::filesystem::path SavesDir() {
    if (const char* path = std::getenv("GT2_SAVE_ROOT"); path && *path) return std::filesystem::absolute(path);
    return DataRoot() / "saves";
}
std::filesystem::path LogPath() { return DataRoot() / "gt2game.log"; }
uint32_t TickCountMs() {
    return uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace gt2::os
