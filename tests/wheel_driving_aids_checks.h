#pragma once
#include "../tools/gt2game/wheel_driving_aids.h"
#include "game/sim/tyres.h"

void CheckWheelDrivingAids() {
    const auto defaults = w::Settings::Parse("");
    Check(defaults.tractionControl == 0 && defaults.countersteer == 1 && !defaults.ignoreShiftSpeed,
          "default wheel aids are TCS off, weak countersteer, speed interlock on");
    gt2::sim::CarBody car{};
    car.gear = 2; car.steerLock = 512; car.forwardSpeed = 4096 * 20; car.driveType = 0;
    for (auto& wheel : car.wheels) wheel.load = 10000;
    for (auto& tyre : car.tyres) tyre.peakSlipRatioNeg = -512;
    gt2::LogicalPad original{}; original.wheel = true; original.wheelGear = 10;
    original.steerAxis = 1700; original.throttle = 900; original.brake = 13;
    w::Settings s; s.tractionControl = 0; s.countersteer = 0;
    gt2game::WheelDrivingAids aids;
    {
        auto rolling = car;
        gt2::sim::CarScratch scratch{};
        for (int i = 0; i < 4; ++i) {
            rolling.wheels[i].contactForwardSpeed = 20 * 4096;
            rolling.wheels[i].rimSpeed = 20 * 4096;
            rolling.wheels[i].slipScale = 4096;
            scratch.wheels[i].steerCos = 4096;
        }
        s.tractionControl = 3;
        gt2::sim::UpdateWheelSlipRatios(rolling, scratch);
        auto input = original;
        aids.Apply(input, rolling, s);
        Check(input.throttle == original.throttle, "native rolling tyres do not trigger TCS at road speed");
        rolling.wheels[2].rimSpeed = 40 * 4096;
        gt2::sim::UpdateWheelSlipRatios(rolling, scratch);
        input = original; aids.Apply(input, rolling, s);
        Check(rolling.wheels[2].slipRatio < rolling.tyres[1].peakSlipRatioNeg && input.throttle < original.throttle,
              "native driven-wheel overspeed triggers TCS, not a vehicle speed limit");
        aids.Reset(); s.tractionControl = 0;
        input = original; aids.Apply(input, rolling, s);
        Check(input.throttle == original.throttle, "TCS zero passes full throttle even with native wheelspin");
    }
    auto pad = original; aids.Apply(pad, car, s);
    Check(gt2::FrameOfPad(pad) == gt2::FrameOfPad(original), "disabled assists preserve every recorded input bit");
    s.tractionControl = 5; s.countersteer = 2;
    pad = original; aids.Apply(pad, car, s);
    Check(gt2::FrameOfPad(pad) == gt2::FrameOfPad(original), "no slip leaves steering and throttle intact");
    car.wheels[0].slipRatio = -3000;
    pad = original; aids.Apply(pad, car, s);
    Check(pad.throttle == original.throttle, "RWD traction control ignores front wheel slip");
    car.wheels[0].slipRatio = 0; car.wheels[2].slipRatio = -3000;
    int lastThrottle = original.throttle;
    for (int level = 1; level <= 5; ++level) {
        aids.Reset(); s.tractionControl = level; pad = original; aids.Apply(pad, car, s);
        Check(pad.throttle < lastThrottle && pad.brake == original.brake && pad.steerAxis == original.steerAxis,
              "higher TCS levels progressively cut driven wheelspin without braking or steering");
        lastThrottle = pad.throttle;
    }
    car.wheels[2].slipRatio = 0;
    for (int tick = 0; tick < 30; ++tick) {
        pad = original; aids.Apply(pad, car, s);
        Check(pad.throttle >= lastThrottle && pad.throttle <= original.throttle, "traction recovers smoothly without exceeding driver throttle");
        lastThrottle = pad.throttle;
    }
    Check(lastThrottle == original.throttle, "full grip restores full requested throttle");
    car.wheels[2].slipRatio = -3000; car.wheels[2].load = 0;
    aids.Reset(); pad = original; aids.Apply(pad, car, s);
    Check(pad.throttle == original.throttle, "airborne wheel is not a false grip measurement");
    car.wheels[2].load = 10000;
    for (int direction : {-1, 1}) {
        car.lateralSpeed = direction * 4096 * 4; car.yawRate = direction * 40000;
        original.steerAxis = uint16_t(2048 - direction * 400);
        int lastCorrection = 0;
        for (int help : {0, 1, 2}) {
            s.countersteer = help; aids.Reset();
            for (int tick = 0; tick < 30; ++tick) { pad = original; aids.Apply(pad, car, s); }
            const int correction = (int(pad.steerAxis) - int(original.steerAxis)) * direction;
            Check(correction >= lastCorrection, "countersteer opposes oversteer symmetrically and grows with assistance");
            if (help) Check(correction > 0, "countersteer assists both left and right slides");
            lastCorrection = correction;
        }
        car.yawRate = -car.yawRate; aids.Reset(); pad = original; aids.Apply(pad, car, s);
        Check(pad.steerAxis == original.steerAxis, "no countersteer in understeer or a stable turn");
    }
    for (int state = 0; state < 5; ++state) {
        auto stopped = car; auto input = original;
        if (state == 0) stopped.gear = 0;
        if (state == 1) input.buttons |= gt2::kPadHandbrake;
        if (state == 2) input.wheel = false;
        if (state == 3) stopped.raceState = 7;
        if (state == 4) input.wheelGear = 1;
        const auto before = gt2::FrameOfPad(input);
        aids.Apply(input, stopped, s);
        Check(gt2::FrameOfPad(input) == before, "reverse, handbrake, non-wheel, finish and neutral disable wheel assists");
    }
    s.tractionControl = 4; s.countersteer = 2;
    const auto saved = w::Settings::Parse(s.Serialize());
    Check(saved.tractionControl == 4 && saved.countersteer == 2, "assist preferences survive save and reload");
    const auto invalid = w::Settings::Parse("driving_aids 999 -20\n");
    Check(invalid.tractionControl == 5 && invalid.countersteer == 0, "assist settings clamp out of range values");
}
