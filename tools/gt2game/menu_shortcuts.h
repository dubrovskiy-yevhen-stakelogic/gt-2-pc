#pragma once
#include "platform/input/ps1_pad.h"

namespace gt2game {
struct MenuShortcutState {
    bool pause = false;
    bool settings = false;
    bool menu = false;

    void Update(uint16_t buttons, bool escapeHeld, bool escapePressed, bool focused) {
        pause = settings = menu = false;
        if (!focused) return;
        using namespace gt2::input::ps1;
        const bool start = (buttons & kStart) != 0;
        const bool select = (buttons & kSelect) != 0;
        const bool chord = start && select;
        settings = chord && !chordHeld_;
        if (chord) suppressStart_ = true;
        menu = start && !startHeld_ && !chord && !suppressStart_;
        const bool down = escapeHeld || escapePressed || start;
        pause = down && !pauseHeld_ && !chord && !suppressStart_;
        pauseHeld_ = down;
        chordHeld_ = chord;
        startHeld_ = start;
        if (!start && !select) suppressStart_ = false;
    }
private:
    bool pauseHeld_ = false;
    bool chordHeld_ = false;
    bool suppressStart_ = false;
    bool startHeld_ = false;
};
}
