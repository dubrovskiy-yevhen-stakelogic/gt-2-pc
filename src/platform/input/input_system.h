#pragma once
// PC controllers -> the virtual PS1 controller of port 1 (ps1_pad.h Ps1PadFrame), for gt2game (Windows only).
//
// Devices (hot-plugged; the most recently used one drives port 1):
//   - XInput pads (Xbox 360 / One / Series and compatibles), slots 0..3: an analog controller (type 7, id 0x73) with the
//     Xbox layout mapped onto the PS1 one (A = Cross, B = Circle, X = Square, Y = Triangle, LB / RB = L1 / R1,
//     LT / RT = L2 / R2 (pressed above the XInput threshold 30) plus their travel as our pressure axes, Back = Select,
//     Start = Start, stick clicks = L3 / R3, D-pad), sticks as the PS1 bytes (0x80 centre, 0 = left / up); both motors.
//   - DirectInput game controllers that are not XInput devices: Sony pads (VID 054C: DualShock 4 / DualSense in their
//     DirectInput layout) and other gamepads as type 7 (generic layout: buttons 1..4 = Cross, Circle, Square, Triangle);
//     wheels (DI8DEVTYPE_DRIVING) as a neGcon (type 2: twist = the wheel, I = accelerator, II = brake, L = clutch), the
//     original's analogue-wheel path with its calibration page. No motors (DirectInput force feedback is not used).
//   - a scripted fake pad (--fake-pad): any type, analogue values per field, motor changes printed.
// The keyboard is not part of this: gt2game merges it (game_window.h, race_view.cpp).


#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "platform/input/ps1_pad.h"

namespace gt2::input {

class Device {
public:
    virtual ~Device() = default;
    // Reads the device; false once it is gone.
    virtual bool Poll(int field, Ps1PadFrame& out) = 0;
    virtual void SetMotors(uint8_t smallMotor, uint8_t largeMotor, int strength = 100) = 0; // DualShock actuator bytes (small: bit 0 on / off)
    virtual bool HasMotors() const = 0;
    virtual void SetTriggers(uint8_t, uint8_t) {}
    virtual bool HasTriggers() const { return false; }
    virtual std::string Name() const = 0;
    virtual bool Scripted() const { return false; }
};

class InputSystem {
public:
    InputSystem();
    ~InputSystem();
    InputSystem(const InputSystem&) = delete;
    InputSystem& operator=(const InputSystem&) = delete;

    // DirectInput needs the window for its cooperative level (optional: without it only XInput / fake pads).
    void AttachWindow(void* window);
    // WM_DEVICECHANGE: look for new devices at the next poll.
    void DevicesChanged() { rescan_ = true; }
    // Once per presentation field. `focused`: real devices count only while the game window is in the foreground
    // (scripted fake pads always).
    void Poll(int field, bool focused);

    // Port 1 of this field (type kTypeNone without a device).
    const Ps1PadFrame& Port1() const { return port1_; }
    std::string Port1Name() const;
    // Port 2 of this field (the 2 player Battle's second controller): a --fake-pad2 script, else the most recently used device
    // other than port 1's (type kTypeNone without one). No motors.
    const Ps1PadFrame& Port2() const { return port2_; }
    std::string Port2Name() const;
    bool Port1HasMotors() const;
    bool Port1HasTriggers() const;
    void SetPedalResistance(uint8_t accelerator, uint8_t brake);
    void StopFeedback();
    // The actuator bytes of this field (ps1_pad.h PollPad); motors fall silent when a field passes without a call
    // (as the original's vibration timer runs out when the race stops feeding it).
    void SetActuators(const Actuators& a);
    // Our strength setting (0..100 %) applied to both motors.
    void SetRumbleScale(int percent) { rumbleScale_ = percent < 0 ? 0 : percent > 100 ? 100 : percent; }

    // --fake-pad: "field:control=value[:hold],..." or a file of such items (docs/formats/pad_input.md).
    void AddFakePad(const std::string& scriptOrFile, int port = 1); // port 2: --fake-pad2 (the second player)
    // Connect / disconnect / motor lines since the last call.
    std::vector<std::string> TakeLog();

private:
    void Rescan(int field);
    void ApplyMotors();

    void* hwnd_ = nullptr;
    struct DirectInputState;
    std::unique_ptr<DirectInputState> dinput_;
    std::vector<std::unique_ptr<Device>> devices_;
    std::vector<Ps1PadFrame> frames_;
    std::vector<int> lastActivity_;    // -1 new, -2 no activity yet, else the field of the last activity
    std::vector<Ps1PadFrame> rest_;    // each device's first frame
    int active_ = -1;
    Ps1PadFrame port1_, port2_;
    int port2Device_ = -1;
    Actuators actuators_, applied_;
    bool actuatorsSet_ = false;
    [[maybe_unused]] int appliedDevice_ = -1;
    int rumbleScale_ = 100;
    [[maybe_unused]] int appliedRumbleScale_ = -1;
    bool rescan_ = true;
    [[maybe_unused]] bool focused_ = false;
    int lastScan_ = -1000;
    std::vector<std::string> log_;
};

} // namespace gt2::input
