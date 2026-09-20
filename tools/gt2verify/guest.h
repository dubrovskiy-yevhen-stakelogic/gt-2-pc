#pragma once
// Shared harness of gt2verify: the guest (original code in the interpreter on a RAM dump) and the generic
// stateful comparison. Every verify_*.cpp adds its own checks on top of this.
#include <array>
#include <cstdio>
#include <functional>
#include <cstring>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "game/sim/ai_driver.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/track.h"
#include "guest/bus.h"
#include "interp/gte.h"
#include "interp/r3000.h"

namespace gt2 {
class GtfsVolume;
class DiscImage;
}
namespace gt2::sim {
struct GhostSession;
}

namespace gt2::verify {

constexpr uint32_t kSentinel = 0xBFC0DEA0, kStack = 0x801FF000;
// Data addresses of the dump's build (gt2formats/exe_profile.h): every table / global the harness names by its US
// Simulation v1.2 address goes through D(); main selects the profile from the SHA-1 of the dump's disc executable (the
// identity for the Simulation disc). Harness-owned scratch areas (kStack, 0x801E8000.., 0x801FC000.., 0x801FFD00..) are
// free RAM in both builds and are not translated.
inline uint32_t D(uint32_t simAddress) { return gt2::ActiveProfile().Race(simAddress); }
constexpr uint32_t kCarStride = 0xB40, kBodyOffset = 0x2C, kMaxCars = 6; // the car array has six slots
inline uint32_t kCarBase = 0x800A9688; // the car array, D(0x800A9688) (set by main: US Arcade v1.1 0x800A9378)

// The car-contact state in the Simulation layout. CarContactState (car_contact.h) is laid out like the Simulation build's
// globals: the head block at 0x801C8608 (step parity, fraction / corner / side tables, 0x86 bytes) and the corner tables at
// + 0x138 (0x801C8740). US Arcade v1.1 keeps the corner tables 8 bytes further from the head block (head -> 0x801C8060 =
// -0x5A8, corners -> 0x801C81A0 = -0x5A0: the lui-built references of 0x800400CC in both builds, gt2tool exe-map). The port's
// struct view starts at ContactBase() = corners - 0x138; SimContactLayout moves the head block there for the native side (and
// keeps the bytes it covers), its destructor moves it back. Both are no-ops for the Simulation build.
inline uint32_t ContactBase() { return D(0x801C8740u) - 0x138u; }
class SimContactLayout {
public:
    static constexpr uint32_t kHead = 0x86;
    explicit SimContactLayout(uint8_t* ram) : ram_(ram), head_(D(0x801C8608u) & 0x1FFFFF), shift_(ContactBase() - D(0x801C8608u)) {
        if (shift_ == 0) return;
        if (shift_ > sizeof(saved_)) throw std::runtime_error("contact state: unexpected layout of the dump's build");
        std::memcpy(saved_, ram_ + head_ + kHead, shift_);
        std::memmove(ram_ + head_ + shift_, ram_ + head_, kHead);
    }
    ~SimContactLayout() { UndoOn(ram_); }
    // The build's layout on a copy of the image taken while this guard is active.
    void UndoOn(uint8_t* image) const {
        if (shift_ == 0) return;
        std::memmove(image + head_, image + head_ + shift_, kHead);
        std::memcpy(image + head_ + kHead, saved_, shift_);
    }
    SimContactLayout(const SimContactLayout&) = delete;
    SimContactLayout& operator=(const SimContactLayout&) = delete;

private:
    uint8_t* ram_;
    uint32_t head_;
    uint32_t shift_;
    uint8_t saved_[16] = {};
};
// Cars taking part in the dump's race: 0x800AF231 (the count every physics pass gets), set by main; 6 when the
// byte is not plausible (attract race dumps have 6).
inline uint32_t kCarCount = kMaxCars;
// The guest's scratchpad (0x1F800000, 1 KB) is the physics step's work area; compared like RAM.
constexpr uint32_t kScratchSize = 0x400;

// Cross-build code addresses (docs/research/arcade_disc.md): set by main for a dump of another build than US Simulation v1.2
// (e.g. US Arcade v1.1): every guest function the harness calls by its Simulation address is translated to the dump's build
// (the profile's aligned runs; with GT2_VERIFY_SIM_DISC=<Sim disc> the map is recomputed from both discs instead).
inline std::function<uint32_t(uint32_t)> gCodeAddressMap;
inline uint32_t MapCode(uint32_t address) { return gCodeAddressMap ? gCodeAddressMap(address) : address; }

class Guest {
public:
    explicit Guest(const std::string& ramPath) : cpu_(bus_, &gte_) {
        std::FILE* f = std::fopen(ramPath.c_str(), "rb");
        if (!f) throw std::runtime_error("cannot open " + ramPath);
        const size_t n = std::fread(bus_.Ram(), 1, Bus::kRamSize, f);
        std::fclose(f);
        if (n != Bus::kRamSize) throw std::runtime_error("RAM dump must be exactly 2 MB");
        cpu_.onException = [](R3000::Exception cause, uint32_t epc) -> bool {
            throw std::runtime_error("guest exception " + std::to_string(cause) + " at " + Bus::Hex(epc));
        };
    }

