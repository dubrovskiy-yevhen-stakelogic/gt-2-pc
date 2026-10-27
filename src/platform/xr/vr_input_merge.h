#pragma once
#include "platform/xr/vr_controls.h"

namespace gt2::vr {
// Menus share buttons; racing maps each device independently before merging.
inline input::Ps1PadFrame MergeMenuPads(input::Ps1PadFrame desktop, const input::Ps1PadFrame& xr) {
    if (xr.type == input::kTypeNone) return desktop;
    if (desktop.type == input::kTypeNone) return xr;
    desktop.buttons |= xr.buttons;
    if (desktop.type != input::kTypeNegcon && desktop.type != input::kTypeJogcon) {
        for (size_t i=0;i<desktop.analog.size();++i)
            if (std::abs(int(xr.analog[i])-128) > std::abs(int(desktop.analog[i])-128)) desktop.analog[i]=xr.analog[i];
        desktop.pressure |= xr.pressure;
        desktop.pressureR2=std::max(desktop.pressureR2,xr.pressureR2);
        desktop.pressureL2=std::max(desktop.pressureL2,xr.pressureL2);
    }
    return desktop;
}

inline void MergeDrivingInput(LogicalPad& desktop, uint32_t& held, const LogicalPad& xr, uint32_t xrHeld) {
    auto pedal=[](const LogicalPad& p,uint32_t bit,uint16_t value) {
        return (p.analog & bit) ? value : uint16_t((p.buttons & bit) ? 255 : 0);
    };
    desktop.throttle=std::max(pedal(desktop,kPadThrottle,desktop.throttle),pedal(xr,kPadThrottle,xr.throttle));
    desktop.brake=std::max(pedal(desktop,kPadBrake,desktop.brake),pedal(xr,kPadBrake,xr.brake));
    auto steering=[](const LogicalPad& p) {
        if (p.analog & 1) return int(p.steerAxis)-128;
        return (p.buttons & kPadLeft) ? -128 : (p.buttons & kPadRight) ? 127 : 0;
    };
    // Neutral Touch input cannot cancel a held gamepad stick or D-pad. Ties keep
    // the desktop input, so simultaneous opposing controls remain deterministic.
    const bool useXr=std::abs(steering(xr))>std::abs(steering(desktop));
    if (useXr) {
        desktop.steerAxis=xr.steerAxis;
        desktop.analog=uint16_t((desktop.analog & ~1u)|(xr.analog & 1u));
        desktop.buttons=(desktop.buttons & ~(kPadLeft|kPadRight|kPadStickCurve)) |
            (xr.buttons & (kPadLeft|kPadRight|kPadStickCurve));
    }
    desktop.analog |= 4|8;
    desktop.buttons |= xr.buttons & ~(kPadLeft|kPadRight|kPadStickCurve);
    held |= xrHeld;
}
}
