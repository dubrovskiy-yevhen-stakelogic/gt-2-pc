#include "game/audio/music_player.h"

#include <algorithm>
#include <stdexcept>

namespace gt2::audio {
namespace {

constexpr uint32_t kFieldSamples = kSampleRate / 60; // the stream driver's fade tick 0x8007CF38 runs once per field
constexpr int32_t kFadeStep = 0x2000;                // script ops 6 / 7: +0x2000 / 0xE000 per tick (0x801F0522)
constexpr int32_t kFadeFull = 0x8000;

float Hermite(float xm1, float x0, float x1, float x2, float t) {
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * t + c2) * t + c1) * t + x0;
}

} // namespace

void MusicPlayer::Open(const std::string& discPath, const GuestImage& exe) {
    auto disc = std::make_unique<DiscImage>(discPath);
    const auto file = disc->FindRootFile("MUSIC.DAT");
    if (!file) throw std::runtime_error("music: MUSIC.DAT is not in the disc root");
    std::vector<MusicTrack> tracks = ReadMusicTable(exe);
    const uint32_t sectors = (file->size + DiscImage::kUserDataSize - 1) / DiscImage::kUserDataSize;
    for (MusicTrack& t : tracks) {
        // The European pressing's final end marker includes two padding sectors.
        if (t.first>=sectors || t.end > sectors + 2) throw std::runtime_error("music: a table entry ends past MUSIC.DAT");
        t.end=std::min(t.end,sectors);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    disc_ = std::move(disc);
    tracks_ = std::move(tracks);
    musicLba_ = file->lba;
    rawCache_.resize(32 * DiscImage::kRawSectorSize);
    decoded_.reserve(4032);
    source_.reserve(8064);
    cacheSectors_ = 0;
    state_ = State::kIdle;
    std::printf("music: %s, %zu tracks, MUSIC.DAT LBA %u\n", exe.fileName.c_str(), tracks_.size(), musicLba_);
}

void MusicPlayer::Play(int id, bool loop) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id < 0 || size_t(id) >= tracks_.size()) return;
    std::printf("music: play %d, XA channel %u, sectors %u..%u, gain %.4f\n", id,
        unsigned(MusicXaChannel(id)), tracks_[size_t(id)].first, tracks_[size_t(id)].end, double(cdVolume_)/32768.0);
    request_ = {true, id, loop};
    ended_ = false;
}

void MusicPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    request_ = {true, -1, false};
    ended_ = false;
}

bool MusicPlayer::Ended() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ended_ && !request_.pending;
}

void MusicPlayer::SetVolume(uint8_t musicVolume) {
    std::lock_guard<std::mutex> lock(mutex_);
    cdVolume_ = uint16_t(((uint32_t(musicVolume) * 0x8000u / 0xFFu) * 0x8CCu) >> 12); // 0x80080F24 -> 0x801F0528
}

void MusicPlayer::Start(int id, bool loop) {
    const MusicTrack& t = tracks_[size_t(id)];
    id_ = id;
    loop_ = loop;
    ended_ = false;
    lba_ = musicLba_ + t.first; // script 5: Setloc base + first, Setfilter (1, id + 1), Setmode 0x68, ReadS
    endLba_ = musicLba_ + t.end;
    channel_ = MusicXaChannel(id);
    decoder_ = XaDecoderState{};
    cacheSectors_ = 0;
    source_.clear();
    sourcePos_ = 0;
    phase_ = 0;
    for (auto& h : history_) h[0] = h[1] = 0;
    state_ = State::kPlaying;
    fade_ = 0;
    fadeRate_ = kFadeStep; // script op 6 after ReadS: fade in
}

