#include "movie_player.h"
#include "pc_overlay.h"
#include "gt2formats/hd_media.h"
#include <atomic>
#include <condition_variable>
#include <thread>

#include "platform/os/keys.h"
#include "platform/os/paths.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>

#include "game/audio/audio_device.h"
#include "game/audio/mixer.h"
#include "game_window.h"
#include "gt2formats/overlay_data.h"
#include "gt2view/movie_view.h"
#include "platform/input/ps1_pad.h"

using namespace gt2;

namespace gt2game {
namespace {

// The drive reads the stream at double speed: 150 sectors per second, 150 * 1001 / 60000 per NTSC field.
constexpr double kSectorsPerField = 150.0 * 1001.0 / 60000.0;
constexpr size_t kRingFrames = 32; // 0x80010D38 / 0x80010078: 32 frame slots between the CD callback and the decoder

// The stream's XA audio (file 1, channel 7) as the SPU's CD input: read and decoded on the device thread through its own
// disc handle, linear interpolation 37800 -> 44100 Hz (the runtime SPU's), the CD volume 0x8000 of the stream block
// (0x800100F8: + 0x18) with the stream script's fade-in of 0x2000 per field (command 5, as the race music).
class MovieAudio final : public audio::StreamSource {
public:
    MovieAudio(const std::string& discPath, uint32_t firstLba, uint32_t sectors)
        : disc_(std::make_unique<DiscImage>(discPath)), lba_(firstLba), end_(firstLba + sectors) {}

    void MixStream(float* output, size_t frames) override {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t f = 0; f < frames; f++) {
            if (++fieldSamples_ >= audio::kSampleRate / 60) {
                fieldSamples_ = 0;
                fade_ = std::min(fade_ + 0x2000, 0x8000);
            }
            phase_ += 37800.0 / audio::kSampleRate;
            while (phase_ >= 1.0) {
                phase_ -= 1.0;
                previous_[0] = next_[0];
                previous_[1] = next_[1];
                if (pos_ * 2 >= source_.size() && !Decode()) { next_[0] = next_[1] = 0; continue; }
                next_[0] = source_[pos_ * 2];
                next_[1] = source_[pos_ * 2 + 1];
                pos_++;
            }
            const float gain = float(fade_) / 32768.0f / 32768.0f;
            for (int c = 0; c < 2; c++)
                output[f * 2 + size_t(c)] += (float(previous_[c]) + (float(next_[c]) - float(previous_[c])) * float(phase_)) * gain;
        }
    }

private:
    bool Decode() {
        uint8_t raw[DiscImage::kRawSectorSize];
        source_.clear();
        pos_ = 0;
        while (lba_ < end_) {
            disc_->ReadRawSector(lba_++, raw);
            const XaSubheader h = XaSubheaderOf(raw);
            if (IsXaAudio(h) && h.file == 1 && h.channel == 7) {
                DecodeXaSector(raw, state_, source_);
                return true;
            }
        }
        return false;
    }

    std::mutex mutex_;
    std::unique_ptr<DiscImage> disc_;
    uint32_t lba_, end_;
    XaDecoderState state_;
    std::vector<int16_t> source_;
    size_t pos_ = 0;
    double phase_ = 0;
    int16_t previous_[2] = {}, next_[2] = {};
    int32_t fade_ = 0;
    uint32_t fieldSamples_ = 0;
};

} // namespace

std::unique_ptr<MovieLibrary> MovieLibrary::Load(const DiscImage& disc, const std::string& discPath) {
    if (!disc.FindRootFile("STREAM.DAT")) return nullptr;
    auto lib = std::make_unique<MovieLibrary>();
    const GuestImage exe = LoadExeImage(disc);
    lib->discPath = discPath;
    lib->movies = ReadStreamMovieTable(exe);
    lib->streamLba = StreamFileLba(disc);
    lib->vlc = ReadGtVlcTable(LoadOverlayImage(disc, 5), kMovieVlcTableAddress);
    lib->mdec = MdecCoreFromExe(exe);
    lib->courseMovies = ReadCourseMovieTable(LoadOverlayImage(disc, 2));
    return lib;
}

MovieSpec MovieSpecOf(int movie) {
    MovieSpec s;
    s.movie = movie;
    if (movie == kMovieEndingA || movie == kMovieEndingB) { // 0x80011430 / 0x800113A8
        s.fieldsPerFrame = 4;
        s.displayWidth = 640;
        s.y = movie == kMovieEndingA ? 12 : 8;
        s.skippable = false;
        s.prebuffer = 0;
    }
    return s;
}

