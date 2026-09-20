// GT-mode career (src/game/career/*) against the original: the new-game initialiser, the garage and money rules,
// the tune sheet and the purchase of a car (GT-mode overlay dumps: GT2.OVL member 4 loaded, e.g. work/re/gtmode),
// and the race-result appliers (race overlay dumps: member 0). Every row runs the original routine on a RAM image
// with randomised career state and our port on a byte-identical copy; all RAM outside the guest stack and the whole
// scratchpad must be equal afterwards (plus the return value where the routine has one).
#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "game/career/career_state.h"
#include "game/career/events.h"
#include "game/career/garage.h"
#include "game/career/results.h"
#include "game/career/championship.h"
#include "game/career/tuning.h"
#include "game/menu/menu_car.h"
#include "gt2formats/car_info.h"
#include "gt2formats/course_data.h"
#include "gt2formats/license_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/change_parts.h"
#include "gt2view/power_graph.h"
#include "guest.h"

namespace gt2::verify {

namespace {

using namespace gt2::career;
using career::CareerRecord; // (gt2::CareerRecord / gt2::GarageCar of save_data.h are the parsed views)
using career::GarageCar;

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
template <typename T> T& At(uint8_t* ram, uint32_t address) { return *reinterpret_cast<T*>(ram + (address & 0x1FFFFF)); }

constexpr uint32_t kWork = 0x801FC000u;          // free guest RAM below the harness stack for argument blocks
constexpr uint32_t kCareerRecordAddress = kStateAddress + 0xB8u;

struct CaseResult { size_t cases = 0, mismatches = 0, unported = 0; bool skipped = false; };

// GT2_VERIFY_SKIP=Row1,Row2 also leaves out career rows.
// GT2_VERIFY_CAREER_ONLY=Row1,Row2 runs only those career rows (development of new rows).
bool SkipCareerRow(const char* row) {
    auto listed = [row](const char* env) { return (std::string(",") + env + ",").find(std::string(",") + row + ",") != std::string::npos; };
    const char* skip = std::getenv("GT2_VERIFY_SKIP");
    const char* only = std::getenv("GT2_VERIFY_CAREER_ONLY");
    return (skip && listed(skip)) || (only && !listed(only));
}

void CompareImages(Guest& guest, const std::vector<uint8_t>& ours, const std::vector<uint8_t>& ourScratch, const char* row, size_t i, bool& equal, size_t& shown) {
    const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
    for (uint32_t a = 0; a < Bus::kRamSize; a++) {
        if (a >= stackLow && a < stackHigh) continue;
        if (ours[a] != guest.Ram()[a]) {
            equal = false;
            if (shown++ < 3) std::printf("    MISMATCH %s case %zu: RAM 0x%08X original %02X ours %02X\n", row, i, 0x80000000u + a, guest.Ram()[a], ours[a]);
            break;
        }
    }
    for (uint32_t a = 0; a < kScratchSize; a++)
        if (ourScratch[a] != guest.Scratch()[a]) {
            equal = false;
            if (shown++ < 3) std::printf("    MISMATCH %s case %zu: scratchpad +0x%03X original %02X ours %02X\n", row, i, a, guest.Scratch()[a], ourScratch[a]);
            break;
        }
}

// Bytes the original leaves to chance and the port cannot reproduce, masked before the comparison (fixup copies the
// original's bytes of those ranges into our image; returns true when it changed anything).
using Fixup = std::function<bool(uint8_t* ours, uint8_t* ourScratch, const uint8_t* guestRam, const uint8_t* guestScratch, size_t i)>;

// prepare(ram, i) randomises the guest image; guestRun(i) calls the original; native(ram, scratch, i) runs the port.
CaseResult RunCases(Guest& guest, const std::vector<uint8_t>& pristine, const char* row, size_t count, const std::function<void(uint8_t*, size_t)>& prepare,
                    const std::function<uint32_t(size_t)>& guestRun, const std::function<std::optional<uint32_t>(uint8_t*, uint8_t*, size_t)>& native,
                    const Fixup& fixup = nullptr, size_t* masked = nullptr) {
    CaseResult r;
    if (SkipCareerRow(row)) {
        r.skipped = true;
        return r;
    }
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    size_t shown = 0;
    for (size_t i = 0; i < count; i++) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
        prepare(guest.Ram(), i);
        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
        std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
        std::optional<uint32_t> nativeResult;
        try {
            nativeResult = native(ours.data(), ourScratch.data(), i);
        } catch (const std::logic_error& e) { // a branch of the original that is not ported (the port says so)
            if (r.unported++ == 0) std::printf("    %s case %zu: not ported: %s\n", row, i, e.what());
            continue;
        }
        const uint32_t original = guestRun(i);
        r.cases++;
        if (fixup && fixup(ours.data(), ourScratch.data(), guest.Ram(), guest.Scratch(), i) && masked) (*masked)++;
        bool equal = true;
        if (nativeResult && *nativeResult != original) {
            equal = false;
            if (shown++ < 3) std::printf("    MISMATCH %s case %zu: return original 0x%08X ours 0x%08X\n", row, i, original, *nativeResult);
        }
        CompareImages(guest, ours, ourScratch, row, i, equal, shown);
        if (!equal) r.mismatches++;
    }
    return r;
}

void ReportRow(const char* name, uint32_t address, const CaseResult& r, int& failures) {
    if (r.skipped) {
        std::printf("%-10s skipped (GT2_VERIFY_SKIP)\n", name);
        return;
    }
    if (r.unported) std::printf("%-10s 0x%08X  %zu cases (%zu skipped: branch not ported), %zu mismatches  %s\n", name, address, r.cases, r.unported, r.mismatches, r.mismatches ? "FAIL" : "ok");
    else std::printf("%-10s 0x%08X  %zu cases, %zu mismatches  %s\n", name, address, r.cases, r.mismatches, r.mismatches ? "FAIL" : "ok");
    failures += r.mismatches ? 1 : 0;
}

// Random career contents on the guest image: garage cars from the catalogue, money, the current car, results.
struct Randomiser {
    std::mt19937& rng;
    const CareerData& data;
    uint32_t RandomCatalogueCar() { return CarCatalogueAt(data.tables, rng() % CarCatalogueCount(data.tables)).spec.carId; }
    void Garage(uint8_t* ram, uint32_t player, int16_t minCount = 0) {
        GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress) + player * 0x4028u);
        const int16_t count = int16_t(minCount + int16_t(rng() % uint32_t(kGarageCapacity + 1 - minCount)));
        g.count = count;
        for (int16_t i = 0; i < count; i++) {
            GarageCar& c = g.cars[i];
            uint8_t* b = reinterpret_cast<uint8_t*>(&c);
            for (size_t k = 0; k < sizeof(GarageCar); k++) b[k] = uint8_t(rng());
            c.carId = RandomCatalogueCar();
            c.modelId = c.carId;
        }
        const uint32_t m = rng() % 8;
        g.money = m == 0 ? 0 : m == 1 ? kMoneyLimit : m == 2 ? int32_t(rng() % 20000) : int32_t(rng() % 3000000);
        g.currentCar = int16_t(int32_t(rng() % uint32_t(count + 2)) - 1);
    }
    // A garage car as the game makes them: a catalogue car bought (0x80017750) and, when `tuned`, some parts at random
    // stages selected on its sheet (0x8005EAC0), random owned-part bits; the other bytes random.
    void RealCar(GarageCar& c, bool tuned) {
        uint8_t* b = reinterpret_cast<uint8_t*>(&c);
        for (size_t k = 0; k < sizeof(GarageCar); k++) b[k] = uint8_t(rng());
        c.carId = RandomCatalogueCar();
        c.modelId = c.carId;
        CarConfig config = *CatalogueCarConfig(data.tables, c.carId);
        auto sheet = std::make_unique<TuneSheet>();
        AnalysePurchase(*sheet, c.carId, config, data);
        if (tuned) {
            const uint32_t changes = rng() % 12;
            for (uint32_t k = 0; k < changes; k++) {
                const int32_t kind = int32_t(rng() % 23);
                if (kind == kTuneProfile) continue;
                static constexpr uint32_t kSlots[23] = {4, 2, 2, 8, 8, 1, 4, 4, 4, 2, 2, 2, 2, 4, 2, 5, 3, 4, 4, 5, 2, 2, 6};
                const int32_t stage = int32_t(rng() % kSlots[kind]);
                static constexpr uint32_t kIndexWords[23] = {0xAB8, 0xAE0, 0xB08, 0xBB0, 0xF48, 0x688, 0x12A8, 0x12F8, 0x1338, 0x1360, 0x1380, 0x13A0,
                                                            0x13C0, 0x13F8, 0x1420, 0x148C, 0x14C4, 0x1500, 0x1540, 0x15DC, 0x1610, 0x1638, 0x1700};
                uint32_t word;
                std::memcpy(&word, reinterpret_cast<const uint8_t*>(sheet.get()) + kIndexWords[kind] + uint32_t(stage) * 4, 4);
                if (word == 0xFFFFFFFFu) continue;
                SetTuneStage(*sheet, kind, stage, data);
            }
            config = sheet->config;
        }
        c.config = config;
        for (uint8_t& p : c.partsOwned) p = uint8_t(rng() % 3 == 0 ? 0xFF : rng());
    }
    void RealGarage(uint8_t* ram, uint32_t player, int16_t minCount, int16_t maxCount) {
        GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress) + player * 0x4028u);
        g.count = int16_t(minCount + int16_t(rng() % uint32_t(maxCount + 1 - minCount)));
        for (int16_t i = 0; i < g.count; i++) RealCar(g.cars[i], rng() % 4 != 0);
        const uint32_t m = rng() % 4;
        g.money = m == 0 ? 0 : m == 1 ? kMoneyLimit : int32_t(rng() % 300000);
        g.currentCar = int16_t(rng() % uint32_t(g.count));
    }
};

// The race overlay's calls into its results screen (0x800595E0) and view switch (0x800481C8) inside a result
// routine are replaced by `nop` on the image (UI, not part of the career); the routine's other UI side effect, a
// word of the results view object (*0x801C90A0 + 0x14), is reproduced by the harness on our side.
void NeutraliseUiCalls(uint8_t* ram, uint32_t begin, uint32_t end) {
    for (uint32_t a = begin; a < end; a += 4) {
        const uint32_t insn = Get<uint32_t>(ram, a);
        if ((insn >> 26) != 3) continue; // jal
        const uint32_t target = 0x80000000u | ((insn & 0x03FFFFFFu) << 2);
        if (target == MapCode(0x800595E0u) || target == MapCode(0x800481C8u)) Put<uint32_t>(ram, a, 0);
    }
}

// The GT-mode overlay loads carparam/usa_gtmode_race.dat to 0x80024430 before a race is built (0x80076CF8: file,
// then 0x80076AE8 turns the first half of the GTDT entries into { pointer, u16 row size (EXE 0x80092490), u16 row
// count } and relocates the extras' offsets). The menus reuse that memory, so the harness puts the loaded file back.
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

// The gearbox residue (docs/research/menus_gtmode.md section 8): with a gearbox row whose gearAutoSet (+0x20) is set,
// 0x8005E93C / 0x8005E99C write reverse..top gear into an 8-entry stack buffer that the caller copies whole into the
// configuration: the entries above the top gear are the stack's residue of earlier, deeper calls (not written by the
// routine's own call tree - checked by running the purchase with the stack filled with 0x00 and 0xAA). They are read
// by nothing but the save / replay bytes; the rows mask exactly those entries of every configuration in play (and the
// scratchpad record built from it) and count the cases where the mask changed something.
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

Fixup GearResidueFixup(const CarParamTables& t) {
    return [&t](uint8_t* ours, uint8_t* ourScratch, const uint8_t* ram, const uint8_t* scratch, size_t) {
        std::vector<uint32_t> configs = {D(kTuneSheetAddress), D(0x800B4490u), kWork, D(0x8016E894u)};
        for (uint32_t p = 0; p < 2; p++)
            for (uint32_t i = 0; i < uint32_t(kGarageCapacity); i++) configs.push_back(D(kGarageAddress) + p * 0x4028u + 4u + i * 0xA4u + 8u);
        for (uint32_t k = 0; k < 4; k++) configs.push_back(D(kPrizeBlockAddress) + 0x20u + k * 0xA4u + 8u);
        bool changed = false;
        for (uint32_t a : configs) changed |= MaskConfigGears(ours, ram, a & 0x1FFFFF, t);
        if (changed) { // the record built from such a configuration on the scratchpad (0x8001EC0C / 0x8005F958)
            const uint32_t gears = scratch[offsetof(sim::CarParams, gearCount)];
            for (uint32_t k = 2u * (gears + 1u); k < 16 && gears < 8; k++) ourScratch[offsetof(sim::CarParams, gearRatio) + k] = scratch[offsetof(sim::CarParams, gearRatio) + k];
        }
        return changed;
    };
}

