#pragma once
#include "platform/input/ps1_pad.h"
// The Android events the game's window backend receives (tools/gt2game/game_window_android.cpp) on their way to the
// virtual PS1 controller of input_system.h (docs/research/vr_port_plan.md, M6).
//
// A NativeActivity gets its input through the looper's AInputQueue: key events from a Bluetooth gamepad, a Bluetooth
// keyboard or the headset's own buttons, and motion events of the gamepad's sticks and triggers. The window backend
// decodes them and calls these two functions; input_system_android.cpp keeps the state and presents it as a DualShock
// (type 7, the XInput mapping of the PC build). The Touch controllers are not here - they are XR actions (M5).
//
// Both functions may be called from the looper thread while the game polls from its own; the state is atomic.
#include <cstdint>

namespace gt2::input::android {
void Reset(); // Before the new activity starts dispatching input.

// One key event of a gamepad (AKEYCODE_BUTTON_*, AKEYCODE_DPAD_*). Ignores keys that are not part of a gamepad.
void KeyEvent(int32_t keyCode, bool down);

// The gamepad's axes of one motion event, in the Android convention: sticks -1..1 (x right, y down), triggers 0..1,
// the hat -1..1 (the D-pad of a gamepad that reports it as an axis).
struct Axes {
    float leftX = 0, leftY = 0, rightX = 0, rightY = 0;
    float leftTrigger = 0, rightTrigger = 0;
    float hatX = 0, hatY = 0;
};
void MotionEvent(const Axes& axes);

// True once any gamepad event has arrived: the InputSystem presents a controller in port 1 from then on (before that
// there is honestly none, and the game shows no pad).
bool PadSeen();
void SetXrPad(const Ps1PadFrame& pad);
float XrVibration();

} // namespace gt2::input::android