MovieResult PlayPreparedMovie(GameWindow& window, const std::string& path, const MovieSpec& spec, bool sound) {
    hd::Movie movie(path);
    std::printf("HD movie: %ux%u, %u frames, %u/%u fps, skip %s\n", movie.width,movie.height,movie.frames,movie.fpsNum,movie.fpsDen,spec.skippable?"enabled":"disabled");
    class Pcm final : public audio::StreamSource {
    public:
        std::vector<int16_t> samples;
        std::atomic<uint64_t> position{0};
        void MixStream(float* out, size_t frames) override {
            const uint64_t start = position.load(std::memory_order_relaxed);
            for (size_t i = 0; i < frames * 2; ++i)
                if (start * 2 + i < samples.size()) out[i] += float(samples[size_t(start * 2 + i)]) / 32768.0f;
            position.store(start + frames, std::memory_order_relaxed);
        }
    } pcm;
    if (sound) pcm.samples = movie.Audio();
    gt2view::MovieView view(window.Renderer());
    auto first = movie.Frame(0);
    view.Upload(first.rgb.data(), int(first.width), int(first.height), int(movie.sourceWidth), int(movie.sourceHeight));
    PrepareNativeUi(window.Renderer());
    auto& renderer = window.Renderer();
    const float oldClear[] = {renderer.clearColor[0], renderer.clearColor[1], renderer.clearColor[2]};
    struct Restore {
        gt2view::VkSceneRenderer& renderer; const float* value;
        ~Restore() { std::copy(value, value + 3, renderer.clearColor); }
    } restore{renderer, oldClear};
    renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<std::pair<uint32_t, hd::Picture>> queue;
    std::atomic<uint32_t> wanted{1};
    bool stop = false;
    std::exception_ptr failure;
    std::thread decoder([&] {
        try {
            for (uint32_t i = 1; i < movie.frames; ++i) {
                std::unique_lock lock(mutex);
                changed.wait(lock, [&] { return stop || queue.size() < 3; });
                if (stop) return;
                lock.unlock();
                i = std::max(i, wanted.load(std::memory_order_relaxed));
                if (i >= movie.frames) return;
                auto picture = movie.Frame(i);
                lock.lock(); queue.emplace_back(i, std::move(picture));
            }
        } catch (...) { std::lock_guard lock(mutex); failure = std::current_exception(); }
    });
    struct Join {
        std::mutex& mutex; std::condition_variable& changed; bool& stop; std::thread& thread;
        ~Join() { { std::lock_guard lock(mutex); stop = true; } changed.notify_all(); thread.join(); }
    } join{mutex, changed, stop, decoder};
    // Device dies before the mixer/PCM source and decoder on every exit path.
    audio::Mixer mixer;
    audio::AudioDevice device;
    std::string error;
    mixer.SetStream(&pcm);
    const bool audioClock = sound && !pcm.samples.empty() && device.Open(mixer, error);
    auto previous = window.DisplayTime();
    double elapsed = 0;
    window.ResetPacing();
    MovieResult result = MovieResult::kFinished;
    while (window.BeginFrame()) {
        if (spec.skippable && (window.PadPressed(input::ps1::kStart | input::ps1::kCross | input::ps1::kTriangle) ||
            window.KeyPressed(keys::kReturn) || window.KeyPressed('S') || window.KeyPressed(keys::kEscape))) {
            result = MovieResult::kSkipped; break;
        }
        const auto now = window.DisplayTime();
        elapsed += std::clamp(std::chrono::duration<double>(now - previous).count(), 0.0, 0.1);
        previous = now;
        const double seconds = audioClock ? double(pcm.position.load()) / audio::kSampleRate : elapsed;
        const uint32_t target = uint32_t(seconds * movie.fpsNum / movie.fpsDen);
        if (target >= movie.frames) break;
        wanted.store(target, std::memory_order_relaxed);
        hd::Picture picture;
        { std::lock_guard lock(mutex);
            if (failure) std::rethrow_exception(failure);
            while (!queue.empty() && queue.front().first <= target) { picture = std::move(queue.front().second); queue.pop_front(); }
        }
        changed.notify_one();
        if (!picture.rgb.empty()) view.Upload(picture.rgb.data(), int(picture.width), int(picture.height), int(movie.sourceWidth), int(movie.sourceHeight));
        std::vector<gt2view::DrawItem> items;
        view.Append(items, spec.displayWidth, spec.displayHeight, spec.x, spec.y, renderer.AspectRatio());
        if (spec.skippable) AppendSkipHint(renderer, items);
        window.EndFrame(items, {}, std::chrono::nanoseconds(16'683'333));
    }
    device.Close(); mixer.SetStream(nullptr);
    if (window.Closed()) result = MovieResult::kClosed;
    window.ResetPacing();
    return result;
}

