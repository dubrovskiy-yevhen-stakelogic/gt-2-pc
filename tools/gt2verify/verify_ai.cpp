// Differential checks of src/game/sim/ai_driver.* against the original (see guest.h for the harness): the three
// AI routines 0x80037834 / 0x800377E8 / 0x800372EC and their callees, on the dump's cars with randomised state
// (position and course distance along the lines, speeds, headings, neighbour sectors, drive class, line and
// section bookkeeping, recovery and race states, the overlay's tuning table and constants).
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/sim/ai_driver.h"
#include "game/sim/trig.h"
#include "guest.h"

namespace gt2::verify {

namespace {

template <typename T>
T RamAt(const uint8_t* ram, uint32_t address) {
    T v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T));
    return v;
}
template <typename T>
void RamPut(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
uint32_t BodyAddress(uint32_t car) { return kCarBase + car * kCarStride + kBodyOffset; }
sim::CarBody& BodyAt(uint8_t* ram, uint32_t body) { return *reinterpret_cast<sim::CarBody*>(At(ram, body)); }
template <typename T>
void Put(uint8_t* object, uint32_t offset, T value) { std::memcpy(object + offset, &value, sizeof(T)); }
void Put32(uint8_t* o, uint32_t off, int32_t v) { Put<int32_t>(o, off, v); }
void Put16(uint8_t* o, uint32_t off, int32_t v) { Put<int16_t>(o, off, int16_t(v)); }
void Put8(uint8_t* o, uint32_t off, int32_t v) { Put<uint8_t>(o, off, uint8_t(v)); }
uint8_t* WheelOf(uint8_t* body, uint32_t wheel) { return body + 0x460 + wheel * 0x68; }

constexpr uint32_t kRaceObject = 0x801C8568u;   // pointer to the race object (line lists at +8)
constexpr uint32_t kCourseObject = 0x800A9500u; // chunk pointer table at + 0xB544, course length at its +0
constexpr uint32_t kCourseIndex = 0x800AF230u;  // u8 course number -> course table 0x801E18E8 + i * 24 (+8 & 4 = dirt)
constexpr uint32_t kCourseTable = 0x801E18E8u;
constexpr uint32_t kClassTuning = 0x801C8690u;  // 4 records of 0x28 bytes
constexpr uint32_t kRaceStateTable = 0x80046DD4u;
constexpr uint32_t kWearConstants = 0x80046F48u;
constexpr uint32_t kWord80046F64 = 0x80046F64u;
constexpr uint32_t kRate = 0x801C8570u;
constexpr uint32_t kGearRequests = 0x364;       // scratchpad offset of the gear requests
constexpr uint32_t kOutArea = kStack + 0x40;    // guest stack area (excluded from the RAM comparison) for out-pointers

struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
    int32_t Pick(std::initializer_list<int32_t> values) { return *(values.begin() + Next() % values.size()); }
};

// ---- stateful comparison with per-case arguments (like verify_core's RunStateful) ----
struct CallArgs { uint32_t a0 = 0, a1 = 0, a2 = 0, a3 = 0; };

