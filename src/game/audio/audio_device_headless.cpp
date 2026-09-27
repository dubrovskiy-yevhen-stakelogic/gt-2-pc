#include "game/audio/audio_device.h"

namespace gt2::audio {
// Shared data helpers link RaceAudio on headless platforms. Report unavailable
// playback explicitly instead of opening hardware or pretending to play sound.
bool AudioDevice::Open(Mixer&, std::string& error) {
    error = "Audio playback is unavailable in this headless build";
    return false;
}
void AudioDevice::Close() { running_ = false; }
bool AudioDevice::Record(const std::string&) { return false; }
} // namespace gt2::audio
