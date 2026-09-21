#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "game/audio/mixer.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/xa_audio.h"
#include "gt2vfs/disc_image.h"

// The race music: XA-ADPCM streamed from MUSIC.DAT on the user's disc image, in the semantics of the executable's CD
// stream driver (docs/formats/sound.md section 6): track `id` = XA file 1, channel id + 1, sectors [base + first,
// base + end) of the table 0x800959C0; the CD input volume (0x80080F24: (musicVolume * 0x8000 / 0xFF) * 0x8CC >> 12,
// 0x423B for the default 0xF0); a start fades the previous track out and the new one in (0x2000 of 0x8000 per 60 Hz
// field, 0x8007CF38); at the end sector the track fades out and either restarts from its first sector (loop flag,
// script 0x80090450 jump 0x80000003) or stops and reports Ended (0x801F0674).
// The player reads the disc through its own DiscImage (a separate file handle) on the audio device thread, decodes
// with gt2formats/xa_audio.h and resamples 37800 / 18900 Hz to 44100 Hz (4-point Hermite; the SPU's own zigzag
// interpolation is not reproduced).
namespace gt2::audio {

class MusicPlayer final : public StreamSource {
public:
    MusicPlayer() = default;
    ~MusicPlayer() override = default;

    // Opens the image a second time, finds MUSIC.DAT in the ISO root and reads the track table from the executable.
    // Throws std::runtime_error when the disc has no MUSIC.DAT or the table is malformed.
    void Open(const std::string& discPath, const GuestImage& exe);
    size_t TrackCount() const { return tracks_.size(); }
    const MusicTrack& Track(size_t id) const { return tracks_[id]; }
    uint32_t MusicLba() const { return musicLba_; }

    // 0x80080F24 (+ command 5): plays track `id` (loop = the task's + 0x2ED byte).
    void Play(int id, bool loop);
    // 0x8007C570: fades out and stops.
    void Stop();
    // Silence output without advancing the decoder, fade or playback cursor.
    void SetPaused(bool paused);
    // A non-looping track has played to its end sector (0x801F0674) and nothing else was requested since.
    bool Ended() const;
    // 0x801C9993: the music volume of the options (0..255; 0xF0 in every dump).
    void SetVolume(uint8_t musicVolume);

    // StreamSource: the device thread's pull (adds into `output`).
    void MixStream(float* output, size_t frames) override;

private:
    enum class State { kIdle, kPlaying, kEnding };
    struct Request { bool pending = false; int id = -1; bool loop = false; };

    bool DecodeNext();           // the next sector of the channel into source_; false at the track's end
    void Start(int id, bool loop);

    std::unique_ptr<DiscImage> disc_;
    std::vector<MusicTrack> tracks_;
    uint32_t musicLba_ = 0;
    mutable std::mutex mutex_;

    Request request_;
    State state_ = State::kIdle;
    int id_ = -1;
    bool loop_ = false, ended_ = false, paused_ = false;
    uint32_t lba_ = 0, endLba_ = 0;
    uint8_t channel_ = 0;
    XaDecoderState decoder_;
    std::vector<uint8_t> rawCache_;
    std::vector<int16_t> decoded_;
    uint32_t cacheLba_ = 0, cacheSectors_ = 0;
    std::vector<int16_t> source_;       // decoded stereo frames at the source rate
    size_t sourcePos_ = 0;              // next unread frame in source_
    double phase_ = 0, step_ = 37800.0 / 44100.0;
    float history_[4][2] = {};          // the resampler's last four source frames
    int32_t fade_ = 0x8000, fadeRate_ = 0; // 0x801F052E level (0..0x8000), 0x801F0522 rate per field
    uint32_t fieldSamples_ = 0;
    uint16_t cdVolume_ = 0x423B;        // 0x801F0528
};

} // namespace gt2::audio
