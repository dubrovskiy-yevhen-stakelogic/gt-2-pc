#pragma once
// Development capture of the original's race, frame by frame (written by `gt2verify --race-capture`, read by
// `gt2game --frames-compare`; the files live under work\ only - they hold RAM contents of the game). One record per call
// of the physics tick 0x8003EBF0 from the frame driver, taken at its entry: the state after the previous race frame.
// Not a game format.
#include <array>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace gt2 {

constexpr uint32_t kRaceCaptureMagic = 0x31464352u; // "RCF1"
constexpr uint32_t kRaceCaptureCarSize = 0xB40, kRaceCaptureCars = 6, kRaceCaptureStreamSize = 0x19 + 0x4400;

#pragma pack(push, 1)
struct RaceCaptureFrame {
    uint32_t magic = kRaceCaptureMagic;
    uint32_t field = 0;          // the field (1/60 s) of the call
    uint32_t race = 0;           // count of race setups (0x8001523C) so far: frames of one race share it
    uint32_t frame = 0;          // tick calls since that setup (0 = the state right after the setup)
    uint16_t hold = 0;           // 0x800A9520
    uint16_t sinceFinish = 0;    // 0x800A9522
    uint32_t clock = 0;          // 0x80046F64
    uint8_t carCount = 0;        // 0x800AF231
    uint8_t gameMode = 0;        // 0x801D5866
    uint8_t demo = 0;            // 0x800A951C
    uint8_t courseIndex = 0;     // 0x800AF230
    uint32_t padButtons = 0;     // player 1's pad object 0x800A9528: + 0x8C held logical buttons
    uint16_t padAnalog = 0, padSteer = 0, padThrottle = 0, padBrake = 0; // + 0x9C, + 0x9E, + 0xA2, + 0xA4
    std::array<uint8_t, kRaceCaptureCarSize * kRaceCaptureCars> cars{}; // the car array 0x800A9688
    std::array<uint8_t, kRaceCaptureStreamSize> stream{};               // player 1's replay stream object 0x801D5F84
};
// The race of a capture (side file <capture>.setup, written at the first race setup): the shell's race block 0x801D585C
// (0x58C bytes: + 0x0A game mode, + 0x0D countdown, + 0x0F laps, car entries of 0xD0 bytes from + 0x5C: u32 car id,
// CarConfig at + 8, grid slot + 0x8D, kind + 0x8E) and the race settings block 0x801C98A0 (0x40 bytes). Addresses are
// US Simulation v1.2's; the capture tool reads them at the build's addresses (gt2formats/exe_profile.h).
constexpr uint32_t kRaceCaptureSetupMagic = 0x31534352u; // "RCS1"
struct RaceCaptureSetup {
    uint32_t magic = kRaceCaptureSetupMagic;
    std::array<uint8_t, 0x58C> raceBlock{};
    std::array<uint8_t, 0x40> settings{};
};
// Game mode 6 (Time Trial / Rally / ghost) side file <capture>.ghost, one record per frame of a mode 6 race (same field /
// race / frame as the RaceCaptureFrame): the ghost block of the race task (0x800A8D70..0x800A94D0: the lap-start snapshot
// 0x800A8D70, the ghost's snapshot 0x800A90B2, the ghost playback state 0x800A93F8), the reference ghost lap buffer
// 0x801DA4A0 (0x10FC bytes) and the overlay's ghost flags 0x8002F4B0..0x8002F4B7. US Simulation v1.2 addresses.
constexpr uint32_t kRaceCaptureGhostMagic = 0x31484752u; // "RGH1"
constexpr uint32_t kRaceCaptureGhostBlock = 0x800A8D70u, kRaceCaptureGhostReference = 0x801DA4A0u, kRaceCaptureGhostFlags = 0x8002F4B0u;
struct RaceCaptureGhost {
    uint32_t magic = kRaceCaptureGhostMagic;
    uint32_t field = 0, race = 0, frame = 0;
    std::array<uint8_t, 0x800A94D0u - kRaceCaptureGhostBlock> block{};
    std::array<uint8_t, 0x10FC> reference{};
    std::array<uint8_t, 8> flags{};
};
#pragma pack(pop)

inline std::vector<RaceCaptureGhost> ReadRaceCaptureGhost(const std::string& path) {
    std::vector<RaceCaptureGhost> records;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return records;
    RaceCaptureGhost r;
    while (std::fread(&r, sizeof r, 1, f) == 1 && r.magic == kRaceCaptureGhostMagic) records.push_back(r);
    std::fclose(f);
    return records;
}

inline bool ReadRaceCaptureSetup(const std::string& path, RaceCaptureSetup& setup) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    const bool ok = std::fread(&setup, sizeof setup, 1, f) == 1 && setup.magic == kRaceCaptureSetupMagic;
    std::fclose(f);
    return ok;
}

inline std::vector<RaceCaptureFrame> ReadRaceCapture(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<RaceCaptureFrame> frames;
    RaceCaptureFrame r;
    while (std::fread(&r, sizeof r, 1, f) == 1) {
        if (r.magic != kRaceCaptureMagic) {
            std::fclose(f);
            throw std::runtime_error(path + ": not a race capture");
        }
        frames.push_back(r);
    }
    std::fclose(f);
    return frames;
}

} // namespace gt2
