#include "platform/input/wheel.h"
#include "platform/input/input_diagnostics.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#ifdef _WIN32
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#endif

namespace gt2::input::wheel {
#ifdef _WIN32
std::string InstanceId(const void* value) {
    const GUID& g = *static_cast<const GUID*>(value);
    char text[40];
    std::snprintf(text, sizeof(text), "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", g.Data1, g.Data2, g.Data3,
        g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
    return text;
}
namespace {
const DWORD offsets[] = {DIJOFS_X, DIJOFS_Y, DIJOFS_Z, DIJOFS_RX, DIJOFS_RY, DIJOFS_RZ, DIJOFS_SLIDER(0), DIJOFS_SLIDER(1)};
struct Handle {
    IDirectInputDevice8A* device = nullptr;
    IDirectInputEffect* effect = nullptr;
    DeviceState state;
    std::array<bool, kAxes> forceAxes{};
    bool exclusive = false, effectRunning = false;
    bool savedAutocenter = false;
    DWORD autocenter = 0;
    int effectAxis = -1;
    ~Handle() { Stop(); if (device) { RestoreAutocenter(); device->Unacquire(); device->Release(); } }
    void RestoreAutocenter() {
        if (!savedAutocenter) return;
        DIPROPDWORD property{}; property.diph = {sizeof(property), sizeof(property.diph), 0, DIPH_DEVICE};
        property.dwData = autocenter; device->SetProperty(DIPROP_AUTOCENTER, &property.diph);
        savedAutocenter = false;
    }
    void Stop() {
        if (effect) { effect->Stop(); effect->Release(); effect = nullptr; }
        effectRunning = false; effectAxis = -1;
    }
};
void ReadAxes(Handle& h) {
    for (int i = 0; i < kAxes; ++i) {
        DIDEVICEOBJECTINSTANCEA object{}; object.dwSize = sizeof(object);
        if (FAILED(h.device->GetObjectInfo(&object, offsets[i], DIPH_BYOFFSET))) continue;
        DIPROPRANGE range{}; range.diph = {sizeof(range), sizeof(range.diph), offsets[i], DIPH_BYOFFSET};
        range.lMin = -32768; range.lMax = 32767;
        h.state.available[i] = SUCCEEDED(h.device->SetProperty(DIPROP_RANGE, &range.diph));
        h.forceAxes[i] = (object.dwFlags & DIDOI_FFACTUATOR) != 0;
        DIPROPDWORD dead{}; dead.diph = {sizeof(dead), sizeof(dead.diph), offsets[i], DIPH_BYOFFSET};
        dead.dwData = 0; h.device->SetProperty(DIPROP_DEADZONE, &dead.diph);
        dead.dwData = 10000; h.device->SetProperty(DIPROP_SATURATION, &dead.diph);
    }
}
}
struct Rig::Hardware {
    HWND window = nullptr;
    IDirectInput8A* input = nullptr;
    std::vector<std::unique_ptr<Handle>> handles;
    bool rescan = true, focused = false;
    ULONGLONG scanAt = 0, forceAt = 0;
    std::string status = "FFB enabled - forces run only while driving";
    ~Hardware() { handles.clear(); if (input) input->Release(); }
    void Enumerate() {
        if (!input || !window) return;
        InputCallTimer timer("wheel device enumeration");
        std::vector<DIDEVICEINSTANCEA> instances;
        const HRESULT result = input->EnumDevices(DI8DEVCLASS_GAMECTRL, [](const DIDEVICEINSTANCEA* i, VOID* p) -> BOOL {
            static_cast<std::vector<DIDEVICEINSTANCEA>*>(p)->push_back(*i); return DIENUM_CONTINUE;
        }, &instances, DIEDFL_ATTACHEDONLY);
        if (FAILED(result)) return;
        std::erase_if(handles, [&](const auto& h) {
            return std::none_of(instances.begin(), instances.end(), [&](const auto& i) { return h->state.id == InstanceId(&i.guidInstance); });
        });
        for (const auto& i : instances) {
            const std::string id = InstanceId(&i.guidInstance);
            if (std::any_of(handles.begin(), handles.end(), [&](const auto& h) { return h->state.id == id; })) continue;
            auto h = std::make_unique<Handle>();
            if (FAILED(input->CreateDevice(i.guidInstance, &h->device, nullptr))) continue;
            if (FAILED(h->device->SetDataFormat(&c_dfDIJoystick2)) || FAILED(h->device->SetCooperativeLevel(window, DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) continue;
            h->state.id = id; h->state.name = i.tszProductName;
            DIPROPDWORD identity{}; identity.diph = {sizeof(identity), sizeof(identity.diph), 0, DIPH_DEVICE};
            const DWORD pidvid = SUCCEEDED(h->device->GetProperty(DIPROP_VIDPID, &identity.diph)) ? identity.dwData : i.guidProduct.Data1;
            h->state.vendor = LOWORD(pidvid); h->state.product = HIWORD(pidvid);
            DIPROPGUIDANDPATH path{}; path.diph = {sizeof(path), sizeof(path.diph), 0, DIPH_DEVICE};
            if (SUCCEEDED(h->device->GetProperty(DIPROP_GUIDANDPATH, &path.diph))) {
                std::wstring name = path.wszPath;
                for (auto& ch : name) if (ch >= L'A' && ch <= L'Z') ch += L'a' - L'A';
                const auto col = name.find(L"&col");
                if (col != std::wstring::npos) h->state.hidCollection = int(std::wcstoul(name.c_str() + col + 4, nullptr, 16));
            }
            ReadAxes(*h);
            DIDEVCAPS caps{}; caps.dwSize = sizeof(caps);
            h->state.forceCapable = SUCCEEDED(h->device->GetCapabilities(&caps)) && (caps.dwFlags & DIDC_FORCEFEEDBACK);
            h->state.buttonCount = int(std::min<DWORD>(caps.dwButtons, 128));
            h->state.hatCount = int(std::min<DWORD>(caps.dwPOVs, 4));
            std::printf("wheel: USB %04x:%04x collection %d%s\n", h->state.vendor, h->state.product, h->state.hidCollection,
                FitsDeviceRole(h->state, DeviceRole::Any) ? "" : " (auxiliary interface, excluded from automatic selection)");
            std::printf("wheel: device %s [%s], FFB %s\n", h->state.name.c_str(), id.c_str(), h->state.forceCapable ? "available" : "unavailable");
            InputDiagnostic("detected " + h->state.name + " [" + id + "]");
            handles.push_back(std::move(h));
        }
    }
    void Stop() { for (auto& h : handles) h->Stop(); forceAt = 0; }
    void Poll(bool focus, std::vector<DeviceState>& states) {
        InputCallTimer timer("wheel hardware poll (including enumeration/acquire)");
        focused = focus;
        const auto now = GetTickCount64();
        if (rescan && now >= scanAt) { Enumerate(); rescan = false; scanAt = now + 250; }
        if (!focus || (forceAt && now - forceAt > 150)) Stop();
        states.clear();
        const auto& c = Config();
        for (auto& h : handles) {
            const bool exclusive = c.enabled && c.feedback && focus && h->state.id == c.axes[Steering].device && h->state.forceCapable;
            if (exclusive != h->exclusive) {
                h->Stop(); h->RestoreAutocenter(); h->device->Unacquire();
                const HRESULT hr = h->device->SetCooperativeLevel(window, DISCL_BACKGROUND | (exclusive ? DISCL_EXCLUSIVE : DISCL_NONEXCLUSIVE));
                if (SUCCEEDED(hr)) h->exclusive = exclusive;
                else status = "FFB access unavailable; close other wheel software";
            }
            DIJOYSTATE2 js{};
            auto read = [&]() {
                const HRESULT poll = h->device->Poll();
                return FAILED(poll) ? poll : h->device->GetDeviceState(sizeof(js), &js);
            };
            HRESULT hr = read();
            if (hr == DIERR_NOTACQUIRED || hr == DIERR_INPUTLOST) {
                hr = h->device->Acquire();
                if (SUCCEEDED(hr)) hr = read();
            }
            if (h->state.online != bool(SUCCEEDED(hr))) {
                char result[32]; std::snprintf(result, sizeof(result), " HRESULT=%08lx", static_cast<unsigned long>(hr));
                InputDiagnostic(h->state.name + " [" + h->state.id + "] " + (SUCCEEDED(hr) ? "online" : "offline") + result);
            }
            h->state.online = SUCCEEDED(hr);
            if (h->state.online) {
                const LONG axes[] = {js.lX, js.lY, js.lZ, js.lRx, js.lRy, js.lRz, js.rglSlider[0], js.rglSlider[1]};
                for (int a = 0; a < kAxes; ++a) h->state.axes[a] = std::clamp(int(axes[a]), -32768, 32767);
                for (int b = 0; b < 128; ++b) h->state.buttons[b] = (js.rgbButtons[b] & 0x80) != 0;
                for (int p = 0; p < 4; ++p) for (int d = 0; d < 4; ++d) {
                    const DWORD angle = js.rgdwPOV[p];
                    const int delta = angle < 36000 ? (int(angle) - d * 9000 + 36000) % 36000 : 18000;
                    h->state.buttons[128 + p * 4 + d] = delta <= 4500 || delta >= 31500;
                }
            } else h->Stop();
            states.push_back(h->state);
        }
    }
    void Force(float force) {
        InputCallTimer timer("wheel FFB driver update");
        const auto& binding = Config().axes[Steering];
        if (!focused) { Stop(); return; }
        for (auto& h : handles) if (h->state.id == binding.device) {
            if (!h->state.online || !h->exclusive || binding.axis < 0 || binding.axis >= kAxes || !h->forceAxes[binding.axis]) {
                h->Stop(); status = "No FFB actuator on the assigned steering axis"; return;
            }
            if (h->effectAxis != binding.axis) h->Stop();
            DWORD axis = offsets[binding.axis]; LONG direction = 10000;
            DICONSTANTFORCE constant{LONG(std::clamp(force, -1.f, 1.f) * 10000)};
            DIEFFECT effect{}; effect.dwSize = sizeof(effect); effect.dwFlags = DIEFF_CARTESIAN | DIEFF_OBJECTOFFSETS;
            effect.dwDuration = 200000; // Driver-enforced 200 ms expiry also covers a stalled application.
            effect.dwGain = 10000; effect.dwTriggerButton = DIEB_NOTRIGGER; effect.cAxes = 1;
            effect.rgdwAxes = &axis; effect.rglDirection = &direction;
            effect.cbTypeSpecificParams = sizeof(constant); effect.lpvTypeSpecificParams = &constant;
            HRESULT hr = DI_OK;
            if (!h->effect) {
                DIPROPDWORD property{}; property.diph = {sizeof(property), sizeof(property.diph), 0, DIPH_DEVICE};
                if (!h->savedAutocenter && SUCCEEDED(h->device->GetProperty(DIPROP_AUTOCENTER, &property.diph))) {
                    h->autocenter = property.dwData; h->savedAutocenter = true;
                }
                property.dwData = DIPROPAUTOCENTER_OFF;
                h->device->SetProperty(DIPROP_AUTOCENTER, &property.diph);
                hr = h->device->CreateEffect(GUID_ConstantForce, &effect, &h->effect, nullptr);
                h->effectAxis = binding.axis;
            } else hr = h->effect->SetParameters(&effect, DIEP_TYPESPECIFICPARAMS);
            if (SUCCEEDED(hr) && h->effect) hr = h->effect->Start(1, 0);
            if (FAILED(hr)) { h->Stop(); status = "FFB driver rejected constant force (check PC mode)"; }
            else { h->effectRunning = true; forceAt = GetTickCount64(); status = "Force feedback ready"; }
            return;
        }
        status = "Steering device disconnected";
    }
};
#else
std::string InstanceId(const void*) { return {}; }
struct Rig::Hardware {
    std::string status = "USB wheels are supported on Windows PC / PCVR";
    void Poll(bool, std::vector<DeviceState>&) {}
    void Stop() {}
    void Force(float) {}
};
#endif
Rig::Rig() : hardware_(std::make_unique<Hardware>()) {}
Rig::~Rig() = default;
void Rig::Attach(void* window) {
#ifdef _WIN32
    hardware_->window = static_cast<HWND>(window);
    if (!hardware_->input) DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&hardware_->input), nullptr);
    hardware_->rescan = true;
#else
    (void)window;
#endif
}
void Rig::Rescan() {
#ifdef _WIN32
    hardware_->rescan = true;
#endif
}
bool Rig::Claims(const std::string& id) const {
    if (!captureDevice_.empty() && captureDevice_ == id) return true;
    for (const auto& d : devices_) if (d.id == id && IsRacingDevice(d)) return true;
    if (!Config().enabled) return false;
    for (const auto& a : Config().axes) if (a.device == id && a.axis >= 0) return true;
    for (const auto& b : Config().buttons) if (b.device == id && b.button >= 0) return true;
    for (const auto& b : Config().navigation) if (b.device == id && b.button >= 0) return true;
    return false;
}
void Rig::Poll(bool focused) {
    hardware_->Poll(focused, devices_);
    const State previous = state_;
#ifdef _WIN32
    if (AutoConfigure(Config(), devices_, autoStatus_)) {
        try { SaveSettings(); saveError_.clear(); }
        catch (const std::exception& e) { saveError_ = std::string("Could not save wheel setup: ") + e.what(); }
        std::printf("wheel: profile %s\n", Config().profile.c_str());
    }
    navigation_ = Navigation(Config(), devices_, focused);
    state_ = Resolve(Config(), devices_, focused);
    if (previous.ready != state_.ready || previous.shifterDisconnected != state_.shifterDisconnected)
        InputDiagnostic("wheel ready=" + std::to_string(state_.ready) + " shifter fallback=" + std::to_string(state_.shifterDisconnected));
#else
    state_ = {};
#endif
    for (int i = 0; i < ButtonCount; ++i) pressed_[i] = focused && state_.held[i] && !previous.held[i];
    if (!state_.ready || !Config().feedback) Stop();
}
void Rig::Stop() { hardware_->Stop(); filteredForce_ = 0; lastSteering_ = state_.steering; feedbackTime_ = {}; moments_ = {}; }
void Rig::Feedback(const FeedbackFrame& frame, bool driving) {
    if (!state_.ready || !Config().feedback || Config().gain <= 0 || !driving) { Stop(); return; }
    const auto now = std::chrono::steady_clock::now();
    const bool first = feedbackTime_ == std::chrono::steady_clock::time_point{};
    const float elapsed = first ? 1.f / 30.f : std::chrono::duration<float>(now - feedbackTime_).count();
    if (!first && elapsed < .001f) return; // catch-up physics must not amplify device derivatives
    const float dt = std::clamp(elapsed, .001f, .1f);
    float physicalSteering = state_.steering;
    auto axis = Config().axes[Steering];
    axis.deadzone = 0; axis.saturation = 100; axis.curve = 100;
    for (const auto& device : devices_)
        if (device.online && device.id == axis.device && axis.Valid(true)) physicalSteering = axis.Map(device.axes[axis.axis], true);
    const float velocity = first || elapsed > .15f ? 0 : (physicalSteering - lastSteering_) / elapsed;
    feedbackTime_ = now;
    lastSteering_ = physicalSteering;
    moments_ = CalculateSteeringMoments(frame, Config().steeringGeometry);
    if (!moments_.valid) { Stop(); return; }
    const float target = Force(moments_, velocity, Config());
    const float slew = std::clamp(Config().gain, 0, 100) * .08f * dt;
    filteredForce_ += std::clamp(target - filteredForce_, -slew, slew);
    const float gain = std::clamp(Config().gain, 0, 100) * .01f;
    filteredForce_ = std::clamp(filteredForce_, -gain, gain);
    hardware_->Force(filteredForce_);
}
std::string Rig::Status() const {
#ifdef _WIN32
    if (!saveError_.empty()) return saveError_;
    if (!Config().enabled) return Config().automatic ? "Connect your wheel to start" : "Wheel input is off";
    if (!hardware_->focused) return "Return to the game window to use wheel controls";
    if (state_.ready && state_.shifterDisconnected) return "Shifter disconnected - using paddles; reconnect to restore H-pattern";
    if (!autoStatus_.empty()) return autoStatus_;
    if (!state_.ready) return "Check connected devices or open Guided setup";
    if (!Config().feedback) return "Inputs ready - force feedback is off";
#endif
    return hardware_->status;
}
}
