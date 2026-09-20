// Differential checks of src/game/sim/race_shell.* against the original (see guest.h for the harness): the
// race shell around the physics tick on the attract-race dump. The shell's state is scattered over the
// executable's globals (race_shell.h RaceShellState lists every address); it is loaded from the RAM image before a
// routine runs and stored back afterwards, so that the whole image can still be compared byte for byte. The
// car records, the contact tables and the results records are worked on in place. The two calls into the
// sound driver (0x800189C4, 0x80060840) are hooks; the verifier replays them through the guest on our image.
#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "game/sim/field.h"
#include "game/sim/race_shell.h"
#include "game/sim/race_sim.h"
#include "guest.h"

namespace gt2::verify {

namespace {

// ---- addresses of the shell's state and inputs (US Simulation v1.2)
constexpr uint32_t kClock = 0x80046F64u, kTwoPlayerLaps = 0x80046F68u, kTimeLimited = 0x80046F69u;
constexpr uint32_t kHoldInitial = 0x800A951Eu, kHold = 0x800A9520u, kSinceFinish = 0x800A9522u;
constexpr uint32_t kBoard = 0x801C8580u;
constexpr uint32_t kStartTimer = 0x800AF224u, kEndTimer = 0x800AF226u, kEndTimerAux = 0x800AF228u;
constexpr uint32_t kClockFrames = 0x8002F864u;
constexpr uint32_t kFinishPosition = 0x801D5DE8u, kNewRecord = 0x801D5DE9u, kPointsTotal = 0x801D5E7Cu, kPointsRace = 0x801D5E82u;
constexpr uint32_t kResults1 = 0x801D5E88u, kResults2 = 0x801DA3A0u;
constexpr uint32_t kCourseRecordPointer = 0x800A9524u, kRaceTaskPointer = 0x8002F4F4u;
constexpr uint32_t kGameMode = 0x801D5866u, kFrameStep = 0x801D5864u, kLapCount = 0x801D586Bu, kCountdown = 0x801D5869u;
constexpr uint32_t kCarCountTick = 0x800AF231u, kCarCountShell = 0x801D58B6u, kDemoFlag = 0x800A951Cu;
constexpr uint32_t kPlayers = 0x801D5DF6u, kRate = 0x801C8570u, kWear = 0x80046F48u;
constexpr uint32_t kLicenseResult = 0x801D5DECu, kLicenseTime = 0x801D5DF0u, kLicenseBlock = 0x801C98A0u; // licence tests (mode 3)
constexpr uint32_t kPointsTable = 0x8002F4CCu, kLabelTable = 0x8002F4BCu;
constexpr uint32_t kCourseLengthPointer = 0x800B4A44u, kStartLineCount = 0x800B4A58u, kStartLines = 0x800B4A5Cu;
constexpr uint32_t kRaceData = 0x801C8568u, kCourseIndex = 0x800AF230u, kCourseTable = 0x801E18E0u, kCourseId = 0x801D589Cu;
constexpr uint32_t kContact = 0x801C8608u, kPads = 0x800A9590u;

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
uint32_t BodyAddress(uint32_t car) { return kCarBase + car * kCarStride + kBodyOffset; }
uint8_t* BodyAt(uint8_t* ram, uint32_t car) { return At(ram, BodyAddress(car)); }
uint8_t* RecordAt(uint8_t* ram, uint32_t car) { return At(ram, kCarBase + car * kCarStride); }

struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
    int32_t Pick(std::initializer_list<int32_t> values) { return *(values.begin() + Next() % values.size()); }
};

// ---- the shell state as the RAM holds it
sim::RaceShellState LoadState(const uint8_t* ram) {
    sim::RaceShellState s;
    s.raceClock = Get<uint32_t>(ram, D(kClock));
    s.twoPlayerLaps = Get<uint8_t>(ram, D(kTwoPlayerLaps));
    s.timeLimited = Get<uint8_t>(ram, D(kTimeLimited));
    s.holdInitial = Get<uint16_t>(ram, D(kHoldInitial));
    s.hold = Get<uint16_t>(ram, D(kHold));
    s.sinceFinish = Get<uint16_t>(ram, D(kSinceFinish));
    std::memcpy(&s.board, ram + (D(kBoard) & 0x1FFFFF), sizeof(s.board));
    s.startTimer = Get<int16_t>(ram, D(kStartTimer));
    s.endTimer = Get<int16_t>(ram, D(kEndTimer));
    s.endTimerAux = Get<int16_t>(ram, D(kEndTimerAux));
    s.clockFrames = Get<uint8_t>(ram, D(kClockFrames));
    s.finishPosition = Get<uint8_t>(ram, D(kFinishPosition));
    s.newRecord = Get<uint8_t>(ram, D(kNewRecord));
    s.licenseResult = Get<int32_t>(ram, D(kLicenseResult));
    s.licenseTime = Get<uint32_t>(ram, D(kLicenseTime));
    std::memcpy(s.pointsTotal.data(), ram + (D(kPointsTotal) & 0x1FFFFF), 6);
    std::memcpy(s.pointsRace.data(), ram + (D(kPointsRace) & 0x1FFFFF), 6);
    std::memcpy(&s.results[0], ram + (D(kResults1) & 0x1FFFFF), sizeof(sim::PlayerResults));
    std::memcpy(&s.results[1], ram + (D(kResults2) & 0x1FFFFF), sizeof(sim::PlayerResults));
    std::memcpy(&s.courseRecord, ram + (Get<uint32_t>(ram, D(kCourseRecordPointer)) & 0x1FFFFF), sizeof(sim::LapEntry));
    const uint32_t task = Get<uint32_t>(ram, D(kRaceTaskPointer));
    s.musicRequestFlag = Get<uint8_t>(ram, task + 0x2ED);
    s.musicRequest = Get<uint8_t>(ram, task + 0x2EE);
    s.musicRaceTrack = Get<uint8_t>(ram, task + 0x2EF);
    s.music2F0 = Get<uint8_t>(ram, task + 0x2F0);
    s.music2F1 = Get<uint8_t>(ram, task + 0x2F1);
    return s;
}

void StoreState(uint8_t* ram, const sim::RaceShellState& s) {
    Put<uint32_t>(ram, D(kClock), s.raceClock);
    Put<uint8_t>(ram, D(kTwoPlayerLaps), s.twoPlayerLaps);
    Put<uint8_t>(ram, D(kTimeLimited), s.timeLimited);
    Put<uint16_t>(ram, D(kHoldInitial), s.holdInitial);
    Put<uint16_t>(ram, D(kHold), s.hold);
    Put<uint16_t>(ram, D(kSinceFinish), s.sinceFinish);
    std::memcpy(ram + (D(kBoard) & 0x1FFFFF), &s.board, sizeof(s.board));
    Put<int16_t>(ram, D(kStartTimer), s.startTimer);
    Put<int16_t>(ram, D(kEndTimer), s.endTimer);
    Put<int16_t>(ram, D(kEndTimerAux), s.endTimerAux);
    Put<uint8_t>(ram, D(kClockFrames), s.clockFrames);
    Put<uint8_t>(ram, D(kFinishPosition), s.finishPosition);
    Put<uint8_t>(ram, D(kNewRecord), s.newRecord);
    Put<int32_t>(ram, D(kLicenseResult), s.licenseResult);
    Put<uint32_t>(ram, D(kLicenseTime), s.licenseTime);
    std::memcpy(ram + (D(kPointsTotal) & 0x1FFFFF), s.pointsTotal.data(), 6);
    std::memcpy(ram + (D(kPointsRace) & 0x1FFFFF), s.pointsRace.data(), 6);
    std::memcpy(ram + (D(kResults1) & 0x1FFFFF), &s.results[0], sizeof(sim::PlayerResults));
    std::memcpy(ram + (D(kResults2) & 0x1FFFFF), &s.results[1], sizeof(sim::PlayerResults));
    std::memcpy(ram + (Get<uint32_t>(ram, D(kCourseRecordPointer)) & 0x1FFFFF), &s.courseRecord, sizeof(sim::LapEntry));
    const uint32_t task = Get<uint32_t>(ram, D(kRaceTaskPointer));
    Put<uint8_t>(ram, task + 0x2ED, s.musicRequestFlag);
    Put<uint8_t>(ram, task + 0x2EE, s.musicRequest);
    Put<uint8_t>(ram, task + 0x2EF, s.musicRaceTrack);
    Put<uint8_t>(ram, task + 0x2F0, s.music2F0);
    Put<uint8_t>(ram, task + 0x2F1, s.music2F1);
}

sim::ShellGlobals LoadGlobals(const uint8_t* ram) {
    sim::ShellGlobals g;
    g.gameMode = Get<uint8_t>(ram, D(kGameMode));
    g.frameStep = Get<uint8_t>(ram, D(kFrameStep));
    g.lapCount = Get<uint8_t>(ram, D(kLapCount));
    g.carCount = Get<uint8_t>(ram, D(kCarCountTick));
    g.carCountShell = Get<uint8_t>(ram, D(kCarCountShell));
    g.demoFlag = Get<uint8_t>(ram, D(kDemoFlag));
    g.countdownEnabled = Get<uint8_t>(ram, D(kCountdown));
    g.players801D5DF6 = Get<uint8_t>(ram, D(kPlayers));
    g.license.targetLap = Get<uint8_t>(ram, D(kLicenseBlock) + 1);
    g.license.type = Get<uint8_t>(ram, D(kLicenseBlock) + 2);
    g.license.boxStart = Get<uint8_t>(ram, D(kLicenseBlock) + 0x24);
    g.license.boxLength = Get<uint8_t>(ram, D(kLicenseBlock) + 0x25);
    g.rate = Get<int32_t>(ram, D(kRate));
    g.wear.wearLimit = Get<int32_t>(ram, D(kWear));
    g.wear.wornGripLoss = Get<int32_t>(ram, D(kWear) + 4);
    g.wear.pitGripFactor = Get<int32_t>(ram, D(kWear) + 8);
    g.wear.coldLimit = Get<int32_t>(ram, D(kWear) + 12);
    g.wear.coldGripLoss = Get<int32_t>(ram, D(kWear) + 16);
    g.wear.wearKnee = Get<int32_t>(ram, D(kWear) + 20);
    g.wear.kneeGripLoss = Get<int32_t>(ram, D(kWear) + 24);
    for (uint32_t i = 0; i < 6; i++) g.pointsByPosition[i] = Get<uint8_t>(ram, D(kPointsTable) + i);
    for (uint32_t i = 0; i < 4; i++) g.splitLabels[i] = Get<uint32_t>(ram, D(kLabelTable) + i * 4);
    g.lapTimeLabel = D(sim::kLapTimeLabel); // "Lap Time" of the dump's build's race text copy
    return g;
}

// The course table entry of `index` (0x80060E94) and the index of the entry with `id` (0x80060EB4, 0 when absent).
uint32_t CourseEntry(uint32_t index) { return D(kCourseTable) + 8 + index * 24; }
uint32_t CourseIndexOfId(const uint8_t* ram, uint32_t id) {
    const uint32_t count = Get<uint16_t>(ram, D(kCourseTable) + 6);
    for (uint32_t i = 0; i < count; i++)
        if (Get<uint32_t>(ram, CourseEntry(i) + 4) == id) return i;
    return 0;
}

sim::ShellCourse LoadCourse(const uint8_t* ram) {
    sim::ShellCourse c;
    c.courseLength = Get<int32_t>(ram, Get<uint32_t>(ram, D(kCourseLengthPointer)));
    c.startLineCount = Get<int32_t>(ram, D(kStartLineCount));
    for (int32_t i = 0; i <= c.startLineCount; i++) c.startLines.push_back(Get<int32_t>(ram, D(kStartLines) + uint32_t(i) * 4)); // count + 1 words, as the original indexes them
    const uint32_t grid = Get<uint32_t>(ram, Get<uint32_t>(ram, D(kRaceData)) + 0x18);
    c.grid.count = grid ? Get<int32_t>(ram, grid) : 0;
    for (int32_t slot = 0; slot < c.grid.count; slot++) {
        c.grid.distance.push_back(Get<int32_t>(ram, grid + 4 + uint32_t(slot) * 0x28 + 0x14));
        c.grid.heading.push_back(Get<int32_t>(ram, grid + 4 + uint32_t(slot) * 0x28 + 0x24));
    }
    c.pointToPoint = (Get<uint16_t>(ram, CourseEntry(Get<uint8_t>(ram, D(kCourseIndex))) + 8) & 0x20) != 0;
    c.pointToPointById = (Get<uint16_t>(ram, CourseEntry(CourseIndexOfId(ram, Get<uint32_t>(ram, D(kCourseId)))) + 8) & 0x20) != 0;
    c.ai = AiContextFromRam(ram);
    return c;
}

// Replays a sound-driver call of the port through the guest on our image (the guest keeps the original's result).
struct SoundBridge {
    Guest* guest = nullptr;
    uint8_t* ram = nullptr;
    std::vector<uint8_t> saved = std::vector<uint8_t>(Bus::kRamSize);
    size_t calls = 0;
    static void Hook(void* user, uint32_t routine, int32_t argument) {
        SoundBridge& b = *static_cast<SoundBridge*>(user);
        b.calls++;
        std::memcpy(b.saved.data(), b.guest->Ram(), Bus::kRamSize);
        std::memcpy(b.guest->Ram(), b.ram, Bus::kRamSize);
        b.guest->Call(routine, uint32_t(argument));
        std::memcpy(b.ram, b.guest->Ram(), Bus::kRamSize);
        std::memcpy(b.guest->Ram(), b.saved.data(), Bus::kRamSize);
    }
};

// Game mode 6: what 0x8001286C does outside the race state when player 1's lap becomes the ghost's reference (ShellHooks::
// ghostReference): the race block's entry 1 = entry 0 (0x8C bytes) with kind 2, car 1's parameter record = car 0's, the car
// sound restart 0x80014674 / 0x800145F4 of car 1 (replayed through the guest on our image) and car 1's + 0x878 / + 0x7C4 block.
void GhostReferenceExtras(void* user) {
    SoundBridge& b = *static_cast<SoundBridge*>(user);
    uint8_t* ram = b.ram;
    std::memcpy(At(ram, D(0x801D5988u)), At(ram, D(0x801D58B8u)), 0x8C);
    Put<uint8_t>(ram, D(0x801D5A16u), 2);
    std::memcpy(At(ram, D(0x801DEA7Au)), At(ram, D(0x801DE8BAu)), 0x1C0);
    const uint32_t car0 = kCarBase, car1 = kCarBase + kCarStride;
    SoundBridge::Hook(user, 0x80014674u, int32_t(car1));
    Put<uint32_t>(ram, car1 + 0x878, Get<uint32_t>(ram, car0 + 0x878));
    std::memcpy(At(ram, car1 + 0x7C4), At(ram, car0 + 0x7C4), 0x40);
    SoundBridge::Hook(user, 0x800145F4u, int32_t(car1));
}

// The context of the port over our RAM image (state loaded here, stored by Finish()).
struct NativeShell {
    sim::RaceShellState state;
    sim::ShellGlobals globals;
    sim::ShellCourse course;
    sim::ShellContext ctx;
    SoundBridge bridge;
    sim::GhostSession ghost, ghostLoaded; // game mode 6
    uint8_t* ram = nullptr;
    NativeShell(uint8_t* image, Guest& guest) : ram(image) {
        state = LoadState(image);
        globals = LoadGlobals(image);
        course = LoadCourse(image);
        ctx.state = &state;
        ctx.globals = &globals;
        ctx.course = &course;
        ctx.cars = reinterpret_cast<sim::Car*>(At(image, kCarBase));
        ctx.contact = reinterpret_cast<sim::CarContactState*>(At(image, ContactBase())); // the shell touches the corner tables only
        ctx.allowOutOfRangeGap = true;
        bridge.guest = &guest;
        bridge.ram = image;
        ctx.hooks.user = &bridge;
        ctx.hooks.sound = &SoundBridge::Hook;
        ctx.hooks.ghostReference = &GhostReferenceExtras;
        ctx.bodyTokenBase = kCarBase + kBodyOffset; // the bodies' pointers are RAM addresses here
        ctx.bodyTokenStride = kCarStride;
        LoadGhostSession(image, ghost);
        ghostLoaded = ghost;
        if (globals.gameMode == 6) ctx.ghost = &ghost;
    }
    void Finish() {
        StoreState(ram, state);
        StoreGhostSession(ram, ghost, ghostLoaded);
    }
};

// One row: `prepare` randomises the guest image, `original` runs the routine in the guest, `native` the port on
// our copy (through a NativeShell); afterwards all RAM outside the guest stack (and the scratchpad) must match.
struct Row {
    Guest& guest;
    const std::vector<uint8_t>& pristine;
    int& failures;
    // `compareReturn`: the routine returns a value (v0) that the port must reproduce.
    template <typename Prepare, typename Original, typename Native>
    void Run(const char* name, uint32_t address, size_t variants, Prepare prepare, Original original, Native native, bool compareReturn = false) {
        std::vector<uint8_t> ours(Bus::kRamSize);
        size_t cases = 0, mismatches = 0;
        for (size_t variant = 0; variant < variants; variant++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            prepare(guest.Ram(), variant);
            std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
            uint32_t expected = 0, got = 0;
            bool threw = false;
            try {
                expected = original(guest);
                NativeShell shell(ours.data(), guest);
                got = native(shell);
                shell.Finish();
            } catch (const std::exception& e) {
                threw = true;
                if (mismatches < 3) std::printf("    EXCEPTION variant %zu: %s\n", variant, e.what());
            }
            cases++;
            const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
            if (!compareReturn) got = expected;
            const bool equal = !threw && expected == got && std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                               std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0;
            if (!equal && mismatches++ < 3) {
                if (threw) continue;
                if (expected != got) std::printf("    MISMATCH variant %zu: return original %u ours %u\n", variant, expected, got);
                size_t shown = 0;
                for (uint32_t i = 0; i < Bus::kRamSize && shown < 6; i++)
                    if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                        std::printf("    MISMATCH variant %zu: differing byte at 0x%08X: original %02X ours %02X\n", variant, 0x80000000u + i, guest.Ram()[i], ours[i]);
                        shown++;
                    }
            }
        }
        Report(name, address, cases, mismatches, failures);
    }
};

