#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

// The native voice mixer of the game: a software SPU in the semantics the original's sound driver
// (0x80079DBC..0x8007AA94, docs/formats/sound.md) programs the hardware with. Voices play SPU-ADPCM from a
// 512 KB "sample RAM" (the banks are uploaded there exactly as the original uploads them to SPU RAM, so the
// addresses in the bank tables keep their meaning), with the SPU's pitch (0x1000 = 44100 Hz), 15-bit volumes
// (0x3FFF = the maximum the driver uses), the ADSR envelope of the voice registers and the driver's duration
// counter (0x8007916C: the sum of the pitch per 60 Hz tick against duration * 0x100). Own code written from
// psx-spx; no bit-exactness with the hardware is claimed (linear interpolation; the reverb unit runs the public
// algorithm at 22050 Hz but averages / holds instead of the hardware's 39-tap half-band filters).
// Output: 44.1 kHz float stereo. Thread-safe: the game thread programs voices, the device thread calls Mix.
namespace gt2::audio {

constexpr uint32_t kSampleRamSize = 512 * 1024;
constexpr int kSampleRate = 44100;
constexpr int kVoices = 48;

// The 16-byte voice descriptor the driver's Play (0x8007A59C) takes.
struct VoiceRequest {
    uint16_t volumeLeft = 0, volumeRight = 0; // +0, +2   0..0x3FFF
    uint16_t pitch = 0x1000;                  // +4       SPU pitch
    uint16_t address = 0;                     // +6       start address / 8
    uint16_t adsr1 = 0x000F, adsr2 = 0x0003;  // +8, +A   SPU ADSR registers
    uint16_t duration = 0;                    // +C       0 = until released
    uint8_t priority = 0;                     // +E       lower = more important (voice stealing)
    uint8_t flags = 0;                        // +F       bit 0: reverb (the voice feeds the reverb unit, SPU EON)
};

// The SPU reverb unit's settings: the 32 registers 0x1F801DC0..0x1F801DFE (dAPF1, dAPF2, vIIR, vCOMB1..4, vWALL,
// vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3,
// mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN), the work area start 0x1F801DA2
// (mBASE, / 8) and the output volumes 0x1F801D84 / 0x1F801D86 (vLOUT, vROUT). The game's boot (EXE 0x80010E14)
// programs preset 4 through 0x80079EF8 and depth 0x0FFF through 0x8007A104 (docs/formats/sound.md section 7).
struct ReverbSettings {
    std::array<uint16_t, 32> registers{};
    uint16_t base = 0;              // mBASE: the work area is [base * 8, 0x80000) of SPU RAM
    int16_t outLeft = 0, outRight = 0;
    bool enabled = false;           // SPU control bit 7
};

// A second input of the mix (the CD / XA audio input of the SPU): called from the device thread inside Mix; adds
// `frames` stereo frames into `output`.
class StreamSource {
public:
    virtual ~StreamSource() = default;
    virtual void MixStream(float* output, size_t frames) = 0;
};

class Mixer {
public:
    Mixer();

    // Copies `data` to sample RAM at `address` (16-byte aligned); returns false when it does not fit.
    bool Upload(uint32_t address, std::span<const uint8_t> data);

    // Voice API in the driver's semantics (car_sound.h VoiceDriver). `handle` (may be null) receives the voice
    // index or -1; a voice that ends by itself or is stolen writes -1 into its handle at the next Poll.
    void Play(int8_t* handle, const VoiceRequest& request);
    void SetPriority(int8_t* handle, uint8_t priority);
    void SetPitch(int8_t* handle, uint16_t pitch);
    void SetVolume(int8_t* handle, uint16_t left, uint16_t right);
    void Release(int8_t* handle);
    void Detach(int8_t* handle);
    // Game thread, once per frame: clears the handles of the voices that ended.
    void Poll();

    // Device thread: `frames` stereo frames of float output (interleaved), added to zeroed buffers.
    void Mix(float* output, size_t frames);

    int ActiveVoices() const;

    // The reverb unit (disabled until set); `false` in SetReverbEnabled mutes it without clearing its work area.
    void SetReverb(const ReverbSettings& settings);
    void SetReverbEnabled(bool enabled);
    // The CD input (the race music); null detaches it. The source must outlive the attachment.
    void SetStream(StreamSource* source);
    // Freeze voices, reverb and attached music together; the output buffer stays silent.
    void SetPaused(bool paused);

    // Offline / test: runs the reverb unit alone on `frames` stereo frames of 16-bit-scale input (voice outputs
    // after their volumes, as the EON voices would send them) and returns its output (16-bit scale, before any
    // other mixing). Uses and advances the same state as Mix.
    void RenderReverb(const float* input, float* output, size_t frames);

private:
    struct Voice {
        bool active = false;
        int8_t* handle = nullptr;
        uint8_t priority = 0;
        uint16_t pitch = 0, adsr1 = 0, adsr2 = 0;
        uint16_t volumeLeft = 0, volumeRight = 0;
        uint32_t startAddress = 0, repeatAddress = 0, currentAddress = 0;
        int16_t block[28] = {};
        int blockPos = 28;
        int16_t old = 0, older = 0, previous = 0, current = 0;
        uint32_t counter = 0;
        enum Phase { kOff, kAttack, kDecay, kSustain, kRelease } phase = kOff;
        int32_t level = 0;
        uint32_t envelopeCounter = 0;
        bool reachedEnd = false;
        uint32_t durationLimit = 0, durationSum = 0; // 0x8007916C: sum of the pitch per tick against duration * 0x100
        uint32_t tickSamples = 0;
        bool reverb = false; // VoiceRequest::flags bit 0
    };

    int Allocate(uint8_t priority);
    Voice* Bound(int8_t* handle);
    void KeyOn(Voice& v);
    void DecodeBlock(Voice& v);
    void StepEnvelope(Voice& v);

    // The reverb unit at its 22050 Hz rate (every second output frame); input / output in 16-bit sample units.
    struct Reverb {
        ReverbSettings settings;
        std::vector<int16_t> work;  // the work area [base * 8, 0x80000) as 16-bit samples
        size_t current = 0;         // the buffer address, relative to the work area (in samples)
        float pendingIn[2] = {0, 0};
        float lastOut[2] = {0, 0}, out[2] = {0, 0};
        bool odd = false;
    };
    void ReverbFrame(float inLeft, float inRight, float& outLeft, float& outRight);
    void ReverbTick(int32_t inLeft, int32_t inRight);

    std::array<uint8_t, kSampleRamSize> ram_{};
    std::array<Voice, kVoices> voices_;
    Reverb reverb_;
    std::vector<float> wet_; // the EON voices' sum of one Mix call
    StreamSource* stream_ = nullptr;
    bool paused_ = false;
    mutable std::mutex mutex_;
};

} // namespace gt2::audio
