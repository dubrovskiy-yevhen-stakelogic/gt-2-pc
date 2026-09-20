#include "game/audio/mixer.h"
#include "game/audio/pause.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace gt2::audio {
namespace {

constexpr int kFilterPos[5] = {0, 60, 115, 98, 122};
constexpr int kFilterNeg[5] = {0, 0, -52, -55, -60};
constexpr uint32_t kTickSamples = kSampleRate / 60; // the driver's 60 Hz tick (0x80079140 from the VBlank handler)

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

Mixer::Mixer() = default;

bool Mixer::Upload(uint32_t address, std::span<const uint8_t> data) {
    if ((address & 15) || address >= kSampleRamSize || data.size() > kSampleRamSize - address) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    std::memcpy(ram_.data() + address, data.data(), data.size());
    return true;
}

Mixer::Voice* Mixer::Bound(int8_t* handle) {
    if (!handle || *handle < 0 || *handle >= kVoices) return nullptr;
    Voice& v = voices_[size_t(*handle)];
    return (v.active && v.handle == handle) ? &v : nullptr;
}

int Mixer::Allocate(uint8_t priority) {
    for (int i = 0; i < kVoices; i++)
        if (!voices_[size_t(i)].active) return i;
    // 0x8007A938: a busy voice of equal or lower importance (larger priority value) is stolen; its handle is told.
    int victim = -1;
    for (int i = 0; i < kVoices; i++) {
        Voice& v = voices_[size_t(i)];
        if (v.priority < priority) continue;
        if (victim < 0 || v.priority > voices_[size_t(victim)].priority) victim = i;
    }
    if (victim >= 0 && voices_[size_t(victim)].handle) *voices_[size_t(victim)].handle = -1;
    return victim;
}

void Mixer::KeyOn(Voice& v) {
    v.currentAddress = v.startAddress;
    v.repeatAddress = v.startAddress;
    v.blockPos = 28;
    v.old = v.older = v.previous = v.current = 0;
    v.counter = 0;
    v.phase = Voice::kAttack;
    v.level = 0;
    v.envelopeCounter = 0;
    v.reachedEnd = false;
    v.durationSum = 0;
    v.tickSamples = 0;
}

void Mixer::Play(int8_t* handle, const VoiceRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int index = Allocate(request.priority);
    if (handle) *handle = int8_t(index);
    if (index < 0) return;
    Voice& v = voices_[size_t(index)];
    v.active = true;
    v.handle = handle;
    v.priority = request.priority;
    v.volumeLeft = request.volumeLeft & 0x7FFF;
    v.volumeRight = request.volumeRight & 0x7FFF;
    v.pitch = request.pitch;
    v.adsr1 = request.adsr1;
    v.adsr2 = request.adsr2;
    v.startAddress = uint32_t(request.address) * 8;
    v.durationLimit = uint32_t(request.duration) * 0x100; // 0x8007AA94: duration * the tick rate (0x100 at 60 Hz)
    v.reverb = (request.flags & 1) != 0;                  // the driver's voice + 0x24 -> SPU EON (0x1F801D98)
    KeyOn(v);
}

void Mixer::SetPriority(int8_t* handle, uint8_t priority) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* v = Bound(handle)) v->priority = priority;
}

void Mixer::SetPitch(int8_t* handle, uint16_t pitch) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* v = Bound(handle)) v->pitch = pitch;
}

void Mixer::SetVolume(int8_t* handle, uint16_t left, uint16_t right) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* v = Bound(handle)) { v->volumeLeft = left & 0x7FFF; v->volumeRight = right & 0x7FFF; }
}

void Mixer::Release(int8_t* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* v = Bound(handle))
        if (v->phase != Voice::kOff) v->phase = Voice::kRelease;
}

void Mixer::Detach(int8_t* handle) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Voice* v = Bound(handle)) { v->handle = nullptr; *handle = -1; }
}

void Mixer::Poll() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (Voice& v : voices_)
        if (!v.active && v.handle) { *v.handle = -1; v.handle = nullptr; }
}

int Mixer::ActiveVoices() const {
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const Voice& v : voices_) n += v.active ? 1 : 0;
    return n;
}