// ---- randomisation of the shell's inputs on the guest image
struct Scenario {
    Rng& rng;
    const Track* track;
    const sim::NativeCourse* course;
    int32_t courseLength = 0;
    std::vector<int32_t> lines; // the sector lines and 0 (the lap line)
    std::vector<uint8_t> drivenLines; // the AI lines that have a section list on this course (0x801C8568 -> +8)
    bool licenseDump = false;         // the dump is a licence test (mode 3): mode 3 is drawn more often

    // `allowTwoPlayer` = false for the rows that reach 0x8003C70C (its mode 6 branches are not ported).
    uint8_t RaceMode(bool allowTwoPlayer) {
        if (rng.Chance(licenseDump ? 2 : 6)) return allowTwoPlayer && rng.Chance(2) ? 6 : 3;
        if (rng.Chance(2)) return 0;
        return uint8_t(rng.Pick({2, 4, 0xC, 1, 7, 8, 9, 10, 11, 5}));
    }
    // The licence test bytes of the race settings block and the shell's licence result.
    void License(uint8_t* ram) {
        Put<uint8_t>(ram, D(kLicenseBlock) + 1, uint8_t(rng.Chance(4) ? rng.Next() : rng.Range(1, 4)));
        Put<uint8_t>(ram, D(kLicenseBlock) + 2, uint8_t(rng.Chance(8) ? rng.Range(0, 6) : rng.Pick({2, 3, 5})));
        if (!rng.Chance(3)) { // the box: around the dump's or anywhere on the course
            Put<uint8_t>(ram, D(kLicenseBlock) + 0x24, uint8_t(rng.Range(0, std::min(255, courseLength / 0xA0000))));
            Put<uint8_t>(ram, D(kLicenseBlock) + 0x25, uint8_t(rng.Chance(4) ? rng.Next() : rng.Range(5, 40)));
        }
        Put<int32_t>(ram, D(kLicenseResult), rng.Chance(4) ? rng.Pick({-1, 0, 1, 3, 4, 5}) : rng.Chance(2) ? 1 : int32_t(rng.Next()));
        Put<uint32_t>(ram, D(kLicenseTime), rng.Next());
    }
    void Globals(uint8_t* ram, bool allowTwoPlayer) {
        Put<uint8_t>(ram, D(kGameMode), RaceMode(allowTwoPlayer));
        License(ram);
        Put<uint8_t>(ram, D(kFrameStep), uint8_t(rng.Chance(4) ? 1 : 2));
        Put<uint8_t>(ram, D(kLapCount), uint8_t(rng.Chance(8) ? 99 : rng.Chance(8) ? 0 : rng.Range(1, 5)));
        Put<uint8_t>(ram, D(kCountdown), uint8_t(rng.Range(0, 1)));
        Put<uint8_t>(ram, D(kDemoFlag), uint8_t(rng.Chance(3) ? 1 : 0));
        Put<uint8_t>(ram, D(kPlayers), uint8_t(rng.Range(0, 2)));
        Put<uint32_t>(ram, D(kClock), rng.Chance(4) ? uint32_t(rng.Range(0, 3000)) : rng.Chance(8) ? 0x01499700u + uint32_t(rng.Range(-200, 200)) : uint32_t(rng.Range(0, 3000000)));
        Put<uint8_t>(ram, D(kTwoPlayerLaps), 0);
        Put<uint8_t>(ram, D(kTimeLimited), uint8_t(rng.Chance(3) ? 1 : 0));
        Put<uint16_t>(ram, D(kHold), uint16_t(rng.Chance(5) ? rng.Range(0, 200) : 0));
        Put<uint16_t>(ram, D(kSinceFinish), uint16_t(rng.Chance(3) ? rng.Range(0, 400) : 0));
        Put<uint8_t>(ram, D(kFinishPosition), uint8_t(rng.Next()));
        Put<uint8_t>(ram, D(kNewRecord), uint8_t(rng.Range(0, 1)));
        for (uint32_t i = 0; i < 6; i++) { Put<uint8_t>(ram, D(kPointsTotal) + i, uint8_t(rng.Range(0, 60))); Put<uint8_t>(ram, D(kPointsRace) + i, uint8_t(rng.Range(0, 10))); }
        const uint32_t task = Get<uint32_t>(ram, D(kRaceTaskPointer));
        for (uint32_t o = 0x2ED; o <= 0x2F1; o++) Put<uint8_t>(ram, task + o, uint8_t(rng.Chance(2) ? 0xFF : rng.Range(0, 5)));
        // the course record the HUD compares with
        const uint32_t record = Get<uint32_t>(ram, D(kCourseRecordPointer));
        Put<int32_t>(ram, record, rng.Chance(2) ? -1 : rng.Range(60000, 200000));
        for (uint32_t i = 1; i < 5; i++) Put<int32_t>(ram, record + i * 4, rng.Range(-1, 100000));
        // point-to-point courses: the flag bit of the course table entry
        const uint32_t entry = CourseEntry(Get<uint8_t>(ram, D(kCourseIndex)));
        Put<uint16_t>(ram, entry + 8, uint16_t((Get<uint16_t>(ram, entry + 8) & ~0x20u) | (rng.Chance(5) ? 0x20u : 0u)));
    }
    void Results(uint8_t* ram, uint32_t address) {
        sim::PlayerResults r{};
        r.position = int16_t(rng.Range(0, 6));
        // At least one kept lap: with an empty record and lapNumber == lap, 0x80013824 (mode 6) copies the course
        // record from the 20 bytes before the record (see OnLapLine), outside the state the port holds.
        r.count = int16_t(rng.Chance(3) ? 10 : rng.Range(1, 10));
        r.lapNumber = int16_t(rng.Chance(8) ? 999 : rng.Range(0, 12));
        r.bestLapNumber = int16_t(rng.Range(-1, 12));
        auto entry = [&]() {
            sim::LapEntry e;
            e.time = rng.Chance(3) ? -1 : rng.Range(50000, 300000);
            for (int32_t& v : e.split) v = rng.Range(-1, 100000);
            e.maxSpeed = int16_t(rng.Range(0, 3000));
            e.pad = int16_t(rng.Chance(2) ? -1 : rng.Next());
            return e;
        };
        for (sim::LapEntry& e : r.laps) e = entry();
        r.best = entry();
        r.pending = entry();
        r.finishTime = rng.Range(0, 1000000);
        std::memcpy(At(ram, address), &r, sizeof(r));
    }
    void Board(uint8_t* ram) {
        sim::SectorBoard b;
        for (uint32_t row = 0; row < 4; row++) {
            const int32_t count = rng.Range(0, 5); // at most five: a sixth entry may be added (a seventh would overflow the board in the original)
            b.count[row] = int8_t(count);
            b.shown[row] = int8_t(rng.Chance(3) ? count : rng.Range(0, count));
            b.bestLap[row] = int16_t(rng.Chance(4) ? -1 : rng.Range(0, 3));
            int32_t t = rng.Range(10000, 200000);
            for (uint32_t i = 0; i < 6; i++) {
                b.times[row][i] = i < uint32_t(count) ? t : sim::kNoTime;
                t += rng.Range(0, 5000);
                b.car[row][i] = int8_t(rng.Range(0, 5));
            }
        }
        std::memcpy(At(ram, D(kBoard)), &b, sizeof(b));
    }
    void Record(uint8_t* ram, uint32_t car) {
        uint8_t* record = RecordAt(ram, car);
        sim::SetField<int16_t>(record, 0x18, int16_t(rng.Chance(2) ? rng.Range(2, 3) : rng.Range(0, 5)));
        sim::SetField<int32_t>(record, 0x24, rng.Next());
        sim::SetField<int8_t>(record, 0xA8C, int8_t(rng.Range(0, 1)));
        for (uint32_t o = 0xA8E; o <= 0xA92; o += 2) sim::SetField<int16_t>(record, o, int16_t(rng.Chance(2) ? 0 : rng.Range(-1, 130)));
        for (uint32_t o = 0xA94; o <= 0xAA4; o += 4) sim::SetField<int32_t>(record, o, rng.Next());
    }
    // The race progress fields of a body. `distance` = the stored course distance (16.16 m).
    void Progress(uint8_t* ram, uint32_t car, int32_t distance) {
        uint8_t* body = BodyAt(ram, car);
        sim::SetField<int32_t>(body, 0x604, distance);
        // Mode 3 numbers the split displays of all laps (lap * (lines + 1) + line, 0x8003D314 / 0x8003D3C0); the
        // original keeps them in a 4-word area of the results record and a 4-entry caption table, so a licence test
        // has at most 4 / (lines + 1) laps before its goal (larger indices would reach unrelated memory).
        const int32_t maxLap = Get<uint8_t>(ram, D(kGameMode)) == 3 ? 4 / int32_t(lines.size()) : 5;
        sim::SetField<int16_t>(body, 0x608, int16_t(rng.Chance(4) ? 0 : rng.Range(1, maxLap)));
        int32_t sector = 0;
        for (size_t i = 0; i < lines.size() - 1; i++)
            if (lines[i] <= distance) sector = int32_t(i) + 1;
        sim::SetField<uint8_t>(body, 0x6B0, uint8_t(rng.Chance(4) ? rng.Range(0, int32_t(lines.size()) - 1) : sector));
        sim::SetField<int8_t>(body, 0x6B2, int8_t(rng.Range(3, 8)));
        sim::SetField<uint16_t>(body, 0x6AE, uint16_t(rng.Range(0, 3000)));
        sim::SetField<uint16_t>(body, 0x6F8, uint16_t(rng.Range(0, 3000)));
        sim::SetField<uint8_t>(body, 0x6FA, uint8_t(rng.Chance(3) ? rng.Range(0, 2) : 0));
        sim::SetField<uint8_t>(body, 0x6FB, uint8_t(rng.Range(0, 50)));
        sim::SetField<uint8_t>(body, 0x6FC, uint8_t(rng.Chance(3) ? rng.Range(0, 2) : 0));
        sim::SetField<uint8_t>(body, 0x6FD, uint8_t(rng.Range(0, 1)));
        sim::SetField<uint8_t>(body, 0x764, uint8_t(rng.Range(0, 12)));
        sim::SetField<uint8_t>(body, 0x765, uint8_t(rng.Chance(2) ? 0 : rng.Range(0, 255)));
        sim::SetField<uint8_t>(body, 0x750, uint8_t(rng.Range(1, 6)));
        sim::SetField<uint32_t>(body, 0x780, uint32_t(rng.Range(0, 300000)));
        sim::SetField<uint8_t>(body, 0x785, uint8_t(rng.Chance(3) ? rng.Range(0, 3) : 0));
        sim::SetField<uint8_t>(body, 0x786, uint8_t(rng.Chance(2) ? 0 : rng.Range(0, 7)));
        sim::SetField<uint8_t>(body, 0x788, uint8_t(rng.Range(0, 4)));
        // The car's line always has a section list (0x800358E0 picks only such lines); on a course without one of
        // these lines (the licence course has only line 6) the original would read through a null list pointer.
        static constexpr uint8_t kLines[] = {6, 6, 6, 2, 1, 3, 6, 2, 1, 3, 6, 4};
        uint8_t line = kLines[rng.Next() % 12];
        if (std::find(drivenLines.begin(), drivenLines.end(), line) == drivenLines.end()) line = drivenLines[rng.Next() % drivenLines.size()];
        sim::SetField<uint8_t>(body, 0x789, line);
        sim::SetField<uint8_t>(body, 0x78D, uint8_t(rng.Chance(2) ? 0 : rng.Next()));
        sim::SetField<uint16_t>(body, 0x78E, uint16_t(rng.Chance(2) ? 0 : rng.Chance(2) ? Get<int32_t>(ram, D(kRate)) : rng.Range(0, 300)));
        sim::SetField<uint8_t>(body, 0x63D, uint8_t(rng.Chance(3) ? rng.Range(0, 3) : 0));
        sim::SetField<int8_t>(body, 0x45D, int8_t(rng.Chance(2) ? 2 : rng.Range(0, 1)));
        sim::SetField<uint8_t>(body, 0x45E, uint8_t(rng.Chance(4) ? rng.Range(0, 2) : 0));
        sim::SetField<int32_t>(body, 0x6A4, rng.Chance(3) ? rng.Range(-0x2000, 0x2000) : rng.Range(-0x40000, 0x40000));
        sim::SetField<int32_t>(body, 0x6A8, rng.Chance(3) ? rng.Range(-0x2000, 0x2000) : rng.Range(-0x20000, 0x20000));
        for (uint32_t w = 0; w < 4; w++) {
            uint8_t* wheel = body + 0x460 + w * 0x68;
            sim::SetField<int32_t>(wheel, 0x64, rng.Chance(2) ? Get<int32_t>(ram, D(kWear) + 12) : rng.Range(-100000, 100000));
            sim::SetField<uint8_t>(wheel, 0x22, uint8_t(rng.Chance(3) ? rng.Range(0, 3) : 0));
        }
    }
    // Places the body on the course at `distance` (a chunk whose distance is close), sets the stored course
    // distance `behind` metres back (so that the step crosses a line when one lies in between).
    void PlaceNear(uint8_t* ram, uint32_t car, int32_t target, int32_t behind) {
        const Track& t = *track;
        if (target >= courseLength) target -= courseLength;
        uint32_t best = 0;
        int32_t bestDelta = 0x7FFFFFFF;
        for (uint32_t c = 0; c < t.chunks.size(); c++) {
            const int32_t delta = t.chunks[c].distance - target;
            if (delta >= 0 && delta < bestDelta) { bestDelta = delta; best = c; }
        }
        const TrackChunk& chunk = t.chunks[best];
        uint8_t* body = BodyAt(ram, car);
        const int32_t x = chunk.origin[0] + rng.Range(-0x40000, 0x40000), h = chunk.origin[1] + rng.Range(0, 0x8000), z = chunk.origin[2] + rng.Range(-0x40000, 0x40000);
        sim::SetField<int32_t>(body, 0x65C, x >> 4);
        sim::SetField<int32_t>(body, 0x660, int32_t(0u - uint32_t(z)) >> 4);
        sim::SetField<int32_t>(body, 0x664, h >> 4);
        sim::SetField<int32_t>(body, 0x600, int32_t(best));
        const int32_t point[3] = {sim::Field<int32_t>(body, 0x65C) << 4, sim::Field<int32_t>(body, 0x664) << 4, z};
        int32_t now = course->CourseDistanceOfWorldPoint(int32_t(best), point);
        int32_t previous = now - behind;
        if (previous < 0) previous += courseLength;
        if (previous >= courseLength) previous -= courseLength;
        Progress(ram, car, previous);
    }
    // A step of the car: either anywhere on the course or just past one of the lines.
    void PlaceForStep(uint8_t* ram, uint32_t car) {
        if (rng.Chance(2)) {
            const int32_t line = lines[rng.Next() % lines.size()];
            const int32_t past = rng.Range(0, 0x80000); // up to 8 m past the line
            PlaceNear(ram, car, line + past, rng.Chance(4) ? rng.Range(-0x40000, 0x40000) : rng.Range(0, past + 0x60000));
        } else {
            PlaceNear(ram, car, rng.Range(0, courseLength - 1), rng.Chance(4) ? rng.Range(-0x100000, 0x100000) : rng.Range(0, 0x80000));
        }
    }
};

} // namespace

