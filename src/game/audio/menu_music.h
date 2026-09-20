#pragma once
// The CD music of the menus and views outside a race: the executable's stream request 0x80080F24(track, loop) (the title's
// 0x80012414, the race overlay's 0x800481C8 in the post-race views) and its stop 0x8007C570 (the race overlay's 0x800481E8).
// Every "CD track" the menus request is an XA track of MUSIC.DAT (docs/formats/sound.md section 6: track id = XA file 1,
// channel id + 1 of the executable's table, Simulation 0x800959C0, read through the build profile); neither US disc has CD-DA audio tracks (the images hold one
// MODE2/2352 data track: the ISO volume covers every sector of both .bin files). This is MusicPlayer with its own mixer and
// output device, the volume (career + 0xB3) taken at each start as 0x80080F24 takes 0x801C9993.
#include <cstdint>
#include <memory>
#include <string>

#include "game/audio/audio_device.h"
#include "game/audio/mixer.h"
#include "game/audio/music_player.h"
#include "gt2formats/overlay_data.h"

namespace gt2::audio {

class MenuMusic {
public:
    MenuMusic() = default;
    ~MenuMusic();
    MenuMusic(const MenuMusic&) = delete;
    MenuMusic& operator=(const MenuMusic&) = delete;

    // Opens MUSIC.DAT of the disc image (the track table of `exe`) and the output device; false (printed) without either:
    // the game runs on silent.
    bool Open(const std::string& discPath, const GuestImage& exe);
    bool IsOpen() const { return player_ != nullptr; }
    // 0x80080F24(track, loop) with the music volume `volume` (career + 0xB3).
    void Play(int track, uint8_t volume, bool loop = true);
    void Stop(); // 0x8007C570
    int Current() const { return current_; }

private:
    Mixer mixer_;
    AudioDevice device_;
    std::unique_ptr<MusicPlayer> player_;
    int current_ = -1;
};

} // namespace gt2::audio
