#include "machine/spu.h"

#include <algorithm>
#include <cstring>

namespace gt2 {
namespace {

constexpr int kFilterPos[5] = {0, 60, 115, 98, 122};
constexpr int kFilterNeg[5] = {0, 0, -52, -55, -60};

constexpr uint32_t kMainVolumeLeft = 0x180, kMainVolumeRight = 0x182, kKeyOn = 0x188, kKeyOff = 0x18C, kEndx = 0x19C,
                   kIrqAddress = 0x1A4, kTransferAddress = 0x1A6, kTransferFifo = 0x1A8, kControl = 0x1AA, kStatus = 0x1AE,
                   kCdVolumeLeft = 0x1B0, kCdVolumeRight = 0x1B2;

int16_t Clamp16(int32_t v) { return int16_t(std::clamp(v, -0x8000, 0x7FFF)); }

int DecodeNibble(int nibble, int shift, int filter, int16_t& old, int16_t& older) {
    int sample = (nibble & 8) ? nibble - 16 : nibble;
    sample = shift <= 12 ? (sample << 12) >> shift : (sample << 12) >> 9;
    sample += (old * kFilterPos[filter] + older * kFilterNeg[filter] + 32) / 64;
    sample = std::clamp(sample, -0x8000, 0x7FFF);
    older = old;
    old = int16_t(sample);
    return sample;
}

} // namespace

int16_t Spu::VolumeFromRegister(uint16_t value) {
    if (value & 0x8000) return 0x3FFF; // sweep mode: approximated by a fixed level (see header)
    return int16_t(value << 1);
}

uint16_t Spu::Read(uint32_t offset) {
    offset &= 0x3FE;
    if (offset < 0x180) {
        const Voice& v = voices_[offset >> 4];
        switch (offset & 0xF) {
        case 0xC: return uint16_t(v.level);
        case 0xE: return uint16_t(v.repeatAddress / 8);
        default: break;
        }
    }
    switch (offset) {
    case kEndx: return uint16_t(endx_);
    case kEndx + 2: return uint16_t(endx_ >> 16);
    case kStatus: return uint16_t((regs_[kControl / 2] & 0x3F) | (irqFlag_ ? 0x40 : 0));
    default: return regs_[offset / 2];
    }
}

void Spu::Write(uint32_t offset, uint16_t value) {
    offset &= 0x3FE;
    regs_[offset / 2] = value;
    if (log) {
        static const char* const kVoiceRegs[8] = {"volL", "volR", "pitch", "start", "adsr1", "adsr2", "envx", "repeat"};
        if (offset < 0x180) std::fprintf(log, "%llu v%02u %s %04X\n", static_cast<unsigned long long>(logTag), unsigned(offset >> 4), kVoiceRegs[(offset & 0xF) >> 1], value);
        else if (offset == kKeyOn || offset == kKeyOn + 2 || offset == kKeyOff || offset == kKeyOff + 2)
            std::fprintf(log, "%llu %s%s %04X\n", static_cast<unsigned long long>(logTag), offset < kKeyOff ? "keyon" : "keyoff", (offset & 2) ? "-hi" : "", value);
        else if (offset == 0x184 || offset == 0x186 || offset == 0x198 || offset == 0x19A || offset == 0x1A2 || offset >= 0x1C0 || offset == kControl)
            std::fprintf(log, "%llu reg %03X %04X\n", static_cast<unsigned long long>(logTag), offset, value); // reverb / control
        else if (offset == kCdVolumeLeft || offset == kCdVolumeRight)
            std::fprintf(log, "%llu cdvol%s %04X\n", static_cast<unsigned long long>(logTag), offset == kCdVolumeLeft ? "L" : "R", value);
    }
    if (offset < 0x180) {
        Voice& v = voices_[offset >> 4];
        switch (offset & 0xF) {
        case 0x0: v.volumeLeft = VolumeFromRegister(value); break;
        case 0x2: v.volumeRight = VolumeFromRegister(value); break;
        case 0x4: v.pitch = value; break;
        case 0x6: v.startAddress = uint32_t(value) * 8; break;
        case 0x8: v.adsrLow = value; break;
        case 0xA: v.adsrHigh = value; break;
        case 0xC: v.level = value & 0x7FFF; break;
        case 0xE: v.repeatAddress = uint32_t(value) * 8; break;
        }
        return;
    }
    switch (offset) {
    case kKeyOn: for (int i = 0; i < 16; i++) if (value & (1u << i)) KeyOn(i); break;
    case kKeyOn + 2: for (int i = 0; i < 8; i++) if (value & (1u << i)) KeyOn(16 + i); break;
    case kKeyOff: for (int i = 0; i < 16; i++) if ((value & (1u << i)) && voices_[i].phase != Voice::kOff) voices_[i].phase = Voice::kRelease; break;
    case kKeyOff + 2: for (int i = 0; i < 8; i++) if ((value & (1u << i)) && voices_[16 + i].phase != Voice::kOff) voices_[16 + i].phase = Voice::kRelease; break;
    case kTransferAddress: transferAddress_ = uint32_t(value) * 8; break;
    case kTransferFifo:
        ram_[transferAddress_ & (kRamSize - 1)] = uint8_t(value);
        ram_[(transferAddress_ + 1) & (kRamSize - 1)] = uint8_t(value >> 8);
        transferAddress_ += 2;
        break;
    case kControl: if (!(value & 0x40)) irqFlag_ = false; break;
    default: break;
    }
}

void Spu::DmaWrite(const uint8_t* src, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) ram_[(transferAddress_ + i) & (kRamSize - 1)] = src[i];
    transferAddress_ += uint32_t(bytes);
}

