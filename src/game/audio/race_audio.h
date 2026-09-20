#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "game/audio/audio_device.h"
#include "game/audio/car_sound.h"
#include "game/audio/mixer.h"
#include "game/sim/race_sim.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/sound_bank.h"
#include "gt2vfs/gtfs.h"

// The race's sound as the product runs it: the banks of the cars from the disc (gt2formats/sound_bank.h), the
// executable's pitch / pan tables, the overlay's sample indices, one CarSound per car driven by the ported logic
// (car_sound.h) on the simulation's bodies, the native mixer and the Windows device. Everything is optional
// (--no-sound / no device): the simulation never depends on it.
namespace gt2::audio {

struct CarSoundSetup {
    int32_t controlClass = 0;  // 2 = player (intake + exhaust banks of the car), else AI (the shared exhaust bank)
    uint32_t soundId = 0;      // CarConfig::engineWord (EngineRow::word0A): the engine sound set number
    uint8_t exhaustByte = 0;   // CarConfig::exhaustByte: muffler stage, + 4 with a turbo kit
    bool turbo = false;        // CarConfig::flags bit 1: the AI cars pick engine/ene_t.es instead of ene_n.es
};

// The listener (the original's camera block, race task + 0xC4) in metres / metres per simulation frame.
struct Listener {
    float position[3] = {0, 0, 0};
    float velocity[3] = {0, 0, 0};   // per 30 Hz frame
    float axisSide[3] = {0, 0, 1};   // camera + 0xEC (stored, unused by the mix)
    float axisRight[3] = {1, 0, 0};  // camera + 0xF4: pans the car (positive = right)
    bool inCarView = false;          // camera + 0x10B == 0: the player's car at a fixed volume, no pan / doppler
    bool inCarViewAlt = false;       // camera + 0x10A != 0
    int focusedCar = 0;              // camera + 0x10C
    // The camera object's own fields, exact (cameraMotion true; StepTwoPlayer): + 0xB8 position (16.16 m), + 0xE4 direction of
    // motion (4096 = 1) and + 0xFC speed (16.16 m per frame) for the doppler, + 0xEC / + 0xF4 axes. Without them the values are
    // derived from the float fields above.
    bool cameraMotion = false;
    int32_t cameraSpeed = 0;
    int16_t cameraDirection[3] = {0, 0, 0};
    int32_t cameraPosition[3] = {0, 0, 0};
    int16_t cameraAxisSide[3] = {0, 0, 0};
    int16_t cameraAxisRight[3] = {0, 0, 0};
};

// The reverb unit as the game's boot programs it (preset 4 from the executable's table, depth 0x0FFF).
ReverbSettings GameReverb(const GuestImage& exe);

class RaceAudio {
public:
    RaceAudio();
    ~RaceAudio();

    // Loads the banks and tables for these cars. Throws std::runtime_error when a file is missing / malformed.
    void Load(const GtfsVolume& vol, const GuestImage& exe, const GuestImage& overlay, std::span<const CarSoundSetup> cars);
    // Opens the sound device (false with `error`: the game keeps running silently).
    bool OpenDevice(std::string& error);
    // Dev aid (gt2game --record-audio): the device output is also written to a WAV (call before OpenDevice).
    bool RecordTo(const std::string& path) { return device_.Record(path); }
    void SetMasterVolume(uint8_t volume) { context_.masterVolume = volume; } // 0x801C9994 (0..255, 0xC0 in the dump)
    // The SPU reverb of the EON voices (on after Load; --no-reverb turns it off).
    void SetReverbEnabled(bool enabled) { mixer_.SetReverbEnabled(enabled); }
    // The CD input of the mix: the race music (music_player.h); null detaches.
    void AttachMusic(StreamSource* music) { mixer_.SetStream(music); }
    void SetPaused(bool paused) { mixer_.SetPaused(paused); }

