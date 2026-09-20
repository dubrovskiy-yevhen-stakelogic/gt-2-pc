#pragma once
// What android_main (android/app/src/main/cpp/android_main.cpp) tells the game's window backend
// (game_window_android.cpp) about the application it lives in (docs/research/vr_port_plan.md, M6).
//
// The looper thread of the NativeActivity owns the lifecycle and the input queue; the game runs on its own thread
// (GameMain). Everything below is written by the looper thread and read by the game's thread.
#include <cstdint>

namespace gt2game::android {

// The Java objects the OpenXR loader and the runtime need: ANativeActivity::vm and ANativeActivity::clazz. Set once,
// before GameMain runs; without them the XR backend refuses to create a session.
void SetHost(void* javaVm, void* activity);
void* JavaVm();
void* Activity();

// APP_CMD_RESUME / APP_CMD_PAUSE: whether the activity is in the foreground. The OpenXR session state is what really
// paces the game (the runtime stops giving frames), so this is only reported, not used as a gate.
void SetResumed(bool resumed);
bool Resumed();

// APP_CMD_DESTROY (or the runtime asking to quit): the game's loops end at the next frame.
void RequestQuit();
bool QuitRequested();

// One key event of the activity's input queue. `androidKeyCode` is an AKEYCODE_*; the backend maps the ones the
// screens use onto platform/os/keys.h and hands the gamepad ones to platform/input/android_input.h as well.
void KeyEvent(int32_t androidKeyCode, bool down);

// The gamepad's axes of one motion event (see platform/input/android_input.h Axes).
void MotionEvent(float leftX, float leftY, float rightX, float rightY, float leftTrigger, float rightTrigger, float hatX, float hatY);

} // namespace gt2game::android
