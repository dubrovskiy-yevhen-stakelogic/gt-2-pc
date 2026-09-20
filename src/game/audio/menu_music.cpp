#include "game/audio/menu_music.h"

#include <cstdio>
#include <exception>

namespace gt2::audio {

MenuMusic::~MenuMusic() {
    device_.Close();
    mixer_.SetStream(nullptr);
}

bool MenuMusic::Open(const std::string& discPath, const GuestImage& exe) {
    try {
        player_ = std::make_unique<MusicPlayer>();
        player_->Open(discPath, exe);
        mixer_.SetStream(player_.get());
        std::string error;
        if (!device_.Open(mixer_, error)) {
            std::printf("music: no sound device (%s)\n", error.c_str());
            mixer_.SetStream(nullptr);
            player_.reset();
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        std::printf("music: disabled (%s)\n", e.what());
        mixer_.SetStream(nullptr);
        player_.reset();
        return false;
    }
}

void MenuMusic::Play(int track, uint8_t volume, bool loop) {
    current_ = track;
    if (!player_) return;
    std::printf("music: CD track %d (MUSIC.DAT XA channel %d)%s\n", track, track + 1, loop ? ", loop" : "");
    player_->SetVolume(volume);
    player_->Play(track, loop);
}

void MenuMusic::Stop() {
    current_ = -1;
    if (player_) player_->Stop();
}

} // namespace gt2::audio
