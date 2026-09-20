// verify_sound.cpp - the car sound logic (src/game/audio/car_sound.*) against the original: the engine layer
// pitch / crossfade volume formulas, the per-frame engine player update and the per-car mix of the US Simulation
// v1.2 executable, on the sound objects of the attract-race dump (car + 0xAA4). The sound driver's voice table
// (0x801EFE68, 24 entries of 0x28 bytes) is made unallocatable in every case (state byte 2, handles -1), so that the
// routines only read tables and write the sound object: the native side runs with a driver that has no voices.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>

#include "game/audio/car_sound.h"
#include "game/audio/mixer.h"
#include "game/audio/music_player.h"
#include "game/audio/race_audio.h"
#include "game/audio/race_music.h"
#include "gt2formats/sound_bank.h"
#include "gt2formats/xa_audio.h"
#include "guest.h"
#include "machine/spu.h"

using namespace gt2;
using namespace gt2::verify;

namespace {

constexpr uint32_t kSoundOffset = 0xAA4, kEngineOffset = 0x24, kExhaustOffset = 0x4C;
constexpr uint32_t kDriverTable = 0x801EFE68, kDriverEntries = 24, kDriverEntrySize = 0x28;
constexpr uint32_t kSinTable = 0x80093150, kPitchTable = 0x8008FDB8, kPanTable = 0x800901B8, kMasterVolume = 0x801C9994;
constexpr uint32_t kEffectBankPointer = 0x8017D894, kEffectSamples = 0x8002F528, kLightSamples = 0x8002F530;

uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }

// The native context on a RAM image: tokens are guest addresses.
struct RamContext {
    audio::NullVoiceDriver driver;
    audio::CarSoundContext ctx;
    uint8_t* ram = nullptr;
    explicit RamContext(uint8_t* image) : ram(image) {
        ctx.driver = &driver;
        ctx.user = this;
        ctx.layers = [](void* user, uint32_t token) { return reinterpret_cast<const EngineLayer*>(At(static_cast<RamContext*>(user)->ram, token)); };
        ctx.sample = [](void* user, uint32_t token) { return reinterpret_cast<const InstSample*>(At(static_cast<RamContext*>(user)->ram, token)); };
        ctx.effect = [](void* user, uint32_t index) {
            uint8_t* r = static_cast<RamContext*>(user)->ram;
            uint32_t bank, table;
            std::memcpy(&bank, At(r, D(kEffectBankPointer)), 4);
            std::memcpy(&table, At(r, bank + 0x1C), 4);
            return reinterpret_cast<const InstSample*>(At(r, table + index * 0x14));
        };
        ctx.sinTable = reinterpret_cast<const int16_t*>(At(ram, D(kSinTable)));
        ctx.pitchTable = reinterpret_cast<const uint16_t*>(At(ram, D(kPitchTable)));
        ctx.panTable = reinterpret_cast<const uint16_t*>(At(ram, D(kPanTable)));
        ctx.masterVolume = *At(ram, D(kMasterVolume));
        std::memcpy(ctx.effectSamples, At(ram, D(kEffectSamples)), 6);
        std::memcpy(ctx.lightSamples, At(ram, D(kLightSamples)), 4);
    }
};

void DisableDriver(uint8_t* ram) {
    for (uint32_t i = 0; i < kDriverEntries; i++) {
        uint8_t* e = At(ram, D(kDriverTable) + i * kDriverEntrySize);
        e[0] = 2;    // allocated to nobody: 0x8007A938 neither takes nor steals it
        e[4] = 0;    // not locked
        std::memset(e + 0x14, 0, 4); // no handle bound
    }
}

audio::EngineSound& EngineAt(uint8_t* ram, uint32_t address) { return *reinterpret_cast<audio::EngineSound*>(At(ram, address)); }
audio::CarSound& SoundAt(uint8_t* ram, uint32_t address) { return *reinterpret_cast<audio::CarSound*>(At(ram, address)); }

