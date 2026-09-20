#include "game/audio/menu_audio.h"

#include <stdexcept>

#include "game/audio/race_audio.h"
#include "gt2vfs/gtfs.h"

namespace gt2::audio {

namespace {
constexpr uint32_t kPitchTableAddress = 0x8008FDB8u;    // 384 x u16 (0x8007A52C)
constexpr uint32_t kPanTableAddress = 0x800901B8u;      // 128 x u16 (0x8007A170)
constexpr uint32_t kVelocityTableAddress = 0x800900B8u; // 128 x u16 (0x80079460 / 0x80079744)
constexpr uint32_t kEffectPairsAddress = 0x8009160Cu;   // 10 x {u8 left, u8 right} (0x80060840)
constexpr uint32_t kTrackFileIds = 0x80052A50u;         // ovl4: 9 x u32 named file id of the menu tracks (0x800228D4)
constexpr uint32_t kTrackCount = 9;
// The EXE's named file ids of the sound directory are consecutive in the volume's order (arcade.ins, arcade.seq,
// arcseq.ins, then) 0xEC sound/gtmseq.ins, 0xED se01.ins, 0xEE..0xF6 spu_02..spu_10.seq, 0xF7 sys.ins (id table
// 0x801E2EF0 in the RAM dumps).
constexpr uint32_t kFirstSoundId = 0xEC;
} // namespace

MenuAudio::MenuAudio()
    : pitchTable_(384), panTable_(128), velocityTable_(128),
      sequencer_(driver_, SequencerTables{pitchTable_.data(), panTable_.data(), velocityTable_.data()}) {}

MenuAudio::~MenuAudio() { device_.Close(); }

void MenuAudio::Load(const GtfsVolume& vol, const GuestImage& exe, const GuestImage& ovl4) {
    LoadExeTables(exe);

    std::vector<std::string> soundFiles; // the sound directory in the volume's order
    for (const GtfsEntry& f : vol.Files())
        if (f.path.rfind("sound/", 0) == 0) soundFiles.push_back(f.path);
    size_t first = soundFiles.size(); // the file of id 0xEC
    for (size_t i = 0; i < soundFiles.size(); i++)
        if (soundFiles[i].rfind("sound/gtmseq.ins", 0) == 0) first = i;
    if (first == soundFiles.size()) throw std::runtime_error("menu sound: sound/gtmseq.ins missing");
    auto byId = [&](uint32_t id) -> std::string {
        if (id < kFirstSoundId || first + (id - kFirstSoundId) >= soundFiles.size()) throw std::runtime_error("menu sound: file id outside the sound directory");
        return soundFiles[first + (id - kFirstSoundId)];
    };

    auto upload = [&](InstBank& bank, uint32_t address) {
        if (!mixer_.Upload(address, bank.data)) throw std::runtime_error("menu sound: bank does not fit the sample RAM");
        for (InstSample& s : bank.samples) s.address = uint16_t(s.address + (address >> 3)); // 0x80078974
    };
    system_ = ParseInstBank(vol.Read("sound/sys.ins")); // 0x80011CE4 (EXE load): resident
    upload(system_, kSystemBankAddress);
    const std::vector<uint8_t> musicBank = vol.Read("sound/gtmseq.ins"); // 0x80022874 at the GT-mode entry
    music_ = ParseInstBank(musicBank);
    programs_ = ParseInstPrograms(musicBank);
    upload(music_, kMusicBankAddress);
    tracks_.clear();
    for (uint32_t t = 0; t < kTrackCount; t++) tracks_.push_back(ParseSeqg(vol.Read(byId(ovl4.Get<uint32_t>(ovl4.Sim(kTrackFileIds) + t * 4)))));
    loaded_ = true;
}

void MenuAudio::LoadExeTables(const GuestImage& exe) {
    // Through the build profile (identity on the Simulation EXE; the arcade EXE keeps these tables 0x308 / 0x2BC lower).
    const uint32_t pitch = exe.Sim(kPitchTableAddress), pan = exe.Sim(kPanTableAddress), velocity = exe.Sim(kVelocityTableAddress),
                   pairs = exe.Sim(kEffectPairsAddress);
    for (uint32_t i = 0; i < 384; i++) pitchTable_[i] = exe.Get<uint16_t>(pitch + i * 2);
    for (uint32_t i = 0; i < 128; i++) panTable_[i] = exe.Get<uint16_t>(pan + i * 2);
    for (uint32_t i = 0; i < 128; i++) velocityTable_[i] = exe.Get<uint16_t>(velocity + i * 2);
    for (uint32_t i = 0; i < effectPairs_.size(); i++) effectPairs_[i] = {exe.Get<uint8_t>(pairs + i * 2), exe.Get<uint8_t>(pairs + i * 2 + 1)};
    context_ = CarSoundContext{};
    context_.driver = &effectDriver_;
    context_.pitchTable = pitchTable_.data();
    context_.panTable = panTable_.data();
    context_.masterVolume = kEffectsVolume;
    mixer_.SetReverb(GameReverb(exe));
}

void MenuAudio::LoadArcade(const GtfsVolume& vol, const GuestImage& exe) {
    LoadExeTables(exe);
    auto upload = [&](InstBank& bank, uint32_t address) {
        if (!mixer_.Upload(address, bank.data)) throw std::runtime_error("menu sound: bank does not fit the sample RAM");
        for (InstSample& s : bank.samples) s.address = uint16_t(s.address + (address >> 3)); // 0x80078974
    };
    system_ = ParseInstBank(vol.Read("sound/sys.ins")); // resident (EXE load), as on the Simulation disc
    upload(system_, kSystemBankAddress);
    // Member 2's 0x800257CC: file id 216 (arcade file table 0x801E2950 -> VOL 0x296F sound/arcseq.ins) as the bank, then
    // id 215 (0x296E sound/arcade.seq) behind it; the bank lands behind sys.ins like gtmseq.ins (SPU 0x9630, the captured
    // voices' start addresses: docs/formats/sound.md section 9).
    const std::vector<uint8_t> musicBank = vol.Read("sound/arcseq.ins");
    music_ = ParseInstBank(musicBank);
    programs_ = ParseInstPrograms(musicBank);
    upload(music_, kMusicBankAddress);
    tracks_.clear();
    tracks_.push_back(ParseSeqg(vol.Read("sound/arcade.seq")));
    loaded_ = true;
}

void MenuAudio::StartArcadeMusic(uint8_t musicVolume) { // member 2 0x800256EC(0x800F3718, 0): stop, master volume, sequence 0 at once
    sequencer_.Stop();
    const int32_t master = (int32_t(musicVolume) * 0x4000) / 255;
    if (log_) std::fprintf(log_, "%d track arcade\n", frame_);
    if (!loaded_ || tracks_.empty()) return;
    sequencer_.Start(tracks_[0], 0, music_, programs_, master);
    playing_ = requested_ = 0;
    countdown_ = 0;
}

void MenuAudio::EffectDriver::Play(int8_t* handle, const VoiceRequest& r) {
    if (owner_.log_)
        std::fprintf(owner_.log_, "%d effect-voice address %04X pitch %04X volume %04X %04X adsr %04X %04X duration %u flags %u\n", owner_.frame_, r.address,
                     r.pitch, r.volumeLeft, r.volumeRight, r.adsr1, r.adsr2, r.duration, r.flags);
    mixer_.Play(handle, r);
}

void MenuAudio::SetEventLog(std::FILE* log) {
    log_ = log;
    if (!log_) {
        sequencer_.onNote = nullptr;
        sequencer_.onVoice = nullptr;
        return;
    }
    sequencer_.onVoice = [this](const VoiceRequest& r) {
        std::fprintf(log_, "%d voice address %04X pitch %04X volume %04X %04X adsr %04X %04X duration %u flags %u\n", frame_, r.address, r.pitch, r.volumeLeft,
                     r.volumeRight, r.adsr1, r.adsr2, r.duration, r.flags);
    };
    sequencer_.onNote = [this](int track, uint8_t note, uint8_t velocity, int32_t length, uint8_t program) {
        std::fprintf(log_, "%d note %d %u %u %d %u\n", frame_, track, note, velocity, length, program);
    };
}

void MenuAudio::Sound(int id) { // 0x80060840
    if (log_) std::fprintf(log_, "%d effect %d\n", frame_, id);
    if (!loaded_ || id < 0 || size_t(id) >= effectPairs_.size()) return;
    const uint8_t left = effectPairs_[size_t(id)][0], right = effectPairs_[size_t(id)][1];
    auto play = [&](uint8_t index, int32_t pan) {
        if (index < system_.samples.size()) PlaySample(context_, system_.samples[index], pan, 0x4000, -1); // 0x80078900 -> 0x800784A0
    };
    if (left == right) {
        play(left, 0x40);
    } else {
        play(left, 0);
        play(right, 0x7F);
    }
}

void MenuAudio::RequestMusic(int track) { // 0x80018FA4
    if (track == requested_) return;
    sequencer_.Stop(); // 0x80022934
    playing_ = -1;
    requested_ = track;
    countdown_ = 2;
}

void MenuAudio::StopMusic() { // 0x80018FF0 (the menus are left; the track is requested again when they come back)
    sequencer_.Stop();
    playing_ = -1;
    requested_ = -1;
    countdown_ = 0;
}

void MenuAudio::StartTrack(int track) { // 0x800228D4: the file of the track, sequence 0, from the beginning
    if (track < 0 || size_t(track) >= tracks_.size()) return;
    const int32_t master = (int32_t(kMusicVolume) * 0x4000) / 255;
    if (log_) std::fprintf(log_, "%d track %d\n", frame_, track);
    sequencer_.Start(tracks_[size_t(track)], 0, music_, programs_, master);
    playing_ = track;
}

void MenuAudio::Frame() {
    if (!loaded_) return;
    mixer_.Poll();
    if (countdown_ > 0 && --countdown_ == 0) StartTrack(requested_); // 0x80019028
    sequencer_.Tick();
    frame_++;
}

} // namespace gt2::audio
