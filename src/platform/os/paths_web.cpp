#include "platform/os/paths.h"
#include <emscripten.h>
#include <cstdlib>

namespace gt2::os {
std::filesystem::path ExecutableDir() { return "/"; }
std::filesystem::path DataRoot() { return "/data"; }
std::filesystem::path SavesDir() { return "/saves"; }
std::filesystem::path LogPath() { return "/tmp/gt2game.log"; }
uint32_t TickCountMs() { return uint32_t(emscripten_get_now()); }
}
