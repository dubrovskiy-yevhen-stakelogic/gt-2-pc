// gt2tool commands for the movies of the Arcade disc (docs/formats/str_video.md): str-info, str-export.
#include "movie_cmds.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2export/png_deflate.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/str_video.h"

namespace gt2 {
namespace {

// "<file>" = STREAM.DAT (all movies) or STREAM.DAT:<n> / <n> (one movie).
int ParseMovieSelector(const std::string& s) {
    const size_t colon = s.find(':');
    std::string name = colon == std::string::npos ? s : s.substr(0, colon);
    std::string number = colon == std::string::npos ? std::string() : s.substr(colon + 1);
    if (!name.empty() && std::isdigit(static_cast<unsigned char>(name[0]))) { number = name; name = "STREAM.DAT"; }
    for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
    if (name != "STREAM.DAT") throw std::runtime_error("movies live in STREAM.DAT (got '" + s + "')");
    if (number.empty()) return -1;
    const int n = std::atoi(number.c_str());
    if (n < 0 || n >= kArcadeMovieCount) throw std::runtime_error("movie number 0.." + std::to_string(kArcadeMovieCount - 1));
    return n;
}

void WriteWav(const std::string& path, const std::vector<int16_t>& samples, int rate, int channels) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    const uint32_t bytes = uint32_t(samples.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    u32(36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16);
    u16(1);
    u16(uint16_t(channels));
    u32(uint32_t(rate));
    u32(uint32_t(rate * channels * 2));
    u16(uint16_t(channels * 2));
    u16(16);
    std::fwrite("data", 1, 4, f);
    u32(bytes);
    std::fwrite(samples.data(), 2, samples.size(), f);
    std::fclose(f);
}

const char* MovieRole(int n) {
    if (n < 24) return "course preview";
    if (n == kMovieIntro) return "intro (boot)";
    return "ending";
}

} // namespace

int CmdStrInfo(const DiscImage& disc, int argc, char** argv) {
    if (argc < 4) throw std::runtime_error("str-info <disc> <STREAM.DAT[:n]> [--decode]");
    const int only = ParseMovieSelector(argv[3]);
    bool decode = false;
    for (int i = 4; i < argc; i++)
        if (std::string(argv[i]) == "--decode") decode = true;
    const GuestImage exe = LoadExeImage(disc);
    const std::vector<StreamMovie> movies = ReadStreamMovieTable(exe);
    const uint32_t lba = StreamFileLba(disc);
    const GuestImage player = LoadOverlayImage(disc, 5), menus = LoadOverlayImage(disc, 2);
    const GtVlcTable vlc = ReadGtVlcTable(player, kMovieVlcTableAddress);
    const GtVlcTable vlcMenus = ReadGtVlcTable(menus, kCourseMovieVlcTableAddress);
    std::printf("STREAM.DAT at LBA %u; movie table 0x%08X (%s); AC tables member 5 0x%08X / member 2 0x%08X %s\n", lba,
                kArcadeMovieTableAddress, exe.fileName.c_str(), kMovieVlcTableAddress, kCourseMovieVlcTableAddress,
                vlc.entries == vlcMenus.entries ? "equal" : "DIFFER");
    const std::vector<uint16_t> courses = ReadCourseMovieTable(menus);
    std::printf("course -> movie (member 2 0x%08X):", kCourseMovieTableAddress);
    for (uint16_t m : courses) std::printf(" %u", m);
    std::printf("\n");
    const MdecCore mdec = MdecCoreFromExe(exe);
    int failures = 0;
    for (int n = 0; n < kArcadeMovieCount; n++) {
        if (only >= 0 && n != only) continue;
        GtMovieReader reader(disc, lba, movies[size_t(n)]);
        GtMovieReader::Frame frame;
        uint32_t frames = 0, expected = 1, gaps = 0, oddWords = 0, maxChunks = 0, frameCount = 0;
        int width = 0, height = 0;
        size_t decoded = 0;
        const auto t0 = std::chrono::steady_clock::now();
        while (reader.NextFrame(frame)) {
            frames++;
            frameCount = frame.frameCount;
            if (frame.number != expected) gaps++;
            expected = frame.number + 1;
            maxChunks = std::max<uint32_t>(maxChunks, uint32_t(frame.data.size() / kGtVideoChunkBytes));
            if (frame.data.size() >= 8) {
                width = frame.data[4] | (frame.data[5] << 8);
                height = frame.data[6] | (frame.data[7] << 8);
                if ((frame.data[0] | (frame.data[1] << 8)) % 32) oddWords++;
            }
            if (decode) {
                try {
                    const MdecFrameCodes codes = GtFrameToMdecCodes(frame.data.data(), frame.data.size(), vlc);
                    const MovieImage img = DecodeMovieFrame(mdec, codes);
                    decoded += img.rgb.empty() ? 0 : 1;
                } catch (const std::exception& e) {
                    if (failures++ < 10) std::printf("  movie %d frame %u: %s\n", n, frame.number, e.what());
                }
            }
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const double audioSeconds = double(reader.Audio().size() / (reader.AudioStereo() ? 2 : 1)) / reader.AudioRate();
        std::printf("movie %2d (%s): sectors %u..%u (%u), %u / %u frames (%u out of order), %dx%d, <= %u chunks, audio %u sectors "
                    "%.2f s (%s %d Hz), %.2f sectors per frame, %u frames with a word count not a multiple of 32",
                    n, MovieRole(n), movies[size_t(n)].first, movies[size_t(n)].first + movies[size_t(n)].sectors - 1,
                    movies[size_t(n)].sectors, frames, frameCount, gaps, width, height, maxChunks, reader.AudioSectors(), audioSeconds,
                    reader.AudioStereo() ? "stereo" : "mono", reader.AudioRate(), frames ? double(reader.SectorsRead()) / frames : 0.0, oddWords);
        if (decode) std::printf(", %zu decoded in %.1f s", decoded, seconds);
        std::printf("\n");
    }
    return failures ? 1 : 0;
}

int CmdStrExport(const DiscImage& disc, int argc, char** argv) {
    if (argc < 5) throw std::runtime_error("str-export <disc> <STREAM.DAT:n> <outDir> [--frames a-b] [--step n] [--no-audio]");
    const int n = ParseMovieSelector(argv[3]);
    if (n < 0) throw std::runtime_error("str-export: name one movie (STREAM.DAT:<n>)");
    const std::filesystem::path out = argv[4];
    uint32_t from = 1, to = 0xFFFFFFFFu, step = 1;
    bool audio = true;
    for (int i = 5; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) {
            const std::string r = argv[++i];
            const size_t dash = r.find('-');
            from = uint32_t(std::strtoul(r.c_str(), nullptr, 10));
            to = dash == std::string::npos ? from : uint32_t(std::strtoul(r.c_str() + dash + 1, nullptr, 10));
        } else if (a == "--step" && i + 1 < argc) {
            step = std::max(1u, uint32_t(std::strtoul(argv[++i], nullptr, 10)));
        } else if (a == "--no-audio") {
            audio = false;
        }
    }
    std::filesystem::create_directories(out);
    const GuestImage exe = LoadExeImage(disc);
    const std::vector<StreamMovie> movies = ReadStreamMovieTable(exe);
    const GtVlcTable vlc = ReadGtVlcTable(LoadOverlayImage(disc, 5), kMovieVlcTableAddress);
    const MdecCore mdec = MdecCoreFromExe(exe);
    GtMovieReader reader(disc, StreamFileLba(disc), movies[size_t(n)]);
    GtMovieReader::Frame frame;
    size_t written = 0;
    while (reader.NextFrame(frame)) {
        if (frame.number < from || frame.number > to || (frame.number - from) % step) continue;
        const MdecFrameCodes codes = GtFrameToMdecCodes(frame.data.data(), frame.data.size(), vlc);
        const MovieImage img = DecodeMovieFrame(mdec, codes);
        std::vector<uint8_t> rgba(size_t(img.width) * size_t(img.height) * 4);
        for (size_t i = 0; i < size_t(img.width) * size_t(img.height); i++) {
            std::memcpy(&rgba[i * 4], &img.rgb[i * 3], 3);
            rgba[i * 4 + 3] = 255;
        }
        char name[32];
        std::snprintf(name, sizeof(name), "frame_%05u.png", frame.number);
        WritePngRgbaCompressed((out / name).string(), img.width, img.height, rgba);
        written++;
    }
    std::printf("movie %d: %zu frames -> %s\n", n, written, out.string().c_str());
    if (audio && !reader.Audio().empty()) {
        const std::string wav = (out / "audio.wav").string();
        WriteWav(wav, reader.Audio(), reader.AudioRate(), reader.AudioStereo() ? 2 : 1);
        std::printf("audio: %u XA sectors, %zu samples -> %s\n", reader.AudioSectors(), reader.Audio().size(), wav.c_str());
    }
    return 0;
}

} // namespace gt2
