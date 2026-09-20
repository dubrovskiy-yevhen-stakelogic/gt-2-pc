#include "platform/input/ps1_pad.h"

#include <cstring>

#include "game/sim/fixed.h"

namespace gt2::input {

namespace {

uint16_t U16(const uint8_t* p, uint32_t o) { uint16_t v; std::memcpy(&v, p + o, 2); return v; }
int16_t S16(const uint8_t* p, uint32_t o) { int16_t v; std::memcpy(&v, p + o, 2); return v; }
uint32_t U32(const uint8_t* p, uint32_t o) { uint32_t v; std::memcpy(&v, p + o, 4); return v; }
void Put16(uint8_t* p, uint32_t o, uint16_t v) { std::memcpy(p + o, &v, 2); }
void Put32(uint8_t* p, uint32_t o, uint32_t v) { std::memcpy(p + o, &v, 4); }

} // namespace

PadTables PadTables::Load(const GuestImage& exe) {
    PadTables t;
    for (uint32_t i = 0; i < 16; i++) {
        t.remap[i][0] = exe.Get<uint8_t>(exe.Sim(kRemap) + i * 2);
        t.remap[i][1] = exe.Get<uint8_t>(exe.Sim(kRemap) + i * 2 + 1);
    }
    for (uint32_t i = 0; i < 3; i++) {
        const uint32_t a = exe.Sim(kNegconButtons) + i * 4;
        t.negconButtons[i].threshold = exe.Get<uint16_t>(a);
        t.negconButtons[i].axis = exe.Get<uint8_t>(a + 2);
        t.negconButtons[i].bit = exe.Get<uint8_t>(a + 3);
    }
    t.repeat[0] = exe.Get<uint8_t>(exe.Sim(kRepeat));
    t.repeat[1] = exe.Get<uint8_t>(exe.Sim(kRepeat) + 1);
    return t;
}

uint32_t RemapButtons(uint32_t raw, const PadTables& t) { // 0x80083A4C(raw, pairs, 16)
    uint32_t out = 0;
    for (const auto& p : t.remap)
        if (raw & (1u << (p[0] & 31))) out |= 1u << (p[1] & 31);
    return out;
}

uint32_t NegconButtons(const std::array<uint16_t, 4>& analog, const PadTables& t) { // 0x80083A88(analog, 0x800A6ECC, 3)
    uint32_t out = 0;
    for (const auto& b : t.negconButtons) {
        const uint16_t v = analog[b.axis & 3]; // lhu analog + axis * 2 (axes 0..3 of the reader's words)
        if (!(int32_t(v) < int32_t(b.threshold))) out |= 1u << (b.bit & 31);
    }
    return out;
}

void TrackButtons(uint8_t* tr, uint32_t bits) { // 0x800838B4
    using namespace tracker;
    uint32_t repeat = U32(tr, kRepeat);
    const uint8_t reload = tr[kRepeatReload], first = tr[kRepeatFirst];
    const uint32_t previous = U32(tr, kCurrent), last = U32(tr, kLast);
    Put32(tr, kLast, 0xFFFFFFFFu);
    Put32(tr, kCurrent, bits);
    const uint32_t changed = previous ^ bits;
    Put32(tr, kHeld, U32(tr, kHeld) | bits);
    Put32(tr, kPressed, U32(tr, kPressed) | (changed & bits));
    Put32(tr, kReleased, (U32(tr, kReleased) | (changed & previous)) & last);
    uint32_t bit = 1;
    for (uint32_t i = 0; i < 32; i++, bit <<= 1) {
        uint32_t c = uint32_t(tr[kCounters + i]) + 1;
        if ((bits & bit) == 0) c = 0;
        if (c == first) c = reload; // (byte compares of the original: c is at most 0x100 here)
        tr[kCounters + i] = uint8_t(c);
        if (c == reload) repeat |= bit; // (the full sum against the byte)
    }
    Put32(tr, kRepeat, repeat);
}

void InitTracker(uint8_t* tr, const PadTables& t) { // 0x80083868 after its 60-byte clear
    tr[tracker::kRepeatReload] = t.repeat[0];
    tr[tracker::kRepeatFirst] = t.repeat[1];
    Put32(tr, tracker::kCurrent, 0xFFFFFFFFu);
}

uint16_t DualShockAxis(uint16_t raw) { // 0x80085890
    const int32_t x = raw;
    if (x < 91) return uint16_t(sim::Div(x << 7, 91));
    if (x < 165) return 128;
    return uint16_t(sim::Div((x - 165) << 7, 91) + 128);
}

uint16_t NegconTwist(const uint8_t* c, uint16_t raw) { // 0x800831BC
    const int32_t x = raw & 0xFF;
    const int32_t lo = U16(c, 0), deadLo = U16(c, 2), deadHi = U16(c, 4), hi = U16(c, 6);
    if (x < lo) return 0;
    if (x < deadLo) return uint16_t(sim::Div((x - lo) << 7, deadLo - lo));
    if (x < deadHi) return 128;
    if (!(x < hi)) return 255;
    return uint16_t(sim::Div((x - deadHi) << 7, hi - deadHi) + 128);
}

uint16_t NegconPedal(const uint8_t* c, int axis, uint16_t raw) { // 0x80083250
    const uint8_t* e = c + (axis - 1) * 4;
    const int32_t x = raw & 0xFF, lo = e[8], hi = e[10]; // lbu of the u16 fields
    if (x < lo) return 0;
    if (!(x < hi)) return 255;
    return uint16_t(sim::Div((x - lo) << 8, hi - lo));
}

Actuators ActuatorsOf(const uint8_t* obj, uint8_t actuatorCount) { // 0x8008371C
    int32_t smallMotor = int32_t(int16_t(U16(obj, pad_object::kVibration + 4))) >> 4; // (lhu << 16) >> 20
    int32_t largeMotor = int32_t(int16_t(U16(obj, pad_object::kVibration + 2))) >> 4;
    if (smallMotor < 0) smallMotor = -smallMotor;
    if (smallMotor > 255) smallMotor = 255;
    if (largeMotor < 0) largeMotor = -largeMotor;
    if (largeMotor > 255) largeMotor = 255;
    Actuators a;
    if (actuatorCount == 1) { // a controller without the extended protocol: one on / off motor on act[1], act[0] = 0x40
        const int32_t onOff = smallMotor != 0 ? smallMotor : (largeMotor >= 97 ? 1 : 0);
        a.smallMotor = 0x40;
        a.largeMotor = uint8_t(onOff);
        return a;
    }
    a.smallMotor = uint8_t(smallMotor);
    a.largeMotor = uint8_t(largeMotor);
    return a;
}

void DigitalHandler(uint8_t* obj, uint32_t raw, const PadTables& t) { // 0x80083818
    TrackButtons(obj + pad_object::kTracker, RemapButtons(raw, t));
    Put16(obj, pad_object::kAxisMask, 0);
}

void AnalogHandler(uint8_t* obj, uint32_t raw, const std::array<uint16_t, 4>& analog, const PadTables& t) { // 0x800858CC
    using namespace pad_object;
    TrackButtons(obj + kTracker, RemapButtons(raw, t));
    Put16(obj, kAxisMask, uint16_t(U16(obj, kAxisMask) | 0xF));
    for (uint32_t k = 0; k < 4; k++) Put16(obj, kAxes + k * 2, DualShockAxis(analog[k]));
}

void NegconHandler(uint8_t* obj, uint32_t raw, const std::array<uint16_t, 4>& analog, const PadTables& t, const uint8_t* calibration) { // 0x800832C0
    using namespace pad_object;
    TrackButtons(obj + kTracker, RemapButtons(raw, t) | NegconButtons(analog, t));
    Put16(obj, kAxisMask, uint16_t(U16(obj, kAxisMask) | 0xF));
    Put16(obj, kAxes, NegconTwist(calibration, analog[0]));
    for (int k = 1; k < 4; k++) Put16(obj, kAxes + uint32_t(k) * 2, NegconPedal(calibration, k, analog[size_t(k)]));
}

Actuators PollPad(uint8_t* obj, const Ps1PadFrame& frame, const PadTables& t, const uint8_t* calibration, uint8_t actuatorCount) {
    using namespace pad_object;
    uint8_t type = frame.type;
    uint32_t buttons = type == kTypeNone ? 0u : frame.buttons;
    std::array<uint16_t, 4> analog{};
    switch (type) {
    case kTypeNegcon: case kTypeAnalogJoystick: case kTypeAnalog:
        for (size_t k = 0; k < 4; k++) analog[k] = frame.analog[k];
        break;
    case 1: case 3: case kTypeDigital: case 6: case kTypeJogcon:
        break; // (mouse / Jogcon words are not used by the race's handlers)
    default:
        type = 0;
        buttons = 0;
        break;
    }
    obj[kConnected] = type != 0 ? 1 : 0;
    obj[kType] = type;
    Actuators sent;
    bool post = false;
    // The race's handler table 0x800A6F5C: 2 -> 0x800832C0, 4 -> 0x80083818 / 0x8008371C, 5 and 7 -> 0x800858CC / 0x8008371C.
    if (type == kTypeDigital) {
        DigitalHandler(obj, buttons, t);
        post = true;
    } else if (type == kTypeAnalog || type == kTypeAnalogJoystick) {
        AnalogHandler(obj, buttons, analog, t);
        if (frame.pressure) { // ours: the trigger travel as axes 4 / 5 (raw, as the neGcon pedals after calibration)
            Put16(obj, kAxisMask, uint16_t(U16(obj, kAxisMask) | (1u << kAxisR2Pressure) | (1u << kAxisL2Pressure)));
            Put16(obj, kAxes + kAxisR2Pressure * 2, frame.pressureR2);
            Put16(obj, kAxes + kAxisL2Pressure * 2, frame.pressureL2);
        } // (without: bits 4 / 5 left as they are, like the original's `|= 0xF`)
        post = true;
    } else if (type == kTypeNegcon) {
        NegconHandler(obj, buttons, analog, t, calibration);
    } else { // no handler: 0x800838B4(tracker, 0), + 0x48 = 0
        TrackButtons(obj + kTracker, 0);
        Put16(obj, kAxisMask, 0);
    }
    if (post) sent = ActuatorsOf(obj, actuatorCount);
    const int16_t timer = int16_t(S16(obj, kVibrationTimer) - 1);
    Put16(obj, kVibrationTimer, uint16_t(timer));
    if (timer == -1)
        for (uint32_t o = kVibration; o <= kVibrationTimer; o += 2) Put16(obj, o, 0);
    return sent;
}

void SnapshotTracker(uint8_t* tr, uint8_t* out) { // 0x80083998 (+ 0x80083958)
    for (uint32_t k = 0; k < 4; k++) Put32(out, k * 4, U32(tr, tracker::kHeld + k * 4));
    for (uint32_t k = 0; k < 4; k++) Put32(tr, tracker::kHeld + k * 4, 0);
}

int TableOfType(uint8_t type) {
    switch (type) {
    case kTypeAnalogJoystick: case kTypeAnalog: return 1;
    case kTypeNegcon: return 2;
    case kTypeJogcon: return 3;
    default: return 0;
    }
}

void BuildRaceLogical(uint8_t* obj, const uint8_t* tables) { // 0x80014BB4
    using namespace pad_object;
    SnapshotTracker(obj + kTracker, obj + kSnapshot);
    std::memmove(obj + kSnapshot + 0x10, obj + kAxisMask, 0x10); // + 0x48 .. + 0x57 -> + 0x78 .. + 0x87
    Put16(obj, kSnapshot + 0x20, U16(obj, kAxisMask + 0x10));   // + 0x58 -> + 0x88
    const uint8_t* table = tables + TableOfType(obj[kType]) * int(pad_block::kTableSize);
    const uint32_t held = U32(obj, kSnapshot), pressed = U32(obj, kSnapshot + 4), released = U32(obj, kSnapshot + 8), repeat = U32(obj, kSnapshot + 12);
    const uint16_t axisMask = U16(obj, kSnapshot + 0x10);
    uint32_t l0 = 0, l1 = 0, l2 = 0, l3 = 0;
    Put16(obj, kAnalogFlags, 0);
    for (uint32_t f = 0; f < uint32_t(kFunctions); f++) {
        const uint32_t e = table[f];
        const uint32_t bit = 1u << f;
        if (e < 0x80) {
            const uint32_t b = 1u << (e & 31);
            if (held & b) l0 |= bit;
            if (pressed & b) l1 |= bit;
            if (released & b) l2 |= bit;
            if (repeat & b) l3 |= bit;
            continue;
        }
        const uint32_t axis = e & 0x1F;
        if ((axisMask & (1u << axis)) == 0) continue; // (the original loads the value first; only axes 0..15 can be valid)
        int32_t v = U16(obj, kSnapshot + 0x12 + axis * 2);
        Put16(obj, kAnalogFlags, uint16_t(U16(obj, kAnalogFlags) | bit));
        if (e >= 0xA0) {
            if (e < 0xC0) v = 127 - v;
            else if (e < 0xE0) v = v - 128;
            else v = v < 128 ? 127 - v : v - 128;
            if (v < 0) v = 0;
            if (v > 127) v = 127;
            v = (v << 1) | (v >> 7);
        }
        Put16(obj, kValues + f * 2, uint16_t(v));
    }
    Put32(obj, kLogical, l0);
    Put32(obj, kLogical + 4, l1);
    Put32(obj, kLogical + 8, l2);
    Put32(obj, kLogical + 12, l3);
}

void FeedVibration(uint8_t* obj, const VibrationSource& s) { // 0x800133F0 tail
    using namespace pad_object;
    if (s.demo || s.padSlot >= 4 || s.padSlot < 2) return;
    Put16(obj, kVibrationTimer, 2);
    if (s.vibrationOff == 0 && s.finishTime == 0) {
        Put16(obj, kVibration, uint16_t(s.word760));
        Put16(obj, kVibration + 2, uint16_t(uint16_t(s.loadLevel) << 4));
        Put16(obj, kVibration + 4, uint16_t(uint16_t(s.impactFlag) << 4));
    } else {
        Put16(obj, kVibration, 0);
        Put16(obj, kVibration + 2, 0);
        Put16(obj, kVibration + 4, 0);
    }
}

std::array<uint8_t, 11> TriggerPedalTable(const uint8_t* analogTable) {
    std::array<uint8_t, 11> t{};
    std::memcpy(t.data(), analogTable, t.size());
    constexpr uint8_t kR2Bit = 13, kL2Bit = 5; // generic bits of R2 / L2
    const uint8_t oldAccel = t[2], oldBrake = t[3];
    t[2] = uint8_t(0x80 | kAxisR2Pressure);
    t[3] = uint8_t(0x80 | kAxisL2Pressure);
    for (size_t f = 0; f < t.size(); f++) {
        if (f == 2 || f == 3) continue;
        if (t[f] == kR2Bit && oldAccel < 0x80) t[f] = oldAccel;
        else if (t[f] == kL2Bit && oldBrake < 0x80) t[f] = oldBrake;
    }
    return t;
}

} // namespace gt2::input