int32_t Random(std::mt19937& rng, int32_t low, int32_t high) { return low + int32_t(rng() % uint32_t(high - low + 1)); }

// Random but valid slot / mix state of an engine player: layers of its own bank (never the same layer on both
// slots: the original never does that and its crossfade would divide by zero), handles unbound.
void RandomiseEngine(std::mt19937& rng, audio::EngineSound& e, bool keepLayers) {
    const int32_t count = e.layerCount;
    e.pitchScale = int16_t(Random(rng, 0x600, 0x2400));
    e.volumeLeft = int16_t(Random(rng, 0, 0x3FFF));
    e.volumeRight = int16_t(Random(rng, 0, 0x3FFF));
    e.rpm = uint16_t(Random(rng, 0, 11000));
    for (audio::EngineSlot& s : e.slots) {
        s.handle = -1;
        s.pitch = uint16_t(Random(rng, 1, 0x3FFF));
        s.volumeLeft = int16_t(Random(rng, 0, 0x3FFF));
        s.volumeRight = int16_t(Random(rng, 0, 0x3FFF));
    }
    if (!keepLayers && count > 0) {
        const int32_t first = Random(rng, 0, count - 1);
        int32_t second = Random(rng, -1, count - 1);
        if (second == first) second = -1;
        e.slots[0].layer = int8_t(first);
        e.slots[1].layer = int8_t(second);
        e.slots[0].previous = int8_t(rng() % 2 ? first : Random(rng, -1, count - 1));
        e.slots[1].previous = int8_t(rng() % 2 ? second : Random(rng, -1, count - 1));
        if (e.slots[0].previous == e.slots[1].previous && e.slots[0].previous >= 0) e.slots[1].previous = -1;
    }
}

struct Row { size_t cases = 0, mismatches = 0; };

// One stateful comparison with explicit register arguments (VerifyStateful takes fixed a1..a3).
template <typename Prepare, typename Native>
void RunCase(Guest& guest, const std::vector<uint8_t>& pristine, uint32_t function, uint32_t object, uint32_t a1, uint32_t a2, uint32_t a3, Prepare prepare,
             Native native, Row& row) {
    static std::vector<uint8_t> ours(Bus::kRamSize);
    std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
    std::memset(guest.Scratch(), 0, kScratchSize);
    DisableDriver(guest.Ram());
    prepare(guest.Ram());
    std::memcpy(ours.data(), guest.Ram(), Bus::kRamSize);
    guest.Call(function, object, a1, a2, a3);
    native(ours.data());
    row.cases++;
    const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
    const bool equal = std::memcmp(ours.data(), guest.Ram(), stackLow) == 0 && std::memcmp(ours.data() + stackHigh, guest.Ram() + stackHigh, Bus::kRamSize - stackHigh) == 0;
    if (!equal && row.mismatches++ < 3)
        for (uint32_t i = 0; i < Bus::kRamSize; i++)
            if ((i < stackLow || i >= stackHigh) && ours[i] != guest.Ram()[i]) {
                std::printf("    MISMATCH 0x%08X object %08X args %u %u %u: first differing byte at 0x%08X (object + 0x%X): original %02X ours %02X\n", function,
                            object, a1, a2, a3, 0x80000000u + i, i - (object & 0x1FFFFF), guest.Ram()[i], ours[i]);
                break;
            }
}

} // namespace