void Spu::DmaRead(uint8_t* dst, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) dst[i] = ram_[(transferAddress_ + i) & (kRamSize - 1)];
    transferAddress_ += uint32_t(bytes);
}

void Spu::KeyOn(int index) {
    Voice& v = voices_[size_t(index)];
    v.currentAddress = v.startAddress;
    v.repeatAddress = v.startAddress;
    v.blockPos = 28;
    v.old = v.older = v.previous = v.current = 0;
    v.counter = 0;
    v.phase = Voice::kAttack;
    v.level = 0;
    v.envelopeCounter = 0;
    endx_ &= ~(1u << index);
    keyOnCount++;
}

bool Spu::DecodeBlock(Voice& v) {
    const uint32_t address = v.currentAddress & (kRamSize - 1) & ~15u;
    const uint8_t* block = &ram_[address];
    const int shift = block[0] & 0xF, filter = std::min((block[0] >> 4) & 0xF, 4);
    const uint8_t flags = block[1];
    for (int i = 0; i < 28; i++) v.block[i] = int16_t(DecodeNibble((block[2 + i / 2] >> ((i & 1) * 4)) & 0xF, shift, filter, v.old, v.older));
    v.blockPos = 0;

    const uint32_t irqAddress = uint32_t(regs_[kIrqAddress / 2]) * 8;
    const bool irq = (regs_[kControl / 2] & 0x40) && irqAddress >= address && irqAddress < address + 16;

    if (flags & 4) v.repeatAddress = address;
    v.currentAddress = address + 16;
    if (flags & 1) {
        endx_ |= 1u << (&v - voices_.data());
        v.currentAddress = v.repeatAddress;
        if (!(flags & 2)) v.reachedEnd = true; // goes silent once this block has played
    }
    return irq;
}

void Spu::StepEnvelope(Voice& v) {
    int rate = 0;
    bool exponential = false, decreasing = false;
    switch (v.phase) {
    case Voice::kAttack: rate = (v.adsrLow >> 8) & 0x7F; exponential = v.adsrLow & 0x8000; break;
    case Voice::kDecay: rate = ((v.adsrLow >> 4) & 0xF) * 4; exponential = true; decreasing = true; break;
    case Voice::kSustain: rate = (v.adsrHigh >> 6) & 0x7F; exponential = v.adsrHigh & 0x8000; decreasing = v.adsrHigh & 0x4000; break;
    case Voice::kRelease: rate = (v.adsrHigh & 0x1F) * 4; exponential = v.adsrHigh & 0x20; decreasing = true; break;
    default: return;
    }
    int step = decreasing ? -8 + (rate & 3) : 7 - (rate & 3);
    const int shift = rate >> 2;
    uint32_t cycles = 1u << std::max(0, shift - 11);
    step <<= std::max(0, 11 - shift);
    if (exponential && !decreasing && v.level > 0x6000) cycles *= 4;
    if (exponential && decreasing) step = (step * v.level) >> 15;

    if (++v.envelopeCounter < cycles) return;
    v.envelopeCounter = 0;
    v.level = std::clamp(v.level + step, 0, 0x7FFF);

    if (v.phase == Voice::kAttack && v.level >= 0x7FFF) v.phase = Voice::kDecay;
    else if (v.phase == Voice::kDecay && v.level <= int32_t(((v.adsrLow & 0xF) + 1) * 0x800)) v.phase = Voice::kSustain;
    else if (v.phase == Voice::kRelease && v.level <= 0) v.phase = Voice::kOff;
}

