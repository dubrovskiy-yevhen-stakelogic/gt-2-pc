#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <chrono>
#include "platform/input/steering_feedback.h"
#include "gt2formats/replay.h"

namespace gt2::input::wheel {
constexpr int kAxes = 8, kButtons = 144;
enum Axis { Steering, Throttle, Brake, Clutch, AxisCount };
enum Button { ShiftUp, ShiftDown, Reverse, Handbrake, Camera, LookBack, Menu,
              Gear1, Gear2, Gear3, Gear4, Gear5, Gear6, Gear7, ButtonCount };
inline constexpr const char* axisLabels[] = {"Steering", "Accelerator", "Brake", "Clutch"};
inline constexpr const char* buttonLabels[] = {"Shift up", "Shift down", "Reverse", "Handbrake", "Change view", "Look back", "Open settings",
    "Gear 1", "Gear 2", "Gear 3", "Gear 4", "Gear 5", "Gear 6", "Gear 7"};
inline constexpr const char* rawAxisLabels[] = {"X", "Y", "Z", "RX", "RY", "RZ", "Slider 1", "Slider 2"};
struct AxisBinding {
    std::string device;
    int axis = -1;
    int rest = 0, end = -32768, right = 32767;
    int deadzone = 2, saturation = 100, curve = 100;
    bool Valid(bool steering) const;
    float Map(int raw, bool steering) const;
};
struct ButtonBinding { std::string device; int button = -1; };
struct Settings {
    bool automatic = true, useClutch = false, rimButtons = false;
    bool autoFeedback = true, autoGearbox = true;
    std::string profile, signature, baseChoice, pedalChoice, shifterChoice;
    std::array<ButtonBinding, 16> navigation{};
    bool enabled = false, feedback = false, invertForce = false;
    int gearbox = 0; // 0 automatic, 1 sequential, 2 H-pattern
    int gain = 25, damping = 10;
    int tractionControl = 0, countersteer = 1; // 0..5; 0 off, 1 weak, 2 strong
    bool ignoreShiftSpeed = false;
    SteeringGeometry steeringGeometry;
    std::array<AxisBinding, AxisCount> axes{};
    std::array<ButtonBinding, ButtonCount> buttons{};
    std::string Serialize() const;
    static Settings Parse(const std::string& text);
};
Settings& Config();
void LoadSettings(const std::string& path);
void SaveSettings();
struct DeviceState {
    std::string id, name;
    uint16_t vendor = 0, product = 0;
    int hidCollection = 0;
    int buttonCount = 128, hatCount = 4;
    bool online = false, forceCapable = false;
    std::array<bool, kAxes> available{};
    std::array<int, kAxes> axes{};
    std::array<bool, kButtons> buttons{};
};
struct State {
    bool active = false, ready = false;
    bool shifterDisconnected = false;
    float steering = 0, throttle = 0, brake = 0, clutch = 0;
    std::array<bool, ButtonCount> held{};
};
State Resolve(const Settings& settings, const std::vector<DeviceState>& devices, bool focused);
// Ready rigs own driving input. Unavailable rigs preserve the keyboard/gamepad
// frame; passing an old wheel frame instead clears its pedals and gear request.
void Apply(const Settings& settings, const State& state, LogicalPad& pad);
// Apply the race's AT/MT choice before recording the live wheel frame. Replays
// keep their recorded gear requests and never pass through this policy.
void ApplyRaceTransmission(LogicalPad& pad, bool manual, int currentGear);
float Force(const SteeringMoments& moments, float steeringVelocity, const Settings& settings);

struct ProfileAxis { int target, axis, rest, end, right; };
struct Profile {
    uint16_t vendor, product;
    const char* name;
    bool rimSpecific;
    std::vector<ProfileAxis> axes;
    std::vector<std::pair<int, int>> buttons, navigation;
    bool HasAxis(int axis) const;
    bool HasButton(int button) const;
};
const std::vector<Profile>& Profiles();
const Profile* MatchProfile(const DeviceState& device);
bool IsRacingDevice(const DeviceState& device);
enum class DeviceRole { Any, Wheel, Pedals, Shifter };
bool FitsDeviceRole(const DeviceState& device, DeviceRole role);
std::string DeviceName(const DeviceState& device);
bool AutoConfigure(Settings& settings, const std::vector<DeviceState>& devices, std::string& status);
uint16_t Navigation(const Settings& settings, const std::vector<DeviceState>& devices, bool focused);
void ResetAutomatic(Settings& settings);
class ShifterSetup {
public:
    void Start(const DeviceState& device, int gears);
    void Sample(const DeviceState* device);
    bool Complete() const { return step_ > gears_; }
    bool WaitingForNeutral() const { return release_; }
    int Gear() const { return step_ < gears_ ? step_ + 1 : 0; }
    const std::string& Error() const { return error_; }
    bool ApplyTo(Settings& settings) const;
private:
    std::string device_, error_;
    std::array<bool, kButtons> neutral_{};
    std::array<int, 8> learned_{};
    int gears_ = 6, step_ = 0, candidate_ = -1, stable_ = 0;
    bool release_ = false;
};

// The hardware handles live with the game window. Settings refer to persistent
// instance IDs, so enumerating a USB pedal set never changes the steering source.
class Rig {
public:
    Rig();
    ~Rig();
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;
    void Attach(void* window);
    void Poll(bool focused);
    void Rescan();
    void Stop();
    void Feedback(const FeedbackFrame& frame, bool driving);
    const SteeringMoments& Moments() const { return moments_; }
    float OutputForce() const { return filteredForce_; }
    bool Claims(const std::string& id) const;
    const std::vector<DeviceState>& Devices() const { return devices_; }
    const State& Current() const { return state_; }
    bool Pressed(Button b) const { return pressed_[b]; }
    std::string Status() const;
    uint16_t MenuButtons() const { return navigation_; }
    void CaptureDevice(const std::string& id) { captureDevice_ = id; }
private:
    struct Hardware;
    std::unique_ptr<Hardware> hardware_;
    std::vector<DeviceState> devices_;
    State state_;
    std::array<bool, ButtonCount> pressed_{};
    float lastSteering_ = 0, filteredForce_ = 0;
    SteeringMoments moments_;
    std::chrono::steady_clock::time_point feedbackTime_{};
    uint16_t navigation_ = 0;
    std::string autoStatus_, saveError_;
    std::string captureDevice_;
};
std::string InstanceId(const void* guid);
}