int gt2::verify::VerifySound(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    int failures = 0;
    // The engine players of the dump with a bank: car 0's intake + exhaust (mode 0), the other cars' exhaust (mode 1).
    std::vector<uint32_t> engines;
    for (uint32_t car = 0; car < kCarCount; car++) {
        const uint32_t sound = kCarBase + car * kCarStride + kSoundOffset;
        for (uint32_t offset : {kEngineOffset, kExhaustOffset}) {
            const audio::EngineSound& e = *reinterpret_cast<const audio::EngineSound*>(pristine.data() + ((sound + offset) & 0x1FFFFF));
            if (e.layerCount != 0 && e.layerTable != 0) engines.push_back(sound + offset);
        }
    }
    constexpr size_t kVariants = 48;
    { // 0x80078A54: pitch of one slot
        Row row;
        for (uint32_t object : engines)
            for (size_t v = 0; v < kVariants; v++) {
                const uint32_t slot = v & 1, rpm = uint32_t(Random(rng, 0, 11000)), continuing = (v >> 1) & 1;
                RunCase(guest, pristine, 0x80078A54, object, slot, rpm, continuing, [&](uint8_t* ram) { RandomiseEngine(rng, EngineAt(ram, object), false);
                            audio::EngineSound& e = EngineAt(ram, object);
                            if (e.slots[slot].layer < 0) e.slots[slot].layer = 0; },
                        [&](uint8_t* ram) { RamContext c(ram); audio::EngineSlotPitch(c.ctx, EngineAt(ram, object), int32_t(slot), int32_t(rpm), continuing != 0); }, row);
            }
        Report("EngPitch", 0x80078A54, row.cases, row.mismatches, failures);
    }
    { // 0x80078B00: crossfade volume of one slot
        Row row;
        for (uint32_t object : engines)
            for (size_t v = 0; v < kVariants; v++) {
                const uint32_t slot = v & 1, rpm = uint32_t(Random(rng, 0, 11000)), continuing = (v >> 1) & 1;
                RunCase(guest, pristine, 0x80078B00, object, slot, rpm, continuing, [&](uint8_t* ram) { RandomiseEngine(rng, EngineAt(ram, object), false);
                            audio::EngineSound& e = EngineAt(ram, object);
                            if (e.slots[slot].layer < 0) { e.slots[slot].layer = 0; if (e.slots[slot ^ 1].layer == 0) e.slots[slot ^ 1].layer = -1; } },
                        [&](uint8_t* ram) { RamContext c(ram); audio::EngineSlotVolume(c.ctx, EngineAt(ram, object), int32_t(slot), int32_t(rpm), continuing != 0); }, row);
            }
        Report("EngVolume", 0x80078B00, row.cases, row.mismatches, failures);
    }
    { // 0x80078C88: the per-frame engine player update (layer selection, restart / continue of both slots)
        Row row;
        for (uint32_t object : engines)
            for (size_t v = 0; v < kVariants * 2; v++)
                RunCase(guest, pristine, 0x80078C88, object, 0, 0, 0, [&](uint8_t* ram) { RandomiseEngine(rng, EngineAt(ram, object), v % 4 == 0); },
                        [&](uint8_t* ram) { RamContext c(ram); audio::UpdateEngineSound(c.ctx, EngineAt(ram, object)); }, row);
        Report("EngUpdate", 0x80078C88, row.cases, row.mismatches, failures);
    }
    { // 0x8001826C: the mix of one car (pan / master / view volumes into the players and the effect voices)
        Row row;
        for (uint32_t car = 0; car < kCarCount; car++) {
            const uint32_t object = kCarBase + car * kCarStride + kSoundOffset;
            for (size_t v = 0; v < kVariants; v++)
                RunCase(guest, pristine, 0x8001826C, object, 0, 0, 0,
                        [&](uint8_t* ram) {
                            audio::CarSound& s = SoundAt(ram, object);
                            s.index = uint8_t(Random(rng, 1, 7));
                            s.doppler = int16_t(Random(rng, 0x600, 0x2400));
                            s.rpm = uint16_t(Random(rng, 0, 11000));
                            s.turboPitch = int16_t(Random(rng, 0, 0x3FFF));
                            s.slipPitch1 = int16_t(Random(rng, 0, 0x3FFF));
                            s.slipPitch5 = int16_t(Random(rng, 0, 0x3FFF));
                            s.masterVolume = int16_t(Random(rng, 0, 0x4000));
                            s.pan = int16_t(Random(rng, -0x1000, 0x1000));
                            s.engineVolume = int16_t(Random(rng, 0, 0x4000));
                            s.exhaustVolume = int16_t(Random(rng, 0, 0x4000));
                            s.turboVolume = int16_t(v % 3 ? Random(rng, 0, 0x4000) : 0);
                            s.squealVolume = int16_t(v % 3 ? Random(rng, 0, 0x4000) : 0);
                            s.slipVolume1 = int16_t(v % 5 ? 0 : Random(rng, 0, 0x4000));
                            s.slipVolume5 = int16_t(v % 5 ? 0 : Random(rng, 0, 0x4000));
                            s.blowOffVolume = int16_t(v % 4 ? 0 : Random(rng, 1, 0x4000));
                            for (audio::SampleVoice& sv : s.voices) sv.handle = -1;
                            if (s.engine.layerCount) RandomiseEngine(rng, s.engine, false);
                            if (s.exhaust.layerCount) RandomiseEngine(rng, s.exhaust, false);
                        },
                        [&](uint8_t* ram) { RamContext c(ram); audio::MixCarSound(c.ctx, SoundAt(ram, object)); }, row);
        }
        Report("CarMix", 0x8001826C, row.cases, row.mismatches, failures);
    }
    return failures;
}

