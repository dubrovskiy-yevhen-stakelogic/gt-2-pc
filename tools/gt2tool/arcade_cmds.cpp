// gt2tool commands for the US Arcade v1.1 disc (docs/research/arcade_disc.md).
#include "arcade_cmds.h"

#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <vector>

#include "game/sim/car_setup.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_json.h"
#include "gt2formats/car_params.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/overlay_data.h"

namespace gt2 {

// arcade-entries <disc> <ram.bin> [table file]: the race entries of a race dump's race block (Simulation 0x801D585C + 0x5C +
// slot * 0xD0: u32 car id, u32, CarConfig at + 8, u8 + 0x8C, grid + 0x8D, kind + 0x8E, + 0x8F, name + 0x90) against the
// stock configuration of the car in the table file (default carparam/usa_arcade_data.dat), and the record the native
// builder makes of the dump's configuration against the dump's record (Simulation 0x801DE8BA + slot * 0x1C0).
int CmdArcadeEntries(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv) {
    if (argc < 4) throw std::runtime_error("arcade-entries <disc> <ram.bin> [table file]");
    const ExeProfile& profile = ProfileOf(disc);
    const std::string tablePath = argc > 4 ? argv[4] : "carparam/usa_arcade_data.dat";
    std::FILE* f = std::fopen(argv[3], "rb");
    if (!f) throw std::runtime_error(std::string("cannot read ") + argv[3]);
    std::vector<uint8_t> ram(0x200000);
    const size_t got = std::fread(ram.data(), 1, ram.size(), f);
    std::fclose(f);
    if (got != ram.size()) throw std::runtime_error("not a 2 MB RAM dump");
    auto at = [&](uint32_t address) { return ram.data() + (address & 0x1FFFFF); };
    const CarParamTables tables = CarParamTables::Load(vol, tablePath);
    std::optional<ArcadeData> arcade;
    if (tablePath == "carparam/usa_arcade_data.dat") arcade.emplace(ArcadeData::Load(vol));
    const uint32_t block = profile.Race(0x801D585Cu), records = profile.Race(0x801DE8BAu);
    const uint8_t cars = *at(profile.Race(0x800AF231u));
    std::printf("%s: race block 0x%08X, mode %u, %u car(s); tables %s\n", profile.name, block, *at(block + 0x0A), cars, tablePath.c_str());
    std::printf("race block +0x00..+0x1F:");
    for (uint32_t k = 0; k < 0x20; k++) std::printf(" %02X", *at(block + k));
    std::printf("\n");
    for (uint32_t slot = 0; slot < cars && slot < 6; slot++) {
        const uint8_t* e = at(block + 0x5C + slot * 0xD0);
        uint32_t carId;
        std::memcpy(&carId, e, 4);
        CarConfig config;
        std::memcpy(&config, e + 8, sizeof(config));
        std::printf("slot %u: car %s, +4 %02X %02X %02X %02X, +8C %u grid %u kind %u +8F %u, name \"%.16s\", flags 0x%02X torque%% %u byte79 %u word38 %u\n", slot,
                    UnpackCarId(carId).c_str(), e[4], e[5], e[6], e[7], e[0x8C], e[0x8D], e[0x8E], e[0x8F], reinterpret_cast<const char*>(e + 0x90), config.flags,
                    config.torqueMultiplier100, config.byte79, config.word38);
        try {
            const CarConfig stock = StockCarConfig(tables, carId);
            for (const std::string& d : DiffCarConfig(stock, config)) std::printf("    config vs stock: %s\n", d.c_str());
        } catch (const std::exception& ex) {
            std::printf("    no stock configuration: %s\n", ex.what());
        }
        // Which rule of the arcade race launcher gives the entry's configuration (arcade_data.h): a player car row of
        // table 32 / 33, or an opponent row of table 31 (the builder fields engineWord / exhaustByte / flags bit 1 are
        // the builder's outputs and are masked).
        if (arcade) {
            auto same = [&](CarConfig c) {
                CarConfig a = config, b = c;
                a.engineWord = b.engineWord = 0;
                a.exhaustByte = b.exhaustByte = 0;
                a.flags = uint8_t(a.flags & 0xFDu);
                b.flags = uint8_t(b.flags & 0xFDu);
                return std::memcmp(&a, &b, sizeof(a)) == 0;
            };
            std::string rules;
            if (const auto row = arcade->FindPlayerCar(carId)) {
                if (same(arcade->PlayerCarConfig(*row, kArcadePlayerCarTable))) rules += " player car row " + std::to_string(*row) + " of table 32";
                if (same(arcade->PlayerCarConfig(*row, kArcadePlayerCarTableAlt))) rules += " player car row " + std::to_string(*row) + " of table 33";
            }
            for (uint32_t n = 1; n <= arcade->OpponentCount(); n++)
                if (same(arcade->OpponentConfig(n))) rules += " opponent " + std::to_string(n);
            std::printf("    configuration = %s\n", rules.empty() ? "no arcade table row" : rules.c_str());
        }
        try {
            CarConfig built = config;
            const sim::CarParams ours = BuildCarParams(vol, tables, built);
            sim::CarParams theirs;
            std::memcpy(&theirs, at(records + slot * uint32_t(sizeof(sim::CarParams))), sizeof(theirs));
            const std::vector<std::string> diff = DiffCarParams(ours, theirs);
            std::printf("    record from the dump's configuration vs the dump's record: %zu field(s) differ\n", diff.size());
            for (size_t k = 0; k < diff.size() && k < 8; k++) std::printf("      %s\n", diff[k].c_str());
        } catch (const std::exception& ex) {
            std::printf("    builder failed: %s\n", ex.what());
        }
    }
    return 0;
}

} // namespace gt2
