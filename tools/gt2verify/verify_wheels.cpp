// Differential check of the wheel mesh generation (gt2export/car_mesh.h GenerateWheelArea / RaceWheelDims):
//   WheelArea: the car setup's wheel step 0x80017FA0 (the wheel records car + 0x7CC.. and, through 0x80061504 /
//              0x80061308 / 0x800611F8, the runtime area of the body model, model + 0x40..0x867) runs on the guest for
//              every car of the dump with the dump's and random wheel dimensions (body + 0x3E4 / 0x3EC / 0x3F0), wheel words
//              (CarConfig +0x00 of the car's race slot) and half tracks; ours writes the same bytes from the templates and
//              the dish table of the disc's executable. All RAM outside the stack must be equal.
//   WheelStrip: the strips' drawer 0x80066EF8 (packets and the per-quad colour rule) against EmitWheelStrips (below).
#include <cstring>
#include <stdexcept>

#include "game/sim/trig.h"
#include "gt2export/car_mesh.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "guest.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kConfigs = 0x801D58C0u, kConfigStride = 0xD0; // CarConfig of each race slot (word +0x00 = the wheel word)

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }

} // namespace

int VerifyWheels(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt, const DiscImage* disc) {
    int failures = 0;
    if (!disc) {
        std::puts("WheelArea  0x80017FA0  skipped (needs the disc: the executable's wheel templates)");
        return failures;
    }
    const GuestImage exe = LoadExeImage(*disc);
    const WheelTemplates templates = LoadWheelTemplates(exe);
    const std::array<int16_t, 4> dishes = LoadWheelDishDepths(exe);
    std::vector<uint32_t> cars;
    for (uint32_t car = 0; car < kCarCount; car++)
        if (const uint32_t model = Get<uint32_t>(pristine.data(), kCarBase + car * kCarStride + 0x878); (model & 0xFFE00000u) == 0x80000000u)
            cars.push_back(kCarBase + car * kCarStride);
    auto range = [&](int32_t low, int32_t high) { return low + int32_t(mt() % uint32_t(high - low + 1)); };
    const StatefulResult r = VerifyStateful(
        guest, pristine, 0x80017FA0u, cars, 60,
        [&](uint8_t* ram, uint8_t*, uint32_t car, size_t variant) {
            if (variant % 4 == 0) return; // the dump's own values
            const uint32_t body = car + kBodyOffset;
            for (uint32_t axle = 0; axle < 2; axle++) {
                const int16_t radius = int16_t(range(700, 2000));
                Put<int16_t>(ram, body + 0x3E4 + axle * 2, radius);
                Put<int16_t>(ram, body + 0x3EC + axle * 2, int16_t(range(300, 1400)));
                Put<int16_t>(ram, body + 0x3F0 + axle * 2, int16_t(range(0, radius - 1)));
                Put<int16_t>(ram, car + 0x44 + axle * 2, int16_t(range(0, 4000)));
            }
            const uint32_t slot = D(kConfigs) + uint32_t(Get<int16_t>(ram, car + 0x0C)) * kConfigStride;
            Put<uint32_t>(ram, slot, mt());
        },
        [&](uint8_t* ram, uint8_t*, uint32_t car) {
            const uint32_t body = car + kBodyOffset;
            const uint32_t model = Get<uint32_t>(ram, car + 0x878);
            // the wheel records: x = -/+ the axle's half track (left wheels negative), y 0, z = the .cdo wheel entry's third s16
            for (uint32_t w = 0; w < 4; w++) {
                const int16_t half = Get<int16_t>(ram, car + 0x44 + (w >> 1) * 2);
                Put<int16_t>(ram, car + 0x7CC + w * 16, int16_t((w & 1) ? half : -half));
                Put<int16_t>(ram, car + 0x7CE + w * 16, 0);
                Put<int16_t>(ram, car + 0x7D0 + w * 16, Get<int16_t>(ram, model + w * 8 + 0x24));
            }
            const uint32_t word = Get<uint32_t>(ram, D(kConfigs) + uint32_t(Get<int16_t>(ram, car + 0x0C)) * kConfigStride);
            const int16_t dish = WheelDishDepth(word, dishes);
            std::array<WheelAxleDims, 2> dims;
            for (uint32_t axle = 0; axle < 2; axle++)
                dims[axle] = RaceWheelDims(Get<int16_t>(ram, body + 0x3E4 + axle * 2), Get<int16_t>(ram, body + 0x3EC + axle * 2), Get<int16_t>(ram, body + 0x3F0 + axle * 2), dish);
            const WheelArea area = GenerateWheelArea(dims, templates);
            const uint32_t base = model + 0x40;
            for (uint32_t axle = 0; axle < 2; axle++) {
                Put<int16_t>(ram, base + kWheelAreaRimZ + axle * 2, area.rimZ[axle]);
                for (uint32_t k = 0; k < 8; k++) Put<int16_t>(ram, base + kWheelAreaSquare[axle] + k * 2, area.square[axle][k]);
                for (uint32_t lod = 0; lod < 3; lod++) {
                    const std::vector<int16_t>& s = area.lods[axle][lod];
                    for (size_t k = 0; k < s.size(); k++) Put<int16_t>(ram, base + kWheelAreaStream[axle][lod] + uint32_t(k) * 2, s[k]);
                }
            }
        });
    Report("WheelArea", 0x80017FA0u, r.cases, r.mismatches, failures);

    // WheelStrip 0x80066EF8: the strips of every car's generated area (both axles, LODs 0..2) through a GTE loaded by 0x8007B778
    // from a random view block (rotation, translation near and far, H; extreme translations reach the GTE's error flags),
    // random ordering-table shift, mirror byte and colour word; ours = EmitWheelStrips on the same RAM. RAM outside the stack
    // and the scratchpad must be equal (packets, colours, the ordering-table links, scratch + 0x68).
    {
        constexpr uint32_t kScratch = 0x1F800000u, kOt = 0x80180000u, kPackets = 0x80190000u;
        auto sput32 = [](uint8_t* scratch, uint32_t off, uint32_t v) { std::memcpy(scratch + off, &v, 4); };
        auto sput16 = [](uint8_t* scratch, uint32_t off, uint16_t v) { std::memcpy(scratch + off, &v, 2); };
        size_t cases = 0, mismatches = 0, packets = 0, grey = 0;
        std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
        const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
        for (uint32_t car : cars)
            for (uint32_t axle = 0; axle < 2; axle++)
                for (uint32_t lod = 0; lod < 3; lod++)
                    for (size_t variant = 0; variant < 12; variant++) {
                        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                        std::memset(guest.Scratch(), 0, kScratchSize);
                        uint8_t* sc = guest.Scratch();
                        // a rotation R = Ry(yaw) Rx(pitch) from the sine table, 4096 = 1
                        const uint32_t yaw = mt() & 0xFFF, pitch = mt() & 0xFFF;
                        const int32_t cy = sim::Cos(yaw), sy = sim::Sin(yaw), cp = sim::Cos(pitch), sp = sim::Sin(pitch);
                        const int32_t m[9] = {cy, (sy * sp) >> 12, (sy * cp) >> 12, 0, cp, -sp, -sy, (cy * sp) >> 12, (cy * cp) >> 12};
                        for (int k = 0; k < 9; k++) sput16(sc, uint32_t(k) * 2, uint16_t(int16_t(m[k])));
                        const bool extreme = variant % 4 == 3;
                        sput32(sc, 0x14, uint32_t(range(extreme ? -60000 : -3000, extreme ? 60000 : 3000)));
                        sput32(sc, 0x18, uint32_t(range(extreme ? -60000 : -2000, extreme ? 60000 : 2000)));
                        sput32(sc, 0x1C, uint32_t(range(extreme ? -2000 : 1500, extreme ? 70000 : 30000)));
                        sput32(sc, 0x54, uint32_t(range(100, 220)) << 16);
                        sput32(sc, 0x58, uint32_t(range(80, 160)) << 16);
                        sput16(sc, 0x5C, uint16_t(range(120, 700)));
                        guest.Call(0x8007B778u, kScratch, 0, 0, 0); // loads the GTE from the block (shift 0), writes + 0x78.., + 0x98 / + 0x9A
                        sput32(sc, 0x64, kOt);
                        sput32(sc, 0x68, kPackets);
                        sput16(sc, 0x98, uint16_t(range(0, 3)));
                        static constexpr uint8_t kMirror[4] = {0x00, 0xFF, 0x7F, 0x80};
                        sc[0x398] = kMirror[mt() % 4];
                        sput32(sc, 0x3B0, variant % 2 ? mt() : 0x888888u);
                        const uint32_t model = Get<uint32_t>(guest.Ram(), car + 0x878);
                        const uint32_t stream = model + 0x40 + kWheelAreaStream[axle][lod];
                        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
                        std::memcpy(ourScratch.data(), sc, kScratchSize);
                        guest.Call(0x80066EF8u, kScratch, stream);
                        WheelStripGte g;
                        for (int k = 0; k < 9; k++) std::memcpy(&g.rotation[k / 3][k % 3], ourScratch.data() + k * 2, 2);
                        for (int k = 0; k < 3; k++) std::memcpy(&g.translation[k], ourScratch.data() + 0x14 + k * 4, 4);
                        std::memcpy(&g.ofx, ourScratch.data() + 0x54, 4);
                        std::memcpy(&g.ofy, ourScratch.data() + 0x58, 4);
                        std::memcpy(&g.h, ourScratch.data() + 0x5C, 2);
                        uint16_t shift = 0;
                        std::memcpy(&shift, ourScratch.data() + 0x98, 2);
                        uint32_t colour = 0;
                        std::memcpy(&colour, ourScratch.data() + 0x3B0, 4);
                        const uint32_t next = EmitWheelStrips(ours.data(), stream, g, kPackets, kOt, shift, int8_t(ourScratch[0x398]), colour);
                        sput32(ourScratch.data(), 0x68, next);
                        cases++;
                        for (uint32_t pk = kPackets; pk < next; pk += 24) { // the packets drawn and their colour (rule check coverage)
                            packets++;
                            grey += (Get<uint32_t>(ours.data(), pk + 4) & 0xFFFFFFu) == (colour & 0xFFFFFFu) && (colour & 0xFFFFFFu) != 0 ? 1 : 0;
                        }
                        const bool equal = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                                           std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0 &&
                                           std::memcmp(ourScratch.data(), sc, kScratchSize) == 0;
                        if (!equal && mismatches++ < 3) {
                            for (uint32_t i = 0; i < Bus::kRamSize; i++)
                                if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                                    std::printf("    MISMATCH car %08X axle %u lod %u variant %zu: first differing byte at 0x%08X: original %02X ours %02X\n", car, axle, lod,
                                                variant, 0x80000000u + i, guest.Ram()[i], ours[i]);
                                    break;
                                }
                            for (uint32_t i = 0; i < kScratchSize; i++)
                                if (ourScratch[i] != sc[i]) {
                                    std::printf("    MISMATCH car %08X: scratchpad + 0x%X: original %02X ours %02X\n", car, i, sc[i], ourScratch[i]);
                                    break;
                                }
                        }
                    }
        std::printf("           (%zu quads drawn, %zu of them grey)\n", packets, grey);
        Report("WheelStrip", 0x80066EF8u, cases, mismatches, failures);
    }
    return failures;
}

} // namespace gt2::verify
