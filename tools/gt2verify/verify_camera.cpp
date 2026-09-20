// Differential checks of src/game/camera (the race cameras) against the original (see guest.h for the harness): the
// camera object's routines of the race overlay (0x80010000..0x80011A58) and the executable's matrix / angle library
// they call, on the dump's camera object (player 1, 0x801FF97C) with randomised cars, options, pad words, start hold,
// replay state and trackside camera progress; the whole guest RAM is compared after each call. With the disc: the
// overlay tables and the course's trackside camera list read natively (from GT2.OVL / the .tro) against the dump,
// and a scan of the camera lists of all courses.
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>

#include <cmath>

#include "game/camera/race_camera.h"
#include "game/sim/trig.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"
#include "machine/machine.h"
#include "../gt2run/ai_player.h"
#include "../gt2run/ai_player_arcade.h"
#include "../gt2run/input_script.h"
#include "scene/scene_extractor.h"

namespace gt2::verify {

namespace {

constexpr uint32_t kCamera = 0x801FF97Cu;          // view object 0x801FF8B8 + 0xC4: player 1's camera
constexpr uint32_t kCourseObject = 0x800A9500u;    // + 0xB544 chunk table (course length at + 0), + 0xB550 camera list
constexpr uint32_t kReplayFlag = 0x800A951Cu, kHoldInitial = 0x800A951Eu, kHold = 0x800A9520u;
constexpr uint32_t kGameMode = 0x801D5866u, kViewAngle = 0x801C9992u, kCareer = 0x801C98E0u;
constexpr uint32_t kCarCountByte = 0x800AF231u, kReplayProgress = 0x800A8D6Cu;
constexpr uint32_t kOutput = 0x801FFD00u;          // result buffer of the library rows (unused RAM above the view object)
constexpr uint32_t kInput = 0x801FFD40u;

template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
uint32_t CarAddress(uint32_t car) { return kCarBase + car * kCarStride; }

struct Rng {
    std::mt19937& g;
    uint32_t Next() { return g(); }
    bool Chance(uint32_t oneIn) { return Next() % oneIn == 0; }
    int32_t Range(int32_t low, int32_t high) { return low + int32_t(Next() % uint32_t(int64_t(high) - int64_t(low) + 1)); }
    int16_t Short() { return int16_t(Next()); }
};

// Everything the native camera reads, built from a RAM image like the original reads it.
struct GuestWorld {
    std::vector<camera::CameraCar> cars;
    camera::CameraConstants constants;
    camera::CameraWorld world;
    camera::CameraPad pad;
    GuestWorld(uint8_t* ram, const Track* track) {
        const uint32_t count = Get<uint8_t>(ram, D(kCarCountByte));
        for (uint32_t i = 0; i < count && i < kMaxCars; i++) {
            const uint32_t car = CarAddress(i);
            camera::CameraCar c;
            std::memcpy(&c.pose, At(ram, car + 0x81C), sizeof c.pose);
            c.chunk = Get<int32_t>(ram, car + 0x14);
            c.viewYawOffset = Get<int16_t>(ram, car + 0x766);
            c.viewPitchOffset = Get<int16_t>(ram, car + 0x768);
            c.courseDistance = Get<int32_t>(ram, car + 0x630);
            c.lap = Get<int16_t>(ram, car + 0x634);
            c.racePosition = Get<int8_t>(ram, car + 0x77C);
            c.halfWidth = Get<int32_t>(ram, car + 0x87C);
            const uint32_t lod = Get<uint32_t>(ram, Get<uint32_t>(ram, car + 0x878) + 0x870);
            c.lodScale = Get<uint16_t>(ram, lod + 0x4C);
            c.lodSize = Get<uint16_t>(ram, lod + 0x4E);
            c.ghostByte = At(ram, car + 0x21);
            cars.push_back(c);
        }
        for (uint32_t i = 0; i < 8; i++) constants.chase[i] = Get<int32_t>(ram, D(camera::kChaseTableAddress) + 4 * i);
        for (uint32_t i = 0; i < 3; i++) constants.viewAngle[i] = Get<int16_t>(ram, D(camera::kViewAngleTableAddress) + 2 * i);
        world.cars = cars;
        world.gameMode = Get<uint8_t>(ram, D(kGameMode));
        world.replay = Get<uint8_t>(ram, D(kReplayFlag));
        world.hold = Get<uint16_t>(ram, D(kHold));
        world.holdInitial = Get<uint16_t>(ram, D(kHoldInitial));
        world.viewAngle = Get<uint8_t>(ram, D(kViewAngle));
        world.track = track;
        world.courseLength = Get<int32_t>(ram, Get<uint32_t>(ram, D(kCourseObject) + 0xB544));
        world.replayCameras.bytes = std::span<const uint8_t>(ram, Bus::kRamSize);
        world.replayCameras.base = 0x80000000u;
        world.replayCameras.list = Get<uint32_t>(ram, D(kCourseObject) + 0xB550);
        world.constants = &constants;
        world.replayProgress = reinterpret_cast<int32_t*>(At(ram, D(kReplayProgress)));
        world.onboard = reinterpret_cast<camera::OnboardView*>(At(ram, D(camera::kOnboardTableAddress)));
        const uint32_t padObject = Get<uint32_t>(ram, D(kCamera) + 0x9C);
        pad.replayPressed = Get<uint32_t>(ram, padObject + 0x6C);
        pad.held = Get<uint32_t>(ram, padObject + 0x8C);
        pad.pressed = Get<uint32_t>(ram, padObject + 0x90);
    }
    GuestWorld(const GuestWorld&) = delete;
    GuestWorld& operator=(const GuestWorld&) = delete;
};

camera::RaceCamera& CameraAt(uint8_t* ram) { return *reinterpret_cast<camera::RaceCamera*>(At(ram, D(kCamera))); }

// Random proper rotation (through our RotationFromAngles; checked by its own row) or, rarely, random words.
void RandomRotation(Rng& r, int16_t m[3][3]) {
    if (r.Chance(12)) {
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) m[i][j] = r.Chance(3) ? int16_t(r.Range(-4096, 4096)) : r.Short();
        return;
    }
    camera::RotationFromAngles(m, r.Range(0, 0xFFF), r.Chance(2) ? r.Range(-300, 300) : r.Range(0, 0xFFF), r.Chance(2) ? r.Range(-200, 200) : r.Range(0, 0xFFF));
}

// The state the camera rows start from: the dump's camera and cars with randomised inputs.
struct Prepared {
    const Track* track = nullptr;
    bool replayOk = false;     // the course (and its camera list) is known: the replay path can run
    uint32_t variant = 0;
};

void RandomiseWorld(uint8_t* ram, Rng& r, const Prepared& p, bool allowReplay) {
    const uint32_t count = Get<uint8_t>(ram, D(kCarCountByte));
    const int32_t courseLength = Get<int32_t>(ram, Get<uint32_t>(ram, D(kCourseObject) + 0xB544));
    for (uint32_t i = 0; i < count && i < kMaxCars; i++) {
        const uint32_t car = CarAddress(i);
        if (!r.Chance(4)) { // the dump's pose otherwise
            int16_t m[3][3];
            RandomRotation(r, m);
            std::memcpy(At(ram, car + 0x81C), m, sizeof m);
            for (uint32_t k = 0; k < 3; k++) {
                const int32_t t = Get<int32_t>(ram, car + 0x830 + 4 * k);
                Put<int32_t>(ram, car + 0x830 + 4 * k, r.Chance(10) ? int32_t(r.Next()) : t + r.Range(-40 << 16, 40 << 16));
            }
            if (r.Chance(8)) Put<int16_t>(ram, car + 0x82E, r.Short());
        }
        Put<int16_t>(ram, car + 0x766, r.Chance(4) ? r.Short() : int16_t(r.Range(-400, 400)));
        Put<int16_t>(ram, car + 0x768, r.Chance(4) ? r.Short() : int16_t(r.Range(-200, 200)));
        Put<int32_t>(ram, car + 0x14, r.Chance(2) ? Get<int32_t>(ram, car + 0x14) : int32_t(r.Next() & 0xFF));
        if (courseLength > 0) Put<int32_t>(ram, car + 0x630, r.Chance(10) ? r.Range(-(8 << 16), 8 << 16) : int32_t(r.Next() % uint32_t(courseLength)));
        Put<int16_t>(ram, car + 0x634, int16_t(r.Chance(10) ? r.Range(-2, 12) : r.Range(0, 4)));
        Put<int8_t>(ram, car + 0x77C, int8_t(r.Chance(10) ? r.Range(0, 8) : r.Range(1, int32_t(count))));
        Put<int32_t>(ram, car + 0x87C, r.Chance(3) ? Get<int32_t>(ram, car + 0x87C) : r.Range(0x8000, 0x18000));
        Put<uint8_t>(ram, car + 0x21, uint8_t(r.Next()));
    }
    // Game modes; mode 0 (two players, split view) only with two cars in the race (the replay's "follow the leader"
    // reads car 0 / car 1).
    static constexpr uint8_t kModes[] = {0, 1, 2, 2, 3, 5, 6};
    uint8_t mode = kModes[r.Next() % sizeof kModes];
    if (mode == 0 && count < 2) mode = 2;
    Put<uint8_t>(ram, D(kGameMode), mode);
    Put<uint8_t>(ram, D(kReplayFlag), uint8_t(allowReplay && p.replayOk && r.Chance(2) ? 1 : 0));
    const uint16_t initial = uint16_t(r.Chance(3) ? r.Range(0, 700) : r.Chance(2) ? 540 : 120);
    Put<uint16_t>(ram, D(kHoldInitial), initial);
    Put<uint16_t>(ram, D(kHold), uint16_t(r.Chance(3) ? 0 : r.Chance(4) ? r.Range(0, 65535) : r.Range(0, initial)));
    Put<uint8_t>(ram, D(kViewAngle), uint8_t(r.Range(0, 2)));
    Put<uint8_t>(ram, D(kCareer) + 0xAE, uint8_t(r.Range(0, 3)));
    Put<uint8_t>(ram, D(kCareer) + 0xAF, uint8_t(r.Range(0, 2)));
    Put<int32_t>(ram, D(kReplayProgress), r.Chance(4) ? int32_t(r.Next()) : r.Range(0, 4096));
    const uint32_t padObject = Get<uint32_t>(ram, D(kCamera) + 0x9C);
    for (uint32_t off : {0x6Cu, 0x8Cu, 0x90u}) {
        uint32_t bits = 0;
        if (!r.Chance(3)) bits = r.Next() & (r.Chance(2) ? 0x1F13u : 0xFFFFFFFFu);
        Put<uint32_t>(ram, padObject + off, bits);
    }
    // the camera object (the library part stays the dump's)
    camera::RaceCamera& c = CameraAt(ram);
    c.position = uint8_t(r.Range(0, 2));
    c.lookBack = uint8_t(r.Chance(3) ? 1 : 0);
    c.split = uint8_t(r.Chance(5) ? r.Range(1, 255) : 0);
    c.target = uint8_t(r.Range(0, int32_t(count) - 1));
    c.replayMode = uint8_t(r.Chance(10) ? r.Range(3, 5) : r.Range(0, 2));
    c.onboardView = uint8_t(r.Range(0, 9));
    c.replayInfo = uint8_t(r.Range(0, 3));
    for (int k = 0; k < 3; k++) c.view.t[k] = Get<int32_t>(ram, CarAddress(c.target) + 0x830 + 4 * uint32_t(k)) + r.Range(-(30 << 16), 30 << 16);
    RandomRotation(r, c.view.m);
    for (uint8_t* b : {&c.hideTarget, &c.mirror, &c.inCarSound, &c.external}) *b = uint8_t(r.Range(0, 1));
}

} // namespace

