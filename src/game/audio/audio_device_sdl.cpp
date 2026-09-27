#include "game/audio/audio_device.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>

namespace gt2::audio {
struct AudioDevice::Buffer { SDL_AudioDeviceID device = 0; };
namespace {
constexpr size_t kFrames = 441;
constexpr Uint32 kQueueBytes = Uint32(kFrames * 2 * sizeof(int16_t) * 4); // 40 ms
}
bool AudioDevice::Open(Mixer& mixer, std::string& error) {
    Close();
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { error = SDL_GetError(); return false; }
    SDL_AudioSpec desired{};
    desired.freq = kSampleRate;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 512;
    // SDL converts to the hardware format if necessary; the game stays at 44.1 kHz.
    const auto device = SDL_OpenAudioDevice(nullptr, 0, &desired, nullptr, 0);
    if (!device) { error = SDL_GetError(); SDL_QuitSubSystem(SDL_INIT_AUDIO); return false; }
    try {
        buffers_.push_back(new Buffer{device});
        mixer_ = &mixer;
        running_ = true;
        thread_ = std::thread([this] { Run(); });
    } catch (...) {
        running_ = false;
        for (auto* buffer : buffers_) delete buffer;
        buffers_.clear(); mixer_ = nullptr;
        SDL_CloseAudioDevice(device); SDL_QuitSubSystem(SDL_INIT_AUDIO);
        throw;
    }
    SDL_PauseAudioDevice(device, 0);
    std::printf("audio: SDL %s, %d Hz stereo\n", SDL_GetCurrentAudioDriver(), kSampleRate);
    return true;
}
void AudioDevice::Run() {
    const auto device = buffers_.front()->device;
    std::vector<float> mix(kFrames * 2);
    std::vector<int16_t> samples(kFrames * 2);
    while (running_.load()) {
        if (SDL_GetQueuedAudioSize(device) >= kQueueBytes) { SDL_Delay(2); continue; }
        std::fill(mix.begin(), mix.end(), 0.0f);
        mixer_->Mix(mix.data(), kFrames);
        for (size_t i = 0; i < mix.size(); ++i) samples[i] = int16_t(std::lround(std::clamp(mix[i], -1.0f, 1.0f) * 32767.0f));
        if (SDL_QueueAudio(device, samples.data(), Uint32(samples.size() * sizeof(int16_t))) != 0) {
            std::fprintf(stderr, "audio: SDL queue failed: %s\n", SDL_GetError());
            running_ = false;
            break;
        }
        if (record_) {
            std::fwrite(samples.data(), sizeof(int16_t), samples.size(), record_);
            recordedFrames_ += uint32_t(kFrames);
        }
    }
}
void AudioDevice::Close() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (!buffers_.empty()) {
        SDL_CloseAudioDevice(buffers_.front()->device);
        for (auto* buffer : buffers_) delete buffer;
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
