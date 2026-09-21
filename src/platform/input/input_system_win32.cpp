#include <windows.h>
#include "platform/input/input_system.h"
#include "platform/input/dualsense.h"

#define DIRECTINPUT_VERSION 0x0800
#include <dinput.h>
#include <xinput.h>

#include <algorithm>
#include <cctype>
#include <cwchar>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace gt2::input {

namespace {

// ---------------------------------------------------------------- XInput

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);

struct XInputApi {
    HMODULE module = nullptr;
    XInputGetStateFn getState = nullptr;
    XInputSetStateFn setState = nullptr;
    XInputApi() {
        for (const char* name : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
            module = LoadLibraryA(name); // system DLLs of Windows / the DirectX runtime
            if (module) break;
        }
        if (!module) return;
        getState = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
        setState = reinterpret_cast<XInputSetStateFn>(reinterpret_cast<void*>(GetProcAddress(module, "XInputSetState")));
        if (!getState || !setState) getState = nullptr, setState = nullptr;
    }
    ~XInputApi() {
        if (module) FreeLibrary(module);
    }
    bool Ok() const { return getState != nullptr; }
};

XInputApi& XInput() {
    static XInputApi api;
    return api;
}

// XInput thumb (-32768..32767, y up) -> a PS1 stick byte (0x80 centre, 0 = left / up).
uint8_t StickX(SHORT v) { return uint8_t((int32_t(v) + 32768) >> 8); }
uint8_t StickY(SHORT v) { return uint8_t((32767 - int32_t(v)) >> 8); }