int VerifyCamera(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& g, const Track* track, const GtfsVolume* vol, const DiscImage* disc,
                 const std::string& trackName) {
    int failures = 0;
    Rng r{g};
    const uint8_t* dump = pristine.data();
    if (Get<uint32_t>(dump, D(kCamera) + 0x9C) != D(0x800A9528u)) {
        std::printf("Camera     the dump's camera object is not at 0x%08X (pad pointer %08X) - camera rows skipped\n", D(kCamera), Get<uint32_t>(dump, D(kCamera) + 0x9C));
        return 0;
    }
    Prepared p;
    p.track = track;
    const int32_t courseLength = Get<int32_t>(dump, Get<uint32_t>(dump, D(kCourseObject) + 0xB544));
    p.replayOk = track && Get<uint32_t>(dump, D(kCourseObject) + 0xB550) != 0 && track->courseLength == courseLength;

    // ---- tables of the executable the library reads
    {
        size_t cases = 0, mismatches = 0;
        for (int32_t i = 0; i < 4096; i++, cases++)
            if (Get<int16_t>(dump, D(0x800A2AC4u) + 2 * uint32_t(i)) != camera::ArcSine(int16_t(i))) mismatches++;
        size_t zero = 0;
        for (uint32_t i = 0; i < 4096; i++) zero += Get<int16_t>(dump, D(0x800A0A88u) + 2 * i) == 0;
        std::printf("%-10s 0x800A2AC4  %zu entries, %zu mismatches (the cosine table 0x800A0A88 has %zu zero entries below 4096)  %s\n", "AsinTab", cases, mismatches,
                    zero, mismatches || zero ? "FAIL" : "ok");
        failures += mismatches || zero ? 1 : 0;
    }

    // ---- library routines (results in a buffer; the whole RAM is compared)
    { // 0x80081374(out, yaw, pitch, roll): per-case arguments, so the guest is called directly
        size_t cases = 0, mismatches = 0;
        for (int v = 0; v < 4000; v++) {
            const int32_t a = r.Chance(4) ? int32_t(r.Next()) : r.Range(0, 0xFFF), b = r.Chance(4) ? int32_t(r.Next()) : r.Range(0, 0xFFF),
                          c = r.Chance(4) ? int32_t(r.Next()) : r.Range(0, 0xFFF);
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            guest.Call(0x80081374u, kOutput, uint32_t(a), uint32_t(b), uint32_t(c));
            int16_t m[3][3];
            camera::RotationFromAngles(m, a, b, c);
            cases++;
            if (std::memcmp(At(guest.Ram(), kOutput), m, sizeof m) != 0 && mismatches++ < 3)
                std::printf("    MISMATCH angles (%d, %d, %d): original [%d %d %d; %d %d %d; %d %d %d] ours [%d %d %d; %d %d %d; %d %d %d]\n", a, b, c,
                            Get<int16_t>(guest.Ram(), kOutput), Get<int16_t>(guest.Ram(), kOutput + 2), Get<int16_t>(guest.Ram(), kOutput + 4),
                            Get<int16_t>(guest.Ram(), kOutput + 6), Get<int16_t>(guest.Ram(), kOutput + 8), Get<int16_t>(guest.Ram(), kOutput + 10),
                            Get<int16_t>(guest.Ram(), kOutput + 12), Get<int16_t>(guest.Ram(), kOutput + 14), Get<int16_t>(guest.Ram(), kOutput + 16), m[0][0], m[0][1],
                            m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
        }
        Report("RotAngles", 0x80081374u, cases, mismatches, failures);
    }
    {
        const StatefulResult res = VerifyStateful(
            guest, pristine, 0x800811B0u, {kOutput}, 4000,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t) {
                int16_t m[3][3];
                RandomRotation(r, m);
                if (r.Chance(8)) m[2][1] = int16_t(r.Chance(2) ? 4096 : -4096);
                std::memcpy(At(ram, kInput), m, sizeof m);
            },
            [&](uint8_t* ram, uint8_t*, uint32_t) {
                int16_t m[3][3], out[3];
                std::memcpy(m, At(ram, kInput), sizeof m);
                camera::AnglesFromRotation(out, m);
                std::memcpy(At(ram, kOutput), out, sizeof out);
            },
            kInput);
        Report("RotToAng", 0x800811B0u, res.cases, res.mismatches, failures);
    }
    {
        const StatefulResult res = VerifyStateful(
            guest, pristine, 0x80081164u, {kOutput}, 4000,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t) {
                for (uint32_t k = 0; k < 3; k++) {
                    const int bits = r.Range(0, 31);
                    int32_t v = bits == 31 ? int32_t(r.Next()) : int32_t(r.Next() & ((1u << bits) - 1)) * (r.Chance(2) ? -1 : 1);
                    if (r.Chance(10)) v = 0;
                    Put<int32_t>(ram, kInput + 4 * k, v);
                }
            },
            [&](uint8_t* ram, uint8_t*, uint32_t) {
                int32_t v[3];
                std::memcpy(v, At(ram, kInput), sizeof v);
                int16_t d[3];
                camera::VectorLength(d, v);
                std::memcpy(At(ram, kOutput), d, sizeof d);
            },
            kInput);
        Report("VecLength", 0x80081164u, res.cases, res.mismatches, failures);
    }
    {
        const StatefulResult res = VerifyStateful(
            guest, pristine, 0x80010E20u, {kOutput}, 4000,
            [&](uint8_t* ram, uint8_t*, uint32_t, size_t) {
                for (uint32_t k = 0; k < 3; k++) {
                    const int bits = r.Range(0, 31);
                    int32_t v = bits == 31 ? int32_t(r.Next()) : int32_t(r.Next() & ((1u << bits) - 1)) * (r.Chance(2) ? -1 : 1);
                    if (r.Chance(8)) v = 0;
                    Put<int32_t>(ram, kInput + 4 * k, v);
                }
                for (uint32_t k = 0; k < 0x20; k += 4) Put<uint32_t>(ram, kOutput + k, r.Next());
            },
            [&](uint8_t* ram, uint8_t*, uint32_t) {
                int32_t v[3];
                std::memcpy(v, At(ram, kInput), sizeof v);
                camera::LookAlong(*reinterpret_cast<camera::Matrix*>(At(ram, kOutput)), v);
            },
            kInput);
        Report("LookAlong", 0x80010E20u, res.cases, res.mismatches, failures);
    }

    // ---- the camera object's routines. The rows need per-case a1..a3 (VerifyStateful passes fixed ones), so they use
    // their own loop with the same comparison: all RAM outside the guest stack.
    std::vector<uint8_t> ours(Bus::kRamSize);
    auto row = [&](const char* name, uint32_t function, size_t cases, bool allowReplay, auto&& args, auto&& native) {
        size_t total = 0, mismatches = 0;
        const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
        for (size_t v = 0; v < cases; v++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            if (v % 4 != 0) RandomiseWorld(guest.Ram(), r, p, allowReplay); // a quarter of the cases: the dump's own state
            uint32_t a[3] = {0, 0, 0};
            args(guest.Ram(), a);
            std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
            std::string error;
            try {
                guest.Call(function, D(kCamera), a[0], a[1], a[2]);
            } catch (const std::exception& e) {
                error = std::string("original: ") + e.what();
            }
            if (error.empty()) try {
                    GuestWorld w(ours.data(), track);
                    native(ours.data(), w, a);
                } catch (const std::exception& e) {
                    error = std::string("ours: ") + e.what();
                }
            total++;
            bool equal = error.empty() && std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                         std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0;
            if (!equal && mismatches++ < 3) {
                if (!error.empty()) std::printf("    MISMATCH case %zu: %s\n", v, error.c_str());
                else
                    for (uint32_t i = 0; i < Bus::kRamSize; i++)
                        if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                            std::printf("    MISMATCH case %zu: first differing byte at 0x%08X (camera + 0x%X): original %02X ours %02X\n", v, 0x80000000u + i,
                                        i - (D(kCamera) & 0x1FFFFF), guest.Ram()[i], ours[i]);
                            break;
                        }
            }
        }
        Report(name, function, total, mismatches, failures);
    };
    auto cam = [](uint8_t* ram) -> camera::RaceCamera& { return CameraAt(ram); };

    row("CamInit", 0x80010000u, 600, true,
        [&](uint8_t*, uint32_t a[3]) { a[0] = uint32_t(r.Range(0, int32_t(kCarCount) - 1)); },
        [&](uint8_t* ram, GuestWorld& w, const uint32_t a[3]) {
            camera::InitCamera(cam(ram), uint8_t(a[0]), Get<uint8_t>(ram, D(kCareer) + 0xAF), Get<uint8_t>(ram, D(kCareer) + 0xAE), w.world);
        });
    row("CamProj", 0x80010088u, 2000, false,
        [&](uint8_t*, uint32_t a[3]) {
            a[0] = uint32_t(r.Chance(4) ? int32_t(r.Next()) : r.Range(1, 1200));
            if (a[0] == 0) a[0] = 1;
            a[1] = uint32_t(r.Chance(2) ? 0 : r.Range(-400, 400));
            a[2] = uint32_t(r.Chance(2) ? 0 : r.Range(-400, 400));
        },
        [&](uint8_t* ram, GuestWorld&, const uint32_t a[3]) { camera::SetProjection(cam(ram), int32_t(a[0]), int32_t(a[1]), int32_t(a[2])); });
    row("CamTarget", 0x800101FCu, 1000, false, [&](uint8_t*, uint32_t[3]) {},
        [&](uint8_t* ram, GuestWorld& w, const uint32_t[3]) { camera::LoadTarget(cam(ram), w.world); });
    row("CamRace", 0x800103C0u, 4000, false, [&](uint8_t*, uint32_t a[3]) { a[0] = uint32_t(r.Range(0, 2)); },
        [&](uint8_t* ram, GuestWorld& w, const uint32_t a[3]) { camera::RaceView(cam(ram), uint8_t(a[0]), w.world); });
    row("CamPlayer", 0x800102D8u, 4000, false, [&](uint8_t* ram, uint32_t[3]) { Put<uint8_t>(ram, D(kReplayFlag), 0); },
        [&](uint8_t* ram, GuestWorld& w, const uint32_t[3]) { camera::PlayerCamera(cam(ram), w.pad, w.world); });
    row("CamIntro", 0x80010608u, 4000, false,
        [&](uint8_t* ram, uint32_t[3]) {
            const uint16_t initial = uint16_t(r.Chance(4) ? r.Range(0, 1000) : r.Chance(2) ? 540 : 180);
            Put<uint16_t>(ram, D(kHoldInitial), initial);
            Put<uint16_t>(ram, D(kHold), uint16_t(r.Chance(8) ? r.Range(0, 65535) : r.Range(0, initial)));
        },
        [&](uint8_t* ram, GuestWorld& w, const uint32_t[3]) { camera::StartIntro(cam(ram), w.world); });
    row("CamOnbrd", 0x800113C0u, 3000, false, [&](uint8_t*, uint32_t a[3]) { a[0] = uint32_t(r.Range(0, 12)); },
        [&](uint8_t* ram, GuestWorld& w, const uint32_t a[3]) { camera::OnboardCamera(cam(ram), int32_t(a[0]), w.world); });
    row("CamOnSet", 0x80011704u, 2000, false, [&](uint8_t*, uint32_t[3]) {},
        [&](uint8_t* ram, GuestWorld& w, const uint32_t[3]) { camera::OnboardSet(cam(ram), w.world); });
    if (p.replayOk) {
        // 0x800117C4(list, lap, distance) and 0x80011890(path, out): the course's own camera list
        size_t cases = 0, mismatches = 0;
        const uint32_t list = Get<uint32_t>(dump, D(kCourseObject) + 0xB550);
        camera::ReplayCameraData data{std::span<const uint8_t>(dump, Bus::kRamSize), 0x80000000u, list};
        for (int v = 0; v < 3000; v++) {
            const int32_t lap = r.Chance(8) ? r.Range(-3, 20) : r.Range(0, 4);
            const int32_t distance = r.Chance(10) ? int32_t(r.Next()) : int32_t(r.Next() % uint32_t(courseLength));
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            const uint32_t original = guest.Call(0x800117C4u, list, uint32_t(lap), uint32_t(distance));
            const uint32_t native = camera::FindReplayCamera(data, lap, distance, courseLength);
            cases++;
            if (original != native && mismatches++ < 3) std::printf("    MISMATCH lap %d distance %d: original %08X ours %08X\n", lap, distance, original, native);
        }
        Report("CamFind", 0x800117C4u, cases, mismatches, failures);
        cases = mismatches = 0;
        std::map<int, size_t> kinds;
        for (uint32_t record : data.Records()) {
            const uint32_t type = data.U16(record) & 0xF;
            if (type != 0 && type != 3) continue;
            const uint32_t path = record + (type == 0 ? 0x18u : 0x1Cu);
            kinds[data.U16(path)]++;
            for (int v = 0; v < 40; v++) {
                const int32_t progress = r.Chance(6) ? int32_t(r.Next()) : r.Range(0, 4096);
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                Put<int32_t>(guest.Ram(), D(kReplayProgress), progress);
                for (uint32_t k = 0; k < 3; k++) Put<int32_t>(guest.Ram(), kOutput + 4 * k, int32_t(r.Next()));
                int32_t out[3];
                std::memcpy(out, At(guest.Ram(), kOutput), sizeof out);
                guest.Call(0x80011890u, path, kOutput);
                camera::PathPosition(data, path, progress, out);
                cases++;
                if (std::memcmp(out, At(guest.Ram(), kOutput), sizeof out) != 0 && mismatches++ < 3)
                    std::printf("    MISMATCH path %08X progress %d: original (%d %d %d) ours (%d %d %d)\n", path, progress, Get<int32_t>(guest.Ram(), kOutput),
                                Get<int32_t>(guest.Ram(), kOutput + 4), Get<int32_t>(guest.Ram(), kOutput + 8), out[0], out[1], out[2]);
            }
        }
        std::printf("           paths of the course: %zu polyline, %zu cubic\n", kinds[0], kinds[1]);
        Report("CamPath", 0x80011890u, cases, mismatches, failures);
        // +0x100 (only written, by the trackside path) marks which record type ran: coverage of the four types.
        std::map<int, size_t> ran;
        row("CamReplay", 0x800109FCu, 5000, true,
            [&](uint8_t* ram, uint32_t[3]) {
                Put<uint8_t>(ram, D(kReplayFlag), 1);
                CameraAt(ram).replayFlags = 0xFFFF;
            },
            [&](uint8_t* ram, GuestWorld& w, const uint32_t[3]) {
                camera::ReplayCamera(cam(ram), w.pad, w.world);
                ran[cam(ram).replayFlags == 0xFFFF ? -1 : cam(ram).replayFlags & 0xF]++;
            });
        std::printf("           cases by trackside record type:");
        for (const auto& [type, n] : ran) std::printf(" %s%d: %zu", type < 0 ? "(none: onboard / race view) " : "type ", type < 0 ? 0 : type, n);
        std::printf("\n");
    } else {
        std::printf("%-10s the trackside cameras need the course (disc + name) - CamFind / CamPath / CamReplay skipped\n", "CamReplay");
    }
    row("CamUpdate", 0x800100F4u, 6000, true, [&](uint8_t*, uint32_t[3]) {},
        [&](uint8_t* ram, GuestWorld& w, const uint32_t[3]) { camera::UpdateCamera(cam(ram), w.pad, w.world); });

    // ---- disc data: the overlay's tables and the course's camera list read natively
    if (disc) {
        const camera::CameraConstants c = camera::LoadCameraConstants(LoadOverlayImage(*disc, kRaceOverlayIndex));
        size_t mismatches = 0;
        for (uint32_t i = 0; i < 8; i++) mismatches += c.chase[i] != Get<int32_t>(dump, D(camera::kChaseTableAddress) + 4 * i);
        for (uint32_t i = 0; i < 3; i++) mismatches += c.viewAngle[i] != Get<int16_t>(dump, D(camera::kViewAngleTableAddress) + 2 * i);
        for (uint32_t i = 0; i < 13; i++) { // entries 8..11: x is written at every use
            camera::OnboardView e;
            std::memcpy(&e, dump + (D(camera::kOnboardTableAddress) + 24 * i - 0x80000000u), sizeof e);
            if (i >= 8 && i <= 11) e.offset[0] = c.onboard[i].offset[0];
            mismatches += std::memcmp(&e, &c.onboard[i], sizeof e) != 0;
        }
        std::printf("%-10s 0x8002F350  24 table entries from GT2.OVL member 0, %zu mismatches  %s\n", "CamConst", mismatches, mismatches ? "FAIL" : "ok");
        failures += mismatches ? 1 : 0;
    }
    if (vol && p.replayOk && !trackName.empty()) {
        const std::vector<uint8_t> tro = vol->Read("crsobj/" + trackName + ".tro");
        const camera::ReplayCameraData file = camera::ReplayCamerasOfTro(tro);
        const uint32_t list = Get<uint32_t>(dump, D(kCourseObject) + 0xB550);
        const camera::ReplayCameraData ram{std::span<const uint8_t>(dump, Bus::kRamSize), 0x80000000u, list};
        const uint32_t base = D(0x800B4A34u); // the .tro is inflated whole there (docs/formats/course_race_data.md)
        size_t cases = 0, mismatches = 0;
        const std::vector<uint32_t> a = file.Records(), b = ram.Records();
        mismatches += a.size() != b.size();
        for (size_t i = 0; i < a.size() && i < b.size(); i++) {
            cases++;
            const uint32_t size = i + 1 < a.size() ? a[i + 1] - a[i] : 0x28;
            bool same = b[i] == base + a[i];
            for (uint32_t k = 0; k < size && same; k += 2) same = file.U16(a[i] + k) == ram.U16(b[i] + k);
            mismatches += !same;
        }
        std::printf("%-10s .tro + 0x1C  %zu records of %s read from the disc against the dump, %zu mismatches  %s\n", "CamList", cases, trackName.c_str(), mismatches,
                    mismatches ? "FAIL" : "ok");
        failures += mismatches ? 1 : 0;
    }
    if (vol) { // every course's list: record types, path kinds, all reads inside the file over the whole course
        size_t courses = 0, records = 0, errors = 0;
        std::map<uint32_t, size_t> types, paths;
        for (const GtfsEntry& f : vol->Files()) {
            if (f.path.rfind("crsobj/", 0) != 0 || f.path.size() < 7 || f.path.compare(f.path.size() - 7, 7, ".tro.gz") != 0) continue;
            const std::vector<uint8_t> tro = vol->Read(f);
            courses++;
            try {
                const camera::ReplayCameraData d = camera::ReplayCamerasOfTro(tro);
                for (uint32_t record : d.Records()) {
                    records++;
                    const uint32_t type = d.U16(record) & 0xF;
                    types[type]++;
                    if (type == 0 || type == 3) {
                        const uint32_t path = record + (type == 0 ? 0x18u : 0x1Cu);
                        paths[d.U16(path)]++;
                        for (int32_t progress = 0; progress <= 4096; progress += 256) {
                            int32_t out[3] = {0, 0, 0};
                            camera::PathPosition(d, path, progress, out);
                        }
                    }
                }
            } catch (const std::exception& e) {
                if (errors++ < 3) std::printf("    %s: %s\n", f.path.c_str(), e.what());
            }
        }
        std::printf("%-10s %zu courses, %zu trackside records (types", "CamScan", courses, records);
        for (const auto& [type, n] : types) std::printf(" %u: %zu", type, n);
        std::printf("; paths");
        for (const auto& [kind, n] : paths) std::printf(" %u: %zu", kind, n);
        std::printf("), %zu errors  %s\n", errors, errors ? "FAIL" : "ok");
        failures += errors ? 1 : 0;
    }
    return failures;
}

} // namespace gt2::verify