MovieResult PlayMovie(GameWindow& window, const DiscImage& disc, const MovieLibrary& library, int movie, bool sound) {
    if (movie < 0 || size_t(movie) >= library.movies.size()) throw std::runtime_error("movie: no movie " + std::to_string(movie));
    MovieSpec spec = MovieSpecOf(movie);
    if (VrMode()) spec.skippable = true;
    const auto prepared = gt2::hd::Asset("movies/" + std::to_string(movie) + ".gtm");
    if (!prepared.empty()) {
        try { return PlayPreparedMovie(window, prepared, spec, sound); }
        catch (const std::exception& e) { std::printf("HD movie fallback: %s\n", e.what()); }
    }
    if (spec.skippable) PrepareNativeUi(window.Renderer());
    const StreamMovie& m = library.movies[size_t(movie)];
    GtMovieReader reader(disc, library.streamLba, m);
    gt2view::MovieView view(window.Renderer());
    auto& renderer = window.Renderer();
    renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0.0f;

    std::unique_ptr<audio::Mixer> mixer;
    std::unique_ptr<audio::AudioDevice> device;
    std::unique_ptr<MovieAudio> stream;
    if (sound) {
        try {
            mixer = std::make_unique<audio::Mixer>();
            stream = std::make_unique<MovieAudio>(library.discPath, library.streamLba + m.first, m.sectors);
            mixer->SetStream(stream.get());
            device = std::make_unique<audio::AudioDevice>();
            std::string error;
            if (!device->Open(*mixer, error)) {
                std::printf("movie: no sound device (%s)\n", error.c_str());
                device.reset();
            }
        } catch (const std::exception& e) {
            std::printf("movie: sound disabled (%s)\n", e.what());
            device.reset();
        }
    }
    auto stopSound = [&] {
        if (device) device->Close();
        device.reset();
        if (mixer) mixer->SetStream(nullptr);
    };

    std::printf("movie %d: %u sectors, %dx%d display, a frame every %d fields%s\n", movie, m.sectors, spec.displayWidth,
                spec.displayHeight, spec.fieldsPerFrame, spec.skippable ? ", Start skips" : "");
    std::deque<GtMovieReader::Frame> ring;
    double clock = 0;
    int cadence = 0, shown = 0, dropped = 0, tail = 0;
    bool started = spec.prebuffer == 0;
    MovieResult result = MovieResult::kFinished;
    window.ResetPacing();
    for (;;) {
        if (!window.BeginFrame()) { result = MovieResult::kClosed; break; }
        if (spec.skippable && (window.PadPressed(input::ps1::kStart | input::ps1::kCross | input::ps1::kTriangle) || window.KeyPressed(gt2::keys::kReturn) || window.KeyPressed('S') || window.KeyPressed(gt2::keys::kEscape))) {
            result = MovieResult::kSkipped;
            break;
        }
        // The CD stream: the sectors due by this field; the sector callback's ring of frames (a full ring drops the frame).
        clock += kSectorsPerField;
        while (!reader.AtEnd() && double(reader.SectorsRead()) < clock) {
            GtMovieReader::Frame f;
            if (!reader.ReadSector(f)) continue;
            if (ring.size() >= kRingFrames) { dropped++; continue; }
            ring.push_back(std::move(f));
        }
        reader.Audio().clear(); // the device thread plays the audio from its own reader
        if (!started && int(ring.size()) >= spec.prebuffer) started = true;
        // The display: a new frame every `fieldsPerFrame` fields (0x800111D4), when one is decoded.
        if (started && ++cadence >= spec.fieldsPerFrame) {
            cadence = 0;
            if (!ring.empty()) {
                const MdecFrameCodes codes = GtFrameToMdecCodes(ring.front().data.data(), ring.front().data.size(), library.vlc);
                ring.pop_front();
                const MovieImage img = DecodeMovieFrame(library.mdec, codes);
                view.Upload(img.rgb.data(), img.width, img.height);
                shown++;
            } else if (reader.AtEnd() && ++tail >= 1) {
                break;
            }
        }
        std::vector<gt2view::DrawItem> items;
        view.Append(items, spec.displayWidth, spec.displayHeight, spec.x, spec.y, renderer.AspectRatio());
        if (spec.skippable) AppendSkipHint(renderer, items);
        window.EndFrame(items, {}, std::chrono::nanoseconds(16'683'333));
    }
    stopSound();
    std::printf("movie %d: %d frames shown, %d dropped, %s\n", movie, shown, dropped,
                result == MovieResult::kSkipped ? "skipped" : result == MovieResult::kClosed ? "window closed" : "finished");
    return result;
}

// ---------------------------------------------------------------- course preview

void CoursePreview::Start(int index) {
    Stop();
    if (index < 0 || size_t(index) >= library_.courseMovies.size()) return;
    movie_ = library_.courseMovies[size_t(index)];
    if (size_t(movie_) >= library_.movies.size()) { movie_ = -1; return; }
    Open();
}

void CoursePreview::Stop() {
    reader_.reset();
    movie_ = -1;
    picture_.clear();
    version_++;
}

void CoursePreview::Open() {
    reader_ = std::make_unique<GtMovieReader>(disc_, library_.streamLba, library_.movies[size_t(movie_)]);
    clock_ = 0;
}

void CoursePreview::Tick() {
    if (!reader_) return;
    clock_ += kSectorsPerField;
    GtMovieReader::Frame last;
    bool any = false;
    while (!reader_->AtEnd() && double(reader_->SectorsRead()) < clock_) {
        GtMovieReader::Frame f;
        if (reader_->ReadSector(f)) {
            last = std::move(f);
            any = true;
        }
    }
    reader_->Audio().clear();
    if (any) {
        picture_ = DecodeMovieFrame15(library_.mdec, GtFrameToMdecCodes(last.data.data(), last.data.size(), library_.vlc));
        version_++;
    }
    if (reader_->AtEnd()) Open(); // the preview starts over (the same sectors again)
}

} // namespace gt2game
