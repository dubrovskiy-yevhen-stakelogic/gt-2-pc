// What an arcade race writes into the career (src/game/arcade/arcade_results.*) against the original's EXE routine in an arcade
// RAM image (US Arcade v1.1; the EXE is resident in every dump of that disc). ARCADE v1.1 addresses.
//   ArcadeWin 0x8005DC64(flags, record, bits): random flag bytes (counter bits 3..5 included), records -3..40, the level bits
//     of 0x800272DC and random bytes - the whole RAM compared (the guest stack excepted).
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/arcade/arcade_results.h"
#include "gt2formats/exe_profile.h"
#include "gt2vfs/disc_image.h"
#include "guest.h"

namespace gt2::verify {

int VerifyArcadeResults(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc) {
    constexpr uint32_t kSetFlags = 0x8005DC64u, kFlags = 0x801C9340u + 0xB8u; // the career block's course flag bytes
    // The routine's first words (bltz a1 / slti v0, a1, 32 / beqz v0 / addu a0, a0, a1): the arcade EXE only.
    const uint32_t expected[4] = {0x04A0001Au, 0x28A20020u, 0x10400018u, 0x00852021u};
    bool present = disc && ProfileOf(*disc).arcade;
    for (uint32_t k = 0; present && k < 4; k++) {
        uint32_t w;
        std::memcpy(&w, pristine.data() + ((kSetFlags + k * 4) & 0x1FFFFF), 4);
        present = w == expected[k];
    }
    if (!present) {
        std::puts("ArcadeWin skipped (the dump is not of the US Arcade v1.1 disc)");
        return 0;
    }
    const auto savedMap = gCodeAddressMap; // the dump's own EXE addresses
    gCodeAddressMap = nullptr;
    int failures = 0;
    size_t cases = 0, bad = 0;
    std::vector<uint8_t> ours(Bus::kRamSize);
    const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
    for (size_t i = 0; i < 400; i++) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        uint8_t* ram = guest.Ram();
        for (uint32_t k = 0; k < 0x40; k++) {
            const uint32_t r = rng() % 4;
            ram[(kFlags + k) & 0x1FFFFF] = uint8_t(r == 0 ? rng() : r == 1 ? (rng() % 6) << 3 | (rng() % 4) : 0);
        }
        const int32_t record = int32_t(rng() % 44) - 3;
        const uint32_t pick = rng() % 6;
        const int32_t bits = pick < 3 ? int32_t(1u << pick) : pick == 3 ? 2 : int32_t(int8_t(rng()));
        std::memcpy(ours.data(), ram, Bus::kRamSize);
        try {
            guest.CallWithBios(kSetFlags, kFlags, uint32_t(record), uint32_t(bits));
        } catch (const std::exception& e) {
            std::printf("    ArcadeWin case %zu: original trapped (%s)\n", i, e.what());
            bad++;
            continue;
        }
        arcade::SetCourseWinFlags(std::span<uint8_t>(ours.data() + (kFlags & 0x1FFFFF), 0x40), record, bits);
        cases++;
        size_t differ = 0;
        uint32_t first = 0;
        for (uint32_t a = 0; a < Bus::kRamSize; a++) {
            if (a >= stackLow && a < stackHigh) continue;
            if (ours[a] != ram[a] && differ++ == 0) first = a;
        }
        if (differ) {
            if (bad < 4)
                std::printf("    MISMATCH ArcadeWin case %zu (record %d, bits %d): %zu bytes differ, first 0x%08X original %02X ours %02X\n", i, record, bits, differ,
                            0x80000000u + first, ram[first], ours[first]);
            bad++;
        }
    }
    Report("ArcadeWin", kSetFlags, cases, bad, failures);
    gCodeAddressMap = savedMap;
    return failures;
}

} // namespace gt2::verify
