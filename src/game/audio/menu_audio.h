#pragma once
// The GT-mode menus' sound (US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; GT-mode overlay =
// GT2.OVL member 4): the menu effects of 0x80060840(id) - one-shot samples of the resident bank sound/sys.ins (SPU
// 0x1010) chosen by the pair table EXE 0x8009160C - and the menu music: SEQG sequences sound/spu_02..10.seq played
// with the instruments of sound/gtmseq.ins (SPU 0x9630, loaded at the GT-mode entry 0x80018F6C), track t of a page's
// flags (bits 16..23) -> file 0x80052A50[t] (0x80018FA4 request: stop at once, start two view updates later
// 0x80019028 -> 0x800228D4, always from the beginning; 0x80018FF0 stops it when the menus are left). No XA in the
// menus. Evidence: docs/formats/sound.md section 8.
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "game/audio/audio_device.h"
#include "game/audio/car_sound.h"
#include "game/audio/mixer.h"
#include "game/audio/sequencer.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/seqg.h"
#include "gt2formats/sound_bank.h"

namespace gt2 {
class GtfsVolume;
}

namespace gt2::audio {

class MenuAudio {
public:
    static constexpr uint32_t kSystemBankAddress = 0x1010, kMusicBankAddress = 0x9630; // SPU RAM (0x80011CE4 / 0x80022874)
    static constexpr uint8_t kEffectsVolume = 0xC0; // 0x801C9994 (the setting in every dump)
    static constexpr uint8_t kMusicVolume = 0xF0;   // 0x801C9993 -> master 0x3C3C

    MenuAudio();
    ~MenuAudio();
    MenuAudio(const MenuAudio&) = delete;
    MenuAudio& operator=(const MenuAudio&) = delete;

    // Banks, sequences and tables from the disc (`exe` = SCUS_944.88, `ovl4` = GT2.OVL member 4). Throws on bad data.
    void Load(const GtfsVolume& vol, const GuestImage& exe, const GuestImage& ovl4);
    // The arcade disc's menus (US Arcade v1.1, GT2.OVL member 2; ARCADE addresses): effects as above (the EXE's 0x80060750),
    // music = sequence 0 of sound/arcade.seq on the instruments of sound/arcseq.ins (0x800257CC loads both at the menus'
    // entry 0x80014064; 0x800256EC starts it from the root view's first init 0x8001D584). `exe` = SCUS_944.55.
    void LoadArcade(const GtfsVolume& vol, const GuestImage& exe);
    // 0x800256EC(music, 0): the sequence from the beginning at once; `musicVolume` = the career's music volume (career + 0xB3).
    void StartArcadeMusic(uint8_t musicVolume);
    bool OpenDevice(std::string& error) { return device_.Open(mixer_, error); }
    bool RecordTo(const std::string& path) { return device_.Record(path); } // dev aid: call before OpenDevice
    void SetReverbEnabled(bool enabled) { mixer_.SetReverbEnabled(enabled); }

    // 0x80060840(id): 0 buzzer, 1 accept, 2 back, 3 car wash done, 5 cursor move, 6 list move (4, 7, 8: title / card /
    // race); ids outside the table are ignored.
    void Sound(int id);
    // 0x80018FA4(track): a new track stops the playing one at once and starts two view updates later.
    void RequestMusic(int track);
    // 0x80018FF0: leaving the menus.
    void StopMusic();
    // Once per view update (60 Hz): 0x80019028 (the start countdown), the sequencer tick, the driver's handle poll.
    void Frame();
    int CurrentTrack() const { return requested_; }
    // Dev aid: one line per effect, track start and sequence note ("<frame> note <track> <note> <velocity> <length>
    // <program>"), frames counted by Frame().
    void SetEventLog(std::FILE* log);

private:
    std::FILE* log_ = nullptr;
    int frame_ = 0;
    void StartTrack(int track); // 0x800228D4
    void LoadExeTables(const GuestImage& exe);

    // The mixer as the voice driver, with the effect voices written to the event log.
    class EffectDriver final : public VoiceDriver {
    public:
        EffectDriver(Mixer& mixer, MenuAudio& owner) : mixer_(mixer), owner_(owner) {}
        void Play(int8_t* handle, const VoiceRequest& r) override;
        void SetPriority(int8_t* handle, uint8_t priority) override { mixer_.SetPriority(handle, priority); }
        void SetPitch(int8_t* handle, uint16_t pitch) override { mixer_.SetPitch(handle, pitch); }
        void SetVolume(int8_t* handle, uint16_t left, uint16_t right) override { mixer_.SetVolume(handle, left, right); }
        void Release(int8_t* handle) override { mixer_.Release(handle); }
        void Detach(int8_t* handle) override { mixer_.Detach(handle); }
    private:
        Mixer& mixer_;
        MenuAudio& owner_;
    };

    Mixer mixer_;
    MixerVoiceDriver driver_{mixer_};
    EffectDriver effectDriver_{mixer_, *this};
    AudioDevice device_;
    CarSoundContext context_;
    std::vector<uint16_t> pitchTable_, panTable_, velocityTable_;
    std::array<std::array<uint8_t, 2>, 10> effectPairs_{}; // EXE 0x8009160C
    InstBank system_, music_;
    std::vector<InstProgram> programs_;
    std::vector<SeqgFile> tracks_;                          // 0x80052A50[t]
    Sequencer sequencer_;
    int requested_ = -1, playing_ = -1, countdown_ = 0;
    bool loaded_ = false;
};

} // namespace gt2::audio
