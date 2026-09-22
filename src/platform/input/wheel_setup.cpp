#include "platform/input/wheel.h"
#include <algorithm>

namespace gt2::input::wheel {
void ShifterSetup::Start(const DeviceState& device, int gears) {
    device_ = device.id; neutral_ = device.buttons; gears_ = std::clamp(gears, 1, 7);
    step_ = 0; candidate_ = -1; stable_ = 0; release_ = false;
    learned_.fill(-1); error_.clear();
}
void ShifterSetup::Sample(const DeviceState* d) {
    if (Complete()) return;
    if (!d || !d->online || d->id != device_) {
        stable_ = 0; candidate_ = -1; error_ = "Reconnect the shifter to continue"; return;
    }
    error_.clear();
    int active = -1, count = 0;
    for (int b = 0; b < std::min(d->buttonCount, 128); ++b) if (d->buttons[b] && !neutral_[b]) { active = b; ++count; }
    if (release_) {
        if (count == 0) { release_ = false; ++step_; stable_ = 0; candidate_ = -1; }
        return;
    }
    if (count != 1) {
        stable_ = 0; candidate_ = -1;
        error_ = count > 1 ? "Several buttons active; check the shifter's PC mode" : "";
        return;
    }
    for (int i = 0; i < step_; ++i) if (learned_[i] == active) {
        error_ = "That gate was already learned. Select the shown gear"; stable_ = 0; return;
    }
    error_.clear();
    if (candidate_ != active) { candidate_ = active; stable_ = 0; }
    if (++stable_ >= 3) { learned_[step_] = active; release_ = true; }
}
bool ShifterSetup::ApplyTo(Settings& s) const {
    if (!Complete() || device_.empty()) return false;
    for (int i = 0; i < 7; ++i) s.buttons[Gear1 + i] = i < gears_ ? ButtonBinding{device_, learned_[i]} : ButtonBinding{};
    s.buttons[Reverse] = {device_, learned_[gears_]};
    s.gearbox = 2; s.automatic = false; s.autoGearbox = false;
    return true;
}
}