class XInputDevice : public Device {
public:
    explicit XInputDevice(DWORD slot) : slot_(slot) {}
    ~XInputDevice() override { SetMotors(0, 0, 100); }
    bool Poll(int, Ps1PadFrame& out) override {
        XINPUT_STATE s{};
        if (XInput().getState(slot_, &s) != ERROR_SUCCESS) return false;
        const XINPUT_GAMEPAD& g = s.Gamepad;
        out = Ps1PadFrame{};
        out.type = kTypeAnalog;
        uint16_t b = 0;
        const WORD w = g.wButtons;
        if (w & XINPUT_GAMEPAD_A) b |= ps1::kCross;
        if (w & XINPUT_GAMEPAD_B) b |= ps1::kCircle;
        if (w & XINPUT_GAMEPAD_X) b |= ps1::kSquare;
        if (w & XINPUT_GAMEPAD_Y) b |= ps1::kTriangle;
        if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= ps1::kL1;
        if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= ps1::kR1;
        if (w & XINPUT_GAMEPAD_BACK) b |= ps1::kSelect;
        if (w & XINPUT_GAMEPAD_START) b |= ps1::kStart;
        if (w & XINPUT_GAMEPAD_LEFT_THUMB) b |= ps1::kL3;
        if (w & XINPUT_GAMEPAD_RIGHT_THUMB) b |= ps1::kR3;
        if (w & XINPUT_GAMEPAD_DPAD_UP) b |= ps1::kUp;
        if (w & XINPUT_GAMEPAD_DPAD_DOWN) b |= ps1::kDown;
        if (w & XINPUT_GAMEPAD_DPAD_LEFT) b |= ps1::kLeft;
        if (w & XINPUT_GAMEPAD_DPAD_RIGHT) b |= ps1::kRight;
        if (g.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= ps1::kL2;
        if (g.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= ps1::kR2;
        out.buttons = b;
        out.analog = {StickX(g.sThumbRX), StickY(g.sThumbRY), StickX(g.sThumbLX), StickY(g.sThumbLY)};
        out.pressure = true;
        out.pressureR2 = g.bRightTrigger;
        out.pressureL2 = g.bLeftTrigger;
        return true;
    }
    void SetMotors(uint8_t smallMotor, uint8_t largeMotor, int strength) override {
        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = WORD(largeMotor * 257u * unsigned(strength) / 100);             // the low-frequency (large) motor
        v.wRightMotorSpeed = WORD((smallMotor & 1) ? 65535u * unsigned(strength) / 100 : 0); // the high-frequency (small) motor: on / off like the DualShock
        XInput().setState(slot_, &v);
    }
    bool HasMotors() const override { return true; }
    std::string Name() const override { return "XInput pad " + std::to_string(slot_ + 1); }
    DWORD Slot() const { return slot_; }

private:
    DWORD slot_;
};

// ---------------------------------------------------------------- fake pad (--fake-pad)

class FakePad : public Device {
public:
    struct Item {
        int field = 0, hold = -1; // hold -1: until the next item of the control
        std::string control;
        int value = 0;
    };
    explicit FakePad(std::vector<Item> items, int port = 1) : items_(std::move(items)), port_(port) {
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.field < b.field; });
    }
    int Port() const { return port_; } // the PS1 port the script drives (--fake-pad 1, --fake-pad2 2)
    bool Poll(int field, Ps1PadFrame& out) override {
        out = Ps1PadFrame{};
        out.type = kTypeAnalog;
        out.pressure = true;
        // Per control: the latest item at or before this field wins; an item with a hold returns the control to rest
        // after `hold` fields.
        std::map<std::string, const Item*> latest;
        for (const Item& it : items_) {
            if (it.field > field) break;
            latest[it.control] = &it;
        }
        std::map<std::string, int> values;
        for (const auto& [name, it] : latest)
            if (it->hold < 0 || field < it->field + it->hold) values[name] = it->value;
        static const std::map<std::string, uint16_t> kButtons = {
            {"select", ps1::kSelect}, {"l3", ps1::kL3},     {"r3", ps1::kR3},           {"start", ps1::kStart},   {"up", ps1::kUp},
            {"right", ps1::kRight},   {"down", ps1::kDown}, {"left", ps1::kLeft},       {"l2", ps1::kL2},         {"r2", ps1::kR2},
            {"l1", ps1::kL1},         {"r1", ps1::kR1},     {"triangle", ps1::kTriangle}, {"circle", ps1::kCircle}, {"cross", ps1::kCross},
            {"square", ps1::kSquare}};
        for (const auto& [name, v] : values) {
            const auto b = kButtons.find(name);
            if (b != kButtons.end()) {
                if (v) out.buttons = uint16_t(out.buttons | b->second);
            } else if (name == "type") {
                out.type = uint8_t(v);
            } else if (name == "rx" || name == "a0") {
                out.analog[0] = uint8_t(v);
            } else if (name == "ry" || name == "a1") {
                out.analog[1] = uint8_t(v);
            } else if (name == "lx" || name == "a2") {
                out.analog[2] = uint8_t(v);
            } else if (name == "ly" || name == "a3") {
                out.analog[3] = uint8_t(v);
            } else if (name == "r2p") {
                out.pressureR2 = uint8_t(v);
            } else if (name == "l2p") {
                out.pressureL2 = uint8_t(v);
            } else if (name == "pressure") {
                out.pressure = v != 0;
            }
        }
        if (out.type == kTypeNegcon && !values.count("a1") && !values.count("a2") && !values.count("a3")) out.analog[1] = out.analog[2] = out.analog[3] = 0;
        field_ = field;
        return true;
    }
    void SetMotors(uint8_t smallMotor, uint8_t largeMotor, int strength) override {
        largeMotor = uint8_t(int(largeMotor) * strength / 100);
        if (strength == 0) smallMotor = 0;
        if (smallMotor == smallMotor_ && largeMotor == largeMotor_) return;
        smallMotor_ = smallMotor, largeMotor_ = largeMotor;
        std::printf("fake-pad f%d: motors small %u large %u\n", field_, unsigned(smallMotor), unsigned(largeMotor));
    }
    bool HasMotors() const override { return true; }
    std::string Name() const override { return "fake pad"; }
    bool Scripted() const override { return true; }

private:
    std::vector<Item> items_;
    int port_ = 1;
    int field_ = 0;
    uint8_t smallMotor_ = 0, largeMotor_ = 0;
};

