#include "game/shell/title_attract.h"

#include <span>
#include <stdexcept>

#include "gt2formats/exe_profile.h"
#include "gt2vfs/gtfs.h"

namespace gt2::shell {

TitleAttract::Step TitleAttract::Next(int demoCount) {
    Step s;
    if (arcade_) { // 0x8001156C: lb +0x10, + 1, >= 4 -> 0; 0 -> 0x8005D9AC(5) (does not return: the index stays)
        cycle = cycle + 1 < 4 ? cycle + 1 : 0;
        if (cycle == 0) {
            s.movie = true;
            return s;
        }
    }
    if (demoCount <= 0) throw std::runtime_error("attract: the demo file holds no replay");
    s.demo = demoIndex; // 0x80020E14 / 0x80020A98(file, 0x801055C0 / 0x801052C0, index)
    demoIndex = demoIndex + 1 < demoCount ? demoIndex + 1 : 0;
    return s;
}

std::string AttractDemoFilePath(const GuestImage& ovl1, const GtfsVolume& vol, uint8_t language) {
    if (ovl1.profile && (ovl1.profile->build == ExeBuild::kArcadeEu || ovl1.profile->build == ExeBuild::kSimEu)) {
        const std::string path = "arcade/demofile_eu.gmr.gz";
        if (!vol.Find(path)) throw std::runtime_error("attract: European demo file is missing");
        return path;
    }
    const bool arcade = ovl1.profile && ovl1.profile->arcade;
    const uint32_t table = arcade ? 0x8004BD7Cu : 0x8004C8A8u; // 0x80020A50 / 0x80020DCC: lbu career + 0 (language), lw table[language]
    const uint32_t id = ovl1.Get<uint32_t>(UiAddress(ovl1, table, arcade) + uint32_t(language) * 4);
    // The boot's file tables (u16 id -> VOL record), the entries of the three demo files.
    struct Map { uint32_t id, record; };
    static constexpr Map kSim[] = {{0x23, 0x2B}, {0x24, 0x2C}, {0x25, 0x2D}};
    static constexpr Map kArcade[] = {{0x1C, 0x24}, {0x1D, 0x25}, {0x1E, 0x26}};
    uint32_t record = 0;
    for (const Map& m : arcade ? std::span<const Map>(kArcade) : std::span<const Map>(kSim))
        if (m.id == id) record = m.record;
    if (record == 0) throw std::runtime_error("attract: demo file id " + std::to_string(id) + " of language " + std::to_string(language) + " is not known");
    for (const GtfsEntry& e : vol.Files())
        if (e.index == record) return e.path;
    throw std::runtime_error("attract: no VOL entry " + std::to_string(record));
}

} // namespace gt2::shell
