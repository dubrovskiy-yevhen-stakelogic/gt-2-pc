#include "platform/input/input_system.h"
#include "platform/input/scripted_pad.h"
#include <SDL.h>
#include <algorithm>
#include <stdexcept>
#include <utility>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

namespace gt2::input {
namespace {
using detail::FakePad;
#ifdef __EMSCRIPTEN__
// SDL's browser backend enumerates connected pads in initial index order, then
// appends hot-plugged pads. Mirror that order, including non-controller devices.
// Connection handles stay distinct when a browser index is reused after unplugging.
EM_JS(void, WebInitGamepads, (), {
    const state = { pads: [], next: 1 };
    state.connect = function(event) {
        const index = event['gamepad']['index'];
        if (!state.pads.some(p => p.index === index))
            state.pads.push({index: index, handle: state.next++});
    };
    state.disconnect = function(event) {
        state.pads = state.pads.filter(p => p.index !== event['gamepad']['index']);
    };
    Module['gt2Gamepads'] = state;
    window.addEventListener('gamepadconnected', state.connect);
    window.addEventListener('gamepaddisconnected', state.disconnect);
    const pads = navigator['getGamepads'] ? navigator['getGamepads']() : [];
    for (const pad of pads) if (pad) state.connect({gamepad: pad});
});
EM_JS(void, WebQuitGamepads, (), {
    const state = Module['gt2Gamepads'];
    if (!state) return;
    window.removeEventListener('gamepadconnected', state.connect);
    window.removeEventListener('gamepaddisconnected', state.disconnect);
    delete Module['gt2Gamepads'];
});
EM_JS(int, WebGamepadHandle, (int deviceIndex), {
    const state = Module['gt2Gamepads'];
    const pad = state && state.pads[deviceIndex];
    return pad ? pad.handle : 0;
});
EM_JS(int, WebRumble, (int handle, int low, int high, int ms), {
    const state = Module['gt2Gamepads'];
    const connection = state && state.pads.find(p => p.handle === handle);
    if (!connection || !navigator['getGamepads']) return 0;
    const pad = navigator['getGamepads']()[connection.index];
    const actuator = pad && pad['vibrationActuator'];
    if (!actuator || typeof actuator['playEffect'] !== 'function') return 0;
    if (ms < 0) return 1; // Capability query, without starting an effect.
    try {
        const result = actuator['playEffect']('dual-rumble', {
            startDelay: 0, duration: ms,
            strongMagnitude: low / 65535, weakMagnitude: high / 65535
        });
        if (result && result['catch']) result['catch'](function () {});
        return 1;
    } catch (error) { return 0; }
});
#endif
class SdlPad final : public Device {
public:
    explicit SdlPad(SDL_GameController* pad, int deviceIndex) : pad_(pad) {
#ifdef __EMSCRIPTEN__
        webHandle_ = WebGamepadHandle(deviceIndex);
#else
        (void)deviceIndex;
#endif
    }
    ~SdlPad() override { SetMotors(0, 0); SDL_GameControllerClose(pad_); }
    SDL_JoystickID Id() const { return SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad_)); }
    bool Poll(int, Ps1PadFrame& out) override {
        if (!SDL_GameControllerGetAttached(pad_)) return false;
        out = {};
        out.type = kTypeAnalog;
        constexpr std::pair<SDL_GameControllerButton, uint16_t> buttons[] = {
            {SDL_CONTROLLER_BUTTON_A, ps1::kCross}, {SDL_CONTROLLER_BUTTON_B, ps1::kCircle},
            {SDL_CONTROLLER_BUTTON_X, ps1::kSquare}, {SDL_CONTROLLER_BUTTON_Y, ps1::kTriangle},
            {SDL_CONTROLLER_BUTTON_BACK, ps1::kSelect}, {SDL_CONTROLLER_BUTTON_START, ps1::kStart},
            {SDL_CONTROLLER_BUTTON_LEFTSTICK, ps1::kL3}, {SDL_CONTROLLER_BUTTON_RIGHTSTICK, ps1::kR3},
            {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, ps1::kL1}, {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, ps1::kR1},
            {SDL_CONTROLLER_BUTTON_DPAD_UP, ps1::kUp}, {SDL_CONTROLLER_BUTTON_DPAD_DOWN, ps1::kDown},
            {SDL_CONTROLLER_BUTTON_DPAD_LEFT, ps1::kLeft}, {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, ps1::kRight}};
        for (const auto& [button, bit] : buttons) if (SDL_GameControllerGetButton(pad_, button)) out.buttons |= bit;
        auto stick = [&](SDL_GameControllerAxis axis) { return uint8_t((int32_t(SDL_GameControllerGetAxis(pad_, axis)) + 32768) >> 8); };
        out.analog = {stick(SDL_CONTROLLER_AXIS_RIGHTX), stick(SDL_CONTROLLER_AXIS_RIGHTY),
                      stick(SDL_CONTROLLER_AXIS_LEFTX), stick(SDL_CONTROLLER_AXIS_LEFTY)};
        auto trigger = [&](SDL_GameControllerAxis axis) {
            return uint8_t(std::max(0, int(SDL_GameControllerGetAxis(pad_, axis))) * 255 / 32767);
        };
        out.pressure = true;
        out.pressureL2 = trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        out.pressureR2 = trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        if (out.pressureL2 > 30) out.buttons |= ps1::kL2;
        if (out.pressureR2 > 30) out.buttons |= ps1::kR2;
        return true;
    }
    void SetMotors(uint8_t small, uint8_t large, int strength = 100) override {
        const auto low = Uint16(int(large) * 257 * strength / 100);
        const auto high = Uint16((small & 1) ? 65535 * strength / 100 : 0);
        // Renewed each field; a stalled client cannot leave the pad vibrating indefinitely.
#ifdef __EMSCRIPTEN__
        WebRumble(webHandle_, low, high, 150);
#else
        SDL_GameControllerRumble(pad_, low, high, 150);
#endif
    }
    bool HasMotors() const override {
#ifdef __EMSCRIPTEN__
        return WebRumble(webHandle_, 0, 0, -1) != 0;
#else
        return SDL_GameControllerHasRumble(pad_) == SDL_TRUE;
#endif
    }
    std::string Name() const override {
        const char* name = SDL_GameControllerName(pad_);
        return name ? name : "SDL gamepad";
    }
private:
    SDL_GameController* pad_;
#ifdef __EMSCRIPTEN__
    int webHandle_ = 0;
#endif
};
bool Active(const Ps1PadFrame& f, const Ps1PadFrame& rest) {
    if (f.type == kTypeNone) return false;
    if (f.buttons) return true;
    for (size_t i = 0; i < f.analog.size(); ++i)
        if (std::abs(int(f.analog[i]) - int(rest.analog[i])) > 48) return true;
    return f.pressure && (f.pressureL2 > 48 || f.pressureR2 > 48);
}
}
struct InputSystem::DirectInputState {};
InputSystem::InputSystem() {
    SDL_SetMainReady();
#ifdef __EMSCRIPTEN__
    WebInitGamepads();
#endif
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
#ifdef __EMSCRIPTEN__
        WebQuitGamepads();
#endif
        throw std::runtime_error(SDL_GetError());
    }
}
InputSystem::~InputSystem() {
    StopFeedback(); devices_.clear();
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
#ifdef __EMSCRIPTEN__
    WebQuitGamepads();
#endif
}
void InputSystem::AttachWindow(void* window) { hwnd_ = window; rescan_ = true; }
void InputSystem::AddFakePad(const std::string& script, int port) {
    AddDevice(std::make_unique<FakePad>(detail::ParseFakePad(script), port));
}
std::vector<std::string> InputSystem::TakeLog() { return std::exchange(log_, {}); }
void InputSystem::Rescan(int field) {
    lastScan_ = field; rescan_ = false;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        const auto id = SDL_JoystickGetDeviceInstanceID(i);
        bool open = false;
        for (const auto& device : devices_)
            if (const auto* pad = dynamic_cast<SdlPad*>(device.get()); pad && pad->Id() == id) open = true;
        if (open) continue;
        if (auto* controller = SDL_GameControllerOpen(i)) {
            auto pad = std::make_unique<SdlPad>(controller, i);
            log_.push_back("connected: " + pad->Name());
            AddDevice(std::move(pad));
        } else log_.push_back(std::string("cannot open gamepad: ") + SDL_GetError());
    }
}
void InputSystem::Poll(int field, bool focused) {
    focused_ = focused;
    if (!focused) StopFeedback();
    SDL_GameControllerUpdate();
    if (rescan_ || field - lastScan_ >= 120) Rescan(field);
    for (size_t i = 0; i < devices_.size();) {
        Ps1PadFrame f;
        if (!devices_[i]->Poll(field, f)) {
            log_.push_back("disconnected: " + devices_[i]->Name());
            if (active_ == int(i)) active_ = -1;
            else if (active_ > int(i)) --active_;
            if (appliedDevice_ == int(i)) appliedDevice_ = -1;
            else if (appliedDevice_ > int(i)) --appliedDevice_;
            const auto offset = std::ptrdiff_t(i);
            devices_.erase(devices_.begin() + offset);
            frames_.erase(frames_.begin() + offset);
            lastActivity_.erase(lastActivity_.begin() + offset);
            rest_.erase(rest_.begin() + offset);
            continue;
        }
        if (!focused && !devices_[i]->Scripted()) {
            const auto type = f.type; f = {}; f.type = type;
        }
        if (lastActivity_[i] == -1) { rest_[i] = f; lastActivity_[i] = -2; }
        frames_[i] = f;
        if (Active(f, rest_[i])) lastActivity_[i] = field;
        ++i;
    }
    wheel_.Poll(focused);
    int best = active_;
    if (best >= 0 && frames_[size_t(best)].type == kTypeNone) best = -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (devices_[i]->Scripted()) continue;
        if (frames_[i].type != kTypeNone && (best < 0 || lastActivity_[i] > lastActivity_[size_t(best)])) best = int(i);
    }
    for (size_t i = 0; i < devices_.size(); ++i)
        if (const auto* pad = dynamic_cast<FakePad*>(devices_[i].get()); pad && pad->Port() == 1) best = int(i);
    if (best != active_ && best >= 0) log_.push_back("port 1: " + devices_[size_t(best)]->Name());
    active_ = best;
    port1_ = active_ >= 0 ? frames_[size_t(active_)] : Ps1PadFrame{};
    int second = -1;
    for (size_t i = 0; i < devices_.size(); ++i) {
        if (int(i) == active_ || frames_[i].type == kTypeNone) continue;
        if (const auto* pad = dynamic_cast<FakePad*>(devices_[i].get())) {
            if (pad->Port() == 2) { second = int(i); break; }
        } else if (second < 0 || lastActivity_[i] > lastActivity_[size_t(second)]) second = int(i);
    }
    port2Device_ = second;
    port2_ = second >= 0 ? frames_[size_t(second)] : Ps1PadFrame{};
    if (!actuatorsSet_) actuators_ = {};
    actuatorsSet_ = false;
    ApplyMotors();
}
void InputSystem::SetActuators(const Actuators& a) { actuators_ = a; actuatorsSet_ = true; ApplyMotors(); }
void InputSystem::ApplyMotors() {
    for (size_t i = 0; i < devices_.size(); ++i) {
        const bool enabled = int(i) == active_ && (focused_ || devices_[i]->Scripted());
        devices_[i]->SetMotors(enabled ? actuators_.smallMotor : 0, enabled ? actuators_.largeMotor : 0, rumbleScale_);
    }
}
void InputSystem::StopFeedback() {
    actuators_ = {}; actuatorsSet_ = false; wheel_.Stop();
    for (auto& device : devices_) device->SetMotors(0, 0);
}
std::string InputSystem::Port1Name() const { return active_ < 0 ? "none" : devices_[size_t(active_)]->Name(); }
std::string InputSystem::Port2Name() const { return port2Device_ < 0 ? "none" : devices_[size_t(port2Device_)]->Name(); }
bool InputSystem::Port1HasMotors() const { return active_ >= 0 && devices_[size_t(active_)]->HasMotors(); }
bool InputSystem::Port1HasTriggers() const { return false; }
void InputSystem::SetPedalResistance(uint8_t, uint8_t) {} // SDL exposes standard rumble, not adaptive pedal resistance.
} // namespace gt2::input
