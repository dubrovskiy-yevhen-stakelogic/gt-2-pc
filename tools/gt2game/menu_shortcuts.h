#pragma once
#include "platform/input/ps1_pad.h"
#include <chrono>

namespace gt2game {
// Menu-owned input remains consumed until the physical control is released.
// Clearing only the pressed edge still lets the game's held/repeat reader see B.
struct MenuInputRelease {
    std::array<uint16_t,2> buttons{};
    std::array<bool,2> leftTrigger{},rightTrigger{};
    std::array<bool,256> keys{};
    void Block(const gt2::input::Ps1PadFrame& pad,size_t port) {
        buttons[port]|=pad.buttons;
        leftTrigger[port]=pad.pressureL2>8;rightTrigger[port]=pad.pressureR2>8;
    }
    gt2::input::Ps1PadFrame Filter(gt2::input::Ps1PadFrame pad,size_t port) {
        buttons[port]&=pad.buttons;
        pad.buttons&=uint16_t(~buttons[port]);
        leftTrigger[port]=leftTrigger[port] && pad.pressureL2>8;
        rightTrigger[port]=rightTrigger[port] && pad.pressureR2>8;
        if(leftTrigger[port])pad.pressureL2=0;
        if(rightTrigger[port])pad.pressureR2=0;
        return pad;
    }
    void UpdateKey(int key,bool held) { if(key>0 && key<256)keys[size_t(key)]=keys[size_t(key)] && held; }
    bool KeyBlocked(int key) const { return key>0 && key<256 && keys[size_t(key)]; }
};
struct MenuShortcutState {
    bool pause = false;
    bool settings = false;
    bool menu = false;

    void Update(uint16_t buttons, bool escapeHeld, bool escapePressed, bool focused,
                std::chrono::steady_clock::time_point now=std::chrono::steady_clock::now()) {
        pause = settings = menu = false;
        if (!focused) {holdActive_=false;return;}
        using namespace gt2::input::ps1;
        const bool start = (buttons & kStart) != 0;
        const bool select = (buttons & kSelect) != 0;
        const bool chord = start && select;
        settings = chord && !chordHeld_;
        if (chord) suppressStart_ = true;
        if(start && !startHeld_ && !chord && !suppressStart_) {holdSince_=now;holdActive_=true;longTriggered_=false;}
        if(!start || chord)holdActive_=false;
        if(holdActive_ && !longTriggered_ && now-holdSince_>=std::chrono::seconds(1)) {
            settings=true;longTriggered_=true;suppressStart_=true;
        }
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
    bool holdActive_=false,longTriggered_=false;
    std::chrono::steady_clock::time_point holdSince_{};
};
}
