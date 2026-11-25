#pragma once
#include "platform/input/ps1_pad.h"
namespace gt2game {
inline bool CockpitFilterPressed(uint16_t pressed, uint16_t held) {
    using namespace gt2::input::ps1;
    // Quest Touch emits Select together with L3. Start/R3 are menu chords.
    return (pressed & kL3) && !(held & (kStart | kR3));
}
}