int VerifyShell(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& mt, const Track* track) {
    int failures = 0;
    Rng rng{mt};
    if (!track) {
        std::puts("Shell      0x8003CF94  skipped (needs the course: gt2verify <ram> <disc> <course>)");
        return failures;
    }
    const sim::CourseExtras extras = sim::BuildCourseExtras(*track, {});
    const sim::NativeCourse course(*track, extras);
    Scenario sc{rng, track, &course};
    sc.courseLength = Get<int32_t>(pristine.data(), Get<uint32_t>(pristine.data(), D(kCourseLengthPointer)));
    for (int32_t i = 0; i < Get<int32_t>(pristine.data(), D(kStartLineCount)); i++) sc.lines.push_back(Get<int32_t>(pristine.data(), D(kStartLines) + uint32_t(i) * 4));
    sc.lines.push_back(0);
    {
        const sim::AiContext ai = AiContextFromRam(pristine.data());
        for (uint8_t line = 0; line < 7; line++)
            if (ai.course.lines[line].sections != nullptr && ai.course.lines[line].count > 0) sc.drivenLines.push_back(line);
        if (sc.drivenLines.empty()) {
            std::puts("Shell      0x8003CF94  skipped (the dump's race object has no AI line list)");
            return failures;
        }
        sc.licenseDump = Get<uint8_t>(pristine.data(), D(kGameMode)) == 3;
    }
    {
        const sim::ShellCourse c = LoadCourse(pristine.data());
        const sim::RaceShellState s = LoadState(pristine.data());
        std::printf("           (shell: course %.1f m, %d sector lines, grid list %d slots, hold %u, clock %u, mode %u, laps %u, cars %u)\n", c.courseLength / 65536.0,
                    c.startLineCount, c.grid.count, s.hold, s.raceClock, Get<uint8_t>(pristine.data(), D(kGameMode)), Get<uint8_t>(pristine.data(), D(kLapCount)),
                    Get<uint8_t>(pristine.data(), D(kCarCountTick)));
    }
    Row row{guest, pristine, failures};
    auto prepareAll = [&](uint8_t* ram, bool allowSpecial) {
        sc.Globals(ram, allowSpecial);
        sc.Board(ram);
        sc.Results(ram, D(kResults1));
        sc.Results(ram, D(kResults2));
        for (uint32_t car = 0; car < kCarCount; car++) {
            sc.Record(ram, car);
            sc.PlaceForStep(ram, car);
        }
    };
    auto carOf = [&](size_t variant) { return uint32_t(variant % kCarCount); };

    // ---- 0x80030308: message request (body, code, frames)
    {
        uint32_t code = 0, frames = 0;
        row.Run("Message", 0x80030308u, 300,
                [&](uint8_t* ram, size_t variant) { prepareAll(ram, true); code = uint32_t(rng.Range(0, 12)); frames = uint32_t(rng.Chance(2) ? 0 : rng.Range(1, 120)); (void)variant; },
                [&](Guest& g) { return g.Call(0x80030308u, BodyAddress(0), code, frames); },
                [&](NativeShell& s) { sim::RequestMessage(s.ctx.cars[0].body, uint8_t(code), int32_t(frames)); return 0u; });
    }
    // ---- 0x8003D138: time limit
    row.Run("TimeLimit", 0x8003D138u, 300, [&](uint8_t* ram, size_t) { prepareAll(ram, true); }, [&](Guest& g) { return g.Call(0x8003D138u); },
            [&](NativeShell& s) { return sim::TimeLimitReached(s.ctx) ? 1u : 0u; }, true);
    // ---- 0x80035714: finished? (body)
    row.Run("Finished", 0x80035714u, 600, [&](uint8_t* ram, size_t) { prepareAll(ram, true); },
            [&](Guest& g) { return g.Call(0x80035714u, BodyAddress(0)); },
            [&](NativeShell& s) { return sim::HasFinished(s.ctx, s.ctx.cars[0].body) ? 1u : 0u; }, true);
    // ---- 0x8003FFDC: contact corners out of range (car, count)
    {
        uint32_t car = 0, count = 0;
        row.Run("Corners", 0x8003FFDCu, 300,
                [&](uint8_t* ram, size_t variant) {
                    car = carOf(variant);
                    count = uint32_t(rng.Range(1, 6));
                    for (uint32_t i = 0; i < 0xA98; i++) ram[(D(kContact) & 0x1FFFFF) + i] = uint8_t(rng.Next());
                },
                [&](Guest& g) { return g.Call(0x8003FFDCu, car, count); },
                [&](NativeShell& s) { sim::MarkContactCorners(*s.ctx.contact, car, count); return 0u; });
    }
    // ---- 0x80036980: race state change (body, code)
    {
        uint32_t car = 0, code = 0;
        row.Run("SetState", 0x80036980u, 900,
                [&](uint8_t* ram, size_t variant) { prepareAll(ram, true); car = carOf(variant); code = uint32_t(rng.Chance(6) ? rng.Range(8, 10) : rng.Range(0, 7)); },
                [&](Guest& g) { return g.Call(0x80036980u, BodyAddress(car), code); },
                [&](NativeShell& s) { return uint32_t(sim::SetRaceState(s.ctx, s.ctx.cars[car].body, code)); }, true);
    }
    // ---- 0x80036ACC: finish (body)
    {
        uint32_t car = 0;
        row.Run("Finish", 0x80036ACCu, 600, [&](uint8_t* ram, size_t variant) { prepareAll(ram, true); car = carOf(variant); },
                [&](Guest& g) { return g.Call(0x80036ACCu, BodyAddress(car)); },
                [&](NativeShell& s) { sim::Finish(s.ctx, s.ctx.cars[car].body); return 0u; });
    }
    // ---- 0x80036CA4: section state machine (body, current, previous)
    {
        uint32_t car = 0;
        int32_t current = 0, previous = 0;
        row.Run("Section", 0x80036CA4u, 1500,
                [&](uint8_t* ram, size_t variant) {
                    prepareAll(ram, true);
                    car = carOf(variant);
                    // around the grid list's slots and lines, sometimes anywhere
                    const sim::ShellCourse c = LoadCourse(ram);
                    if (c.grid.count > 0 && !rng.Chance(4)) {
                        const int32_t anchor = c.grid.distance[size_t(rng.Range(0, c.grid.count - 1))] + rng.Pick({0, 0, -0x190000, 0x8000});
                        current = anchor + rng.Range(-0x30000, 0x30000);
                    } else {
                        current = rng.Range(0, sc.courseLength - 1);
                    }
                    previous = current - (rng.Chance(4) ? rng.Range(-0x40000, 0x40000) : rng.Range(0, 0x40000));
                    if (current < 0) current += sc.courseLength;
                    if (previous < 0) previous += sc.courseLength;
                    if (current >= sc.courseLength) current -= sc.courseLength;
                    if (previous >= sc.courseLength) previous -= sc.courseLength;
                },
                [&](Guest& g) { return g.Call(0x80036CA4u, BodyAddress(car), uint32_t(current), uint32_t(previous)); },
                [&](NativeShell& s) { sim::SectionState(s.ctx, s.ctx.cars[car].body, current, previous); return 0u; });
    }
    // ---- 0x8003C520: sector board entry (body, lapIndex, sector, time, delta, car)
    {
        int32_t lapIndex = 0, sector = 0, time = 0, car = 0;
        row.Run("SectorRec", 0x8003C520u, 1500,
                [&](uint8_t* ram, size_t) {
                    prepareAll(ram, true);
                    sector = rng.Range(0, 3);
                    const int16_t best = sim::Field<int16_t>(At(ram, D(kBoard)), 0x60 + uint32_t(sector) * 2);
                    lapIndex = rng.Chance(2) ? best : rng.Range(-1, 4);
                    time = rng.Range(5000, 250000);
                    car = rng.Range(0, 5);
                    Put<uint32_t>(ram, kStack + 0x10, uint32_t(rng.Next()));
                    Put<uint32_t>(ram, kStack + 0x14, uint32_t(car));
                },
                [&](Guest& g) { return g.Call(0x8003C520u, BodyAddress(0), uint32_t(lapIndex), uint32_t(sector), uint32_t(time)); },
                [&](NativeShell& s) { sim::RecordSector(s.state.board, lapIndex, sector, time, car); return 0u; });
    }
    // ---- 0x8005E3C4: the player's lap record (results, lap, time, maxSpeed, invalid)
    {
        int32_t lap = 0, time = 0, maxSpeed = 0;
        uint32_t invalid = 0;
        row.Run("RecordLap", 0x8005E3C4u, 900,
                [&](uint8_t* ram, size_t) {
                    prepareAll(ram, true);
                    lap = rng.Chance(3) ? sim::Field<int16_t>(At(ram, D(kResults1)), 2) : rng.Range(0, 1000);
                    time = rng.Range(40000, 200000);
                    maxSpeed = rng.Range(0, 3000);
                    invalid = uint32_t(rng.Chance(3) ? 1 : 0);
                    Put<uint32_t>(ram, kStack + 0x10, invalid);
                },
                [&](Guest& g) { return g.Call(0x8005E3C4u, D(kResults1), uint32_t(lap), uint32_t(time), uint32_t(maxSpeed)); },
                [&](NativeShell& s) { sim::RecordLap(s.state.results[0], lap, time, maxSpeed, invalid != 0); return 0u; });
    }
    // ---- 0x80029C84: finish jingle (task object)
    row.Run("Jingle", 0x80029C84u, 300, [&](uint8_t* ram, size_t) { prepareAll(ram, true); },
            [&](Guest& g) { return g.Call(0x80029C84u, Get<uint32_t>(g.Ram(), D(kRaceTaskPointer))); },
            [&](NativeShell& s) { sim::FinishJingle(s.ctx); return 0u; });
    // ---- 0x8001555C -> 0x80013824: lap line display / finish (car, lap, elapsed, lapTime, maxSpeed, invalid)
    {
        uint32_t car = 0, lap = 0, elapsed = 0, lapTime = 0, maxSpeed = 0, invalid = 0;
        row.Run("LapLine", 0x8001555Cu, 1500,
                [&](uint8_t* ram, size_t variant) {
                    prepareAll(ram, true);
                    car = carOf(variant);
                    const uint32_t laps = Get<uint8_t>(ram, D(kLapCount));
                    lap = rng.Chance(2) ? (laps ? laps : 100) : uint32_t(rng.Chance(8) ? 999 : rng.Range(1, 6));
                    elapsed = uint32_t(rng.Range(30000, 600000));
                    lapTime = uint32_t(rng.Range(40000, 200000));
                    maxSpeed = uint32_t(rng.Range(0, 3000));
                    invalid = uint32_t(rng.Chance(3) ? 1 : 0);
                    Put<uint32_t>(ram, kStack + 0x10, maxSpeed);
                    Put<uint32_t>(ram, kStack + 0x14, invalid);
                },
                [&](Guest& g) { return g.Call(0x8001555Cu, car, lap, elapsed, lapTime); },
                [&](NativeShell& s) { sim::OnLapLine(s.ctx, int(car), int32_t(lap), int32_t(lapTime), int32_t(elapsed), uint16_t(maxSpeed), uint8_t(invalid)); return 0u; });
    }
    // ---- 0x80015510 -> 0x8001374C: split display (car, sector, split, invalid)
    {
        uint32_t car = 0, sector = 0, split = 0, invalid = 0;
        row.Run("Split", 0x80015510u, 600,
                [&](uint8_t* ram, size_t variant) {
                    prepareAll(ram, true);
                    car = carOf(variant);
                    sector = uint32_t(rng.Range(0, 2));
                    split = uint32_t(rng.Range(1000, 100000));
                    invalid = uint32_t(rng.Chance(3) ? 1 : 0);
                },
                [&](Guest& g) { return g.Call(0x80015510u, car, sector, split, invalid); },
                [&](NativeShell& s) { sim::OnSplit(s.ctx, int(car), int32_t(sector), int32_t(split), uint8_t(invalid)); return 0u; });
    }
    // ---- 0x800155C4: gap display (car, gap)
    {
        uint32_t car = 0, gap = 0;
        row.Run("Gap", 0x800155C4u, 600,
                [&](uint8_t* ram, size_t variant) { prepareAll(ram, true); car = carOf(variant); gap = uint32_t(rng.Range(0, 60000)); },
                [&](Guest& g) { return g.Call(0x800155C4u, car, gap); },
                [&](NativeShell& s) { sim::ShowGap(s.ctx, int(car), int32_t(gap)); return 0u; });
    }
    // ---- 0x8003C70C: lap / sector lines (body, previous, car)
    {
        uint32_t car = 0;
        int32_t previous = 0;
        row.Run("LapCheck", 0x8003C70Cu, 3000,
                [&](uint8_t* ram, size_t variant) {
                    prepareAll(ram, false);
                    car = carOf(variant);
                    // the stored distance is what the step computed; `previous` is the one before it
                    uint8_t* body = BodyAt(ram, car);
                    previous = sim::Field<int32_t>(body, 0x604);
                    const int32_t point[3] = {sim::Field<int32_t>(body, 0x65C) << 4, sim::Field<int32_t>(body, 0x664) << 4, int32_t(0u - uint32_t(sim::Field<int32_t>(body, 0x660) << 4))};
                    sim::SetField<int32_t>(body, 0x604, course.CourseDistanceOfWorldPoint(sim::Field<int32_t>(body, 0x600), point));
                },
                [&](Guest& g) { return g.Call(0x8003C70Cu, BodyAddress(car), uint32_t(previous), car); },
                [&](NativeShell& s) { return sim::LapCheck(s.ctx, int(car), previous); }, true);
    }
    // ---- 0x8003CE3C: one car's progress (body, car): course distance, lap check, requests, section machine
    {
        uint32_t car = 0;
        row.Run("Progress", 0x8003CE3Cu, 3000, [&](uint8_t* ram, size_t variant) { prepareAll(ram, false); car = carOf(variant); },
                [&](Guest& g) { return g.Call(0x8003CE3Cu, BodyAddress(car), car); },
                [&](NativeShell& s) { sim::ProgressCar(s.ctx, int(car), *const_cast<sim::NativeCourse*>(&course)); return 0u; });
    }
    // ---- 0x8003CF94: all cars in race order + the gap displays (cars, count)
    {
        row.Run("Frame", 0x8003CF94u, 600,
                [&](uint8_t* ram, size_t) {
                    prepareAll(ram, false);
                    int8_t order[kMaxCars];
                    for (uint32_t k = 0; k < kCarCount; k++) order[k] = int8_t(k);
                    for (uint32_t k = kCarCount - 1; k > 0; k--) std::swap(order[k], order[rng.Next() % (k + 1)]);
                    for (uint32_t k = 0; k < kCarCount; k++) Put<int8_t>(ram, D(0x801C8578u) + k, order[k]);
                },
                [&](Guest& g) { return g.Call(0x8003CF94u, kCarBase, kCarCount); },
                [&](NativeShell& s) {
                    sim::ProgressCars(s.ctx, reinterpret_cast<const int8_t*>(At(s.ram, D(0x801C8578u))), int(kCarCount), *const_cast<sim::NativeCourse*>(&course));
                    return 0u;
                });
    }
    // ---- 0x8003D168: race clock
    row.Run("Clock", 0x8003D168u, 300, [&](uint8_t* ram, size_t) { prepareAll(ram, true); }, [&](Guest& g) { return g.Call(0x8003D168u); },
            [&](NativeShell& s) { sim::AdvanceClock(s.ctx); return 0u; });
    // ---- 0x8003C3F4: race-state init
    row.Run("InitState", 0x8003C3F4u, 300, [&](uint8_t* ram, size_t) { prepareAll(ram, true); }, [&](Guest& g) { return g.Call(0x8003C3F4u); },
            [&](NativeShell& s) { sim::InitRaceState(s.ctx); return 0u; });
    // ---- 0x8002E550: end of frame: display timers, start-signal timer, race-end timer (task, pads)
    {
        uint32_t pads[2] = {0, 0};
        row.Run("EndFrame", 0x8002E550u, 1500,
                [&](uint8_t* ram, size_t) {
                    prepareAll(ram, true);
                    Put<int16_t>(ram, D(kStartTimer), int16_t(rng.Chance(2) ? rng.Pick({0, 240, 241, 300, 301, 360, 361, 420, 421, 1, 2}) : rng.Range(-5, 800)));
                    Put<int16_t>(ram, D(kEndTimer), int16_t(rng.Chance(3) ? -1 : rng.Chance(2) ? rng.Pick({0x1D, 0x78, 0x92, 0x9E, 0xE5, 0x13B, 0x13C, 0x1B9, 0x237, 0xC6}) : rng.Range(0, 600)));
                    Put<int16_t>(ram, D(kEndTimerAux), int16_t(rng.Chance(3) ? -1 : rng.Range(0, 260)));
                    Put<uint8_t>(ram, D(kClockFrames), uint8_t(rng.Next()));
                    Put<uint8_t>(ram, D(kCarCountShell), uint8_t(rng.Chance(4) ? rng.Range(1, 6) : 6));
                    for (uint32_t& pad : pads) pad = rng.Chance(2) ? 0 : uint32_t(rng.Next()) & 0xFFFF;
                    Put<uint32_t>(ram, D(kPads), pads[0]);
                    Put<uint32_t>(ram, D(kPads) + 4, pads[1]);
                },
                [&](Guest& g) { return g.Call(0x8002E550u, Get<uint32_t>(g.Ram(), D(kRaceTaskPointer)), D(kPads)); },
                [&](NativeShell& s) { return sim::EndFrame(s.ctx, pads) ? 1u : 0u; }, true);
    }

    // ================================================================ licence tests (mode 3)
    // A quarter of the variants keep the dump's state (only the arguments are drawn); the rest randomise the shell,
    // the licence bytes and the cars as above.
    auto prepareLicense = [&](uint8_t* ram, size_t variant) {
        if (variant % 4 == 0) return;
        prepareAll(ram, true);
        if (!rng.Chance(3)) Put<uint8_t>(ram, D(kGameMode), 3);
    };
    // The outcome bytes of a body (0x751 state, 0x756 code, 0x75C time) and what the check reads.
    auto licenseBody = [&](uint8_t* ram, uint32_t car) {
        uint8_t* body = BodyAt(ram, car);
        sim::SetField<uint8_t>(body, 0x751, uint8_t(rng.Chance(2) ? 0 : rng.Range(0, 3)));
        sim::SetField<uint8_t>(body, 0x756, uint8_t(rng.Range(0, 6)));
        sim::SetField<int32_t>(body, 0x75C, rng.Chance(2) ? 359999999 : int32_t(rng.Next()));
        sim::SetField<uint8_t>(body, 0x6FA, uint8_t(rng.Chance(2) ? 0 : rng.Range(0, 3)));
        for (uint32_t w = 0; w < 4; w++) sim::SetField<uint8_t>(body + 0x460 + w * 0x68, 0x14, uint8_t(rng.Chance(2) ? 0 : rng.Range(0, 4)));
        for (uint32_t i = 0; i < 3; i++)
            sim::SetField<int32_t>(body, 0x628 + i * 4, rng.Chance(2) ? rng.Range(-600, 600) : rng.Chance(2) ? rng.Range(-3000, 3000) : rng.Range(-0x40000, 0x40000));
    };
    // ---- 0x8003D5F8: the per-frame licence check (body): wall / course-out, the stop box
    {
        uint32_t car = 0;
        uint32_t outcomes[7] = {};
        row.Run("LicCheck", 0x8003D5F8u, 1500,
                [&](uint8_t* ram, size_t variant) {
                    car = carOf(variant);
                    if (variant % 4 == 0) return;
                    prepareLicense(ram, variant);
                    licenseBody(ram, car);
                    if (rng.Chance(3)) { // a stop test the car may pass: stopped inside the box, on tarmac, no reset
                        Put<uint8_t>(ram, D(kLicenseBlock) + 2, 2);
                        const int32_t boxStart = rng.Range(1, std::min(255, sc.courseLength / 0xA0000) - 5);
                        const int32_t boxLength = rng.Range(12, 40);
                        Put<uint8_t>(ram, D(kLicenseBlock) + 0x24, uint8_t(boxStart));
                        Put<uint8_t>(ram, D(kLicenseBlock) + 0x25, uint8_t(boxLength));
                        sc.PlaceNear(ram, car, boxStart * 0xA0000 + boxLength * 0x8000 + rng.Range(-0x40000, 0x40000), 0);
                        licenseBody(ram, car);
                        uint8_t* body = BodyAt(ram, car);
                        if (!rng.Chance(4)) {
                            sim::SetField<uint8_t>(body, 0x751, 0);
                            sim::SetField<uint8_t>(body, 0x6FA, 0);
                        }
                        for (uint32_t w = 0; w < 4; w++)
                            if (!rng.Chance(8)) sim::SetField<uint8_t>(body + 0x460 + w * 0x68, 0x14, uint8_t(rng.Range(0, 1)));
                        for (uint32_t i = 0; i < 3; i++) sim::SetField<int32_t>(body, 0x628 + i * 4, rng.Range(-500, 500));
                    } else if (!rng.Chance(4)) { // around the box start / end
                        const int32_t start = int32_t(Get<uint8_t>(ram, D(kLicenseBlock) + 0x24)) * 0xA0000;
                        const int32_t end = start + int32_t(Get<uint8_t>(ram, D(kLicenseBlock) + 0x25)) * 0x10000;
                        const int32_t target = (rng.Chance(2) ? start : end) + rng.Range(-0x60000, 0x60000);
                        sc.PlaceNear(ram, car, target < 0 ? target + sc.courseLength : target, 0);
                        licenseBody(ram, car); // PlaceNear redraws the progress fields
                    }
                },
                [&](Guest& g) {
                    const uint8_t before = Get<uint8_t>(g.Ram(), BodyAddress(car) + 0x751);
                    const uint32_t result = g.Call(0x8003D5F8u, BodyAddress(car));
                    if (before == 0 && Get<uint8_t>(g.Ram(), BodyAddress(car) + 0x751) != 0) outcomes[std::min<uint32_t>(Get<uint8_t>(g.Ram(), BodyAddress(car) + 0x756), 6)]++;
                    else outcomes[0]++;
                    return result;
                },
                [&](NativeShell& s) { sim::LicenseCheck(s.ctx, s.ctx.cars[car].body, *const_cast<sim::NativeCourse*>(&course)); return 0u; });
        std::printf("           (licence check outcomes: running %u, passed %u, overshot %u, off course %u, wall %u)\n", outcomes[0], outcomes[1], outcomes[3],
                    outcomes[4], outcomes[5]);
    }
    // ---- 0x8003D498: the wheels' course-distance extent (body, &min, &max): the original's minimum is the return
    // value compared; the maximum is checked by the native side (a difference turns into a return mismatch).
    {
        uint32_t car = 0;
        int32_t guestMax = 0;
        const uint32_t outMin = kStack + 0x10, outMax = kStack + 0x14;
        row.Run("WheelExt", 0x8003D498u, 900,
                [&](uint8_t* ram, size_t variant) {
                    car = carOf(variant);
                    if (variant % 4 == 0) return;
                    prepareAll(ram, true);
                    if (rng.Chance(3)) sc.PlaceNear(ram, car, rng.Pick({0, 0x10000, sc.courseLength - 0x20000, sc.courseLength / 8 * 7}) + rng.Range(0, 0x30000), 0);
                },
                [&](Guest& g) {
                    g.Call(0x8003D498u, BodyAddress(car), outMin, outMax);
                    guestMax = Get<int32_t>(g.Ram(), outMax);
                    return Get<uint32_t>(g.Ram(), outMin);
                },
                [&](NativeShell& s) {
                    int32_t low = 0, high = 0;
                    sim::WheelCourseExtent(s.ctx.cars[car].body, s.course.courseLength, *const_cast<sim::NativeCourse*>(&course), low, high);
                    return uint32_t(low) ^ (high == guestMax ? 0u : 0x80000000u);
                }, true);
    }
    // ---- 0x8003D458: wheels on a loose surface (body)
    {
        uint32_t car = 0;
        row.Run("LooseWhl", 0x8003D458u, 300,
                [&](uint8_t* ram, size_t variant) { car = carOf(variant); if (variant % 4 != 0) { prepareAll(ram, true); licenseBody(ram, car); } },
                [&](Guest& g) { return g.Call(0x8003D458u, BodyAddress(car)); },
                [&](NativeShell& s) { return uint32_t(sim::LooseSurfaceWheels(s.ctx.cars[car].body)); }, true);
    }
    // ---- 0x800156EC: the licence result (carIndex, type, code, time, maxSpeed)
    {
        uint32_t car = 0, type = 0, code = 0, time = 0, maxSpeed = 0;
        row.Run("LicResult", 0x800156ECu, 900,
                [&](uint8_t* ram, size_t variant) {
                    prepareLicense(ram, variant);
                    car = carOf(variant);
                    type = uint32_t(rng.Range(0, 6));
                    code = uint32_t(rng.Pick({1, 1, 3, 4, 5, 0, 2}));
                    time = uint32_t(rng.Chance(8) ? rng.Next() : rng.Range(10000, 200000));
                    maxSpeed = uint32_t(rng.Range(0, 0xFFFF));
                    Put<uint32_t>(ram, kStack + 0x10, maxSpeed | (rng.Next() << 16)); // the original reads the halfword
                },
                [&](Guest& g) { return g.Call(0x800156ECu, car, type, code, time); },
                [&](NativeShell& s) { sim::LicenseResult(s.ctx, car, int32_t(code), time, uint16_t(maxSpeed)); return 0u; });
    }
    // ---- 0x8003D244 / 0x8003D2A0: licence failed / passed (body)
    for (const bool passed : {false, true}) {
        uint32_t car = 0;
        const uint32_t address = passed ? 0x8003D2A0u : 0x8003D244u;
        row.Run(passed ? "LicPass" : "LicFail", address, 600,
                [&](uint8_t* ram, size_t variant) { car = carOf(variant); if (variant % 4 != 0) { prepareLicense(ram, variant); licenseBody(ram, car); } },
                [&](Guest& g) { return g.Call(address, BodyAddress(car)); },
                [&](NativeShell& s) {
                    if (passed) sim::LicensePass(s.ctx, s.ctx.cars[car].body);
                    else sim::LicenseFail(s.ctx, s.ctx.cars[car].body);
                    return 0u;
                });
    }
    // ---- 0x8003D314: mode 3 sector line (body, car, lap, sector, split)
    // The split index (lap - 1) * (lines + 1) + sector stays within the 4 words the results record has for it (see
    // Scenario::Progress); the goal test compares the sector with the course's last line.
    {
        uint32_t car = 0;
        int32_t lap = 0, sector = 0, split = 0;
        row.Run("LicSector", 0x8003D314u, 900,
                [&](uint8_t* ram, size_t variant) {
                    prepareLicense(ram, variant);
                    car = carOf(variant);
                    const int32_t lineCount = Get<int32_t>(ram, D(kStartLineCount));
                    lap = rng.Range(1, 4 / (lineCount + 1));
                    sector = rng.Chance(2) ? lineCount - 1 : rng.Range(0, std::max(0, lineCount - 1));
                    if (sector < 0) sector = 0;
                    split = rng.Chance(8) ? int32_t(rng.Next()) : rng.Range(1000, 200000);
                    Put<int32_t>(ram, kStack + 0x10, split);
                },
                [&](Guest& g) { return g.Call(0x8003D314u, BodyAddress(car), car, uint32_t(lap), uint32_t(sector)); },
                [&](NativeShell& s) { sim::LicenseSectorLine(s.ctx, s.ctx.cars[car].body, int(car), lap, sector, split); return 0u; });
    }
    // ---- 0x8003D3C0: mode 3 lap line (body, car, lap, elapsed, lapTime, maxSpeed)
    {
        uint32_t car = 0, elapsed = 0;
        int32_t lap = 0;
        row.Run("LicLap", 0x8003D3C0u, 900,
                [&](uint8_t* ram, size_t variant) {
                    prepareLicense(ram, variant);
                    car = carOf(variant);
                    const int32_t lineCount = Get<int32_t>(ram, D(kStartLineCount));
                    lap = rng.Chance(2) ? int32_t(Get<uint8_t>(ram, D(kLicenseBlock) + 1)) : rng.Range(1, 4 / (lineCount + 1));
                    if (lap < 1) lap = 1; // the lap line is timed from lap 1 on
                    if (lap * (lineCount + 1) - 1 > 3 && Get<uint8_t>(ram, D(kLicenseBlock) + 2) != 5) lap = 1; // a split outside the record (see LicSector)
                    if (lap * (lineCount + 1) - 1 > 3 && Get<uint8_t>(ram, D(kLicenseBlock) + 2) == 5) Put<uint8_t>(ram, D(kLicenseBlock) + 1, uint8_t(lap)); // the goal lap
                    elapsed = uint32_t(rng.Chance(8) ? rng.Next() : rng.Range(10000, 400000));
                    Put<uint32_t>(ram, kStack + 0x10, rng.Next()); // lapTime: unused by the routine
                    Put<uint32_t>(ram, kStack + 0x14, rng.Next() & 0xFFFF);
                },
                [&](Guest& g) { return g.Call(0x8003D3C0u, BodyAddress(car), car, uint32_t(lap), elapsed); },
                [&](NativeShell& s) { sim::LicenseLapLine(s.ctx, s.ctx.cars[car].body, int(car), lap, elapsed); return 0u; });
    }
    // ---- 0x8003D7B8: a medal time of a licence record (record, medal); the settings block holds a copy of the
    // record's time pairs at + 0x26.., so it serves as the record here
    {
        uint32_t medal = 0;
        row.Run("LicTarget", 0x8003D7B8u, 600,
                [&](uint8_t* ram, size_t variant) {
                    medal = uint32_t(rng.Range(0, 4));
                    if (variant % 4 != 0)
                        for (uint32_t i = 0x26; i < 0x30; i++) Put<uint8_t>(ram, D(kLicenseBlock) + i, uint8_t(rng.Chance(2) ? rng.Range(0, 99) : rng.Next()));
                },
                [&](Guest& g) { return g.Call(0x8003D7B8u, D(kLicenseBlock), medal); },
                [&](NativeShell& s) { return sim::LicenseTargetTime(At(s.ram, D(kLicenseBlock)), medal); }, true);
    }
    // ---- 0x8003D1E4: the lap number the HUD shows (body)
    {
        uint32_t car = 0;
        row.Run("DispLap", 0x8003D1E4u, 300,
                [&](uint8_t* ram, size_t variant) {
                    car = carOf(variant);
                    if (variant % 4 == 0) return;
                    prepareLicense(ram, variant);
                    sim::SetField<int16_t>(BodyAt(ram, car), 0x608, int16_t(rng.Chance(4) ? rng.Range(-3, 0) : rng.Chance(8) ? int32_t(rng.Next()) : rng.Range(1, 5)));
                },
                [&](Guest& g) { return g.Call(0x8003D1E4u, BodyAddress(car)); },
                [&](NativeShell& s) { return uint32_t(sim::DisplayLap(s.ctx, s.ctx.cars[car].body)); }, true);
    }

    // ================================================================ 0x800140A4: how a car is drawn (the ghost rule, race_shell.h CarDrawRule)
    // The original runs until it calls the EXE's car renderer 0x80067444 (a1 = the record: +0 LOD, +1 group) or returns (not
    // drawn); the port decides from the same inputs. Car and view fields randomised: the pad slot (1 = the ghost), + 0x0F, the
    // render position against the camera, the wheels' ground classes, the view's followed car / driver view, the game mode, the
    // start hold, the attract flag, the display toggle and the career's ghost option; the mirror's pass (a2) too.
    {
        constexpr uint32_t kView = 0x801FC000u, kDrawList = 0x800ADA08u, kToggle = 0x800AF232u, kGhostOption = 0x801C9995u;
        size_t cases = 0, mismatches = 0;
        for (size_t variant = 0; variant < 3000; variant++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            uint8_t* ram = guest.Ram();
            const uint32_t car = uint32_t(rng.Range(0, int32_t(std::max<uint32_t>(kCarCount, 1)) - 1));
            uint8_t* rec = RecordAt(ram, car);
            uint8_t* view = At(ram, kView);
            std::memset(view, 0, 0x120);
            Put<uint32_t>(ram, D(kDrawList), 0);
            const bool mirror = rng.Chance(5);
            sim::CarDrawInputs in;
            in.mirror = mirror;
            sim::SetField<uint16_t>(rec, 0x0C, uint16_t(rng.Range(0, 5)));
            view[0x10C] = uint8_t(rng.Chance(2) ? sim::Field<uint16_t>(rec, 0x0C) : rng.Range(0, 5));
            view[0x108] = uint8_t(rng.Chance(3) ? rng.Range(1, 255) : 0);
            in.viewCar = sim::Field<uint16_t>(rec, 0x0C) == view[0x10C];
            in.viewCarHidden = view[0x108] != 0;
            in.noLap = rec[0x0F] = uint8_t(rng.Chance(6) ? rng.Range(1, 255) : 0);
            in.padSlot = int16_t(rng.Chance(2) ? 1 : rng.Pick({0, 2, 3, -1}));
            sim::SetField<int16_t>(rec, 0x18, in.padSlot);
            const int32_t span = rng.Pick({0x10000, 0x40000, 0x80000, 0x800000, 0x7FFFFFFF});
            int32_t d[3];
            for (uint32_t k = 0; k < 3; k++) {
                const int32_t camera = rng.Range(-0x4000000, 0x4000000);
                d[k] = rng.Range(-span / 2, span / 2);
                sim::SetField<int32_t>(view, 0xB8 + k * 4, camera);
                sim::SetField<int32_t>(rec, 0x830 + k * 4, int32_t(uint32_t(camera) + uint32_t(d[k])));
            }
            in.dx = d[0], in.dy = d[1], in.dz = d[2];
            for (uint32_t w = 0; w < 4; w++) in.wheelGround[w] = rec[0x4A1 + w * 0x68] = uint8_t(rng.Chance(8) ? rng.Range(3, 255) : rng.Range(0, 2));
            in.gameMode = uint8_t(rng.Chance(2) ? 6 : rng.Pick({0, 2, 3, 4}));
            Put<uint8_t>(ram, D(kGameMode), in.gameMode);
            in.hold = uint16_t(rng.Chance(2) ? 0 : rng.Range(1, 600));
            Put<uint16_t>(ram, D(kHold), in.hold);
            in.demo = uint8_t(rng.Chance(4) ? 1 : 0);
            Put<uint8_t>(ram, D(kDemoFlag), in.demo);
            in.ghostToggle = uint8_t(rng.Chance(4) ? 0 : rng.Pick({1, 1, 2}));
            Put<uint8_t>(ram, D(kToggle), in.ghostToggle);
            in.ghostOption = uint8_t(rng.Chance(8) ? rng.Range(4, 255) : rng.Range(0, 3));
            Put<uint8_t>(ram, D(kGhostOption), in.ghostOption);
            const sim::CarDrawMode ours = sim::CarDrawRule(in);
            bool drawn = false;
            uint8_t lod = 0, group = 0;
            int32_t distance = 0;
            try {
                drawn = guest.CallUntil(0x800140A4u, 0x80067444u, kCarBase + car * kCarStride, kView, mirror ? 1u : 0u);
                if (drawn) {
                    lod = Get<uint8_t>(guest.Ram(), guest.Reg(5));
                    group = Get<uint8_t>(guest.Ram(), guest.Reg(5) + 1);
                    distance = sim::Field<int32_t>(RecordAt(guest.Ram(), car), 0x804);
                }
            } catch (const std::exception& e) {
                if (mismatches < 3) std::printf("    EXCEPTION variant %zu: %s\n", variant, e.what());
                mismatches++;
                cases++;
                continue;
            }
            cases++;
            if (drawn != ours.drawn || (drawn && (lod != ours.lod || group != ours.group || distance != ours.distance))) {
                if (mismatches++ < 3)
                    std::printf("    MISMATCH variant %zu: original drawn %d lod %u group %u distance 0x%X, ours %d %u %u 0x%X (mirror %d slot %d mode %u opt %u)\n",
                                variant, int(drawn), lod, group, uint32_t(distance), int(ours.drawn), ours.lod, ours.group, uint32_t(ours.distance), int(mirror),
                                in.padSlot, in.gameMode, in.ghostOption);
            }
        }
        Report("CarDraw", 0x800140A4u, cases, mismatches, failures);
    }

    // ================================================================ game mode 6: the ghost (Time Trial / Rally dumps only)
    if (Get<uint8_t>(pristine.data(), D(kGameMode)) != 6 || kCarCount != 2) return failures;
    constexpr uint32_t kGhostBlock = 0x800A8D70u, kReference = 0x801DA4A0u, kRing = 0x801D5F84u, kRingStream0 = 0x801D6068u;
    // A quarter of the variants keep the dump; the rest draw the ghost's state bytes (not the streams / snapshot bodies).
    auto prepareGhost = [&](uint8_t* ram, size_t variant) {
        if (variant % 4 == 0) return;
        Put<uint32_t>(ram, D(kClock), uint32_t(rng.Range(0, 3000000)));
        Put<uint8_t>(ram, D(kFrameStep), uint8_t(rng.Chance(4) ? 1 : 2));
        uint8_t* block = At(ram, D(kGhostBlock));
        for (const uint32_t snap : {0u, 0x342u}) {
            block[snap] = uint8_t(rng.Pick({0, 1, 2, 3}));
            block[snap + 1] = uint8_t(rng.Pick({0, 1, 2}));
            sim::SetField<uint16_t>(block, snap + 2, uint16_t(rng.Chance(2) ? rng.Range(0, 2) : rng.Range(0, 3000)));
        }
        uint8_t* playback = block + sim::kGhostPlayback;
        sim::SetField<int16_t>(playback, 0xB8, int16_t(rng.Range(0, 1)));
        sim::SetField<int16_t>(playback, 0xBA, int16_t(rng.Pick({0, 0, 0, 1, 2, 3})));
        sim::SetField<int16_t>(playback, 0xBC, int16_t(rng.Range(0, 9)));
        sim::SetField<int16_t>(playback, 0xBE, int16_t(rng.Range(0, 0x1000)));
        sim::SetField<int32_t>(playback, 0xC0, rng.Range(-2, 2));
        const int16_t index = int16_t(rng.Range(0, 3));
        sim::SetField<int16_t>(At(ram, D(kRing)), 0, int16_t(rng.Range(0, 4)));
        sim::SetField<int16_t>(At(ram, D(kRing)), 2, index);
        Put<uint32_t>(ram, kCarBase + 0x1C, D(kRingStream0) + uint32_t(index) * sim::kGhostLapSize); // player 1 records into the current lap
        uint8_t* reference = At(ram, D(kReference));
        sim::SetField<int16_t>(reference, 0, int16_t(rng.Range(0, 0x1000)));
        reference[2] = uint8_t(rng.Range(0, 100));
        reference[3] = uint8_t(rng.Range(0, 2));
        sim::SetField<int32_t>(reference, 4, rng.Chance(4) ? sim::kNoTime : rng.Range(40000, 200000));
        Put<uint8_t>(ram, kCarBase + kCarStride + 0x0E, uint8_t(rng.Range(0, 1)));
        Put<uint8_t>(ram, D(0x801D5A14u), uint8_t(rng.Range(0, 1)));
        Put<uint8_t>(ram, D(0x8002F4B0u), uint8_t(rng.Range(0, 1)));
        for (uint32_t car = 0; car < 2; car++) {
            uint8_t* body = BodyAt(ram, car);
            sim::SetField<int16_t>(body, 0x608, int16_t(rng.Range(0, 5)));
            sim::SetField<int32_t>(body, 0x780, rng.Range(0, 300000));
            sim::SetField<uint8_t>(body, 0x45C, uint8_t(car == 0 && rng.Chance(8) ? 1 : car));
            sim::SetField<uint8_t>(body, 0x45E, uint8_t(car == 1 ? (rng.Chance(8) ? 0 : 2) : (rng.Chance(8) ? 2 : 0)));
        }
    };
    // ---- 0x800350FC / 0x8003519C: the car state of a lap start (dst, body) / the body from it (body, src)
    for (const uint32_t car : {0u, 1u}) {
        row.Run(car == 0 ? "CarSave" : "CarSave1", 0x800350FCu, 200, [&](uint8_t* ram, size_t variant) { prepareGhost(ram, variant); },
                [&](Guest& g) { return g.Call(0x800350FCu, D(kReference) + 8, BodyAddress(car)); },
                [&](NativeShell& s) { sim::SaveCarState(s.ghost.reference.data() + 8, s.ctx.cars[car].body); return 0u; });
    }
    {
        uint32_t car = 0, source = 0;
        row.Run("CarRestore", 0x8003519Cu, 400,
                [&](uint8_t* ram, size_t variant) {
                    prepareGhost(ram, variant);
                    car = uint32_t(variant % 2);
                    source = D(variant % 3 == 0 ? kReference + 8 : kRingStream0 - sim::kGhostLapStream + 8 + uint32_t(rng.Range(0, 3)) * sim::kGhostLapSize);
                },
                [&](Guest& g) { return g.Call(0x8003519Cu, BodyAddress(car), source); },
                [&](NativeShell& s) { sim::RestoreCarState(s.ctx.cars[car].body, At(s.ram, source), s.course.grid.count != 0); return 0u; });
    }
    // ---- 0x80012410: the race load's ghost init
    row.Run("GhostInit", 0x80012410u, 200,
            [&](uint8_t* ram, size_t variant) { prepareGhost(ram, variant); Put<uint32_t>(ram, D(0x8002F4B4u), uint32_t(rng.Range(0, 1))); },
            [&](Guest& g) { return g.Call(0x80012410u); },
            [&](NativeShell& s) { sim::GhostRaceLoad(s.ghost, false); return 0u; });
    // ---- 0x8003FAEC: the ghost's hold
    row.Run("GhostHold", 0x8003FAECu, 400, [&](uint8_t* ram, size_t variant) { prepareGhost(ram, variant); }, [&](Guest& g) { return g.Call(0x8003FAECu); },
            [&](NativeShell& s) { return uint32_t(sim::GhostHold(s.ctx)); }, true);
    // ---- 0x8001286C: player 1's lap line (body, clock at the line, fraction, frame rest, invalid, arm)
    {
        uint32_t clockAtLine = 0, fraction = 0, rest = 0, invalid = 0, arm = 0;
        row.Run("GhostLap", 0x8001286Cu, 1200,
                [&](uint8_t* ram, size_t variant) {
                    prepareGhost(ram, variant);
                    clockAtLine = rng.Chance(5) ? sim::kGhostFirstLine : Get<uint32_t>(ram, D(kClock)) + uint32_t(rng.Range(0, 100));
                    fraction = uint32_t(rng.Range(0, 0x1000));
                    rest = uint32_t(rng.Range(0, 100));
                    invalid = uint32_t(rng.Chance(4) ? rng.Range(1, 2) : 0);
                    arm = uint32_t(rng.Chance(5) ? 0 : 1);
                    Put<uint32_t>(ram, kStack + 0x10, invalid);
                    Put<uint32_t>(ram, kStack + 0x14, arm);
                },
                [&](Guest& g) { return g.Call(0x8001286Cu, BodyAddress(0), clockAtLine, fraction, rest); },
                [&](NativeShell& s) {
                    sim::GhostLapLine(s.ctx, s.ctx.cars[0].body, clockAtLine, int32_t(fraction), int32_t(rest), uint8_t(invalid), arm != 0);
                    return 0u;
                });
    }
    // ---- 0x8003F724: the ghost's restart on the reference (body, lap, fraction)
    {
        uint32_t lap = 0, fraction = 0;
        row.Run("GhostStart", 0x8003F724u, 600,
                [&](uint8_t* ram, size_t variant) { prepareGhost(ram, variant); lap = uint32_t(rng.Range(0, 6)); fraction = uint32_t(rng.Range(0, 0x1000)); },
                [&](Guest& g) { return g.Call(0x8003F724u, BodyAddress(1), lap, fraction); },
                [&](NativeShell& s) { sim::GhostLapStart(s.ctx, s.ctx.cars[1].body, int16_t(lap), int16_t(fraction)); return 0u; });
    }
    // ---- 0x8003FB70: the snapshots / deferred start after a car's lap check (body)
    {
        uint32_t car = 0;
        row.Run("GhostCheck", 0x8003FB70u, 800, [&](uint8_t* ram, size_t variant) { prepareGhost(ram, variant); car = uint32_t(variant % 2); },
                [&](Guest& g) { return g.Call(0x8003FB70u, BodyAddress(car)); },
                [&](NativeShell& s) { sim::GhostCarCheck(s.ctx, s.ctx.cars[car].body); return 0u; });
    }
    // ---- 0x8003F2F0 / 0x8003F548: the ghost's display pose and back (car)
    row.Run("GhostBlend", 0x8003F2F0u, 600, [&](uint8_t* ram, size_t variant) { prepareGhost(ram, variant); }, [&](Guest& g) { return g.Call(0x8003F2F0u, 1); },
            [&](NativeShell& s) { sim::GhostBlendPose(s.ctx, 1); return 0u; });
    row.Run("GhostPose", 0x8003F548u, 300, [&](uint8_t* ram, size_t variant) { prepareGhost(ram, variant); }, [&](Guest& g) { return g.Call(0x8003F548u, 1); },
            [&](NativeShell& s) { sim::GhostRestorePose(s.ctx, 1); return 0u; });
    // ---- 0x8003C70C in game mode 6: the lap / sector lines of both cars with the ghost's clock and 0x8001286C / 0x8003FB70
    {
        uint32_t car = 0;
        int32_t previous = 0;
        row.Run("LapCheck6", 0x8003C70Cu, 2000,
                [&](uint8_t* ram, size_t variant) {
                    prepareAll(ram, true);
                    Put<uint8_t>(ram, D(kGameMode), 6);
                    Put<uint8_t>(ram, D(kDemoFlag), 0);
                    Put<uint8_t>(ram, D(kTwoPlayerLaps), uint8_t(rng.Chance(4) ? 0 : 1));
                    prepareGhost(ram, variant | 1);
                    car = uint32_t(variant % 2);
                    uint8_t* body = BodyAt(ram, car);
                    previous = sim::Field<int32_t>(body, 0x604);
                    const int32_t point[3] = {sim::Field<int32_t>(body, 0x65C) << 4, sim::Field<int32_t>(body, 0x664) << 4, int32_t(0u - uint32_t(sim::Field<int32_t>(body, 0x660) << 4))};
                    sim::SetField<int32_t>(body, 0x604, course.CourseDistanceOfWorldPoint(sim::Field<int32_t>(body, 0x600), point));
                },
                [&](Guest& g) { return g.Call(0x8003C70Cu, BodyAddress(car), uint32_t(previous), car); },
                [&](NativeShell& s) { return sim::LapCheck(s.ctx, int(car), previous); }, true);
    }
    return failures;
}