void Mixer::DecodeBlock(Voice& v) {
    const uint32_t address = v.currentAddress & (kSampleRamSize - 1) & ~15u;
    const uint8_t* block = &ram_[address];
    const int shift = block[0] & 0xF, filter = std::min((block[0] >> 4) & 0xF, 4);
    const uint8_t flags = block[1];
    for (int i = 0; i < 28; i++) v.block[i] = int16_t(DecodeNibble((block[2 + i / 2] >> ((i & 1) * 4)) & 0xF, shift, filter, v.old, v.older));
    v.blockPos = 0;
    if (flags & 4) v.repeatAddress = address;
    v.currentAddress = address + 16;
    if (flags & 1) {
        v.currentAddress = v.repeatAddress;
        if (!(flags & 2)) v.reachedEnd = true; // one-shot: silent once this block has played
    }
}

void Mixer::StepEnvelope(Voice& v) {
    int rate = 0;
    bool exponential = false, decreasing = false;
    switch (v.phase) {
    case Voice::kAttack: rate = (v.adsr1 >> 8) & 0x7F; exponential = v.adsr1 & 0x8000; break;
    case Voice::kDecay: rate = ((v.adsr1 >> 4) & 0xF) * 4; exponential = true; decreasing = true; break;
    case Voice::kSustain: rate = (v.adsr2 >> 6) & 0x7F; exponential = v.adsr2 & 0x8000; decreasing = v.adsr2 & 0x4000; break;
    case Voice::kRelease: rate = (v.adsr2 & 0x1F) * 4; exponential = v.adsr2 & 0x20; decreasing = true; break;
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
    else if (v.phase == Voice::kDecay && v.level <= int32_t(((v.adsr1 & 0xF) + 1) * 0x800)) v.phase = Voice::kSustain;
    else if (v.phase == Voice::kRelease && v.level <= 0) v.phase = Voice::kOff;
}

void Mixer::SetReverb(const ReverbSettings& settings) {
    std::lock_guard<std::mutex> lock(mutex_);
    reverb_ = Reverb{};
    reverb_.settings = settings;
    const uint32_t start = uint32_t(settings.base) * 8;
    reverb_.work.assign(start < kSampleRamSize ? (kSampleRamSize - start) / 2 : 0, 0);
    if (reverb_.work.empty()) reverb_.settings.enabled = false;
}

void Mixer::SetReverbEnabled(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    reverb_.settings.enabled = enabled && !reverb_.work.empty();
}

void Mixer::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

void Mixer::SetStream(StreamSource* source) {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_ = source;
}

// One step of the reverb unit (psx-spx "SPU Reverb Formula"), all values 16-bit samples, volumes 1.15 fixed point.
// Register addresses are in 8-byte units (4 samples); "[m - 2]" of the byte formula is the sample before m.
void Mixer::ReverbTick(int32_t inLeft, int32_t inRight) {
    Reverb& r = reverb_;
    const auto& g = r.settings.registers;
    const size_t size = r.work.size();
    auto reg = [&](int i) { return int32_t(int16_t(g[size_t(i)])); };  // volumes: signed
    auto off = [&](int i) { return size_t(g[size_t(i)]) * 4; };          // addresses: unsigned, 8-byte units
    auto at = [&](size_t offset, int back) -> int16_t& { return r.work[(r.current + offset + size - size_t(back)) % size]; };
    auto clamp = [](int32_t v) { return int16_t(std::clamp(v, -0x8000, 0x7FFF)); };
    auto mul = [](int32_t a, int32_t b) { return (a * b) >> 15; };
    enum { dAPF1, dAPF2, vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2, mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2,
           dLSAME, dRSAME, mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF, mLAPF1, mRAPF1, mLAPF2, mRAPF2, vLIN, vRIN };

    const int32_t lin = mul(clamp(inLeft), reg(vLIN)), rin = mul(clamp(inRight), reg(vRIN));
    auto reflect = [&](int32_t in, int dst, int src) {
        const int32_t previous = at(off(dst), 1);
        const int32_t t = in + mul(at(off(src), 0), reg(vWALL)) - previous;
        at(off(dst), 0) = clamp(mul(t, reg(vIIR)) + previous);
    };
    reflect(lin, mLSAME, dLSAME);
    reflect(rin, mRSAME, dRSAME);
    reflect(lin, mLDIFF, dRDIFF);
    reflect(rin, mRDIFF, dLDIFF);
    auto side = [&](int c1, int c2, int c3, int c4, int apf1, int apf2, int16_t volume) {
        int32_t out = mul(at(off(c1), 0), reg(vCOMB1)) + mul(at(off(c2), 0), reg(vCOMB2)) + mul(at(off(c3), 0), reg(vCOMB3)) + mul(at(off(c4), 0), reg(vCOMB4));
        for (int stage = 0; stage < 2; stage++) {
            const int m = stage == 0 ? apf1 : apf2;
            const int32_t v = reg(stage == 0 ? vAPF1 : vAPF2);
            const size_t delayed = (off(m) + size - (off(stage == 0 ? dAPF1 : dAPF2) % size)) % size;
            out = clamp(out - mul(v, at(delayed, 0)));
            at(off(m), 0) = int16_t(out);
            out = clamp(mul(out, v) + at(delayed, 0));
        }
        return mul(out, volume);
    };
    const int32_t outLeft = side(mLCOMB1, mLCOMB2, mLCOMB3, mLCOMB4, mLAPF1, mLAPF2, r.settings.outLeft);
    const int32_t outRight = side(mRCOMB1, mRCOMB2, mRCOMB3, mRCOMB4, mRAPF1, mRAPF2, r.settings.outRight);
    r.lastOut[0] = r.out[0];
    r.lastOut[1] = r.out[1];
    r.out[0] = float(outLeft);
    r.out[1] = float(outRight);
    r.current = (r.current + 1) % size;
}

// The unit runs at half the output rate: the input is averaged over two frames, the output interpolated between steps.
void Mixer::ReverbFrame(float inLeft, float inRight, float& outLeft, float& outRight) {
    Reverb& r = reverb_;
    if (!r.odd) {
        r.pendingIn[0] = inLeft;
        r.pendingIn[1] = inRight;
        outLeft = r.out[0];
        outRight = r.out[1];
    } else {
        ReverbTick(int32_t(std::lround((r.pendingIn[0] + inLeft) * 0.5f)), int32_t(std::lround((r.pendingIn[1] + inRight) * 0.5f)));
        outLeft = (r.lastOut[0] + r.out[0]) * 0.5f;
        outRight = (r.lastOut[1] + r.out[1]) * 0.5f;
    }
    r.odd = !r.odd;
}

void Mixer::RenderReverb(const float* input, float* output, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t f = 0; f < frames; f++) {
        if (!reverb_.settings.enabled) { output[f * 2] = output[f * 2 + 1] = 0; continue; }
        ReverbFrame(input[f * 2], input[f * 2 + 1], output[f * 2], output[f * 2 + 1]);
    }
}