std::vector<FakePad::Item> ParseFakePad(const std::string& scriptOrFile) {
    std::string text = scriptOrFile;
    if (std::ifstream f{scriptOrFile}; f) { // a file: items separated by commas, newlines or spaces; '#' comments
        std::ostringstream all;
        std::string line;
        while (std::getline(f, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            all << line << ',';
        }
        text = all.str();
    }
    static const std::map<std::string, int> kTypes = {{"digital", kTypeDigital}, {"analog", kTypeAnalog}, {"negcon", kTypeNegcon}, {"none", kTypeNone}};
    std::vector<FakePad::Item> items;
    std::string item;
    std::istringstream in(text);
    auto flush = [&] {
        size_t a = item.find_first_not_of(" \t\r\n"), b = item.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) { item.clear(); return; }
        const std::string s = item.substr(a, b - a + 1);
        item.clear();
        const size_t c1 = s.find(':'), eq = s.find('=');
        if (c1 == std::string::npos || eq == std::string::npos || eq < c1) throw std::runtime_error("bad --fake-pad item " + s);
        const size_t c2 = s.find(':', eq);
        FakePad::Item it;
        it.field = std::atoi(s.substr(0, c1).c_str());
        it.control = s.substr(c1 + 1, eq - c1 - 1);
        for (char& ch : it.control) ch = char(std::tolower(uint8_t(ch)));
        const std::string value = s.substr(eq + 1, c2 == std::string::npos ? std::string::npos : c2 - eq - 1);
        if (it.control == "type") {
            const auto t = kTypes.find(value);
            it.value = t != kTypes.end() ? t->second : std::atoi(value.c_str());
        } else {
            it.value = int(std::strtol(value.c_str(), nullptr, 0));
        }
        if (c2 != std::string::npos) it.hold = std::max(1, std::atoi(s.substr(c2 + 1).c_str()));
        items.push_back(it);
    };
    for (char ch; in.get(ch);) {
        if (ch == ',' || ch == '\n' || ch == ';') flush();
        else item.push_back(ch);
    }
    flush();
    return items;
}

// A device is "in use" in a field when a button is down or an axis is well away from where it was when the device
// appeared (sticks and pedals at rest; a device that reports centred axes before its first real report stays quiet).
bool Active(const Ps1PadFrame& f, const Ps1PadFrame& rest) {
    if (f.type == kTypeNone) return false;
    if (f.buttons) return true;
    auto away = [](uint8_t v, uint8_t r) { return (v > r ? v - r : r - v) > 0x30; };
    for (size_t k = 0; k < 4; k++)
        if (away(f.analog[k], rest.analog[k])) return true;
    return f.pressure && (away(f.pressureR2, rest.pressure ? rest.pressureR2 : 0) || away(f.pressureL2, rest.pressure ? rest.pressureL2 : 0));
}

} // namespace

// ---------------------------------------------------------------- DirectInput

struct InputSystem::DirectInputState {
    IDirectInput8A* di = nullptr;
    ~DirectInputState() {
        if (di) di->Release();
    }
};

