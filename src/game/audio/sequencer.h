#pragma once
// The SEQG sequence player of the sound driver (US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a):
// start 0x8007A300, stop 0x8007A4D8, the 60 Hz tick 0x80079A38 (tempo accumulator, per-track countdowns, events
// 0x80079744), note-on 0x80079460 (every sample of the program whose key range holds the note, one of 25 note slots)
// and the slot update 0x80079B64 (note length, bend and volume changes). The voices go through the same driver
// calls as the race's sounds (car_sound.h VoiceDriver). Evidence: docs/formats/sound.md section 8 (a model of this
// player reproduced all 1038 note-ons of two captured menu tracks, field, track, note and velocity).
#include <array>
#include <cstdint>
#include <functional>

#include "game/audio/car_sound.h"
#include "gt2formats/seqg.h"
#include "gt2formats/sound_bank.h"

namespace gt2::audio {

struct SequencerTables {
    const uint16_t* pitch = nullptr;    // 0x8008FDB8 (384 entries, NotePitch)
    const uint16_t* pan = nullptr;      // 0x800901B8 (128 entries, PanVolume)
    const uint16_t* velocity = nullptr; // 0x800900B8 (128 entries, 0..0x4000): note velocity / track volume curve
};

class Sequencer {
public:
    static constexpr int kTracks = 16, kSlots = 25, kLoopDepth = 4;
    static constexpr uint32_t kIncrementPerField = 480000000u / 60u; // 0x8007A300: + per 60 Hz tick

    Sequencer(VoiceDriver& driver, const SequencerTables& tables) : driver_(driver), tables_(tables) {}

    // 0x8007A300: sequence `index` of `file` with the bank's samples (addresses relocated to where the bank's data
    // sits) and programs; `master` = the music volume (0x801F0236 = setting * 0x4000 / 255). The objects must outlive
    // the playback.
    void Start(const SeqgFile& file, size_t index, const InstBank& bank, const std::vector<InstProgram>& programs, int32_t master);
    // 0x8007A4D8: the player leaves the list; its notes are released at the next Tick.
    void Stop();
    // One 60 Hz tick: the sequence's events (0x80079A38), then the note slots (0x80079B64).
    void Tick();
    bool Playing() const { return playing_; }
    // Dev aid: every note event (before the slot search), with the tick count of the field.
    std::function<void(int track, uint8_t note, uint8_t velocity, int32_t length, uint8_t program)> onNote;
    std::function<void(const VoiceRequest& request)> onVoice; // every voice a note starts

private:
    struct Track {
        uint32_t at = 0;          // +0 read position (file offset)
        int32_t countdown = 0;    // +4
        int32_t volume = 0x4000;  // +8
        int32_t effective = 0;    // +A master * volume
        int16_t bend = 0;         // +C
        uint8_t pan = 0;          // +10
        uint8_t program = 0;      // +11
        int8_t depth = -1;        // +12
        bool alive = true;
        std::array<uint32_t, kLoopDepth> loopAt{};  // +14
        std::array<uint8_t, kLoopDepth> loopCount{}; // +24
    };
    struct Slot {
        int8_t handle = -1;       // +1B the voice (-1 = free)
        bool owned = false;       // bound to the player
        int track = 0;
        const InstSample* sample = nullptr;
        uint8_t note = 0, velocity = 0, programPan = 0x40;
        int16_t programVolume = 0x4000;
        int32_t countdown = 0;    // release when it runs out
        int16_t bend = 0;         // the track's values the voice was last set with
        int32_t effective = 0;
        uint8_t pan = 0;
        bool released = false;
    };
    uint32_t Vlv(uint32_t& at) const;
    void Event(Track& t, int ticks);
    void NoteOn(Track& t, int trackIndex, uint8_t note, uint8_t velocity, int32_t length, int ticks);
    void SlotVolume(Slot& s, uint16_t out[2]) const;
    uint16_t SlotPitch(const Slot& s, int16_t bend) const;

    VoiceDriver& driver_;
    SequencerTables tables_;
    const SeqgFile* file_ = nullptr;
    const InstBank* bank_ = nullptr;
    const std::vector<InstProgram>* programs_ = nullptr;
    bool playing_ = false;
    int32_t master_ = 0x4000;
    int32_t volume_ = 0x4000, effective_ = 0x4000;
    uint32_t tempo_ = 0;
    int32_t accumulator_ = 0;
    std::array<Track, kTracks> tracks_{};
    std::array<Slot, kSlots> slots_{};
    int cursor_ = 0; // 0x801F0238: the slot search starts here
};

} // namespace gt2::audio
