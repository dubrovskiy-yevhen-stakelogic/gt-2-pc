// The 2 player Battle (game mode 0 of the US Arcade v1.1 disc; docs/research/arcade_disc.md section 19) parts of the race
// overlay against the original, on any race dump (the routines are the same in both builds; addresses US Simulation v1.2,
// translated to the dump's build by the profile):
//   CatchBattle 0x80042038(body 0, body 1): the frame driver's mode-0 catch-up ("Slow Car Boost") - random laps / course
//     distances / off-road flags of both cars and random tuning words 0x80046F78 / 0x80046F7C / 0x80046F80 (fastGain 0 in a
//     fifth of the cases); ALL of RAM compared (race_sim.h CatchUpBattle).
//   LineStart 0x80039040(&chunk, &x, &y, &dx, &dy, offset): the Handicap Start's placement on the main line (ai_driver.h
//     LineStartPlacement) - offsets 0, +-1, the handicap steps -10 .. -90 m and random ones up to +-2 course lengths, random
//     out-word seeds; the result and the five out words compared.
//   BoostSet 0x8003B73C(option), WearSet 0x8003B69C(option): the race load's settings bytes of the two race options (options
//     0..3 and random bytes; the settings block randomised first); RAM and 0x8003B73C's result compared (RaceLoadSettings).
#include <cstdio>
#include <cstring>
#include <vector>

#include "game/sim/ai_driver.h"
#include "game/sim/race_sim.h"
#include "guest.h"

namespace gt2::verify {
namespace {

uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(At(ram, address), &v, sizeof(T)); }
template <typename T> T Get(const uint8_t* ram, uint32_t address) {
    T v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T));
    return v;
}

} // namespace

