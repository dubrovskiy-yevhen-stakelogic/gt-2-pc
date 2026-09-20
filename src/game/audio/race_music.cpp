#include "game/audio/race_music.h"

#include "game/audio/music_player.h"

namespace gt2::audio {

uint32_t TaskRandom(uint32_t& state) { // 0x80083AE0
    const uint32_t s = state * 0x11u + 0x11u;
    state = s;
    return s ^ ((s << 16) | (s >> 16));
}

void InitRaceMusic(RaceMusicBytes& m, uint32_t& randomState, const RaceMusicInputs& in, const std::vector<MusicTrack>& tracks,
                   uint16_t& holdInitial, uint16_t& hold) { // 0x800299D8
    m.playing = 0xFF;
    m.request = 0xFF;
    m.loop = 0;
    const uint32_t r = TaskRandom(randomState);
    m.next = 0xFF;
    m.kind = 0;
    m.raceTrack = uint8_t(r % 6u);
    if (in.gameMode == 3) {
        m.kind = 2;
    } else if (in.gameMode == 7) {
        m.kind = 1;
    } else if (hold != 0) {
        if (in.flag5DDC != 0) {
            m.kind = 4;
            m.request = 11;
        } else {
            m.request = in.oneMakeEvent ? 10 : 12;
        }
    }
    if (int8_t(m.request) >= 0) {
        const int32_t fields = int32_t(MusicTrackSeconds(tracks.at(m.request)) * 60u); // 0x8008103C(id) * 0x3C
        if (int32_t(hold) < fields) {
            hold = uint16_t(fields);
            holdInitial = uint16_t(fields);
        }
    }
    if (in.demoFlag != 0) m.request = 0xFF;
}

void UpdateRaceMusic(RaceMusicBytes& m, MusicPlayer* player) { // 0x80029B60
    if (m.playing == m.request) {
        bool finished = true;
        // The stream still runs (0x801F0674 == 0): nothing to do (the original also resumes a stream another CD
        // read paused, command 6 - the native player is never paused by other reads).
        if (int8_t(m.playing) >= 0 && !(player && player->Ended())) finished = false;
        if (finished && int8_t(m.next) >= 0) {
            m.request = m.next;
            m.next = 0xFF;
            m.loop = 1;
        }
    } else {
        m.playing = m.request;
        if (!player) return;
        if (int8_t(m.request) < 0) player->Stop();
        else player->Play(m.request, m.loop != 0);
    }
}

} // namespace gt2::audio