    uint32_t Call(uint32_t function, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        function = MapCode(function);
        cpu_.gpr[4] = a0; cpu_.gpr[5] = a1; cpu_.gpr[6] = a2; cpu_.gpr[7] = a3;
        cpu_.gpr[29] = kStack;
        cpu_.gpr[31] = kSentinel;
        cpu_.SetPc(function);
        if (!cpu_.RunUntil(kSentinel, 50'000'000)) throw std::runtime_error("guest function " + Bus::Hex(function) + " did not return");
        return cpu_.gpr[2];
    }

    // Call() for routines that reach the BIOS A-table (jump to 0xA0 with the function number in t1; the dumps come
    // from the HLE kernel and hold no BIOS code): the harness performs A(27h) bcopy(src, dst, length) itself and
    // throws on any other function.
    uint32_t CallWithBios(uint32_t function, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        function = MapCode(function);
        cpu_.gpr[4] = a0; cpu_.gpr[5] = a1; cpu_.gpr[6] = a2; cpu_.gpr[7] = a3;
        cpu_.gpr[29] = kStack;
        cpu_.gpr[31] = kSentinel;
        cpu_.SetPc(function);
        for (uint64_t n = 0; cpu_.pc != kSentinel; n++) {
            if (n > 200'000'000) throw std::runtime_error("guest function " + Bus::Hex(function) + " did not return");
            if ((cpu_.pc & 0x1FFFFFFF) == 0xB0 || (cpu_.pc & 0x1FFFFFFF) == 0xC0)
                throw std::runtime_error("BIOS table " + Bus::Hex(cpu_.pc) + " function " + Bus::Hex(cpu_.gpr[9]) + " not provided by the harness");
            if ((cpu_.pc & 0x1FFFFFFF) == 0xA0) {
                if (cpu_.gpr[9] != 0x27) throw std::runtime_error("BIOS A(" + Bus::Hex(cpu_.gpr[9]) + ") not provided by the harness");
                auto byteAt = [&](uint32_t address) -> uint8_t& { // RAM or the scratchpad (0x1F800000, 1 KB)
                    const uint32_t p = address & 0x1FFFFFFF;
                    return (p >= 0x1F800000u && p < 0x1F800400u) ? bus_.Scratch()[p - 0x1F800000u] : bus_.Ram()[p & 0x1FFFFF];
                };
                for (uint32_t i = 0; i < cpu_.gpr[6]; i++) byteAt(cpu_.gpr[5] + i) = byteAt(cpu_.gpr[4] + i);
                cpu_.SetPc(cpu_.gpr[31]);
                continue;
            }
            recent_[n % recent_.size()] = cpu_.pc;
            try {
                cpu_.Step();
            } catch (const std::exception& e) {
                std::string trail;
                for (size_t k = 1; k <= recent_.size(); k++) trail += " " + Bus::Hex(recent_[(n + k) % recent_.size()]);
                throw std::runtime_error(std::string(e.what()) + " (last pcs:" + trail + ")");
            }
        }
        return cpu_.gpr[2];
    }

    // Call() that stops when the pc reaches `stop` (a callee's entry: its arguments are then in Reg(4..7) and on the stack
    // at Reg(29) + 0x10..). Returns false when the function returned (or ran out of budget) first.
    bool CallUntil(uint32_t function, uint32_t stop, uint32_t a0 = 0, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0) {
        function = MapCode(function);
        stop = MapCode(stop);
        cpu_.gpr[4] = a0; cpu_.gpr[5] = a1; cpu_.gpr[6] = a2; cpu_.gpr[7] = a3;
        cpu_.gpr[29] = kStack;
        cpu_.gpr[31] = kSentinel;
        cpu_.SetPc(function);
        for (uint64_t n = 0; n < 50'000'000; n++) {
            if (cpu_.pc == stop) return true;
            if (cpu_.pc == kSentinel) return false;
            cpu_.Step();
        }
        return false;
    }
    uint32_t Reg(int i) const { return cpu_.gpr[i]; }

