// The GT-mode machine test (src/game/career/machine_test.h) against the original on RAM images:
//   MtName   EXE 0x8005E03C   the NEW RECORD name into a record entry (any dump)
//   MtRecord ovl0 0x80050D78  the race's entry ranked into the career's record of the sub-mode + the work block W + 2 / + 8 /
//                             + 0xC (race-overlay dumps, e.g. work/re/mtest/race/ram.bin)
//   MtSetup  ovl4 0x80012C6C  the menus' race setup of G400 / G1000 / GMAX: race block, settings block, the garage car's race
//                             tyres, slot 0's parameter record, the RECORD view's car names, the Settings page's sheet
//                             (GT-mode overlay dumps: work/re/gtmode, work/re/mtest/menu/ram.bin)
// Every case runs the original routine on a randomised image and our port on a byte-identical copy; all RAM outside the guest
// stack and the scratchpad must be equal afterwards (the masks below are stack residue the original leaves in its data).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/career/events.h"
#include "game/career/garage.h"
#include "game/career/machine_test.h"
#include "game/career/tuning.h"
#include "gt2formats/course_data.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {

namespace {

using namespace gt2::career;

constexpr uint32_t kWork = 0x801FC000u; // free guest RAM below the harness stack (argument blocks)
constexpr uint32_t kWorkBlockPointer = 0x801C90A0u, kSubMode = 0x801D5866u, kSlot0Car = 0x801D58B8u;
constexpr uint32_t kResultTime = 0x801D5F58u, kResultSpeed = 0x801D5F68u;
constexpr uint32_t kRaceBlock = 0x801D585Cu, kSettingsBlock = 0x801C98A0u, kSlot0Record = kStateAddress + 0x14FDAu, kCarNames = 0x801D5FA0u;

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
template <typename T> T& At(uint8_t* ram, uint32_t address) { return *reinterpret_cast<T*>(ram + (address & 0x1FFFFF)); }

struct Rows {
    size_t cases = 0, mismatches = 0, masked = 0, shown = 0;
};

// First difference outside the guest stack (RAM) and in the scratchpad; prints up to three.
bool Equal(Guest& guest, const std::vector<uint8_t>& ours, const std::vector<uint8_t>& ourScratch, const char* row, size_t i, Rows& r) {
    const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
    for (uint32_t a = 0; a < Bus::kRamSize; a++) {
        if (a >= stackLow && a < stackHigh) continue;
        if (ours[a] != guest.Ram()[a]) {
            if (r.shown++ < 3) std::printf("    MISMATCH %s case %zu: RAM 0x%08X original %02X ours %02X\n", row, i, 0x80000000u + a, guest.Ram()[a], ours[a]);
            return false;
        }
    }
    for (uint32_t a = 0; a < kScratchSize; a++)
        if (ourScratch[a] != guest.Scratch()[a]) {
            if (r.shown++ < 3) std::printf("    MISMATCH %s case %zu: scratchpad +0x%03X original %02X ours %02X\n", row, i, a, guest.Scratch()[a], ourScratch[a]);
            return false;
        }
    return true;
}

void Print(const char* name, uint32_t address, const Rows& r, const char* maskNote, int& failures) {
    std::string extra;
    if (r.masked) extra = ", " + std::to_string(r.masked) + " with " + maskNote;
    std::printf("%-10s 0x%08X  %zu cases%s, %zu mismatches  %s\n", name, address, r.cases, extra.c_str(), r.mismatches, r.mismatches ? "FAIL" : "ok");
    failures += r.mismatches ? 1 : 0;
}

// A record as the game makes them: 0..8 entries sorted by value (some of `cars`), the rest -1; sometimes a count > 8.
void RandomRecord(MachineTestRecord& rec, std::mt19937& rng, bool higher, const std::vector<uint32_t>& cars) {
    InitMachineTestRecord(rec);
    const int n = int(rng() % 10 == 0 ? 9 + rng() % 4 : rng() % 9);
    std::vector<uint32_t> values;
    for (int k = 0; k < std::min(n, 8); k++) values.push_back(rng() % 6 == 0 ? rng() : 10000 + rng() % 40 * 250);
    std::sort(values.begin(), values.end(), [higher](uint32_t a, uint32_t b) { return higher ? b < a : a < b; });
    for (int k = 0; k < int(values.size()); k++) {
        uint8_t* e = MachineTestEntry(rec, k);
        const uint32_t car = cars[rng() % cars.size()];
        std::memcpy(e, &car, 4);
        std::memcpy(e + 4, &values[size_t(k)], 4);
        for (int b = 8; b < 0x14; b++) e[b] = uint8_t(rng() % 3 == 0 ? 0 : 0x20 + rng() % 0x5F);
    }
    rec.bytes[0] = uint8_t(n);
}

// The GT-mode overlay loads carparam/usa_gtmode_race.dat to 0x80024430 before a race is built (0x80076CF8 + 0x80076AE8: the
// GTDT entries turned into {pointer, u16 row size (EXE 0x80092490), u16 row count}); the menus reuse that memory, so the
// harness puts the loaded file back (the same preparation as verify_career.cpp's event rows).
void PutRaceFile(uint8_t* ram, const std::vector<uint8_t>& file, const GuestImage& exe, uint8_t language) {
    constexpr uint32_t kBase = 0x80024430u;
    if (file.size() > 0x2C4C0) throw std::runtime_error("race file larger than its buffer");
    std::memcpy(ram + (kBase & 0x1FFFFF), file.data(), file.size());
    const uint32_t half = Get<uint16_t>(ram, kBase + 6) >> 1;
    for (uint32_t k = 0; k < half; k++) {
        const uint32_t e = kBase + 8 + k * 8;
        const uint32_t size = Get<uint32_t>(ram, e + 4);
        const uint32_t rowSize = exe.Get<uint32_t>(D(0x80092490u) + k * 4);
        Put<uint32_t>(ram, e, Get<uint32_t>(ram, e) + kBase);
        Put<uint16_t>(ram, e + 4, uint16_t(rowSize));
        Put<uint16_t>(ram, e + 6, uint16_t(size / rowSize));
        const uint32_t x = kBase + 8 + (k + half) * 8;
        if (Get<uint32_t>(ram, x) != 0) Put<uint32_t>(ram, x, Get<uint32_t>(ram, x) + kBase);
    }
    Put<uint32_t>(ram, D(0x80092870u), kBase);
    Put<uint32_t>(ram, D(0x80092E70u), kBase);
    Put<uint32_t>(ram, D(0x80092884u), language);
    Put<uint32_t>(ram, D(0x801C94E4u), Get<uint16_t>(ram, kBase + 4));
}

// The gearbox residue of the configurations the tune code writes back (docs/research/menus_gtmode.md section 8: 0x8005E93C /
// 0x8005E99C leave the gear entries above the top gear to the stack): masked as verify_career.cpp does for the same code.
bool MaskConfigGears(uint8_t* ours, const uint8_t* theirs, uint32_t configAt, const CarParamTables& t) {
    const uint16_t gearbox = uint16_t(theirs[configAt + 0x10] | theirs[configAt + 0x11] << 8);
    if (gearbox >= t.RowCount(kTableGearbox)) return false;
    const std::span<const uint8_t> row = t.Row(kTableGearbox, gearbox);
    if (row[0x20] == 0) return false;
    bool changed = false;
    for (uint32_t k = 2u * (uint32_t(row[9]) + 1u); k < 16; k++) {
        const uint32_t a = configAt + 0x3C + k;
        if (ours[a] != theirs[a]) { ours[a] = theirs[a]; changed = true; }
    }
    return changed;
}

} // namespace

