#include "platform/input/wheel.h"
#include "game/sim/drivetrain.h"
#include "game/sim/drive_shafts.h"
#include "../tools/gt2game/wheel_shift_hint.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace w = gt2::input::wheel;
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
bool Near(float a, float b) { return std::abs(a - b) < .002f; }
#include "steering_feedback_checks.h"
#include "wheel_driving_aids_checks.h"
int main() try {
    {
        w::DeviceState base, pedals, shifter;
        base.id = "dd-pro"; base.name = "FANATEC Wheel"; base.vendor = 0x0eb7; base.product = 0x0020;
        pedals.id = "usb-pedals"; pedals.vendor = 0x0eb7; pedals.product = 0x6205;
        shifter.id = "usb-shifter"; shifter.vendor = 0x16c0; shifter.product = 0x05e1;
        for (auto* d : {&base, &pedals, &shifter}) { d->online = true; d->available.fill(true); }
        base.forceCapable = true; base.axes[1] = base.axes[5] = 32767;
        base.axes[6] = -32768; // A phantom clutch axis must not disengage a two-pedal rig.
        pedals.axes[0] = pedals.axes[1] = pedals.axes[2] = -32768;
        w::Settings automatic;
        std::string status;
        Check(w::AutoConfigure(automatic, {base}, status), "first known wheel configures automatically");
        Check(automatic.enabled && automatic.feedback && automatic.gearbox == 1, "automatic wheel / paddles / FFB defaults");
        Check(automatic.axes[w::Throttle].axis == 1 && automatic.axes[w::Brake].axis == 5 && automatic.axes[w::Clutch].device.empty(), "actual base mapping and optional clutch");
        auto state = w::Resolve(automatic, {base}, true);
        Check(state.ready && state.throttle == 0 && state.brake == 0 && state.clutch == 0, "two-pedal base is ready without phantom clutch");
        base.buttons[1] = base.buttons[128] = true;
        Check((w::Navigation(automatic, {base}, true) & 0x4010) == 0x4010, "wheel confirm and POV reach menus");
        Check(w::Navigation(automatic, {base}, false) == 0, "menu buttons clear on focus loss");
        base.buttons.fill(false);
        for (const auto shift : {w::ShiftUp, w::ShiftDown}) {
            base.buttons[automatic.buttons[shift].button] = true;
            const auto restored = w::Settings::Parse(automatic.Serialize());
            Check(w::Navigation(restored, {base}, true) == 0,
                  "saved paddle navigation bindings must not leak into controller camera/look-back actions");
            const auto shifted = w::Resolve(restored, {base}, true);
            gt2::LogicalPad paddlePad;
            w::Apply(restored, shifted, paddlePad);
            Check(paddlePad.wheelGear == 15 && paddlePad.buttons == (shift == w::ShiftUp ? gt2::kPadShiftUp : gt2::kPadShiftDown),
                  "each paddle reaches the sequential gearbox exclusively");
            Check(!shifted.held[w::Camera] && !shifted.held[w::LookBack], "paddles do not request wheel camera controls");
            base.buttons.fill(false);
        }
        base.buttons[automatic.buttons[w::Camera].button] = true;
        Check(w::Resolve(automatic, {base}, true).held[w::Camera], "dedicated wheel camera button remains available");
        base.buttons.fill(false);
        base.buttons[1] = base.buttons[128] = true;
        Check((w::Navigation(automatic, {base}, true) & 0x4010) == 0x4010, "confirm and D-pad navigation remain available");
        auto saved = automatic.Serialize();
        const auto baseButtons = automatic.buttons;
        Check(!w::AutoConfigure(automatic, {base}, status) && saved == automatic.Serialize(), "poll does not rewrite profile or user preferences");
        automatic.gain = 15; automatic.gearbox = 0; automatic.autoGearbox = false;
        Check(w::AutoConfigure(automatic, {shifter, base, pedals}, status), "compose wheel, USB pedals and shifter by role");
        Check(automatic.axes[w::Throttle].device == pedals.id && automatic.axes[w::Throttle].axis == 0 && automatic.axes[w::Throttle].rest == -32768,
              "USB Fanatec pedals use documented independent mapping");
        Check(automatic.buttons[w::Gear1].device == shifter.id && automatic.buttons[w::Gear1].button == 0, "external H shifter replaces base gear buttons");
        Check(automatic.gain == 15 && automatic.gearbox == 0, "hotplug preserves FFB strength and chosen gearbox");
        saved = automatic.Serialize();
        Check(!w::AutoConfigure(automatic, {base, shifter}, status) && saved == automatic.Serialize(), "pedal disconnect never falls back to base axes");
        Check(!w::Resolve(automatic, {base, shifter}, true).ready, "disconnected USB pedals stop driving");
        automatic.gearbox = 2;
        // The user's saved rig keeps paddles on the base and H gates on USB.
        automatic.buttons[w::ShiftUp] = baseButtons[w::ShiftUp];
        automatic.buttons[w::ShiftDown] = baseButtons[w::ShiftDown];
        saved = automatic.Serialize();
        Check(!w::AutoConfigure(automatic, {base, pedals}, status) && saved == automatic.Serialize(), "shifter disconnect preserves saved assignments");
        base.axes[0] = 16384; pedals.axes[0] = 32767;
        for (const auto shift : {w::ShiftUp, w::ShiftDown}) {
            base.buttons.fill(false); base.buttons[automatic.buttons[shift].button] = true;
            auto offlineShifter = shifter; offlineShifter.online = false; offlineShifter.buttons.fill(true);
            for (const auto& connected : {std::vector<w::DeviceState>{base, pedals}, std::vector<w::DeviceState>{offlineShifter, pedals, base}}) {
                const auto restored = w::Settings::Parse(automatic.Serialize());
                const auto fallback = w::Resolve(restored, connected, true);
                gt2::LogicalPad fallbackPad;
                w::Apply(restored, fallback, fallbackPad);
                Check(fallback.ready && Near(fallback.steering, .5f) && fallback.throttle == 1 && fallback.brake == 0,
                      "absent or offline shifter must preserve steering and pedals after restart");
                Check(fallbackPad.wheelGear == 15 && fallbackPad.clutch == 0 && fallbackPad.throttle == 1023 &&
                      fallbackPad.buttons == (shift == w::ShiftUp ? gt2::kPadShiftUp : gt2::kPadShiftDown),
                      "disconnected H shifter uses paddles without stale reverse or clutch input");
                Check(w::Navigation(restored, connected, true) == 0, "fallback paddles must not change the camera");
                std::array<uint16_t, 16> steeringTable{};
                const auto record = gt2::PadOfFrame(gt2::FrameOfPad(fallbackPad), steeringTable);
                gt2::sim::CarBody car{}; car.forwardGears = 6; car.gear = 2;
                gt2::sim::GearRequest request{};
                gt2::sim::ApplyWheelInput(car, record, request);
                Check(car.throttle == 4096 && gt2::sim::SelectGear(car, request) == (shift == w::ShiftUp ? 3 : 1),
                      "fallback paddle and throttle reach the native drivetrain");
            }
        }
        base.buttons.fill(false);
        shifter.buttons[automatic.buttons[w::Gear3].button] = true;
        gt2::LogicalPad reconnectedPad;
        w::Apply(automatic, w::Resolve(automatic, {base, pedals, shifter}, true), reconnectedPad);
        Check(reconnectedPad.wheelGear == 5 && automatic.Serialize() == saved, "same shifter reconnect restores H mode without rewriting calibration");
        shifter.buttons.fill(false);
        automatic.useClutch = true;
        Check(w::AutoConfigure(automatic, {base, pedals, shifter}, status) && automatic.axes[w::Clutch].axis == 2, "clutch opts into the selected USB pedal profile");
        auto parsed = w::Settings::Parse(automatic.Serialize());
        Check(parsed.Serialize() == automatic.Serialize(), "automatic settings / navigation round trip");
        Check(!w::Settings::Parse("version 1\noptions 1 0 0 25 10 0\n").automatic, "existing manual setup is not overwritten by migration");
        auto unknown = base; unknown.vendor = 0x1234;
        Check(!w::MatchProfile(unknown), "matching never guesses from a manufacturer name");
        unknown.vendor = 0x0eb7; unknown.product = 0x1a92;
        Check(!w::MatchProfile(unknown), "quarantined Fanatec shifter is not activated");
        Check(w::IsRacingDevice(unknown), "unmapped shifter is not a second player's gamepad");
        w::Settings optedOut;
        optedOut.autoFeedback = false; optedOut.feedback = false;
        optedOut.autoGearbox = false; optedOut.gearbox = 0;
        Check(w::AutoConfigure(optedOut, {base}, status) && !optedOut.feedback && optedOut.gearbox == 0,
              "explicit force-off and automatic gearbox choices survive first wheel connection");
        w::Settings multiple;
        auto primary = base, auxiliary = base;
        primary.hidCollection = 1; auxiliary.hidCollection = 2; auxiliary.id = "fanatec-extended";
        auxiliary.buttonCount = 63; auxiliary.hatCount = 4;
        w::Settings composite;
        Check(w::AutoConfigure(composite, {auxiliary, primary}, status) && composite.axes[w::Steering].device == primary.id,
              "one Fanatec base with two HID collections auto-selects the primary interface");
        Check(w::FitsDeviceRole(primary, w::DeviceRole::Wheel) && !w::FitsDeviceRole(auxiliary, w::DeviceRole::Wheel),
              "extended Fanatec layout is not a second wheel");
        Check(!w::FitsDeviceRole(primary, w::DeviceRole::Pedals) && !w::FitsDeviceRole(primary, w::DeviceRole::Shifter),
              "wheel base is not listed as a standalone USB pedal set or shifter");
        Check(w::FitsDeviceRole(pedals, w::DeviceRole::Pedals) && w::FitsDeviceRole(shifter, w::DeviceRole::Shifter),
              "independent accessories remain selectable");
        Check(w::DeviceName(primary) != primary.name && w::DeviceName(primary).find("GT DD Pro") != std::string::npos,
              "model label replaces generic Fanatec driver name");
        Check(w::IsRacingDevice(auxiliary), "auxiliary collection is still reserved from ordinary gamepad input");
        w::Settings absentPrimary;
        Check(!w::AutoConfigure(absentPrimary, {auxiliary}, status), "extended layout cannot silently replace a missing primary interface");
        auto secondPhysicalBase = primary; secondPhysicalBase.id = "second-physical-base";
        Check(!w::AutoConfigure(absentPrimary, {primary, auxiliary, secondPhysicalBase}, status),
              "two real bases with the same product ID still require a choice");
        auto otherBrand = primary; otherBrand.vendor = 0x346e; otherBrand.product = 2; otherBrand.hidCollection = 2;
        Check(w::FitsDeviceRole(otherBrand, w::DeviceRole::Wheel), "Fanatec collection rule does not hide other manufacturers");
        auto other = base; other.id = "second-base";
        Check(!w::AutoConfigure(multiple, {base, other}, status) && !multiple.enabled, "ambiguous bases require explicit choice");
        multiple.baseChoice = other.id;
        Check(w::AutoConfigure(multiple, {base, other}, status) && multiple.axes[w::Steering].device == other.id, "explicit base choice resolves ambiguity");
        w::Settings twoPedals;
        other = pedals; other.id = "second-pedals";
        Check(!w::AutoConfigure(twoPedals, {base, pedals, other}, status), "ambiguous USB pedals are not selected arbitrarily");
        twoPedals.pedalChoice = "base";
        Check(w::AutoConfigure(twoPedals, {base, pedals, other}, status) && twoPedals.axes[w::Throttle].device == base.id, "player can prefer base-connected pedals");
        w::Settings rim;
        other = base; other.vendor = 0x346e; other.product = 2;
        Check(w::AutoConfigure(rim, {other}, status) && rim.navigation[14].button == -1, "rim-specific buttons require selecting the documented preset");
        rim.rimButtons = true;
        Check(w::AutoConfigure(rim, {other}, status) && rim.navigation[14].button == 19, "explicit rim preset uses sourced buttons");
        for (const auto& profile : w::Profiles()) {
            for (const auto& a : profile.axes) Check(a.target >= 0 && a.target < w::AxisCount && a.axis >= 0 && a.axis < 8, "catalogue axis bounds");
            for (const auto& b : profile.buttons) Check(b.first >= 0 && b.first < w::ButtonCount && b.second >= 0 && b.second < w::kButtons, "catalogue button bounds");
        }
        w::ShifterSetup guide;
        shifter.buttons.fill(false); guide.Start(shifter, 6);
        auto untouched = automatic.Serialize();
        Check(!guide.ApplyTo(automatic) && automatic.Serialize() == untouched, "incomplete shifter wizard cannot change settings");
        for (int gear = 0; gear < 7; ++gear) {
            shifter.buttons[gear] = true;
            guide.Sample(&shifter); guide.Sample(&shifter); guide.Sample(&shifter);
            Check(guide.WaitingForNeutral(), "wizard confirms a stable gate");
            guide.Sample(&shifter); Check(!guide.Complete(), "held gate cannot skip the neutral step");
            shifter.buttons[gear] = false; guide.Sample(&shifter);
        }
        Check(guide.Complete() && guide.ApplyTo(automatic), "six gears and reverse finish the shifter wizard");
        Check(automatic.buttons[w::Gear6].button == 5 && automatic.buttons[w::Reverse].button == 6 && !automatic.automatic, "wizard commits real gate mapping without guesses");
        guide.Start(shifter, 6); shifter.buttons[0] = shifter.buttons[1] = true;
        for (int i = 0; i < 8; ++i) guide.Sample(&shifter);
        Check(!guide.WaitingForNeutral() && !guide.Error().empty(), "multi-button gate is rejected instead of guessed");
        guide.Sample(nullptr); Check(!guide.Complete(), "disconnection cannot complete a wizard");
    }
    w::Settings s;
    s.enabled = true;
    s.axes[w::Steering] = {"base", 0, 0, -32768, 32767, 0, 100, 100};
    s.axes[w::Throttle] = {"pedals", 1, 32767, -32768, 0, 0, 100, 100};
    s.axes[w::Brake] = {"pedals", 2, -32768, 32767, 0, 0, 100, 100};
    auto a = s.axes[0];
    Check(a.Valid(true) && Near(a.Map(-32768, true), -1) && Near(a.Map(32767, true), 1) && Near(a.Map(0, true), 0), "steering endpoints / centre");
    Check(Near(a.Map(16384, true), .5f), "steering must be linear");
    std::swap(a.end, a.right);
    Check(Near(a.Map(-32768, true), 1), "inverted steering");
    a.end = a.rest;
    Check(!a.Valid(true) && a.Map(123, true) == 0, "degenerate calibration");
    a = s.axes[1];
    Check(Near(a.Map(32767, false), 0) && Near(a.Map(-32768, false), 1) && Near(a.Map(0, false), .5f), "inverted pedal range");
    a.deadzone = 10; a.saturation = 80;
    Check(a.Map(30000, false) == 0 && a.Map(-30000, false) == 1, "dead zone and saturation");
    a = s.axes[2]; a.curve = 200;
    Check(Near(a.Map(0, false), .25f), "pedal response curve");
    // Two pedals can share an axis: calibrated centre to opposite endpoints.
    a = {"combined", 1, 0, 32767, 0, 0, 100, 100};
    auto combinedBrake = a; combinedBrake.end = -32768;
    Check(a.Map(-32768, false) == 0 && combinedBrake.Map(32767, false) == 0, "combined pedals do not overlap");

    w::DeviceState base, pedals, shifter;
    base.id = "base"; pedals.id = "pedals"; shifter.id = "shifter";
    for (auto* d : {&base, &pedals, &shifter}) { d->online = true; d->available.fill(true); }
    base.axes[0] = 16384; pedals.axes[1] = -32768; pedals.axes[2] = -32768;
    s.buttons[w::Gear3] = {"shifter", 2}; shifter.buttons[2] = true;
    s.buttons[w::Camera] = {"base", 6}; base.buttons[6] = true;
    s.gearbox = 2;
    std::vector<w::DeviceState> devices{shifter, pedals, base};
    auto state = w::Resolve(s, devices, true);
    Check(state.ready && Near(state.steering, .5f) && state.throttle == 1 && state.brake == 0 && state.held[w::Gear3], "merge independent USB devices");
    gt2::LogicalPad logical;
    w::Apply(s, state, logical);
    Check(logical.wheel && logical.wheelGear == 5 && logical.throttle == 1023, "direct H gear request");
    devices.erase(devices.begin());
    state = w::Resolve(s, devices, true);
    w::Apply(s, state, logical);
    Check(state.ready && logical.wheelGear == 15 && logical.clutch == 0 && logical.throttle == 1023 && Near(state.steering, .5f),
          "custom H bindings also preserve driving on shifter disconnect");
    Check(!w::Resolve(s, {pedals}, true).ready, "missing wheel still stops driving");
    devices = {base, pedals, shifter};
    state = w::Resolve(s, devices, false);
    Check(!state.ready && state.throttle == 0 && !state.held[w::Camera], "focus loss clears held controls");
    s.enabled = false;
    state = w::Resolve(s, devices, true); logical = {}; logical.throttle = 98;
    w::Apply(s, state, logical);
    Check(!logical.wheel && logical.throttle == 98, "disabled wheel preserves normal pad");
    s.enabled = true;
    s.axes[w::Clutch] = {"clutch", 0, 32767, -32768, 0, 0, 100, 100};
    Check(!w::Resolve(s, devices, true).ready, "missing assigned clutch is not a released clutch");
    s.axes[w::Clutch] = {};
    s.buttons[w::Gear2] = {"base", 6};
    state = w::Resolve(s, devices, true); w::Apply(s, state, logical);
    Check(logical.wheelGear == 1, "conflicting H gears select neutral");
    Check(w::Settings::Parse(s.Serialize()).Serialize() == s.Serialize(), "settings round trip");
    const auto corrupt = w::Settings::Parse("options 1 99 1 10000 -1 1\naxis 9000 \"x\" 3 0 1 2 3 4 5\nbutton 0 \"x\" 999\naxis 0 \"x\" 90 999999 -999999 100000 999 0 -3\n");
    Check(corrupt.gearbox == 2 && corrupt.gain == 100 && corrupt.damping == 0 && corrupt.buttons[0].button == -1 && corrupt.axes[0].rest == 32767 && corrupt.axes[0].deadzone == 25, "malformed settings bounded");

    std::array<uint16_t, 16> table{}; for (int i = 0; i < 16; ++i) table[i] = uint16_t(i * 4096 / 15);
    {
        w::Settings settings;
        settings.ignoreShiftSpeed = true;
        Check(w::Settings::Parse(settings.Serialize()).ignoreShiftSpeed, "speed override persists");
        Check(!w::Settings::Parse("ignore_shift_speed 99\n").ignoreShiftSpeed, "invalid override remains off");
        w::State shiftState{}; shiftState.active = shiftState.ready = true;
        gt2::LogicalPad mapped{}; w::Apply(settings, shiftState, mapped);
        Check(mapped.ignoreShiftSpeed, "live wheel carries speed override");
        shiftState.ready = false; w::Apply(settings, shiftState, mapped);
        Check(!mapped.ignoreShiftSpeed && mapped.wheelGear == 1, "disconnect still forces neutral");
        for (bool unrestricted : {false, true}) for (int clutch : {0, 127, 255})
        for (int mode : {2, 3, 4, 5, 6, 7, 8, 9, 15}) {
            gt2::LogicalPad live{}; live.wheel = true; live.wheelGear = uint8_t(mode);
            live.ignoreShiftSpeed = unrestricted; live.clutch = uint8_t(clutch);
            live.steerAxis = 987; live.throttle = 811; live.brake = 257;
            live.buttons = gt2::kPadHandbrake | gt2::kPadShiftUp;
            const auto frame = gt2::FrameOfPad(live);
            gt2::ReplayStream recording;
            recording.Record(frame); recording.Record(frame); recording.End(false);
            auto playback = gt2::ReplayStream::FromBytes(recording.Bytes());
            playback.Init(true, playback.Capacity());
            gt2::ReplayFrame restored; playback.Read(restored);
            Check(restored == frame, "speed override survives compressed replay stream");
            const auto pad = gt2::PadOfFrame(restored, table);
            Check((pad.reserved & 15) == mode && bool(pad.flags & gt2::sim::kWheelIgnoreShiftSpeed) == unrestricted,
                  "replay restores gear and speed policy independently of current settings");
            Check(((pad.reserved & 0xf0) | ((pad.flags >> 4) & 15)) == clutch && pad.throttle == 811 * 4096 / 1023 &&
                  pad.brake == 257 * 4096 / 1023 && pad.handbrake == 1, "override preserves clutch and pedal precision");
            gt2::sim::CarBody car{}; car.forwardGears = 6; car.gear = mode == 2 ? 1 : 0;
            car.forwardSpeed = mode == 2 ? 20 * 4096 : -20 * 4096;
            car.clutchState = 1;
            gt2::sim::GearRequest request{};
            gt2::sim::ApplyWheelInput(car, pad, request);
            const int target = mode == 15 ? 1 : mode - 2;
            const int expected = unrestricted && target <= car.forwardGears ? target : -1;
            Check(gt2::sim::SelectGear(car, request) == expected, "speed override changes only the direction interlock");
            Check(gt2::sim::WheelDirectionBlocked(car, request) == (!unrestricted && target <= car.forwardGears),
                  "unrestricted shifts do not show a stop warning");
            if (mode != 15) Check(gt2::sim::WheelNeutral(car, request) == (!unrestricted || target > car.forwardGears),
                                 "override does not leave a valid direct gear in neutral");
            if (clutch == 255) Check(car.clutchState == 0, "speed override preserves a fully open clutch");
        }
    }
    gt2::ReplayStream stream(60000);
    std::vector<gt2::ReplayFrame> recorded;
    for (int gear : {0, 1, 2, 3, 9, 15}) for (int clutch = 0; clutch < 256; ++clutch) {
        logical = {}; logical.wheel = true; logical.analog = 13;
        logical.wheelGear = uint8_t(gear); logical.clutch = uint8_t(clutch); logical.steerAxis = uint16_t(2048 + clutch); logical.throttle = 1023; logical.brake = uint16_t(500 + clutch);
        logical.buttons = gt2::kPadHandbrake | gt2::kPadShiftDown;
        const auto frame = gt2::FrameOfPad(logical);
        const auto pad = gt2::PadOfFrame(frame, table);
        Check(pad.flags == (0x107 | ((clutch & 15) << 4)) && pad.reserved == ((clutch & 0xf0) | gear) && pad.reverse == 0 && pad.shift == -1 && pad.handbrake == 1 && pad.throttle == 4096 && pad.brake == (500 + clutch) * 4096 / 1023, "wheel frame decode");
        stream.Record(frame); stream = gt2::ReplayStream::FromBytes(stream.Bytes());
        stream.Record(frame); recorded.push_back(frame); recorded.push_back(frame);
    }
    for (int i = 0; i < 100; ++i) {
        logical = {}; logical.wheel = i % 7 != 0; logical.wheelGear = 3;
        logical.steerAxis = uint16_t(2048 + (i & 15)); logical.throttle = uint16_t(512 + (i & 63)); logical.brake = uint16_t(512 + (i & 31));
        const auto frame = gt2::FrameOfPad(logical);
        stream.Record(frame); recorded.push_back(frame);
        stream = gt2::ReplayStream::FromBytes(stream.Bytes());
        if (logical.wheel) {
            const auto expanded = gt2::PadOfFrame(frame, table);
            Check(expanded.throttle == logical.throttle * 4096 / 1023 && expanded.brake == logical.brake * 4096 / 1023,
                  "fine pedal changes survive without a coarse byte change");
        }
    }
    stream.Record({}); // The original reader consumes a final end tick without returning it.
    stream.End(false);
    auto playback = gt2::ReplayStream::FromBytes(stream.Bytes()); playback.Init(true, playback.Capacity());
    for (const auto& expected : recorded) {
        gt2::ReplayFrame frame;
        playback.Read(frame);
        Check(!playback.Ended() && frame == expected, "wheel replay compressed stream round trip");
        playback = gt2::ReplayStream::FromBytes(playback.Bytes());
    }
    logical = {}; logical.buttons = gt2::kPadThrottle | gt2::kPadStickCurve; logical.analog = 13; logical.throttle = 255; logical.brake = 32;
    const auto normal = gt2::FrameOfPad(logical);
    Check(normal.flags == 15 && normal.buttons == 4 && normal.steer == 128 && normal.throttle == 15 && normal.brake == 2, "original replay bytes unchanged");

    gt2::sim::CarBody body{}; body.forwardGears = 6; body.gear = 1; body.steerLock = 400;
    gt2::sim::PadRecord pad{}; pad.flags = gt2::sim::kWheelPad | 7; pad.steer = 2048; pad.throttle = 1000; pad.brake = 2000; pad.reserved = 5;
    gt2::sim::GearRequest request{};
    gt2::sim::ApplyWheelInput(body, pad, request);
    Check(body.steerAngle == 200 && body.throttle == 1000 && body.brake == 2000, "wheel bypasses stick curve / pedal boost");
    Check(gt2::sim::SelectGear(body, request) == 3, "recorded H pattern retains its original gear request");
    pad.reserved = 1; body.clutchState = 1; body.clutchRequest = 4096;
    gt2::sim::ApplyWheelInput(body, pad, request);
    Check(body.clutchState == 0 && body.clutchRequest == 0 && gt2::sim::SelectGear(body, request) == -1, "neutral disconnects engine without invalid gear index");
    pad.reserved = 9; body.clutchState = 1;
    gt2::sim::ApplyWheelInput(body, pad, request);
    Check(body.clutchState == 0 && gt2::sim::SelectGear(body, request) == -1, "seventh gear on six speed car is neutral");
    pad.reserved = 2; body.forwardSpeed = 4096 * 10; body.clutchState = 1;
    gt2::sim::ApplyWheelInput(body, pad, request);
    Check(body.clutchState == 0 && gt2::sim::SelectGear(body, request) == -1, "reverse blocked while moving forward");
    body.forwardSpeed = 0; gt2::sim::ApplyWheelInput(body, pad, request);
    Check(gt2::sim::SelectGear(body, request) == 0 && body.throttle == 1000, "reverse retains analog throttle");
    pad.reserved = 0xf3; pad.flags = 0x1f7; body.clutchState = 1;
    gt2::sim::ApplyWheelInput(body, pad, request);
    Check(body.clutchState == 0 && body.clutchRequest == 0, "clutch pedal fully opens clutch");
    pad.reserved = 0x73; pad.flags = 0x107; body.clutchState = 1;
    gt2::sim::ApplyWheelInput(body, pad, request);
    Check(body.clutchState == 2 && gt2::sim::WheelClutch(request) == 112, "partial clutch uses slipping state");
    pad.reserved = 15; pad.shift = 1; body.gear = 1; body.shiftTimer = 0;
    gt2::sim::ApplyWheelInput(body, pad, request); gt2::sim::UpdateGear(body, request);
    Check(body.gear == 2 && gt2::sim::SelectGear(body, request) == -1, "held sequential button does not repeat");
    request.shift = 0; gt2::sim::SelectGear(body, request); request.shift = 1;
    Check(gt2::sim::SelectGear(body, request) == 3, "sequential release re-arms shifting");
    for (int stoppedSpeed : {-2048, 0, 2048}) {
        gt2::sim::CarBody car{}; car.forwardGears = 6; car.gear = 1;
        gt2::sim::GearRequest shifts{};
        auto tick = [&](int shift, int speed) {
            gt2::sim::PadRecord wheelPad{};
            wheelPad.flags = gt2::sim::kWheelPad | 7; wheelPad.reserved = 15;
            wheelPad.shift = int8_t(shift); car.forwardSpeed = speed;
            gt2::sim::ApplyWheelInput(car, wheelPad, shifts);
            gt2::sim::UpdateGear(car, shifts);
        };
        tick(-1, 0);
        Check(car.gear == 0, "left paddle selects reverse at rest");
        tick(0, -4096); tick(1, -4096);
        Check(car.gear == 0, "reverse to first is blocked above reverse speed threshold");
        Check(gt2::sim::WheelDirectionBlocked(car, shifts), "HUD explains reverse-to-first speed interlock");
        tick(0, stoppedSpeed); tick(1, stoppedSpeed);
        Check(car.gear == 1, "brake, release and right paddle recover first from reverse");
        Check(!gt2::sim::WheelDirectionBlocked(car, shifts), "direction warning clears after slowing down");
        for (int n = 0; n < 10; ++n) tick(1, stoppedSpeed);
        Check(car.gear == 1, "holding up after leaving reverse cannot skip to higher gears");
        tick(0, stoppedSpeed); tick(1, stoppedSpeed);
        Check(car.gear == 2, "next separate paddle press selects second after reverse");
    }
    // Exercise the full axis/replay/physics mapping around centre and both ends.
    int previousSteer = 4097;
    for (int axis = 0; axis <= 4095; ++axis) {
        gt2::LogicalPad centered{}; centered.wheel = true; centered.steerAxis = uint16_t(axis);
        const auto decoded = gt2::PadOfFrame(gt2::FrameOfPad(centered), table);
        Check(decoded.steer <= previousSteer && decoded.steer >= -4096 && decoded.steer <= 4096,
              "steering is monotonic through every packed axis value without sign wrap");
        if (axis == 2048) Check(decoded.steer == 0, "a centered wheel stays exactly centered in physics input");
        previousSteer = decoded.steer;
    }
    pad.flags = 7; gt2::sim::ApplyWheelInput(body, pad, request);
    Check(request.reserved[0] == 0 && request.reserved[1] == 0, "normal pad clears wheel extension");

    auto transmitted = [](uint8_t depression) {
        gt2::sim::CarBody b{};
        b.forwardGears = 6; b.gear = 1; b.driveType = 0; b.carIndex = 0;
        b.clutchState = depression == 255 ? 0 : 3;
        b.stepTime = 2184; b.engineInvInertia = 4096; b.engineRpm = 4000; b.revLimitRpm = 8000;
        b.engineTorque = 400; b.lockedClutchTorque = 300; b.effectiveThrottle = 4096; b.gearRatio[1] = 4096;
        gt2::sim::DriveStepWork work{}; work.cars[0].clutchInputSpeed = 100000; work.cars[0].clutchOutputSpeed = 50000;
        gt2::sim::DrivetrainGlobals globals; globals.wheelClutch[0] = depression;
        gt2::sim::UpdateDriveShafts(b, work, 0, globals);
        return work.cars[0].axleTorque[1];
    };
    const int releasedTorque = transmitted(0), middleTorque = transmitted(127);
    Check(releasedTorque > 0 && middleTorque > 0 && middleTorque < releasedTorque && transmitted(255) == 0,
          "clutch progressively reduces actual axle torque to zero");

    CheckSteeringFeedback();
    CheckWheelDrivingAids();
    {
        gt2game::WheelShiftHint hint;
        hint.Tick(true, 0, true);
        for (int i = 0; i < 119; ++i) { hint.Tick(false, 0, true); Check(hint.Visible(), "brief blocked press remains readable for four seconds"); }
        hint.Tick(false, 0, true); Check(!hint.Visible(), "shift hint expires after four seconds");
        hint.Tick(true, 0, true); hint.Tick(false, 1, true);
        Check(!hint.Visible(), "successful shift clears obsolete stop hint");
        hint.Tick(true, 1, true); hint.Tick(false, 1, false);
        Check(!hint.Visible(), "leaving driving clears shift hint");
        hint.Tick(true, 0, true); hint.Reset(); Check(!hint.Visible(), "race restart clears shift hint");
    }
    {
        gt2::sim::CarBody car{}; car.forwardGears = 6; car.gear = 1; car.transmissionMode = 0;
        car.revLimitRpm = 6000; car.forwardSpeed = 4096 * 30;
        car.revsPerSpeed[1] = 4096 * 10; car.effectiveThrottle = 4096;
        gt2::sim::GearRequest shifts{};
        auto tick = [&](int wheelGear, uint32_t buttons, bool manual) {
            gt2::LogicalPad live{}; live.wheel = true; live.steerAxis = 2048; live.throttle = 1023;
            live.wheelGear = uint8_t(wheelGear); live.buttons = buttons;
            w::ApplyRaceTransmission(live, manual, car.gear);
            const auto recorded = gt2::FrameOfPad(live);
            gt2::sim::ApplyWheelInput(car, gt2::PadOfFrame(recorded, table), shifts);
            return gt2::sim::SelectGear(car, shifts);
        };
        for (int hardwareGear : {0, 1, 3, 5, 15})
            Check(tick(hardwareGear, 0, false) == 2, "race AT shifts up without paddles regardless of saved wheel gearbox/gate");
        Check(tick(15, 0, true) == -1 && tick(3, 0, true) == -1, "race MT retains paddles and H gate");
        Check(tick(0, 0, true) == -1, "race MT overrides wheel automatic preference");
        car.forwardSpeed = 0;
        Check(tick(15, gt2::kPadShiftDown, false) == 0, "AT left paddle selects reverse at rest");
        car.gear = 0; car.forwardSpeed = -4096;
        Check(tick(15, 0, false) == -1 && car.throttle == 4096, "AT reverse stays selected with analog throttle after paddle release");
        Check(tick(15, gt2::kPadShiftUp, false) == -1 && gt2::sim::WheelDirectionBlocked(car, shifts), "AT cannot leave reverse until car slows");
        car.forwardSpeed = 0;
        Check(tick(15, gt2::kPadShiftUp, false) == 1, "AT right paddle leaves reverse when stopped");
        Check(tick(5, 0, false) == 1, "AT forward H gate leaves reverse in first");
        gt2::LogicalPad unavailable{}; unavailable.wheel = true; unavailable.wheelGear = 1; unavailable.clutch = 255;
        w::ApplyRaceTransmission(unavailable, false, 1);
        Check(unavailable.wheelGear == 1 && unavailable.clutch == 255, "AT preserves disconnected wheel neutral and open clutch");
        car.gear = 1; car.effectiveThrottle = 0; car.clutchState = 0; car.shiftTimer = 0;
        for (int speed : {-1140, -2148, -26325, -100000}) {
            car.forwardSpeed = speed;
            Check(tick(15, 0, false) == -1, "AT never selects reverse automatically while coasting backwards");
        }
        shifts = {}; shifts.reserved[1] = 0x80; shifts.reserved[0] = 0;
        Check(gt2::sim::SelectGear(car, shifts) == 0, "old wheel replay automatic code retains its original behavior");
        shifts = {};
        Check(gt2::sim::SelectGear(car, shifts) == 0, "original gamepad automatic behavior remains unchanged");
    }
    std::cout << "Wheel checks passed: calibration, USB merging, disconnects, settings, replay, gearbox, clutch and force limits\n";
    return 0;
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
