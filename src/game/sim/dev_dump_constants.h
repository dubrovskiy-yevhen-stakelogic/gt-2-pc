#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/sim/disc_data.h"
#include "game/sim/race_sim.h"

// DEVELOPMENT ONLY. Reads the simulation constants, the course's race data and the car parameter records of a
// race from a RAM dump of the original (work/re/race_load/ram.bin, gitignored game data). The game builds all of
// these from the disc (game/sim/disc_data.*, gt2formats/car_params.*); this loader is the cross-check: gt2game
// --dump and tools/gt2verify compare the disc-derived data with what the original had in memory. The game never
// depends on this file's addresses. See dev_dump_constants.cpp for the addresses (all Sim US v1.2).
namespace gt2::sim::dev {

struct DumpRace {
    SimConstants constants;
    RaceCourseData course;
    uint8_t courseIndex = 0;          // 0x800AF230 (86 = seattle in the dump)
    std::vector<CarParams> params;    // the records of the dump's race slots (0x801DE8BA + slot * 0x1C0)
    std::vector<Car> cars;            // the car objects as dumped (0x800A9688 + i * 0xB40), for LoadState
    CarContactState contact{};        // 0x801C8608 ..
    uint16_t holdFrames = 0;          // 0x800A9520
    RaceSettings settings;            // the shell's race settings block 0x801C98A0.. as dumped
    ShellState shell;                 // the shell globals as dumped (mode block 0x801D585C.., view block, flags)
};

// Returns false with `error` set when the file cannot be read or is not a 2 MB dump.
bool LoadRaceFromDump(const std::string& path, DumpRace& out, std::string& error);
// The same on a 2 MB RAM image already in memory (a RAM dump loaded at 0x80000000).
bool LoadRaceFromImage(std::span<const uint8_t> ram, DumpRace& out, std::string& error);

// Every difference between two constant sets / race data sets, one line each ("field: a vs b"). The shell-state
// members of SimConstants (gameMode, shellControlClass, the flags) are runtime inputs and are listed separately
// by DiffShellState so that a caller can treat them as informational.
std::vector<std::string> DiffSimConstants(const SimConstants& a, const SimConstants& b);
std::vector<std::string> DiffShellState(const SimConstants& a, const SimConstants& b);
std::vector<std::string> DiffRaceCourseData(const RaceCourseData& a, const RaceCourseData& b);

} // namespace gt2::sim::dev
