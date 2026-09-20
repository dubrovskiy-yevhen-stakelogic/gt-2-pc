#pragma once
#include <cstdint>
#include <vector>

#include "gt2formats/xa_audio.h"

// The race's music requests (docs/formats/sound.md section 6): the music bytes of the race task object
// (*(0x8002F4F4) + 0x2EC..0x2F1), chosen at race load by 0x800299D8 and serviced every frame by 0x80029B60. The
// race shell (game/sim/race_shell.*) writes the same bytes at the green light (0x80029C60: request = the race track,
// loop) and at the finish (0x80029C84: the jingle); RaceShellState keeps + 0x2ED..0x2F1, `playing` (+ 0x2EC) is kept here.
namespace gt2::audio {

class MusicPlayer;

struct RaceMusicBytes {
    uint8_t playing = 0xFF;   // +0x2EC  the id last handed to the stream driver (0xFF = none / stopped)
    uint8_t loop = 0;         // +0x2ED  the request loops
    uint8_t request = 0xFF;   // +0x2EE  the requested id (0xFF = silence)
    uint8_t raceTrack = 0;    // +0x2EF  the race's track, requested at the green light
    uint8_t next = 0xFF;      // +0x2F0  queued after a non-looping request ends (8 after the finish jingle outside modes 0 / 3)
    uint8_t kind = 0;         // +0x2F1  selects the finish jingle (0x80029C84: 3 -> 13, 1 -> 16, else 14)
};

// The shell globals / race state 0x800299D8 reads and writes.
struct RaceMusicInputs {
    uint8_t gameMode = 0;       // 0x801D5866
    int8_t flag5DDC = 0;        // 0x801D5DDC (signed byte; != 0 -> intro 11, kind 4)
    bool oneMakeEvent = false;  // 0x801D5865 == 1 and the event name 0x801D58A0 equals the overlay string 0x8002F20C
    uint8_t demoFlag = 0;       // 0x800A951C (attract race: no music, the hold is raised all the same)
};

// 0x80083AE0: the task's pseudo-random generator (state = state * 17 + 17; result = state ^ (state rotated by 16)).
uint32_t TaskRandom(uint32_t& state);

// 0x800299D8 (race load, after the hold of 0x8001584C is set): the race track = TaskRandom % 6 (one of the six
// licensed songs), the pre-race intro (12; 10 for one-make events; 11 when 0x801D5DDC != 0) unless the race is a
// licence test (mode 3, kind 2) or mode 7 (kind 1) or has no hold; a hold shorter than the intro is raised to its
// length (0x8008103C seconds * 60 fields). `holdInitial` / `hold` = 0x800A951E / 0x800A9520.
void InitRaceMusic(RaceMusicBytes& m, uint32_t& randomState, const RaceMusicInputs& in, const std::vector<MusicTrack>& tracks,
                   uint16_t& holdInitial, uint16_t& hold);

// 0x80029B60 (every race frame, while the task's + 0x2E9 is 0): a changed request is handed to the stream driver
// (0x80080F24 play with the loop flag, or 0x8007C570 stop); when the current id has ended (0x801F0674) the queued
// `next` becomes a looping request. `player` may be null (no music output: the bytes still advance).
void UpdateRaceMusic(RaceMusicBytes& m, MusicPlayer* player);

} // namespace gt2::audio
