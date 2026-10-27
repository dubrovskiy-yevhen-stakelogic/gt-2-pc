#pragma once
// Native ports of engine-side leaf helpers of CORE.GT4 US 1.01 (callees of 0x003519b8 / 0x003535b0 / 0x00357700 /
// 0x00344530). `car` = byte image of a car dynamics object (offsets from the car base, docs/research/car_state.md).
// Verified by gt4verify rows "phys_*" (docs/research/vu0.md).
#include <cstdint>

namespace gt4::game {

// 0x0034eb00(table, x s16, i): piecewise torque-like shape of x for segment i: breakpoint s16 table[+4+0xd0+2i],
// mode byte table[+0xd8+i], coefficients table[+4+0xe0+4i] / table[+4+0xf0+4i]. 0 outside [.., 3*breakpoint).
uint32_t PhysPiecewiseShape(const uint8_t* table, int64_t x, uint64_t i);
// 0x00350858(-, car): distance step |speed|(+0x6c8) * dt(+0x54c).
uint32_t PhysDistanceStep(const uint8_t* car);
// 0x00351178(car): if car[+0x514] == 0 -> clears byte +0x522 and returns 1; else sets byte +0x522 = 1 and returns 1.
uint64_t PhysUpdateStandstillLatch(uint8_t* car);
// 0x00355d88(car, divisor): car[+0x788] / global float at 0x008438dc.
uint32_t PhysScaledByGlobal(const uint8_t* car, uint32_t divisor);
// 0x003560f0(car, g): power = (car[+0x5ec] + car[+0x5f0]) * rpm(s16 +0x5fe) * 2pi/60000; returns
// min(1, car[+0x598] / (power / (power + 2g))) with g = global float at 0x008438c4.
uint32_t PhysPowerRatio(const uint8_t* car, uint32_t g);
// 0x00356e58(n, x): x * min(49, 95428.4 / n) (49 for n <= 100).
uint32_t PhysInverseScale(int64_t n, uint32_t x);
// 0x003598f8(curve, curve2, out1, out2, x): one linear search on curve's xs, interpolates curve's and curve2's ys
// (both {s16 n, xs*, ys*}); returns 1.
struct PhysCurve {
    int16_t count;
    const uint8_t* xs;
    const uint8_t* ys;
};
uint64_t PhysCurvePair(const PhysCurve& a, const uint8_t* ys2, uint8_t* out1, uint8_t* out2, uint32_t x);
// 0x0035dc08(car): rate-limits +0x6d0 toward |speed| +0x6c8 by 111.11 * dt, then a k = 200 / c = 20 follower of
// +0x664 (rate +0x660) toward +0x658 with dt = 1/60.
void PhysSpeedFilters(uint8_t* car);
// 0x003685a0(p, x, v): blends x toward 0.8 * clamp((4.1667 - v') / 4.1667, -1, 0) for v' = v (negated if byte
// p[+0x520] == 0) above 4.1667; x otherwise.
uint32_t PhysLowSpeedBlend(const uint8_t* p, uint32_t x, uint32_t v);
// 0x003694d0(car): tachometer target s16 +0x5fe = trunc(engine rad/s (+0x600) * 60 / 2pi).
void PhysRpmFromAngularSpeed(uint8_t* car);

// 0x00350e28(car): PhysDriveSelect(car, spec) * spec[+0x274 + 4*g] floored at 0, g = byte car[+0x624], forced to
// 1 for g >= 2 when spec byte +0x246 == 2 (0x003f1f88). Gear-ratio scaling of the selected drive quantity.
uint32_t PhysGearScaledDrive(const uint8_t* car, const uint8_t* spec);
// 0x003518e8(car, entry, globals): per wheel (count entry[+1], stride 0xec from car+0x164) splits
// w[+0x54]*w[+0x4c] (sign from s8 w[+0x45]) times w[+0x3c] into +0x7c/+0x80 by s8 w[+0x44] and copies into
// +0x74/+0x70 by global bytes +0xb6/+0xb7 (block 0x00620cd8). `entry` = image of the 0x00359510 table entry.
void PhysWheelSplit(uint8_t* car, const uint8_t* entry, const uint8_t* globals);
// 0x00360f80(obj, i, sel): address obj + 1040 + 492 * obj[3992 + sel + 24*k], k = entry[20 + i] for i < entry[1]
// else 0 (entry = 0x00359510 table entry of obj). `objAddress` is obj's guest address.
uint32_t DynWheelRecordAddress(uint32_t objAddress, const uint8_t* obj, const uint8_t* entry, int64_t i, uint64_t sel);
// 0x003f1f78 / 0x003f1f88 / 0x00368398: byte b[+18] == 1 / 2 / 3; 0x003f1f98: (b[+18] - 1) & 0xff < 2.
uint64_t DynKindIs(const uint8_t* b, unsigned kind);
uint64_t DynKindIs1or2(const uint8_t* b);
// 0x0035a5f8(car, i, v): car[+0x774 + i] = (s8)v for i < 4.
void DynSetSlotByte(uint8_t* car, int64_t i, uint64_t v);

} // namespace gt4::game
