#pragma once
namespace gt2view {
struct HudVisibility {
    bool map = true, lap = true, records = true, gauges = true, turbo = true, tyres = true;
    bool mirror = true, countdown = true, warnings = true, messages = true, replay = true;
};
}