namespace {

class DirectInputDevice : public Device {
public:
    enum class Layout { kSony, kGeneric, kWheel };
    DirectInputDevice(IDirectInputDevice8A* device, const GUID& instance, Layout layout, std::string name)
        : device_(device), instance_(instance), layout_(layout), name_(std::move(name)) {}
    ~DirectInputDevice() override {
        if (device_) {
            device_->Unacquire();
            device_->Release();
        }
    }
    bool Poll(int, Ps1PadFrame& out) override {
        if (effects_) {
            const int native = effects_->ReadPad(out);
            if (native) return native > 0;
        }
        if (FAILED(device_->Poll())) {
            const HRESULT r = device_->Acquire();
            if (r == DIERR_UNPLUGGED || r == DIERR_INPUTLOST) return false;
        }
        DIJOYSTATE2 s{};
        const HRESULT r = device_->GetDeviceState(sizeof(s), &s);
        if (r == DIERR_INPUTLOST || r == DIERR_NOTACQUIRED) {
            if (FAILED(device_->Acquire())) return false;
            if (FAILED(device_->GetDeviceState(sizeof(s), &s))) return false;
        } else if (FAILED(r)) {
            return false;
        }
        auto axis = [](LONG v) { return uint8_t(std::clamp<LONG>(v, 0, 255)); };
        auto button = [&](int i) { return (s.rgbButtons[i] & 0x80) != 0; };
        out = Ps1PadFrame{};
        uint16_t b = 0;
        const DWORD pov = s.rgdwPOV[0];
        if (LOWORD(pov) != 0xFFFF) { // hundredths of a degree, clockwise from up
            if (pov >= 31500 || pov <= 4500) b |= ps1::kUp;
            if (pov >= 4500 && pov <= 13500) b |= ps1::kRight;
            if (pov >= 13500 && pov <= 22500) b |= ps1::kDown;
            if (pov >= 22500 && pov <= 31500) b |= ps1::kLeft;
        }
        if (layout_ == Layout::kWheel) { // a neGcon: twist, I (accelerator), II (brake), L (clutch); buttons A / B / R / Start
            out.type = kTypeNegcon;
            if (button(0)) b |= ps1::kCircle;   // neGcon A
            if (button(1)) b |= ps1::kTriangle; // neGcon B
            if (button(2) || button(4)) b |= ps1::kR1;
            if (button(3) || button(9)) b |= ps1::kStart;
            if (button(5)) b |= ps1::kUp; // (a paddle as a D-pad direction: menus)
            out.buttons = b;
            auto pedal = [&](LONG v, bool inverted) { const uint8_t p = axis(v); return uint8_t(inverted ? 255 - p : p); };
            out.analog = {axis(s.lX), pedal(s.lY, invert_[0]), pedal(s.lRz, invert_[1]), pedal(s.lZ, invert_[2])};
            return true;
        }
        out.type = kTypeAnalog;
        static const uint16_t kSony[12] = {ps1::kSquare, ps1::kCross, ps1::kCircle, ps1::kTriangle, ps1::kL1, ps1::kR1,
                                           ps1::kL2,     ps1::kR2,    ps1::kSelect, ps1::kStart,    ps1::kL3, ps1::kR3};
        static const uint16_t kGeneric[12] = {ps1::kCross, ps1::kCircle, ps1::kSquare, ps1::kTriangle, ps1::kL1, ps1::kR1,
                                              ps1::kL2,    ps1::kR2,     ps1::kSelect, ps1::kStart,    ps1::kL3, ps1::kR3};
        const uint16_t* map = layout_ == Layout::kSony ? kSony : kGeneric;
        for (int i = 0; i < 12; i++)
            if (button(i)) b |= map[i];
        out.buttons = b;
        out.analog = {axis(s.lZ), axis(s.lRz), axis(s.lX), axis(s.lY)};
        if (layout_ == Layout::kSony) { // the triggers' travel on Rx (L2) / Ry (R2), trusted once both were seen released
            const uint8_t l2 = axis(s.lRx), r2 = axis(s.lRy);
            if (l2 < 0x20) triggerRest_[0] = true;
            if (r2 < 0x20) triggerRest_[1] = true;
            if (triggerRest_[0] && triggerRest_[1]) {
                out.pressure = true;
                out.pressureL2 = l2;
                out.pressureR2 = r2;
            }
        }
        return true;
    }
    // Pedals that rest at the top of their range are read inverted (sampled once when the wheel is opened).
    void SamplePedals() {
        DIJOYSTATE2 s{};
        device_->Poll();
        if (FAILED(device_->GetDeviceState(sizeof(s), &s))) return;
        invert_[0] = s.lY > 127;
        invert_[1] = s.lRz > 127;
        invert_[2] = s.lZ > 127;
    }
    void OpenEffects(const wchar_t* path) { effects_ = std::make_unique<DualSenseEffects>(path); }
    void SetMotors(uint8_t smallMotor, uint8_t largeMotor, int strength) override { if (effects_) effects_->Motors(smallMotor, largeMotor, strength); }
    bool HasMotors() const override { return effects_ && effects_->Available(); }
    void SetTriggers(uint8_t a, uint8_t b) override { if (effects_) effects_->Triggers(a, b); }
    bool HasTriggers() const override { return HasMotors(); }
    std::string Name() const override { return name_; }
    const GUID& Instance() const { return instance_; }

private:
    IDirectInputDevice8A* device_;
    std::unique_ptr<DualSenseEffects> effects_;
    GUID instance_;
    Layout layout_;
    std::string name_;
    bool invert_[3] = {false, false, false};
    bool triggerRest_[2] = {false, false};
};

struct EnumContext {
    std::vector<DIDEVICEINSTANCEA> found;
};

BOOL CALLBACK EnumDevice(LPCDIDEVICEINSTANCEA instance, LPVOID user) {
    static_cast<EnumContext*>(user)->found.push_back(*instance);
    return DIENUM_CONTINUE;
}

} // namespace