template <typename Prepare, typename Native>
StatefulResult RunStateful(Guest& guest, const std::vector<uint8_t>& pristine, uint32_t function, size_t variants, Prepare prepare, Native native,
                           size_t& skipped) {
    StatefulResult result;
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
    for (size_t variant = 0; variant < variants; variant++) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        std::memset(guest.Scratch(), 0, kScratchSize);
        CallArgs args;
        {
            SimContactLayout layout(guest.Ram()); // the native view of the contact state (guest.h)
            args = prepare(guest.Ram(), guest.Scratch(), variant);
        }
        std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
        std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
        std::vector<uint8_t> before(guest.Ram() + (kCarBase & 0x1FFFFF), guest.Ram() + (kCarBase & 0x1FFFFF) + kCarCount * kCarStride); // for the diagnostics
        try {
            guest.Call(function, args.a0, args.a1, args.a2, args.a3);
        } catch (const std::runtime_error&) { // the original trapped (a 64-bit division by zero)
            skipped++;
            continue;
        }
        {
            SimContactLayout layout(ours.data());
            native(ours.data(), ourScratch.data(), args);
        }
        result.cases++;
        const bool equal = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                           std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0 &&
                           std::memcmp(ourScratch.data(), guest.Scratch(), kScratchSize) == 0;
        if (!equal && result.mismatches++ < 3) {
            if (args.a0 >= kCarBase && args.a0 < kCarBase + kCarCount * kCarStride) { // the AI state of the car before the call
                const uint8_t* b = before.data() + (args.a0 - kCarBase);
                auto s32 = [&](uint32_t off) { int32_t v; std::memcpy(&v, b + off, 4); return v; };
                auto s16 = [&](uint32_t off) { int16_t v; std::memcpy(&v, b + off, 2); return int32_t(v); };
                std::printf("    state: dist %d speed %d heading %d 45D %u 786 %u 788 %u line %u section %u prev %u type %u 784 %u 785 %u 78D %02X 76A %d n %d %d %d %d %d 1D %u 790 %d 794 %d clock %u\n",
                            s32(0x604), s32(0x6A4), s16(0x648), b[0x45D], b[0x786], b[0x788], b[0x789], b[0x78A], b[0x78B], b[0x78C], b[0x784], b[0x785], b[0x78D],
                            s16(0x76A), int8_t(b[0x76C]), int8_t(b[0x76D]), int8_t(b[0x76E]), int8_t(b[0x76F]), int8_t(b[0x770]), b[0x1D], s32(0x790), s32(0x794),
                            RamAt<uint32_t>(ours.data(), D(kWord80046F64)));
            }
            int shown = 0;
            for (uint32_t i = 0; i < Bus::kRamSize && shown < 6; i++)
                if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                    const uint32_t address = 0x80000000u + i;
                    const bool inCars = address >= kCarBase && address < kCarBase + kCarCount * kCarStride;
                    std::printf("    MISMATCH variant %zu: %s byte at 0x%08X", variant, shown ? "     also" : "first differing", address);
                    if (inCars) std::printf(" (car %u body + 0x%X)", (address - kCarBase) / kCarStride, (address - kCarBase) % kCarStride - kBodyOffset);
                    std::printf(": original %02X ours %02X\n", guest.Ram()[i], ours[i]);
                    shown++;
                }
            for (uint32_t i = 0; i < kScratchSize && shown < 6; i++)
                if (ourScratch[i] != guest.Scratch()[i]) {
                    std::printf("    MISMATCH variant %zu: %s scratchpad byte at 0x1F800000 + 0x%X: original %02X ours %02X\n", variant,
                                shown ? "     also" : "first differing", i, guest.Scratch()[i], ourScratch[i]);
                    shown++;
                }
        }
    }
    return result;
}

// ---- the AI's view of an image ----
struct LineInfo { uint32_t list = 0; int32_t count = 0; };
LineInfo LineOf(const uint8_t* ram, uint32_t line) {
    const uint32_t race = RamAt<uint32_t>(ram, D(kRaceObject));
    LineInfo info;
    info.list = RamAt<uint32_t>(ram, race + 8 + line * 4);
    info.count = info.list ? RamAt<int32_t>(ram, info.list) : 0;
    return info;
}
uint32_t SectionAddress(const uint8_t* ram, uint32_t line, int32_t section) { return LineOf(ram, line).list + 4 + uint32_t(section) * 0x28; }
int32_t CourseLength(const uint8_t* ram) { return RamAt<int32_t>(ram, RamAt<uint32_t>(ram, D(kCourseObject) + 0xB544)); }
uint32_t CourseTableEntry(const uint8_t* ram) { return D(kCourseTable) + RamAt<uint8_t>(ram, D(kCourseIndex)) * 24u; }

// Randomises the overlay constants and tables the AI reads (on the guest image; the port rebuilds its context from it).
void RandomiseAiConstants(Rng& rng, uint8_t* ram) {
    if (rng.Chance(3)) RamPut<uint16_t>(ram, CourseTableEntry(ram) + 8, uint16_t(RamAt<uint16_t>(ram, CourseTableEntry(ram) + 8) ^ 4u)); // dirt
    if (rng.Chance(8)) RamPut<uint32_t>(ram, D(kWord80046F64), uint32_t(rng.Range(0, 20000)));
    RamPut<int32_t>(ram, D(kWearConstants), rng.Chance(2) ? 0 : rng.Range(1000, 2000000)); // wear enabled: the grip estimate uses the wheels
    for (uint32_t i = 0; i < 4; i++) {
        uint8_t* record = At(ram, D(kClassTuning) + i * 0x28);
        if (rng.Chance(2)) Put16(record, 0x00, rng.Chance(2) ? 0x1000 + rng.Range(0, 0x1000) : rng.Range(0, 0x1000)); // brake margin factor
        if (rng.Chance(3)) Put16(record, 0x08, rng.Range(0, 0x3000));   // steer gain
        if (rng.Chance(3)) Put16(record, 0x0A, rng.Range(0, 0x2000));   // overshoot throttle cut
        if (rng.Chance(3)) Put16(record, 0x0C, rng.Range(0, 0x2000));   // overshoot brake
        if (rng.Chance(3)) Put16(record, 0x0E, rng.Range(0, 400));      // lookahead speed gain
        if (rng.Chance(3)) Put32(record, 0x10, rng.Range(0x20000, 0x200000));
        if (rng.Chance(3)) Put32(record, 0x14, rng.Range(0x100000, 0x600000));
        if (rng.Chance(3)) Put16(record, 0x26, rng.Range(0x400, 0x1400)); // throttle scale
    }
}

