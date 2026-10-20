#pragma once
namespace gt2view {
struct HudVisibility {
    bool map = true, lap = true, records = true, gauges = true, turbo = true, tyres = true;
    bool mirror = true, countdown = true, warnings = true, messages = true, replay = true, movieHint = true;
};

// The gauges switch keeps the cabin clear of the 2D cluster; without the cabin it is the only speedometer.
inline bool GaugesVisible(const HudVisibility& v, bool cockpitShown) { return v.gauges || !cockpitShown; }
}
