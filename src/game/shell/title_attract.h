#pragma once
// The title's attract cycle: title result 6 (member 1's list view ends it after 900 fields without a held button, TitleMenu
// kIdleFields) and the entry's case 6 of the jump table on 0x801EF5F3 (Simulation) / 0x801EF023 (Arcade).
//
// US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), member 1 0x80011624: the demo file of
// the language (0x80020DCC: id *(0x8004C8A8 + language * 4)) is read to 0x800E15C0, replay *0x801EF5FE of it is gathered to
// 0x801055C0 (0x80020E14), 0x801EF5FE = (index + 1) mod (the file's replay count, +0x200), 0x801EF5F1 = 0x801EF5F2 = 0; then
// 0x80010EDC and the race overlay with argument 1 (0x8005DA7C(0, 0x80011F64, 1)): the replay plays until its stream runs out,
// then member 1 starts again with the title list (0x801EF5F2 = 0).
// US Arcade v1.1 (SCUS_944.55, EXE SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95), member 1 0x8001156C: the same, but first
// 0x801EF030 = (0x801EF030 + 1) mod 4 and when that is 0, member 5 (0x8005D9AC(5): the intro movie, then member 1) instead -
// the demo index is not advanced in that cycle. Demo file table 0x8004BD7C (= Simulation 0x8004C8A8 - 0xB2C), file 0x800E12C0,
// index 0x801EF02E, gather 0x80020A98, race 0x8005D9EC(0, 0x80011F64, 1).
// Boot (EXE 0x8001083C arcade / the Simulation equivalent): the index and the counter start at 0, so the order is
// Simulation: demo 0, 1, 2, ... (wrapping); Arcade: demo 0, 1, 2, intro, demo 3, 4, 5, intro, demo 6, 0, 1, intro, ...
// Observed on the original in our interpreter (gt2run session, no input): Simulation title at field 547, demo 0 gathered at
// 1567, back in member 1 at 18150, demo 1 at 19147, member 1 at 25511, demo 2 at 26508 (docs/formats/title.md section 11).
#include <cstdint>
#include <string>

#include "gt2formats/overlay_data.h"

namespace gt2 {
class GtfsVolume;
}

namespace gt2::shell {

class TitleAttract {
public:
    explicit TitleAttract(bool arcade) : arcade_(arcade) {}

    struct Step {
        bool movie = false; // the arcade's every fourth cycle: member 5 (the intro)
        int demo = -1;      // else the replay of the demo file to play
    };
    // One attract (case 6): what it plays; `demoCount` = the demo file's replay count (+0x200).
    Step Next(int demoCount);

    int demoIndex = 0; // 0x801EF5FE (Simulation) / 0x801EF02E (Arcade)
    int cycle = 0;     // 0x801EF030 (Arcade only)

private:
    bool arcade_;
};

// The VOL path of the language's demo file, both builds: the id of member 1's table (Simulation 0x8004C8A8, Arcade 0x8004BD7C)
// through the boot's file table (Simulation 0x801E2EF0: 0x23 / 0x24 / 0x25 -> VOL records 0x2B / 0x2C / 0x2D; Arcade 0x801E2950:
// 0x1C / 0x1D / 0x1E -> 0x24 / 0x25 / 0x26, read from the RAM of work/re/arcade_menu) to the VOL entry of that record number.
std::string AttractDemoFilePath(const GuestImage& ovl1, const GtfsVolume& vol, uint8_t language);

} // namespace gt2::shell