void LoadGhostSession(const uint8_t* ram, sim::GhostSession& g) {
    constexpr uint32_t kFlags = 0x8002F4B0u, kRingStream0 = 0x801D6068u;
    std::memcpy(g.ring.data(), ram + (D(0x801D5F84u) & 0x1FFFFF), g.ring.size());
    std::memcpy(g.reference.data(), ram + (D(0x801DA4A0u) & 0x1FFFFF), g.reference.size());
    std::memcpy(g.block.data(), ram + (D(0x800A8D70u) & 0x1FFFFF), g.block.size());
    g.newBest = Get<uint8_t>(ram, D(kFlags));
    g.savedBest = Get<uint8_t>(ram, D(kFlags) + 1);
    g.initialised = Get<uint32_t>(ram, D(kFlags) + 4);
    g.replayRestart = Get<uint32_t>(ram, D(kFlags) + 8);
    const uint32_t stream = Get<uint32_t>(ram, kCarBase + 0x1C), first = D(kRingStream0);
    g.playerStreamLap = 0;
    if (stream >= first && (stream - first) % sim::kGhostLapSize == 0 && (stream - first) / sim::kGhostLapSize < 4) g.playerStreamLap = int8_t((stream - first) / sim::kGhostLapSize);
    const uint32_t ghostStream = Get<uint32_t>(ram, kCarBase + kCarStride + 0x1C); // 0x801DA580 or a ring lap (0x800125BC)
    g.ghostStreamLap = -1;
    if (ghostStream >= first && (ghostStream - first) % sim::kGhostLapSize == 0 && (ghostStream - first) / sim::kGhostLapSize < 4)
        g.ghostStreamLap = int8_t((ghostStream - first) / sim::kGhostLapSize);
    g.ghostPresent = Get<uint8_t>(ram, kCarBase + kCarStride + 0x0E);
    g.ghostStreamOwned = Get<uint8_t>(ram, kCarBase + kCarStride + 0x0F);
    g.entryHasLap = Get<uint8_t>(ram, D(0x801D5A14u)) != 0;
}