    // Once per simulation frame, after RaceSim::Step: every car's sound from its body and the listener (0x80014E6C).
    void Step(const sim::RaceSim& race, const Listener& listener, bool attract);
    // The 2 player Battle's sound (game mode 0), once per simulation frame after RaceSim::Step and the frame's camera update:
    // `cameras` = the two camera objects (view + 0xC4 + i * 0x110) after it, `split` = the view flag the frame draws with (view +
    // 0x2EA). The original's sound pass (Sim 0x80015DF4) runs after the camera update but BEFORE the frame's draw: it reads the
    // car records' view values (car + 0x868 of 0x800133F0, car + 0x80C / + 0x808 / + 0x804 of 0x800140A4) and the split flag
    // (0x800292A0) the previous frame's draw left - split, 0x80014ED0: the two players' cars, car i with rank 0 against camera i
    // (0x800297F4 saves car 0's vector / volume around player 2's view pass, Sim 0x80029888..0x800298DC); else 0x80014E6C: every
    // car against camera 1.
    void StepTwoPlayer(const sim::RaceSim& race, const std::array<Listener, 2>& cameras, bool split, bool attract);
    // The state of the race setup for StepTwoPlayer: no view drawn yet (car + 0x80C / + 0x808 zero: the first sound pass hears
    // nothing), car + 0x85C = the cars' positions, `split` = the view flag of the setup.
    void PrimeTwoPlayer(const sim::RaceSim& race, bool split);
    // car + 0x85C at the race setup (0x800133F0 runs there once): the cars' positions the first frame's velocities are taken
    // from. Without it the first Step / StepTwoPlayer takes that frame's own positions.
    void Prime(const sim::RaceSim& race);
    // The start signal (race_shell.h ShellHooks::sound 0x800189C4): stage 0 = a red light, 1 = the green light.
    void StartLight(int32_t stage);

    const CarSound& Sound(size_t car) const { return cars_[car]; }
    size_t CarCount() const { return cars_.size(); }
    int ActiveVoices() const { return mixer_.ActiveVoices(); }
    static constexpr int32_t kSpeedOfSound = 0x5AAAA; // 0x800AF22C in the dump: 340 m/s in 16.16 m per 1/60 s

private:
    // What a drawn frame leaves in a car's record for the next sound pass.
    struct CarView {
        int32_t relative[3] = {0, 0, 0}; // car + 0x80C
        int32_t distance = 0;            // car + 0x804
        int32_t volume = 0;              // car + 0x808
        int32_t velocity[3] = {0, 0, 0}; // car + 0x868
    };
    void ViewCar(const sim::RaceSim& race, size_t car, const Listener& camera, bool attract);
    void MixCar(const sim::RaceSim& race, size_t car, const CarView& view, const Listener& listener, int32_t rank, bool attract);
    void CarVelocity(const sim::RaceSim& race, size_t car, int32_t out[3]);
    struct Bank { EngineBank bank; uint32_t address = 0; }; // address = where the data sits in the mixer's sample RAM
    uint32_t Upload(std::span<const uint8_t> data);
    int LoadEngineBank(const GtfsVolume& vol, const std::string& path);
    static const EngineLayer* LayersOf(void* user, uint32_t token);
    static const InstSample* SampleOf(void* user, uint32_t token);
    static const InstSample* EffectOf(void* user, uint32_t index);

    Mixer mixer_;
    MixerVoiceDriver driver_;
    AudioDevice device_;
    CarSoundContext context_;
    std::vector<int16_t> sinTable_;
    std::vector<uint16_t> pitchTable_, panTable_;
    std::vector<Bank> banks_;
    InstBank effects_;
    uint32_t nextAddress_ = 0x1000;
    std::vector<CarSound> cars_;
    std::vector<int32_t> controlClass_;
    std::vector<std::array<int32_t, 3>> previousPosition_; // car + 0x85C: 16.16 m, for the car velocity (car + 0x868)
    std::vector<uint8_t> primed_;
    std::vector<CarView> view_;          // StepTwoPlayer: the previous frame's draw
    bool lastSplit_ = false, twoPlayerPrimed_ = false;
    bool loaded_ = false;
};

} // namespace gt2::audio
