#pragma once
// Player 1's keyboard as a second driving map (the pad's map is the original's key table, ps1_pad.h): every driving
// function has a key, so the keyboard can be rebound without touching the controller's bindings. The defaults are the
// keys the race used before this map existed (race_view.cpp / split_race.cpp). Menu keys stay fixed.
#include <array>
#include <cstddef>
#include <iterator>

#include "gt2formats/replay.h"
#include "platform/os/keys.h"

namespace gt2::input {

enum class KeyboardAction : int {
    kLeft, kRight, kAccelerate, kBrake, kHandbrake, kReverse, kShiftUp, kShiftDown, kCamera, kLookBack, kCount
};

inline constexpr const char* kKeyboardActionNames[] = {
    "Steer left", "Steer right", "Accelerator", "Brake", "Handbrake", "Reverse", "Shift up", "Shift down", "Camera", "Look back",
};
static_assert(std::size(kKeyboardActionNames) == std::size_t(KeyboardAction::kCount));

struct KeyboardBindings {
    // 0 = unbound. A key code of platform/os/keys.h (Win32 virtual-key numbers on every platform).
    std::array<int, std::size_t(KeyboardAction::kCount)> key{
        gt2::keys::kLeft, gt2::keys::kRight, gt2::keys::kUp, gt2::keys::kDown, gt2::keys::kSpace, 'R', 'Q', 'A', 'C', 'V'};
    int& operator[](KeyboardAction action) { return key[std::size_t(action)]; }
    int operator[](KeyboardAction action) const { return key[std::size_t(action)]; }
};

// Global menu/reset/save shortcuts remain available independently of driving bindings.
// Limit stored bindings to the portable key set supported by all desktop backends.
inline bool KeyboardKeyAllowed(int key) {
    if (key == 0) return true; // unbound
    if (key == keys::kEscape || key == keys::kBack || key == keys::kReturn ||
        key == keys::kF5 || key == keys::kF8 || key == keys::kF10 || key == 'G') return false;
    if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
        (key >= keys::kF1 && key <= keys::kF12)) return true;
    switch (key) {
    case keys::kTab: case keys::kShift: case keys::kControl: case keys::kAlt:
    case keys::kSpace: case keys::kPageUp: case keys::kPageDown: case keys::kEnd:
    case keys::kHome: case keys::kLeft: case keys::kUp: case keys::kRight: case keys::kDown:
    case keys::kInsert: case keys::kDelete: return true;
    default: return false;
    }
}

// The action that had `key` is left unbound, so one key never drives two functions.
inline bool AssignKeyboardKey(KeyboardBindings& bindings, KeyboardAction action, int key) {
    if (!KeyboardKeyAllowed(key)) return false;
    for (int& other : bindings.key)
        if (other == key) other = 0;
    bindings[action] = key;
    return true;
}

// The keyboard's driving buttons and pedals. Shift and camera are presses, not held functions, so they stay with the
// callers (`held` is GameWindow::KeyHeld).
template <class Held>
LogicalPad KeyboardDrivingPad(const KeyboardBindings& bindings, Held&& held) {
    LogicalPad pad;
    const auto down = [&](KeyboardAction action) { return bindings[action] != 0 && held(bindings[action]); };
    if (down(KeyboardAction::kLeft)) pad.buttons |= gt2::kPadLeft;
    if (down(KeyboardAction::kRight)) pad.buttons |= gt2::kPadRight;
    if (down(KeyboardAction::kAccelerate)) pad.buttons |= gt2::kPadThrottle;
    if (down(KeyboardAction::kBrake)) pad.buttons |= gt2::kPadBrake;
    if (down(KeyboardAction::kHandbrake)) pad.buttons |= gt2::kPadHandbrake;
    if (down(KeyboardAction::kReverse)) pad.buttons |= gt2::kPadReverse;
    return pad;
}

} // namespace gt2::input
