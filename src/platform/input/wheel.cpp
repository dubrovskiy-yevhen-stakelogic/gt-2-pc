#include "platform/input/wheel.h"
#include "platform/input/input_diagnostics.h"
#include "game/shell/shared_vr_settings.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace gt2::input {
namespace {
std::ofstream diagnosticLog;
std::chrono::steady_clock::time_point diagnosticStart;
}
void InputDiagnostic(const std::string& message) {
    if (!diagnosticLog.is_open()) return;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - diagnosticStart).count();
    diagnosticLog << ms << " ms: " << message << '\n';
    diagnosticLog.flush();
}
}

namespace gt2::input::wheel {
namespace {
Settings currentSettings;
std::string settingsFile;
const DeviceState* Find(const std::vector<DeviceState>& devices, const std::string& id) {
    for (const auto& d : devices) if (d.id == id && d.online) return &d;
    return nullptr;
}
}
Settings& Config() { return currentSettings; }
void LoadSettings(const std::string& path) {
    settingsFile = path; currentSettings = Settings::Parse(shell::ReadPreferences(path));
    diagnosticLog.close(); diagnosticLog.clear();
    if (!path.empty()) diagnosticLog.open(std::filesystem::path(path).parent_path() / "input-diagnostics.log", std::ios::app);
    diagnosticStart = std::chrono::steady_clock::now();
    InputDiagnostic("session start; wheel disconnect fallback build; gearbox=" + std::to_string(currentSettings.gearbox));
}
void SaveSettings() { if (!settingsFile.empty()) shell::WritePreferences(settingsFile, currentSettings.Serialize()); }
bool AxisBinding::Valid(bool steering) const {
    if (device.empty() || axis < 0 || axis >= kAxes || std::abs(end - rest) < 256) return false;
    return !steering || (std::abs(right - rest) >= 256 && int64_t(end - rest) * (right - rest) < 0);
}
float AxisBinding::Map(int raw, bool steering) const {
    if (!Valid(steering)) return 0;
    float value;
    if (steering) {
        const bool left = int64_t(raw - rest) * (end - rest) > 0;
        value = float(raw - rest) / float((left ? end : right) - rest) * (left ? -1.f : 1.f);
    } else value = float(raw - rest) / float(end - rest);
    const float sign = value < 0 && steering ? -1.f : 1.f;
    const float magnitude = steering ? std::abs(value) : std::max(0.f, value);
    const float low = std::clamp(deadzone, 0, 25) * .01f;
    const float high = std::clamp(saturation, 50, 100) * .01f;
    return sign * std::pow(std::clamp((magnitude - low) / (high - low), 0.f, 1.f), std::clamp(curve, 50, 200) * .01f);
}
std::string Settings::Serialize() const {
    std::ostringstream out;
    out << "# GT2 racing wheel settings, shared by both discs and PC / PCVR.\nversion 2\n"
        << "options " << enabled << ' ' << gearbox << ' ' << feedback << ' ' << gain << ' ' << damping << ' ' << invertForce << '\n';
    out << "auto " << automatic << ' ' << useClutch << ' ' << rimButtons << ' ' << std::quoted(profile) << ' ' << std::quoted(signature) << '\n';
    out << "sources " << std::quoted(baseChoice) << ' ' << std::quoted(pedalChoice) << ' ' << std::quoted(shifterChoice) << '\n';
    out << "defaults " << autoFeedback << ' ' << autoGearbox << '\n';
    out << "driving_aids " << tractionControl << ' ' << countersteer << '\n';
    out << "ignore_shift_speed " << ignoreShiftSpeed << '\n';
    out << "steering_model " << steeringGeometry.wheelDegrees << ' ' << steeringGeometry.mechanicalTrailMm << ' '
        << steeringGeometry.patchHalfLengthMm << ' ' << steeringGeometry.referenceTorqueNm << '\n';
    for (size_t i = 0; i < navigation.size(); ++i) out << "nav " << i << ' ' << std::quoted(navigation[i].device) << ' ' << navigation[i].button << '\n';
    for (size_t i = 0; i < axes.size(); ++i) {
        const auto& a = axes[i];
        out << "axis " << i << ' ' << std::quoted(a.device) << ' ' << a.axis << ' ' << a.rest << ' ' << a.end << ' ' << a.right
            << ' ' << a.deadzone << ' ' << a.saturation << ' ' << a.curve << '\n';
    }
    for (size_t i = 0; i < buttons.size(); ++i) out << "button " << i << ' ' << std::quoted(buttons[i].device) << ' ' << buttons[i].button << '\n';
    return out.str();
}
Settings Settings::Parse(const std::string& text) {
    Settings result;
    result.automatic = text.empty();
    result.autoFeedback = result.autoGearbox = text.empty();
    bool hasAutoSettings = false;
    std::istringstream stream(text); std::string line;
    while (std::getline(stream, line)) {
        std::istringstream in(line); std::string key; in >> key;
        if (key == "auto") {
            int automatic, clutch, rim; std::string profile, signature;
            if (in >> automatic >> clutch >> rim >> std::quoted(profile) >> std::quoted(signature)) {
                hasAutoSettings = true;
                result.automatic = automatic == 1; result.useClutch = clutch == 1; result.rimButtons = rim == 1;
                result.profile = profile; result.signature = signature;
            }
        } else if (key == "defaults") {
            int feedback, gearbox;
            if (in >> feedback >> gearbox) { result.autoFeedback = feedback == 1; result.autoGearbox = gearbox == 1; }
        } else if (key == "driving_aids") {
            int traction, steering;
            if (in >> traction >> steering) { result.tractionControl = std::clamp(traction, 0, 5); result.countersteer = std::clamp(steering, 0, 2); }
        } else if (key == "ignore_shift_speed") {
            int value;
            if (in >> value) result.ignoreShiftSpeed = value == 1;
        } else if (key == "steering_model") {
            int degrees, trail, patch, torque;
            if (in >> degrees >> trail >> patch >> torque) {
                result.steeringGeometry = {std::clamp(degrees, 180, 2520), std::clamp(trail, 0, 100),
                    std::clamp(patch, 20, 150), std::clamp(torque, 1, 100)};
            }
        } else if (key == "sources") {
            in >> std::quoted(result.baseChoice) >> std::quoted(result.pedalChoice) >> std::quoted(result.shifterChoice);
        } else if (key == "nav") {
            size_t index; ButtonBinding b;
            if (in >> index >> std::quoted(b.device) >> b.button)
                if (index < result.navigation.size() && b.button >= -1 && b.button < kButtons) result.navigation[index] = b;
        } else if (key == "options") {
            int enabled, gearbox, feedback, gain, damping, invert;
            if (in >> enabled >> gearbox >> feedback >> gain >> damping >> invert) {
                result.enabled = enabled == 1; result.gearbox = std::clamp(gearbox, 0, 2); result.feedback = feedback == 1;
                result.gain = std::clamp(gain, 0, 100); result.damping = std::clamp(damping, 0, 100); result.invertForce = invert == 1;
            }
        } else if (key == "axis") {
            size_t index; AxisBinding a;
            if (in >> index >> std::quoted(a.device) >> a.axis >> a.rest >> a.end >> a.right >> a.deadzone >> a.saturation >> a.curve) {
                if (index >= result.axes.size()) continue;
                a.axis = std::clamp(a.axis, -1, kAxes - 1);
                a.rest = std::clamp(a.rest, -32768, 32767); a.end = std::clamp(a.end, -32768, 32767); a.right = std::clamp(a.right, -32768, 32767);
                a.deadzone = std::clamp(a.deadzone, 0, 25); a.saturation = std::clamp(a.saturation, 50, 100); a.curve = std::clamp(a.curve, 50, 200);
                result.axes[index] = a;
            }
        } else if (key == "button") {
            size_t index; ButtonBinding b;
            if (in >> index >> std::quoted(b.device) >> b.button)
                if (index < result.buttons.size() && b.button >= -1 && b.button < kButtons) result.buttons[index] = b;
        }
    }
    if (!hasAutoSettings) result.useClutch = result.axes[Clutch].Valid(false);
    return result;
}
State Resolve(const Settings& settings, const std::vector<DeviceState>& devices, bool focused) {
    State state; state.active = settings.enabled;
    if (!settings.enabled || !focused) return state;
    bool ready = true;
    float values[AxisCount]{};
    for (int i = 0; i < AxisCount; ++i) {
        const auto& a = settings.axes[i]; const auto* d = Find(devices, a.device);
        const bool present = a.Valid(i == Steering) && d && d->available[a.axis];
        if (!present && (i != Clutch || !a.device.empty())) ready = false;
        if (present) values[i] = a.Map(d->axes[a.axis], i == Steering);
    }
    for (int i = 0; i < ButtonCount; ++i) {
        const auto& b = settings.buttons[i]; const auto* d = Find(devices, b.device);
        if (settings.gearbox == 2 && (i >= Gear1 || i == Reverse) && !b.device.empty() && b.button >= 0 && !d)
            state.shifterDisconnected = true;
        state.held[i] = d && b.button >= 0 && b.button < kButtons && d->buttons[b.button];
    }
    state.ready = ready;
    if (ready) { state.steering = values[0]; state.throttle = values[1]; state.brake = values[2]; state.clutch = values[3]; }
    return state;
}
void Apply(const Settings& settings, const State& state, LogicalPad& pad) {
    if (!state.active) return;
    // A saved rig that is not connected must not erase a fresh controller frame.
    // An already mapped wheel frame still takes the neutral-on-disconnect path.
    if (!state.ready && !pad.wheel) return;
    pad = {};
    pad.wheel = true;
    pad.analog = 13;
    const float steering = std::clamp(state.steering, -1.f, 1.f);
    pad.steerAxis = uint16_t(std::lround(2048 + steering * (steering < 0 ? 2048 : 2047)));
    pad.throttle = uint16_t(std::lround(state.throttle * 1023));
    pad.brake = uint16_t(std::lround(state.brake * 1023));
    pad.clutch = uint8_t(std::lround(state.clutch * 255));
    if (!state.ready) { pad.wheelGear = 1; pad.clutch = 255; return; }
    pad.ignoreShiftSpeed = settings.ignoreShiftSpeed;
    if (state.held[Handbrake]) pad.buttons |= kPadHandbrake;
    if (state.held[ShiftUp]) pad.buttons |= kPadShiftUp;
    if (state.held[ShiftDown]) pad.buttons |= kPadShiftDown;
    // Keep the saved H-pattern bindings intact so reconnecting restores the
    // lever. A missing accessory must not disconnect working driving axes.
    const int gearbox = state.shifterDisconnected ? 1 : settings.gearbox;
    pad.wheelGear = gearbox == 1 ? 15 : 0;
    if (gearbox == 2) {
        int count = 0; pad.wheelGear = 1;
        if (state.held[Reverse]) { pad.wheelGear = 2; ++count; }
        for (int i = 0; i < 7; ++i) if (state.held[Gear1 + i]) { pad.wheelGear = uint8_t(i + 3); ++count; }
        if (count > 1) pad.wheelGear = 1;
    } else if (!state.shifterDisconnected && state.held[Reverse]) pad.wheelGear = 2;
}
void ApplyRaceTransmission(LogicalPad& pad, bool manual, int currentGear) {
    if (!pad.wheel || (pad.wheelGear == 1 && pad.clutch == 255)) return;
    if (manual) {
        if (pad.wheelGear == 0) pad.wheelGear = 15;
        return;
    }
    const bool up = (pad.buttons & kPadShiftUp) != 0, down = (pad.buttons & kPadShiftDown) != 0;
    const bool forwardGate = pad.wheelGear >= 3 && pad.wheelGear <= 9;
    if (pad.wheelGear == 2 || (currentGear == 1 && down && !up)) pad.wheelGear = 2;
    else if (currentGear == 0) pad.wheelGear = (up && !down) || forwardGate ? 3 : 2;
    else pad.wheelGear = 10;
    // Forward gears are automatic. Explicit R/1 requests carry the player's
    // speed-interlock policy; holding reverse keeps it engaged under throttle.
}
float Force(const SteeringMoments& moments, float steeringVelocity, const Settings& settings) {
    if (!settings.enabled || !settings.feedback || !moments.valid || !std::isfinite(moments.handwheelNm) || !std::isfinite(steeringVelocity)) return 0;
    const float aligning = moments.handwheelNm / std::clamp(settings.steeringGeometry.referenceTorqueNm, 1, 100);
    const float sign = settings.invertForce ? -1.f : 1.f;
    const auto& axis = settings.axes[Steering];
    const float physicalSign = axis.right < axis.rest ? -1.f : 1.f;
    return physicalSign * std::clamp(sign * aligning - steeringVelocity * std::clamp(settings.damping, 0, 100) * .002f, -1.f, 1.f) * std::clamp(settings.gain, 0, 100) * .01f;
}
}
