// gt2verify --race-capture <disc> <to-field> <out.bin> ["script"]: runs the original from boot with the scripted pad and
// writes one gt2formats/race_capture.h record per race frame (at the entry of the physics tick 0x8003EBF0: the car array,
// the shell's counters, player 1's logical pad and replay stream). gt2game --frames-compare replays the same race natively
// and compares frame by frame. The capture holds RAM contents of the game: write it under work\ only.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

#include "game/camera/race_camera.h"
#include "gt2formats/race_capture.h"
#include "gt2vfs/disc_image.h"
#include "guest.h"
#include "machine/machine.h"
#include "../gt2run/ai_player.h"
#include "../gt2run/ai_player_arcade.h"
#include "../gt2run/input_script.h"

namespace gt2::verify {

namespace {
template <typename T> T Get(const uint8_t* ram, uint32_t address) { T v; std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T)); return v; }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(ram + (address & 0x1FFFFF), &v, sizeof(T)); }
constexpr uint32_t kViewObject = 0x801FF8B8u, kCamera = kViewObject + 0xC4; // player 1's camera object
} // namespace

// 0x800294D4's mirror camera (camera::MirrorCamera): the renderer runs on the guest until it hands the mirror's camera
// copy to 0x800298FC; the copy (the whole 0x110-byte object) is compared with ours.
int VerifyMirror(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    int failures = 0;
    size_t cases = 0, mismatches = 0;
    for (size_t variant = 0; variant < 400; variant++) {
        std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
        uint8_t* ram = guest.Ram();
        camera::RaceCamera c;
        std::memcpy(&c, ram + (D(kCamera) & 0x1FFFFF), sizeof c);
        if (variant % 4 != 0) {
            camera::RotationFromAngles(c.view.m, int32_t(rng() & 0xFFF), int32_t(rng() % 1024) - 512, int32_t(rng() % 512) - 256);
            for (int i = 0; i < 3; i++) c.view.t[i] = int32_t(rng() % 0x8000000u) - 0x4000000;
        }
        c.split = 0;
        c.mirror = 1;
        std::memcpy(ram + (D(kCamera) & 0x1FFFFF), &c, sizeof c);
        Put<uint8_t>(ram, D(0x801D5866u), uint8_t(1 + rng() % 4));
        Put<uint8_t>(ram, D(0x801D5864u), 2);
        if (!guest.CallUntil(0x800294D4u, 0x800298FCu, D(kViewObject), 0)) throw std::runtime_error("mirror: 0x800294D4 did not reach 0x800298FC");
        if (guest.Reg(6) != 1) throw std::runtime_error("mirror: the first 0x800298FC call is not the mirror's");
        camera::RaceCamera original;
        std::memcpy(&original, guest.Ram() + (guest.Reg(5) & 0x1FFFFF), sizeof original);
        const camera::RaceCamera ours = camera::MirrorCamera(c);
        cases++;
        if (std::memcmp(&ours, &original, sizeof ours) != 0) {
            if (mismatches++ < 3) {
                for (size_t i = 0; i < sizeof ours; i++)
                    if (reinterpret_cast<const uint8_t*>(&ours)[i] != reinterpret_cast<const uint8_t*>(&original)[i]) {
                        std::printf("    MISMATCH variant %zu: first differing byte + 0x%zX\n", variant, i);
                        break;
                    }
            }
        }
    }
    Report("CamMirror", 0x800294D4u, cases, mismatches, failures);
    return failures;
}

