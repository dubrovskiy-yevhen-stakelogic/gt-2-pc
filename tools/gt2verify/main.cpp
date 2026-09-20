// gt2verify - differential testing of ported simulation code against the original.
//   gt2verify <ram.bin>        (RAM dump written by `gt2run calltrace`, contains the race overlay + EXE)
// For every entry of the test tables the ORIGINAL routine is executed in the R3000 interpreter on the dump
// and OUR C++ port is executed natively with the same inputs; any difference is a failure.
// This tool is the only place where ported code and the interpreter meet. The game never links the interpreter.
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "game/sim/car_body.h"
#include "game/sim/fixed.h"
#include "game/sim/track_collision.h"
#include "game/sim/trig.h"
#include "gt2formats/exe_map.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/track.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

using namespace gt2;
using namespace gt2::verify;

namespace {

// Last checkpoint before a host crash (access violation in native port code or in the harness), printed by the
// SIGSEGV handler so that a redirected log still names the row / object / variant.
char g_checkpoint[160] = "start";
void Checkpoint(const char* row, const char* side, uint32_t object, int variant) {
    std::snprintf(g_checkpoint, sizeof(g_checkpoint), "%s: %s side, object 0x%08X, variant %d", row, side, object, variant);
    static const bool trace = std::getenv("GT2_VERIFY_TRACE") != nullptr; // print every checkpoint (crash hunting)
    if (trace) std::fprintf(stderr, "  [checkpoint] %s\n", g_checkpoint);
}
// GT2_VERIFY_SKIP=Row1,Row2: rows to leave out (e.g. to get past a row that crashes on a new dump).
bool SkipRow(const char* row) {
    static const std::string list = std::getenv("GT2_VERIFY_SKIP") ? std::string(",") + std::getenv("GT2_VERIFY_SKIP") + "," : std::string();
    const bool skip = list.find(std::string(",") + row + ",") != std::string::npos;
    if (skip) std::printf("%-10s skipped (GT2_VERIFY_SKIP)\n", row);
    return skip;
}
extern "C" void OnCrash(int) {
    std::fputs("\nCRASH (access violation) after checkpoint: ", stderr);
    std::fputs(g_checkpoint, stderr);
    std::fputs("\n", stderr);
    std::fputs("\nCRASH (access violation) after checkpoint: ", stdout);
    std::fputs(g_checkpoint, stdout);
    std::fputs("\n", stdout);
    std::_Exit(3);
}

// Body of car `object` (a guest body address) inside a RAM image.
sim::CarBody& BodyAt(uint8_t* ram, uint32_t object) { return *reinterpret_cast<sim::CarBody*>(ram + (object & 0x1FFFFF)); }

struct BinaryTest {
    const char* name;
    uint32_t address;
    int32_t (*native)(int32_t, int32_t);
};

const BinaryTest kFixedTests[] = {
    {"Mul12", 0x8007596C, sim::Mul12}, {"Mul16", 0x8007598C, sim::Mul16}, {"Mul8", 0x800759AC, sim::Mul8},
    {"Mul12Wide", 0x80075A5C, sim::Mul12Wide}, {"Mul16Wide", 0x80075A94, sim::Mul16Wide},
    {"Mul12Floor", 0x80075BF4, sim::Mul12Floor}, {"Mul16Floor", 0x80075C14, sim::Mul16Floor},
    {"ApproxLen", 0x8003C360, sim::ApproxLength},
};

struct TernaryTest {
    const char* name;
    uint32_t address;
    int32_t (*native)(int32_t, int32_t, int32_t);
    int32_t thirdMin, thirdMax; // range of the third argument
};

const TernaryTest kTernaryTests[] = {
    {"Mul12ShFl", 0x80075C54, [](int32_t a, int32_t b, int32_t e) { return sim::Mul12ShiftFloor(a, b, uint32_t(e)); }, 0, 8},
    {"Mul12ShWd", 0x80075B04, [](int32_t a, int32_t b, int32_t e) { return sim::Mul12ShiftWide(a, b, uint32_t(e)); }, 0, 8},
    {"Mul16ShWd", 0x80075B54, [](int32_t a, int32_t b, int32_t e) { return sim::Mul16ShiftWide(a, b, uint32_t(e)); }, 0, 7},
    {"Falloff", 0x8003DFDC, sim::Falloff, -0x2000, 0x2000},
};


} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::puts("usage: gt2verify <ram.bin>");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0); // rows must survive a crash of a later row when redirected to a file
    std::signal(SIGSEGV, OnCrash);
    try {
        if (std::string(argv[1]) == "--race-capture" && argc >= 5)
            return RaceCapture(argv[2], std::strtoull(argv[3], nullptr, 10), argv[4], argc >= 6 ? argv[5] : "");
        if (std::string(argv[1]) == "--camera-capture" && argc >= 6)
            return CameraCaptureCheck(argv[2], std::strtoull(argv[3], nullptr, 10), std::strtoull(argv[4], nullptr, 10), std::strtoull(argv[5], nullptr, 10),
                                      argc >= 7 ? argv[6] : "");
        Guest guest(argv[1]);
        // Cross-build dump (GT2_VERIFY_SIM_DISC=<US Simulation v1.2 disc>, argv[2] = the dump's disc): guest calls are
        // translated from Simulation addresses to the dump's build; race overlay (member 0) below the resident code.
        std::unique_ptr<CodeModules> simModules, dumpModules;
        std::unique_ptr<ProgramMap> crossMap;
        // The dump's build (gt2formats/exe_profile.h) from the SHA-1 of its disc's executable: data addresses (D) and guest
        // calls are translated by its facts (identity for US Simulation v1.2).
        if (argc >= 3) {
            const ExeProfile& profile = ProfileOf(DiscImage(argv[2]));
            SetActiveProfile(profile);
            kCarBase = D(0x800A9688u);
            if (!profile.reference) {
                gCodeAddressMap = [&profile](uint32_t address) { return profile.TryCode(address).value_or(address); };
                std::printf("dump build: %s (%s): data addresses and guest calls translated by the address profile\n", profile.name, profile.exeName);
            }
        }
        if (argc >= 3 && std::getenv("GT2_VERIFY_SIM_DISC")) {
            const DiscImage simDisc(std::getenv("GT2_VERIFY_SIM_DISC")), dumpDisc(argv[2]);
            simModules = std::make_unique<CodeModules>(CodeModules::Load(simDisc));
            dumpModules = std::make_unique<CodeModules>(CodeModules::Load(dumpDisc));
            if (simModules->exe.bytes != dumpModules->exe.bytes) {
                crossMap = std::make_unique<ProgramMap>(*simModules, *dumpModules);
                uint32_t overlayEnd = 0;
                for (const GuestImage& o : simModules->overlays) overlayEnd = std::max(overlayEnd, o.End());
                gCodeAddressMap = [map = crossMap.get(), overlayEnd](uint32_t address) {
                    const auto mapped = map->Alignment(address >= overlayEnd ? -1 : 0).Map(address);
                    return mapped.value_or(address);
                };
                std::printf("cross-build dump: %s -> %s, guest calls translated (data addresses are not)\n", simModules->exeName.c_str(),
                            dumpModules->exeName.c_str());
            }
        }
        // Dumps taken outside a race (e.g. the GT-mode menus, work/re/gtmode) have another overlay at 0x80010000 and no
        // car array: only the career rows apply there.
        if (argc >= 3) {
            DiscImage disc(argv[2]);
            if (!OverlayMemberLoaded(guest.Ram(), disc, 0, 0x80011F64u, 0x400)) {
                std::puts("dump: the race overlay (GT2.OVL member 0) is not loaded - the race rows (car array, physics, shell, AI, sound) are skipped");
                GtfsVolume vol(disc);
                const std::vector<uint8_t> pristine(guest.Ram(), guest.Ram() + Bus::kRamSize);
                std::mt19937 rng(20260919);
                int menuFailures = 0;
                if (!SkipRow("Menu")) menuFailures = VerifyMenu(guest, pristine, rng);
                if (!SkipRow("Title")) menuFailures += VerifyTitle(guest, pristine, rng);
                if (!SkipRow("Replay")) menuFailures += VerifyTitleReplay(guest, pristine, rng, &disc, &vol);
                if (!SkipRow("Transfer")) menuFailures += VerifyTitleTransfer(guest, pristine, rng);
                if (!SkipRow("ArcadeMenu")) menuFailures += VerifyArcadeMenu(guest, pristine, rng, &disc, &vol);
                if (!SkipRow("Pad")) menuFailures += VerifyPad(guest, pristine, rng, false);
                if (!SkipRow("ArcadeResults")) menuFailures += VerifyArcadeResults(guest, pristine, rng, &disc);
                if (!SkipRow("ArcadeTitle")) menuFailures += VerifyArcadeTitle(guest, pristine, rng, &disc, &vol);
                if (!SkipRow("MachineTest")) menuFailures += VerifyMachineTest(guest, pristine, rng, &disc, &vol);
                return (VerifyCareer(guest, pristine, rng, &disc, &vol) || menuFailures) ? 1 : 0;
            }
        }
        kCarCount = guest.Ram()[D(0x800AF231u) & 0x1FFFFF]; // cars in the dump's race; the attract race has 6
        if (kCarCount == 0 || kCarCount > kMaxCars) kCarCount = kMaxCars;
        std::printf("dump: course %u, %u car(s) at 0x%08X\n", unsigned(guest.Ram()[D(0x800AF230u) & 0x1FFFFF]), unsigned(kCarCount), kCarBase);
        std::mt19937 rng(20260918);
        const int32_t edges[] = {0, 1, -1, 2, -2, 4095, 4096, 4097, -4095, -4096, -4097, 0x7FFF, -0x8000, 0xFFFF, 0x10000, -0x10000,
                                 0x7FFFFFFF, int32_t(0x80000000), 0x12345678, -0x12345678, 0x00FFFFFF, -0x00FFFFFF};
        int failures = 0;
        for (const BinaryTest& test : kFixedTests) {
            size_t cases = 0, bad = 0;
            auto check = [&](int32_t a, int32_t b) {
                cases++;
                const int32_t original = int32_t(guest.Call(test.address, uint32_t(a), uint32_t(b)));
                const int32_t ours = test.native(a, b);
                if (original != ours && bad++ < 3) std::printf("    MISMATCH %s(%d, %d): original %d, ours %d\n", test.name, a, b, original, ours);
            };
            for (int32_t a : edges) for (int32_t b : edges) check(a, b);
            for (int i = 0; i < 100000; i++) {
                const int magnitude = int(rng() % 4); // small, medium, large operands
                const int32_t limit = magnitude == 0 ? 0x1000 : magnitude == 1 ? 0x100000 : 0x7FFFFFFF;
                check(int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1), int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1));
            }
            std::printf("%-10s 0x%08X  %zu cases, %zu mismatches  %s\n", test.name, test.address, cases, bad, bad ? "FAIL" : "ok");
            failures += bad ? 1 : 0;
        }
        for (const TernaryTest& test : kTernaryTests) {
            size_t cases = 0, bad = 0;
            auto check = [&](int32_t a, int32_t b, int32_t c) {
                cases++;
                const int32_t original = int32_t(guest.Call(test.address, uint32_t(a), uint32_t(b), uint32_t(c)));
                const int32_t ours = test.native(a, b, c);
                if (original != ours && bad++ < 3) std::printf("    MISMATCH %s(%d, %d, %d): original %d, ours %d\n", test.name, a, b, c, original, ours);
            };
            const uint32_t span = uint32_t(test.thirdMax - test.thirdMin) + 1u;
            for (int32_t a : edges) for (int32_t b : edges) check(a, b, test.thirdMin + int32_t(rng() % span));
            for (int i = 0; i < 100000; i++) {
                const int magnitude = int(rng() % 4);
                const int32_t limit = magnitude == 0 ? 0x1000 : magnitude == 1 ? 0x100000 : 0x7FFFFFFF;
                check(int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1), int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1),
                      test.thirdMin + int32_t(rng() % span));
            }
            Report(test.name, test.address, cases, bad, failures);
        }
        // ---- 64-bit division of the compiler runtime: (lo, hi) / (lo, hi) -> (v0, v1)
        {
            size_t cases = 0, bad = 0;
            auto check = [&](int64_t n, int64_t d) {
                if (d == 0) return;
                cases++;
                const uint32_t lo = uint32_t(guest.Call(0x80086084, uint32_t(uint64_t(n)), uint32_t(uint64_t(n) >> 32), uint32_t(uint64_t(d)), uint32_t(uint64_t(d) >> 32)));
                const int64_t original = int64_t((uint64_t(guest.V1()) << 32) | lo), ours = sim::Div64(n, d);
                if (original != ours && bad++ < 3) std::printf("    MISMATCH Div64(%lld, %lld): original %lld, ours %lld\n", n, d, original, ours);
            };
            const int64_t edges64[] = {0, 1, -1, 2, -2, 0x7FFFFFFF, int64_t(0x80000000), -int64_t(0x80000000), 0xFFFFFFFFll, 0x100000000ll, -0x100000000ll,
                                       0x7FFFFFFFFFFFFFFFll, int64_t(0x8000000000000000ull), 0x123456789ABCDll, -0x123456789ABCDll, 0x1000, -0x1000};
            for (int64_t n : edges64) for (int64_t d : edges64) check(n, d);
            for (int i = 0; i < 100000; i++) {
                auto random64 = [&] {
                    const int bits = 1 + int(rng() % 63);
                    const uint64_t magnitude = ((uint64_t(rng()) << 32) | rng()) & ((uint64_t(1) << bits) - 1u);
                    return (rng() & 1) ? -int64_t(magnitude) : int64_t(magnitude);
                };
                check(random64(), random64());
            }
            Report("Div64", 0x80086084u, cases, bad, failures);
        }
        // ---- (a << (12 + extra)) / b: the 64-bit shift of 0x80075E90, then the runtime division above
        {
            size_t cases = 0, bad = 0;
            auto check = [&](int32_t a, int32_t b, uint32_t extra) {
                if (b == 0) return; // the runtime division traps on a zero divisor
                cases++;
                const int32_t original = int32_t(guest.Call(0x80075E90, uint32_t(a), uint32_t(b), extra));
                const int32_t ours = sim::Div12Shift(a, b, extra);
                if (original != ours && bad++ < 3) std::printf("    MISMATCH Div12Shift(%d, %d, %u): original %d, ours %d\n", a, b, extra, original, ours);
            };
            for (int32_t a : edges) for (int32_t b : edges) check(a, b, uint32_t(rng() % 17));
            for (int i = 0; i < 100000; i++) {
                const int magnitude = int(rng() % 4);
                const int32_t limit = magnitude == 0 ? 0x1000 : magnitude == 1 ? 0x100000 : 0x7FFFFFFF;
                check(int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1), int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1),
                      uint32_t(rng() % 17));
            }
            Report("Div12Shift", 0x80075E90u, cases, bad, failures);
        }
        // ---- curve lookup on tables placed in the guest's free RAM
        {
            constexpr uint32_t kTable = 0x801E8000u, kXs = 0x801E8010u, kYs = 0x801E8810u;
            size_t cases = 0, bad = 0;
            for (int variant = 0; variant < 3000; variant++) {
                const uint32_t count = 1u + uint32_t(rng() % 40);
                std::vector<int32_t> xs(count), ys(count);
                int32_t x = int32_t(rng() % 2000) - 1000;
                for (uint32_t i = 0; i < count; i++) {
                    x += int32_t(rng() % 3000) + (rng() % 4 == 0 ? 0 : 1); // ascending, occasionally repeated
                    xs[i] = x;
                    ys[i] = int32_t(rng() % 200000) - 100000;
                }
                uint8_t* ram = guest.Ram();
                const uint16_t n = uint16_t(count);
                std::memcpy(ram + (kTable & 0x1FFFFF), &n, 2);
                std::memcpy(ram + (kTable & 0x1FFFFF) + 4, &kXs, 4);
                std::memcpy(ram + (kTable & 0x1FFFFF) + 8, &kYs, 4);
                std::memcpy(ram + (kXs & 0x1FFFFF), xs.data(), count * 4);
                std::memcpy(ram + (kYs & 0x1FFFFF), ys.data(), count * 4);
                for (int q = 0; q < 20; q++) {
                    const int32_t v = q < 4 ? xs[rng() % count] + int32_t(q) - 2 : xs[0] - 500 + int32_t(rng() % uint32_t(xs[count - 1] - xs[0] + 1000));
                    cases++;
                    const int32_t original = int32_t(guest.Call(0x80075D2C, kTable, uint32_t(v)));
                    const int32_t ours = sim::Interpolate(xs.data(), ys.data(), count, v);
                    if (original != ours && bad++ < 3) std::printf("    MISMATCH Interpolate(count %u, %d): original %d, ours %d\n", count, v, original, ours);
                }
            }
            Report("Interp", 0x80075D2Cu, cases, bad, failures);
        }
        // ---- trigonometric tables: ours are generated, the original's are data in the executable
        {
            std::vector<uint8_t> pristineForTables(guest.Ram(), guest.Ram() + Bus::kRamSize);
            auto original = [&](uint32_t table, size_t i) { int16_t v; std::memcpy(&v, &pristineForTables[(table & 0x1FFFFF) + i * 2], 2); return v; };
            size_t sinBad = 0, cosBad = 0;
            for (size_t i = 0; i < 4096; i++) {
                sinBad += original(D(0x80093150), i) != sim::SinTable()[i];
                cosBad += original(D(0x80093950), i) != sim::CosTable()[i];
            }
            std::printf("SinTable   0x80093150  4096 entries, %zu mismatches  %s\n", sinBad, sinBad ? "FAIL" : "ok");
            std::printf("CosTable   0x80093950  4096 entries, %zu mismatches  %s\n", cosBad, cosBad ? "FAIL" : "ok");
            failures += (sinBad ? 1 : 0) + (cosBad ? 1 : 0);
            // The library's arc tangent table is not reproducible by a rounding rule (see trig.h); the generated
            // one may differ by one unit. Report how far it is, and fail only on the octant constants or a
            // difference of more than one unit.
            size_t atanOff = 0, atanBad = 0;
            for (size_t i = 0; i <= 4096; i++) {
                const int diff = original(D(0x800A4AC8), i) - sim::AtanTable()[i];
                atanOff += diff != 0;
                atanBad += diff > 1 || diff < -1;
            }
            for (size_t i = 0; i < 8; i++) {
                atanBad += int8_t(pristineForTables[(D(0x800A2A8Cu) & 0x1FFFFF) + i]) != sim::kAtanOctantFlip[i];
                atanBad += original(D(0x800A2A94), i) != sim::kAtanOctantOffset[i];
            }
            std::printf("AtanTable  0x800A4AC8  4097 entries, %zu differ by one unit from the generated table (library data), %zu worse  %s\n", atanOff,
                        atanBad, atanBad ? "FAIL" : "ok");
            failures += atanBad ? 1 : 0;
            // From here on the ports use the original's table so that everything above the arc tangent verifies exactly.
            static std::vector<int16_t> originalAtan(4097);
            for (size_t i = 0; i <= 4096; i++) originalAtan[i] = original(D(0x800A4AC8), i);
            sim::AtanTableOverride() = originalAtan.data();
            size_t cases = 0, bad = 0;
            for (int i = 0; i < 100000; i++) {
                const int32_t limit = i % 3 == 0 ? 0x1000 : i % 3 == 1 ? 0x100000 : 0x7FFFFFFF;
                const int32_t y = int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1), x = int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1);
                cases++;
                const int32_t o = int32_t(guest.Call(0x80081AF0, uint32_t(y), uint32_t(x))), ours = sim::Atan2(y, x);
                if (o != ours && bad++ < 3) std::printf("    MISMATCH Atan2(%d, %d): original %d, ours %d\n", y, x, o, ours);
            }
            Report("Atan2*", 0x80081AF0u, cases, bad, failures); // * = with the original's table
        }

        // ---- stateful routines on the six car bodies of the dump
        {
            const std::vector<uint8_t> pristine(guest.Ram(), guest.Ram() + Bus::kRamSize);
            std::vector<uint32_t> bodies;
            for (uint32_t car = 0; car < kCarCount; car++) bodies.push_back(D(0x800A9688u) + car * 0xB40u + 0x2Cu);

            StatefulResult r = VerifyStateful(
                guest, pristine, 0x80041AE8, bodies, 2 * 111,
                [](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                    sim::CarBody& b = BodyAt(ram, object);
                    b.footprintSet = uint8_t(variant & 1);
                    b.heading = uint16_t((variant / 2) * 37);
                },
                [](uint8_t* ram, uint8_t*, uint32_t object) { sim::UpdateFootprint(BodyAt(ram, object)); });
            std::printf("%-10s 0x%08X  %zu cases, %zu mismatches  %s\n", "UpdateFootprint", 0x80041AE8u, r.cases, r.mismatches, r.mismatches ? "FAIL" : "ok");
            failures += r.mismatches ? 1 : 0;

            auto ramS32 = [&](uint32_t address) { int32_t v; std::memcpy(&v, &pristine[address & 0x1FFFFF], 4); return v; };
            sim::StepGlobals globals;
            globals.frameTime = ramS32(D(0x801C856Cu));
            globals.rate = ramS32(D(0x801C8570u));
            globals.draftDragFloor = ramS32(D(0x80046EF4u));

            r = VerifyStateful(
                guest, pristine, 0x8004232C, bodies, 200,
                [&](uint8_t* ram, uint8_t*, uint32_t object, size_t variant) {
                    BodyAt(ram, object).timeScale = variant == 0 ? int16_t(0x1000) : int16_t(int32_t(rng() % 0x3000) - 0x800);
                },
                [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::SetStepTime(BodyAt(ram, object), globals); });
            Report("StepTime", 0x8004232Cu, r.cases, r.mismatches, failures);

            r = VerifyStateful(
                guest, pristine, 0x8003DAA8, bodies, 400,
                [&](uint8_t* ram, uint8_t* scratch, uint32_t object, size_t variant) {
                    sim::CarBody& b = BodyAt(ram, object);
                    if (variant % 4 != 0) { // keep the dump's values for a quarter of the cases
                        const int32_t speedLimit = variant % 4 == 1 ? 0x10000 : variant % 4 == 2 ? 0x80000 : 0x7FFFFFFF;
                        b.forwardSpeed = int32_t(rng() % uint32_t(speedLimit)) * ((rng() & 1) ? 1 : -1);
                        b.draftInput = (rng() & 1) ? 0 : int16_t(rng() % 0x2000);
                        b.draftBlend = int16_t(rng() % 0x1200);
                        b.stepTime = int16_t(rng() % 0x2000) - 0x100;
                        b.dragCoefficient = int32_t(rng() % 0x20000);
                        b.downforce[0] = int32_t(rng() % 0x20000);
                        b.downforce[1] = int32_t(rng() % 0x20000);
                    }
                    const int32_t dt = b.stepTime; // the caller of the original passes the step through the scratchpad
                    std::memcpy(scratch, &dt, 4);
                },
                [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::UpdateAero(BodyAt(ram, object), globals); });
            Report("Aero", 0x8003DAA8u, r.cases, r.mismatches, failures);

            constexpr uint32_t kPad = 0x801E8000u;
            uint16_t pad[4] = {};
            r = VerifyStateful(
                guest, pristine, 0x8003E020, bodies, 300,
                [&](uint8_t* ram, uint8_t*, uint32_t object, size_t) {
                    BodyAt(ram, object).inputFlag6FD = uint8_t(rng() & 1);
                    pad[0] = uint16_t(rng() & 3);
                    pad[1] = uint16_t(int16_t(int32_t(rng() % 0x2400) - 0x1200));
                    pad[2] = 0;
                    pad[3] = uint16_t(rng() % 3 == 0 ? 0x1000 : rng() % 2 == 0 ? 0 : rng() % 0x1100);
                    if (rng() % 4 == 0) pad[1] = 0;
                    std::memcpy(ram + (kPad & 0x1FFFFF), pad, sizeof(pad));
                },
                [&](uint8_t* ram, uint8_t*, uint32_t object) { sim::MapPadInput(BodyAt(ram, object), pad); }, kPad);
            Report("PadInput", 0x8003E020u, r.cases, r.mismatches, failures);

            // Displacement of all six cars in one call; the original writes the scratchpad, ours an array.
            {
                size_t cases = 0, bad = 0;
                for (int variant = 0; variant < 300; variant++) {
                    std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                    for (uint32_t car = 0; car < kCarCount; car++) {
                        auto* b = reinterpret_cast<sim::CarBody*>(guest.Ram() + (bodies[car] & 0x1FFFFF));
                        if (variant == 0) continue;
                        const int32_t limit = variant % 3 == 0 ? 0x10000 : variant % 3 == 1 ? 0x400000 : 0x7FFFFFFF;
                        for (int i = 0; i < 3; i++) b->velocity[i] = int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1);
                        b->yawRate = int32_t(rng() % uint32_t(limit)) * ((rng() & 1) ? 1 : -1);
                        b->stepTime = int16_t(rng() % 0x2000) - 0x100;
                    }
                    guest.Call(0x80030330, D(0x800A9688u), kCarCount);
                    for (uint32_t car = 0; car < kCarCount; car++) {
                        int32_t ours[4], original[4];
                        sim::ComputeDisplacement(*reinterpret_cast<sim::CarBody*>(guest.Ram() + (bodies[car] & 0x1FFFFF)), ours);
                        std::memcpy(original, guest.Scratch() + 0xB4 + car * 0x10, sizeof(original));
                        cases++;
                        if (std::memcmp(ours, original, sizeof(ours)) != 0 && bad++ < 3)
                            std::printf("    MISMATCH Displacement car %u variant %d: original %d %d %d %d, ours %d %d %d %d\n", car, variant, original[0], original[1],
                                        original[2], original[3], ours[0], ours[1], ours[2], ours[3]);
                    }
                }
                Report("Displace", 0x80030330u, cases, bad, failures);
            }
        }
        // ---- course wall collision: original on the guest's course structures vs ours on the parsed .tro
        if (argc >= 4) {
            DiscImage disc(argv[2]);
            GtfsVolume vol(disc);
            const Track track = ParseTrack(vol.Read(std::string("crsobj/") + argv[3] + ".tro"));
            const std::vector<uint8_t> pristine(guest.Ram(), guest.Ram() + Bus::kRamSize);
            auto ramU32 = [&](uint32_t address) { uint32_t v; std::memcpy(&v, &pristine[address & 0x1FFFFF], 4); return v; };
            // 0x800A9520 is the u16 hold counter (lhu in 0x80033E6C / 0x80034320); 0x800A9522 next to it counts after the finish
            auto holdActive = [&] { uint16_t v; std::memcpy(&v, &pristine[D(0x800A9520u) & 0x1FFFFF], 2); return v != 0; };
            const uint32_t chunkTable = ramU32(D(0x800A9500u) + 0xB544u);
            constexpr uint32_t kSegments = 0x801E8000u;
            size_t cases = 0, mismatches = 0, hits = 0;
            for (uint32_t c = 0; c < track.chunks.size(); c++) {
                const uint32_t guestChunk = ramU32(chunkTable + 0xC + c * 4);
                const TrackChunk& chunk = track.chunks[c];
                for (int variant = 0; variant < 40; variant++) {
                    sim::SweepSegment segments[4]{};
                    for (auto& s : segments) {
                        double sx = chunk.cellOrigin[0] / 65536.0 - 8 + (rng() % 8000) / 100.0, sz = chunk.cellOrigin[1] / 65536.0 - 8 + (rng() % 8000) / 100.0;
                        double dx = (int(rng() % 800) - 400) / 100.0, dz = (int(rng() % 800) - 400) / 100.0;
                        if (!chunk.boundaries.empty() && (rng() & 1)) { // half of the sweeps start near a wall and cross it
                            const TrackBoundary& wall = chunk.boundaries[rng() % chunk.boundaries.size()];
                            const TrackVertex& a = chunk.road.vertices[wall.vertexA];
                            const TrackVertex& b = chunk.road.vertices[wall.vertexB];
                            const double t = (rng() % 100) / 100.0;
                            const double mx = chunk.cellOrigin[0] / 65536.0 + (a.x + (b.x - a.x) * t) / 64.0, mz = chunk.cellOrigin[1] / 65536.0 + (a.y + (b.y - a.y) * t) / 64.0;
                            const double nx = -(b.y - a.y) / 64.0, nz = (b.x - a.x) / 64.0, nl = std::sqrt(nx * nx + nz * nz) + 1e-9;
                            const double along = (int(rng() % 200) - 100) / 100.0, across = 0.5 + (rng() % 300) / 100.0, sign = (rng() & 1) ? 1.0 : -1.0;
                            sx = mx - sign * nx / nl * across * 0.5 + nz / nl * along;
                            sz = mz - sign * nz / nl * across * 0.5 - nx / nl * along;
                            dx = sign * nx / nl * across;
                            dz = sign * nz / nl * across;
                        }
                        s.x0 = int32_t(sx * 65536); s.y0 = int32_t(sz * 65536); s.x1 = int32_t((sx + dx) * 65536); s.y1 = int32_t((sz + dz) * 65536);
                        s.fraction = 0x1000;
                    }
                    std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                    std::memcpy(guest.Ram() + (kSegments & 0x1FFFFF), segments, sizeof(segments));
                    guest.Call(0x80027FC4, guestChunk, kSegments);
                    sim::TestSegmentsAgainstChunk(chunk, segments);
                    cases++;
                    for (const auto& s : segments) hits += s.fraction != 0x1000;
                    if (std::memcmp(guest.Ram() + (kSegments & 0x1FFFFF), segments, sizeof(segments)) != 0 && mismatches++ < 3) {
                        const auto* g = reinterpret_cast<const sim::SweepSegment*>(guest.Ram() + (kSegments & 0x1FFFFF));
                        for (int i = 0; i < 4; i++)
                            if (std::memcmp(&g[i], &segments[i], sizeof(sim::SweepSegment)) != 0)
                                std::printf("    MISMATCH chunk %u variant %d segment %d: original fraction %d normal %d,%d; ours %d normal %d,%d\n", c, variant, i,
                                            g[i].fraction, g[i].hitNormal0, g[i].hitNormal1, segments[i].fraction, segments[i].hitNormal0, segments[i].hitNormal1);
                    }
                }
            }
            std::printf("%-10s 0x80027FC4  %zu cases (%zu segment hits), %zu mismatches  %s\n", "WallTest", cases, hits, mismatches, mismatches ? "FAIL" : "ok");
            failures += mismatches ? 1 : 0;

            // Course sweep: same segments (sim-plane y = -course z), walked over the chunk chain.
            cases = mismatches = hits = 0;
            if (!SkipRow("CourseSweep"))
            for (uint32_t c = 0; c < track.chunks.size(); c++) {
                const TrackChunk& chunk = track.chunks[c];
                for (int variant = 0; variant < 20; variant++) {
                    sim::SweepSegment segments[4]{};
                    Checkpoint("CourseSweep", "prepare", c, variant);
                    for (auto& s : segments) {
                        if (chunk.boundaries.empty()) { // no wall in this chunk (license courses have such chunks): a random sweep near it
                            const double sx = chunk.cellOrigin[0] / 65536.0 - 8 + (rng() % 8000) / 100.0, sz = chunk.cellOrigin[1] / 65536.0 - 8 + (rng() % 8000) / 100.0;
                            const double dx = (int(rng() % 800) - 400) / 100.0, dz = (int(rng() % 800) - 400) / 100.0;
                            s.x0 = int32_t(sx * 65536); s.y0 = int32_t(-sz * 65536);
                            s.x1 = int32_t((sx + dx) * 65536); s.y1 = int32_t(-(sz + dz) * 65536);
                            s.fraction = 0x1000;
                            continue;
                        }
                        const TrackBoundary& wall = chunk.boundaries[rng() % chunk.boundaries.size()];
                        const TrackVertex& a = chunk.road.vertices[wall.vertexA];
                        const TrackVertex& b = chunk.road.vertices[wall.vertexB];
                        const double t = (rng() % 100) / 100.0;
                        const double mx = chunk.cellOrigin[0] / 65536.0 + (a.x + (b.x - a.x) * t) / 64.0, mz = chunk.cellOrigin[1] / 65536.0 + (a.y + (b.y - a.y) * t) / 64.0;
                        const double nx = -(b.y - a.y) / 64.0, nz = (b.x - a.x) / 64.0, nl = std::sqrt(nx * nx + nz * nz) + 1e-9;
                        const double along = (int(rng() % 4000) - 2000) / 100.0, across = 0.5 + (rng() % 600) / 100.0, sign = (rng() & 1) ? 1.0 : -1.0;
                        const double sx = mx - sign * nx / nl * across * 0.5 + nz / nl * along, sz = mz - sign * nz / nl * across * 0.5 - nx / nl * along;
                        s.x0 = int32_t(sx * 65536); s.y0 = int32_t(-sz * 65536);
                        s.x1 = int32_t((sx + sign * nx / nl * across) * 65536); s.y1 = int32_t(-(sz + sign * nz / nl * across) * 65536);
                        s.fraction = 0x1000;
                    }
                    std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                    std::memcpy(guest.Ram() + (kSegments & 0x1FFFFF), segments, sizeof(segments));
                    Checkpoint("CourseSweep", "guest", c, variant);
                    guest.Call(0x80028968, D(0x800A9500u), c, kSegments);
                    Checkpoint("CourseSweep", "native", c, variant);
                    sim::SweepAgainstCourse(track, c, segments);
                    cases++;
                    for (const auto& s : segments) hits += s.fraction != 0x1000;
                    if (std::memcmp(guest.Ram() + (kSegments & 0x1FFFFF), segments, sizeof(segments)) != 0 && mismatches++ < 3) {
                        const auto* g = reinterpret_cast<const sim::SweepSegment*>(guest.Ram() + (kSegments & 0x1FFFFF));
                        for (int i = 0; i < 4; i++)
                            if (std::memcmp(&g[i], &segments[i], sizeof(sim::SweepSegment)) != 0)
                                std::printf("    MISMATCH chunk %u variant %d segment %d: original fraction %d normal %d,%d; ours %d normal %d,%d\n", c, variant, i,
                                            g[i].fraction, g[i].hitNormal0, g[i].hitNormal1, segments[i].fraction, segments[i].hitNormal0, segments[i].hitNormal1);
                    }
                }
            }
            if (cases) std::printf("%-10s 0x80028968  %zu cases (%zu segment hits), %zu mismatches  %s\n", "CourseSweep", cases, hits, mismatches, mismatches ? "FAIL" : "ok");
            failures += mismatches ? 1 : 0;

            // MoveBody: the whole step on the six bodies with random displacements (some large enough to hit walls).
            if (!SkipRow("MoveBody")) {
                const bool collisionDisabled = holdActive();
                const int32_t rate = int32_t(ramU32(D(0x801C8570u)));
                constexpr uint32_t kDelta = 0x801E8100u, kRemaining = 0x801E8110u;
                std::vector<uint32_t> bodies;
                for (uint32_t car = 0; car < kCarCount; car++) bodies.push_back(D(0x800A9688u) + car * 0xB40u + 0x2Cu);
                std::vector<uint8_t> ours(Bus::kRamSize);
                size_t moveCases = 0, moveMismatches = 0, moveHits = 0;
                for (uint32_t body : bodies)
                    for (int variant = 0; variant < 300; variant++) {
                        const int mode = variant % 3;
                        static constexpr int32_t kScales[4] = {1500, 20000, 60000, 140000}; // 0.37 m .. 34 m
                        const int32_t scale = kScales[(variant / 3) % 4];
                        int32_t delta[4];
                        for (int i = 0; i < 3; i++) delta[i] = int32_t(rng() % uint32_t(2 * scale)) - scale;
                        delta[3] = int32_t(rng() % 200) - 100;
                        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                        std::memcpy(guest.Ram() + (kDelta & 0x1FFFFF), delta, sizeof(delta));
                        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
                        Checkpoint("MoveBody", "guest", body, variant);
                        const uint32_t original = guest.Call(0x80033E6C, body, kDelta, kRemaining, uint32_t(mode));
                        int32_t remaining = 0;
                        Checkpoint("MoveBody", "native", body, variant);
                        const uint32_t mask = sim::MoveBody(track, *reinterpret_cast<sim::CarBody*>(ours.data() + (body & 0x1FFFFF)), delta, remaining, mode, rate,
                                                            collisionDisabled);
                        Checkpoint("MoveBody", "compare", body, variant);
                        std::memcpy(ours.data() + (kRemaining & 0x1FFFFF), &remaining, 4);
                        moveCases++;
                        moveHits += mask != 0;
                        const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
                        const bool equal = original == mask && std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                                           std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0;
                        if (!equal && moveMismatches++ < 3) {
                            uint32_t first = 0;
                            for (uint32_t i = 0; i < stackLow; i++) if (ours[i] != guest.Ram()[i]) { first = i; break; }
                            std::printf("    MISMATCH body %08X variant %d mode %d: mask original %u ours %u, first differing byte 0x%08X (body + 0x%X)\n", body,
                                        variant, mode, original, mask, 0x80000000u + first, first - (body & 0x1FFFFF));
                        }
                    }
                std::printf("%-10s 0x80033E6C  %zu cases (%zu with wall hits), %zu mismatches  %s\n", "MoveBody", moveCases, moveHits, moveMismatches,
                            moveMismatches ? "FAIL" : "ok");
                failures += moveMismatches ? 1 : 0;
            }
            // MovePass: the whole move pass of the step (move, wall response, scrape decay, corner damage) on all cars.
            if (!SkipRow("MovePass")) {
                {
                    sim::MoveContext context;
                    context.track = &track;
                    context.globals.frameTime = int32_t(ramU32(D(0x801C856Cu)));
                    context.globals.rate = int32_t(ramU32(D(0x801C8570u)));
                    context.collisionDisabled = holdActive();
                    { // the dump's course: the wall response differs on dirt courses (first seen on the Arcade rally dump)
                        uint16_t flags = 0;
                        std::memcpy(&flags, &pristine[(D(0x801E18E8u) + pristine[D(0x800AF230u) & 0x1FFFFF] * 0x18u + 8u) & 0x1FFFFF], 2);
                        context.dirtCourse = (flags & 4) != 0;
                    }
                    constexpr uint32_t kDelta = 0x801E8100u;
                    for (int run = 0; run < 4; run++) {
                        const int mode = run & 1;
                        const int32_t remaining = run < 2 ? 0x600 : 0x1000;
                        std::vector<uint32_t> bodyList;
                        for (uint32_t car = 0; car < kCarCount; car++) bodyList.push_back(kCarBase + car * kCarStride + kBodyOffset);
                        StatefulResult r = VerifyStateful(
                            guest, pristine, 0x800340A4, bodyList, 100,
                            [&](uint8_t* ram, uint8_t* scratch, uint32_t object, size_t variant) {
                                const int32_t stepTime = 2184;
                                std::memcpy(scratch, &stepTime, 4);
                                sim::CarBody& b = BodyAt(ram, object);
                                int32_t delta[4] = {int32_t(rng() % 20000) - 10000, int32_t(rng() % 20000) - 10000, int32_t(rng() % 2000) - 1000, int32_t(rng() % 200) - 100};
                                std::memcpy(ram + (kDelta & 0x1FFFFF), delta, sizeof(delta));
                                if (variant % 4 == 0) return;
                                const int32_t scale = variant % 4 == 1 ? 0x10000 : variant % 4 == 2 ? 0x100000 : 0x1000000;
                                for (int i = 0; i < 3; i++) b.velocity[i] = int32_t(rng() % uint32_t(2 * scale)) - scale;
                                b.hitNormal0 = int16_t(rng() % 0x2000) - 0x1000;
                                b.hitNormal1 = int16_t(rng() % 0x2000) - 0x1000;
                                b.wallScrape = int16_t(rng() % 0x3000);
                            },
                            [&](uint8_t* ram, uint8_t*, uint32_t object) {
                                sim::RespondToWall(context, BodyAt(ram, object), reinterpret_cast<int32_t*>(ram + (kDelta & 0x1FFFFF)), remaining, mode);
                            },
                            kDelta, uint32_t(remaining), uint32_t(mode), ScratchIgnore{0, 0xB4}); // +0: the original mirrors stepTime there
                        std::printf("%-10s 0x800340A4  mode %d remaining 0x%X: %zu cases, %zu mismatches  %s\n", "WallResp", mode, remaining, r.cases, r.mismatches,
                                    r.mismatches ? "FAIL" : "ok");
                        failures += r.mismatches ? 1 : 0;
                    }
                }
                sim::MoveContext context;
                context.track = &track;
                context.globals.frameTime = int32_t(ramU32(D(0x801C856Cu)));
                context.globals.rate = int32_t(ramU32(D(0x801C8570u)));
                context.collisionDisabled = holdActive();
                const uint32_t course = pristine[D(0x800AF230u) & 0x1FFFFF];
                uint16_t courseFlags = 0;
                std::memcpy(&courseFlags, &pristine[(D(0x801E18E8u) + course * 0x18u + 8u) & 0x1FFFFF], 2);
                context.dirtCourse = (courseFlags & 4) != 0;
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                context.controlClass = int(guest.Call(0x800419E8, kCarBase + kBodyOffset));
                std::printf("           (course %u, dirt %d, control class %d)\n", course, context.dirtCourse, context.controlClass);
                size_t passHits = 0;
                StatefulResult r = VerifyStateful(
                    guest, pristine, 0x80034320, {kCarBase}, 400,
                    [&](uint8_t* ram, uint8_t* scratch, uint32_t, size_t variant) {
                        const int32_t stepTime = 2184;
                        std::memcpy(scratch, &stepTime, 4);
                        for (uint32_t car = 0; car < kCarCount; car++) {
                            sim::CarBody& b = BodyAt(ram, kCarBase + car * kCarStride + kBodyOffset);
                            static constexpr int32_t kScales[4] = {1500, 20000, 60000, 140000};
                            const int32_t scale = kScales[(variant / 3) % 4];
                            int32_t delta[4];
                            for (int i = 0; i < 3; i++) delta[i] = int32_t(rng() % uint32_t(2 * scale)) - scale;
                            delta[3] = int32_t(rng() % 200) - 100;
                            std::memcpy(scratch + 0xB4 + car * 0x10, delta, sizeof(delta));
                            if (variant % 4 == 0) continue;
                            for (int i = 0; i < 3; i++) b.velocity[i] = int32_t(rng() % uint32_t(2 * scale * 30)) - scale * 30;
                            b.yawRate = int32_t(rng() % 0x200000) - 0x100000;
                            b.wallScrape = int16_t(rng() % 0x3000);
                            b.wallImpact = int16_t(rng() % 0x800);
                            for (auto& w : b.wheels) w.damage = uint8_t(rng() % 256);
                        }
                    },
                    [&](uint8_t* ram, uint8_t* scratch, uint32_t) {
                        auto* cars = reinterpret_cast<sim::Car*>(ram + (kCarBase & 0x1FFFFF));
                        sim::MovePass(context, cars, int(kCarCount), reinterpret_cast<int32_t(*)[4]>(scratch + 0xB4));
                        for (uint32_t car = 0; car < kCarCount; car++) passHits += cars[car].body.wallHitMask != 0;
                    },
                    kCarCount, 0, 0, ScratchIgnore{0, 0xB4});
                std::printf("%-10s 0x80034320  %zu cases (%zu car-steps with wall hits), %zu mismatches  %s\n", "MovePass", r.cases, passHits, r.mismatches,
                            r.mismatches ? "FAIL" : "ok");
                failures += r.mismatches ? 1 : 0;
            }
        }
        // ---- subsystems ported in their own files (verify_*.cpp)
        {
            const std::vector<uint8_t> pristine(guest.Ram(), guest.Ram() + Bus::kRamSize);
            if (!SkipRow("DriveShafts")) failures += VerifyDriveShafts(guest, pristine, rng);
            if (!SkipRow("Tyres")) failures += VerifyTyres(guest, pristine, rng);
            if (!SkipRow("Drivetrain")) failures += VerifyDrivetrain(guest, pristine, rng);
            if (!SkipRow("Setup")) failures += VerifySetup(guest, pristine, rng);
            if (!SkipRow("Sound")) failures += VerifySound(guest, pristine, rng);
            if (argc >= 4) {
                DiscImage disc(argv[2]);
                GtfsVolume vol(disc);
                const std::vector<uint8_t> tro = vol.Read(std::string("crsobj/") + argv[3] + ".tro");
                const Track track = ParseTrack(tro);
                if (!SkipRow("Params")) failures += VerifyParams(guest, pristine, rng, &vol);
                if (!SkipRow("Ground")) failures += VerifyGround(guest, pristine, rng, &track);
                if (!SkipRow("Contact")) failures += VerifyContact(guest, pristine, rng, &track);
                if (!SkipRow("Ai")) failures += VerifyAi(guest, pristine, rng, &track);
                if (!SkipRow("Core")) failures += VerifyCore(guest, pristine, rng, &track);
                if (!SkipRow("Race")) failures += VerifyRace(guest, pristine, rng, &track, tro);
                if (!SkipRow("Grid")) failures += VerifyGrid(guest, pristine, rng, &track, tro, &vol);
                if (!SkipRow("Shell")) failures += VerifyShell(guest, pristine, rng, &track);
                if (!SkipRow("Data")) failures += VerifyData(guest, pristine, rng, &vol, &disc);
                if (!SkipRow("License")) failures += VerifyLicense(guest, pristine, rng, &vol, argv[3]);
                if (!SkipRow("Music")) failures += VerifyMusic(guest, pristine, rng, &disc, &vol, argv[2]);
                if (!SkipRow("Career")) failures += VerifyCareer(guest, pristine, rng, &disc, &vol);
                if (!SkipRow("Menu")) failures += VerifyMenu(guest, pristine, rng);
                if (!SkipRow("Title")) failures += VerifyTitle(guest, pristine, rng);
                if (!SkipRow("Replay")) failures += VerifyTitleReplay(guest, pristine, rng, &disc, &vol);
                if (!SkipRow("Camera")) failures += VerifyCamera(guest, pristine, rng, &track, &vol, &disc, argv[3]);
                if (!SkipRow("Mirror")) failures += VerifyMirror(guest, pristine, rng);
                if (!SkipRow("Pad")) failures += VerifyPad(guest, pristine, rng, true);
                if (!SkipRow("Wheels")) failures += VerifyWheels(guest, pristine, rng, &disc);
                if (!SkipRow("Battle")) failures += VerifyBattle(guest, pristine, rng);
                if (!SkipRow("MachineTest")) failures += VerifyMachineTest(guest, pristine, rng, &disc, &vol);
            } else {
                failures += VerifyGround(guest, pristine, rng, nullptr);
                failures += VerifyContact(guest, pristine, rng, nullptr);
                failures += VerifyCore(guest, pristine, rng, nullptr);
                failures += VerifyRace(guest, pristine, rng, nullptr);
                if (!SkipRow("License")) failures += VerifyLicense(guest, pristine, rng, nullptr, std::string());
                if (!SkipRow("Music")) failures += VerifyMusic(guest, pristine, rng, nullptr, nullptr, std::string());
                if (!SkipRow("Camera")) failures += VerifyCamera(guest, pristine, rng, nullptr, nullptr, nullptr, std::string());
            }
        }
        return failures ? 1 : 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