bool Spu::Generate(size_t frames) {
    bool irq = false;
    const uint16_t control = regs_[kControl / 2];
    const int16_t mainLeft = VolumeFromRegister(regs_[kMainVolumeLeft / 2]), mainRight = VolumeFromRegister(regs_[kMainVolumeRight / 2]);
    const int16_t cdLeft = int16_t(regs_[kCdVolumeLeft / 2]), cdRight = int16_t(regs_[kCdVolumeRight / 2]);

    for (size_t f = 0; f < frames; f++) {
        int32_t left = 0, right = 0;
        if (control & 0x8000) {
            for (Voice& v : voices_) {
                if (v.phase == Voice::kOff) continue;
                StepEnvelope(v);
                v.counter += std::min<uint32_t>(v.pitch, 0x4000);
                while (v.counter >= 0x1000) {
                    v.counter -= 0x1000;
                    if (v.blockPos >= 28) {
                        if (v.reachedEnd) { v.phase = Voice::kOff; v.level = 0; v.reachedEnd = false; break; }
                        irq |= DecodeBlock(v);
                    }
                    v.previous = v.current;
                    v.current = v.block[v.blockPos++];
                }
                if (v.phase == Voice::kOff) continue;
                const int32_t sample = v.previous + ((v.current - v.previous) * int32_t(v.counter) >> 12);
                const int32_t scaled = (sample * v.level) >> 15;
                left += (scaled * v.volumeLeft) >> 15;
                right += (scaled * v.volumeRight) >> 15;
            }
        }
        if ((control & 1) && cdAudio_.size() >= 2) {
            left += (cdAudio_[0] * cdLeft) >> 15;
            right += (cdAudio_[1] * cdRight) >> 15;
            cdAudio_.pop_front();
            cdAudio_.pop_front();
        }
        left = (left * mainLeft) >> 15;
        right = (right * mainRight) >> 15;
        if (!(control & 0x4000)) left = right = 0; // mute
        output.push_back(Clamp16(left));
        output.push_back(Clamp16(right));
    }
    if (irq) irqFlag_ = true;
    return irq;
}

void Spu::PushXaSector(const uint8_t* sector) {
    const uint8_t coding = sector[19];
    if (coding & 0x10) return; // 8-bit XA is not used by this game
    const bool stereo = coding & 1;
    const double ratio = ((coding & 4) ? 18900.0 : 37800.0) / kSampleRate;
    xaSectors++;

    auto emit = [&](int16_t l, int16_t r) {
        while (xaPhase_ < 1.0) {
            cdAudio_.push_back(int16_t(xaLast_[0] + (l - xaLast_[0]) * xaPhase_));
            cdAudio_.push_back(int16_t(xaLast_[1] + (r - xaLast_[1]) * xaPhase_));
            xaPhase_ += ratio;
        }
        xaPhase_ -= 1.0;
        xaLast_[0] = l;
        xaLast_[1] = r;
    };

    const uint8_t* data = sector + 24;
    for (int group = 0; group < 18; group++) {
        const uint8_t* base = data + group * 128;
        for (int unit = 0; unit < 8; unit += stereo ? 2 : 1) {
            int16_t decoded[2][28];
            for (int ch = 0; ch < (stereo ? 2 : 1); ch++) {
                const int u = unit + ch;
                const int shift = base[4 + u] & 0xF, filter = std::min((base[4 + u] >> 4) & 3, 3);
                for (int row = 0; row < 28; row++)
                    decoded[ch][row] = int16_t(DecodeNibble((base[16 + row * 4 + u / 2] >> ((u & 1) * 4)) & 0xF, shift, filter, xaOld_[ch], xaOlder_[ch]));
            }
            for (int row = 0; row < 28; row++) emit(decoded[0][row], decoded[stereo ? 1 : 0][row]);
        }
    }
    while (cdAudio_.size() > size_t(kSampleRate) * 2 * 2) cdAudio_.pop_front(); // never buffer more than 2 s
}

} // namespace gt2
