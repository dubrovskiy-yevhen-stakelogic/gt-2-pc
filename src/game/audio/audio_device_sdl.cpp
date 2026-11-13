#include "game/audio/audio_device.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>

namespace gt2::audio {
struct AudioDevice::Buffer {
    SDL_AudioDeviceID device = 0;
    SDL_AudioStream* resample = nullptr; // mixer 44.1 kHz -> the device's own rate
};
namespace {
constexpr size_t kFrames = 441;
constexpr Uint32 kQueueBytes = Uint32(kFrames * 2 * sizeof(int16_t) * 4); // 40 ms
}
bool AudioDevice::Open(Mixer& mixer, std::string& error) {
    Close();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { error = SDL_GetError(); return false; }
    SDL_AudioSpec deviceInfo{};
    int deviceRate = SDL_GetDefaultAudioInfo(nullptr, &deviceInfo, 0) == 0 && deviceInfo.freq > 0 ? deviceInfo.freq : kSampleRate;
    // Dev aid: the dummy driver of the tests has no default rate, so force one to exercise the resampling path.
    if (const char* forced = SDL_getenv("GT2_AUDIO_DEVICE_RATE")) {
        const int rate = SDL_atoi(forced);
        if (rate > 0) deviceRate = rate;
    }
    SDL_AudioSpec desired{};
    desired.freq = deviceRate;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 512;
    // Ask for the device's own rate: forcing 44.1 kHz makes CoreAudio resample every stream and is part of the
    // macOS 26 "audio silenced until the output device is switched" bug. The mixer still runs at 44.1 kHz.
    SDL_AudioSpec obtained{};
    const auto device = SDL_OpenAudioDevice(nullptr, 0, &desired, &obtained, 0);
    if (!device) { error = SDL_GetError(); SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }
    SDL_AudioStream* resample = nullptr;
    if (obtained.freq != kSampleRate) {
        resample = SDL_NewAudioStream(AUDIO_S16SYS, 2, kSampleRate, obtained.format, obtained.channels, obtained.freq);
        if (!resample) {
            error = SDL_GetError();
            SDL_CloseAudioDevice(device); SDL_QuitSubSystem(SDL_INIT_AUDIO);
            return false;
        }
    }
    try {
        buffers_.push_back(new Buffer{device, resample});
        mixer_ = &mixer;
        running_ = true;
        thread_ = std::thread([this] { Run(); });
    } catch (...) {
        running_ = false;
        if (resample) SDL_FreeAudioStream(resample);
        for (auto* buffer : buffers_) delete buffer;
        buffers_.clear(); mixer_ = nullptr;
        SDL_CloseAudioDevice(device); SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw;
    }
    SDL_PauseAudioDevice(device, 0);
    std::printf("audio: SDL %s, %d Hz stereo (device %d Hz)\n", SDL_GetCurrentAudioDriver(), kSampleRate, obtained.freq);
    return true;
}
void AudioDevice::Run() {
    const auto device = buffers_.front()->device;
    SDL_AudioStream* const resample = buffers_.front()->resample;
    std::vector<float> mix(kFrames * 2);
    std::vector<int16_t> samples(kFrames * 2);
    std::vector<uint8_t> converted;
    while (running_.load()) {
        if (SDL_GetQueuedAudioSize(device) >= kQueueBytes) { SDL_Delay(2); continue; }
        std::fill(mix.begin(), mix.end(), 0.0f);
        mixer_->Mix(mix.data(), kFrames);
        for (size_t i = 0; i < mix.size(); ++i) samples[i] = int16_t(std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f));
        if (record_) {
            std::fwrite(samples.data(), sizeof(int16_t), samples.size(), record_);
            recordedFrames_ += uint32_t(kFrames);
        }
        int queued = 0;
        if (resample) {
            if (SDL_AudioStreamPut(resample, samples.data(), int(samples.size() * sizeof(int16_t))) != 0) {
                queued = -1;
            } else {
                const int available = SDL_AudioStreamAvailable(resample);
                if (available > 0) {
                    converted.resize(size_t(available));
                    const int got = SDL_AudioStreamGet(resample, converted.data(), available);
                    if (got > 0 && SDL_QueueAudio(device, converted.data(), Uint32(got)) != 0) queued = -1;
                }
            }
        } else if (SDL_QueueAudio(device, samples.data(), Uint32(samples.size() * sizeof(int16_t))) != 0) {
            queued = -1;
        }
        if (queued != 0) {
            std::fprintf(stderr, "audio: SDL queue failed: %s\n", SDL_GetError());
            running_ = false;
            break;
        }
    }
}
void AudioDevice::Close() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (!buffers_.empty()) {
        SDL_CloseAudioDevice(buffers_.front()->device);
        for (auto* buffer : buffers_) {
            if (buffer->resample) SDL_FreeAudioStream(buffer->resample);
            delete buffer;
        }
        buffers_.clear();
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
    if (record_ && mixer_) {
        const uint32_t dataBytes = recordedFrames_ * 4, riffBytes = 36 + dataBytes;
        std::fseek(record_, 4, SEEK_SET); std::fwrite(&riffBytes, 4, 1, record_);
        std::fseek(record_, 40, SEEK_SET); std::fwrite(&dataBytes, 4, 1, record_);
        std::fclose(record_); record_ = nullptr;
    }
    mixer_ = nullptr;
}
bool AudioDevice::Record(const std::string& path) {
    if (running_.load() || record_) return false;
    record_ = std::fopen(path.c_str(), "wb");
    if (!record_) return false;
    recordedFrames_ = 0;
    const uint32_t zero = 0, fmtBytes = 16, rate = uint32_t(kSampleRate), byteRate = rate * 4;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, record_); std::fwrite(&zero, 4, 1, record_);
    std::fwrite("WAVEfmt ", 1, 8, record_); std::fwrite(&fmtBytes, 4, 1, record_);
    std::fwrite(&pcm, 2, 1, record_); std::fwrite(&channels, 2, 1, record_);
    std::fwrite(&rate, 4, 1, record_); std::fwrite(&byteRate, 4, 1, record_);
    std::fwrite(&align, 2, 1, record_); std::fwrite(&bits, 2, 1, record_);
    std::fwrite("data", 1, 4, record_); std::fwrite(&zero, 4, 1, record_);
    return true;
}
} // namespace gt2::audio
