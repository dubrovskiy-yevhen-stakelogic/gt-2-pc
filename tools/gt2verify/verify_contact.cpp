// Differential checks of src/game/sim/car_contact.* (car-to-car contact) against the original.
#include "game/sim/car_contact.h"
#include "guest.h"

namespace gt2::verify {

namespace {
constexpr uint32_t kStateBase = 0x801C8608u; // CarContactState in the original's RAM
sim::Car* CarsOf(uint8_t* ram) { return reinterpret_cast<sim::Car*>(ram + (kCarBase & 0x1FFFFF)); }
sim::CarContactState& StateOf(uint8_t* ram) { return *reinterpret_cast<sim::CarContactState*>(ram + (ContactBase() & 0x1FFFFF)); }

// Puts the cars close to each other with random headings so that corners fall inside other cars' frames.
void ScatterCars(std::mt19937& rng, uint8_t* ram, size_t variant) {
    sim::Car* cars = CarsOf(ram);
    if (variant % 4 == 0) return; // dump positions
    const int32_t anchorX = cars[0].body.position[0], anchorY = cars[0].body.position[1], anchorZ = cars[0].body.position[2];
    const int32_t spread = variant % 4 == 1 ? 0x6000 : variant % 4 == 2 ? 0x10000 : 0x40000; // 6 m, 16 m, 64 m
    for (uint32_t car = 0; car < kCarCount; car++) {
        sim::CarBody& b = cars[car].body;
        b.position[0] = int32_t(uint32_t(anchorX) + rng() % uint32_t(2 * spread) - uint32_t(spread));
        b.position[1] = int32_t(uint32_t(anchorY) + rng() % uint32_t(2 * spread) - uint32_t(spread));
        b.position[2] = int32_t(uint32_t(anchorZ) + rng() % 0x5000 - 0x2800);
        b.heading = int16_t(rng() % 0x1000);
        b.contactType = rng() % 8 == 0;
        b.aiLine = rng() % 8 == 0 ? 4 : uint8_t(rng() % 4);
        for (int set = 0; set < 2; set++) {
            b.footprintSet = uint8_t(set);
            sim::UpdateFootprint(b);
        }
        b.footprintSet = uint8_t(rng() & 1);
    }
}
} // namespace

int VerifyContact(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track) {
    int failures = 0;
    auto ramS32 = [&](uint32_t address) { int32_t v; std::memcpy(&v, &pristine[address & 0x1FFFFF], 4); return v; };

    // Proximity tables from the cars' positions.
    StatefulResult r = VerifyStateful(
        guest, pristine, 0x800400CC, {kCarBase}, 400,
        [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
            ScatterCars(rng, ram, variant);
            StateOf(ram).buffer = int32_t(rng() & 1);
        },
        [&](uint8_t* ram, uint8_t*, uint32_t) { sim::UpdateCarProximity(StateOf(ram), CarsOf(ram), int(kCarCount)); }, kCarCount);
    Report("Proximity", 0x800400CCu, r.cases, r.mismatches, failures);

    // Pair sweep from random corner tables (both step parities).
    size_t contacts = 0;
    r = VerifyStateful(
        guest, pristine, 0x800407A0, {kCarBase}, 400,
        [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
            ScatterCars(rng, ram, variant);
            sim::CarContactState& state = StateOf(ram);
            state.buffer = int32_t(rng() & 1);
            for (auto& perCar : state.corners)
                for (auto& perOther : perCar)
                    for (sim::ContactCorner& corner : perOther)
                        for (int buffer = 0; buffer < 2; buffer++) {
                            corner.edgeFlags[buffer] = uint8_t(rng() % 5 == 0 ? 0xF : rng() % 16);
                            corner.x[buffer] = int32_t(rng() % 0x8000) - 0x4000;
                            corner.y[buffer] = int32_t(rng() % 0x10000) - 0x8000;
                        }
        },
        [&](uint8_t* ram, uint8_t*, uint32_t) {
            sim::SweepCarPairs(StateOf(ram), CarsOf(ram), int(kCarCount));
            for (auto& row : StateOf(ram).fraction)
                for (int16_t f : row) contacts += f != 0x1000;
        },
        kCarCount);
    std::printf("%-10s 0x800407A0  %zu cases (%zu contacts), %zu mismatches  %s\n", "PairSweep", r.cases, contacts, r.mismatches, r.mismatches ? "FAIL" : "ok");
    failures += r.mismatches ? 1 : 0;

    if (!track) {
        std::puts("CarContact 0x80040924  skipped (needs the course: gt2verify <ram> <disc> <course>)");
        return failures;
    }
    sim::MoveContext context;
    context.track = track;
    context.globals.frameTime = ramS32(D(0x801C856Cu));
    context.globals.rate = ramS32(D(0x801C8570u));
    context.collisionDisabled = uint16_t(ramS32(D(0x800A9520u))) != 0; // the u16 hold counter (0x800A9522 next to it is another field)
    size_t resolved = 0;
    r = VerifyStateful(
        guest, pristine, 0x80040924, {kCarBase}, 400,
        [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
            const int32_t stepTime = 2184;
            std::memcpy(scratch, &stepTime, 4);
            ScatterCars(rng, ram, variant);
            sim::CarContactState& state = StateOf(ram);
            for (uint32_t car = 0; car < kCarCount; car++) {
                sim::CarBody& b = CarsOf(ram)[car].body;
                if (variant % 4 != 0)
                    for (int k = 0; k < 3; k++) b.velocity[k] = int32_t(rng() % 0x80000) - 0x40000;
                for (uint32_t slot = 0; slot < 5; slot++) {
                    const bool contact = rng() % 3 == 0;
                    state.fraction[car][slot] = contact ? int16_t(rng() % 0x1000) : int16_t(0x1000);
                    state.side[car][slot] = contact ? int8_t(rng() % 4) : int8_t(-1);
                    state.corner[car][slot] = contact ? uint8_t(rng() % 4) : 0xFF;
                }
            }
        },
        [&](uint8_t* ram, uint8_t*, uint32_t) {
            sim::ResolveCarContacts(context, StateOf(ram), CarsOf(ram), int(kCarCount));
            for (uint32_t car = 0; car < kCarCount; car++) resolved += (CarsOf(ram)[car].body.contactFlags & 2) != 0;
        },
        kCarCount, 0, 0, ScratchIgnore{0, 0xD4}); // the original uses the scratchpad for its wall sweep and the two separation moves
    std::printf("%-10s 0x80040924  %zu cases (%zu car-contacts), %zu mismatches  %s\n", "CarContact", r.cases, resolved, r.mismatches, r.mismatches ? "FAIL" : "ok");
    failures += r.mismatches ? 1 : 0;
    return failures;
}

} // namespace gt2::verify
