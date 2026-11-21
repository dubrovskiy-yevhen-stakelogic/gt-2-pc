#include "gt2formats/save_ids.h"

#include "gt2formats/overlay_data.h"

namespace gt2 {
namespace {

std::string CardNameAt(const GuestImage& exe, uint32_t simAddress) {
    try {
        const uint8_t* p = exe.At(exe.Sim(simAddress), 20);
        size_t n = 0;
        while (n < 20 && p[n] != 0) ++n;
        if (n < 4) return {};
        std::string name(reinterpret_cast<const char*>(p), n);
        for (char c : name)
            if (c < 0x20 || c > 0x7E) return {};
        return name;
    } catch (...) {
        return {};
    }
}

SaveFileIds g_active;

} // namespace

SaveFileIds ReadSaveFileIds(const GuestImage& exe) {
    SaveFileIds ids;
    if (std::string game = CardNameAt(exe, 0x80091C94u); !game.empty()) ids.game = std::move(game);
    if (std::string replay = CardNameAt(exe, 0x80091AB8u); !replay.empty()) ids.replay = std::move(replay);
    return ids;
}

const SaveFileIds& ActiveSaveFileIds() { return g_active; }
void SetActiveSaveFileIds(const SaveFileIds& ids) { g_active = ids; }

} // namespace gt2
