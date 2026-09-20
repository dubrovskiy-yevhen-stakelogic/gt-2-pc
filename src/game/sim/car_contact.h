#pragma once
#include <cstddef>
#include <cstdint>

#include "game/sim/car_body.h"

// Car-to-car contact of the simulation step (original: 0x800400CC proximity, 0x800407A0 / 0x80040478 sweep,
// 0x80040924 resolution). Every car keeps, for each other car, the positions of that car's four footprint
// corners in its own frame (x across, y along the heading) for the previous and the current step; a corner
// that crosses one of the car's four edges between the two steps is a contact, resolved by an impulse along
// the edge normal and a corrective move of both bodies.
namespace gt2::sim {

#pragma pack(push, 1)
struct ContactCorner {          // 0x14 bytes; index [buffer] = step parity (0x801C8608)
    uint8_t edgeFlags[2];        // bits: 8 = beyond the front edge, 4 = behind the rear, 2 = left of, 1 = right of; 0xF = out of range
    uint8_t reserved2[2];
    int32_t x[2];                // across, 1/4096 m
    int32_t y[2];                // along the heading
};

// The original keeps this state in global RAM (0x801C8608..0x801C90A0); laid out identically so that the
// verification tool can run both on the same image.
struct CarContactState {
    int32_t buffer;                          // 0x000 (0x801C8608) current step parity, flipped every step
    uint8_t reserved004[4];
    int16_t fraction[6][5];                  // 0x008 (0x801C8610) [car][other slot]: earliest crossing, 0x1000 = none
    uint8_t reserved044[4];
    uint8_t corner[6][5];                    // 0x048 (0x801C8650) crossing corner, 0xFF = none
    uint8_t reserved066[2];
    int8_t side[6][5];                       // 0x068 (0x801C8670) edge crossed: 0 front, 1 rear, 2 left, 3 right, -1 none
    uint8_t reserved086[0x138 - 0x086];
    ContactCorner corners[6][5][4];          // 0x138 (0x801C8740) [car][other slot][corner]
};
#pragma pack(pop)

static_assert(sizeof(ContactCorner) == 0x14);
static_assert(offsetof(CarContactState, fraction) == 0x08);
static_assert(offsetof(CarContactState, corner) == 0x48);
static_assert(offsetof(CarContactState, side) == 0x68);
static_assert(offsetof(CarContactState, corners) == 0x138);
static_assert(sizeof(CarContactState) == 0x138 + 6 * 5 * 4 * 0x14);

// Cars take part in contact unless excluded or out of the race (0x800400CC, 0x800407A0).
inline bool TakesPartInContact(const CarBody& body) { return body.contactType == 0 && body.aiLine != 4; }

// Flips the step parity and records the corners of every other car in each car's frame (original: 0x800400CC).
void UpdateCarProximity(CarContactState& state, Car* cars, int count);

// Earliest crossing of any corner of the other car (`slot`) through car `car`'s edges between the previous and
// the current step. Returns the fraction (0x1000 = none), the corner and the side (original: 0x80040478).
int32_t SweepCarPair(const CarContactState& state, int car, int slot, const CarBody& body, int32_t& corner, int32_t& side);

// Runs the pair sweep for every ordered pair into the state tables (original: 0x800407A0).
void SweepCarPairs(CarContactState& state, Car* cars, int count);

// Resolves every contacting pair: impulse on both velocities, impact strength, corrective move of both bodies
// (original: 0x80040924). Uses the move context's track and globals.
void ResolveCarContacts(const MoveContext& context, const CarContactState& state, Car* cars, int count);

} // namespace gt2::sim
