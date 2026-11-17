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

// The action that had `key` is left unbound, so one key never drives two functions.
inline void AssignKeyboardKey(KeyboardBindings& bindings, KeyboardAction action, int key) {
    for (int& other : bindings.key)
        if (other == key) other = 0;
    bindings[action] = key;
}

// The keyboard's driving buttons and pedals. Shift and camera are presses, not held functions, so they stay with the
// callers (`held` is GameWindow::KeyHeld).
template <class Held>
LogicalPad KeyboardDrivingPad(const KeyboardBindings& bindings, Held&& held) {
    LogicalPad pad;
    if (held(bindings[KeyboardAction::kLeft])) pad.buttons |= gt2::kPadLeft;
    if (held(bindings[KeyboardAction::kRight])) pad.buttons |= gt2::kPadRight;
    if (held(bindings[KeyboardAction::kAccelerate])) pad.buttons |= gt2::kPadThrottle;
    if (held(bindings[KeyboardAction::kBrake])) pad.buttons |= gt2::kPadBrake;
    if (held(bindings[KeyboardAction::kHandbrake])) pad.buttons |= gt2::kPadHandbrake;
    if (held(bindings[KeyboardAction::kReverse])) pad.buttons |= gt2::kPadReverse;
    return pad;
}

} // namespace gt2::input