void StoreGhostSession(uint8_t* ram, const sim::GhostSession& g, const sim::GhostSession& loaded) {
    constexpr uint32_t kFlags = 0x8002F4B0u, kRingStream0 = 0x801D6068u;
    std::memcpy(ram + (D(0x801D5F84u) & 0x1FFFFF), g.ring.data(), g.ring.size());
    std::memcpy(ram + (D(0x801DA4A0u) & 0x1FFFFF), g.reference.data(), g.reference.size());
    std::memcpy(ram + (D(0x800A8D70u) & 0x1FFFFF), g.block.data(), g.block.size());
    Put<uint8_t>(ram, D(kFlags), g.newBest);
    Put<uint8_t>(ram, D(kFlags) + 1, g.savedBest);
    Put<uint32_t>(ram, D(kFlags) + 4, g.initialised);
    Put<uint32_t>(ram, D(kFlags) + 8, g.replayRestart);
    if (g.playerStreamLap != loaded.playerStreamLap) Put<uint32_t>(ram, kCarBase + 0x1C, D(kRingStream0) + uint32_t(g.playerStreamLap) * sim::kGhostLapSize);
    if (g.ghostStreamLap != loaded.ghostStreamLap)
        Put<uint32_t>(ram, kCarBase + kCarStride + 0x1C, g.ghostStreamLap < 0 ? D(0x801DA580u) : D(kRingStream0) + uint32_t(g.ghostStreamLap) * sim::kGhostLapSize);
    Put<uint8_t>(ram, kCarBase + kCarStride + 0x0E, g.ghostPresent);
    Put<uint8_t>(ram, kCarBase + kCarStride + 0x0F, g.ghostStreamOwned);
    if (g.entryHasLap != loaded.entryHasLap) Put<uint8_t>(ram, D(0x801D5A14u), g.entryHasLap ? 1 : 0);
}

} // namespace gt2::verify
