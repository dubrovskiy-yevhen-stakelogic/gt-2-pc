// The Android implementation (AAudio) of the AudioDevice of audio_device.h (docs/research/vr_port_plan.md, M6;
// model: C:\Dev\harry-potter-vr\android\app\src\main\cpp\quest_audio.cpp).
//
// AAudio calls back on its own high-priority thread, so this device has no thread of its own: the callback runs the
// mixer for exactly the block AAudio asks for. The mixer produces 44 100 Hz stereo float (game/audio/mixer.h); the
// stream is opened at that rate, and when the device insists on another one (the Quest's own mix runs at 48 kHz) the
// callback resamples linearly from a 44.1 kHz block it mixes itself, instead of pretending the rates match.
//
// The platform's state lives in AudioDevice::Buffer, which audio_device.h leaves to the platform file; `waveOut_`
// holds the AAudioStream. The device thread, its event and the buffer ring of the Windows file are unused here.
#include "game/audio/audio_device.h"
#include "game/audio/audio_resampler.h"

#include <aaudio/AAudio.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace gt2::audio {

// Everything this implementation needs besides the mixer: the mixed 44.1 kHz block and the resampler's position.
struct AudioDevice::Buffer {
    int32_t streamRate = kSampleRate;       // what the stream really runs at
    std::vector<float> mixed;               // the 44.1 kHz block of this callback (interleaved stereo)
    double position = 0.0;                  // fractional source frame inside `mixed`
};

namespace {
constexpr int32_t kBufferFrames = 441; // 10 ms at 44.1 kHz: the size the callback is asked to fill, nominally
}

namespace {

// Fills `out` (interleaved stereo int16) with `frames` frames of the mixer's output, resampling when the stream does
// not run at the mixer's rate.

} // namespace

bool AudioDevice::Open(Mixer& mixer, std::string& error) {
    Close();
    mixer_ = &mixer;
    Buffer* b = new Buffer;
    buffers_.push_back(b);

    AAudioStreamBuilder* builder = nullptr;
    if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
        error = "AAudio_createStreamBuilder failed";
        Close();
        return false;
    }
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSampleRate(builder, kSampleRate);
    AAudioStreamBuilder_setChannelCount(builder, 2);
    AAudioStreamBuilder_setFramesPerDataCallback(builder, kBufferFrames);
    AAudioStreamBuilder_setDataCallback(
        builder,
        [](AAudioStream*, void* user, void* audioData, int32_t frames) -> aaudio_data_callback_result_t {
            AudioDevice* self = static_cast<AudioDevice*>(user);
            if (!self || !audioData || frames <= 0) return AAUDIO_CALLBACK_RESULT_STOP;
            return self->Callback(audioData, frames);
        },
        this);
    AAudioStream* stream = nullptr;
    const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream);
    AAudioStreamBuilder_delete(builder);
    if (opened != AAUDIO_OK || !stream) {
        error = std::string("AAudioStreamBuilder_openStream failed (") + AAudio_convertResultToText(opened) + ")";
        Close();
        return false;
    }
    if (AAudioStream_getFormat(stream) != AAUDIO_FORMAT_PCM_I16 || AAudioStream_getChannelCount(stream) != 2) {
        AAudioStream_close(stream);
        error = "the AAudio stream is not 16-bit stereo";
        Close();
        return false;
    }
    b->streamRate = AAudioStream_getSampleRate(stream);
    if (b->streamRate <= 0) {
        AAudioStream_close(stream);
        error = "the AAudio stream reports no sample rate";
        Close();
        return false;
    }
    waveOut_ = stream;
    running_.store(true);
    const aaudio_result_t started = AAudioStream_requestStart(stream);
    if (started != AAUDIO_OK) {
        error = std::string("AAudioStream_requestStart failed (") + AAudio_convertResultToText(started) + ")";
        Close();
        return false;
    }
    std::printf("audio: AAudio %d Hz stereo (the mixer runs at %d Hz%s)\n", b->streamRate, int(kSampleRate),
                b->streamRate == kSampleRate ? "" : ", resampled in the callback");
    return true;
}

void AudioDevice::Close() {
    running_.store(false);
    if (waveOut_) {
        AAudioStream* stream = static_cast<AAudioStream*>(waveOut_);
        AAudioStream_requestStop(stream);
        std::printf("audio: AAudio underruns=%d\n", AAudioStream_getXRunCount(stream));
        AAudioStream_close(stream); // waits for the callback to return
        waveOut_ = nullptr;
    }
    const bool wasOpen = mixer_ != nullptr;
    mixer_ = nullptr;
    for (Buffer* b : buffers_) delete b;
    buffers_.clear();
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

// The AAudio callback (its own thread). `Run` is the Windows device thread and is not used here.
void AudioDevice::Run() {}

int AudioDevice::Callback(void* audioData, int32_t frames) {
    if (!running_.load() || !mixer_ || buffers_.empty()) return AAUDIO_CALLBACK_RESULT_STOP;
    Buffer& b = *buffers_[0];
    int16_t* out = static_cast<int16_t*>(audioData);
    ResampleAudio(*mixer_, b, out, frames);
    if (record_) {
        std::fwrite(out, sizeof(int16_t), size_t(frames) * 2, record_);
        recordedFrames_ += uint32_t(frames);
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

} // namespace gt2::audio