// ---------------------------------------------------------------- the system

InputSystem::InputSystem() = default;

InputSystem::~InputSystem() {
    for (auto& d : devices_) d->SetMotors(0, 0);
    devices_.clear();
    dinput_.reset();
}

void InputSystem::AttachWindow(void* hwnd) {
    hwnd_ = hwnd;
    rescan_ = true;
}

void InputSystem::AddFakePad(const std::string& scriptOrFile, int port) {
    devices_.push_back(std::make_unique<FakePad>(ParseFakePad(scriptOrFile), port));
    frames_.emplace_back();
    lastActivity_.push_back(-1);
    rest_.emplace_back();
    log_.push_back("connected: fake pad (--fake-pad)");
}

std::vector<std::string> InputSystem::TakeLog() {
    std::vector<std::string> out;
    out.swap(log_);
    return out;
}

void InputSystem::Rescan(int field) {
    lastScan_ = field;
    // XInput slots not open yet.
    if (XInput().Ok()) {
        for (DWORD slot = 0; slot < XUSER_MAX_COUNT; slot++) {
            bool open = false;
            for (const auto& d : devices_)
                if (auto* x = dynamic_cast<XInputDevice*>(d.get()); x && x->Slot() == slot) open = true;
            if (open) continue;
            XINPUT_STATE s{};
            if (XInput().getState(slot, &s) != ERROR_SUCCESS) continue;
            devices_.push_back(std::make_unique<XInputDevice>(slot));
            frames_.emplace_back();
            lastActivity_.push_back(-1);
            rest_.emplace_back();
            log_.push_back("connected: " + devices_.back()->Name());
        }
    }
    // DirectInput game controllers (only on a device change: enumeration is slow).
    if (!rescan_ || !hwnd_) return;
    rescan_ = false;
    if (!dinput_) {
        dinput_ = std::make_unique<DirectInputState>();
        if (FAILED(DirectInput8Create(GetModuleHandleA(nullptr), DIRECTINPUT_VERSION, IID_IDirectInput8A, reinterpret_cast<void**>(&dinput_->di), nullptr)))
            dinput_->di = nullptr;
    }
    if (!dinput_->di) return;
    EnumContext ctx;
    dinput_->di->EnumDevices(DI8DEVCLASS_GAMECTRL, EnumDevice, &ctx, DIEDFL_ATTACHEDONLY);
    for (const DIDEVICEINSTANCEA& inst : ctx.found) {
        bool open = false;
        for (const auto& d : devices_)
            if (auto* x = dynamic_cast<DirectInputDevice*>(d.get()); x && IsEqualGUID(x->Instance(), inst.guidInstance)) open = true;
        if (open) continue;
        IDirectInputDevice8A* device = nullptr;
        if (FAILED(dinput_->di->CreateDevice(inst.guidInstance, &device, nullptr)) || !device) continue;
        DIPROPGUIDANDPATH path{};
        path.diph.dwSize = sizeof(path);
        path.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        path.diph.dwHow = DIPH_DEVICE;
        const bool xinput = SUCCEEDED(device->GetProperty(DIPROP_GUIDANDPATH, &path.diph)) && std::wcsstr(path.wszPath, L"IG_") != nullptr;
        if (xinput) { // read through XInput instead (its motors, its trigger axes)
            device->Release();
            continue;
        }
        DIPROPRANGE range{};
        range.diph.dwSize = sizeof(range);
        range.diph.dwHeaderSize = sizeof(DIPROPHEADER);
        range.diph.dwHow = DIPH_DEVICE;
        range.lMin = 0;
        range.lMax = 255;
        if (FAILED(device->SetDataFormat(&c_dfDIJoystick2)) || FAILED(device->SetCooperativeLevel(static_cast<HWND>(hwnd_), DISCL_BACKGROUND | DISCL_NONEXCLUSIVE))) {
            device->Release();
            continue;
        }
        device->SetProperty(DIPROP_RANGE, &range.diph);
        device->Acquire();
        const uint16_t vid = LOWORD(inst.guidProduct.Data1);
        const BYTE kind = GET_DIDEVICE_TYPE(inst.dwDevType);
        DirectInputDevice::Layout layout = kind == DI8DEVTYPE_DRIVING ? DirectInputDevice::Layout::kWheel
                                           : vid == 0x054C             ? DirectInputDevice::Layout::kSony
                                                                       : DirectInputDevice::Layout::kGeneric;
        auto d = std::make_unique<DirectInputDevice>(device, inst.guidInstance, layout, std::string(inst.tszProductName));
        const uint16_t pid = HIWORD(inst.guidProduct.Data1);
        if (vid == 0x054C && (pid == 0x0CE6 || pid == 0x0DF2)) d->OpenEffects(path.wszPath);
        if (layout == DirectInputDevice::Layout::kWheel) d->SamplePedals();
        log_.push_back(std::string("connected: ") + inst.tszProductName + (layout == DirectInputDevice::Layout::kWheel ? " (DirectInput wheel as a neGcon)"
                                                                            : layout == DirectInputDevice::Layout::kSony ? " (DirectInput, Sony layout)"
                                                                                                                        : " (DirectInput, generic layout)"));
        devices_.push_back(std::move(d));
        frames_.emplace_back();
        lastActivity_.push_back(-1);
        rest_.emplace_back();
    }
}

