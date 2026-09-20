#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Sound banks of GT2.VOL as the US Simulation v1.2 executable (SHA-1 3030aa27...) reads them: the engine sound
// sets engine/*.es ("ENGN", relocated by 0x80079078) and the instrument / effect banks sound/*.ins ("INST",
// relocated by 0x80078974). Both share the container header (docs/formats/sound.md):
//   +0x00 magic  +0x04 self pointer (0 on disc)  +0x08 u32 header bytes  +0x0C u32 SPU RAM address of the data
//   (0 on disc)  +0x10 u32 data bytes  +0x14 u32 header bytes (relocated as a pointer)  +0x18 u32 count of table 0
//   +0x1C u32 offset of table 0  +0x20 / +0x24 count / offset of table 1 (ENGN: a second layer set, unused on the
//   disc; INST: the program list).
// The loader 0x800680A0 reads `header bytes` into RAM and uploads the `data bytes` that follow straight into SPU
// RAM; sample offsets in the tables are relative to that data area and are patched by adding the SPU address.
namespace gt2 {

// ENGN table 0 entry (16 bytes). The engine sound player 0x80078C88 picks the layer whose fadeIn..fadeOut
// contains the rpm, or crossfades the two layers around a gap (docs/formats/sound.md, game/audio/car_sound.h).
struct EngineLayer {
    int16_t rpmPitch;      // +0  rpm at which the loop plays at `pitch`
    int16_t fadeIn;        // +2  the layer plays alone for fadeIn <= rpm <= fadeOut
    int16_t fadeOut;       // +4
    int16_t volume;        // +6  0x4000 = 1.0
    uint32_t pitch;        // +8  SPU pitch at rpmPitch (0x1000 = 44100 Hz)
    uint32_t sampleAddress; // +C  offset of the ADPCM loop in the data area (SPU address once uploaded)
};
static_assert(sizeof(EngineLayer) == 16);

struct EngineBank {
    std::vector<EngineLayer> layers;
    std::vector<uint8_t> data; // SPU-ADPCM, 16-byte blocks with loop flags
};

// INST table 0 entry (20 bytes): one sample with its playback defaults (read by 0x800784A0 / 0x800785A8).
struct InstSample {
    uint16_t address;      // +0  data offset / 8 (SPU address / 8 once uploaded; +0x8 units of 8 bytes)
    int16_t volume;        // +2  0x4000 = 1.0 (one-shot volume scale of 0x800784A0)
    uint16_t baseNote;     // +4  note << 8 | fine: the note the sample was recorded at
    uint16_t duration;     // +6  play length in the driver's units (0 = until released)
    uint8_t priority;      // +8  default voice priority
    uint8_t byte09;
    uint8_t flags;         // +A  passed to the voice (bit 0: reverb)
    uint8_t byte0B;
    uint8_t note;          // +C  note to play (pitch = 2^((note - baseNote / 256) / 12) via the table 0x8008FDB8)
    uint8_t byte0D, byte0E, byte0F;
    uint16_t adsr1;        // +10 SPU ADSR register 1
    uint16_t adsr2;        // +12 SPU ADSR register 2
};
static_assert(sizeof(InstSample) == 20);

struct InstBank {
    std::vector<InstSample> samples;
    std::vector<uint8_t> data;
};

// Throw std::runtime_error on a bad magic / inconsistent header.
EngineBank ParseEngineBank(std::span<const uint8_t> file);
InstBank ParseInstBank(std::span<const uint8_t> file);

// File names of the engine sound set `soundId` (CarConfig::engineWord = EngineRow::word0A): the intake loop set
// "engine/%05u.es" and the exhaust set "engine/%05u_n<k>.es" (k = exhaustByte & 3) or "_t<k>" for exhaustByte >= 4
// (CarConfig::exhaustByte: muffler stage + 4 with a turbo kit). The AI cars share "engine/ene_n.es" /
// "engine/ene_t.es" (0x8001882C loads them as the 3rd / 4th bank; 0x800145F4 binds them by the turbo flag).
std::string EngineSoundPath(uint32_t soundId);
std::string ExhaustSoundPath(uint32_t soundId, uint8_t exhaustByte);
inline const char* AiExhaustSoundPath(bool turbo) { return turbo ? "engine/ene_t.es" : "engine/ene_n.es"; }

} // namespace gt2
