#pragma once
#include "gt2formats/replay.h"
#include "platform/input/ps1_pad.h"
#include <algorithm>
#include <cmath>
#include <array>
namespace gt2::vr {
// Menu/system buttons and grips remain reserved; these bindings affect driving only.
inline constexpr const char* controlSources[] = {"Off", "Right trigger", "Left trigger", "A", "B", "X", "Y", "Left stick click", "Right stick click"};
inline constexpr const char* controlNames[] = {"Accelerator", "Brake", "Handbrake", "Reverse", "Shift up", "Shift down", "Camera", "Look back"};
struct ControlBindings {
    std::array<int,8> source{1,2,8,4,3,5,6,7};
    bool brakeReverse = true;
    int steeringStick = 0; // left / right
};
inline constexpr const char* desktopControlSources[] = {"Off", "R2 / RT", "L2 / LT", "Cross / A", "Circle / B",
    "Square / X", "Triangle / Y", "L3", "R3", "L1 / LB", "R1 / RB", "D-pad up", "D-pad down"};
inline ControlBindings DesktopBindings() {
    ControlBindings bindings;
    bindings.source = {1,2,4,6,10,9,8,7};
    return bindings;
}
inline uint16_t ControlValue(const input::Ps1PadFrame& pad, int source) {
    constexpr uint16_t buttons[] = {0,0,0,input::ps1::kCross,input::ps1::kTriangle,input::ps1::kSquare,input::ps1::kR1,input::ps1::kSelect,input::ps1::kCircle};
    if (source == 1) return pad.pressureR2;
    if (source == 2) return pad.pressureL2;
    return source > 2 && source < int(std::size(buttons)) && (pad.buttons & buttons[source]) ? 255 : 0;
}
inline void ApplyControlBindings(LogicalPad& logical, uint32_t& held, const input::Ps1PadFrame& raw, const ControlBindings& bindings) {
    held &= ~0x3fcu;
    logical.buttons &= ~0x3fcu;
    logical.analog |= 4|8;
    logical.throttle = ControlValue(raw, bindings.source[0]);
    logical.brake = ControlValue(raw, bindings.source[1]);
    for (int i=0;i<8;++i) if (ControlValue(raw,bindings.source[i]) > 30) held |= 1u << (i+2);
    logical.buttons |= held & 0x3fcu;
}
inline void ApplyDesktopControlBindings(LogicalPad& logical, uint32_t& held, const input::Ps1PadFrame& raw, const ControlBindings& bindings) {
    constexpr uint16_t buttons[] = {0,input::ps1::kR2,input::ps1::kL2,input::ps1::kCross,input::ps1::kCircle,
        input::ps1::kSquare,input::ps1::kTriangle,input::ps1::kL3,input::ps1::kR3,input::ps1::kL1,input::ps1::kR1,
        input::ps1::kUp,input::ps1::kDown};
    auto value = [&](int source) -> uint16_t {
        if (raw.pressure && source == 1) return raw.pressureR2;
        if (raw.pressure && source == 2) return raw.pressureL2;
        return source > 0 && source < int(std::size(buttons)) && (raw.buttons & buttons[source]) ? 255 : 0;
    };
    held &= ~0x3fcu;
    logical.buttons &= ~0x3fcu;
    logical.analog |= 1|4|8;
    logical.steerAxis = raw.analog[bindings.steeringStick ? 0 : 2];
    logical.throttle = value(bindings.source[0]);
    logical.brake = value(bindings.source[1]);
    for (int i=0;i<8;++i) if (value(bindings.source[i]) > 30) held |= 1u << (i+2);
    logical.buttons |= held & 0x3fcu;
}
// Stop before selecting reverse; throttle brakes a reversing car before selecting forward.
// State survives pedal release, but is reset by leaving driving or selecting manual transmission.
class BrakeReverse {
public:
    void Reset() { reverse_ = false; }
    void Apply(LogicalPad& pad, float speed, bool automatic) {
        if (!automatic) { Reset(); return; }
        if (pad.buttons & kPadReverse) { reverse_ = true; return; }
        const bool gas = pad.throttle > 30, brake = pad.brake > 30;
        if (!reverse_ && brake && !gas && std::abs(speed) < .30f) reverse_ = true;
        if (reverse_ && gas && speed > -.30f) reverse_ = false;
        if (!reverse_) return;
        pad.buttons &= ~(kPadThrottle|kPadBrake|kPadReverse);
        if (gas) { pad.brake = pad.throttle; pad.throttle = 0; pad.buttons |= kPadBrake; }
        else {
            pad.throttle = 0; pad.brake = 0;
            if (brake) pad.buttons |= kPadReverse;
        }
    }
private:
    bool reverse_ = false;
};
}
