// movie-check: the oracle of the native movie decoder (docs/formats/str_video.md section 7). Runs the original (Arcade
// disc) in the interpreter with its MDEC device (src/machine/mdec.*) and, every field:
//   - reads which movie the stream plays (the CD stream block 0x801EFF00 + 0x24 = start LBA, set by 0x800100F8 /
//     member 2 0x80025978);
//   - intro / endings (24-bit display): when the displayed picture changes, cuts the movie rectangle out of it (the
//     player's display offset: 0x80011328 y 24, 0x80011430 y 12, 0x800113A8 y 8) and looks for the native frame that
//     equals it (gt2formats/str_video.h: GtMovieReader -> GtFrameToMdecCodes -> DecodeMovieFrame);
//   - course previews (15-bit, VRAM (640, 256), member 2 0x80025B98): when the 112 x 96 rectangle changes, the same
//     with DecodeMovieFrame15;
//   - records the XA sectors the CD-ROM hands to the SPU and compares them (order, bytes) with the native reader's
//     audio sectors, and the runtime SPU's XA path with the native decoder (as gt2verify XaDecode).
// Usage: gt2run movie-check <disc> <fields> <outDir> "<script>" [pngs=N] [poke8=field:hexaddr:hexbyte[:count]]   (outputs under work\ only)
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2export/png_writer.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/str_video.h"
#include "gt2formats/xa_audio.h"
#include "input_script.h"
#include "machine/machine.h"

using namespace gt2;

namespace {

constexpr uint32_t kStreamBlockStart = 0x801EFF24u;

uint32_t SectorLba(const uint8_t* raw) {
    auto bcd = [](uint8_t v) { return uint32_t((v >> 4) * 10 + (v & 15)); };
    return (bcd(raw[12]) * 60 + bcd(raw[13])) * 75 + bcd(raw[14]) - 150;
}

struct NativeMovie {
    int number = -1;
    std::unique_ptr<GtMovieReader> reader;
    std::deque<std::pair<uint32_t, std::vector<uint8_t>>> window; // (frame number, picture: RGB24 or 15-bit words as bytes)
    uint32_t lastMatch = 0;
    bool ended = false;
};

} // namespace

