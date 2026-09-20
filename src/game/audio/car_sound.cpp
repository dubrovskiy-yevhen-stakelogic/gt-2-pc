#include "game/audio/car_sound.h"

#include <cstring>

#include "game/sim/car_body.h"
#include "game/sim/fixed.h"

// Integer semantics follow the MIPS code: 32-bit products wrap, `>>` on signed values is arithmetic, unsigned
// where the original used logical shifts / unsigned compares, sim::Div = the MIPS division.
namespace gt2::audio {
namespace {

using sim::Div;

// A one-shot of the effect bank at the car's priority (FUN_80018760: sample effectSamples[index], priority = car index * 6).
void CarEffect(const CarSoundContext& ctx, const CarSound& sound, int32_t index, int32_t pan, int32_t volume) {
    if (const InstSample* s = ctx.effect(ctx.user, ctx.effectSamples[index])) PlaySample(ctx, *s, pan, volume, int32_t(sound.index) * 6);
}

} // namespace

int32_t DistanceVolume(const int32_t relative[3], bool aiCar, bool attract, int32_t* distanceOut) { // 0x800140A4
    uint32_t a = uint32_t(relative[0] < 0 ? -relative[0] : relative[0]);
    uint32_t b = uint32_t(relative[1] < 0 ? -relative[1] : relative[1]);
    uint32_t c = uint32_t(relative[2] < 0 ? -relative[2] : relative[2]);
    uint32_t mid = b;
    if (a < b) { mid = a; a = b; }
    uint32_t low = c;
    if (a < c) { low = a; a = c; }
    if (mid < low) { const uint32_t t = mid; mid = low; low = t; }
    const int32_t distance = int32_t(a + (mid >> 1) + (low >> 2));
    if (distanceOut) *distanceOut = distance;
    int32_t scale = 0x80000;
    if (!attract && aiCar) scale = 0x18000;
    const int32_t v = Div(scale, (distance >> 16) + 1);
    int32_t volume = v > 0x4000 ? 0x4000 : v;
    if (v < 0x400 && aiCar) volume = 0;
    return volume;
}

int32_t DopplerFactor(int32_t speedOfSound, int32_t listenerSpeed, int32_t dirDotListenerDir, int32_t carSpeed, int32_t dirDotCarDir) { // 0x800146D8
    if (listenerSpeed >= (speedOfSound >> 1)) return 0x1000;
    const int32_t denominator = speedOfSound + ((carSpeed * dirDotCarDir) >> 12);
    const uint32_t numerator = uint32_t(speedOfSound + ((listenerSpeed * dirDotListenerDir) >> 12));
    // 0x80086084: 64-bit signed division of numerator * 0x1000 (the original forms the high word as (n >> 31) << 12 | n >> 20)
    const int64_t wide = (int64_t(int32_t(numerator) >> 31) << 44) | (int64_t(numerator) << 12);
    return int32_t(sim::Div64(wide, denominator));
}

uint32_t NotePitch(const uint16_t* pitchTable, int32_t note256, int32_t baseNote256) { // 0x8007A52C
    int32_t d = note256 - baseNote256;
    int32_t octave = -2;
    if (d < 0) {
        do { d += 0xC00; octave--; } while (d < 0);
    } else {
        while (d > 0xBFF) { octave++; d -= 0xC00; }
    }
    const uint32_t v = pitchTable[uint32_t(d) >> 3];
    if (octave < 1) return (v >> (uint32_t(-octave) & 0x1F)) & 0xFFFF;
    return (v << (uint32_t(octave) & 0x1F)) & 0xFFFF;
}

void PanVolume(const uint16_t* panTable, int16_t out[2], int32_t volume, int32_t pan) { // 0x8007A170
    out[0] = int16_t(int32_t(uint32_t(volume) * panTable[0x7F - pan]) >> 14);
    out[1] = int16_t(int32_t(uint32_t(volume) * panTable[pan]) >> 14);
}

void PlaySample(const CarSoundContext& ctx, const InstSample& sample, int32_t pan, int32_t volume, int32_t priority) { // 0x800784A0
    if (volume == 0) return;
    int32_t v = (Div(int32_t(uint32_t(ctx.masterVolume) * uint32_t(volume)), 0xFF) * int32_t(sample.volume)) >> 14;
    if (v > 0x3FFF) v = 0x3FFF;
    int16_t lr[2];
    PanVolume(ctx.panTable, lr, v, pan);
    VoiceRequest r;
    r.volumeLeft = uint16_t(lr[0]);
    r.volumeRight = uint16_t(lr[1]);
    r.pitch = uint16_t(NotePitch(ctx.pitchTable, int32_t(sample.note) << 8, sample.baseNote));
    r.address = sample.address;
    r.adsr1 = sample.adsr1;
    r.adsr2 = sample.adsr2;
    r.duration = sample.duration;
    r.priority = priority < 0 ? sample.priority : uint8_t(priority);
    r.flags = sample.flags;
    ctx.driver->Play(nullptr, r);
}

void StartLightSound(const CarSoundContext& ctx, int32_t stage) { // 0x800189C4
    const uint8_t left = ctx.lightSamples[stage * 2], right = ctx.lightSamples[stage * 2 + 1];
    int32_t pan = 0x40;
    if (left != right) {
        if (const InstSample* s = ctx.effect(ctx.user, left)) PlaySample(ctx, *s, 0, 0x4000, -1);
        pan = 0x7F;
    }
    if (const InstSample* s = ctx.effect(ctx.user, right)) PlaySample(ctx, *s, pan, 0x4000, -1);
}

void InitEngineSound(EngineSound& engine, uint32_t layerTable, uint8_t layerCount, uint8_t priority) { // 0x80078F10
    engine.priority = priority;
    engine.layerCount = layerCount;
    engine.layerTable = layerTable;
    engine.pitchScale = 0x1000;
    engine.volumeLeft = 0x3FFF;
    engine.volumeRight = 0x3FFF;
    engine.pad06 = 0x4000;
    for (EngineSlot& s : engine.slots) { s.handle = -1; s.layer = -1; s.previous = -1; }
}

void InitCarSound(const CarSoundContext& ctx, CarSound& sound, uint8_t mode, uint32_t engineTable, uint8_t engineLayers, uint32_t exhaustTable,
                  uint8_t exhaustLayers, const uint32_t effectTokens[4]) { // 0x80018138
    (void)ctx;
    sound.mode = mode;
    sound.masterVolume = 0;
    sound.pad94[0] = sound.pad94[1] = 0;
    if (mode == 0) {
        InitEngineSound(sound.engine, engineTable, engineLayers, 0);
        InitEngineSound(sound.exhaust, exhaustTable, exhaustLayers, 0);
    } else if (mode == 1) {
        InitEngineSound(sound.exhaust, engineTable, engineLayers, 0);
    }
    for (int i = 0; i < 4; i++) { sound.voices[i].sample = effectTokens[i]; sound.voices[i].handle = -1; } // 0x80078598
}

void EngineSlotPitch(const CarSoundContext& ctx, EngineSound& engine, int32_t slot, int32_t rpm, bool continuing) { // 0x80078A54
    EngineSlot& s = engine.slots[slot];
    const EngineLayer& layer = ctx.layers(ctx.user, engine.layerTable)[s.layer];
    uint32_t pitch = (uint32_t(int32_t(layer.pitch) * rpm) / uint32_t(int32_t(layer.rpmPitch))) * uint32_t(int32_t(engine.pitchScale)) >> 12;
    if (pitch == 0) pitch = 1;
    if (continuing) {
        const uint32_t high = uint32_t(s.pitch) << 1, low = uint32_t(s.pitch) >> 1;
        if (high < pitch) pitch = high;
        else if (pitch < low) pitch = low;
    }
    s.pitch = uint16_t(pitch);
}

void EngineSlotVolume(const CarSoundContext& ctx, EngineSound& engine, int32_t slot, int32_t rpm, bool continuing) { // 0x80078B00
    const EngineLayer* table = ctx.layers(ctx.user, engine.layerTable);
    EngineSlot& s = engine.slots[slot];
    const int32_t other = engine.slots[(slot + 1) & 1].layer;
    const EngineLayer& layer = table[s.layer];
    uint32_t fade = 0x4000;
    if (other >= 0) {
        const EngineLayer& neighbour = table[other];
        int32_t numerator, denominator;
        bool full = false;
        if (rpm < layer.fadeIn) { numerator = rpm - neighbour.fadeOut; denominator = layer.fadeIn - neighbour.fadeOut; }
        else if (rpm <= layer.fadeOut) full = true, numerator = denominator = 0;
        else { numerator = neighbour.fadeIn - rpm; denominator = neighbour.fadeIn - layer.fadeOut; }
        if (!full) fade = uint32_t(Div(numerator * 0x4000, denominator));
    }
    const int32_t sine = ctx.sinTable[(fade >> 4) & 0xFFF];
    fade = uint32_t(((sine * 0x3FFF) >> 12) * int32_t(layer.volume)) >> 14;
    const uint32_t left = uint32_t(int32_t(engine.volumeLeft) * int32_t(fade)) >> 14;
    const uint32_t right = uint32_t(int32_t(engine.volumeRight) * int32_t(fade)) >> 14;
    uint32_t outLeft = left, outRight = right;
    if (continuing) {
        const int32_t previousLeft = s.volumeLeft, previousRight = s.volumeRight;
        if (int32_t(left) > previousLeft + 0x1000) outLeft = uint32_t(previousLeft + 0x1000);
        else if (int32_t(left) < previousLeft - 0x1000) outLeft = uint32_t(previousLeft - 0x1000);
        if (int32_t(right) > previousRight + 0x1000) outRight = uint32_t(previousRight + 0x1000);
        else if (int32_t(right) < previousRight - 0x1000) outRight = uint32_t(previousRight - 0x1000);
    }
    s.volumeLeft = int16_t(outLeft);
    s.volumeRight = int16_t(outRight);
}

void UpdateEngineSound(const CarSoundContext& ctx, EngineSound& engine) { // 0x80078C88
    const EngineLayer* table = ctx.layers(ctx.user, engine.layerTable);
    int32_t rpm = engine.rpm;
    if (rpm < 200) rpm = 200;
    const EngineLayer* previous = nullptr;
    for (int32_t i = 0; i < int32_t(engine.layerCount); i++) {
        const EngineLayer& layer = table[i];
        if (layer.fadeIn <= rpm && rpm <= layer.fadeOut) { engine.slots[0].layer = int8_t(i); engine.slots[1].layer = -1; break; }
        if (previous && previous->fadeOut < rpm && rpm < layer.fadeIn) { engine.slots[0].layer = int8_t(i - 1); engine.slots[1].layer = int8_t(i); break; }
        previous = &layer;
    }
    if (engine.slots[0].layer == engine.slots[1].previous || engine.slots[1].layer == engine.slots[0].previous) {
        const int8_t t = engine.slots[1].layer;
        engine.slots[1].layer = engine.slots[0].layer;
        engine.slots[0].layer = t;
    }
    for (int32_t slot = 0; slot < 2; slot++) {
        EngineSlot& s = engine.slots[slot];
        bool restart = true;
        if (s.layer == s.previous) {
            if (s.handle >= 0) {
                restart = false;
                if (s.layer >= 0) {
                    EngineSlotPitch(ctx, engine, slot, rpm, true);
                    EngineSlotVolume(ctx, engine, slot, rpm, true);
                    ctx.driver->SetPitch(&s.handle, s.pitch);
                    ctx.driver->SetVolume(&s.handle, uint16_t(s.volumeLeft), uint16_t(s.volumeRight));
                }
            }
        } else if (s.handle >= 0) {
            ctx.driver->Release(&s.handle);
            ctx.driver->Detach(&s.handle);
        }
        if (restart) {
            if (s.layer >= 0) {
                EngineSlotPitch(ctx, engine, slot, rpm, false);
                EngineSlotVolume(ctx, engine, slot, rpm, false);
                s.address = uint16_t(table[s.layer].sampleAddress >> 3);
                VoiceRequest r;
                r.volumeLeft = uint16_t(s.volumeLeft);
                r.volumeRight = uint16_t(s.volumeRight);
                r.pitch = s.pitch;
                r.address = s.address;
                r.adsr1 = 0x000F;
                r.adsr2 = 0x0003;
                r.duration = 0;
                r.priority = engine.priority;
                r.flags = 1;
                ctx.driver->Play(&s.handle, r);
            }
            s.previous = s.layer;
        }
    }
}

void SetEnginePriority(const CarSoundContext& ctx, EngineSound& engine, uint8_t priority) { // 0x80078FF8
    engine.priority = priority;
    for (EngineSlot& s : engine.slots)
        if (s.handle >= 0) ctx.driver->SetPriority(&s.handle, priority);
}

void UpdateSampleVoice(const CarSoundContext& ctx, SampleVoice& voice, int32_t left, int32_t right, int32_t priority, int32_t pitchScale) { // 0x800785A8
    int32_t l = Div(int32_t(uint32_t(ctx.masterVolume) * uint32_t(left)), 0xFF);
    int32_t r = Div(int32_t(uint32_t(ctx.masterVolume) * uint32_t(right)), 0xFF);
    if (l == 0 && r == 0) { StopSampleVoice(ctx, voice); return; }
    if (l > 0x3FFF) l = 0x3FFF;
    if (r > 0x3FFF) r = 0x3FFF;
    const InstSample& sample = *ctx.sample(ctx.user, voice.sample);
    const uint32_t pitch = uint32_t(int32_t(NotePitch(ctx.pitchTable, int32_t(sample.note) << 8, sample.baseNote) * uint32_t(pitchScale)) >> 12);
    if (voice.handle < 0) {
        VoiceRequest req;
        req.volumeLeft = uint16_t(l);
        req.volumeRight = uint16_t(r);
        req.pitch = uint16_t(pitch);
        req.address = sample.address;
        req.adsr1 = sample.adsr1;
        req.adsr2 = sample.adsr2;
        req.duration = sample.duration;
        req.priority = priority < 0 ? sample.priority : uint8_t(priority);
        req.flags = sample.flags;
        ctx.driver->Play(&voice.handle, req);
    } else {
        if (priority >= 0) ctx.driver->SetPriority(&voice.handle, uint8_t(priority));
        ctx.driver->SetPitch(&voice.handle, uint16_t(pitch));
        ctx.driver->SetVolume(&voice.handle, uint16_t(l), uint16_t(r));
    }
}

void StopSampleVoice(const CarSoundContext& ctx, SampleVoice& voice) { // 0x80078760
    ctx.driver->Release(&voice.handle);
    ctx.driver->Detach(&voice.handle);
}

void MixCarSound(const CarSoundContext& ctx, CarSound& sound) { // 0x8001826C
    const int32_t master = sound.masterVolume, doppler = sound.doppler;
    const int32_t pan = ((sound.pan + 0x1000) >> 1) * 0x7F >> 12;
    sound.engine.pitchScale = int16_t(doppler);
    sound.exhaust.pitchScale = int16_t(doppler);
    sound.engine.rpm = sound.rpm;
    sound.exhaust.rpm = sound.rpm;
    int16_t lr[2];
    PanVolume(ctx.panTable, lr, master, pan);
    int32_t left = Div(int32_t(uint32_t(ctx.masterVolume) * uint32_t(int32_t(lr[0]))), 0xFF);
    int32_t right = Div(int32_t(uint32_t(ctx.masterVolume) * uint32_t(int32_t(lr[1]))), 0xFF);
    if (left > 0x3FFF) left = 0x3FFF;
    if (right > 0x3FFF) right = 0x3FFF;
    sound.engine.volumeLeft = int16_t((left * sound.engineVolume) >> 14);
    sound.engine.volumeRight = int16_t((right * sound.engineVolume) >> 14);
    sound.exhaust.volumeLeft = int16_t((left * sound.exhaustVolume) >> 14);
    sound.exhaust.volumeRight = int16_t((right * sound.exhaustVolume) >> 14);
    if (sound.mode == 1) {
        sound.exhaust.volumeLeft = int16_t((uint32_t(sound.exhaust.volumeLeft * 0x333) & 0x3FFFFFFFu) >> 10);
        sound.exhaust.volumeRight = int16_t((uint32_t(sound.exhaust.volumeRight * 0x333) & 0x3FFFFFFFu) >> 10);
    }
    const uint32_t base = uint32_t(sound.index) * 6;
    SetEnginePriority(ctx, sound.engine, uint8_t(base & 0xFE));
    SetEnginePriority(ctx, sound.exhaust, uint8_t((base + 1) & 0xFF));
    UpdateSampleVoice(ctx, sound.voices[0], (left * sound.turboVolume) >> 14, (right * sound.turboVolume) >> 14, int32_t(base + 2), (doppler * sound.turboPitch) >> 12);
    UpdateSampleVoice(ctx, sound.voices[1], (left * sound.squealVolume) >> 14, (right * sound.squealVolume) >> 14, int32_t(base + 4), doppler);
    UpdateSampleVoice(ctx, sound.voices[2], (left * sound.slipVolume1) >> 14, (right * sound.slipVolume1) >> 14, int32_t(base + 3), (doppler * sound.slipPitch1) >> 12);
    UpdateSampleVoice(ctx, sound.voices[3], (left * sound.slipVolume5) >> 14, (right * sound.slipVolume5) >> 14, int32_t(base + 3), (doppler * sound.slipPitch5) >> 12);
    if (sound.mode == 0 && sound.blowOffVolume != 0) CarEffect(ctx, sound, 4, pan, (int32_t(sound.blowOffVolume) * master) >> 14);
    if (sound.mode == 0) UpdateEngineSound(ctx, sound.engine);
    if (sound.mode == 0 || sound.mode == 1) UpdateEngineSound(ctx, sound.exhaust);
}

void UpdateCarSound(const CarSoundContext& ctx, CarSound& sound, const CarSoundInputs& in) { // 0x800146D8
    const sim::CarBody& body = *reinterpret_cast<const sim::CarBody*>(in.body); // pack(1) layout of the original
    int32_t master = in.muted ? 0 : int32_t(uint16_t(in.distanceVolume));
    sound.index = uint8_t(in.focused ? 1 : in.rank + 1);
    sound.masterVolume = int16_t(master);
    sound.panSide = int16_t(in.panSide);
    sound.pan = int16_t(in.pan);
    sound.doppler = int16_t(in.doppler);
    int32_t engineScale = 0x2000, exhaustScale = 0x4000, effectScale = 0x1333;
    int32_t loadScale = in.attract ? 0x3000 : 0x1000;
    if (in.focused) {
        if (!in.attract && body.impactTimer == in.rate) { // a hard wall impact this frame: the crash sample
            int32_t v = body.wallImpact << 3;
            if (v > 0xFFF) v = 0x1000;
            CarEffect(ctx, sound, 5, 0x40, (v << 14) >> 12);
        }
        if (in.controlClass < 4 && in.controlClass > 1) {
            loadScale = 0x3000;
            if (in.inCarView) {
                sound.doppler = 0x1000;
                sound.panSide = 0;
                sound.pan = 0;
                master = 0x4000;
                if (!in.inCarViewAlt) { engineScale = 0x2199; exhaustScale = 0x2B33; effectScale = 0x2199; }
                else { engineScale = 0x3000; exhaustScale = 0x1800; effectScale = 0x3000; }
            }
        }
    }
    sound.masterVolume = int16_t(master);
    sound.rpm = uint16_t(body.engineRpm);
    sound.engineVolume = int16_t((engineScale * int32_t(body.engineVisual0)) >> 7);
    sound.exhaustVolume = int16_t((exhaustScale * int32_t(body.engineVisual1)) >> 7);
    const int32_t blowOff = body.blowOffState;
    sound.blowOffVolume = int16_t(blowOff < 0 ? 0 : blowOff << 2);
    sound.turboPitch = int16_t((uint32_t(uint16_t(body.turboSpoolMax)) << 12) / 0x1800);
    sound.turboVolume = int16_t((effectScale * int32_t(body.exhaustFlame)) >> 7);
    int32_t count0 = 0, sum0 = 0, count1 = 0, sum1 = 0, dust1 = 0, count5 = 0, sum5 = 0, dust5 = 0;
    for (int i = 0; i < 4; i++) {
        const sim::Wheel& wheel = body.wheels[i];
        const int32_t surface = wheel.surface, smoke = wheel.smokeLevel, dust = wheel.dustLevel;
        if (surface == 1) { count1++; sum1 += smoke; dust1 += dust; }
        else if (surface == 0) { count0++; sum0 += smoke; }
        else if (surface == 5) { count5++; sum5 += smoke; dust5 += dust; }
    }
    if (count0 != 0) sum0 = (sum0 * 0x10 * loadScale) >> 12;
    if (count1 != 0) { sum1 = (sum1 * 0x40 * loadScale) >> 12; dust1 = Div(dust1 << 7, count1); }
    if (count5 != 0) { sum5 = (sum5 * 0x10 * loadScale) >> 12; dust5 = (Div(dust5 << 7, count5) * 0x266) >> 12; }
    sound.squealVolume = int16_t(sum0 > 0x4000 ? 0x4000 : sum0);
    sound.slipVolume1 = int16_t(sum1 > 0x4000 ? 0x4000 : sum1);
    sound.slipPitch1 = int16_t(dust1 > 0x3FFF ? 0x3FFF : dust1);
    sound.slipVolume5 = int16_t(sum5 > 0x4000 ? 0x4000 : sum5);
    sound.slipPitch5 = int16_t(dust5 > 0x3FFF ? 0x3FFF : dust5);
    MixCarSound(ctx, sound);
}

} // namespace gt2::audio
