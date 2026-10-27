#pragma once
// Garage car object from a car code (CORE.GT4 US 1.01, SHA-1 30031137ce494323816bb707b90d3ec0d07937d2) and the
// "ShowRoomCar" metric of a car, used by the menu's opponent pick (MCarData.PickUpEnemyCars 0x001308c8):
//   0x00444190 / 0x004442b8  garage car constructor (part keys -1, settings bytes)
//   0x004443a8               from a label: GENERIC_CAR code (0x00443e00 table 0), else ENEMY_CARS (table 0x26)
//   0x00444440               by key table: 0x26 -> 0x00444690 (enemy car), else 0x00444480 (generic car)
//   0x00444480 / 0x00444690  DEFAULT_PARTS row -> part keys, 0x004449c0 settings defaults from the part rows
//                            (0x00444a88 .. 0x00445230, clamp 0x004452a8), VARIATION key 0x004457b0, enemy extra
//                            settings 0x00444860, garage gear recompute 0x00443240 (carparams::GarageGearRatios)
//   0x00130540 / 0x001305a0  metric = ShowRoomCar setup 0x00364868 (carparams::SetupCar) -> phys + 0xfe0
// The car object is 0x178 bytes (garage stride); bytes no routine writes (0x12b, 0x15d, ...) keep the caller's
// residue, as in the original (the caller passes the buffer). Works on a guest memory image because the gear
// recompute and the car setup do (src/game/physics/carparams_init.h).
#include <cstdint>

#include "game/physics/carparams.h"
#include "game/physics/carparams_init.h"

namespace gt4::game::menu {

constexpr uint32_t kGarageCarSize = 0x178;

// 0x004442b8 on the car at guest address `car`.
void GarageCarConstruct(carparams::Guest g, uint32_t car);
// 0x00444440(car, key): fills the car from a GENERIC_CAR or ENEMY_CARS key. `sp` = the caller's stack pointer
// (0x00443240 builds its car parameter block below it). Returns the original's v0 (0 = failed).
uint32_t GarageCarFromKey(carparams::Guest g, uint32_t car, uint64_t key, uint32_t sp, const carparams::SpecSource& db);

// 0x00130540(car): ShowRoomCar on `work` (0x1800 bytes in guest memory; cprm at work, physics block at work+0x600),
// SetupCar(car, opt 0), returns the float bit pattern at work + 0x15e0.
uint32_t ShowRoomCarMetric(carparams::Guest g, uint32_t work, uint32_t car, const carparams::SpecSource& db,
                           carparams::BuildFrame& frame);

}  // namespace gt4::game::menu