int CmdMovieCheck(const DiscImage& disc, uint64_t fields, const std::string& outDir, const std::string& script,
                  const std::vector<std::string>& options) {
    int pngBudget = 4;
    struct Poke { uint64_t field; uint32_t address, count; uint8_t value; };
    std::vector<Poke> pokes; // DEV CAPTURE AID (as gt2play --poke): bytes written into guest RAM at a field, e.g. the career's unlock flags
    for (const std::string& o : options)
        if (o.rfind("pngs=", 0) == 0) pngBudget = std::atoi(o.c_str() + 5);
        else if (o.rfind("poke8=", 0) == 0) { // poke8=<field>:<hex address>:<hex byte>[:<count>]
            Poke p{};
            char* end = nullptr;
            p.field = std::strtoull(o.c_str() + 6, &end, 10);
            p.address = uint32_t(std::strtoul(end + 1, &end, 16));
            p.value = uint8_t(std::strtoul(end + 1, &end, 16));
            p.count = *end == ':' ? uint32_t(std::strtoul(end + 1, nullptr, 10)) : 1;
            pokes.push_back(p);
        }
        else throw std::runtime_error("unknown movie-check option: " + o);
    std::filesystem::create_directories(outDir);
    const GuestImage exe = LoadExeImage(disc);
    const std::vector<StreamMovie> movies = ReadStreamMovieTable(exe);
    const uint32_t streamLba = StreamFileLba(disc);
    const GtVlcTable vlc = ReadGtVlcTable(LoadOverlayImage(disc, 5), kMovieVlcTableAddress);
    const MdecCore mdec = MdecCoreFromExe(exe);

    const auto file = disc.FindRootFile(exe.fileName);
    if (!file) throw std::runtime_error("executable not found");
    std::vector<uint8_t> exeFile(file->size);
    disc.ReadForm1(file->lba, 0, exeFile.data(), exeFile.size());
    Machine m;
    m.AttachDisc(&disc);
    m.gpu.skip3dRaster = true;
    m.LoadExe(exeFile, 0x801FFF00);
    const std::vector<ScriptPress> presses = ParseInputScript(script);

    std::FILE* log = std::fopen((outDir + "/movie_check.txt").c_str(), "w");
    if (!log) throw std::runtime_error("cannot write " + outDir + "/movie_check.txt");
    std::fprintf(log, "script: %s\n", script.c_str());

    // XA sectors handed to the SPU, per movie.
    // One entry per play (a movie started again - a course chosen again - is a new play from its first sector).
    std::vector<std::pair<int, std::vector<std::vector<uint8_t>>>> xaByMovie;
    int playing = -1;
    m.onXaSector = [&](const uint8_t* raw) {
        if (playing < 0) return;
        if (xaByMovie.empty() || xaByMovie.back().first != playing) xaByMovie.emplace_back(playing, std::vector<std::vector<uint8_t>>{});
        xaByMovie.back().second.emplace_back(raw, raw + DiscImage::kRawSectorSize);
    };

    NativeMovie native;
    std::vector<uint8_t> lastPicture;
    struct Stats { uint64_t captures = 0, exact = 0, mismatched = 0, blank = 0; uint32_t firstFrame = 0, lastFrame = 0; };
    std::map<int, Stats> stats;
    int pngs = 0;

    auto nativePicture = [&](uint32_t index, bool course) -> const std::vector<uint8_t>* {
        while (!native.ended && (native.window.empty() || native.window.back().first < index)) {
            GtMovieReader::Frame f;
            if (!native.reader->NextFrame(f)) { native.ended = true; break; }
            const MdecFrameCodes codes = GtFrameToMdecCodes(f.data.data(), f.data.size(), vlc);
            std::vector<uint8_t> pic;
            if (course) {
                const std::vector<uint16_t> w = DecodeMovieFrame15(mdec, codes);
                pic.resize(w.size() * 2);
                std::memcpy(pic.data(), w.data(), pic.size());
            } else {
                pic = DecodeMovieFrame(mdec, codes).rgb;
            }
            native.window.emplace_back(f.number, std::move(pic));
            while (native.window.size() > 256) native.window.pop_front();
        }
        for (const auto& [n, pic] : native.window)
            if (n == index) return &pic;
        return nullptr;
    };

    for (uint64_t field = 1; field <= fields; field++) {
        m.padButtons = ScriptButtons(presses, field);
        for (const Poke& p : pokes)
            if (p.field == field) {
                for (uint32_t i = 0; i < p.count; i++) m.bus.Ram()[(p.address + i) & 0x1FFFFF] = p.value;
                std::fprintf(log, "f%llu poke %08X x%u = %02X (dev capture aid)\n", (unsigned long long)field, p.address, p.count, p.value);
            }
        const std::string reason = m.Run(Machine::kInstructionsPerVBlank);
        if (reason != "instruction budget exhausted") {
            std::fprintf(log, "guest stopped at field %llu: %s\n", (unsigned long long)field, reason.c_str());
            break;
        }
        uint32_t lba = 0;
        std::memcpy(&lba, m.bus.Ram() + (kStreamBlockStart & 0x1FFFFF), 4);
        int movie = -1;
        for (int i = 0; i < kArcadeMovieCount; i++)
            if (lba == streamLba + movies[size_t(i)].first) movie = i;
        if (movie != playing) {
            std::fprintf(log, "f%llu stream start LBA %u: movie %d\n", (unsigned long long)field, lba, movie);
            playing = movie;
            if (movie >= 0) xaByMovie.emplace_back(movie, std::vector<std::vector<uint8_t>>{});
            if (movie >= 0 && movie != native.number) {
                native = NativeMovie{};
                native.number = movie;
                native.reader = std::make_unique<GtMovieReader>(disc, streamLba, movies[size_t(movie)]);
            }
            // lastPicture is kept: the previous movie's last picture stays on screen / in VRAM until the new one's first.
        }
        if (movie < 0) continue;
        const bool course = movie < kMovieIntro;
        const int width = course ? 112 : (movie == kMovieIntro ? 320 : 640);
        const int height = course ? 96 : (movie == kMovieIntro ? 192 : movie == kMovieEndingA ? 216 : 224);
        const int offsetY = movie == kMovieIntro ? 24 : movie == kMovieEndingA ? 12 : 8;
        std::vector<uint8_t> picture;
        if (course) {
            const auto& vram = m.gpu.Vram();
            picture.resize(size_t(width) * size_t(height) * 2);
            for (int y = 0; y < height; y++) std::memcpy(&picture[size_t(y) * size_t(width) * 2], &vram[size_t(256 + y) * 1024 + 640], size_t(width) * 2);
        } else {
            if (!m.gpu.Display24()) continue;
            int w = 0, h = 0;
            const std::vector<uint8_t> rgba = m.gpu.DisplayRgba(w, h);
            if (w < width || h < offsetY + height) continue;
            picture.resize(size_t(width) * size_t(height) * 3);
            for (int y = 0; y < height; y++)
                for (int x = 0; x < width; x++) std::memcpy(&picture[(size_t(y) * size_t(width) + size_t(x)) * 3], &rgba[(size_t(y + offsetY) * size_t(w) + size_t(x)) * 4], 3);
        }
        if (picture == lastPicture) continue;
        lastPicture = picture;
        Stats& st = stats[movie];
        if (std::all_of(picture.begin(), picture.end(), [](uint8_t b) { return b == 0; })) {
            st.blank++;
            continue;
        }
        st.captures++;
        // The native frame equal to the capture: frames after the previous match first (the display never goes back).
        int64_t best = -1;
        size_t bestDiff = SIZE_MAX;
        uint32_t bestIndex = 0;
        int bestMaxDelta = 0;
        for (uint32_t k = native.lastMatch; k <= native.lastMatch + 200; k++) { // the display skips frames, never goes back
            const std::vector<uint8_t>* pic = nativePicture(k, course);
            if (!pic) continue;
            size_t diff = 0;
            int maxDelta = 0;
            const size_t px = course ? 2 : 3;
            for (size_t i = 0; i < pic->size(); i += px)
                if (std::memcmp(&(*pic)[i], &picture[i], px) != 0) {
                    diff++;
                    if (!course)
                        for (size_t c = 0; c < 3; c++) maxDelta = std::max(maxDelta, std::abs(int((*pic)[i + c]) - int(picture[i + c])));
                }
            if (diff < bestDiff) { bestDiff = diff; bestIndex = k; bestMaxDelta = maxDelta; }
            if (diff == 0) { best = k; break; }
        }
        if (best < 0) { // the stream may have started over (the course previews loop: member 2 restarts the same LBA range)
            NativeMovie again;
            again.number = movie;
            again.reader = std::make_unique<GtMovieReader>(disc, streamLba, movies[size_t(movie)]);
            std::swap(native, again);
            for (uint32_t k = 1; k <= 60 && best < 0; k++) {
                const std::vector<uint8_t>* pic = nativePicture(k, course);
                if (pic && *pic == picture) best = k;
            }
            if (best >= 0) std::fprintf(log, "f%llu movie %d: the stream started over (frame %lld)\n", (unsigned long long)field, movie, (long long)best);
            else std::swap(native, again);
        }
        if (best < 0 && bestDiff < picture.size() / (course ? 4 : 6)) native.lastMatch = bestIndex; // follow the closest frame
        if (best >= 0) {
            st.exact++;
            if (!st.firstFrame) st.firstFrame = uint32_t(best);
            st.lastFrame = uint32_t(best);
            if (uint32_t(best) != native.lastMatch + 1 && native.lastMatch)
                std::fprintf(log, "f%llu movie %d: frame %lld (previous match %u)\n", (unsigned long long)field, movie, (long long)best, native.lastMatch);
            native.lastMatch = uint32_t(best);
        } else {
            st.mismatched++;
            std::fprintf(log, "f%llu movie %d: NO native frame equals the capture (closest frame %u, %zu pixels differ, max channel difference %d)\n",
                         (unsigned long long)field, movie, bestIndex, bestDiff, bestMaxDelta);
            if (pngs < pngBudget && !course) {
                std::vector<uint8_t> rgba(size_t(width) * size_t(height) * 4 * 2, 255);
                const std::vector<uint8_t>* pic = nativePicture(bestIndex, false);
                for (size_t i = 0; i < size_t(width) * size_t(height); i++) {
                    std::memcpy(&rgba[i * 4], &picture[i * 3], 3);
                    if (pic) std::memcpy(&rgba[(size_t(width) * size_t(height) + i) * 4], &(*pic)[i * 3], 3);
                }
                WritePngRgba(outDir + "/mismatch_" + std::to_string(field) + ".png", width, height * 2, rgba);
                pngs++;
            }
        }
    }

    int failures = 0;
    for (const auto& [movie, st] : stats) {
        std::printf("movie %d: %llu displayed pictures, %llu equal to a native frame (frames %u..%u), %llu differ, %llu blank\n", movie,
                    (unsigned long long)st.captures, (unsigned long long)st.exact, st.firstFrame, st.lastFrame,
                    (unsigned long long)st.mismatched, (unsigned long long)st.blank);
        std::fprintf(log, "movie %d: %llu displayed pictures, %llu equal, %llu differ, %llu blank\n", movie, (unsigned long long)st.captures,
                     (unsigned long long)st.exact, (unsigned long long)st.mismatched, (unsigned long long)st.blank);
        if (st.mismatched || !st.exact) failures++;
    }

    // Audio: the sectors the CD handed to the SPU against the native reader's (channel 7 of file 1, in movie order).
    for (const auto& [movie, sectors] : xaByMovie) {
        if (sectors.empty()) continue; // a play that ended before its first audio sector
        std::vector<uint32_t> nativeLbas;
        const StreamMovie& mv = movies[size_t(movie)];
        uint8_t raw[DiscImage::kRawSectorSize];
        for (uint32_t s = 0; s < mv.sectors; s++) {
            disc.ReadRawSector(streamLba + mv.first + s, raw);
            const XaSubheader h = XaSubheaderOf(raw);
            if (IsXaAudio(h) && h.file == 1 && h.channel == 7) nativeLbas.push_back(streamLba + mv.first + s);
        }
        // The CD stream stops by polling its position (GetlocL >= end LBA), so it may hand over a sector behind the movie's
        // last one (the 25 sectors between two movies): counted apart, not played natively.
        const uint32_t endLba = streamLba + mv.first + mv.sectors;
        size_t inRange = 0, sameOrder = 0;
        for (const auto& sec : sectors)
            if (SectorLba(sec.data()) < endLba) inRange++;
        // In order = the native list, again from its start after its end (the looping previews); past-the-end ones skipped.
        for (const auto& sec : sectors) {
            const uint32_t lba = SectorLba(sec.data());
            if (lba >= endLba) continue;
            if (nativeLbas.empty() || lba != nativeLbas[sameOrder % nativeLbas.size()]) break;
            sameOrder++;
        }
        // The runtime SPU's XA path (its own ADPCM decoder + linear resampling) against DecodeXaSector + the same
        // resampling and volume steps, on every recorded sector (gt2verify XaDecode's comparison).
        Spu spu;
        spu.Write(0x1AA, 0xC001);
        spu.Write(0x180, 0x3FFF);
        spu.Write(0x182, 0x3FFF);
        spu.Write(0x1B0, 0x7FFF);
        spu.Write(0x1B2, 0x7FFF);
        XaDecoderState state;
        std::vector<int16_t> decoded;
        std::vector<int32_t> ours;
        double phase = 0;
        int16_t last[2] = {0, 0};
        size_t compared = 0, differ = 0, spuFrames = 0;
        std::vector<int16_t> runtime;
        for (size_t i = 0; i < sectors.size(); i++) {
            spu.PushXaSector(sectors[i].data());
            decoded.clear();
            DecodeXaSector(sectors[i].data(), state, decoded);
            for (size_t k = 0; k + 1 < decoded.size(); k += 2) {
                while (phase < 1.0) {
                    for (int c = 0; c < 2; c++) {
                        const int32_t v = int16_t(last[c] + (decoded[k + size_t(c)] - last[c]) * phase);
                        ours.push_back(((v * 0x7FFF) >> 15) * 0x7FFE >> 15);
                    }
                    phase += 37800.0 / Spu::kSampleRate;
                }
                phase -= 1.0;
                last[0] = decoded[k];
                last[1] = decoded[k + 1];
            }
            if (i >= 1) { // keep the SPU's CD buffer ahead of its output
                const size_t target = ours.size() / 2 - 2048;
                if (target > spuFrames) {
                    spu.output.clear();
                    spu.Generate(target - spuFrames);
                    runtime.insert(runtime.end(), spu.output.begin(), spu.output.end());
                    spuFrames = target;
                }
            }
        }
        for (size_t i = 0; i < std::min(ours.size(), runtime.size()); i++, compared++)
            if (ours[i] != runtime[i]) differ++;
        std::printf("movie %d audio: %zu XA sectors to the SPU (%zu past the movie's end), %zu in the native reader's order (native %zu); "
                    "runtime SPU vs native decoder: %zu samples, %zu differ\n",
                    movie, sectors.size(), sectors.size() - inRange, sameOrder, nativeLbas.size(), compared, differ);
        std::fprintf(log, "movie %d audio: %zu sectors (%zu past the end), %zu in order, %zu samples compared, %zu differ\n", movie, sectors.size(),
                     sectors.size() - inRange, sameOrder, compared, differ);
        if (sameOrder != inRange || differ || !compared) failures++;
    }
    std::printf("MDEC: %llu commands, %llu macroblocks, %llu reads past the output\n", (unsigned long long)m.mdec.commands,
                (unsigned long long)m.mdec.macroblocks, (unsigned long long)m.mdec.underflows);
    std::fclose(log);
    std::printf("movie-check: %s (log %s/movie_check.txt)\n", failures ? "FAIL" : "ok", outDir.c_str());
    return failures ? 1 : 0;
}