// A course distance and a matching position on `line` near section `section` (plus noise).
void PlaceOnLine(Rng& rng, uint8_t* ram, uint8_t* body, uint32_t line, int32_t section, int32_t distanceNoise) {
    const LineInfo info = LineOf(ram, line);
    const int32_t index = section < info.count ? section : info.count - 1;
    const uint32_t record = SectionAddress(ram, line, index);
    const int32_t length = CourseLength(ram);
    int32_t distance = RamAt<int32_t>(ram, record + 0x14) + distanceNoise;
    if (distance < 0) distance += length;
    else if (distance >= length) distance -= length;
    Put32(body, 0x604, distance);
    Put32(body, 0x65C, RamAt<int32_t>(ram, record + 4) + rng.Range(-0x14000, 0x14000));
    Put32(body, 0x660, RamAt<int32_t>(ram, record + 8) + rng.Range(-0x14000, 0x14000));
    Put32(body, 0x664, rng.Range(-0x10000, 0x10000));
}

// The dynamic state the AI reads or writes (the car's parameters stay as in the dump).
void RandomiseAiState(Rng& rng, uint8_t* ram, uint8_t* body, uint32_t line) {
    const LineInfo info = LineOf(ram, line);
    Put8(body, 0x789, int32_t(line));
    int32_t section = rng.Chance(8) ? info.count : rng.Range(0, info.count - 1);
    Put8(body, 0x78A, section);
    if (line == 4) { // the grid line: keep the lookahead (at most 96 m of the randomised tuning + 9 m of steps) inside the
                     // grid's span, because the original steers at uninitialised stack words when 0x80036160 fails
        const int32_t first = RamAt<int32_t>(ram, SectionAddress(ram, 4, 0) + 0x14), last = RamAt<int32_t>(ram, SectionAddress(ram, 4, info.count - 1) + 0x14);
        const int32_t length = CourseLength(ram);
        int32_t distance = first + 0x10000 + rng.Range(0, last - first - 0x6E0000);
        if (distance >= length) distance -= length;
        Put32(body, 0x604, distance);
        const uint32_t record = SectionAddress(ram, 4, rng.Range(0, info.count - 1));
        Put32(body, 0x65C, RamAt<int32_t>(ram, record + 4) + rng.Range(-0x14000, 0x14000));
        Put32(body, 0x660, RamAt<int32_t>(ram, record + 8) + rng.Range(-0x14000, 0x14000));
    } else {
        PlaceOnLine(rng, ram, body, line, section, rng.Chance(3) ? rng.Range(-0x50000, 0x50000) : rng.Range(-0x30000, 0x400000));
    }
    static constexpr int32_t kSpeedEdges[] = {0, 1, -1, 0x8E4, 0x8E5, -0x8E4, -0x8E5, 0x855C, 0x855D, -0x855C, -0x855D, 0x8E3, -0x8E3, 0x1639 * 4, 0x163A * 4};
    const int32_t speed = rng.Chance(5) ? kSpeedEdges[rng.Next() % 15] : rng.Chance(4) ? rng.Range(-0x30000, 0x30000) : rng.Range(-0x2000, 0x70000);
    Put32(body, 0x6A4, speed);
    Put32(body, 0x6A8, rng.Range(-0x8000, 0x8000));
    const int32_t heading = rng.Range(-0xFFF, 0xFFF);
    Put16(body, 0x648, heading);
    // The velocity follows the heading most of the time (the direction of travel matters above 5 km/h).
    const int32_t s = sim::Sin(uint32_t(uint16_t(heading))), c = sim::Cos(uint32_t(uint16_t(heading)));
    if (rng.Chance(4)) {
        for (uint32_t k = 0; k < 3; k++) Put32(body, 0x628 + k * 4, rng.Range(-0x40000, 0x40000));
    } else {
        const int32_t lateral = rng.Range(-0x4000, 0x4000);
        Put32(body, 0x628, int32_t((int64_t(-s) * speed + int64_t(c) * lateral) >> 12));
        Put32(body, 0x62C, int32_t((int64_t(c) * speed + int64_t(s) * lateral) >> 12));
        Put32(body, 0x630, rng.Range(-0x4000, 0x4000));
    }
    Put32(body, 0x64C, rng.Chance(4) ? 0 : rng.Range(-0x40000, 0x40000)); // yaw rate
    Put16(body, 0x650, rng.Range(-0x400, 0x400));
    Put16(body, 0x652, rng.Range(-0x400, 0x400));
    Put8(body, 0x78B, rng.Pick({0, 0, 1, 2, 3, 4}));
    Put8(body, 0x78C, section == info.count ? 0 : rng.Pick({0, 1, 2})); // (an index past the end reads past the list for a corner type)
    Put32(body, 0x790, rng.Range(0x58E8, 0x60000));
    Put32(body, 0x794, rng.Chance(3) ? speed - rng.Range(-0x2000, 0x20000) : rng.Range(0x58E8, 0x60000));
    Put8(body, 0x786, rng.Chance(2) ? 0 : rng.Pick({0, 1, 2, 3, 4, 5, 6}));
    Put8(body, 0x788, rng.Pick({0, 0, 3, 4, 1}));
    Put8(body, 0x784, rng.Range(0, 1));
    Put8(body, 0x785, rng.Chance(2) ? 0 : rng.Range(0, 15));
    Put8(body, 0x78D, (rng.Chance(3) ? 0x10 : 0) | (rng.Chance(5) ? 0x4 : 0));
    Put8(body, 0x1D, rng.Range(0, 3));
    Put8(body, 0x45D, rng.Chance(4) ? 0 : 2);
    Put16(body, 0x76A, rng.Chance(2) ? 0 : rng.Pick({0x400, 0x800, 0x1000}));
    for (uint32_t i = 0; i < 8; i++) Put8(body, 0x76C + i, rng.Chance(2) ? 0 : rng.Range(0, 2));
    Put16(body, 0x60C, rng.Range(-0x300, 0x300));
    Put8(body, 0x618, rng.Range(0, 6));
    Put8(body, 0x61C, rng.Range(0, 250));
    Put16(body, 0x708, rng.Chance(3) ? 0x1000 : rng.Range(0, 0x1000));
    Put16(body, 0x610, rng.Range(0, 0x1000));
    Put16(body, 0x612, rng.Range(0, 0x1000));
    Put8(body, 0x769, rng.Range(0, 1));
    Put16(body, 0x6AC, rng.Range(0, 9000));
    for (uint32_t w = 0; w < 4; w++) {
        Put16(WheelOf(body, w), 0x38, rng.Range(0, 0x1000));
        Put16(WheelOf(body, w), 0x50, rng.Range(-0x400, 0x400));
        Put16(WheelOf(body, w), 0x60, rng.Chance(2) ? 0 : rng.Range(0, 0x1000));
    }
}

