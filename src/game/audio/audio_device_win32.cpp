#include "game/audio/audio_device.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>

namespace gt2::audio {

struct AudioDevice::Buffer {
    WAVEHDR header{};
    std::vector<int16_t> samples;
};

namespace {
constexpr size_t kFramesPerBuffer = 441; // 10 ms
// Streaming endpoints can return completed buffers in 50 ms batches. Keep more than one such batch queued;
// a 40 ms queue underruns between callbacks and makes audio-clocked movies run slower than real time.
constexpr size_t kBufferCount = 8;
} // namespace

bool AudioDevice::Open(Mixer& mixer, std::string& error) {
    Close();
    mixer_ = &mixer;
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 2;
    format.nSamplesPerSec = kSampleRate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = 4;
    format.nAvgBytesPerSec = format.nSamplesPerSec * 4;
    HANDLE event = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!event) { error = "CreateEvent failed"; return false; }
    HWAVEOUT out = nullptr;
    const MMRESULT r = waveOutOpen(&out, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event), 0, CALLBACK_EVENT);
    if (r != MMSYSERR_NOERROR) {
        CloseHandle(event);
        error = "waveOutOpen failed (" + std::to_string(r) + ")";
        return false;
    }
    waveOut_ = out;
    event_ = event;
    UINT deviceId = 0;
    WAVEOUTCAPSA caps{};
    const bool named = waveOutGetID(out, &deviceId) == MMSYSERR_NOERROR &&
                       waveOutGetDevCapsA(deviceId, &caps, sizeof(caps)) == MMSYSERR_NOERROR;
    std::printf("audio: waveOut %s, %d Hz, %zu buffers x %zu frames (%zu ms)\n",
                named ? caps.szPname : "default device", kSampleRate, kBufferCount, kFramesPerBuffer,
                kBufferCount * kFramesPerBuffer * 1000 / kSampleRate);
    for (size_t i = 0; i < kBufferCount; i++) {
        Buffer* b = new Buffer;
        b->samples.assign(kFramesPerBuffer * 2, 0);
        b->header.lpData = reinterpret_cast<LPSTR>(b->samples.data());
        b->header.dwBufferLength = DWORD(b->samples.size() * sizeof(int16_t));
        waveOutPrepareHeader(out, &b->header, sizeof(WAVEHDR));
        b->header.dwFlags |= WHDR_DONE; // free to fill
        buffers_.push_back(b);
    }
    running_.store(true);
    thread_ = std::thread([this] { Run(); });
    return true;
}

void AudioDevice::Close() {
    if (running_.exchange(false)) {
        SetEvent(static_cast<HANDLE>(event_));
        if (thread_.joinable()) thread_.join();
    }
    if (waveOut_) {
        HWAVEOUT out = static_cast<HWAVEOUT>(waveOut_);
        waveOutReset(out);
        for (Buffer* b : buffers_) {
            waveOutUnprepareHeader(out, &b->header, sizeof(WAVEHDR));
            delete b;
        }
        buffers_.clear();
        waveOutClose(out);
        waveOut_ = nullptr;
    }
    if (event_) { CloseHandle(static_cast<HANDLE>(event_)); event_ = nullptr; }
    const bool wasOpen = mixer_ != nullptr;
    mixer_ = nullptr;
    if (record_ && wasOpen) { // patch the RIFF / data sizes of the header written by Record
        const uint32_t dataBytes = recordedFrames_ * 4, riffBytes = 36 + dataBytes;
        std::fseek(record_, 4, SEEK_SET);
        std::fwrite(&riffBytes, 4, 1, record_);
        std::fseek(record_, 40, SEEK_SET);
        std::fwrite(&dataBytes, 4, 1, record_);
        std::fclose(record_);
        record_ = nullptr;
    }
}

bool AudioDevice::Record(const std::string& path) {
    if (running_.load() || record_) return false;
    record_ = std::fopen(path.c_str(), "wb");
    if (!record_) return false;
    recordedFrames_ = 0;
    const uint32_t zero = 0, fmtBytes = 16, rate = uint32_t(kSampleRate), byteRate = rate * 4;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, record_);
    std::fwrite(&zero, 4, 1, record_);
    std::fwrite("WAVEfmt ", 1, 8, record_);
    std::fwrite(&fmtBytes, 4, 1, record_);
    std::fwrite(&pcm, 2, 1, record_);
    std::fwrite(&channels, 2, 1, record_);
    std::fwrite(&rate, 4, 1, record_);
    std::fwrite(&byteRate, 4, 1, record_);
    std::fwrite(&align, 2, 1, record_);
    std::fwrite(&bits, 2, 1, record_);
    std::fwrite("data", 1, 4, record_);
    std::fwrite(&zero, 4, 1, record_);
    return true;
}

void AudioDevice::Run() {
    std::vector<float> mix(kFramesPerBuffer * 2);
    HWAVEOUT out = static_cast<HWAVEOUT>(waveOut_);
    while (running_.load()) {
        bool filled = false;
        for (Buffer* b : buffers_) {
            if (!(b->header.dwFlags & WHDR_DONE)) continue;
            std::fill(mix.begin(), mix.end(), 0.0f);
            mixer_->Mix(mix.data(), kFramesPerBuffer);
            for (size_t i = 0; i < mix.size(); i++) b->samples[i] = int16_t(std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f));
            if (record_) {
                std::fwrite(b->samples.data(), sizeof(int16_t), b->samples.size(), record_);
                recordedFrames_ += uint32_t(kFramesPerBuffer);
            }
            b->header.dwFlags &= ~WHDR_DONE;
            waveOutWrite(out, &b->header, sizeof(WAVEHDR));
            filled = true;
        }
        if (!filled) WaitForSingleObject(static_cast<HANDLE>(event_), 20);
    }
}

} // namespace gt2::audio