int RaceCapture(const std::string& discPath, uint64_t to, const std::string& outPath, const std::string& script) {
    DiscImage disc(discPath);
    // The disc's build (gt2formats/exe_profile.h): the hooks and the captured globals by their Simulation addresses.
    const ExeProfile& profile = ProfileOf(disc);
    SetActiveProfile(profile);
    kCarBase = D(0x800A9688u);
    const uint32_t raceSetup = profile.Code(0x8001523Cu), tick = profile.Code(0x8003EBF0u);
    if (!profile.reference) std::printf("race capture on %s (%s): hooks 0x%08X / 0x%08X\n", profile.name, profile.exeName, raceSetup, tick);
    const std::vector<ScriptPress> presses = ParseInputScript(script);
    std::string exeName;
    for (const auto& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0) exeName = f.name;
    const auto exeFile = disc.FindRootFile(exeName);
    if (!exeFile) throw std::runtime_error("no PS-X EXE in the disc root");
    std::vector<uint8_t> exe(exeFile->size);
    disc.ReadForm1(exeFile->lba, 0, exe.data(), exe.size());
    std::FILE* out = std::fopen(outPath.c_str(), "wb");
    if (!out) throw std::runtime_error("cannot write " + outPath);

    Machine m;
    m.AttachDisc(&disc);
    // GT2_CAPTURE_CARD=<mcd>: memory card 1 (e.g. a card with a replay file for the title's Replay Theater; the capture
    // writes to it when the game saves - use a copy).
    if (const char* card = std::getenv("GT2_CAPTURE_CARD")) m.AttachMemoryCard(card);
    if (const char* card = std::getenv("GT2_CAPTURE_CARD2")) m.AttachMemoryCard(card, 1); // memory card 2 (slot 2)
    m.gpu.skip3dRaster = true;
    m.LoadExe(exe, 0x801FFF00u);
    uint64_t field = 0;
    uint32_t races = 0, frame = 0, written = 0;
    bool inRace = false;
    auto record = std::make_unique<RaceCaptureFrame>();
    // DEV CAPTURE AID (GT2_CAPTURE_AI_PLAYER=1): the original's AI drives player 1's car (gt2run/ai_player*.h) - the
    // state gt2game's --ai-player builds natively; needed for the one-car game mode 6 (Time Trial / Rally) captures.
    const bool aiPlayer = std::getenv("GT2_CAPTURE_AI_PLAYER") != nullptr;
    // Game mode 6 side file <out>.ghost (gt2formats/race_capture.h RaceCaptureGhost): the ghost block of the race task and
    // the reference ghost buffer, per frame.
    std::FILE* ghost = nullptr;
    auto ghostRecord = std::make_unique<RaceCaptureGhost>();
    m.cpu.onCall = [&](uint32_t from, uint32_t target) {
        if (aiPlayer && (AiPlayerSwitch(m, from, target) || ArcadeAiPlayerSwitch(m, from, target)))
            std::printf("race capture: field %llu: the player's car starts with control class 2 (dev capture aid)\n", (unsigned long long)field);
        if (target == raceSetup) { // the race load's per-car setup driver (0x8001523C): a new race
            // Side file <out>.setup (gt2formats/race_capture.h RaceCaptureSetup): the race block 0x801D585C (0x58C bytes: mode
            // bytes, laps, the car entries with their configurations) and the settings block 0x801C98A0 (0x40) of the race.
            if (races == 0) {
                RaceCaptureSetup setup;
                std::memcpy(setup.raceBlock.data(), m.bus.Ram() + (D(0x801D585Cu) & 0x1FFFFF), setup.raceBlock.size());
                std::memcpy(setup.settings.data(), m.bus.Ram() + (D(0x801C98A0u) & 0x1FFFFF), setup.settings.size());
                if (std::FILE* s = std::fopen((outPath + ".setup").c_str(), "wb")) {
                    std::fwrite(&setup, sizeof setup, 1, s);
                    std::fclose(s);
                }
            }
            races++;
            frame = 0;
            inRace = true;
            return;
        }
        if (target != tick || !inRace) return; // the physics tick 0x8003EBF0
        const uint8_t* ram = m.bus.Ram();
        RaceCaptureFrame& r = *record;
        r.field = uint32_t(field);
        r.race = races;
        r.frame = frame++;
        r.hold = Get<uint16_t>(ram, D(0x800A9520u));
        r.sinceFinish = Get<uint16_t>(ram, D(0x800A9522u));
        r.clock = Get<uint32_t>(ram, D(0x80046F64u));
        r.carCount = Get<uint8_t>(ram, D(0x800AF231u));
        r.gameMode = Get<uint8_t>(ram, D(0x801D5866u));
        r.demo = Get<uint8_t>(ram, D(0x800A951Cu));
        r.courseIndex = Get<uint8_t>(ram, D(0x800AF230u));
        r.padButtons = Get<uint32_t>(ram, D(0x800A9528u) + 0x8C);
        r.padAnalog = Get<uint16_t>(ram, D(0x800A9528u) + 0x9C);
        r.padSteer = Get<uint16_t>(ram, D(0x800A9528u) + 0x9E);
        r.padThrottle = Get<uint16_t>(ram, D(0x800A9528u) + 0xA2);
        r.padBrake = Get<uint16_t>(ram, D(0x800A9528u) + 0xA4);
        std::memcpy(r.cars.data(), ram + (kCarBase & 0x1FFFFF), r.cars.size());
        std::memcpy(r.stream.data(), ram + (D(0x801D5F84u) & 0x1FFFFF), r.stream.size());
        std::fwrite(&r, sizeof r, 1, out);
        written++;
        if (r.gameMode == 6) {
            if (!ghost) ghost = std::fopen((outPath + ".ghost").c_str(), "wb");
            if (ghost) {
                RaceCaptureGhost& g = *ghostRecord;
                g.field = r.field;
                g.race = r.race;
                g.frame = r.frame;
                std::memcpy(g.block.data(), ram + (D(kRaceCaptureGhostBlock) & 0x1FFFFF), g.block.size());
                std::memcpy(g.reference.data(), ram + (D(kRaceCaptureGhostReference) & 0x1FFFFF), g.reference.size());
                std::memcpy(g.flags.data(), ram + (D(kRaceCaptureGhostFlags) & 0x1FFFFF), g.flags.size());
                std::fwrite(&g, sizeof g, 1, ghost);
            }
        }
    };
    // DEV CAPTURE AID (GT2_CAPTURE_POKE="field:address=byte,..." hex address / byte): writes a guest RAM byte before that field
    // (e.g. a career option such as the 2 player Battle's Handicap Start, career + 7, without the options screen route).
    struct Poke { uint64_t field; uint32_t address; uint8_t value; };
    std::vector<Poke> pokes;
    if (const char* p = std::getenv("GT2_CAPTURE_POKE"))
        for (const char* c = p; *c;) {
            char* end = nullptr;
            Poke k;
            k.field = std::strtoull(c, &end, 10);
            if (*end != ':') throw std::runtime_error("GT2_CAPTURE_POKE: field:address=byte expected");
            k.address = uint32_t(std::strtoul(end + 1, &end, 16));
            if (*end != '=') throw std::runtime_error("GT2_CAPTURE_POKE: field:address=byte expected");
            k.value = uint8_t(std::strtoul(end + 1, &end, 16));
            pokes.push_back(k);
            c = *end ? end + 1 : end;
        }
    for (field = 1; field <= to; field++) {
        for (const Poke& k : pokes)
            if (k.field == field) {
                m.bus.Ram()[k.address & 0x1FFFFF] = k.value;
                std::printf("race capture: field %llu: poke 0x%08X = %02X (dev capture aid)\n", (unsigned long long)field, k.address, k.value);
            }
        m.padButtons = ScriptButtons(presses, field);
        m.pad2Connected = ScriptUsesPort2(presses);
        m.pad2Buttons = ScriptButtons(presses, field, 1);
        const std::string reason = m.Run(Machine::kInstructionsPerVBlank);
        if (reason != "instruction budget exhausted") {
            std::printf("guest stopped at field %llu: %s\n", (unsigned long long)field, reason.c_str());
            break;
        }
    }
    std::fclose(out);
    if (ghost) std::fclose(ghost);
    std::printf("race capture: %u race frame(s) of %u race(s) up to field %llu -> %s\n", written, races, (unsigned long long)to, outPath.c_str());
    return written ? 0 : 1;
}

} // namespace gt2::verify
