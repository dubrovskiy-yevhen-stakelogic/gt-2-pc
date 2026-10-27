#pragma once
// Native ports of geometry / angle / curve helpers of CORE.GT4 US 1.01 (0x00359568..0x0036fa84, 0x005f5010..):
// raw EE float patterns, src/ee/fpu arithmetic, guest structures as byte images (access order kept, so aliasing
// between arguments behaves as in the original). Verified by gt4verify rows "geo_*" (docs/research/vu0.md).
#include <cstdint>

namespace gt4::game {

// ---- angles in degrees ----
uint32_t GeoWrap360(uint32_t deg);                                 // 0x0036ead8: [0, 360)
uint32_t GeoWrap180(uint32_t deg);                                 // 0x0036eb58: (-180, 180]
uint32_t GeoAngleDiff(uint32_t a, uint32_t b);                     // 0x0036eb98: a - b wrapped to [-180, 180]
uint32_t GeoAngleLerp(uint32_t a, uint32_t b, uint32_t t);         // 0x0036ec78
uint32_t GeoHeading(const uint8_t* v);                             // 0x0036ea98: atan2(v.x, v.z) (VU0) in degrees
uint32_t GeoPitch(const uint8_t* v);                               // 0x0036ea50: -atan2(v.y, |v.xz|) in degrees
uint32_t GeoAtan2Degrees(uint32_t y, uint32_t x);                  // 0x0036ecd0 (octant tables 0x00620e30/50)
uint32_t GeoCosineOffset(uint32_t a, uint32_t b, uint32_t c);      // 0x0036ebe8: a - cos(a deg) * b/2 * c

// ---- angles in radians ----
uint32_t GeoWrapRadiansGeneral(uint32_t x);                        // 0x0036cb20: x - 2pi*trunc((|x|+pi)/2pi)*sign
uint32_t GeoWrapRadians(uint32_t x);                               // 0x0036cb90: to [-pi, pi) (fast path +-2pi)
uint32_t GeoAngleDiffRadians(uint32_t a, uint32_t b);              // 0x0036cc40
uint32_t GeoClampScaled(uint32_t x);                               // 0x00348800: clamp(x * 12/pi, -1, 1)

// ---- vec3 (float[3] images) ----
void GeoCross(uint8_t* out, const uint8_t* a, const uint8_t* b);   // 0x0036e950 (may alias)
uint32_t GeoLengthSquared(const uint8_t* v);                       // 0x005f5010
void GeoNormalize(uint8_t* v);                                     // 0x0036e9d8: no-op if length <= 0
void GeoAddAssign(uint8_t* a, const uint8_t* b);                   // 0x005f5230 (returns a)
void GeoSubAssign(uint8_t* a, const uint8_t* b);                   // 0x005f5268 (returns a)

// ---- curves ----
// {s16 count; u32 xs; u32 ys} with xs non-decreasing: piecewise-linear y(x), clamped at the ends.
struct GeoCurve {
    int16_t count;
    const uint8_t* xs;   // image of the xs array
    const uint8_t* ys;
};
uint32_t GeoCurveBinary(const GeoCurve& c, uint32_t x);            // 0x00359568 (binary search)
uint32_t GeoCurveLinear(const GeoCurve& c, uint32_t x);            // 0x00359830 (linear search)
// 0x0036f880: {s32 n; {x, y}[n]}, x clamped to [0, 1], binary search, linear interpolation between pairs.
uint32_t GeoPairTable(const uint8_t* table, uint32_t x);
// 0x0036f7d8: {x1, x2, y1, y2}: x clamped to [0, 1], piecewise linear through (0,0) (x1,y1) (x2,y2) (1,1).
uint32_t GeoPiecewise(const uint8_t* p, uint32_t x);
// 0x0036f710: cubic h(t) = ((A t + B) t + C) t from end slopes: stores p0..p3 at +0..+12, A at +16, B at +20,
// C = p1/p0 at +24 (A = C + p3/p2 - 2, B = -2C - p3/p2 + 3).
void GeoHermiteSetup(uint8_t* h, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t p3);
uint32_t GeoHermiteEval(const uint8_t* h, uint32_t t);             // 0x0036f778 (t clamped to [0, 1])
// 0x0036f980: r = clamp(h(clamp(x / obj[+36])), 0, 1) with h at obj+8; returns obj[4] + (obj[0] - obj[4]) * r.
uint32_t GeoHermiteRemap(const uint8_t* obj, uint32_t x);

// ---- small integer / counter helpers ----
uint64_t GeoSaturatingIncrement(uint64_t v);                       // 0x0036ec60
uint32_t GeoPercentOrReciprocal(uint64_t v);                       // 0x0036fa20: v<0 -> 1/(-v), 0 -> 1, else v/100
// 0x0036f6a0(counter{s8 cur, s8 max}, dir, x): dir == 0 resets cur and returns 0; otherwise cur steps toward max
// and the result is x * cur / max, negated for dir < 0.
uint32_t GeoRampCounter(uint8_t* counter, uint64_t dir, uint32_t x);

// 0x00407208(dst, src): copies 64 words src -> dst one word at a time (forward); returns dst.
void GeoCopy64(uint8_t* dst, const uint8_t* src);
// 0x00486ff8(q, out, v): out = rotation of v by the unit quaternion q = {x, y, z, w} (2 * rows of the half matrix),
// written component by component with v re-read after each store (out may alias v).
void GeoQuatRotate(const uint8_t* q, uint8_t* out, const uint8_t* v);

} // namespace gt4::game