// The C library's sprintf cursor (0x8008DEC4 / 0x8008DEA4 keep the output pointer in 0x801C9898; it points into the
// caller's stack afterwards and is reset by the next sprintf): not career state, masked where the routine formats.
Fixup WithSprintfCursor(Fixup inner) {
    return [inner](uint8_t* ours, uint8_t* ourScratch, const uint8_t* ram, const uint8_t* scratch, size_t i) {
        bool changed = inner ? inner(ours, ourScratch, ram, scratch, i) : false;
        std::memcpy(ours + (D(0x801C9898u) & 0x1FFFFF), ram + (D(0x801C9898u) & 0x1FFFFF), 4);
        return changed;
    };
}

// Fills the guest stack below the harness frame with 0xAA before a call, so that the gearbox residue shows up as
// non-zero entries (the dump's stack there is zero, like the gearbox rows' unused entries) and the mask is exercised.
void FillGuestStack(uint8_t* ram) { std::memset(ram + ((kStack - 0x1000) & 0x1FFFFF), 0xAA, 0x1000); }

void ReportMasked(const char* name, uint32_t address, const CaseResult& r, const size_t& masked, int& failures) { // (masked by reference: read after RunCases ran)
    if (r.skipped) {
        std::printf("%-10s skipped (GT2_VERIFY_SKIP)\n", name);
        return;
    }
    std::string extra;
    if (r.unported) extra += ", " + std::to_string(r.unported) + " skipped: branch not ported";
    if (masked) extra += ", " + std::to_string(masked) + " with the gearbox residue masked";
    std::printf("%-10s 0x%08X  %zu cases%s, %zu mismatches  %s\n", name, address, r.cases, extra.c_str(), r.mismatches, r.mismatches ? "FAIL" : "ok");
    failures += r.mismatches ? 1 : 0;
}

} // namespace

bool OverlayMemberLoaded(const uint8_t* ram, const DiscImage& disc, uint32_t member, uint32_t address, uint32_t length) {
    const GuestImage image = LoadOverlayImage(disc, member);
    if (!image.Contains(address, length)) return false;
    return std::memcmp(ram + (address & 0x1FFFFF), image.At(address, length), length) == 0;
}

