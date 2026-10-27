#pragma once
// Race-start physics initialisation from the car parameter block (CORE.GT4 US 1.01): 0x00361908 and every callee
// (0x0034aca8 drivetrain, 0x0034ef30 engine curve, 0x0034ba30 tyre radii, 0x00360dc8 suspension, ...).
// The port works on a guest memory image (EE RAM, physical addresses) so that the physics block - which stores
// guest pointers to its own curve tables - and the globals it reads and writes (0x00620xxx settings / tables,
// 0x008438c0.. derived constants) stay byte-identical to the original. Verified by the gt4verify rows
// "carparams_physinit" (random car parameter blocks) and "carparams_state" (the PCSX2 race state).
#include <cstddef>
#include <cstdint>

namespace gt4::game::carparams {

struct Guest {
    uint8_t* ram;      // 32 MiB EE RAM image
};

// 0x00361908(phys, cprm, mode, arg4, table, opt): fills the 0x10e0-byte physics block at `phys` from the car
// parameter block at `cprm` (which it also normalises: drive kind, gear table). `table` = 0x006203c8 or the race's
// per-entry settings; `opt` = guest address of a 0x20-byte option record (0x003433e8 defaults) or 0.
void PhysInit(Guest g, uint32_t phys, uint32_t cprm, int32_t mode, uint32_t arg4, uint32_t table, uint32_t opt);

// 0x0035f808(race): physics settings constants (0x003f2f78), the drag curve 0x00620938 from the settings bytes
// 0x00620ce3.., and with a race object (`raceKind` = 0x0034c998(race), -1 = none) the 24 per-entry tables.
void RaceSettings(Guest g, int32_t raceKind);
// 0x00364818: engine power / torque peak record at phys + 0x1100 (0x0034f3f8 + 0x00356ea8).
void PhysPeakRecord(Guest g, uint32_t phys, uint32_t opt);
// 0x00361570(phys, cprm, mode, 0): per-wheel tyre records of the axles in `mode` (pit tyre rebuild).
void PhysTyreSetup(Guest g, uint32_t phys, uint32_t cprm, int32_t mode);

class SpecSource;
struct BuildFrame;
// 0x00364868(obj, car, opt) - the car setup of the race / showroom car object {cprm at `cprm`, physics block at
// cprm + 0x600}: RaceSettings(none), 0x003f3b68 (builder + race defaults) into guest memory, PhysInit, peaks.
void SetupCar(Guest g, uint32_t cprm, uint8_t* car, const SpecSource& db, BuildFrame& frame, uint32_t opt);

// 0x00443240(car): garage gear recompute after a GEAR install. With the GEAR row's auto flag (r+0x25) it builds a
// car parameter block in its stack frame (0x004413c8, the frame's prior bytes as residue) and replaces the car's gear
// ratios car+0x110.. with the auto table of 0x00354ff8 for the top speed car+0x12a * 10 km/h. Works on guest
// memory: `car` = the garage car object, `sp` = the caller's sp (frame at sp - 0x670, builder frame below it).
// Returns 1 when the GEAR row exists, else 0.
uint32_t GarageGearRatios(Guest g, uint32_t car, uint32_t sp, const SpecSource& db);

} // namespace gt4::game::carparams
