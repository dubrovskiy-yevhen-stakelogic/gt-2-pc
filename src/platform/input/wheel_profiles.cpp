#include "platform/input/wheel.h"
#include <algorithm>

namespace gt2::input::wheel {
namespace {
#include "wheel_profiles_data.h"
bool PresentButton(const DeviceState& d, int b) {
    return b >= 0 && b < kButtons && (b < 128 ? b < d.buttonCount : (b - 128) / 4 < d.hatCount);
}
}
bool Profile::HasAxis(int target) const {
    return std::any_of(axes.begin(), axes.end(), [=](const auto& a) { return a.target == target; });
}
bool Profile::HasButton(int target) const {
    return std::any_of(buttons.begin(), buttons.end(), [=](const auto& b) { return b.first == target; });
}
const std::vector<Profile>& Profiles() { return builtInProfiles; }
const Profile* MatchProfile(const DeviceState& d) {
    for (const auto& p : Profiles()) if (p.vendor == d.vendor && p.product == d.product) return &p;
    return nullptr;
}
bool IsRacingDevice(const DeviceState& d) {
    // The USB shifter's identity is known even though its reference bindings
    // are invalid. Do not expose gear gates as a second player's gamepad.
    return MatchProfile(d) || (d.vendor == 0x0eb7 && d.product == 0x1a92);
}
bool FitsDeviceRole(const DeviceState& d, DeviceRole role) {
    const auto* p = MatchProfile(d);
    // Fanatec exposes its base twice. The catalogue describes the primary
    // collection; the extended collection has a different axis/button layout.
    if (p && p->HasAxis(Steering) && d.vendor == 0x0eb7 && d.hidCollection > 1) return false;
    if (role == DeviceRole::Any) return true;
    if (!p) return false;
    if (role == DeviceRole::Wheel) {
        for (const auto& a : p->axes) if (a.target == Steering) return d.available[a.axis];
        return false;
    }
    if (p->HasAxis(Steering)) return false;
    if (role == DeviceRole::Pedals) return p->HasAxis(Throttle) && p->HasAxis(Brake);
    return p->HasButton(Gear1) || (p->HasButton(ShiftUp) && p->HasButton(ShiftDown));
}
std::string DeviceName(const DeviceState& d) {
    if (const auto* p = MatchProfile(d)) return p->name;
    if (d.vendor == 0x0eb7 && d.product == 0x1a92) return "Fanatec USB shifter";
    return d.name;
}
void ResetAutomatic(Settings& s) {
    s.automatic = true; s.enabled = false; s.profile.clear(); s.signature.clear();
    s.axes = {}; s.buttons = {}; s.navigation = {};
    s.baseChoice.clear(); s.pedalChoice.clear(); s.shifterChoice.clear();
}
bool AutoConfigure(Settings& s, const std::vector<DeviceState>& devices, std::string& status) {
    status.clear();
    if (!s.automatic) return false;
    bool ambiguous = false;
    auto choose = [&](int role, const std::string& choice, const std::string& previous) -> const DeviceState* {
        const std::string wanted = !choice.empty() ? choice : previous;
        const DeviceState* result = nullptr;
        int count = 0;
        for (const auto& d : devices) if (d.online) {
            if (!FitsDeviceRole(d, role == 0 ? DeviceRole::Wheel : role == 1 ? DeviceRole::Pedals : DeviceRole::Shifter)) continue;
            if (!wanted.empty()) { if (d.id == wanted) return &d; }
            else { result = &d; ++count; }
        }
        if (!wanted.empty()) return nullptr;
        if (count > 1) { ambiguous = true; return nullptr; }
        return result;
    };
    const auto* base = choose(0, s.baseChoice, s.profile.empty() ? "" : s.axes[Steering].device);
    if (!base) { status = ambiguous ? "Choose your wheel in Devices" : "Connect a wheel, or choose Guided setup"; return false; }
    const auto& profile = *MatchProfile(*base);
    const std::string oldPedals = s.axes[Throttle].device != base->id ? s.axes[Throttle].device : "";
    const auto* pedals = s.pedalChoice == "base" ? nullptr : choose(1, s.pedalChoice, oldPedals);
    if (ambiguous) { status = "Choose your pedal set in Devices"; return false; }
    if (!pedals && s.pedalChoice != "base" && (!oldPedals.empty() || !s.pedalChoice.empty())) {
        status = "Reconnect the selected USB pedals"; return false;
    }
    std::string oldShifter = s.buttons[Gear1].device;
    if (oldShifter.empty()) oldShifter = s.buttons[ShiftUp].device;
    if (oldShifter == base->id) oldShifter.clear();
    const auto* shifter = s.shifterChoice == "none" ? nullptr : choose(2, s.shifterChoice, s.profile.empty() ? "" : oldShifter);
    if (ambiguous) { status = "Choose your shifter in Devices"; return false; }
    if (!shifter && s.shifterChoice != "none" && (!oldShifter.empty() || !s.shifterChoice.empty())) {
        status = "Reconnect the selected USB shifter"; return false;
    }
    const std::string signature = std::string(builtInProfileRevision) + ":" + base->id + ":" + (pedals ? pedals->id : "base") + ":" + (shifter ? shifter->id : "none")
        + (s.useClutch ? ":clutch" : ":no-clutch") + (s.rimButtons ? ":rim" : ":no-rim");
    if (profile.rimSpecific && !s.rimButtons) status = "Choose rim buttons in Devices, or learn your buttons";
    if (s.signature == signature) return false;
    Settings next = s;
    next.axes = {}; next.buttons = {}; next.navigation = {};
    auto axes = [&](const DeviceState& d, bool steering) {
        for (const auto& a : MatchProfile(d)->axes) {
            if ((a.target == Steering) != steering || (a.target == Clutch && !s.useClutch)) continue;
            if (a.axis < 0 || a.axis >= kAxes || !d.available[a.axis]) continue;
            next.axes[a.target] = {d.id, a.axis, a.rest, a.end, a.right, a.target == Steering ? 0 : 2, 100, 100};
        }
    };
    axes(*base, true); axes(pedals ? *pedals : *base, false);
    auto buttons = [&](const DeviceState& d, bool includeNavigation) {
        const auto& p = *MatchProfile(d);
        if (p.rimSpecific && !s.rimButtons) return;
        for (const auto& [target, button] : p.buttons) if (PresentButton(d, button)) next.buttons[target] = {d.id, button};
        if (includeNavigation) for (const auto& [target, button] : p.navigation)
            if (PresentButton(d, button)) next.navigation[target] = {d.id, button};
    };
    buttons(*base, true);
    if (shifter) {
        if (MatchProfile(*shifter)->HasButton(Gear1)) {
            next.buttons[Reverse] = {};
            for (int i = Gear1; i <= Gear7; ++i) next.buttons[i] = {};
        }
        buttons(*shifter, false);
    }
    if (s.profile.empty()) {
        next.enabled = true;
        if (s.autoFeedback) next.feedback = base->forceCapable;
    }
    if (s.autoGearbox) next.gearbox = shifter && MatchProfile(*shifter)->HasButton(Gear1) ? 2 : 1;
    next.profile = profile.name; next.signature = signature;
    s = std::move(next);
    return true;
}
uint16_t Navigation(const Settings& s, const std::vector<DeviceState>& devices, bool focused) {
    if (!s.enabled || !focused) return 0;
    uint16_t mask = 0;
    for (size_t i = 0; i < s.navigation.size(); ++i) {
        const auto& b = s.navigation[i];
        // Paddles already reach the gearbox through Apply. Sending the same
        // press as a pad button would also trigger its camera/menu binding.
        const auto paddle = [&](Button action) {
            const auto& shift = s.buttons[action];
            return b.button >= 0 && b.button == shift.button && b.device == shift.device;
        };
        if (paddle(ShiftUp) || paddle(ShiftDown)) continue;
        for (const auto& d : devices) if (d.online && d.id == b.device && PresentButton(d, b.button) && d.buttons[b.button]) mask |= uint16_t(1u << i);
    }
    return mask;
}
}