// ---------------------------------------------------------------- race music and reverb

namespace {

constexpr uint32_t kTaskPointer = 0x8002F4F4, kGameModeAddress = 0x801D5866, kFlag5865 = 0x801D5865, kFlag5DDC = 0x801D5DDC,
                   kEventName = 0x801D58A0, kOneMakeString = 0x8002F20C, kHoldAddress = 0x800A9520, kHoldInitialAddress = 0x800A951E,
                   kDemoFlagAddress = 0x800A951C, kMusicBaseAddress = 0x80095AC0;

GuestImage RamImage(const std::vector<uint8_t>& ram) {
    GuestImage g;
    g.base = 0x80000000u;
    g.profile = &ActiveProfile(); // Simulation addresses of the image translate to the dump's build
    g.module = GuestImage::kRaceRam;
    g.bytes = ram;
    return g;
}

void NativeInitMusic(uint8_t* ram, uint32_t task, const std::vector<MusicTrack>& tracks) {
    uint32_t state;
    uint16_t hold, holdInitial;
    std::memcpy(&state, At(ram, task + 0x70), 4);
    std::memcpy(&hold, At(ram, D(kHoldAddress)), 2);
    std::memcpy(&holdInitial, At(ram, D(kHoldInitialAddress)), 2);
    audio::RaceMusicInputs in;
    in.gameMode = *At(ram, D(kGameModeAddress));
    in.flag5DDC = int8_t(*At(ram, D(kFlag5DDC)));
    in.oneMakeEvent = *At(ram, D(kFlag5865)) == 1 &&
                      std::strcmp(reinterpret_cast<const char*>(At(ram, D(kEventName))), reinterpret_cast<const char*>(At(ram, D(kOneMakeString)))) == 0;
    in.demoFlag = *At(ram, D(kDemoFlagAddress));
    audio::RaceMusicBytes m;
    audio::InitRaceMusic(m, state, in, tracks, holdInitial, hold);
    std::memcpy(At(ram, task + 0x70), &state, 4);
    std::memcpy(At(ram, D(kHoldAddress)), &hold, 2);
    std::memcpy(At(ram, D(kHoldInitialAddress)), &holdInitial, 2);
    const uint8_t bytes[6] = {m.playing, m.loop, m.request, m.raceTrack, m.next, m.kind};
    std::memcpy(At(ram, task + 0x2EC), bytes, 6);
}

void WriteWav(const std::string& path, const std::vector<float>& stereo, float scale) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    const uint32_t frames = uint32_t(stereo.size() / 2), dataBytes = frames * 4, riffBytes = 36 + dataBytes, rate = uint32_t(audio::kSampleRate),
                   byteRate = rate * 4, fmtBytes = 16;
    const uint16_t pcm = 1, channels = 2, align = 4, bits = 16;
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riffBytes, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtBytes, 4, 1, f);
    std::fwrite(&pcm, 2, 1, f);
    std::fwrite(&channels, 2, 1, f);
    std::fwrite(&rate, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&dataBytes, 4, 1, f);
    for (float s : stereo) {
        const int16_t v = int16_t(std::lround(std::clamp(s * scale, -32768.0f, 32767.0f)));
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

// One voice of a bank rendered through a mixer for `seconds` (reverb on / off), for listening.
std::vector<float> RenderVoice(const audio::ReverbSettings& reverb, bool wet, std::span<const uint8_t> data, const audio::VoiceRequest& request, double seconds) {
    auto mixer = std::make_unique<audio::Mixer>();
    mixer->Upload(0x1000, data);
    mixer->SetReverb(reverb);
    mixer->SetReverbEnabled(wet);
    int8_t handle = -1;
    mixer->Play(&handle, request);
    std::vector<float> out(size_t(seconds * audio::kSampleRate) * 2, 0.0f);
    const size_t releaseAt = out.size() / 2 / 2;
    for (size_t f = 0; f < out.size() / 2; f += 441) {
        if (f >= releaseAt && handle >= 0) mixer->Release(&handle);
        mixer->Mix(out.data() + f * 2, std::min<size_t>(441, out.size() / 2 - f));
    }
    return out;
}

} // namespace