void InputSystem::Poll(int field, bool focused) {
    focused_ = focused;
    if (!focused) StopFeedback();
    if (rescan_ || field - lastScan_ >= 120) Rescan(field);
    for (size_t i = 0; i < devices_.size();) {
        Ps1PadFrame f;
        if (!devices_[i]->Poll(field, f)) {
            log_.push_back("disconnected: " + devices_[i]->Name());
            if (active_ == int(i)) active_ = -1;
            else if (active_ > int(i)) active_--;
            if (appliedDevice_ == int(i)) appliedDevice_ = -1;
            else if (appliedDevice_ > int(i)) appliedDevice_--;
            devices_.erase(devices_.begin() + std::ptrdiff_t(i));
            frames_.erase(frames_.begin() + std::ptrdiff_t(i));
            lastActivity_.erase(lastActivity_.begin() + std::ptrdiff_t(i));
            rest_.erase(rest_.begin() + std::ptrdiff_t(i));
            continue;
        }
        if (!focused && !devices_[i]->Scripted()) { // a real pad counts only while the window is in the foreground
            const uint8_t type = f.type;
            f = Ps1PadFrame{};
            f.type = type;
        }
        if (lastActivity_[i] == -1) rest_[i] = f, lastActivity_[i] = -2; // the first frame: the device's rest position
        frames_[i] = f;
        if (Active(f, rest_[i])) lastActivity_[i] = field;
        i++;
    }
    int best = active_;
    if (best >= 0 && frames_[size_t(best)].type == kTypeNone) best = -1;
    for (size_t i = 0; i < devices_.size(); i++)
        if (frames_[i].type != kTypeNone && (best < 0 || lastActivity_[i] > lastActivity_[size_t(best)])) best = int(i);
    for (size_t i = 0; i < devices_.size(); i++) // a scripted pad (--fake-pad) is port 1 whenever it exists: reproducible runs
        if (devices_[i]->Scripted() && static_cast<const FakePad*>(devices_[i].get())->Port() == 1) best = int(i);
    if (best >= 0 && devices_[size_t(best)]->Scripted() && static_cast<const FakePad*>(devices_[size_t(best)].get())->Port() != 1) best = -1;
    if (best != active_ && best >= 0) log_.push_back("port 1: " + devices_[size_t(best)]->Name());
    active_ = best;
    for (size_t i = 0; i < devices_.size(); ++i) devices_[i]->SetActive(int(i) == active_);
    port1_ = active_ >= 0 ? frames_[size_t(active_)] : Ps1PadFrame{};
    // Port 2 (the 2 player Battle): a --fake-pad2 script, else the most recently used of the other devices (a second pad).
    int second = -1;
    for (size_t i = 0; i < devices_.size(); i++) {
        if (int(i) == active_) continue;
        if (frames_[i].type == kTypeNone) continue;
        if (devices_[i]->Scripted()) {
            if (static_cast<const FakePad*>(devices_[i].get())->Port() == 2) { second = int(i); break; }
            continue;
        }
        if (second < 0 || lastActivity_[i] > lastActivity_[size_t(second)]) second = int(i);
    }
    if (second != port2Device_) log_.push_back(second >= 0 ? "port 2: " + devices_[size_t(second)]->Name() : std::string("port 2: none"));
    port2Device_ = second;
    port2_ = second >= 0 ? frames_[size_t(second)] : Ps1PadFrame{};
    // Motors: silent unless the game set the actuators during the previous field.
    if (!actuatorsSet_) actuators_ = Actuators{};
    actuatorsSet_ = false;
    ApplyMotors();
}