bool MusicPlayer::DecodeNext() {
    while (lba_ < endLba_) {
        // XA channels are interleaved. Read ahead once for a block instead of
        // seeking through every other channel on the audio callback thread.
        if (!cacheSectors_ || lba_ < cacheLba_ || lba_ - cacheLba_ >= cacheSectors_) {
            cacheLba_ = lba_;
            cacheSectors_ = std::min<uint32_t>(32, endLba_ - lba_);
            disc_->ReadRawSectors(cacheLba_, cacheSectors_, rawCache_.data());
        }
        const uint8_t* raw = rawCache_.data() + size_t(lba_++ - cacheLba_) * DiscImage::kRawSectorSize;
        const XaSubheader h = XaSubheaderOf(raw);
        if (!IsXaAudio(h) || h.file != kMusicXaFile || h.channel != channel_) continue; // the drive's XA filter (mode 0x08)
        const XaFormat format = XaFormatOf(h.coding);
        step_ = double(format.sampleRate) / double(kSampleRate);
        decoded_.clear();
        DecodeXaSector(raw, decoder_, decoded_);
        source_.erase(source_.begin(), source_.begin() + std::ptrdiff_t(sourcePos_ * 2));
        sourcePos_ = 0;
        if (format.stereo) {
            source_.insert(source_.end(), decoded_.begin(), decoded_.end());
        } else {
            for (int16_t s : decoded_) { source_.push_back(s); source_.push_back(s); }
        }
        return true;
    }
    return false;
}

void MusicPlayer::SetPaused(bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = paused;
}

void MusicPlayer::MixStream(float* output, size_t frames) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!disc_ || paused_) return;
    for (size_t f = 0; f < frames; f++) {
        // Requests: the running track fades out first (script op 7 waits for level 0), then the new one starts.
        if (request_.pending) {
            if (state_ == State::kIdle || fade_ == 0) {
                request_.pending = false;
                if (request_.id >= 0) Start(request_.id, request_.loop);
                else state_ = State::kIdle;
            } else if (fadeRate_ >= 0) {
                fadeRate_ = -kFadeStep;
            }
        }
        // The end sector was reached and the fade-out is over: pause, then restart (loop) or stop.
        if (state_ == State::kEnding && fade_ == 0 && !request_.pending) {
            if (loop_) Start(id_, true);
            else { state_ = State::kIdle; ended_ = true; }
        }
        if (++fieldSamples_ >= kFieldSamples) { // 0x8007CF38
            fieldSamples_ = 0;
            if (fadeRate_ != 0) {
                fade_ = std::clamp(fade_ + fadeRate_, 0, kFadeFull);
                if (fade_ == 0 || fade_ == kFadeFull) fadeRate_ = 0;
            }
        }
        if (state_ == State::kIdle) continue;

        // Resample the channel's frames (4-point Hermite between history_[1] and history_[2]).
        phase_ += step_;
        while (phase_ >= 1.0) {
            phase_ -= 1.0;
            for (int i = 0; i < 3; i++) { history_[i][0] = history_[i + 1][0]; history_[i][1] = history_[i + 1][1]; }
            if (state_ == State::kPlaying && sourcePos_ * 2 >= source_.size() && !DecodeNext()) {
                state_ = State::kEnding; // 0x10 GetlocL: position >= end -> op 7 fade out, Pause
                if (fadeRate_ >= 0) fadeRate_ = -kFadeStep;
            }
            if (sourcePos_ * 2 < source_.size()) {
                history_[3][0] = float(source_[sourcePos_ * 2]);
                history_[3][1] = float(source_[sourcePos_ * 2 + 1]);
                sourcePos_++;
            } else {
                history_[3][0] = history_[3][1] = 0;
            }
        }
        const float t = float(phase_);
        // CD input volume (fn 0x8007C68C: volume * fade >> 15, both sides at 0x8000) against 1.0 = 0x8000.
        const float gain = float(cdVolume_) / 32768.0f * float(fade_) / 32768.0f / 32768.0f;
        for (int c = 0; c < 2; c++)
            output[f * 2 + size_t(c)] += Hermite(history_[0][c], history_[1][c], history_[2][c], history_[3][c], t) * gain;
    }
}

} // namespace gt2::audio
