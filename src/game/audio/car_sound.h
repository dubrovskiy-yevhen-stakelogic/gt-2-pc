#pragma once
#include <cstdint>

#include "game/audio/mixer.h"
#include "gt2formats/sound_bank.h"

// The car sound logic of the US Simulation v1.2 executable (SHA-1 3030aa27...), ported routine by routine and
// verified bit for bit by tools/gt2verify (verify_sound.cpp) on the attract-race RAM dump; docs/formats/sound.md
// has the evidence. The state is laid out as the original's per-car sound object (car + 0xAA4, 0x98 bytes).
//
// Per race frame (30 Hz), for every car in listener-distance order (0x80014E6C -> 0x800146D8):
//   UpdateCarSound   0x800146D8  doppler / pan / distance volume, the per-frame levels from the physics body
//                                (rpm, engine load bytes, wheel skid levels, blow-off, wall impact), then
//   MixCarSound      0x8001826C  the pan / master / view volumes into the two engine layers and the four effect
//                                voices; the blow-off one-shot; then
//   UpdateEngineSound 0x80078C88 per ENGN bank (intake "engine" bank, exhaust bank): picks the layer(s) for the rpm,
//                                EngineSlotPitch 0x80078A54 / EngineSlotVolume 0x80078B00 give pitch and crossfade
//                                volume, and the voice driver is programmed (start / pitch / volume / release).
// The voice driver (0x8007A59C..0x8007A778) is behind VoiceDriver; the game's Mixer implements it, the verifier
// uses a driver that never allocates (the original's table is made unallocatable) so that the routines only touch
// the sound object.
namespace gt2::audio {

#pragma pack(push, 1)
// One of the two voices of an ENGN player (car sound + 0x34 / + 0x40 and + 0x5C / + 0x68).
struct EngineSlot {
    int8_t handle;         // +0  driver voice, -1 = none
    int8_t layer;          // +1  layer of the bank playing on this slot, -1 = none
    int8_t previous;       // +2  layer of the last frame (a change restarts the voice)
    uint8_t pad03;
    int16_t volumeLeft;    // +4  the driver's volume registers
    int16_t volumeRight;   // +6
    uint16_t pitch;        // +8  the driver's pitch
    uint16_t address;      // +A  sample address / 8 of the layer
};
static_assert(sizeof(EngineSlot) == 12);

// An ENGN bank player (car sound + 0x24 engine, + 0x4C exhaust; 0x80078F10 initialises it).
struct EngineSound {
    uint32_t layerTable;   // +0   the bank's layer table (a token resolved by CarSoundContext; a guest pointer in the original)
    uint8_t layerCount;    // +4
    uint8_t priority;      // +5   voice priority (0x80078FF8)
    uint16_t pad06;
    uint16_t rpm;          // +8
    int16_t pitchScale;    // +A   0x1000 = 1.0 (the doppler factor)
    int16_t volumeLeft;    // +C   0..0x3FFF
    int16_t volumeRight;   // +E
    EngineSlot slots[2];   // +10, +1C
};
static_assert(sizeof(EngineSound) == 0x28);

// A sample voice of the effect bank (car sound + 0x74..; 0x80078598 binds the sample).
struct SampleVoice {
    uint32_t sample;       // +0  the INST sample (token; a guest pointer in the original)
    int8_t handle;         // +4  driver voice, -1 = none
    uint8_t pad05[3];
};
static_assert(sizeof(SampleVoice) == 8);

// The per-car sound object (car + 0xAA4).
struct CarSound {
    uint8_t mode;          // +00  0 = full (player: intake + exhaust + effects), 1 = exhaust only (the other cars), else silent
    uint8_t index;         // +01  1 = the listener's car, else the car's rank by distance + 1 (voice priorities = index * 6 + n)
    int16_t doppler;       // +02  pitch factor, 0x1000 = 1.0
    uint16_t rpm;          // +04  body + 0x6AC
    int16_t turboPitch;    // +06  turbo spool (body + 0x746) << 12 / 0x1800
    int16_t slipPitch1;    // +08  mean dust level << 7 of the wheels on surface 1
    int16_t slipPitch5;    // +0A  mean dust level << 7 * 0x266 >> 12 of the wheels on surface 5
    int16_t masterVolume;  // +0C  distance volume (car + 0x808) or 0x4000 in the in-car views
    int16_t panSide;       // +0E  direction to the car . listener axis A (unused by the mix)
    int16_t pan;           // +10  direction to the car . listener axis B: -0x1000 left .. 0x1000 right
    int16_t pad12, pad14;
    int16_t engineVolume;  // +16  0x2000 (view-dependent) * body + 0x757 >> 7
    int16_t exhaustVolume; // +18  0x4000 (view-dependent) * body + 0x758 >> 7
    int16_t turboVolume;   // +1A  0x1333 (view-dependent) * body + 0x759 >> 7
    int16_t squealVolume;  // +1C  wheels on surface 0: sum of the smoke levels * 0x10 (* 3 in the in-car views), max 0x4000
    int16_t slipVolume1;   // +1E  surface 1: sum * 0x40
    int16_t slipVolume5;   // +20  surface 5: sum * 0x10
    int16_t blowOffVolume; // +22  blow-off state (body + 0x744) << 2 while armed
    EngineSound engine;    // +24  intake bank
    EngineSound exhaust;   // +4C  exhaust bank
    SampleVoice voices[4]; // +74  turbo whine, tyre squeal (surface 0), surface 1, surface 5 (samples of the overlay table 0x8002F528)
    uint8_t pad94[4];
};
static_assert(sizeof(CarSound) == 0x98);
#pragma pack(pop)

// The sound driver as the routines call it (0x8007A59C Play, 0x8007A614 SetPriority, 0x8007A668 SetPitch,
// 0x8007A6C8 SetVolume, 0x8007A728 Release, 0x8007A778 Detach). `handle` addresses the caller's int8 voice slot;
// Play writes the voice index or -1 into it (null handle: fire and forget).
class VoiceDriver {
public:
    virtual ~VoiceDriver() = default;
    virtual void Play(int8_t* handle, const VoiceRequest& request) = 0;
    virtual void SetPriority(int8_t* handle, uint8_t priority) = 0;
    virtual void SetPitch(int8_t* handle, uint16_t pitch) = 0;
    virtual void SetVolume(int8_t* handle, uint16_t left, uint16_t right) = 0;
    virtual void Release(int8_t* handle) = 0;
    virtual void Detach(int8_t* handle) = 0;
};

// A driver with no voices: Play answers -1, nothing else happens (the verifier; also a muted game).
class NullVoiceDriver : public VoiceDriver {
public:
    void Play(int8_t* handle, const VoiceRequest&) override { if (handle) *handle = -1; }
    void SetPriority(int8_t*, uint8_t) override {}
    void SetPitch(int8_t*, uint16_t) override {}
    void SetVolume(int8_t*, uint16_t, uint16_t) override {}
    void Release(int8_t*) override {}
    void Detach(int8_t*) override {}
};

// The game's Mixer as the driver.
class MixerVoiceDriver : public VoiceDriver {
public:
    explicit MixerVoiceDriver(Mixer& mixer) : mixer_(mixer) {}
    void Play(int8_t* handle, const VoiceRequest& request) override { mixer_.Play(handle, request); }
    void SetPriority(int8_t* handle, uint8_t priority) override { mixer_.SetPriority(handle, priority); }
    void SetPitch(int8_t* handle, uint16_t pitch) override { mixer_.SetPitch(handle, pitch); }
    void SetVolume(int8_t* handle, uint16_t left, uint16_t right) override { mixer_.SetVolume(handle, left, right); }
    void Release(int8_t* handle) override { mixer_.Release(handle); }
    void Detach(int8_t* handle) override { mixer_.Detach(handle); }
private:
    Mixer& mixer_;
};

// Everything the routines read from outside the sound object.
struct CarSoundContext {
    VoiceDriver* driver = nullptr;
    void* user = nullptr;
    const EngineLayer* (*layers)(void* user, uint32_t token) = nullptr;   // EngineSound::layerTable -> the table
    const InstSample* (*sample)(void* user, uint32_t token) = nullptr;    // SampleVoice::sample -> the sample
    const InstSample* (*effect)(void* user, uint32_t index) = nullptr;    // sample `index` of the race effect bank (0x8017D894 -> sound/se01.ins)
    const int16_t* sinTable = nullptr;     // 4096 entries, 0x80093150 (sim/trig.h SinTable)
    const uint16_t* pitchTable = nullptr;  // 384 entries, 0x8008FDB8: pitch of note fraction i/384 of an octave, 0x4000 = one octave down... (see NotePitch)
    const uint16_t* panTable = nullptr;    // 128 entries, 0x800901B8: right-channel gain of pan 0..127, 0x4000 = 1.0
    uint8_t masterVolume = 0xC0;           // 0x801C9994: the effects volume setting, 0..255
    uint8_t effectSamples[6] = {};         // 0x8002F528: turbo, squeal, surface 1, surface 5, blow-off, wall impact
    uint8_t lightSamples[4] = {};          // 0x8002F530: start-light beeps (left, right) for the lights and the green
};

// The inputs of UpdateCarSound (0x800146D8) that the original takes from the car record, the physics body and
// the listener (the race task's camera block, + 0xC4).
struct CarSoundInputs {
    int32_t distanceVolume = 0;  // car + 0x808 (DistanceVolume)
    bool muted = false;          // car + 0xB3C non-zero
    int32_t rank = 0;            // 1-based position in the distance-sorted car list
    bool focused = false;        // car index == the listener's car (camera + 0x10C)
    int32_t controlClass = 0;    // car + 0x18: 2..3 = player controlled
    bool attract = false;        // 0x800A951C
    bool inCarView = false;      // camera + 0x10B == 0: the listener sits in the car (fixed master volume, no pan / doppler)
    bool inCarViewAlt = false;   // camera + 0x10A != 0: the second in-car mix
    int32_t rate = 30;           // 0x801C8570 frames per second
    int32_t doppler = 0x1000;    // computed by the caller (DopplerFactor); 0x1000 = 1.0
    int32_t panSide = 0, pan = 0; // direction to the car . listener axes, 4096 = 1.0
    // The body (car + 0x2C) as bytes: rpm 0x6AC, impact timer 0x73E, wall impact 0x740, blow-off 0x744, turbo spool
    // 0x746, engine load bytes 0x757..0x759, wheels 0x460 + i * 0x68 (surface + 0x14, smoke + 0x1D, dust + 0x1F).
    const uint8_t* body = nullptr;
};

// 0x800140A4 (the volume part): car + 0x808 from the car - listener vector (16.16 m). `aiCar` = car + 0x18 < 2.
int32_t DistanceVolume(const int32_t relative[3], bool aiCar, bool attract, int32_t* distanceOut = nullptr);
// 0x800146D8 (the doppler part): (c + listenerSpeed * dir.listenerDir) * 0x1000 / (c + carSpeed * dir.carDir), c =
// 0x800AF22C, speeds in 16.16 m per frame, directions unit vectors (4096); 0x1000 when the listener moves at c/2 or more.
int32_t DopplerFactor(int32_t speedOfSound, int32_t listenerSpeed, int32_t dirDotListenerDir, int32_t carSpeed, int32_t dirDotCarDir);
// 0x8007A52C: SPU pitch of `note256` (note << 8 | fine) for a sample recorded at `baseNote256`.
uint32_t NotePitch(const uint16_t* pitchTable, int32_t note256, int32_t baseNote256);
// 0x8007A170: left / right volumes of `volume` at pan 0..127.
void PanVolume(const uint16_t* panTable, int16_t out[2], int32_t volume, int32_t pan);
// 0x800784A0: a one-shot sample (`priority` < 0: the sample's own).
void PlaySample(const CarSoundContext& ctx, const InstSample& sample, int32_t pan, int32_t volume, int32_t priority);
// 0x800189C4: the start signal: `stage` 0 = a red light, 1 = the green light (a left / right beep pair).
void StartLightSound(const CarSoundContext& ctx, int32_t stage);
// 0x80078F10: binds `layerTable` / `layerCount` to an engine player and resets it.
void InitEngineSound(EngineSound& engine, uint32_t layerTable, uint8_t layerCount, uint8_t priority);
// 0x80018138: the sound object of a car: `mode` 0 = engine + exhaust banks, 1 = exhaust bank only; the effect voices
// take the overlay's samples.
void InitCarSound(const CarSoundContext& ctx, CarSound& sound, uint8_t mode, uint32_t engineTable, uint8_t engineLayers, uint32_t exhaustTable,
                  uint8_t exhaustLayers, const uint32_t effectTokens[4]);
// 0x80078A54 / 0x80078B00: pitch / crossfade volume of slot `slot` for `rpm`; `continuing` limits the change to the
// previous frame's value (an octave / 0x1000).
void EngineSlotPitch(const CarSoundContext& ctx, EngineSound& engine, int32_t slot, int32_t rpm, bool continuing);
void EngineSlotVolume(const CarSoundContext& ctx, EngineSound& engine, int32_t slot, int32_t rpm, bool continuing);
// 0x80078C88: the per-frame update of an engine player.
void UpdateEngineSound(const CarSoundContext& ctx, EngineSound& engine);
// 0x80078FF8: the priority of both slots.
void SetEnginePriority(const CarSoundContext& ctx, EngineSound& engine, uint8_t priority);
// 0x800785A8: a sample voice at `left` / `right` (0 / 0 releases it), `pitchScale` 0x1000 = 1.0, `priority` < 0 = keep.
void UpdateSampleVoice(const CarSoundContext& ctx, SampleVoice& voice, int32_t left, int32_t right, int32_t priority, int32_t pitchScale);
// 0x80078760: release and forget.
void StopSampleVoice(const CarSoundContext& ctx, SampleVoice& voice);
// 0x8001826C: the mix of one car.
void MixCarSound(const CarSoundContext& ctx, CarSound& sound);
// 0x800146D8: the per-frame inputs of one car, then MixCarSound.
void UpdateCarSound(const CarSoundContext& ctx, CarSound& sound, const CarSoundInputs& in);

} // namespace gt2::audio