int VerifyCareer(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol) {
    int failures = 0;
    if (!disc || !vol) {
        std::puts("Career     skipped (needs the disc: car tables, overlays)");
        return 0;
    }
    const bool gtMode = OverlayMemberLoaded(pristine.data(), *disc, 4, 0x80013628u, 0x400);
    const bool race = OverlayMemberLoaded(pristine.data(), *disc, 0, MapCode(0x80059704u), 0x400);
    if (!gtMode && !race) {
        std::puts("Career     skipped (neither the GT-mode overlay (member 4) nor the race overlay (member 0) is loaded)");
        return 0;
    }
    CareerData data = CareerData::Load(*disc, *vol);
    { // the menus' gear generator reads the dirt flag of the last race's course (0x800AF230 -> .crsinfo flags bit 2)
        const uint32_t course = pristine[D(0x800AF230u) & 0x1FFFFF];
        data.menuDirtCourse = (Get<uint16_t>(pristine.data(), D(0x801E18E8u) + course * 24u + 8u) & 4) != 0;
    }
    Randomiser random{rng, data};

    // ---------------------------------------------------------------- race overlay: results
    if (race) {
        CaseResult r = RunCases(
            guest, pristine, "RaceStats", 300, [&](uint8_t* ram, size_t) { random.Garage(ram, 0); for (size_t k = 0; k < 0x60; k++) ram[(D(kCareerRecordAddress) & 0x1FFFFF) + k] = uint8_t(rng()); },
            [&](size_t i) { return guest.Call(0x8005DC64u, D(kCareerRecordAddress), uint32_t(int32_t(i % 8) - 1)); },
            [&](uint8_t* ram, uint8_t*, size_t i) { AddRaceStats(At<CareerRecord>(ram, D(kCareerRecordAddress)), int32_t(i % 8) - 1); return std::nullopt; });
        ReportRow("RaceStats", 0x8005DC64u, r, failures);

        std::vector<int32_t> prizes(400);
        for (int32_t& p : prizes) p = int32_t(rng() % 4 == 0 ? rng() : rng() % 2000000);
        r = RunCases(
            guest, pristine, "PrizeTot", prizes.size(),
            [&](uint8_t* ram, size_t i) {
                CareerRecord& c = At<CareerRecord>(ram, D(kCareerRecordAddress));
                c.prizeTotal = i % 3 == 0 ? 100000000u - uint32_t(rng() % 3000000) : uint32_t(rng());
                c.prizeCarry = i % 5 == 0 ? 0x7FFFFFFF : int32_t(rng() % 100);
            },
            [&](size_t i) { return guest.Call(0x8005DC9Cu, D(kCareerRecordAddress), uint32_t(prizes[i])); },
            [&](uint8_t* ram, uint8_t*, size_t i) { AddPrizeTotal(At<CareerRecord>(ram, D(kCareerRecordAddress)), prizes[i]); return std::nullopt; });
        ReportRow("PrizeTot", 0x8005DC9Cu, r, failures);

        struct ResultArgs { int32_t index, value; };
        std::vector<ResultArgs> ra(600);
        for (ResultArgs& a : ra) a = {int32_t(rng() % 520) - 4, int32_t(rng() % 17) - 1};
        r = RunCases(
            guest, pristine, "SetResult", ra.size(), [&](uint8_t* ram, size_t) { for (size_t k = 0; k < 0x100; k++) ram[(D(kCareerRecordAddress) & 0x1FFFFF) + 0x60 + k] = uint8_t(rng()); },
            [&](size_t i) { return guest.Call(0x8005DBC0u, D(kCareerRecordAddress), uint32_t(ra[i].index), uint32_t(ra[i].value)); },
            [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                if (ra[i].index < 0) return 0u; // the original reads before the array for the test; nothing written
                return uint32_t(SetResult(At<CareerRecord>(ram, D(kCareerRecordAddress)), ra[i].index, ra[i].value));
            });
        ReportRow("SetResult", 0x8005DBC0u, r, failures);

        r = RunCases(
            guest, pristine, "RandIdx", 300, [&](uint8_t* ram, size_t) { Put<uint32_t>(ram, D(kVsyncCounterAddress), rng()); },
            [&](size_t i) { return guest.Call(0x800597C4u, uint32_t(i % 4) + 1); },
            [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> { return RandomIndex(Get<uint32_t>(ram, D(kVsyncCounterAddress)), uint32_t(i % 4) + 1); });
        ReportRow("RandIdx", 0x800597C4u, r, failures);

        // Prize block with prepared cars (garage slots of random catalogue cars) and a random finishing position.
        auto preparePrizes = [&](uint8_t* ram, size_t i) {
            random.Garage(ram, 0, i % 6 == 0 ? int16_t(99) : int16_t(0));
            for (size_t k = 0; k < 0x60; k++) ram[(D(kCareerRecordAddress) & 0x1FFFFF) + k] = uint8_t(rng());
            PrizeBlock& p = At<PrizeBlock>(ram, D(kPrizeBlockAddress));
            p.bonus = int32_t(rng() % 5000000);
            for (int32_t& v : p.prize) v = int32_t(rng() % 1000000);
            p.prizeCarCount = int8_t(rng() % 5);
            for (GarageCar& c : p.prizeCars) {
                uint8_t* b = reinterpret_cast<uint8_t*>(&c);
                for (size_t k = 0; k < sizeof(GarageCar); k++) b[k] = uint8_t(rng());
            }
            Put<int8_t>(ram, D(kRacePositionAddress), int8_t(1 + rng() % 6));
            Put<int16_t>(ram, D(kResultIndexAddress), int16_t(int32_t(rng() % 300) - 20));
            Put<uint32_t>(ram, D(kVsyncCounterAddress), rng());
            Put<uint8_t>(ram, D(kChampionshipFlagAddress), uint8_t(rng() % 2));
            NeutraliseUiCalls(ram, MapCode(0x80059704u), MapCode(0x80059B90u));
        };
        auto uiWord = [&](uint8_t* ram, uint32_t value) { Put<uint32_t>(ram, Get<uint32_t>(ram, D(0x801C90A0u)) + 0x14u, value); };
        r = RunCases(
            guest, pristine, "RaceResult", 600, preparePrizes, [&](size_t) { return guest.Call(0x80059A7Cu); },
            [&](uint8_t* ram, uint8_t*, size_t) {
                ApplyRaceResult(At<CareerRecord>(ram, D(kCareerRecordAddress)), At<GarageBlock>(ram, D(kGarageAddress)), At<PrizeBlock>(ram, D(kPrizeBlockAddress)),
                                Get<int8_t>(ram, D(kRacePositionAddress)), Get<int16_t>(ram, D(kResultIndexAddress)), Get<uint32_t>(ram, D(kVsyncCounterAddress)));
                uiWord(ram, 1);
                return std::nullopt;
            });
        ReportRow("RaceResult", 0x80059A7Cu, r, failures);
        r = RunCases(
            guest, pristine, "ChampRace", 300, preparePrizes, [&](size_t) { return guest.Call(0x80059704u); },
            [&](uint8_t* ram, uint8_t*, size_t) {
                ApplyChampionshipRace(At<CareerRecord>(ram, D(kCareerRecordAddress)), At<GarageBlock>(ram, D(kGarageAddress)), At<PrizeBlock>(ram, D(kPrizeBlockAddress)),
                                      Get<int8_t>(ram, D(kRacePositionAddress)));
                uiWord(ram, 0);
                return std::nullopt;
            });
        ReportRow("ChampRace", 0x80059704u, r, failures);
        // Rolling start 0x8003311C(body, kmh) on the dump's car bodies (some turbo / gearbox fields varied).
        {
            struct RollCase { uint32_t body = 0; int32_t kmh = 0; };
            std::vector<RollCase> rc(600);
            ReportRow("RollStart", 0x8003311Cu,
                      RunCases(
                          guest, pristine, "RollStart", rc.size(),
                          [&](uint8_t* ram, size_t i) {
                              // a set-up car (the gear table of 0x800448C8 non-zero: an empty slot divides by zero in the original)
                              do rc[i].body = kCarBase + uint32_t(i % 3 == 0 ? 0 : rng() % kMaxCars) * kCarStride + kBodyOffset;
                              while (At<sim::CarBody>(ram, rc[i].body).revsPerSpeed[1] == 0);
                              rc[i].kmh = int32_t(i % 5 == 0 ? rng() % 256 : 20 + rng() % 200);
                              sim::CarBody& b = At<sim::CarBody>(ram, rc[i].body);
                              if (i % 2) for (int k = 0; k < 3; k++) b.basis[0][k] = int16_t(int32_t(rng() % 0x2001) - 0x1000);
                              if (i % 4 == 1) b.boostCap = 0;
                              if (i % 4 == 2) { b.boostCap = int16_t(rng() % 0x1000); b.turboBoost[1] = int16_t(rng() % 2 ? 0 : rng() % 0x800); }
                              if (i % 7 == 3) b.forwardGears = uint8_t(1 + rng() % std::max<uint32_t>(1, b.forwardGears));
                          },
                          [&](size_t i) { return guest.Call(0x8003311Cu, rc[i].body, uint32_t(rc[i].kmh)); },
                          [&](uint8_t* ram, uint8_t*, size_t i) {
                              sim::RollingStart(At<sim::CarBody>(ram, rc[i].body), rc[i].kmh);
                              return std::nullopt;
                          }),
                      failures);
        }
        // The settings slider of the race overlay 0x80054D10(widget, pad) (its sound call 0x80060840 replaced by nop).
        if (OverlayMemberLoaded(pristine.data(), *disc, 0, MapCode(0x80054D10u), 0x110)) {
            constexpr uint32_t kWidget = kWork, kPad = kWork + 0x40u;
            ReportRow("Slider", 0x80054D10u,
                      RunCases(
                          guest, pristine, "Slider", 2000,
                          [&](uint8_t* ram, size_t) {
                              for (uint32_t a = MapCode(0x80054D10u); a < MapCode(0x80054E20u); a += 4) {
                                  const uint32_t insn = Get<uint32_t>(ram, a);
                                  if ((insn >> 26) == 3 && (0x80000000u | ((insn & 0x03FFFFFFu) << 2)) == MapCode(0x80060840u)) Put<uint32_t>(ram, a, 0);
                              }
                              for (uint32_t k = 0; k < 0x18; k++) ram[(kWidget & 0x1FFFFF) + k] = uint8_t(rng());
                              const int16_t lo = int16_t(rng() % 3 == 0 ? int32_t(rng()) : int32_t(rng() % 100)), hi = int16_t(lo + int32_t(rng() % 400));
                              Put<int16_t>(ram, kWidget + 0x0C, lo);
                              Put<int16_t>(ram, kWidget + 0x0E, hi);
                              Put<int16_t>(ram, kWidget + 0x0A, int16_t(rng() % 4 == 0 ? int32_t(rng()) : lo + int32_t(rng() % uint32_t(hi - lo + 1))));
                              Put<int16_t>(ram, kWidget + 0x12, int16_t(rng() % 8 == 0 ? -1 : int32_t(rng() % 100)));
                              Put<int16_t>(ram, kWidget + 0x14, int16_t(rng() % 70));
                              static constexpr uint32_t kBits[] = {0x4, 0x8, 0xC, 0x10, 0x1000, 0x1010, 0x14, 0x1008, 0};
                              Put<uint32_t>(ram, kPad + 0, kBits[rng() % 9] | (rng() % 8 == 0 ? rng() : 0));
                              Put<uint32_t>(ram, kPad + 4, kBits[rng() % 9]);
                              Put<uint32_t>(ram, kPad + 8, rng());
                              Put<uint32_t>(ram, kPad + 12, kBits[rng() % 9]);
                          },
                          [&](size_t i) { return guest.Call(0x80054D10u, kWidget, i % 10 == 0 ? 0u : kPad); },
                          [&](uint8_t* ram, uint8_t*, size_t i) {
                              if (Get<int16_t>(ram, kWidget + 0x12) < 0) return std::nullopt;
                              int16_t counter = int16_t(Get<int16_t>(ram, kWidget + 0x14) + 1); // UI blink counter
                              if (counter > 0x3C) counter = 0;
                              Put<int16_t>(ram, kWidget + 0x14, counter);
                              if (i % 10 == 0) return std::nullopt; // no pad record
                              const int32_t delta = SliderDelta(Get<uint32_t>(ram, kPad), Get<uint32_t>(ram, kPad + 4), Get<uint32_t>(ram, kPad + 12));
                              Put<int16_t>(ram, kWidget + 0x0A,
                                           SliderStep(Get<int16_t>(ram, kWidget + 0x0A), Get<int16_t>(ram, kWidget + 0x0C), Get<int16_t>(ram, kWidget + 0x0E), delta));
                              return std::nullopt;
                          }),
                      failures);
        }
        // The settings commit 0x80056FF0 (the race overlay's settings screens, sheet 0x8016E894): a real garage car's
        // sheet with random settings, the race block's garage selector (+0x582 / +0x584) and game mode (+0x0A) varied.
        if (OverlayMemberLoaded(pristine.data(), *disc, 0, MapCode(0x80056FF0u), 0x2D4)) {
            constexpr uint32_t kSheet = kSettingsSheetAddress, kGarageKind = 0x801D5DDEu, kGarageIndex = 0x801D5DE0u, kGameMode = 0x801D5866u;
            constexpr uint32_t kPurchaseSheet = 0x801DA4B8u, kGuestGarage = kGarageAddress + 0x4028u;
            ReportRow("StoreSet", 0x80056FF0u,
                      RunCases(
                          guest, pristine, "StoreSet", 400,
                          [&](uint8_t* ram, size_t i) {
                              random.RealGarage(ram, 0, 1, 12);
                              random.RealGarage(ram, 1, 1, 4);
                              static constexpr int16_t kKinds[] = {0, 0, 0, 1, -1, 2};
                              const int16_t kind = kKinds[i % 6];
                              const GarageBlock& g = At<GarageBlock>(ram, kind == 1 ? D(kGuestGarage) : D(kGarageAddress));
                              const int16_t index = int16_t(rng() % uint32_t(g.count));
                              Put<int16_t>(ram, D(kGarageKind), kind);
                              Put<int16_t>(ram, D(kGarageIndex), index);
                              static constexpr uint8_t kModes[] = {0, 1, 3, 1};
                              Put<uint8_t>(ram, D(kGameMode), kModes[rng() % 4]);
                              TuneSheet& s = At<TuneSheet>(ram, D(kSheet));
                              LoadCarSheet(s, g.cars[index], data.tables);
                              for (int32_t k = 0; k < kSettingCount; k++) {
                                  if (rng() % 3 == 0) continue;
                                  SettingValue v[9]{};
                                  const int32_t n = GetSetting(s, k, v, data);
                                  if (n < 1) continue;
                                  for (int32_t e = 0; e < n; e++)
                                      if (v[e].max >= v[e].min) v[e].value = int16_t(v[e].min + int32_t(rng() % uint32_t(v[e].max - v[e].min + 1)));
                                  SetSetting(s, k, v, n, data);
                              }
                          },
                          [&](size_t) { return guest.CallWithBios(0x80056FF0u); },
                          [&](uint8_t* ram, uint8_t* scratch, size_t) {
                              SettingsCommit out;
                              out.raceRecord = reinterpret_cast<sim::CarParams*>(ram + (D(kSettingsRecordAddress) & 0x1FFFFF));
                              out.raceSlotConfig = &At<CarConfig>(ram, D(kRaceSlotConfigAddress));
                              const int16_t kind = Get<int16_t>(ram, D(kGarageKind));
                              if (kind == 0 || kind == 1) out.garageCar = &At<GarageBlock>(ram, kind == 1 ? D(kGuestGarage) : D(kGarageAddress)).cars[Get<int16_t>(ram, D(kGarageIndex))];
                              if (Get<uint8_t>(ram, D(kGameMode)) == 1) out.purchaseSheet = &At<TuneSheet>(ram, D(kPurchaseSheet));
                              CommitSettings(At<TuneSheet>(ram, D(kSheet)), data.tables, BuildScratch{scratch}, out);
                              return std::nullopt;
                          }),
                      failures);
        }
        // CHANGE PARTS' preview queue 0x800576FC (gt2view/change_parts.h PartsPreview::Step: per call one slot - the sheet's own
        // record (0x8005F410) or a stage's (0x8005EE4C row present: 0x8005F044), 0x80077214, the figures 0x80075930 and the graph
        // samples 0x8007489C) on a real garage car's sheet at 0x8016E894, the whole queue of a random part kind (0x80057654 with
        // every stage of the kind's slots); then 0x80057854 (the graph's maxima). Compared: the done flags, the index and, for
        // every built slot, its figures (+0x288, 0x6C bytes) and the two sample lists (+0x2F4 / +0x394, 80 s16 each).
        if (OverlayMemberLoaded(pristine.data(), *disc, 0, MapCode(0x800576FCu), 0x150)) {
            constexpr uint32_t kSheet = kSettingsSheetAddress, kQueue = kWork, kBuffer = 0x801F8000u, kMaxima = kWork + 0x40u;
            static constexpr int kSlots[23] = {4, 2, 2, 10, 10, 1, 4, 4, 4, 2, 2, 2, 2, 4, 2, 5, 3, 4, 4, 5, 2, 2, 6};
            size_t cases = 0, bad = 0, unported = 0;
            for (size_t i = 0; i < 300 && !SkipCareerRow("PartsPrev"); i++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                std::memset(guest.Scratch(), 0, kScratchSize);
                uint8_t* ram = guest.Ram();
                random.RealGarage(ram, 0, 1, 12);
                const GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress));
                const int16_t index = int16_t(rng() % uint32_t(g.count));
                TuneSheet& s = At<TuneSheet>(ram, D(kSheet));
                LoadCarSheet(s, g.cars[index], data.tables);
                static constexpr int16_t kPower[] = {10, 13, 15, 16, 17};
                const int16_t kind = i % 3 == 0 ? int16_t(rng() % 23) : kPower[rng() % 5];
                if (kind == 5) continue; // table 29 (the wheels): no stage list of the page
                const int stages = kSlots[kind] > 9 ? 9 : kSlots[kind];
                Put<uint32_t>(ram, kQueue + 0, D(kSheet));
                Put<uint32_t>(ram, kQueue + 4, kBuffer);
                Put<int8_t>(ram, kQueue + 8, 0);
                Put<int8_t>(ram, kQueue + 9, int8_t(stages + 1));
                for (uint32_t k = 0; k < 10; k++) Put<uint8_t>(ram, kQueue + 0xA + k, 0);
                for (int k = 0; k < stages; k++) { // the pairs 0x80057654 copies: {kind, stage}
                    Put<int16_t>(ram, kBuffer + uint32_t(k + 1) * 0x434u, kind);
                    Put<int16_t>(ram, kBuffer + uint32_t(k + 1) * 0x434u + 2, int16_t(k));
                }
                const TuneSheet sheet = s;
                screens::PartsPreview ours;
                screens::ChangePartsContext c;
                TuneSheet nativeSheet = sheet;
                c.sheet = &nativeSheet;
                c.data = &data;
                ours.Queue(stages, kind);
                bool lastOurs = false;
                try {
                    for (int k = 0; k <= stages; k++) lastOurs = ours.Step(c);
                } catch (const std::logic_error& e) {
                    if (unported++ == 0) std::printf("    PartsPrev case %zu: not ported: %s\n", i, e.what());
                    continue;
                }
                uint32_t lastOriginal = 0;
                for (int k = 0; k <= stages; k++) lastOriginal = guest.CallWithBios(0x800576FCu, kQueue);
                guest.CallWithBios(0x80057854u, kQueue, kMaxima);
                cases++;
                bool equal = (lastOriginal != 0) == lastOurs && Get<int8_t>(ram, kQueue + 8) == ours.index;
                for (int k = 0; k <= stages && equal; k++) {
                    const bool done = Get<uint8_t>(ram, kQueue + 0xA + uint32_t(k)) != 0;
                    if (done != ours.done[size_t(k)]) equal = false;
                    if (!done || !equal) continue;
                    const uint32_t slot = kBuffer + uint32_t(k) * 0x434u;
                    const screens::PartsPreview::Slot& o = ours.slots[size_t(k)];
                    // the figures' head and the points 0x80075930 writes (the list entries past + 0xA's count keep the buffer's residue)
                    const uint32_t points = std::min<uint32_t>(16, o.Figure(0xA));
                    if (std::memcmp(ram + ((slot + 0x288) & 0x1FFFFF), o.figures.data(), 0xC) != 0) equal = false;
                    for (uint32_t list : {0xCu, 0x2Cu, 0x4Cu})
                        if (std::memcmp(ram + ((slot + 0x288 + list) & 0x1FFFFF), o.figures.data() + list, points * 2) != 0) equal = false;
                    if (std::memcmp(ram + ((slot + 0x2F4) & 0x1FFFFF), o.power.data(), 0xA0) != 0) equal = false;
                    if (std::memcmp(ram + ((slot + 0x394) & 0x1FFFFF), o.torque.data(), 0xA0) != 0) equal = false;
                }
                int rpm = 0, power = 0, torque = 0;
                ours.Maxima(rpm, power, torque);
                if (Get<int32_t>(ram, kMaxima) != rpm || Get<int32_t>(ram, kMaxima + 4) != power || Get<int32_t>(ram, kMaxima + 8) != torque) equal = false;
                if (std::memcmp(&At<TuneSheet>(ram, D(kSheet)), &nativeSheet, sizeof(TuneSheet)) != 0) equal = false; // not written by either
                if (!equal && bad++ < 3) {
                    std::printf("    MISMATCH PartsPrev case %zu (kind %d, %d stages): last %u/%d index %d/%d\n", i, int(kind), stages, lastOriginal, int(lastOurs),
                                int(Get<int8_t>(ram, kQueue + 8)), int(ours.index));
                    for (int k = 0; k <= stages; k++) {
                        const uint32_t slot = kBuffer + uint32_t(k) * 0x434u;
                        const screens::PartsPreview::Slot& o = ours.slots[size_t(k)];
                        int fig = -1, pw = -1, tq = -1;
                        for (int b2 = 0; b2 < 0xC && fig < 0; b2++)
                            if (ram[((slot + 0x288) & 0x1FFFFF) + size_t(b2)] != o.figures[size_t(b2)]) fig = b2;
                        for (int b2 = 0; b2 < 80 && pw < 0; b2++)
                            if (Get<int16_t>(ram, slot + 0x2F4 + uint32_t(b2) * 2) != o.power[size_t(b2)]) pw = b2;
                        for (int b2 = 0; b2 < 80 && tq < 0; b2++)
                            if (Get<int16_t>(ram, slot + 0x394 + uint32_t(b2) * 2) != o.torque[size_t(b2)]) tq = b2;
                        std::printf("      slot %d done %d/%d fig@%d pw@%d tq@%d power %u/%u\n", k, int(Get<uint8_t>(ram, kQueue + 0xA + uint32_t(k))),
                                    int(ours.done[size_t(k)]), fig, pw, tq, unsigned(Get<uint16_t>(ram, slot + 0x288)), unsigned(o.Figure(0)));
                    }
                }
            }
            if (!SkipCareerRow("PartsPrev")) {
                if (unported) std::printf("    PartsPrev: %zu case(s) not ported\n", unported);
                Report("PartsPrev", 0x800576FCu, cases, bad + unported, failures);
            }
        }
        // The power graph widget (EXE; power_graph.h): 0x8007489C on random point lists (count 0..16, rpm ascending or not,
        // negative values), the scales of 0x80073CFC (0x80073DA4) and the tick 0x80073D4C / close 0x80073D30 of random objects.
        if (!SkipCareerRow("PgCurves")) {
            constexpr uint32_t kIn = kWork, kOut = kWork + 0x100u;
            size_t cases = 0, bad = 0;
            for (size_t i = 0; i < 3000; i++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                uint8_t* ram = guest.Ram();
                const int count = int(rng() % 17);
                int16_t rpm[16], pw[16], tq[16];
                int at = int(rng() % 1500);
                for (int k = 0; k < 16; k++) {
                    at += i % 7 == 0 ? int(rng() % 2000) - 500 : int(250 + rng() % 1200);
                    rpm[k] = int16_t(i % 11 == 0 ? int(rng()) : at);
                    pw[k] = int16_t(i % 5 == 0 ? int(rng()) : int(rng() % 600));
                    tq[k] = int16_t(i % 5 == 1 ? int(rng()) : int(rng() % 900));
                }
                for (int k = 0; k < 16; k++) {
                    Put<int16_t>(ram, kIn + uint32_t(k) * 2, rpm[k]);
                    Put<int16_t>(ram, kIn + 0x20 + uint32_t(k) * 2, pw[k]);
                    Put<int16_t>(ram, kIn + 0x40 + uint32_t(k) * 2, tq[k]);
                }
                for (uint32_t k = 0; k < 0x140; k++) ram[((kOut + k) & 0x1FFFFF)] = 0xCD;
                Put<uint32_t>(ram, kStack + 0x10, kOut);         // the fifth / sixth arguments in the caller's slots
                Put<uint32_t>(ram, kStack + 0x14, kOut + 0xA0);
                guest.Call(0x8007489Cu, uint32_t(count), kIn, kIn + 0x20, kIn + 0x40);
                int16_t op[80], ot[80];
                screens::BuildPowerGraphCurves(count, rpm, pw, tq, op, ot);
                cases++;
                if ((std::memcmp(ram + (kOut & 0x1FFFFF), op, 0xA0) != 0 || std::memcmp(ram + ((kOut + 0xA0) & 0x1FFFFF), ot, 0xA0) != 0) && bad++ < 3)
                    std::printf("    MISMATCH PgCurves case %zu (count %d)\n", i, count);
            }
            Report("PgCurves", 0x8007489Cu, cases, bad, failures);
        }
        if (!SkipCareerRow("PgScale")) {
            constexpr uint32_t kObject = kWork;
            size_t cases = 0, bad = 0;
            for (size_t i = 0; i < 3000; i++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                uint8_t* ram = guest.Ram();
                for (uint32_t k = 0; k < 0x40; k++) ram[(kObject + k) & 0x1FFFFF] = uint8_t(rng());
                Put<int16_t>(ram, kObject + 0x20, int16_t(i % 9 == 0 ? int(rng()) : int(rng() % 12000)));
                Put<int16_t>(ram, kObject + 0x22, int16_t(i % 9 == 1 ? int(rng()) : int(rng() % 1200)));
                Put<int16_t>(ram, kObject + 0x24, int16_t(i % 9 == 2 ? int(rng()) : int(rng() % 1500)));
                screens::PowerGraph g;
                g.span = Get<int16_t>(ram, kObject);
                g.anim = Get<int16_t>(ram, kObject + 0x26);
                g.animEnd = Get<int16_t>(ram, kObject + 0x38);
                const int op = int(i % 4);
                if (op == 0) {
                    guest.Call(0x80073CFCu, kObject);
                    g.Open(Get<int16_t>(ram, kObject + 0x20), Get<int16_t>(ram, kObject + 0x22), Get<int16_t>(ram, kObject + 0x24));
                } else if (op == 1) {
                    guest.Call(0x80073D4Cu, kObject);
                    g.Tick();
                } else if (op == 2) {
                    guest.Call(0x80073D30u, kObject);
                    g.Close();
                } else {
                    guest.Call(0x80073CE4u, kObject);
                    g.Reset();
                }
                cases++;
                bool equal = Get<int16_t>(ram, kObject + 0x26) == g.anim && Get<int16_t>(ram, kObject + 0x38) == g.animEnd;
                if (op == 0) {
                    const int16_t want[7] = {g.powerScale, g.powerTicks, g.powerStep, g.torqueScale, g.torqueTicks, g.torqueStep, g.rpmTicks};
                    for (uint32_t k = 0; k < 7; k++) equal = equal && Get<int16_t>(ram, kObject + 0x28 + k * 2) == want[k];
                    equal = equal && Get<int16_t>(ram, kObject + 0x36) == g.samples;
                }
                if (!equal && bad++ < 3) std::printf("    MISMATCH PgScale case %zu (op %d)\n", i, op);
            }
            Report("PgScale", 0x80073DA4u, cases, bad, failures);
        }
        // The licence test result 0x8004DD80 (race overlay; licence dumps: game mode 3 and the licence database at
        // *0x80092E6C): a random test named at 0x801D586C, random licence / test bytes of the race block, random
        // career licence records and result times around the test's medal times.
        if (vol && Get<uint8_t>(pristine.data(), D(0x801D5866u)) == 3 && Get<uint32_t>(pristine.data(), D(0x80092E6Cu)) != 0 &&
            OverlayMemberLoaded(pristine.data(), *disc, 0, 0x8004DD80u, 0x184)) {
            const LicenseData lic = LicenseData::Load(*vol);
            struct LicCase { size_t row = 0; int32_t licence = 0, test = 0; uint32_t time = 0; };
            std::vector<LicCase> lc(1200);
            ReportRow("LicRecord", 0x8004DD80u,
                      RunCases(
                          guest, pristine, "LicRecord", lc.size(),
                          [&](uint8_t* ram, size_t i) {
                              LicCase& c = lc[i];
                              c.row = rng() % lic.TestCount();
                              const LicenseTest t = lic.TestAt(c.row);
                              for (uint32_t k = 0; k < 16; k++) Put<char>(ram, D(0x801D586Cu) + k, k < t.name.size() ? t.name[k] : '\0');
                              c.licence = int32_t(rng() % kLicenceCount);
                              c.test = int32_t(rng() % kLicenceTests);
                              Put<uint8_t>(ram, D(kRaceLicenceAddress), uint8_t(c.licence));
                              Put<uint8_t>(ram, D(kRaceLicenceTestAddress), uint8_t(c.test));
                              const uint32_t medal = t.MedalTime(1 + rng() % 4);
                              const uint32_t r = rng() % 6;
                              c.time = r == 0 ? rng() : r == 1 ? medal : r == 2 ? medal - 1 : uint32_t(int32_t(medal) + int32_t(rng() % 2001) - 1000);
                              Put<uint32_t>(ram, D(kLicenceTimeAddress), c.time);
                              CareerState& s = At<CareerState>(ram, D(kStateAddress));
                              for (auto& l : s.licences)
                                  for (LicenceTestRecord& rec : l) {
                                      static constexpr uint8_t kPassed[] = {0, 0, 0, 1, 2, 3, 4, 0xFF, 0x80};
                                      rec.passed = rng() % 5 == 0 ? uint8_t(rng()) : kPassed[rng() % 9];
                                      const uint8_t threshold = t.settings[0x30];
                                      rec.byte2 = rng() % 4 == 0 ? uint8_t(rng()) : rng() % 3 == 0 ? uint8_t(0xFF) : uint8_t(threshold + int32_t(rng() % 3) - 2);
                                  }
                          },
                          [&](size_t) { return guest.Call(0x8004DD80u); },
                          [&](uint8_t* ram, uint8_t*, size_t i) {
                              const LicenseTest t = lic.TestAt(lc[i].row);
                              RecordLicenceResult(At<CareerState>(ram, D(kStateAddress)), lc[i].licence, lc[i].test, lc[i].time, t.settings.data());
                              return std::nullopt;
                          }),
                      failures);
        }
    }

    if (!gtMode) {
        std::puts("Career     garage rows skipped (the GT-mode overlay, GT2.OVL member 4, is not loaded in this dump)");
        return failures;
    }

    // ---------------------------------------------------------------- GT-mode overlay: new game, garage, money
    {
        const GuestImage exe = LoadExeImage(*disc);
        const uint16_t courses = Get<uint16_t>(pristine.data(), D(0x801E18E6u));
        const CourseInfoTable info = ParseCourseInfo(vol->Read(".crsinfo"));
        if (courses != info.entries.size()) {
            std::printf("    MISMATCH course count: dump 0x801E18E6 = %u, .crsinfo %zu\n", courses, info.entries.size());
            failures++;
        }
        const NewGameDefaults defaults = ReadNewGameDefaults(exe, courses);
        const CaseResult r = RunCases(
            guest, pristine, "NewGame", 40,
            [&](uint8_t* ram, size_t) {
                for (uint32_t k = 0; k < kSavedStateSize + 0x4028u; k++) ram[(D(kStateAddress) & 0x1FFFFF) + k] = uint8_t(rng());
                std::memcpy(ram + (0x800104A0u & 0x1FFFFF), exe.At(0x800104A0u, 0x800107E8u - 0x800104A0u), 0x800107E8u - 0x800104A0u); // the boot block
            },
            [&](size_t) { return guest.Call(0x800104A0u); },
            [&](uint8_t* ram, uint8_t*, size_t) {
                InitNewCareer(At<CareerState>(ram, D(kStateAddress)), &At<GarageBlock>(ram, D(kStateAddress) + uint32_t(kSavedStateSize)), defaults);
                return std::nullopt;
            });
        ReportRow("NewGame", 0x800104A0u, r, failures);
    }
    std::vector<int32_t> amounts(500);
    for (int32_t& a : amounts) a = rng() % 3 == 0 ? int32_t(rng()) : int32_t(rng() % 4000000) - 2000000;
    ReportRow("AddMoney", 0x8005E7B0u,
              RunCases(
                  guest, pristine, "AddMoney", amounts.size(), [&](uint8_t* ram, size_t) { random.Garage(ram, 0); random.Garage(ram, 1); },
                  [&](size_t i) { return guest.Call(0x8005E7B0u, D(kGarageAddress) + uint32_t(i % 2) * 0x4028u, uint32_t(amounts[i])); },
                  [&](uint8_t* ram, uint8_t*, size_t i) { AddMoney(At<GarageBlock>(ram, D(kGarageAddress) + uint32_t(i % 2) * 0x4028u), amounts[i]); return std::nullopt; }),
              failures);
    ReportRow("CanBuy", 0x80017914u,
              RunCases(
                  guest, pristine, "CanBuy", amounts.size(), [&](uint8_t* ram, size_t i) { random.Garage(ram, 0, i % 5 == 0 ? int16_t(100) : int16_t(0)); random.Garage(ram, 1); },
                  [&](size_t i) { return guest.Call(0x80017914u, 0, uint32_t(amounts[i] < 0 ? -amounts[i] : amounts[i]), uint32_t(i % 2)); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                      return uint32_t(CanBuy(At<GarageBlock>(ram, D(kGarageAddress) + uint32_t(i % 2) * 0x4028u), amounts[i] < 0 ? -amounts[i] : amounts[i]));
                  }),
              failures);
    ReportRow("AddCar", 0x8005E7F0u,
              RunCases(
                  guest, pristine, "AddCar", 300,
                  [&](uint8_t* ram, size_t i) {
                      random.Garage(ram, 0, i % 4 == 0 ? int16_t(99) : int16_t(0));
                      for (uint32_t k = 0; k < sizeof(GarageCar); k++) ram[(kWork & 0x1FFFFF) + k] = uint8_t(rng());
                  },
                  [&](size_t) { return guest.Call(0x8005E7F0u, D(kGarageAddress), kWork); },
                  [&](uint8_t* ram, uint8_t*, size_t) -> std::optional<uint32_t> { return uint32_t(AddPreparedCar(At<GarageBlock>(ram, D(kGarageAddress)), At<GarageCar>(ram, kWork))); }),
              failures);
    std::vector<std::array<int32_t, 2>> moves(400);
    ReportRow("RemoveCar", 0x8001EDACu,
              RunCases(
                  guest, pristine, "RemoveCar", moves.size(),
                  [&](uint8_t* ram, size_t i) {
                      random.Garage(ram, 0, 1);
                      const int16_t count = At<GarageBlock>(ram, D(kGarageAddress)).count;
                      moves[i][0] = int32_t(rng() % uint32_t(count));
                  },
                  [&](size_t i) { return guest.Call(0x8001EDACu, D(kGarageAddress), uint32_t(moves[i][0])); },
                  [&](uint8_t* ram, uint8_t*, size_t i) { RemoveCar(At<GarageBlock>(ram, D(kGarageAddress)), moves[i][0]); return std::nullopt; }),
              failures);
    ReportRow("MoveCar", 0x8001EF10u,
              RunCases(
                  guest, pristine, "MoveCar", moves.size(),
                  [&](uint8_t* ram, size_t i) {
                      random.Garage(ram, 0, 1);
                      const int16_t count = At<GarageBlock>(ram, D(kGarageAddress)).count;
                      moves[i] = {int32_t(rng() % uint32_t(count)), int32_t(rng() % uint32_t(count))};
                  },
                  [&](size_t i) { return guest.Call(0x8001EF10u, D(kGarageAddress), uint32_t(moves[i][0]), uint32_t(moves[i][1])); },
                  [&](uint8_t* ram, uint8_t*, size_t i) { MoveCar(At<GarageBlock>(ram, D(kGarageAddress)), moves[i][0], moves[i][1]); return std::nullopt; }),
              failures);

    // ---- parts owned
    const uint32_t firstSlot = D(kGarageAddress) + 4u;
    ReportRow("PartOwned", 0x8005E874u,
              RunCases(
                  guest, pristine, "PartOwned", 400, [&](uint8_t* ram, size_t) { random.Garage(ram, 0, 1); },
                  [&](size_t i) { return guest.Call(0x8005E874u, firstSlot, uint32_t(i % 56)); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> { return PartOwned(At<GarageCar>(ram, firstSlot), int32_t(i % 56)) ? 1u : 0u; }),
              failures);
    ReportRow("SetPartBit", 0x8005E900u,
              RunCases(
                  guest, pristine, "SetPartBit", 200, [&](uint8_t* ram, size_t) { random.Garage(ram, 0, 1); },
                  [&](size_t i) { return guest.Call(0x8005E900u, uint32_t(i % 56), firstSlot + 0x9Au); },
                  [&](uint8_t* ram, uint8_t*, size_t i) { SetPartBit(At<GarageCar>(ram, firstSlot).partsOwned, int32_t(i % 56)); return std::nullopt; }),
              failures);
    ReportRow("AddPart", 0x8005E8B0u,
              RunCases(
                  guest, pristine, "AddPart", 200, [&](uint8_t* ram, size_t) { random.Garage(ram, 0, 1); },
                  [&](size_t i) { return guest.Call(0x8005E8B0u, firstSlot, uint32_t(i % 56), uint32_t(amounts[i])); },
                  [&](uint8_t* ram, uint8_t*, size_t i) { AddPart(At<GarageCar>(ram, firstSlot), int32_t(i % 56), amounts[i]); return std::nullopt; }),
              failures);

    // ---- table lookups (every part kind for random catalogue cars, and the generic "00000")
    std::vector<uint32_t> cars(600);
    for (size_t i = 0; i < cars.size(); i++) cars[i] = i % 50 == 0 ? kNoPartId : random.RandomCatalogueCar();
    ReportRow("PartRow", 0x80076570u,
              RunCases(
                  guest, pristine, "PartRow", cars.size() * 2, [&](uint8_t* ram, size_t) { Put<uint32_t>(ram, kWork, rng()); },
                  [&](size_t i) { return guest.Call(0x80076570u, uint32_t(i % 0x32), cars[i / 2], kWork); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                      uint32_t price = 0;
                      const int32_t row = PartRow(data.tables, int32_t(i % 0x32), cars[i / 2], &price);
                      Put<uint32_t>(ram, kWork, price);
                      return uint32_t(row);
                  }),
              failures);
    ReportRow("RmRow", 0x80076500u,
              RunCases(
                  guest, pristine, "RmRow", cars.size(), [&](uint8_t* ram, size_t) { Put<uint32_t>(ram, kWork, rng()); },
                  [&](size_t i) { return guest.Call(0x80076500u, cars[i], uint32_t(i % 6), kWork); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                      uint32_t price = 0;
                      const int32_t row = RacingModifyStageRow(data.tables, cars[i], uint8_t(i % 6), &price);
                      Put<uint32_t>(ram, kWork, price);
                      return uint32_t(row);
                  }),
              failures);
    ReportRow("CatPrice", 0x800177D4u,
              RunCases(
                  guest, pristine, "CatPrice", cars.size(), [&](uint8_t*, size_t) {}, [&](size_t i) { return guest.Call(0x800177D4u, cars[i]); },
                  [&](uint8_t*, uint8_t*, size_t i) -> std::optional<uint32_t> { return uint32_t(CataloguePrice(data.tables, cars[i])); }),
              failures);
    ReportRow("SellCar", 0x80017A70u,
              RunCases(
                  guest, pristine, "SellCar", 300,
                  [&](uint8_t* ram, size_t i) {
                      random.Garage(ram, 0, 1);
                      moves[i][0] = int32_t(rng() % uint32_t(At<GarageBlock>(ram, D(kGarageAddress)).count));
                  },
                  [&](size_t i) { return guest.Call(0x80017A70u, uint32_t(moves[i][0]), 0); },
                  [&](uint8_t* ram, uint8_t*, size_t i) { SellCar(At<GarageBlock>(ram, D(kGarageAddress)), moves[i][0], data.tables); return std::nullopt; }),
              failures);
    auto preparePart = [&](uint8_t* ram, size_t i) {
        random.Garage(ram, 0, 1);
        GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress));
        moves[i % moves.size()][0] = int32_t(rng() % uint32_t(g.count));
        if (i % 3 == 0) g.money = int32_t(rng() % 2000000);
    };
    ReportRow("PartCheck", 0x80017B40u,
              RunCases(
                  guest, pristine, "PartCheck", 1200, preparePart,
                  [&](size_t i) { return guest.Call(0x80017B40u, uint32_t(moves[i % moves.size()][0]), uint32_t(i % 0x32), 0); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                      return uint32_t(PartPurchaseCheck(At<GarageBlock>(ram, D(kGarageAddress)), moves[i % moves.size()][0], int32_t(i % 0x32), data.tables));
                  }),
              failures);
    ReportRow("BuyPart", 0x80017C98u,
              RunCases(
                  guest, pristine, "BuyPart", 1200, preparePart,
                  [&](size_t i) { return guest.Call(0x80017C98u, uint32_t(moves[i % moves.size()][0]), uint32_t(i % 0x32), 0); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                      return uint32_t(BuyPart(At<GarageBlock>(ram, D(kGarageAddress)), moves[i % moves.size()][0], int32_t(i % 0x32), data.tables));
                  }),
              failures);

    // ---- licences
    auto prepareLicences = [&](uint8_t* ram, size_t i) {
        CareerState& s = At<CareerState>(ram, D(kStateAddress));
        const uint32_t mode = uint32_t(i % 4);
        for (auto& l : s.licences)
            for (LicenceTestRecord& t : l) t.passed = mode == 0 ? uint8_t(rng() % 2) : mode == 1 ? uint8_t(rng() % 8 != 0) : uint8_t(rng());
    };
    ReportRow("LicHeld", 0x8001915Cu,
              RunCases(
                  guest, pristine, "LicHeld", 300, prepareLicences, [&](size_t i) { return guest.Call(0x8001915Cu, uint32_t(i % 6)); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> { return LicenceHeld(At<CareerState>(ram, D(kStateAddress)), int32_t(i % 6)) ? 1u : 0u; }),
              failures);
    ReportRow("LicLevel", 0x800191C4u,
              RunCases(
                  guest, pristine, "LicLevel", 300, prepareLicences, [&](size_t) { return guest.Call(0x800191C4u); },
                  [&](uint8_t* ram, uint8_t*, size_t) -> std::optional<uint32_t> { return uint32_t(LicenceLevel(At<CareerState>(ram, D(kStateAddress)))); }),
              failures);

    // ---- tune sheet and purchase
    std::vector<uint32_t> buyCars(160);
    for (uint32_t& c : buyCars) c = random.RandomCatalogueCar();
    ReportRow("TuneLoad", 0x80015428u,
              RunCases(
                  guest, pristine, "TuneLoad", buyCars.size(), [&](uint8_t*, size_t) {},
                  [&](size_t i) {
                      guest.CallWithBios(0x80015404u, D(kTuneSheetAddress));
                      return guest.CallWithBios(0x80015428u, D(kTuneSheetAddress), buyCars[i]);
                  },
                  [&](uint8_t* ram, uint8_t*, size_t i) {
                      TuneSheet& s = At<TuneSheet>(ram, D(kTuneSheetAddress));
                      ClearTuneSheet(s);
                      LoadTuneSheet(s, buyCars[i], data.tables);
                      return std::nullopt;
                  }),
              failures);
    const Fixup gearMask = GearResidueFixup(data.tables);
    size_t masked = 0;
    ReportMasked("Purchase", 0x80017750u,
              RunCases(
                  guest, pristine, "Purchase", buyCars.size(),
                  [&](uint8_t* ram, size_t i) {
                      const std::optional<CarConfig> c = CatalogueCarConfig(data.tables, buyCars[i]);
                      std::memcpy(ram + (kWork & 0x1FFFFF), &*c, sizeof(CarConfig));
                      FillGuestStack(ram);
                  },
                  [&](size_t i) { return guest.CallWithBios(0x80017750u, buyCars[i], kWork); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                      return AnalysePurchase(At<TuneSheet>(ram, D(kTuneSheetAddress)), buyCars[i], At<CarConfig>(ram, kWork), data) ? 1u : 0u;
                  },
                  gearMask, &masked),
              masked, failures);
    ReportRow("PowerFig", 0x80075930u,
              RunCases(
                  guest, pristine, "PowerFig", buyCars.size(),
                  [&](uint8_t*, size_t i) {
                      CarConfig c = *CatalogueCarConfig(data.tables, buyCars[i]);
                      sim::CarParams record;
                      BuildMenuRecord(data.tables, c, record);
                      std::memcpy(guest.Scratch(), &record, sizeof(record));
                  },
                  [&](size_t) { return guest.Call(0x80075930u, 0x1F800000u, 0x1F8001C0u); },
                  [&](uint8_t*, uint8_t* scratch, size_t) -> std::optional<uint32_t> {
                      return CarPowerFigures(*reinterpret_cast<sim::CarParams*>(scratch), scratch + 0x1C0);
                  }),
              failures);
    masked = 0;
    ReportMasked("BuyCar", 0x8001796Cu,
              RunCases(
                  guest, pristine, "BuyCar", buyCars.size(),
                  [&](uint8_t* ram, size_t i) {
                      random.Garage(ram, 0, i % 8 == 0 ? int16_t(100) : int16_t(0));
                      random.Garage(ram, 1);
                      moves[i][0] = int32_t(rng() % 400000);
                      FillGuestStack(ram);
                  },
                  [&](size_t i) { return guest.CallWithBios(0x8001796Cu, buyCars[i], 0x61u + uint32_t(i % 26), uint32_t(moves[i][0]), uint32_t(i % 2)); },
                  [&](uint8_t* ram, uint8_t* scratch, size_t i) -> std::optional<uint32_t> {
                      return uint32_t(BuyCar(At<GarageBlock>(ram, D(kGarageAddress) + uint32_t(i % 2) * 0x4028u), buyCars[i], 0x61u + uint32_t(i % 26), moves[i][0], data,
                                             At<TuneSheet>(ram, D(kTuneSheetAddress)), BuildScratch{scratch}));
                  },
                  gearMask, &masked),
              masked, failures);

    // ---------------------------------------------------------------- fitting parts, wheels, settings (src/game/career/tuning.*)
    {
        struct TuneCase { int32_t index = 0, kind = 0, extra = 0; uint32_t paint = 0; };
        std::vector<TuneCase> tc(1200);
        // A garage of bought (and tuned) cars; the menus' sheet of car `index` loaded by the original first.
        auto prepareGarage = [&](uint8_t* ram, size_t i) {
            random.RealGarage(ram, 0, 1, 6);
            random.Garage(ram, 1);
            GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress));
            TuneCase& c = tc[i % tc.size()];
            c.index = int32_t(rng() % uint32_t(g.count));
            c.kind = int32_t(rng() % 0x32);
            c.paint = 0x61u + rng() % 26;
            // the body chosen on the fit page: 1..the car's body count (0x800174F4 / 0x80017530 keep it there)
            auto sheet = std::make_unique<TuneSheet>();
            LoadCarSheet(*sheet, g.cars[c.index], data.tables);
            Put<int16_t>(ram, D(kRacingBodyAddress), int16_t(1 + rng() % uint32_t(std::max(1, RacingBodyCount(*sheet)))));
            FillGuestStack(ram);
        };
        auto loadSheet = [&](uint8_t* ram, int32_t index) { LoadCarSheet(At<TuneSheet>(ram, D(kCarSheetAddress)), At<GarageBlock>(ram, D(kGarageAddress)).cars[index], data.tables); };
        size_t tuneMasked = 0;
        ReportMasked("CarSheet", 0x800173E8u,
                     RunCases(
                         guest, pristine, "CarSheet", 200, prepareGarage, [&](size_t i) { return guest.CallWithBios(0x800173E8u, uint32_t(tc[i].index), 0); },
                         [&](uint8_t* ram, uint8_t*, size_t i) { loadSheet(ram, tc[i].index); return std::nullopt; }, gearMask, &tuneMasked),
                     tuneMasked, failures);
        tuneMasked = 0;
        ReportMasked("FitPart", 0x80017D6Cu,
                     RunCases(
                         guest, pristine, "FitPart", 600, prepareGarage,
                         [&](size_t i) {
                             guest.CallWithBios(0x800173E8u, uint32_t(tc[i].index), 0);
                             return guest.CallWithBios(0x80017D6Cu, uint32_t(tc[i].index), uint32_t(tc[i].kind), 0, tc[i].paint);
                         },
                         [&](uint8_t* ram, uint8_t* scratch, size_t i) -> std::optional<uint32_t> {
                             loadSheet(ram, tc[i].index);
                             return uint32_t(FitPart(At<GarageBlock>(ram, D(kGarageAddress)), tc[i].index, tc[i].kind, tc[i].paint, At<TuneSheet>(ram, D(kCarSheetAddress)),
                                                     Get<int16_t>(ram, D(kRacingBodyAddress)), data, BuildScratch{scratch}));
                         },
                         gearMask, &tuneMasked),
                     tuneMasked, failures);
        tuneMasked = 0;
        ReportMasked("RaceTyres", 0x80018004u,
                     RunCases(
                         guest, pristine, "RaceTyres", 300,
                         [&](uint8_t* ram, size_t i) {
                             prepareGarage(ram, i);
                             GarageCar& car = At<GarageBlock>(ram, D(kGarageAddress)).cars[tc[i].index];
                             if (rng() % 2) { // dirt tyres fitted (stage 7 front and rear) when the car has them
                                 auto sheet = std::make_unique<TuneSheet>();
                                 LoadCarSheet(*sheet, car, data.tables);
                                 if (sheet->tyresFrontRow[7] != 0xFFFFFFFFu && sheet->tyresRearRow[7] != 0xFFFFFFFFu) {
                                     SetTuneStage(*sheet, kTuneTyresFront, 7, data);
                                     SetTuneStage(*sheet, kTuneTyresRear, 7, data);
                                     car.config = sheet->config;
                                 }
                             }
                             tc[i].extra = int32_t(rng() % 2);
                         },
                         [&](size_t i) { return guest.CallWithBios(0x80018004u, uint32_t(tc[i].index), 0, uint32_t(tc[i].extra)); },
                         [&](uint8_t* ram, uint8_t* scratch, size_t i) {
                             PrepareRaceTyres(At<GarageBlock>(ram, D(kGarageAddress)), tc[i].index, tc[i].extra != 0, At<TuneSheet>(ram, D(kCarSheetAddress)), data,
                                              BuildScratch{scratch});
                             return std::nullopt;
                         },
                         gearMask, &tuneMasked),
                     tuneMasked, failures);
        std::vector<uint32_t> wheelIds;
        for (size_t r = 0; r < data.tables.RowCount(kCarProfileTable); r++) {
            const std::span<const uint8_t> row = data.tables.Row(kCarProfileTable, r);
            wheelIds.push_back(uint32_t(row[0]) | uint32_t(row[1]) << 8 | uint32_t(row[2]) << 16 | uint32_t(row[3]) << 24);
        }
        tuneMasked = 0;
        ReportMasked("Wheels", 0x80018100u,
                     RunCases(
                         guest, pristine, "Wheels", 300,
                         [&](uint8_t* ram, size_t i) {
                             prepareGarage(ram, i);
                             tc[i].paint = wheelIds[rng() % wheelIds.size()];
                             tc[i].extra = int32_t(rng() % 60000);
                         },
                         [&](size_t i) {
                             guest.CallWithBios(0x800173E8u, uint32_t(tc[i].index), 0);
                             return guest.CallWithBios(0x80018100u, uint32_t(tc[i].index), tc[i].paint, uint32_t(tc[i].extra), 0);
                         },
                         [&](uint8_t* ram, uint8_t* scratch, size_t i) -> std::optional<uint32_t> {
                             loadSheet(ram, tc[i].index);
                             return uint32_t(BuyWheels(At<GarageBlock>(ram, D(kGarageAddress)), tc[i].index, tc[i].paint, tc[i].extra, At<TuneSheet>(ram, D(kCarSheetAddress)), data,
                                                       BuildScratch{scratch}));
                         },
                         gearMask, &tuneMasked),
                     tuneMasked, failures);
        ReportRow("Bodies", 0x80017530u,
                  RunCases(
                      guest, pristine, "Bodies", 200,
                      [&](uint8_t* ram, size_t i) {
                          prepareGarage(ram, i);
                          loadSheet(ram, tc[i].index);
                          Put<int16_t>(ram, D(kRacingBodyAddress), int16_t(rng() % 6));
                      },
                      [&](size_t i) { return guest.Call(i % 3 == 0 ? 0x800174F4u : 0x80017530u); },
                      [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                          int16_t& body = At<int16_t>(ram, D(kRacingBodyAddress));
                          const TuneSheet& sheet = At<TuneSheet>(ram, D(kCarSheetAddress));
                          return i % 3 == 0 ? FirstRacingBody(sheet, body) : NextRacingBody(sheet, body);
                      }),
                  failures);

        // The parts page's power preview of the current car (0x8001DB90 on the transaction object at kWork).
        ReportRow("PartPower", 0x8001DB90u,
                  RunCases(
                      guest, pristine, "PartPower", 600,
                      [&](uint8_t* ram, size_t i) {
                          prepareGarage(ram, i);
                          GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress));
                          g.currentCar = int16_t(tc[i].index);
                          loadSheet(ram, tc[i].index);
                          for (uint32_t k = 0; k < 0x40; k++) ram[(kWork & 0x1FFFFF) + k] = uint8_t(rng());
                      },
                      [&](size_t i) { return guest.CallWithBios(0x8001DB90u, kWork, uint32_t(tc[i].kind)); },
                      [&](uint8_t* ram, uint8_t*, size_t i) {
                          const GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress));
                          const PartPreview p = PreviewPart(g.cars[tc[i].index], At<TuneSheet>(ram, D(kCarSheetAddress)), tc[i].kind, data);
                          Put<int32_t>(ram, kWork + 0x30, p.price);
                          Put<int32_t>(ram, kWork + 0x24, p.owned);
                          Put<int32_t>(ram, kWork + 0x1C, p.powerBefore);
                          Put<int32_t>(ram, kWork + 0x20, p.powerAfter);
                          return std::nullopt;
                      }),
                  failures);
        // Wheel codes of the wheel shop (0x80013A28) and the colour byte (0x80021BEC).
        {
            std::vector<std::string> codes(400);
            const std::string makers = "bbbrduenfaozraspyoxx";
            for (std::string& c : codes) {
                c = makers.substr(2 * (rng() % 10), 2);
                for (int k = 0; k < 3; k++) c.push_back(char('0' + rng() % 10));
                c.push_back('-');
                c.push_back("-3456"[rng() % 5]);
                c.push_back(char(rng() % 3 == 0 ? '-' : 'a' + rng() % 26));
            }
            ReportRow("WheelId", 0x80013A28u,
                      RunCases(
                          guest, pristine, "WheelId", codes.size(),
                          [&](uint8_t* ram, size_t i) { std::memset(ram + (kWork & 0x1FFFFF), 0, 16); std::memcpy(ram + (kWork & 0x1FFFFF), codes[i].c_str(), 8); },
                          [&](size_t) { return guest.Call(0x80013A28u, kWork); },
                          [&](uint8_t*, uint8_t*, size_t i) -> std::optional<uint32_t> { return WheelIdOfCode(data, codes[i]); }),
                      failures);
            ReportRow("WheelCol", 0x80021BECu,
                      RunCases(
                          guest, pristine, "WheelCol", 300, [&](uint8_t*, size_t) {},
                          [&](size_t i) { return guest.Call(0x80021BECu, wheelIds[i % wheelIds.size()], uint32_t(i % 2)); },
                          [&](uint8_t*, uint8_t*, size_t i) -> std::optional<uint32_t> { return WheelColour(data, wheelIds[i % wheelIds.size()], i % 2 != 0); }),
                      failures);
        }

        // Settings of the loaded sheet: 0x8005FC9C / 0x8005F9DC / 0x80060410 for all 17 settings.
        auto prepareSettings = [&](uint8_t* ram, size_t i) {
            prepareGarage(ram, i);
            loadSheet(ram, tc[i].index);
            tc[i].kind = int32_t(i % kSettingCount);
            for (uint32_t k = 0; k < 0x48; k++) ram[(kWork & 0x1FFFFF) + k] = uint8_t(rng());
        };
        ReportRow("GetSet", 0x8005FC9Cu,
                  RunCases(
                      guest, pristine, "GetSet", 1200, prepareSettings,
                      [&](size_t i) { return guest.CallWithBios(0x8005FC9Cu, D(kCarSheetAddress), uint32_t(tc[i].kind), kWork); },
                      [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                          return uint32_t(GetSetting(At<TuneSheet>(ram, D(kCarSheetAddress)), tc[i].kind, &At<SettingValue>(ram, kWork), data));
                      }),
                  failures);
        tuneMasked = 0;
        ReportMasked("SetSet", 0x8005F9DCu,
                     RunCases(
                         guest, pristine, "SetSet", 1200,
                         [&](uint8_t* ram, size_t i) {
                             prepareSettings(ram, i);
                             SettingValue* v = &At<SettingValue>(ram, kWork);
                             const int32_t count = GetSetting(At<TuneSheet>(ram, D(kCarSheetAddress)), tc[i].kind, v, data);
                             tc[i].extra = count > 0 ? count : int32_t(1 + rng() % 2);
                             for (int32_t k = 0; k < tc[i].extra; k++) { // move the values inside their ranges (sometimes anywhere)
                                 // (not for the auto-set top speed: 0 makes 0x80074B38 divide by zero, a break exception)
                                 if (rng() % 4 == 0 && tc[i].kind != kSettingGearAuto) v[k].value = int16_t(rng());
                                 else if (v[k].max > v[k].min) v[k].value = int16_t(v[k].min + int32_t(rng() % uint32_t(v[k].max - v[k].min + 1)));
                                 if (tc[i].kind >= kSettingLsdInitial && tc[i].kind <= kSettingLsdRearInitial && (count < 1 || rng() % 4 == 0)) v[k].field = int16_t(rng() % 6);
                             }
                             if (tc[i].kind == kSettingGearAuto && uint8_t(v[0].value) == 0) v[0].value = 20;
                         },
                         [&](size_t i) { return guest.CallWithBios(0x8005F9DCu, D(kCarSheetAddress), uint32_t(tc[i].kind), kWork, uint32_t(tc[i].extra)); },
                         [&](uint8_t* ram, uint8_t*, size_t i) {
                             SetSetting(At<TuneSheet>(ram, D(kCarSheetAddress)), tc[i].kind, &At<SettingValue>(ram, kWork), tc[i].extra, data);
                             return std::nullopt;
                         },
                         gearMask, &tuneMasked),
                     tuneMasked, failures);
        tuneMasked = 0;
        ReportMasked("DfltSet", 0x80060410u,
                     RunCases(
                         guest, pristine, "DfltSet", 1200, prepareSettings, [&](size_t i) { return guest.CallWithBios(0x80060410u, D(kCarSheetAddress), uint32_t(tc[i].kind)); },
                         [&](uint8_t* ram, uint8_t*, size_t i) {
                             DefaultSetting(At<TuneSheet>(ram, D(kCarSheetAddress)), tc[i].kind, data);
                             return std::nullopt;
                         },
                         gearMask, &tuneMasked),
                     tuneMasked, failures);
    }

    // ---------------------------------------------------------------- events
    const EventMenuData menu = EventMenuData::Load(data.ovl4);
    const std::vector<uint8_t> raceFile = vol->Read("carparam/usa_gtmode_race.dat");
    const GuestImage exeImage = LoadExeImage(*disc);
    const uint8_t language = Get<uint8_t>(pristine.data(), D(kStateAddress));
    const EventInfoTable infos = BuildEventInfos(data, menu);
    { // the menu's info records (built at the GT-mode entry, 0x80019474) against the dump's
        size_t bad = 0;
        const uint8_t* dumpInfos = &pristine[D(kEventInfoAddress) & 0x1FFFFF];
        if (std::memcmp(dumpInfos, infos.infos.data(), infos.infos.size() * sizeof(EventInfo)) != 0) bad++;
        for (size_t k = 0; k < infos.carListPool.size(); k++)
            if (Get<uint32_t>(pristine.data(), D(kCarListPoolAddress) + uint32_t(k) * 4) != infos.carListPool[k]) { bad++; break; }
        for (size_t i = 0; i < infos.infos.size() && bad; i++)
            if (std::memcmp(dumpInfos + i * sizeof(EventInfo), &infos.infos[i], sizeof(EventInfo)) != 0) {
                std::printf("    MISMATCH event info %zu (%s)\n", i, menu.events[i].c_str());
                break;
            }
        std::printf("%-10s 0x%08X  %zu events, %zu car-list ids, %zu mismatches  %s\n", "EventInfo", 0x80019474u, infos.infos.size(), infos.carListPool.size(), bad,
                    bad ? "FAIL" : "ok");
        failures += bad ? 1 : 0;
    }
    std::vector<std::string> names(600);
    for (std::string& n : names) {
        n = menu.events[rng() % menu.events.size()];
        if (rng() % 16 == 0) n = menu.prefixes[rng() % 3] + "0001";
    }
    constexpr uint32_t kSheetB = 0x800B4490u; // the current car's tune sheet of the menus
    ReportRow("EntryCheck", 0x8001973Cu,
              RunCases(
                  guest, pristine, "EntryCheck", names.size(),
                  [&](uint8_t* ram, size_t i) {
                      random.Garage(ram, 0, 1);
                      GarageBlock& g = At<GarageBlock>(ram, D(kGarageAddress));
                      g.currentCar = int16_t(int32_t(rng() % uint32_t(g.count + 1)) - (i % 9 == 0 ? 1 : 0));
                      if (g.currentCar >= g.count) g.currentCar = int16_t(g.count - 1);
                      GarageCar& c = g.cars[g.currentCar < 0 ? 0 : g.currentCar];
                      if (rng() % 2 && !infos.carListPool.empty()) c.carId = infos.carListPool[rng() % infos.carListPool.size()];
                      c.powerFlags = uint16_t(rng() % 3 == 0 ? rng() : rng() % 700);
                      CareerState& s = At<CareerState>(ram, D(kStateAddress));
                      const uint32_t mode = rng() % 3;
                      for (auto& l : s.licences)
                          for (LicenceTestRecord& t : l) t.passed = mode == 0 ? uint8_t(1) : mode == 1 ? uint8_t(rng() % 2) : uint8_t(rng() % 5 != 0);
                      for (uint8_t& r : s.record.results) r = rng() % 3 == 0 ? uint8_t(0x11) : uint8_t(rng());
                      Put<uint8_t>(ram, D(kSheetB) + 0x54u, uint8_t(rng() % 2 ? 0 : rng()));
                      for (int k = 0; k < 27; k++) Put<int16_t>(ram, D(kSheetB) + 0x17B0u + uint32_t(k) * 2, int16_t(rng() % 4 == 0 ? int32_t(rng() % 4) : 0));
                      std::memset(ram + (kWork & 0x1FFFFF), 0, 0x48);
                      std::memcpy(ram + (kWork & 0x1FFFFF), names[i].c_str(), names[i].size() + 1);
                  },
                  [&](size_t) { return guest.Call(0x8001973Cu, kWork, kWork + 0x40u); },
                  [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                      uint32_t message = 0;
                      const int32_t code = EntryCheck(At<CareerState>(ram, D(kStateAddress)), At<GarageBlock>(ram, D(kGarageAddress)), menu, infos, names[i],
                                                      EntryCarStateOf(At<TuneSheet>(ram, D(kSheetB))), message);
                      Put<uint32_t>(ram, kWork + 0x40u, message);
                      return uint32_t(code);
                  }),
              failures);

    { // licence tests from the menus (0x80019B88)
        std::vector<std::string> licNames(300);
        for (std::string& n : licNames) n = (rng() % 10 == 0 ? std::string("LXX") : menu.licencePrefixes[rng() % 6]) + char('0' + rng() % 10) + char('0' + rng() % 10);
        ReportRow("LicEntry", 0x80019B88u,
                  RunCases(
                      guest, pristine, "LicEntry", licNames.size(),
                      [&](uint8_t* ram, size_t i) {
                          prepareLicences(ram, i);
                          std::memset(ram + (kWork & 0x1FFFFF), 0, 0x48);
                          std::memcpy(ram + (kWork & 0x1FFFFF), licNames[i].c_str(), licNames[i].size() + 1);
                      },
                      [&](size_t) { return guest.Call(0x80019B88u, kWork, kWork + 0x40u); },
                      [&](uint8_t* ram, uint8_t*, size_t i) -> std::optional<uint32_t> {
                          uint32_t message = 0;
                          const int32_t r = LicenceEntryCheck(At<CareerState>(ram, D(kStateAddress)), menu, licNames[i], message);
                          Put<uint32_t>(ram, kWork + 0x40u, message);
                          return uint32_t(r);
                      }),
                  failures);
    }

    // events (not licences) for the prize block: series use the row of their first race (SeriesPrizeEvent)
    std::vector<std::string> singles;
    for (const std::string& n : menu.events)
        if (n.size() >= 4 && n[0] != 'L' && MachineTestMode(menu, n) < 0) singles.push_back(n);
    std::vector<std::string> prizeNames(120);
    for (std::string& n : prizeNames) n = singles[rng() % singles.size()];
    masked = 0;
    ReportMasked("Prizes", 0x80018C8Cu,
              RunCases(
                  guest, pristine, "Prizes", prizeNames.size(),
                  [&](uint8_t* ram, size_t i) {
                      PutRaceFile(ram, raceFile, exeImage, language);
                      for (uint32_t k = 0; k < sizeof(PrizeBlock); k++) ram[(D(kPrizeBlockAddress) & 0x1FFFFF) + k] = uint8_t(rng());
                      Put<uint32_t>(ram, D(kVsyncCounterAddress), rng());
                      std::memset(ram + (kWork & 0x1FFFFF), 0, 0x48);
                      std::memcpy(ram + (kWork & 0x1FFFFF), prizeNames[i].c_str(), prizeNames[i].size() + 1);
                      FillGuestStack(ram);
                  },
                  [&](size_t) { return guest.CallWithBios(0x80018C8Cu, D(kPrizeBlockAddress), kWork); },
                  [&](uint8_t* ram, uint8_t* scratch, size_t i) {
                      const RaceEvent e = data.race.EventAt(size_t(data.race.FindEvent(SeriesPrizeEvent(prizeNames[i]))));
                      PreparePrizes(At<PrizeBlock>(ram, D(kPrizeBlockAddress)), e, Get<uint32_t>(ram, D(kVsyncCounterAddress)), data, At<TuneSheet>(ram, D(kTuneSheetAddress)),
                                    BuildScratch{scratch});
                      return std::nullopt;
                  },
                  WithSprintfCursor(gearMask), &masked),
              masked, failures);

    // ---------------------------------------------------------------- championships (src/game/career/championship.*)
    {
        std::vector<std::string> seriesNames;
        for (const std::string& n : menu.events)
            if (!n.empty() && n[0] != 'L' && MachineTestMode(menu, n) < 0) seriesNames.push_back(n); // (0x80013628 never builds these)
        std::vector<std::string> sn(400);
        for (std::string& n : sn) n = seriesNames[rng() % seriesNames.size()];
        ReportRow("Series", 0x80018A84u,
                  RunCases(
                      guest, pristine, "Series", sn.size(),
                      [&](uint8_t* ram, size_t i) {
                          PutRaceFile(ram, raceFile, exeImage, language);
                          for (uint32_t k = 0; k < sizeof(SeriesBlock); k++) ram[(D(kSeriesAddress) & 0x1FFFFF) + k] = uint8_t(rng());
                          Put<uint32_t>(ram, D(kVsyncCounterAddress), rng());
                          std::memset(ram + (kWork & 0x1FFFFF), 0, 0x48);
                          std::memcpy(ram + (kWork & 0x1FFFFF), sn[i].c_str(), sn[i].size() + 1);
                      },
                      [&](size_t) { return guest.CallWithBios(0x80018A84u, D(kSeriesAddress), kWork); },
                      [&](uint8_t* ram, uint8_t*, size_t i) {
                          BuildSeries(At<SeriesBlock>(ram, D(kSeriesAddress)), sn[i], data, menu, Get<uint32_t>(ram, D(kVsyncCounterAddress)));
                          return std::nullopt;
                      },
                      WithSprintfCursor(nullptr)),
                  failures);
        auto randomPoints = [&](uint8_t* ram, size_t i) {
            SeriesBlock& b = At<SeriesBlock>(ram, D(kSeriesAddress));
            for (size_t k = 0; k < 6; k++) {
                b.pointsTotal[k] = int8_t(i % 3 == 0 ? int32_t(rng() % 4) * 5 : int32_t(rng()));
                b.pointsRace[k] = int8_t(rng() % 11);
            }
            for (uint32_t k = 0; k < 8; k++) ram[(kWork & 0x1FFFFF) + k] = uint8_t(rng());
        };
        ReportRow("Standings", 0x8005E6B0u,
                  RunCases(
                      guest, pristine, "Standings", 400, randomPoints, [&](size_t) { return guest.Call(0x8005E6B0u, D(kSeriesAddress), kWork); },
                      [&](uint8_t* ram, uint8_t*, size_t) {
                          std::array<int8_t, 6> order{};
                          std::memcpy(order.data(), ram + (kWork & 0x1FFFFF), 6);
                          ChampionshipOrder(At<SeriesBlock>(ram, D(kSeriesAddress)), order);
                          std::memcpy(ram + (kWork & 0x1FFFFF), order.data(), 6);
                          return std::nullopt;
                      }),
                  failures);
        ReportRow("AddPoints", 0x8005E67Cu,
                  RunCases(
                      guest, pristine, "AddPoints", 200, randomPoints, [&](size_t) { return guest.Call(0x8005E67Cu, D(kSeriesAddress)); },
                      [&](uint8_t* ram, uint8_t*, size_t) { AddRacePoints(At<SeriesBlock>(ram, D(kSeriesAddress))); return std::nullopt; }),
                  failures);
        ReportRow("EventName", 0x8005E548u,
                  RunCases(
                      guest, pristine, "EventName", 200,
                      [&](uint8_t* ram, size_t i) {
                          for (uint32_t k = 0; k < 0x60; k++) ram[(D(kRaceBlockAddress) & 0x1FFFFF) + k] = uint8_t(rng());
                          std::memset(ram + (kWork & 0x1FFFFF), 0, 0x48);
                          std::memcpy(ram + (kWork & 0x1FFFFF), sn[i].c_str(), sn[i].size() + 1);
                      },
                      [&](size_t) { return guest.CallWithBios(0x8005E548u, D(kRaceBlockAddress), kWork); },
                      [&](uint8_t* ram, uint8_t*, size_t i) {
                          SetRaceEventName(std::span<uint8_t>(ram + (D(kRaceBlockAddress) & 0x1FFFFF), kRaceBlockSize), sn[i]);
                          return std::nullopt;
                      }),
                  failures);
        const CourseInfoTable courseInfo = ParseCourseInfo(vol->Read(".crsinfo"));
        std::vector<uint32_t> ids(300);
        for (uint32_t& id : ids) id = rng() % 8 == 0 ? rng() : courseInfo.entries[rng() % courseInfo.entries.size()].fileId;
        ReportRow("Course", 0x8005E590u,
                  RunCases(
                      guest, pristine, "Course", ids.size(),
                      [&](uint8_t* ram, size_t) { for (uint32_t k = 0; k < 0x60; k++) ram[(D(kRaceBlockAddress) & 0x1FFFFF) + k] = uint8_t(rng()); },
                      [&](size_t i) { return guest.CallWithBios(0x8005E590u, D(kRaceBlockAddress), ids[i]); },
                      [&](uint8_t* ram, uint8_t*, size_t i) {
                          SetRaceCourse(std::span<uint8_t>(ram + (D(kRaceBlockAddress) & 0x1FFFFF), kRaceBlockSize), ids[i], courseInfo);
                          return std::nullopt;
                      }),
                  failures);
    }

    // opponents of an event race: 0x80010A30(0, 1, row, 0, 0, 0, 0) against PickEventOpponents; compared field by
    // field (the six car slots of the race block 0x801D58B8 + i * 0xD0: car, paint, configuration, bytes +0x8C..,
    // name; and the six records the builder wrote at 0x801DE8BA + i * 0x1C0). The block's course / name fields are
    // not part of this port.
    {
        std::vector<std::string> events;
        for (const std::string& n : menu.events) {
            const int32_t row = data.race.FindEvent(n);
            if (n[0] != 'L' && row >= 0 && data.race.EventAt(size_t(row)).OpponentCount() > 0) events.push_back(n);
        }
        size_t cases = 0, bad = 0, unported = 0;
        const uint32_t raceFileAddress = 0x80024430u;
        for (size_t i = 0; i < 120; i++) {
            const std::string name = events[rng() % events.size()];
            const uint32_t vsync = rng();
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            PutRaceFile(guest.Ram(), raceFile, exeImage, language);
            Put<uint32_t>(guest.Ram(), D(kVsyncCounterAddress), vsync);
            std::memset(guest.Ram() + (kWork & 0x1FFFFF), 0, 0x48);
            std::memcpy(guest.Ram() + (kWork & 0x1FFFFF), name.c_str(), name.size() + 1);
            const uint32_t rowPtr = guest.Call(0x8007830Cu, raceFileAddress, kWork);
            for (uint32_t k = 0; k < 4; k++) Put<uint32_t>(guest.Ram(), kStack + 0x10u + k * 4, 0);
            const RaceEvent e = data.race.EventAt(size_t(data.race.FindEvent(name)));
            uint32_t seed = vsync;
            std::vector<GridCar> grid;
            try {
                grid = PickEventOpponents(data, e, Get<uint8_t>(pristine.data(), D(kStateAddress)), seed);
            } catch (const std::logic_error& ex) {
                if (unported++ == 0) std::printf("    Opponents case %zu: not ported: %s\n", i, ex.what());
                continue;
            }
            if (std::getenv("GT2_VERIFY_TRACE")) std::printf("      Opponents case %zu: %s seed %08X\n", i, name.c_str(), vsync);
            guest.CallWithBios(0x80010A30u, 0, 1, rowPtr, 0);
            cases++;
            bool equal = true;
            for (size_t s = 0; s < grid.size() && equal; s++) {
                const uint32_t slot = D(0x801D58B8u) + uint32_t(s) * 0xD0u;
                CarConfig config = grid[s].config;
                sim::CarParams record;
                BuildMenuRecord(data.tables, config, record);
                const uint8_t* ram = guest.Ram();
                const bool same = Get<uint32_t>(ram, slot) == grid[s].carId && Get<int32_t>(ram, slot + 4) == int32_t(grid[s].paint) &&
                                  std::memcmp(ram + ((slot + 8) & 0x1FFFFF), &config, sizeof(CarConfig)) == 0 && Get<uint8_t>(ram, slot + 0x8C) == 1 &&
                                  Get<uint8_t>(ram, slot + 0x8D) == uint8_t(5 - s) && Get<uint8_t>(ram, slot + 0x8E) == 1 && Get<uint8_t>(ram, slot + 0x8F) == 0 &&
                                  std::string(reinterpret_cast<const char*>(ram + ((slot + 0x90) & 0x1FFFFF))) == grid[s].name &&
                                  std::memcmp(ram + ((D(0x801DE8BAu) + uint32_t(s) * 0x1C0u) & 0x1FFFFF), &record, sizeof(record)) == 0;
                if (!same) {
                    equal = false;
                    if (bad < 3) {
                        const uint8_t* cfgOrig = ram + ((slot + 8) & 0x1FFFFF);
                        const uint8_t* cfgOurs = reinterpret_cast<const uint8_t*>(&config);
                        for (size_t k = 0; k < sizeof(CarConfig); k++)
                            if (cfgOrig[k] != cfgOurs[k]) { std::printf("      config +0x%02zX original %02X ours %02X\n", k, cfgOrig[k], cfgOurs[k]); break; }
                        const uint8_t* recOrig = ram + ((D(0x801DE8BAu) + uint32_t(s) * 0x1C0u) & 0x1FFFFF);
                        const uint8_t* recOurs = reinterpret_cast<const uint8_t*>(&record);
                        for (size_t k = 0; k < sizeof(record); k++)
                            if (recOrig[k] != recOurs[k]) { std::printf("      record +0x%03zX original %02X ours %02X\n", k, recOrig[k], recOurs[k]); break; }
                        std::printf("      car %08X ours %08X, paint %d ours %d, bytes +8C..8F %02X %02X %02X %02X, name '%s' (%zu) ours '%s' (%zu)\n", Get<uint32_t>(ram, slot),
                                    grid[s].carId, Get<int32_t>(ram, slot + 4), int32_t(grid[s].paint), Get<uint8_t>(ram, slot + 0x8C), Get<uint8_t>(ram, slot + 0x8D),
                                    Get<uint8_t>(ram, slot + 0x8E), Get<uint8_t>(ram, slot + 0x8F), reinterpret_cast<const char*>(ram + ((slot + 0x90) & 0x1FFFFF)),
                                    std::strlen(reinterpret_cast<const char*>(ram + ((slot + 0x90) & 0x1FFFFF))), grid[s].name.c_str(), grid[s].name.size());
                    }
                    if (bad < 3)
                        std::printf("    MISMATCH Opponents %s seed %08X slot %zu: original car %s paint %d, ours %s paint %d (opponent %u)\n", name.c_str(), vsync, s,
                                    UnpackCarId(Get<uint32_t>(ram, slot)).c_str(), Get<int32_t>(ram, slot + 4), UnpackCarId(grid[s].carId).c_str(), grid[s].paint,
                                    grid[s].opponent);
                }
            }
            if (!equal) bad++;
        }
        if (unported) std::printf("%-10s 0x%08X  %zu cases (%zu skipped: branch not ported), %zu mismatches  %s\n", "Opponents", 0x80010A30u, cases, unported, bad, bad ? "FAIL" : "ok");
        else std::printf("%-10s 0x%08X  %zu cases (6 cars each: slot + record), %zu mismatches  %s\n", "Opponents", 0x80010A30u, cases, bad, bad ? "FAIL" : "ok");
        failures += bad ? 1 : 0;
    }

    // ---------------------------------------------------------------- the menus' wheel files (game/menu/menu_car.h)
    // The boot's table (0x8001194C: u32 0x801E30F0 x s16 0x801C93B4, first file u16 0x801E2FC6) against the native
    // one from the VOL directory, then 0x800615E8's word mask + 0x80060D74 (EXE, resident) for every table id, the ids
    // with random dish / colour codes (bits 8..12), neighbours and random words: file number = base + native index.
    if (!SkipCareerRow("WheelFile")) {
        const menu::MenuWheelFiles files = menu::LoadMenuWheelFiles(*vol, LoadExeImage(*disc));
        const uint32_t count = uint32_t(int32_t(Get<int16_t>(pristine.data(), D(0x801C93B4u))));
        const uint32_t base = Get<uint16_t>(pristine.data(), D(0x801E2FC6u));
        size_t cases = 1, bad = 0;
        bool tableOk = count == files.ids.size();
        for (uint32_t i = 0; tableOk && i < count; i++) tableOk = Get<uint32_t>(pristine.data(), D(0x801E30F0u) + i * 4) == files.ids[i];
        const GtfsEntry* first = vol->Find(files.paths.empty() ? std::string() : files.paths[0]);
        if (!first || first->index != base) tableOk = false;
        if (!tableOk) { bad++; std::printf("    MISMATCH wheel table: count %u / native %zu, base %u\n", count, files.ids.size(), base); }
        std::vector<uint32_t> words;
        for (uint32_t id : files.ids) {
            words.push_back(id);
            words.push_back(id | ((rng() & 0x1F) << 8));
            words.push_back(id + 1);
            words.push_back(id - 1);
        }
        for (int k = 0; k < 200; k++) words.push_back(rng());
        words.push_back(0);
        words.push_back(0x1F00);
        for (uint32_t w : words) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            const int native = menu::MenuWheelFile(files, w);
            cases++;
            if ((w & 0xFFFFE0FFu) == 0) continue; // 0x800615E8 loads nothing (checked natively: -1)
            const uint32_t original = guest.Call(0x80060D74u, w & 0xFFFFE0FFu);
            if (native < 0 || original != base + uint32_t(native)) {
                if (bad++ < 4) std::printf("    MISMATCH wheel word %08X: original file %u, native %d (+ base %u)\n", w, original, native, base);
            }
        }
        std::printf("%-10s 0x%08X  %zu cases (%zu wheel files), %zu mismatches  %s\n", "WheelFile", 0x80060D74u, cases, files.ids.size(), bad, bad ? "FAIL" : "ok");
        failures += bad ? 1 : 0;
    }
    return failures;
}

} // namespace gt2::verify