void Mixer::Mix(float* output, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (paused_ || mixPauseDepth.load() != 0) return;
    const bool reverb = reverb_.settings.enabled;
    if (reverb) wet_.assign(frames * 2, 0.0f);
    for (Voice& v : voices_) {
        if (!v.active) continue;
        for (size_t f = 0; f < frames; f++) {
            if (v.phase == Voice::kOff) break;
            // The driver's tick: a voice with a duration is released once the summed pitch exceeds it.
            if (++v.tickSamples >= kTickSamples) {
                v.tickSamples = 0;
                v.durationSum += v.pitch;
                if (v.durationLimit != 0 && v.durationLimit < v.durationSum && v.phase != Voice::kRelease) v.phase = Voice::kRelease;
            }
            StepEnvelope(v);
            v.counter += std::min<uint32_t>(v.pitch, 0x4000);
            while (v.counter >= 0x1000) {
                v.counter -= 0x1000;
                if (v.blockPos >= 28) {
                    if (v.reachedEnd) { v.phase = Voice::kOff; break; }
                    DecodeBlock(v);
                }
                v.previous = v.current;
                v.current = v.block[v.blockPos++];
            }
            if (v.phase == Voice::kOff) break;
            const int32_t sample = v.previous + ((v.current - v.previous) * int32_t(v.counter) >> 12);
            const int32_t scaled = (sample * v.level) >> 15;
            // Register semantics: a 15-bit volume is the voice volume / 2 (psx-spx), so 0x3FFF is about full scale.
            const float left = float(scaled) * float(v.volumeLeft * 2) / 32768.0f, right = float(scaled) * float(v.volumeRight * 2) / 32768.0f;
            output[f * 2] += left / 32768.0f;
            output[f * 2 + 1] += right / 32768.0f;
            if (reverb && v.reverb) { wet_[f * 2] += left; wet_[f * 2 + 1] += right; }
        }
        if (v.phase == Voice::kOff) v.active = false; // the handle is cleared by Poll on the game thread
    }
    if (reverb)
        for (size_t f = 0; f < frames; f++) {
            float l, r;
            ReverbFrame(wet_[f * 2], wet_[f * 2 + 1], l, r);
            output[f * 2] += l / 32768.0f;
            output[f * 2 + 1] += r / 32768.0f;
        }
    if (stream_) stream_->MixStream(output, frames);
}

} // namespace gt2::audio