int VerifyBattle(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    int failures = 0;
    const uint32_t body0 = kCarBase + kBodyOffset, body1 = kCarBase + kCarStride + kBodyOffset;
    const uint32_t fastGain = D(0x80046F78u), fastFrom = D(0x80046F7Cu), fastTo = D(0x80046F80u);
    // The course length the gap routine 0x800423BC reads: *(*(0x800B4A44)) (the chunk table's first word).
    const uint32_t chunkTable = Get<uint32_t>(pristine.data(), D(0x800B4A44u));
    const int32_t courseLength = Get<int32_t>(pristine.data(), chunkTable);

    // ---- CatchBattle
    {
        const StatefulResult r = VerifyStateful(
            guest, pristine, 0x80042038u, {body0}, 3000,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t variant) {
                if (variant % 4 == 0) return; // the dump's own state
                for (const uint32_t b : {body0, body1}) {
                    Put<int16_t>(ram, b + 0x608, int16_t(rng() % 4));                        // lap
                    Put<int32_t>(ram, b + 0x604, int32_t(rng() % uint32_t(courseLength > 0 ? courseLength : 1))); // course distance
                    Put<uint8_t>(ram, b + 0x78D, uint8_t(rng() % 3 == 0 ? rng() : 0));      // flags (bit 1 backwards, bit 4 off road)
                    Put<int16_t>(ram, b + 0x766, int16_t(rng()));                            // time scale (overwritten)
                }
                Put<int32_t>(ram, fastGain, rng() % 5 == 0 ? 0 : int32_t(rng() % 0x1000));
                const int32_t from = int32_t(rng() % 0x400000), to = from + int32_t(rng() % 0x4000000) + (rng() % 8 == 0 ? -from : 1);
                Put<int32_t>(ram, fastFrom, from);
                Put<int32_t>(ram, fastTo, to);
            },
            [&](uint8_t* ram, uint8_t*, uint32_t) {
                sim::CatchUpTuning t;
                t.fastGain = Get<int32_t>(ram, fastGain);
                t.fastFrom = Get<int32_t>(ram, fastFrom);
                t.fastTo = Get<int32_t>(ram, fastTo);
                sim::CatchUpBattle(*reinterpret_cast<sim::CarBody*>(At(ram, body0)), *reinterpret_cast<sim::CarBody*>(At(ram, body1)), t, courseLength);
            },
            body1);
        Report("CatchBattle", 0x80042038u, r.cases, r.mismatches, failures);
    }

    // ---- LineStart
    {
        size_t cases = 0, bad = 0;
        const uint32_t out = kStack + 0x40;
        const uint32_t startLines = D(0x800B4A58u);
        for (size_t i = 0; i < 600; i++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            uint8_t* ram = guest.Ram();
            const sim::AiContext c = AiContextFromRam(ram);
            const int32_t length = c.course.courseLength >> 16;
            int32_t offset;
            if (i < 3) offset = int32_t(i) - 1;
            else if (i < 13) offset = -10 * int32_t(i - 3);
            else offset = int32_t(rng() % uint32_t(4 * length + 1)) - 2 * length;
            int32_t words[5];
            for (int32_t& w : words) w = int32_t(rng() % 0x100000) - 0x80000;
            for (uint32_t k = 0; k < 5; k++) Put<int32_t>(ram, out + 4 * k, words[k]);
            Put<uint32_t>(ram, kStack + 16, out + 16);
            Put<int32_t>(ram, kStack + 20, offset);
            const uint32_t original = guest.Call(0x80039040u, out, out + 4, out + 8, out + 12);
            const uint32_t courseTable = D(0x801E18E8u) + uint32_t(Get<uint8_t>(pristine.data(), D(0x800AF230u))) * 24u;
            const bool pointToPoint = (Get<uint16_t>(pristine.data(), courseTable + 8) & 0x20) != 0;
            const int32_t lineCount = Get<int32_t>(pristine.data(), startLines);
            std::vector<int32_t> lines(size_t(lineCount > 0 ? lineCount : 0));
            for (size_t k = 0; k < lines.size(); k++) lines[k] = Get<int32_t>(pristine.data(), startLines + 4 + uint32_t(k) * 4);
            int32_t ours[5] = {words[0], words[1], words[2], words[3], words[4]};
            const int32_t result = sim::LineStartPlacement(c.course, pointToPoint, lines.data(), lineCount, offset, ours[0], ours[1], ours[2], ours[3], ours[4]);
            bool same = uint32_t(result) == original;
            for (uint32_t k = 0; k < 5; k++) same = same && Get<int32_t>(ram, out + 4 * k) == ours[k];
            cases++;
            if (!same && bad++ < 3)
                std::printf("    MISMATCH LineStart offset %d: original %d (%d %d %d %d %d) ours %d (%d %d %d %d %d)\n", offset, int32_t(original), Get<int32_t>(ram, out),
                            Get<int32_t>(ram, out + 4), Get<int32_t>(ram, out + 8), Get<int32_t>(ram, out + 12), Get<int32_t>(ram, out + 16), result, ours[0], ours[1],
                            ours[2], ours[3], ours[4]);
        }
        Report("LineStart", 0x80039040u, cases, bad, failures);
    }

    // ---- BoostSet / WearSet
    for (int routine = 0; routine < 2; routine++) {
        const uint32_t address = routine == 0 ? 0x8003B73Cu : 0x8003B69Cu;
        size_t cases = 0, bad = 0;
        std::vector<uint8_t> ours(Bus::kRamSize);
        const uint32_t settings = D(0x801C98A0u);
        for (size_t i = 0; i < 400; i++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            uint8_t* ram = guest.Ram();
            for (uint32_t k = 0; k < 0x40; k++) ram[(settings + k) & 0x1FFFFF] = uint8_t(rng());
            const uint32_t option = i < 8 ? uint32_t(i % 4) : (rng() % 2 ? rng() % 4 : rng() % 256); // the race load passes a byte (lbu)
            std::memcpy(ours.data(), ram, Bus::kRamSize);
            const uint32_t original = guest.Call(address, option);
            const std::span<uint8_t> block(At(ours.data(), settings), 0x40);
            bool same = true;
            if (routine == 0) same = (original != 0) == sim::SlowCarBoostSettings(uint8_t(option), block);
            else sim::TyreWearSettings(uint8_t(option), block);
            const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
            same = same && std::memcmp(ours.data(), ram, stackLow) == 0 && std::memcmp(ours.data() + stackHigh, ram + stackHigh, Bus::kRamSize - stackHigh) == 0;
            cases++;
            if (!same && bad++ < 3) std::printf("    MISMATCH %s option %u\n", routine == 0 ? "BoostSet" : "WearSet", option);
        }
        Report(routine == 0 ? "BoostSet" : "WearSet", address, cases, bad, failures);
    }
    return failures;
}

} // namespace gt2::verify