int32_t ScratchStepTime(const uint8_t* scratch) { int32_t v; std::memcpy(&v, scratch, 4); return v; }
void SetScratch(Rng& rng, uint8_t* scratch, uint32_t car) {
    const int32_t stepTime = rng.Chance(2) ? 2184 : rng.Range(0, 0x2000);
    std::memcpy(scratch, &stepTime, 4);
    for (uint32_t c = 0; c < kCarCount; c++) {
        scratch[kGearRequests + c * 4] = uint8_t(rng.Range(0, 1));
        scratch[kGearRequests + c * 4 + 1] = uint8_t(rng.Range(-1, 1));
    }
    (void)car;
}
sim::GearRequest& RequestOf(uint8_t* scratch, uint32_t car) { return *reinterpret_cast<sim::GearRequest*>(scratch + kGearRequests + car * 4); }

} // namespace

sim::AiContext AiContextFromRam(const uint8_t* ram) {
    sim::AiContext c;
    const uint32_t race = RamAt<uint32_t>(ram, D(kRaceObject));
    for (uint32_t i = 0; i < 7; i++) {
        const uint32_t list = RamAt<uint32_t>(ram, race + 8 + i * 4);
        c.course.lines[i] = {};
        if (list == 0) continue;
        c.course.lines[i].count = RamAt<int32_t>(ram, list);
        c.course.lines[i].sections = reinterpret_cast<const sim::RaceSection*>(ram + ((list + 4) & 0x1FFFFF));
    }
    c.course.courseLength = CourseLength(ram);
    c.course.dirtCourse = (RamAt<uint16_t>(ram, CourseTableEntry(ram) + 8) & 4) != 0;
    c.course.raceStateTable = reinterpret_cast<const int8_t*>(ram + (D(kRaceStateTable) & 0x1FFFFF));
    c.classTuning = reinterpret_cast<const sim::DriveClassTuning*>(ram + (D(kClassTuning) & 0x1FFFFF));
    c.wear.wearLimit = RamAt<int32_t>(ram, D(kWearConstants));
    c.wear.wornGripLoss = RamAt<int32_t>(ram, D(kWearConstants) + 4);
    c.wear.pitGripFactor = RamAt<int32_t>(ram, D(kWearConstants) + 8);
    c.wear.coldLimit = RamAt<int32_t>(ram, D(kWearConstants) + 12);
    c.wear.coldGripLoss = RamAt<int32_t>(ram, D(kWearConstants) + 16);
    c.wear.wearKnee = RamAt<int32_t>(ram, D(kWearConstants) + 20);
    c.wear.kneeGripLoss = RamAt<int32_t>(ram, D(kWearConstants) + 24);
    c.raceClock = RamAt<uint32_t>(ram, D(kWord80046F64));
    c.rate = RamAt<int32_t>(ram, D(kRate));
    return c;
}

