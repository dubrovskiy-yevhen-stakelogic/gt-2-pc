#pragma once

#include <cstdint>
#include <string>

namespace gt2 {

struct GuestImage;

// The memory-card file names of the disc's executable (EXE 0x80091C94 game / 0x80091AB8 replay). The retail US
// build uses BASCUS-94455GAME / BASCUS-94455REPLAY; GT2+ appends 7C so its saves coexist with the original's.
struct SaveFileIds {
    std::string game = "BASCUS-94455GAME";
    std::string replay = "BASCUS-94455REPLAY";
};

// Reads both names from the image (translated to its build). Missing or unreadable names keep the defaults.
SaveFileIds ReadSaveFileIds(const GuestImage& exe);

// The names of the disc being played (set after the disc is loaded); the retail names for tools without a disc.
const SaveFileIds& ActiveSaveFileIds();
void SetActiveSaveFileIds(const SaveFileIds& ids);

} // namespace gt2