    uint8_t* Ram() { return bus_.Ram(); }
    uint8_t* Scratch() { return bus_.Scratch(); }
    uint32_t V1() const { return cpu_.gpr[3]; } // high word of a 64-bit result

private:
    std::array<uint32_t, 12> recent_{};
    Bus bus_;
    Gte gte_;
    R3000 cpu_;
};

// Prints one result line and counts the failure.
inline void Report(const char* name, uint32_t address, size_t cases, size_t mismatches, int& failures) {
    std::printf("%-10s 0x%08X  %zu cases, %zu mismatches  %s\n", name, address, cases, mismatches, mismatches ? "FAIL" : "ok");
    failures += mismatches ? 1 : 0;
}

// Stateful routines: the original runs on the guest RAM (+ scratchpad), our port on byte-identical copies
// (the port's structs are laid out like the original's, so it works directly on the image). Afterwards ALL of
// RAM and the scratchpad must be equal, except the guest stack the original used for its frame.
//   prepare(ram, scratch, object, variant): randomise the inputs on the guest image (before it is copied)
//   native(ram, scratch, object): run the port on our copy
// `object` (a guest address) is passed as a0 (a1..a3 as given); `ram + (object & 0x1FFFFF)` is the object.
struct StatefulResult { size_t cases = 0, mismatches = 0; };

// `ignoreScratch` = [begin, end) of scratchpad bytes excluded from the comparison: the original's wall sweep
// (0x80041CCC) keeps its four sweep segments at 0x1F800004..0x1F8000B4, which our port holds in locals.
struct ScratchIgnore { uint32_t begin = 0, end = 0; };

template <typename Prepare, typename Native>
StatefulResult VerifyStateful(Guest& guest, const std::vector<uint8_t>& pristine, uint32_t function, const std::vector<uint32_t>& objects,
                              size_t variants, Prepare prepare, Native native, uint32_t a1 = 0, uint32_t a2 = 0, uint32_t a3 = 0,
                              ScratchIgnore ignoreScratch = {}) {
    StatefulResult result;
    std::vector<uint8_t> ours(Bus::kRamSize), ourScratch(kScratchSize);
    for (uint32_t object : objects)
        for (size_t variant = 0; variant < variants; variant++) {
            std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
            std::memset(guest.Scratch(), 0, kScratchSize);
            {
                SimContactLayout layout(guest.Ram());
                prepare(guest.Ram(), guest.Scratch(), object, variant);
            }
            std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
            std::memcpy(ourScratch.data(), guest.Scratch(), kScratchSize);
            try {
                guest.Call(function, object, a1, a2, a3);
            } catch (const std::runtime_error& e) { // the original trapped (e.g. its 64-bit division on a zero divisor): no case
                std::printf("    skipped (original trapped): object %08X variant %zu: %s\n", object, variant, e.what());
                continue;
            }
            {
                SimContactLayout layout(ours.data());
                native(ours.data(), ourScratch.data(), object);
            }
            result.cases++;
            const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
            if (ignoreScratch.end > ignoreScratch.begin) // neutralise the ignored range in our copy
                std::memcpy(ourScratch.data() + ignoreScratch.begin, guest.Scratch() + ignoreScratch.begin, ignoreScratch.end - ignoreScratch.begin);
            const bool equal = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 &&
                               std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0 &&
                               std::memcmp(ourScratch.data(), guest.Scratch(), kScratchSize) == 0;
            if (!equal && result.mismatches++ < 3) {
                bool shown = false;
                for (uint32_t i = 0; i < Bus::kRamSize && !shown; i++)
                    if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                        std::printf("    MISMATCH object %08X variant %zu: first differing byte at 0x%08X (object + 0x%X): original %02X ours %02X\n", object,
                                    variant, 0x80000000u + i, i - (object & 0x1FFFFF), guest.Ram()[i], ours[i]);
                        shown = true;
                    }
                for (uint32_t i = 0; i < kScratchSize && !shown; i++)
                    if (ourScratch[i] != guest.Scratch()[i]) {
                        std::printf("    MISMATCH object %08X variant %zu: first differing scratchpad byte at 0x1F800000 + 0x%X: original %02X ours %02X\n",
                                    object, variant, i, guest.Scratch()[i], ourScratch[i]);
                        shown = true;
                    }
            }
        }
    return result;
}