// ================================================================ captured frames (gt2verify --camera-capture)

namespace gt2::verify {

int CameraCaptureCheck(const std::string& discPath, uint64_t from, uint64_t to, uint64_t every, const std::string& script) {
    DiscImage disc(discPath);
    GtfsVolume vol(disc);
    SetActiveProfile(ProfileOf(disc)); // the disc's build: the camera object / globals at its addresses (D), as --race-capture
    kCarBase = D(0x800A9688u);
    const std::vector<ScriptPress> presses = ParseInputScript(script);
    std::string exeName;
    for (const auto& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0) exeName = f.name;
    const auto exeFile = disc.FindRootFile(exeName);
    if (!exeFile) throw std::runtime_error("no PS-X EXE in the disc root");
    std::vector<uint8_t> exe(exeFile->size);
    disc.ReadForm1(exeFile->lba, 0, exe.data(), exe.size());
    std::puts("indexing car models...");
    SceneExtractor extractor(vol);
    const GuestImage raceOverlay = LoadOverlayImage(disc, kRaceOverlayIndex);
    auto raceOverlayLoaded = [&](const uint8_t* ram) { return std::memcmp(ram + 0x100F4, raceOverlay.At(0x800100F4u, 0x2000), 0x2000) == 0; };
    Machine m;
    m.AttachDisc(&disc);
    m.EnableSceneCapture();
    m.LoadExe(exe, 0x801FFF00u);

    uint64_t field = 0;
    size_t frames = 0, oursExact = 0, oursCompared = 0, extracted = 0, hEqual = 0, players2 = 0;
    double worstPosition = 0, worstAngle = 0;
    uint64_t frameCounter = 0;
    std::vector<uint8_t> copy(Bus::kRamSize);
    camera::RaceCamera previous{};
    bool havePrevious = false;
    // DEV CAPTURE AID (GT2_CAPTURE_AI_PLAYER=1, as --race-capture): the original's AI drives the player cars (gt2run/ai_player*.h).
    if (std::getenv("GT2_CAPTURE_AI_PLAYER"))
        m.cpu.onCall = [&](uint32_t from, uint32_t target) {
            if (AiPlayerSwitch(m, from, target) || ArcadeAiPlayerSwitch(m, from, target)) std::printf("camera capture: field %llu: a player car starts with control class 2 (dev capture aid)\n", (unsigned long long)field);
        };
    m.onSceneFrame = [&](const Machine::SceneFrame& frame) {
        if (field < from) return;
        const SceneExtractor::Scene scene = extractor.Extract(frame.transforms, frame.vertices);
        const uint8_t* ram = m.bus.Ram();
        if (Get<uint32_t>(ram, D(kCamera) + 0x9C) != D(0x800A9528u) || !raceOverlayLoaded(ram)) return; // not in a race
        frames++;
        const camera::RaceCamera& c = *reinterpret_cast<const camera::RaceCamera*>(ram + (D(kCamera) & 0x1FFFFF));
        // Our camera from the same state: the frame's update without the pad (0x80010608 / 0x800103C0 / 0x800109FC).
        std::memcpy(copy.data(), ram, Bus::kRamSize);
        const Track* track = extractor.CurrentTrack();
        bool exact = false, compared = false;
        std::string ourNote;
        try {
            GuestWorld w(copy.data(), track && track->courseLength == Get<int32_t>(copy.data(), Get<uint32_t>(copy.data(), D(kCourseObject) + 0xB544)) ? track : nullptr);
            camera::RaceCamera ours = c;
            const camera::CameraPad none{};
            if (!w.world.replay) {
                if (w.world.hold != 0) camera::StartIntro(ours, w.world);
                else {
                    camera::SetViewAngleProjection(ours, w.world);
                    camera::RaceView(ours, ours.position, w.world);
                }
                compared = true;
            } else if (w.world.track) {
                camera::ReplayCamera(ours, none, w.world);
                compared = true;
            }
            exact = compared && std::memcmp(&ours.view, &c.view, sizeof ours.view) == 0 && ours.H == c.H && ours.centreX == c.centreX && ours.centreY == c.centreY;
        } catch (const std::exception& e) {
            ourNote = e.what();
        }
        oursCompared += compared;
        oursExact += exact;
        // 2 player Battle (game mode 0): player 2's camera object (view + 0xC4 + 0x110, pad object 0x800A95D8; split view, following
        // car 1) recomputed the same way (docs/research/arcade_disc.md section 19).
        if (Get<uint8_t>(ram, D(0x801D5866u)) == 0 && Get<uint32_t>(ram, D(kCamera) + 0x110 + 0x9C) == D(0x800A95D8u)) {
            const camera::RaceCamera& c2 = *reinterpret_cast<const camera::RaceCamera*>(ram + ((D(kCamera) + 0x110) & 0x1FFFFF));
            bool exact2 = false, compared2 = false;
            try {
                GuestWorld w(copy.data(), track && track->courseLength == Get<int32_t>(copy.data(), Get<uint32_t>(copy.data(), D(kCourseObject) + 0xB544)) ? track : nullptr);
                camera::RaceCamera ours = c2;
                if (!w.world.replay) {
                    if (w.world.hold != 0) camera::StartIntro(ours, w.world);
                    else {
                        camera::SetViewAngleProjection(ours, w.world);
                        camera::RaceView(ours, ours.position, w.world);
                    }
                    compared2 = true;
                } else if (w.world.track) {
                    camera::ReplayCamera(ours, camera::CameraPad{}, w.world);
                    compared2 = true;
                }
                exact2 = compared2 && std::memcmp(&ours.view, &c2.view, sizeof ours.view) == 0 && ours.H == c2.H && ours.centreX == c2.centreX && ours.centreY == c2.centreY;
            } catch (const std::exception& e) {
                ourNote += std::string(" player 2: ") + e.what();
            }
            oursCompared += compared2;
            oursExact += exact2;
            if (compared2 && !exact2) ourNote += " player 2 DIFFERS";
            players2++;
        }
        // The extracted camera (GTE transforms of the flip) against a camera object: position error (m), worst axis
        // angle (degrees; rows right, down, forward = columns 0, -1, -2 of the camera matrix), H equal.
        struct Errors { double position = -1, angle = -1; bool h = false; };
        auto errorsOf = [&](const camera::RaceCamera& cam) {
            Errors e;
            double d2 = 0;
            for (int i = 0; i < 3; i++) {
                const double d = double(scene.cameraPosition[size_t(i)]) - double(cam.view.t[i]) / 65536.0;
                d2 += d * d;
            }
            e.position = std::sqrt(d2);
            double worstDot = 1;
            for (int row = 0; row < 3; row++) {
                double dot = 0, n = 0;
                for (int k = 0; k < 3; k++) {
                    const double col = double(cam.view.m[k][row]) / 4096.0 * (row == 0 ? 1 : -1);
                    dot += double(scene.cameraAxes[size_t(row)][size_t(k)]) * col;
                    n += col * col;
                }
                worstDot = std::min(worstDot, dot / std::sqrt(n));
            }
            e.angle = std::acos(std::max(-1.0, std::min(1.0, worstDot))) * 180.0 / 3.14159265358979;
            e.h = int(scene.projectionDistance) == int(cam.H);
            return e;
        };
        Errors now, before;
        if (scene.valid && havePrevious) { // the flip reports the transforms of the frame drawn before this update
            extracted++;
            now = errorsOf(c);
            before = errorsOf(previous);
            worstPosition = std::max(worstPosition, before.position);
            worstAngle = std::max(worstAngle, before.angle);
            hEqual += before.h;
        }
        previous = c;
        havePrevious = true;
        const double dp = before.position, angle = before.angle;
        if (frameCounter++ % every == 0) {
            char what[64];
            const uint8_t replay = Get<uint8_t>(ram, D(kReplayFlag));
            const uint16_t hold = Get<uint16_t>(ram, D(kHold));
            if (replay) std::snprintf(what, sizeof what, "replay mode %u type %u", c.replayMode, unsigned(c.replayFlags & 0xF));
            else if (hold) std::snprintf(what, sizeof what, "intro hold %u", hold);
            else std::snprintf(what, sizeof what, "position %u%s", c.position, c.lookBack ? " back" : "");
            std::printf("f%-6llu %-22s camera eye (%9.3f %8.3f %9.3f) H %4d | extracted (%9.3f %8.3f %9.3f) H %4.0f vs the previous frame's camera: "
                        "|d| %.3f m, axes %.3f deg, H %s (this frame's: %.3f m, %.3f deg) | ours %s%s\n",
                        (unsigned long long)field, what, c.view.t[0] / 65536.0, c.view.t[1] / 65536.0, c.view.t[2] / 65536.0, c.H, scene.cameraPosition[0],
                        scene.cameraPosition[1], scene.cameraPosition[2], scene.projectionDistance, dp, angle, before.h ? "=" : "differs", now.position, now.angle,
                        !compared ? "-" : exact ? "= camera object" : "DIFFERS", ourNote.empty() ? "" : (" (" + ourNote + ")").c_str());
        }
    };
    for (field = 1; field <= to; field++) {
        m.padButtons = ScriptButtons(presses, field);
        m.pad2Connected = ScriptUsesPort2(presses);
        m.pad2Buttons = ScriptButtons(presses, field, 1);
        const std::string reason = m.Run(Machine::kInstructionsPerVBlank);
        if (reason != "instruction budget exhausted") {
            std::printf("guest stopped at field %llu: %s\n", (unsigned long long)field, reason.c_str());
            break;
        }
    }
    std::printf("camera capture: %zu race frames in fields %llu..%llu (player 2's camera in %zu); ours bit-exact to the camera object in %zu of %zu recomputed; extracted camera in %zu "
                "frames against the previous frame's camera object: worst position %.3f m, worst axis %.3f deg, H equal in %zu\n",
                frames, (unsigned long long)from, (unsigned long long)to, players2, oursExact, oursCompared, extracted, worstPosition, worstAngle, hEqual);
    return oursExact == oursCompared ? 0 : 1;
}

} // namespace gt2::verify
