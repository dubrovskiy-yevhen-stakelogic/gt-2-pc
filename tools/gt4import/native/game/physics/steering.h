#pragma once
// Native port of the per-car steering step 0x003f7ea8 of CORE.GT4 US 1.01 and its helpers: wheel steer angles
// 0x003f7ca8 / 0x003f7e20 (+0x0035a008), steering-wheel spring 0x0040cc58, steering assist / counter-steer servo
// (globals 0x00845bf0..) and the self-aligning wheel servo (globals 0x00620af0..0x00620b54).
// Verified by gt4verify rows phys_* / cap_phys_* (docs/research/physics_port.md section 11).
#include <cstdint>

#include "game/guest/bytes.h"

namespace gt4::game {

// 0x003f7ca8(spec, i, steer, -): steer angle of wheel i from the steering input (front axle: ratio spec +268 by
// side, rear axle: spec +4300 when byte +4304 == 1) plus the static toe spec +4104 (sign by entry byte 28 + i).
uint32_t PhysWheelSteerAngle(const uint8_t* spec, const uint8_t* entry, int64_t i, uint32_t steer);
// 0x0035a008(a, b): cos(a) * (3 - 2 cos(b)).
uint32_t PhysSteerGeometry(uint32_t a, uint32_t b);
// 0x003f7e20(car, i): steer angle of a front wheel for layout 1 (car +0x56c * 0x0035a008(table +48, camber)).
uint32_t PhysWheelSteerLayout1(const GuestBytes& mem, uint32_t car, int64_t i);
// 0x0040cc58(car): steering-wheel spring: +0x770 acceleration, +0x76c rate, +0x768 angle toward +0x56c * 11.25.
void PhysSteeringWheel(uint8_t* car);
// 0x0036ccd8(a, b, c, d): wrap(a * wrap(b) + c * wrap(d)) with the two angles unwrapped to within pi.
uint32_t GeoAngleBlend(uint32_t a, uint32_t b, uint32_t c, uint32_t d);
// 0x003f7ea8(car).
void PhysSteering(const GuestBytes& mem, uint32_t car);

} // namespace gt4::game