// Entry points of the per-subsystem checks (verify_*.cpp). Each returns the number of failed rows.
int VerifyDriveShafts(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
int VerifyTyres(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
int VerifyDrivetrain(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// `track` is the parsed course of the dump (null when gt2verify runs without the disc image).
int VerifyGround(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track);
int VerifyContact(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track);
int VerifyCore(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track);
int VerifySetup(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// The native car parameter record builder (gt2formats/car_params.*) against the six attract-race records. `vol` =
// the disc's GT2.VOL (null = skipped: the tables and the replay come from it).
int VerifyParams(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const GtfsVolume* vol);
// The assembled race (race_sim.*): course queries, render transform, race order. `tro` = the course file's bytes
// (empty = the .tro-parsed extras are not cross-checked against the guest's course object).
int VerifyRace(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track, std::span<const uint8_t> tro = {});
// The start grid (verify_grid.cpp, race_sim.h PlaceOnGrid): the arguments 0x80012CD4 hands to 0x80033384 against ours, on
// random grids / angles / models of the dump (GridArgs) and, with the disc, from the disc's course and car models as gt2game
// computes them (GridDisc: the licence test / the attract race of the dump).
int VerifyGrid(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track, std::span<const uint8_t> tro, const GtfsVolume* vol);
// The wheel mesh area of the car setup 0x80017FA0 (verify_wheels.cpp, gt2export/car_mesh.h GenerateWheelArea); needs the disc.
int VerifyWheels(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc);
// The AI drivers (ai_driver.*): the three routines 0x80038540 dispatches to and their callees, on the dump's lines.
int VerifyAi(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track);
// The AI's context (line lists, tuning table, constants) resolved on a RAM image (pointers into it), for the
// full-frame check of verify_core.cpp that runs the native AI inside the physics core.
sim::AiContext AiContextFromRam(const uint8_t* ram);
// The disc-derived game data (gt2formats/course_data.*, overlay_data.*, game/sim/disc_data.*) against the dump:
// the overlay / executable constants, the course table, the start lines and the race lists of the dump's course
// (verify_data.cpp). Needs the disc; `vol` / `disc` null = skipped.
int VerifyData(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const GtfsVolume* vol, const DiscImage* disc);
// The race shell (race_shell.*): lap / sector lines, the section state machine, finish, results, the frame
// timers (verify_shell.cpp). Needs the course.
int VerifyShell(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track);
// Game mode 6 (race_shell.h GhostSession) over a RAM image: the ring 0x801D5F84, the reference lap 0x801DA4A0, the block
// 0x800A8D70, the overlay flags 0x8002F4B0.., car 0's stream pointer (+ 0x1C), car 1's + 0x0E / + 0x0F and the ghost entry's
// byte 0x801D5A14 (verify_shell.cpp). Store writes the pointer / entry byte back only when they changed.
void LoadGhostSession(const uint8_t* ram, sim::GhostSession& ghost);
void StoreGhostSession(uint8_t* ram, const sim::GhostSession& ghost, const sim::GhostSession& loaded);
// The car sound logic (game/audio/car_sound.*): engine layer pitch / crossfade, the engine player update, the
// per-car mix (verify_sound.cpp). Runs on the dump alone.
int VerifySound(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// The race music and reverb (verify_sound.cpp): the music choice at race load 0x800299D8 (any dump); with the disc the
// XA track table against MUSIC.DAT's sectors and our XA decoder / music player against the runtime SPU's XA path;
// the reverb unit's impulse response (measurement). GT2_AUDIO_WAV=<dir> also writes listening WAVs there.
int VerifyMusic(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol, const std::string& discPath);
// Licence tests (verify_license.cpp): the result record clear 0x8005E2FC (any dump); with the disc (`vol`) the licence
// database lookups / medal times of carparam/usa_license_data.dat, and on a licence dump the dump's test read from
// the disc against the original's settings block, course (`trackName`), car and car record.
int VerifyLicense(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const GtfsVolume* vol, const std::string& trackName);
// The GT-mode career (verify_career.cpp, src/game/career): the result appliers on race-overlay dumps, the new game,
// garage / money / parts rules, the tune sheet and the car purchase on GT-mode overlay dumps. Needs the disc (null =
// skipped).
int VerifyCareer(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc = nullptr, const GtfsVolume* vol = nullptr);
// GT-mode menu navigation (verify_menu.cpp, src/game/menu/menu_nav.*): 0x8001D754 nearest item, 0x8001D954,
// 0x8001D6CC, 0x8001B6F0 / 0x8001B680, 0x8005F858 on random pages. Skips itself unless GT2.OVL member 4 is loaded.
int VerifyMenu(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// The title overlay (verify_title.cpp, src/game/shell): option rows 0x80017D74 / 0x80017E68 / 0x80017F2C / 0x80018574,
// the title menu 0x8001792C + 0x80017984, the save-file class 0x8006A214 / 0x8006A314 / 0x8006A278 / 0x8006A038. Skips
// itself unless GT2.OVL member 1 is loaded.
int VerifyTitle(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// The replay file class of the executable (verify_title_replay.cpp, gt2formats/replay_card.h): 0x800691DC, 0x800692DC,
// 0x80069358, 0x80069418, 0x800695DC, 0x8006911C, 0x80069AC4, 0x80069948 (+ the title's 0x80020E14 when member 1 is loaded).
int VerifyTitleReplay(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol);
// The title's DATA TRANSFER rules (verify_title_transfer.cpp, game/shell/title_transfer.h): member 1 0x8001E014 / 0x8001E11C /
// 0x8001E284 / 0x8001E38C.. / 0x8001FBFC / 0x8001FC7C / 0x8001E61C / 0x8001EAD8 / 0x8001EB60, EXE 0x8005E0D0. Skips unless member 1 is loaded.
int VerifyTitleTransfer(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// The GT-mode machine test (verify_machine_test.cpp, game/career/machine_test.h): EXE 0x8005E03C (any dump), the race
// overlay's record write 0x80050D78 (member 0 loaded) and the menus' setup ovl4 0x80012C6C (member 4 loaded); needs the disc.
int VerifyMachineTest(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol);
// The arcade menus' race build and availability rules (verify_arcade_menu.cpp, src/game/arcade): 0x80010C84, 0x8001D120,
// 0x8001D418 of US Arcade v1.1 member 2. Skips itself unless the dump is of the arcade disc with member 2 loaded.
int VerifyArcadeMenu(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol);
// What an arcade race writes into the career (verify_arcade_results.cpp, src/game/arcade/arcade_results.*): EXE 0x8005DC64 of
// US Arcade v1.1. Skips itself on other discs.
int VerifyArcadeResults(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc);
// The arcade title's texts, option rows and save file (verify_arcade_title.cpp): US Arcade v1.1 member 1 0x800179E0 / 0x80017AD4 /
// 0x80017B98, EXE 0x8006A124 / 0x8006A224 / 0x8006A188 / 0x80069F48. Skips itself unless the arcade member 1 is loaded.
int VerifyArcadeTitle(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol);
// True when `length` bytes at `address` of the RAM image equal GT2.OVL member `member` (the overlay is loaded).
bool OverlayMemberLoaded(const uint8_t* ram, const DiscImage& disc, uint32_t member, uint32_t address, uint32_t length);
// The race cameras (verify_camera.cpp, src/game/camera): the camera object's routines 0x80010000..0x80011A58 and the
// library 0x80081374 / 0x800811B0 / 0x80081164 / 0x80082DA8 on the dump's camera; the trackside rows need `track`
// (the dump's course); with `vol` / `disc` the overlay tables, the course's camera list and a scan of all courses.
int VerifyCamera(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const Track* track, const GtfsVolume* vol, const DiscImage* disc,
                 const std::string& trackName);
// gt2verify --camera-capture <disc> <from> <to> <every> ["script"]: runs the original from boot (scripted pad) and at every
// flip of a race frame compares the camera object with the camera extracted from the frame's GTE transforms
// (scene_extractor) and with our camera recomputed from the same RAM; prints every <every>-th frame. Exit 0 = ours equal.
int CameraCaptureCheck(const std::string& discPath, uint64_t from, uint64_t to, uint64_t every, const std::string& script);
// gt2verify --race-capture <disc> <to> <out.bin> ["script"] (verify_replay.cpp): the original from boot, one
// gt2formats/race_capture.h record per race frame (compared natively by gt2game --frames-compare).
int RaceCapture(const std::string& discPath, uint64_t to, const std::string& outPath, const std::string& script);
// The rear-view mirror's camera (verify_replay.cpp, camera::MirrorCamera) against the renderer 0x800294D4 on the dump.
int VerifyMirror(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// The 2 player Battle's (game mode 0) race-overlay routines (verify_battle.cpp): catch-up 0x80042038, race-load settings
// 0x8003B73C / 0x8003B69C.
int VerifyBattle(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng);
// verify_pad.cpp: the controller path (libpad handlers of the EXE; with `raceOverlay` the race's logical pad 0x80014BB4 and
// the vibration tail of 0x800133F0).
int VerifyPad(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, bool raceOverlay);

} // namespace gt2::verify