void InputSystem::SetActuators(const Actuators& a) {
    actuators_ = a;
    actuatorsSet_ = true;
    ApplyMotors();
}

void InputSystem::ApplyMotors() {
    Actuators a = actuators_;
    if (!focused_ && (active_ < 0 || !devices_[size_t(active_)]->Scripted())) a = {};
    if (appliedDevice_ != active_ && appliedDevice_ >= 0 && size_t(appliedDevice_) < devices_.size()) {
        devices_[size_t(appliedDevice_)]->SetMotors(0, 0);
        devices_[size_t(appliedDevice_)]->SetTriggers(0, 0);
    }
    if (active_ < 0) {
        appliedDevice_ = -1;
        return;
    }
    if (appliedDevice_ == active_ && rumbleScale_ == appliedRumbleScale_ && a.smallMotor == applied_.smallMotor && a.largeMotor == applied_.largeMotor) return;
    devices_[size_t(active_)]->SetMotors(a.smallMotor, a.largeMotor, rumbleScale_);
    appliedRumbleScale_ = rumbleScale_;
    applied_ = a;
    appliedDevice_ = active_;
}

std::string InputSystem::Port2Name() const { return port2Device_ >= 0 && size_t(port2Device_) < devices_.size() ? devices_[size_t(port2Device_)]->Name() : std::string("none"); }

std::string InputSystem::Port1Name() const { return active_ >= 0 ? devices_[size_t(active_)]->Name() : std::string(); }
bool InputSystem::Port1HasMotors() const { return active_ >= 0 && devices_[size_t(active_)]->HasMotors(); }
bool InputSystem::Port1HasTriggers() const { return active_ >= 0 && devices_[size_t(active_)]->HasTriggers(); }
void InputSystem::SetPedalResistance(uint8_t a, uint8_t b) {
    if (active_ >= 0) devices_[size_t(active_)]->SetTriggers(focused_ ? a : 0, focused_ ? b : 0);
}
void InputSystem::StopFeedback() {
    actuators_ = {}; applied_ = {}; actuatorsSet_ = false;
    for (auto& d : devices_) { d->SetMotors(0, 0); d->SetTriggers(0, 0); }
}

} // namespace gt2::input
