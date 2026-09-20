// The Android devices of platform/input/input_system.h (docs/research/vr_port_plan.md, M6).
//
// On the headset the only controller the game can read today is an Android gamepad (a Bluetooth pad, or a keyboard's
// arrow block) whose events reach us through the NativeActivity's input queue (android_input.h). It is presented as a
// DualShock (type 7) with the same mapping as the PC build's XInput device, so menus and the race take the original's
// pad path unchanged. The Touch controllers are XR actions and belong to M5; until they exist the game says so
// instead of pretending a controller is there.
//
// Not on Android: XInput / DirectInput (no such devices) and the scripted --fake-pad of the automated PC runs (there
// is no command line on the headset; AddFakePad refuses instead of silently running without it).
#include "platform/input/input_system.h"

#include <android/log.h>

#include <atomic>
#include <mutex>
#include <cmath>
#include <cstdint>

#include "platform/input/android_input.h"

namespace gt2::input {

namespace android_events = gt2::input::android;

namespace {

// The raw state the looper thread writes and the game thread reads.
std::atomic<uint32_t> g_buttons{0}; // ps1::k* bits
std::atomic<int32_t> g_leftX{0}, g_leftY{0}, g_rightX{0}, g_rightY{0};     // -1000..1000
std::atomic<int32_t> g_leftTrigger{0}, g_rightTrigger{0};                  // 0..1000
std::atomic<bool> g_seen{false};
std::mutex g_xrMutex;
Ps1PadFrame g_xrPad;
std::atomic<float> g_xrVibration{0};

int32_t Quantise(float v) {
    if (!(v > -2.0f && v < 2.0f)) return 0; // NaN or nonsense from an unknown device
    const float clamped = v < -1.0f ? -1.0f : v > 1.0f ? 1.0f : v;
    return int32_t(std::lround(clamped * 1000.0f));
}

// -1000..1000 (Android: +x right, +y down) -> a PS1 stick byte (0x80 centre, 0 = left / up).
uint8_t StickByte(int32_t v) {
    const int32_t b = 128 + (v * 127) / 1000;
    return uint8_t(b < 0 ? 0 : b > 255 ? 255 : b);
}

uint8_t TriggerByte(int32_t v) {
    const int32_t b = (v * 255) / 1000;
    return uint8_t(b < 0 ? 0 : b > 255 ? 255 : b);
}

// Android key codes (android/keycodes.h; spelled out so this file needs no NDK header for them).
constexpr int32_t kKeyDpadUp = 19, kKeyDpadDown = 20, kKeyDpadLeft = 21, kKeyDpadRight = 22, kKeyDpadCenter = 23;
constexpr int32_t kKeyButtonA = 96, kKeyButtonB = 97, kKeyButtonX = 99, kKeyButtonY = 100;
constexpr int32_t kKeyButtonL1 = 102, kKeyButtonR1 = 103, kKeyButtonL2 = 104, kKeyButtonR2 = 105;
constexpr int32_t kKeyButtonThumbL = 106, kKeyButtonThumbR = 107, kKeyButtonStart = 108, kKeyButtonSelect = 109, kKeyButtonMode = 110;

uint16_t Ps1Bit(int32_t keyCode) {
    switch (keyCode) {
    case kKeyDpadUp: return ps1::kUp;
    case kKeyDpadDown: return ps1::kDown;
    case kKeyDpadLeft: return ps1::kLeft;
    case kKeyDpadRight: return ps1::kRight;
    case kKeyDpadCenter:
    case kKeyButtonA: return ps1::kCross;
    case kKeyButtonB: return ps1::kCircle;
    case kKeyButtonX: return ps1::kSquare;
    case kKeyButtonY: return ps1::kTriangle;
    case kKeyButtonL1: return ps1::kL1;
    case kKeyButtonR1: return ps1::kR1;
    case kKeyButtonL2: return ps1::kL2;
    case kKeyButtonR2: return ps1::kR2;
    case kKeyButtonThumbL: return ps1::kL3;
    case kKeyButtonThumbR: return ps1::kR3;
    case kKeyButtonStart:
    case kKeyButtonMode: return ps1::kStart;
    case kKeyButtonSelect: return ps1::kSelect;
    default: return 0;
    }
}

// The Android gamepad as a DualShock, exactly the layout the PC build's XInput device produces.
class AndroidPad : public Device {
public:
    bool Poll(int, Ps1PadFrame& out) override {
        out = Ps1PadFrame{};
        out.type = kTypeAnalog;
        uint16_t buttons = uint16_t(g_buttons.load());
        const int32_t lt = g_leftTrigger.load(), rt = g_rightTrigger.load();
        if (lt > 300) buttons |= ps1::kL2; // the XInput threshold (30 of 255), as the PC build uses
        if (rt > 300) buttons |= ps1::kR2;
        out.buttons = buttons;
        out.analog = {StickByte(g_rightX.load()), StickByte(g_rightY.load()), StickByte(g_leftX.load()), StickByte(g_leftY.load())};
        out.pressure = true;
        out.pressureL2 = TriggerByte(lt);
        out.pressureR2 = TriggerByte(rt);
        return true;
    }
    void SetMotors(uint8_t, uint8_t, int) override {} // no rumble path to an Android gamepad from native code
    bool HasMotors() const override { return false; }
    std::string Name() const override { return "Android gamepad"; }
};

bool Active(const Ps1PadFrame& f, const Ps1PadFrame& rest) {
    if (f.buttons != rest.buttons) return true;
    for (size_t i = 0; i < f.analog.size(); i++)
        if (std::abs(int(f.analog[i]) - int(rest.analog[i])) > 16) return true;
    return std::abs(int(f.pressureL2) - int(rest.pressureL2)) > 16 || std::abs(int(f.pressureR2) - int(rest.pressureR2)) > 16;
}

} // namespace

namespace android {

void Reset() {
    g_buttons = 0; g_leftX = 0; g_leftY = 0; g_rightX = 0; g_rightY = 0;
    g_leftTrigger = 0; g_rightTrigger = 0; g_seen = false; g_xrVibration = 0;
    std::lock_guard<std::mutex> lock(g_xrMutex);
    g_xrPad = {};
}

void KeyEvent(int32_t keyCode, bool down) {
    const uint16_t bit = Ps1Bit(keyCode);
    if (!bit) return;
    g_seen.store(true);
    uint32_t before = g_buttons.load();
    uint32_t after;
    do {
        after = down ? (before | bit) : (before & ~uint32_t(bit));
    } while (!g_buttons.compare_exchange_weak(before, after));
}

void MotionEvent(const Axes& axes) {
    g_seen.store(true);
    g_leftX.store(Quantise(axes.leftX));
    g_leftY.store(Quantise(axes.leftY));
    g_rightX.store(Quantise(axes.rightX));
    g_rightY.store(Quantise(axes.rightY));
    g_leftTrigger.store(Quantise(axes.leftTrigger));
    g_rightTrigger.store(Quantise(axes.rightTrigger));
    // A gamepad whose D-pad is a hat axis: the same bits the key events set.
    const int32_t hx = Quantise(axes.hatX), hy = Quantise(axes.hatY);
    uint32_t b = g_buttons.load() & ~uint32_t(ps1::kUp | ps1::kDown | ps1::kLeft | ps1::kRight);
    if (hx < -500) b |= ps1::kLeft;
    if (hx > 500) b |= ps1::kRight;
    if (hy < -500) b |= ps1::kUp;
    if (hy > 500) b |= ps1::kDown;
    g_buttons.store(b);
}

bool PadSeen() { return g_seen.load(); }
void SetXrPad(const Ps1PadFrame& pad) { std::lock_guard<std::mutex> lock(g_xrMutex); g_xrPad = pad; }
float XrVibration() { return g_xrVibration.load(); }

} // namespace android

// The header keeps the DirectInput handle of the Windows devices as an opaque member; on Android there is nothing
// behind it, but the type must be complete where the destructor is compiled.
struct InputSystem::DirectInputState {};

InputSystem::InputSystem() = default;

InputSystem::~InputSystem() {
    devices_.clear();
}

void InputSystem::AttachWindow(void* window) {
    hwnd_ = window; // no window handle is needed on Android: the events come from the activity's input queue
    rescan_ = true;
}

void InputSystem::AddFakePad(const std::string&, int) {
    // The scripted pad is a feature of the automated PC runs (--fake-pad); the headset build has no command line and
    // no run to reproduce, so it must not pretend to have applied a script.
    __android_log_print(ANDROID_LOG_WARN, "GT2.Quest", "input: --fake-pad is a PC development feature and is not available on Android");
}

std::vector<std::string> InputSystem::TakeLog() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

void InputSystem::Rescan(int field) {
    lastScan_ = field;
    rescan_ = false;
    if (!devices_.empty() || !android::PadSeen()) return;
    devices_.push_back(std::make_unique<AndroidPad>());
    frames_.emplace_back();
    lastActivity_.push_back(-1);
    rest_.emplace_back();
    log_.push_back("connected: " + devices_.back()->Name());
}

void InputSystem::Poll(int field, bool focused) {
    if (rescan_ || field - lastScan_ >= 60) Rescan(field);
    for (size_t i = 0; i < devices_.size(); i++) {
        Ps1PadFrame f;
        devices_[i]->Poll(field, f); // the Android pad never disappears once it has been seen
        if (!focused) f = {};
        if (lastActivity_[i] == -1) rest_[i] = f, lastActivity_[i] = -2;
        frames_[i] = f;
        if (Active(f, rest_[i])) lastActivity_[i] = field;
    }
    int best = active_;
    for (size_t i = 0; i < devices_.size(); i++)
        if (best < 0 || lastActivity_[i] > lastActivity_[size_t(best)]) best = int(i);
    if (best != active_ && best >= 0) log_.push_back("port 1: " + devices_[size_t(best)]->Name());
    active_ = best;
    port1_ = active_ >= 0 ? frames_[size_t(active_)] : Ps1PadFrame{};
    { std::lock_guard<std::mutex> lock(g_xrMutex); if (g_xrPad.type != kTypeNone) port1_ = g_xrPad; }
    if (!focused) port1_ = {};
    port2_ = Ps1PadFrame{}; // no second controller on the headset (the 2 player Battle is a PC mode for now)
    port2Device_ = -1;
    if (!actuatorsSet_) actuators_ = Actuators{};
    actuatorsSet_ = false;
    g_xrVibration.store(focused ? std::max(actuators_.smallMotor ? 0.25f : 0.0f, float(actuators_.largeMotor) / 255.0f) * float(rumbleScale_) / 100.0f : 0.0f);
}

void InputSystem::SetActuators(const Actuators& a) {
    actuators_ = a;
    actuatorsSet_ = true;
}

void InputSystem::ApplyMotors() {}

std::string InputSystem::Port1Name() const { return active_ >= 0 ? devices_[size_t(active_)]->Name() : std::string(); }
std::string InputSystem::Port2Name() const { return "none"; }
bool InputSystem::Port1HasMotors() const { return g_xrPad.type != kTypeNone; }
bool InputSystem::Port1HasTriggers() const { return false; }
void InputSystem::SetPedalResistance(uint8_t, uint8_t) {}
void InputSystem::StopFeedback() { actuators_ = {}; g_xrVibration = 0; }

} // namespace gt2::input
