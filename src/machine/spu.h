#pragma once
#include <array>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <vector>

namespace gt2 {

// PS1 sound processor at the register level (0x1F801C00-0x1F801FFF), driven by the game's own sound
// driver. Written from psx-spx. Deviations from hardware, to be revisited against an emulator dump
// : linear instead of Gaussian interpolation, no reverb, no noise, no pitch
// modulation, volume sweeps treated as fixed levels, XA resampled linearly.
class Spu {
public:
    static constexpr uint32_t kRamSize = 512 * 1024;
    static constexpr int kSampleRate = 44100;

    Spu() : ram_(kRamSize, 0) {}

    uint16_t Read(uint32_t offset);              // offset from 0x1F801C00, even
    void Write(uint32_t offset, uint16_t value);
    void DmaWrite(const uint8_t* src, size_t bytes);
    void DmaRead(uint8_t* dst, size_t bytes);

    // Produces `frames` stereo frames into `output`. Returns true if the SPU IRQ fired.
    bool Generate(size_t frames);

    // One raw 2352-byte XA-ADPCM sector from the CD-ROM.
    void PushXaSector(const uint8_t* sector);

    std::vector<int16_t> output; // interleaved L/R, drained by the host
    uint64_t keyOnCount = 0, xaSectors = 0;
    // Dev aid (gt2play --spu-log): every voice register write and key on / off is printed as
    // "<tag> v<voice> <reg> <value>" / "<tag> keyon|keyoff <mask>"; `logTag` is set by the host (the field number).
    std::FILE* log = nullptr;
    uint64_t logTag = 0;

private:
    struct Voice {
        int16_t volumeLeft = 0, volumeRight = 0;
        uint16_t pitch = 0, adsrLow = 0, adsrHigh = 0;
        uint32_t startAddress = 0, repeatAddress = 0, currentAddress = 0;
        int16_t block[28] = {};
        int blockPos = 28;
        int16_t old = 0, older = 0, previous = 0, current = 0;
        uint32_t counter = 0;
        enum Phase { kOff, kAttack, kDecay, kSustain, kRelease } phase = kOff;
        int32_t level = 0;
        uint32_t envelopeCounter = 0;
        bool reachedEnd = false;
    };

    void KeyOn(int v);
    bool DecodeBlock(Voice& voice); // true when the block address hit the IRQ address
    void StepEnvelope(Voice& voice);
    static int16_t VolumeFromRegister(uint16_t value);

    std::vector<uint8_t> ram_;
    std::array<Voice, 24> voices_;
    std::array<uint16_t, 0x200> regs_{}; // raw register file (word index)
    uint32_t transferAddress_ = 0;
    uint32_t endx_ = 0;
    bool irqFlag_ = false;

    // XA / CD audio input, already at 44.1 kHz.
    std::deque<int16_t> cdAudio_;
    int16_t xaOld_[2] = {}, xaOlder_[2] = {};
    double xaPhase_ = 0;
    int16_t xaLast_[2] = {};
};

} // namespace gt2
