#pragma once
// Native ports of leaf helpers of the engine / drivetrain step (callees of 0x003519b8, 0x003535b0, 0x00349fd0,
// 0x00357700 in CORE.GT4 US 1.01). `car` = byte image of a car dynamics object (offsets from the car base, as in
// docs/research/car_state.md); `spec` = image of the block pointed to by car[+0x10]. Verified by gt4verify rows
// "phys_*" (docs/research/vu0.md). Roles are stated only where code/data show them.
#include <cstdint>

namespace gt4::game {

// 0x0034ea68(limiter, x, w): soft limit of x for speed w (fields +0xdb flag, +0xdc, +0xe0): returns
// min(x, h(x*w)/w) with a quadratic knee, or x when inactive / below the knee.
uint32_t PhysSoftLimit(const uint8_t* limiter, uint32_t x, uint32_t w);
// 0x00355410(car, throttle): rev limiter: flag car[+0x625] set when rpm (s16 +0x5fe) >= spec[+0x134+0xcc] and
// cleared below that limit - 500; throttle = flag ? 0 : x * (car[+0x5ec] + car[+0x5f0]); then
// PhysSoftLimit(spec + 0x134, throttle - car[+0x5f0], engine speed car[+0x600]).
uint32_t PhysRevLimiter(uint8_t* car, const uint8_t* spec, uint32_t throttle);
// 0x00350da0(car): selects / blends car[+0x630] and car[+0x634] by the spec byte +0x234 (jump table 0x0069f650;
// 8 = blend by spec[+0x238]; >= 11 -> 0).
uint32_t PhysDriveSelect(const uint8_t* car, const uint8_t* spec);
// 0x00350d40(p): second-order follower (k = 500, c = 20, dt = 1/60): v(+0x640) += ((p[+0x588] - x(+0x63c)) * 500
// - v * 20) / 60; x += v / 60.
void PhysFollower(uint8_t* p);
// 0x00354ac0(car): car mass +0x5dc = spec[+0x4c] + 0.74 * car[+0x5d4] and +0x5d8 = spec[+0x48] + 0.74 * car[+0x5d4]
// (0.74 = fuel density in kg per litre; +0x5d4 = fuel).
void PhysMassWithFuel(uint8_t* car, const uint8_t* spec);
// 0x0035a9d0(curve{s32 mode, a, b, c}, slopeOut or null, x): odd response curve of |x| (mode 1 quadratic, 2 sqrt,
// 3 quartic-like, else identity) with optional derivative. `slopeOut` = image of the output float or nullptr.
uint32_t PhysResponseCurve(const uint8_t* curve, uint8_t* slopeOut, uint32_t x);
// 0x00348588(value*, target, step): moves *value toward target by at most step.
void PhysApproach(uint8_t* value, uint32_t target, uint32_t step);
// 0x0034ea08(obj, x, flagByte): flag (byte 0x00620e14) == 0 -> obj[+0xd0] * x; otherwise
// obj[+0xd0] * (x^2 + v^2/9) / v^2 with v = obj[+8 + 4 * s16 obj[+0]].
uint32_t PhysQuadraticTerm(const uint8_t* obj, uint32_t x, uint8_t flagByte);
// 0x00352218(signOut, angle): folds an angle into [0, pi/2] mirror range; writes +-1 to *signOut.
uint32_t PhysFoldAngle(uint8_t* signOut, uint32_t angle);
// 0x00369760(car, wheel, axis, d): car[+0x104 + 4a] + wheel mount car[+0x170 + 0xec*w + 4a] + car[+0x128 + 4a] * d
// (position + wheel mount + up-row * d).
uint32_t PhysWheelPointAxis(const uint8_t* car, uint64_t wheel, uint64_t axis, uint32_t d);
// 0x00348368(p): sets p[+0x514] to 1.0 or 0 from flags +0x4bc/+0x4ba/+0x467/+0x522, p[+0x48c] > 0.1 and
// |p[+0x53c]| < 0.2777 (1 km/h).
void PhysStandstillFlag(uint8_t* p);
// 0x00350678(out, globals): once (flag globals[+0xcc]) converts bytes globals[+0xd0..+0xd8] into 9 floats at out
// (x 100000 / x 0.001, some negated / halved). `globals` = image of the block at 0x00620cd8.
void PhysLoadByteConstants(uint8_t* out, uint8_t* globals);

} // namespace gt4::game
