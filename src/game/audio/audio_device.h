#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "game/audio/mixer.h"

// The sound device of the game: 44.1 kHz stereo 16-bit output of the mixer, a ring of small buffers (10 ms) refilled
// by a device thread, so the game thread only programs voices. Optional: a game without a sound device runs on.
//
// One implementation per platform behind this interface - audio_device_win32.cpp (waveOut) today; the Quest build
// adds audio_device_aaudio.cpp. The private members below are the platform file's own state.
namespace gt2::audio {

class AudioDevice {
public:
    AudioDevice() = default;
    ~AudioDevice() { Close(); }
    AudioDevice(const AudioDevice&) = delete;
    AudioDevice& operator=(const AudioDevice&) = delete;

    // Opens the default device; false (with `error` set) when there is none.
    bool Open(Mixer& mixer, std::string& error);
    void Close();
    bool IsOpen() const { return running_.load(); }
    // Dev aid: every buffer sent to the device is also written to a 44.1 kHz stereo WAV at `path` (call before Open;
    // the file is finalised by Close).
    bool Record(const std::string& path);

private:
    void Run(); // the device thread of the Windows implementation (AAudio calls back on its own)
#if defined(__ANDROID__)
    // The AAudio data callback of audio_device_aaudio.cpp (AAudio's own thread); returns aaudio_data_callback_result_t.
    int Callback(void* audioData, int32_t frames);
#endif
    std::FILE* record_ = nullptr;
    uint32_t recordedFrames_ = 0;

    Mixer* mixer_ = nullptr;
    std::atomic<bool> running_{false};
    std::thread thread_;
    void* waveOut_ = nullptr; // HWAVEOUT
    [[maybe_unused]] void* event_ = nullptr;   // HANDLE
    struct Buffer;
    std::vector<Buffer*> buffers_;
};

} // namespace gt2::audio