int VerifyAi(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt, const Track* track) {
    (void)track;
    int failures = 0;
    Rng rng{mt};
    std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
    {
        const sim::AiContext c = AiContextFromRam(guest.Ram());
        std::printf("           (AI: lines");
        for (uint32_t i = 0; i < 7; i++) std::printf(" %u:%d", i, c.course.lines[i].count);
        std::printf(", course %.1f m, dirt %d, race clock %u)\n", c.course.courseLength / 65536.0, c.course.dirtCourse ? 1 : 0, c.raceClock);
    }
    static constexpr uint32_t kLines[] = {6, 6, 6, 2, 1, 3, 6, 2, 1, 3, 6, 4}; // the lines of the dump's race object (0 and 5 are not driven)
    auto pickLine = [&]() { return kLines[rng.Next() % 12]; };
    // The driven lines must exist in the dump: a race without opponents (a license test) has no race lists, and
    // the original would read through null list pointers.
    {
        const sim::AiContext c = AiContextFromRam(guest.Ram());
        bool missing = false;
        for (uint32_t line : kLines) missing = missing || c.course.lines[line].count <= 0;
        if (missing) {
            std::puts("Ai         skipped (the dump's course has no AI race lines: a race without opponents)");
            return 0;
        }
    }

    // ---- 0x80035874: lookahead distance (pure)
    {
        size_t cases = 0, mismatches = 0;
        for (size_t v = 0; v < 400; v++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            RandomiseAiConstants(rng, guest.Ram());
            const uint32_t record = D(kClassTuning) + uint32_t(rng.Range(0, 3)) * 0x28;
            const int32_t speed = rng.Chance(4) ? rng.Pick({0, 1, -1, 0x7FFFFFFF}) : rng.Range(-0x10000, 0x80000);
            const uint32_t expected = guest.Call(0x80035874u, record, uint32_t(speed));
            const sim::DriveClassTuning& tuning = *reinterpret_cast<const sim::DriveClassTuning*>(At(guest.Ram(), record));
            const int32_t ours = sim::LookaheadDistance(tuning, speed);
            cases++;
            if (uint32_t(ours) != expected && mismatches++ < 3) std::printf("    MISMATCH speed %d: original %u ours %d\n", speed, expected, ours);
        }
        Report("Lookahead", 0x80035874u, cases, mismatches, failures);
    }
    // ---- 0x80035C48 / 0x80035D68 / 0x80036160 / 0x800360C8: line lookups (pure, outputs through pointers)
    {
        size_t cases[4] = {}, mismatches[4] = {};
        for (size_t v = 0; v < 1500; v++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            const sim::AiContext c = AiContextFromRam(guest.Ram());
            const int32_t length = c.course.courseLength;
            const uint32_t line = rng.Chance(6) ? 5 : pickLine();
            const int32_t count = c.course.lines[line].count;
            const int32_t hint = rng.Chance(6) ? count : rng.Range(0, count - 1);
            int32_t distance;
            if (rng.Chance(4)) distance = rng.Range(-length, 2 * length);
            else distance = c.course.lines[line].sections[rng.Range(0, count - 1)].distance + rng.Range(-0x30000, 0x30000);
            // FindLineSegment (its distance is already wrapped by the callers)
            {
                int32_t d = distance;
                if (d < 0) d += length; else if (d >= length) d -= length;
                RamPut<uint32_t>(guest.Ram(), kStack + 16, kOutArea + 4);
                const uint32_t expected = guest.Call(0x80035C48u, line, uint32_t(hint), uint32_t(d), kOutArea);
                const int32_t guestNext = RamAt<int32_t>(guest.Ram(), kOutArea), guestPrevious = RamAt<int32_t>(guest.Ram(), kOutArea + 4);
                int32_t next = 0, previous = 0;
                const int32_t ours = sim::FindLineSegment(c.course.lines[line], hint, d, next, previous);
                cases[0]++;
                const bool same = uint32_t(ours) == expected && (ours == 0 || (next == guestNext && previous == guestPrevious));
                if (!same && mismatches[0]++ < 3) std::printf("    MISMATCH segment line %u hint %d distance %d: original %u (%d, %d) ours %d (%d, %d)\n", line, hint, d, expected, guestNext, guestPrevious, ours, next, previous);
            }
            if (line != 4 && line != 5) { // LinePoint
                RamPut<uint32_t>(guest.Ram(), kStack + 16, kOutArea + 4);
                const uint32_t expected = guest.Call(0x80035D68u, line, uint32_t(hint), uint32_t(distance), kOutArea);
                int32_t x = 0, y = 0;
                const bool ours = sim::LinePoint(c.course, int32_t(line), hint, distance, x, y);
                cases[1]++;
                const bool same = uint32_t(ours ? 1 : 0) == expected && (!ours || (x == RamAt<int32_t>(guest.Ram(), kOutArea) && y == RamAt<int32_t>(guest.Ram(), kOutArea + 4)));
                if (!same && mismatches[1]++ < 3)
                    std::printf("    MISMATCH line point line %u hint %d distance %d: original %u (%d, %d) ours %d (%d, %d)\n", line, hint, distance, expected,
                                RamAt<int32_t>(guest.Ram(), kOutArea), RamAt<int32_t>(guest.Ram(), kOutArea + 4), ours ? 1 : 0, x, y);
            }
            { // GridLinePoint (line 4)
                const int32_t gridCount = c.course.lines[4].count;
                const int32_t d = rng.Chance(3) ? distance : c.course.lines[4].sections[rng.Range(0, gridCount - 1)].distance + rng.Range(-0x30000, 0x30000) - (rng.Chance(2) ? length : 0);
                RamPut<uint32_t>(guest.Ram(), kStack + 16, kOutArea + 4);
                RamPut<int32_t>(guest.Ram(), kOutArea, 0x5A5A5A5A);
                RamPut<int32_t>(guest.Ram(), kOutArea + 4, 0x5A5A5A5A);
                const uint32_t expected = guest.Call(0x80036160u, 4, uint32_t(hint), uint32_t(d), kOutArea);
                int32_t x = 0x5A5A5A5A, y = 0x5A5A5A5A;
                const bool ours = sim::GridLinePoint(c.course, 4, d, x, y);
                cases[2]++;
                const bool same = uint32_t(ours ? 1 : 0) == expected && x == RamAt<int32_t>(guest.Ram(), kOutArea) && y == RamAt<int32_t>(guest.Ram(), kOutArea + 4);
                if (!same && mismatches[2]++ < 3)
                    std::printf("    MISMATCH grid point distance %d: original %u (%d, %d) ours %d (%d, %d)\n", d, expected, RamAt<int32_t>(guest.Ram(), kOutArea),
                                RamAt<int32_t>(guest.Ram(), kOutArea + 4), ours ? 1 : 0, x, y);
            }
            { // InLineSwapZone
                const int32_t section = rng.Range(0, 60);
                int32_t d = distance;
                if (d < 0) d += length; else if (d >= length) d -= length;
                const uint32_t expected = guest.Call(0x800360C8u, uint32_t(section), uint32_t(d));
                const bool ours = sim::InLineSwapZone(c.course, section, d);
                cases[3]++;
                if (uint32_t(ours ? 1 : 0) != expected && mismatches[3]++ < 3) std::printf("    MISMATCH swap zone section %d distance %d: original %u ours %d\n", section, d, expected, ours ? 1 : 0);
            }
        }
        Report("Segment", 0x80035C48u, cases[0], mismatches[0], failures);
        Report("LinePoint", 0x80035D68u, cases[1], mismatches[1], failures);
        Report("GridPoint", 0x80036160u, cases[2], mismatches[2], failures);
        Report("SwapZone", 0x800360C8u, cases[3], mismatches[3], failures);
    }
    // ---- 0x80037538: the lookahead point of a body (pure on the body, outputs through pointers)
    {
        size_t cases = 0, mismatches = 0;
        for (size_t v = 0; v < 600; v++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
            uint8_t* body = At(guest.Ram(), BodyAddress(car));
            RandomiseAiState(rng, guest.Ram(), body, pickLine());
            const int32_t lookahead = rng.Pick({0x50000, 0x140000, 0xA0000, 0x280000, rng.Range(0, 0x400000)});
            const uint32_t expected = guest.Call(0x80037538u, BodyAddress(car), uint32_t(lookahead), kOutArea, kOutArea + 4);
            const sim::AiContext c = AiContextFromRam(guest.Ram());
            int32_t dx = 0, dy = 0;
            const int32_t ours = sim::LookaheadPoint(c.course, BodyAt(guest.Ram(), BodyAddress(car)), lookahead, dx, dy);
            cases++;
            const bool same = uint32_t(ours) == expected && dx == RamAt<int32_t>(guest.Ram(), kOutArea) && dy == RamAt<int32_t>(guest.Ram(), kOutArea + 4);
            if (!same && mismatches++ < 3)
                std::printf("    MISMATCH lookahead point car %u line %u: original %u (%d, %d) ours %d (%d, %d)\n", car, RamAt<uint8_t>(guest.Ram(), BodyAddress(car) + 0x789), expected,
                            RamAt<int32_t>(guest.Ram(), kOutArea), RamAt<int32_t>(guest.Ram(), kOutArea + 4), ours, dx, dy);
        }
        Report("LookPoint", 0x80037538u, cases, mismatches, failures);
    }

    size_t skipped = 0;
    // ---- 0x8003643C: section advance and the braking model (a2 = out situation, compared through the guest stack area)
    {
        size_t situationMismatches = 0;
        const StatefulResult r = RunStateful(
            guest, pristine, 0x8003643Cu, 1200,
            [&](uint8_t* ram, uint8_t* scratch, size_t) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                RandomiseAiConstants(rng, ram);
                RandomiseAiState(rng, ram, At(ram, BodyAddress(car)), pickLine());
                SetScratch(rng, scratch, car);
                return CallArgs{BodyAddress(car), car, kOutArea};
            },
            [&](uint8_t* ram, uint8_t*, const CallArgs& a) {
                const sim::AiContext c = AiContextFromRam(ram);
                int32_t situation = -1;
                const int32_t scale = sim::AdvanceLine(c, BodyAt(ram, a.a0), situation);
                RamPut<int32_t>(ram, a.a2, situation);
                if (situation != RamAt<int32_t>(guest.Ram(), a.a2) && situationMismatches++ < 3)
                    std::printf("    MISMATCH situation: original %d ours %d (scale %d)\n", RamAt<int32_t>(guest.Ram(), a.a2), situation, scale);
            },
            skipped);
        Report("AdvLine", 0x8003643Cu, r.cases, r.mismatches + situationMismatches, failures);
    }
    // ---- 0x80037494: speed hold
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80037494u, 400,
            [&](uint8_t* ram, uint8_t* scratch, size_t) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                RandomiseAiState(rng, ram, At(ram, BodyAddress(car)), pickLine());
                SetScratch(rng, scratch, car);
                const int32_t target = rng.Chance(3) ? RamAt<int32_t>(ram, BodyAddress(car) + 0x6A4) + rng.Range(-0x3000, 0x3000) : rng.Range(0, 0x60000);
                return CallArgs{BodyAddress(car), uint32_t(target), uint32_t(rng.Pick({-0x800, -0x2000, -0x1000, rng.Range(-0x4000, 0x4000)}))};
            },
            [&](uint8_t* ram, uint8_t*, const CallArgs& a) { sim::HoldSpeed(BodyAt(ram, a.a0), int32_t(a.a1), int32_t(a.a2)); }, skipped);
        Report("HoldSpeed", 0x80037494u, r.cases, r.mismatches, failures);
    }
    // ---- 0x80037664: steering rate limit (reads the scratchpad's step time)
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80037664u, 400,
            [&](uint8_t* ram, uint8_t* scratch, size_t) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                RandomiseAiState(rng, ram, At(ram, BodyAddress(car)), pickLine());
                SetScratch(rng, scratch, car);
                if (rng.Chance(4)) Put16(At(ram, BodyAddress(car)), 0x62, rng.Range(0, 0x4000));
                return CallArgs{BodyAddress(car), uint32_t(rng.Range(-0x300, 0x300))};
            },
            [&](uint8_t* ram, uint8_t* scratch, const CallArgs& a) { sim::EaseSteering(BodyAt(ram, a.a0), int32_t(a.a1), ScratchStepTime(scratch)); }, skipped);
        Report("EaseSteer", 0x80037664u, r.cases, r.mismatches, failures);
    }
    // ---- 0x800376D8: stop the car
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x800376D8u, 400,
            [&](uint8_t* ram, uint8_t* scratch, size_t variant) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                uint8_t* body = At(ram, BodyAddress(car));
                RandomiseAiState(rng, ram, body, pickLine());
                if (variant % 2) { // nearly at rest
                    for (uint32_t k = 0; k < 3; k++) Put32(body, 0x628 + k * 4, rng.Range(-300, 300));
                    Put32(body, 0x64C, rng.Range(-0x1200, 0x1200));
                    Put16(body, 0x650, rng.Range(-0x220, 0x220));
                    Put16(body, 0x652, rng.Range(-0x220, 0x220));
                }
                SetScratch(rng, scratch, car);
                return CallArgs{BodyAddress(car), uint32_t(rng.Range(-0x300, 0x300))};
            },
            [&](uint8_t* ram, uint8_t* scratch, const CallArgs& a) { sim::StopCar(BodyAt(ram, a.a0), int32_t(a.a1), ScratchStepTime(scratch)); }, skipped);
        Report("StopCar", 0x800376D8u, r.cases, r.mismatches, failures);
    }
    // ---- 0x800367AC: line switch (InitRaceProgress on the AI's lines)
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x800367ACu, 400,
            [&](uint8_t* ram, uint8_t* scratch, size_t) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                RandomiseAiConstants(rng, ram);
                RandomiseAiState(rng, ram, At(ram, BodyAddress(car)), pickLine());
                SetScratch(rng, scratch, car);
                return CallArgs{BodyAddress(car), pickLine()};
            },
            [&](uint8_t* ram, uint8_t*, const CallArgs& a) { sim::SwitchLine(AiContextFromRam(ram), BodyAt(ram, a.a0), uint8_t(a.a1)); }, skipped);
        Report("SwitchLine", 0x800367ACu, r.cases, r.mismatches, failures);
    }

    // ---- the three entry points
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x800377E8u, 300,
            [&](uint8_t* ram, uint8_t* scratch, size_t) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                RandomiseAiState(rng, ram, At(ram, BodyAddress(car)), pickLine());
                SetScratch(rng, scratch, car);
                return CallArgs{BodyAddress(car)};
            },
            [&](uint8_t* ram, uint8_t* scratch, const CallArgs& a) { sim::AiFinished(BodyAt(ram, a.a0), ScratchStepTime(scratch)); }, skipped);
        Report("AiFinish", 0x800377E8u, r.cases, r.mismatches, failures);
    }
    {
        const StatefulResult r = RunStateful(
            guest, pristine, 0x800372ECu, 400,
            [&](uint8_t* ram, uint8_t* scratch, size_t) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                uint8_t* body = At(ram, BodyAddress(car));
                RandomiseAiState(rng, ram, body, pickLine());
                if (rng.Chance(3)) Put16(body, 0x108, rng.Range(3000, 12000));
                if (rng.Chance(3)) Put16(body, 0x396, rng.Range(2000, 11000));
                if (rng.Chance(5)) RamPut<int32_t>(ram, D(kRate), rng.Pick({25, 30, 60}));
                SetScratch(rng, scratch, car);
                return CallArgs{BodyAddress(car), car, uint32_t(rng.Chance(2) ? rng.Range(0, 14) : rng.Range(0, 200))};
            },
            [&](uint8_t* ram, uint8_t*, const CallArgs& a) { sim::AiScripted(BodyAt(ram, a.a0), int32_t(a.a2), RamAt<int32_t>(ram, D(kRate))); }, skipped);
        Report("AiScript", 0x800372ECu, r.cases, r.mismatches, failures);
    }
    {
        size_t lineChanges = 0, reversing = 0;
        const StatefulResult r = RunStateful(
            guest, pristine, 0x80037834u, 3000,
            [&](uint8_t* ram, uint8_t* scratch, size_t variant) {
                const uint32_t car = uint32_t(rng.Range(0, int32_t(kCarCount) - 1));
                uint8_t* body = At(ram, BodyAddress(car));
                if (variant % 5 != 0) RandomiseAiConstants(rng, ram);
                if (variant % 7 != 0) RandomiseAiState(rng, ram, body, pickLine()); // every 7th case: the dump's own state
                SetScratch(rng, scratch, car);
                return CallArgs{BodyAddress(car), car};
            },
            [&](uint8_t* ram, uint8_t* scratch, const CallArgs& a) {
                const uint8_t lineBefore = RamAt<uint8_t>(ram, a.a0 + 0x789);
                sim::AiDrive(AiContextFromRam(ram), BodyAt(ram, a.a0), RequestOf(scratch, a.a1), ScratchStepTime(scratch));
                lineChanges += RamAt<uint8_t>(ram, a.a0 + 0x789) != lineBefore;
                reversing += RequestOf(scratch, a.a1).reverse != 0;
            },
            skipped);
        std::printf("%-10s 0x80037834  %zu cases (%zu line changes, %zu reversing), %zu mismatches, %zu skipped (original trapped)  %s\n", "AiDrive", r.cases,
                    lineChanges, reversing, r.mismatches, skipped, r.mismatches ? "FAIL" : "ok");
        failures += r.mismatches ? 1 : 0;
    }
    return failures;
}

} // namespace gt2::verify
