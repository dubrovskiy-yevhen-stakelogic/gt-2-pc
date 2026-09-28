#include "game/audio/audio_device.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>

namespace gt2::audio {
struct AudioDevice::Buffer { SDL_AudioDeviceID device=0; std::vector<float> mix; };
bool AudioDevice::Open(Mixer& mixer,std::string& error) {
    Close();
    if(SDL_InitSubSystem(SDL_INIT_AUDIO)!=0) { error=SDL_GetError(); return false; }
    mixer_=&mixer; buffers_.push_back(new Buffer);
    SDL_AudioSpec desired{};
    desired.freq=kSampleRate; desired.format=AUDIO_S16SYS; desired.channels=2; desired.samples=1024;
    desired.userdata=this;
    desired.callback=[](void* user,Uint8* output,int bytes) {
        auto& self=*static_cast<AudioDevice*>(user);
        const size_t frames=size_t(bytes)/4;
        auto& mix=self.buffers_[0]->mix;
        mix.assign(frames*2,0.0f); self.mixer_->Mix(mix.data(),frames);
        auto* samples=reinterpret_cast<int16_t*>(output);
        for(size_t i=0;i<mix.size();++i) samples[i]=int16_t(std::lround(std::clamp(mix[i],-1.0f,1.0f)*32767.0f));
    };
    const auto device=SDL_OpenAudioDevice(nullptr,0,&desired,nullptr,0);
    if(!device) { error=SDL_GetError(); Close(); return false; }
    buffers_[0]->device=device; running_=true; SDL_PauseAudioDevice(device,0);
    std::puts("audio: Web Audio via SDL, shared 44100 Hz mixer"); return true;
}
void AudioDevice::Close() {
    running_=false;
    for(auto* buffer:buffers_) { if(buffer->device) SDL_CloseAudioDevice(buffer->device); delete buffer; }
    if(!buffers_.empty()) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    buffers_.clear(); mixer_=nullptr;
}
// Capture is a development feature; returning false reports that it is unavailable.
bool AudioDevice::Record(const std::string&) { return false; }
void AudioDevice::Run() {}
}