int VerifyMachineTest(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol) {
    int failures = 0;
    if (!gt2::ActiveProfile().reference) { // the machine test (G400 / G1000 / GMAX, career + 0x3A88) is a Simulation-disc feature
        std::printf("MachineTest skipped (the dump is of %s: the machine test and its records exist on the US Simulation v1.2 disc only)\n",
                    gt2::ActiveProfile().name);
        return 0;
    }
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    auto start = [&](size_t) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
    };
    auto copy = [&] {
        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
        std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
    };
    const uint32_t recordBase = kStateAddress + uint32_t(offsetof(CareerState, machineTests));
    std::vector<uint32_t> someCars = {0x0B0CC758u, 0x01020304u, 0x0A0B0C0Du, 0x11223344u, 0x0B0CC759u};

    // ---- MtName: EXE 0x8005E03C(record, index, name)
    {
        Rows r;
        for (size_t i = 0; i < 400; i++) {
            start(i);
            const int test = int(rng() % 3);
            RandomRecord(At<MachineTestRecord>(guest.Ram(), recordBase + uint32_t(test) * 0xA4u), rng, test == 2, someCars);
            const int32_t index = int32_t(rng() % 12) - 2;
            std::string name;
            const size_t length = rng() % 12;
            for (size_t k = 0; k < length; k++) name.push_back(char(0x21 + rng() % 0x5E));
            std::memcpy(guest.Ram() + (kWork & 0x1FFFFF), name.c_str(), name.size() + 1);
            copy();
            guest.Call(0x8005E03Cu, recordBase + uint32_t(test) * 0xA4u, uint32_t(index), kWork);
            StoreMachineTestName(At<MachineTestRecord>(ours.data(), recordBase + uint32_t(test) * 0xA4u), index, name);
            r.cases++;
            if (!Equal(guest, ours, ourScratch, "MtName", i, r)) r.mismatches++;
        }
        Print("MtName", 0x8005E03Cu, r, "", failures);
    }
    if (!disc || !vol) {
        std::puts("MtRecord   skipped (needs the disc)");
        return failures;
    }

    // ---- MtRecord: ovl0 0x80050D78 (the race overlay loaded)
    if (OverlayMemberLoaded(pristine.data(), *disc, 0, MapCode(0x80050D78u), 0x120)) {
        Rows r;
        for (size_t i = 0; i < 1200; i++) {
            start(i);
            uint8_t* ram = guest.Ram();
            for (int t = 0; t < 3; t++) RandomRecord(At<MachineTestRecord>(ram, recordBase + uint32_t(t) * 0xA4u), rng, t == 2, someCars);
            const uint8_t mode = uint8_t(i % 8 == 7 ? rng() % 13 : 7 + rng() % 3);
            Put<uint8_t>(ram, D(kSubMode), mode);
            Put<uint32_t>(ram, D(kSlot0Car), someCars[rng() % someCars.size()]);
            Put<uint32_t>(ram, D(kResultTime), rng() % 4 == 0 ? rng() : 10000 + rng() % 40 * 250);
            Put<uint16_t>(ram, D(kResultSpeed), uint16_t(rng() % 4 == 0 ? rng() : 5000 + rng() % 40 * 125));
            Put<uint32_t>(ram, D(kWorkBlockPointer), kWork + 0x100);
            for (uint32_t k = 0; k < 0x10; k++) ram[((kWork + 0x100) & 0x1FFFFF) + k] = uint8_t(rng());
            // the stack the original's entry takes its name tail from (0x80050D78's sp + 0x19 ..): filled so that the residue shows
            std::memset(ram + ((kStack - 0x1000) & 0x1FFFFF), int(0xA5 ^ (i & 0xFF)), 0x1000);
            copy();
            guest.Call(0x80050D78u);
            CareerState& s = At<CareerState>(ours.data(), kStateAddress);
            const MachineTestOutcome o = WriteMachineTestRecord(s, Get<uint8_t>(ours.data(), D(kSubMode)), Get<uint32_t>(ours.data(), D(kSlot0Car)),
                                                                Get<uint32_t>(ours.data(), D(kResultTime)), Get<uint16_t>(ours.data(), D(kResultSpeed)));
            Put<int16_t>(ours.data(), kWork + 0x102, o.rank);
            Put<uint32_t>(ours.data(), kWork + 0x108, o.time);
            Put<uint32_t>(ours.data(), kWork + 0x10C, o.maxSpeed);
            // The inserted entry's name: strcpy(entry + 8, "") of 0x8005A980 writes the NUL only, the other 11 bytes are the
            // stack's (0x80050D78's frame): the original's residue, masked.
            if (o.rank >= 0) {
                const uint32_t tail = recordBase + uint32_t(MachineTestIndex(mode)) * 0xA4u + 4u + uint32_t(o.rank) * 0x14u + 9u;
                if (std::memcmp(ours.data() + (tail & 0x1FFFFF), guest.Ram() + (tail & 0x1FFFFF), 11) != 0) r.masked++;
                std::memcpy(ours.data() + (tail & 0x1FFFFF), guest.Ram() + (tail & 0x1FFFFF), 11);
            }
            r.cases++;
            if (!Equal(guest, ours, ourScratch, "MtRecord", i, r)) r.mismatches++;
        }
        Print("MtRecord", 0x80050D78u, r, "the entry's name tail (stack residue) masked", failures);
    }

    // ---- MtSetup: ovl4 0x80012C6C(name, sub-mode) (the GT-mode overlay loaded)
    if (OverlayMemberLoaded(pristine.data(), *disc, 4, 0x80012C6Cu, 0x300)) {
        CareerData data = CareerData::Load(*disc, *vol);
        const EventMenuData menu = EventMenuData::Load(data.ovl4);
        const EventInfoTable infos = BuildEventInfos(data, menu);
        const CourseInfoTable courses = ParseCourseInfo(vol->Read(".crsinfo"));
        const std::vector<uint8_t> raceFile = vol->Read("carparam/usa_gtmode_race.dat");
        const GuestImage exe = LoadExeImage(*disc);
        { // the menus' gear generator reads the dirt flag of the last race's course (as verify_career.cpp)
            const uint32_t course = pristine[D(0x800AF230u) & 0x1FFFFF];
            data.menuDirtCourse = (Get<uint16_t>(pristine.data(), D(0x801E18E8u) + course * 24u + 8u) & 4) != 0;
        }
        Rows r;
        for (size_t i = 0; i < 150; i++) {
            start(i);
            uint8_t* ram = guest.Ram();
            CareerState& g0 = At<CareerState>(ram, kStateAddress);
            PutRaceFile(ram, raceFile, exe, g0.language);
            // a garage of catalogue cars (some with dirt tyres or other stages selected by the purchase pass), a current car
            GarageBlock& g = g0.garage;
            g.count = int16_t(1 + rng() % 5);
            std::vector<uint32_t> cars;
            for (int16_t k = 0; k < g.count; k++) {
                career::GarageCar& c = g.cars[k];
                uint8_t* b = reinterpret_cast<uint8_t*>(&c);
                for (size_t n = 0; n < sizeof(career::GarageCar); n++) b[n] = uint8_t(rng());
                c.carId = CarCatalogueAt(data.tables, rng() % CarCatalogueCount(data.tables)).spec.carId;
                c.modelId = c.carId;
                c.paint = 0x61u + rng() % 4;
                CarConfig config = *CatalogueCarConfig(data.tables, c.carId);
                auto sheet = std::make_unique<TuneSheet>();
                AnalysePurchase(*sheet, c.carId, config, data);
                c.config = config;
                cars.push_back(c.carId);
            }
            g.currentCar = int16_t(rng() % uint32_t(g.count));
            for (int t = 0; t < 3; t++) RandomRecord(g0.machineTests[t], rng, t == 2, cars);
            for (size_t k = 1; k <= 8; k++) reinterpret_cast<uint8_t*>(&g0)[k] = uint8_t(rng() % 4);
            const int test = int(rng() % 3);
            const std::string& name = menu.machineTests[size_t(test)];
            std::memcpy(ram + (kWork & 0x1FFFFF), name.c_str(), name.size() + 1);
            std::memset(ram + ((kStack - 0x1000) & 0x1FFFFF), 0xAA, 0x1000);
            copy();
            try {
                guest.CallWithBios(0x80012C6Cu, kWork, uint32_t(7 + test));
            } catch (const std::exception& e) {
                std::printf("    MtSetup case %zu: the original stopped: %s\n", i, e.what());
                r.mismatches++;
                continue;
            }
            CareerState& s = At<CareerState>(ours.data(), kStateAddress);
            const MachineTestRace m = PrepareMachineTest(s, data, menu, infos, courses, name, At<TuneSheet>(ours.data(), D(kCarSheetAddress)), ourScratch.data(),
                                                         &At<TuneSheet>(ours.data(), D(kTuneSheetAddress)));
            std::memcpy(ours.data() + (kRaceBlock & 0x1FFFFF), m.raceBlock.data(), m.raceBlock.size());
            std::memcpy(ours.data() + (kSettingsBlock & 0x1FFFFF), m.settings.data(), m.settings.size());
            std::memcpy(ours.data() + (kSlot0Record & 0x1FFFFF), &m.params, sizeof(m.params));
            for (size_t k = 0; k < m.carNames.bytes.size(); k++)
                if (m.carNames.written[k]) ours[((kCarNames + uint32_t(k)) & 0x1FFFFF)] = m.carNames.bytes[k];
            bool maskedHere = false;
            for (int16_t k = 0; k < g.count; k++)
                maskedHere |= MaskConfigGears(ours.data(), guest.Ram(), (kGarageAddress + 4u + uint32_t(k) * 0xA4u + 8u) & 0x1FFFFF, data.tables);
            maskedHere |= MaskConfigGears(ours.data(), guest.Ram(), (kRaceBlock + 0x5Cu + 8u) & 0x1FFFFF, data.tables);
            maskedHere |= MaskConfigGears(ours.data(), guest.Ram(), (D(kCarSheetAddress)) & 0x1FFFFF, data.tables);
            maskedHere |= MaskConfigGears(ours.data(), guest.Ram(), (D(kTuneSheetAddress)) & 0x1FFFFF, data.tables);
            r.masked += maskedHere ? 1 : 0;
            r.cases++;
            if (!Equal(guest, ours, ourScratch, "MtSetup", i, r)) r.mismatches++;
        }
        Print("MtSetup", 0x80012C6Cu, r, "the gearbox residue masked", failures);
    }
    return failures;
}

} // namespace gt2::verify