int gt2::verify::VerifyMusic(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol,
                             const std::string& discPath) {
    int failures = 0;
    const GuestImage ramImage = RamImage(pristine);
    const std::vector<MusicTrack> ramTracks = ReadMusicTable(ramImage);
    const char* wavDir = std::getenv("GT2_AUDIO_WAV");
    { // 0x800299D8: the race's music bytes (+ 0x2EC..0x2F1), the task generator, the intro's hold raise
        uint32_t task;
        std::memcpy(&task, pristine.data() + (D(kTaskPointer) & 0x1FFFFF), 4);
        const char* oneMake = reinterpret_cast<const char*>(pristine.data() + (D(kOneMakeString) & 0x1FFFFF));
        const size_t oneMakeLength = strnlen(oneMake, 32);
        Row row;
        for (size_t v = 0; v < 600; v++)
            RunCase(guest, pristine, 0x800299D8, task, 0, 0, 0,
                    [&](uint8_t* ram) {
                        static const uint8_t kModes[] = {0, 1, 2, 3, 4, 6, 7, 0xC};
                        *At(ram, D(kGameModeAddress)) = v % 5 == 4 ? uint8_t(rng()) : kModes[rng() % 8];
                        static const uint16_t kHolds[] = {0, 0xB4, 0x78, 540, 600, 1};
                        const uint16_t hold = v % 4 == 3 ? uint16_t(rng() % 2000) : kHolds[rng() % 6];
                        const uint16_t holdInitial = uint16_t(rng());
                        std::memcpy(At(ram, D(kHoldAddress)), &hold, 2);
                        std::memcpy(At(ram, D(kHoldInitialAddress)), &holdInitial, 2);
                        *At(ram, D(kFlag5DDC)) = rng() % 2 ? 0 : uint8_t(rng());
                        *At(ram, D(kFlag5865)) = uint8_t(rng() % 3);
                        char* name = reinterpret_cast<char*>(At(ram, D(kEventName)));
                        std::memcpy(name, oneMake, oneMakeLength + 1);
                        if (rng() % 2 && oneMakeLength > 0) name[rng() % oneMakeLength] ^= char(1 + rng() % 0x3F);
                        *At(ram, D(kDemoFlagAddress)) = uint8_t(rng() % 4 == 0 ? 1 : 0);
                        const uint32_t state = uint32_t(rng());
                        std::memcpy(At(ram, task + 0x70), &state, 4);
                        for (uint32_t i = 0; i < 6; i++) *At(ram, task + 0x2EC + i) = uint8_t(rng());
                    },
                    [&](uint8_t* ram) { NativeInitMusic(ram, task, ramTracks); }, row);
        Report("MusicInit", 0x800299D8, row.cases, row.mismatches, failures);
    }
    if (disc) { // the track table against the XA sectors of MUSIC.DAT
        const auto file = disc->FindRootFile("MUSIC.DAT");
        const GuestImage exe = LoadExeImage(*disc);
        const std::vector<MusicTrack> tracks = ReadMusicTable(exe);
        size_t mismatches = 0;
        uint32_t ramBase;
        std::memcpy(&ramBase, pristine.data() + (D(kMusicBaseAddress) & 0x1FFFFF), 4);
        if (!file || file->lba != ramBase) {
            std::printf("    MUSIC.DAT LBA %u, the executable's 0x80095AC0 = %u\n", file ? file->lba : 0u, ramBase);
            mismatches++;
        }
        for (size_t i = 0; i < tracks.size(); i++)
            if (tracks[i].first != ramTracks[i].first || tracks[i].end != ramTracks[i].end) mismatches++;
        struct Channel { uint32_t first = UINT32_MAX, last = 0, count = 0; std::map<uint8_t, uint32_t> codings, files; };
        std::map<uint8_t, Channel> channels;
        const uint32_t sectors = file ? (file->size + DiscImage::kUserDataSize - 1) / DiscImage::kUserDataSize : 0;
        uint8_t raw[DiscImage::kRawSectorSize];
        uint32_t other = 0;
        for (uint32_t s = 0; s < sectors; s++) {
            disc->ReadRawSector(file->lba + s, raw);
            const XaSubheader h = XaSubheaderOf(raw);
            if (!IsXaAudio(h)) { other++; continue; }
            Channel& c = channels[h.channel];
            c.first = std::min(c.first, s);
            c.last = std::max(c.last, s);
            c.count++;
            c.codings[h.coding]++;
            c.files[h.file]++;
        }
        std::printf("    MUSIC.DAT: LBA %u, %u sectors (%u not XA audio), %zu channels\n", file ? file->lba : 0u, sectors, other, channels.size());
        for (size_t id = 0; id < tracks.size(); id++) {
            const MusicTrack& t = tracks[id];
            const Channel& c = channels[MusicXaChannel(int(id))];
            const bool ok = c.count > 0 && c.first == t.first && c.last < t.end && c.codings.size() == 1 && c.codings.begin()->first == 0x01 &&
                            c.files.size() == 1 && c.files.begin()->first == kMusicXaFile;
            std::printf("    track %2zu: channel %2u, sectors %5u..%5u (%3u s), channel sectors %4u at %5u..%5u%s\n", id, MusicXaChannel(int(id)), t.first, t.end,
                        MusicTrackSeconds(t), c.count, c.first, c.last, ok ? "" : "  MISMATCH");
            if (!ok) mismatches++;
        }
        for (const auto& [channel, c] : channels)
            if (channel == 0 || channel > tracks.size())
                std::printf("    channel %u (no table entry): %u sectors at %u..%u\n", channel, c.count, c.first, c.last);
        Report("XaTable", kMusicTableAddress, tracks.size(), mismatches, failures);

        // Our decoder + music player against the runtime SPU's XA path (src/machine/spu.cpp, linear resampling),
        // on the first 24 sectors of track 1 (the race track of the licence capture). Gain / lag are fitted.
        audio::MusicPlayer player;
        player.Open(discPath, exe);
        player.SetVolume(0xF0);
        player.Play(1, false);
        constexpr size_t kFrames = 50000;
        std::vector<float> ours(kFrames * 2, 0.0f);
        for (size_t f = 0; f < kFrames; f += 441) player.MixStream(ours.data() + f * 2, std::min<size_t>(441, kFrames - f));
        Spu spu;
        spu.Write(0x1AA, 0xC001); // SPU on, unmuted, CD audio input on
        spu.Write(0x180, 0x3FFF);
        spu.Write(0x182, 0x3FFF);
        spu.Write(0x1B0, 0x7FFF);
        spu.Write(0x1B2, 0x7FFF);
        uint32_t pushed = 0;
        // The decoder alone: DecodeXaSector + the SPU's own linear resampling and volume steps (spu.cpp PushXaSector /
        // Generate), which must reproduce the runtime's output up to the rounding of the ADPCM filter (/ 64 there,
        // >> 6 here as psx-spx specifies).
        std::vector<float> decoderOnly;
        XaDecoderState decoderState;
        std::vector<int16_t> decoded;
        double phase = 0;
        int16_t last[2] = {0, 0};
        for (uint32_t s = file->lba + tracks[1].first; s < file->lba + tracks[1].end && pushed < 24; s++) {
            disc->ReadRawSector(s, raw);
            const XaSubheader h = XaSubheaderOf(raw);
            if (!IsXaAudio(h) || h.file != kMusicXaFile || h.channel != MusicXaChannel(1)) continue;
            spu.PushXaSector(raw);
            pushed++;
            decoded.clear();
            DecodeXaSector(raw, decoderState, decoded);
            for (size_t i = 0; i + 1 < decoded.size(); i += 2) {
                while (phase < 1.0) {
                    for (int c = 0; c < 2; c++) {
                        const int32_t v = int16_t(last[c] + (decoded[i + size_t(c)] - last[c]) * phase);
                        decoderOnly.push_back(float(((v * 0x7FFF) >> 15) * 0x7FFE >> 15));
                    }
                    phase += 37800.0 / audio::kSampleRate;
                }
                phase -= 1.0;
                last[0] = decoded[i];
                last[1] = decoded[i + 1];
            }
        }
        spu.Generate(kFrames);
        size_t decoderDiff = 0;
        double decoderMax = 0;
        for (size_t i = 0; i < std::min(decoderOnly.size(), kFrames * 2); i++) {
            const double d = std::abs(double(decoderOnly[i]) - spu.output[i]);
            decoderMax = std::max(decoderMax, d);
            if (d > 0) decoderDiff++;
        }
        std::printf("    XA decoder vs runtime SPU (same resampling): %zu samples, %zu differ, max difference %.0f\n",
                    std::min(decoderOnly.size(), kFrames * 2), decoderDiff, decoderMax);
        Report("XaDecode", 0x80080F24, std::min(decoderOnly.size(), kFrames * 2), decoderDiff, failures);
        double bestSnr = -1e9, bestGain = 0;
        int bestLag = 0;
        const size_t from = 4410, to = kFrames - 2000; // after the fade-in, before the end of the SPU's buffer
        for (int lag = -8; lag <= 8; lag++) {
            double oo = 0, or_ = 0, rr = 0;
            for (size_t f = from; f < to; f++)
                for (int c = 0; c < 2; c++) {
                    const double o = ours[(f + size_t(lag)) * 2 + size_t(c)], r = spu.output[f * 2 + size_t(c)];
                    oo += o * o;
                    or_ += o * r;
                    rr += r * r;
                }
            if (oo <= 0 || rr <= 0) continue;
            const double g = or_ / oo, err = rr - 2 * g * or_ + g * g * oo;
            const double snr = 10.0 * std::log10(rr / std::max(err, 1e-9));
            if (snr > bestSnr) { bestSnr = snr; bestGain = g; bestLag = lag; }
        }
        // The music player (Hermite resampling, CD volume, fade-in) against the runtime's linear resampling: a
        // measurement of how far the two interpolations differ, not a pass / fail criterion.
        std::printf("    music player vs runtime SPU: %u sectors, SNR %.1f dB at lag %d frames, gain %.1f (ours = CD volume 0x423B * source)\n", pushed, bestSnr,
                    bestLag, bestGain);
        if (wavDir) {
            std::vector<float> ref(spu.output.begin(), spu.output.begin() + std::ptrdiff_t(kFrames * 2));
            WriteWav(std::string(wavDir) + "/music_t1_ours.wav", ours, float(32768.0 * bestGain));
            WriteWav(std::string(wavDir) + "/music_t1_spu.wav", ref, 1.0f);
        }
    }
    { // the reverb unit (preset 4 of the executable, depth 0x0FFF): impulse response of the left input
        const audio::ReverbSettings reverb = audio::GameReverb(disc ? LoadExeImage(*disc) : ramImage);
        auto mixer = std::make_unique<audio::Mixer>();
        mixer->SetReverb(reverb);
        const size_t frames = size_t(audio::kSampleRate) * 4;
        std::vector<float> in(frames * 2, 0.0f), out(frames * 2, 0.0f);
        in[0] = in[2] = 16384.0f;
        mixer->RenderReverb(in.data(), out.data(), frames);
        size_t firstArrival = SIZE_MAX;
        double total = 0, peak = 0;
        std::vector<double> energy(frames, 0.0);
        for (size_t f = 0; f < frames; f++) {
            const double e = double(out[f * 2]) * out[f * 2] + double(out[f * 2 + 1]) * out[f * 2 + 1];
            energy[f] = e;
            total += e;
            peak = std::max(peak, std::sqrt(e));
            if (firstArrival == SIZE_MAX && std::abs(out[f * 2]) + std::abs(out[f * 2 + 1]) >= 1.0f) firstArrival = f;
        }
        // Schroeder backward integration: the time the remaining energy falls 20 / 40 dB below the total.
        double remaining = total;
        size_t t20 = 0, t40 = 0;
        for (size_t f = 0; f < frames; f++) {
            remaining -= energy[f];
            if (!t20 && remaining < total * 1e-2) t20 = f;
            if (!t40 && remaining < total * 1e-4) t40 = f;
        }
        const bool ok = total > 0 && std::isfinite(total) && firstArrival != SIZE_MAX && t40 != 0;
        std::printf("    reverb preset: mBASE 0x%04X (work area %u bytes), vIIR 0x%04X, vWALL 0x%04X, out 0x%04X; impulse 16384: first output %.1f ms, peak %.0f,"
                    " -20 dB %.0f ms, -40 dB %.0f ms\n",
                    reverb.base, 0x80000u - uint32_t(reverb.base) * 8, reverb.registers[2], reverb.registers[7], uint16_t(reverb.outLeft),
                    firstArrival * 1000.0 / audio::kSampleRate, peak, t20 * 1000.0 / audio::kSampleRate, t40 * 1000.0 / audio::kSampleRate);
        Report("Reverb", 0x80079EF8, 1, ok ? 0 : 1, failures);
        if (wavDir) {
            WriteWav(std::string(wavDir) + "/reverb_impulse.wav", out, 1.0f);
            if (vol) { // an engine loop of the attract race's car and the wall impact sample, dry / wet
                const EngineBank bank = ParseEngineBank(vol->Read("engine/20403.es"));
                audio::VoiceRequest engine;
                engine.volumeLeft = engine.volumeRight = 0x3FFF;
                engine.pitch = uint16_t(bank.layers[2].pitch);
                engine.address = uint16_t((0x1000 + bank.layers[2].sampleAddress) >> 3);
                engine.flags = 1;
                WriteWav(std::string(wavDir) + "/engine_dry.wav", RenderVoice(reverb, false, bank.data, engine, 3.0), 32768.0f);
                WriteWav(std::string(wavDir) + "/engine_wet.wav", RenderVoice(reverb, true, bank.data, engine, 3.0), 32768.0f);
                const InstBank effects = ParseInstBank(vol->Read("sound/se01.ins"));
                const InstSample& hit = effects.samples[5];
                audio::VoiceRequest impact;
                impact.volumeLeft = impact.volumeRight = 0x3FFF;
                impact.pitch = 0x1000;
                impact.address = uint16_t(hit.address + (0x1000 >> 3));
                impact.adsr1 = hit.adsr1;
                impact.adsr2 = hit.adsr2;
                impact.flags = 1;
                WriteWav(std::string(wavDir) + "/impact_dry.wav", RenderVoice(reverb, false, effects.data, impact, 2.0), 32768.0f);
                WriteWav(std::string(wavDir) + "/impact_wet.wav", RenderVoice(reverb, true, effects.data, impact, 2.0), 32768.0f);
            }
        }
    }
    return failures;
}
