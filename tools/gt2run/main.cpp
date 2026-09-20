// gt2run - guest code harness: runs original game code inside our R3000 interpreter.
//   gt2run inflate-test <disc.bin>
//     Reference validation: the game's own inflate routine must unpack all six GT2.OVL members
//     to exactly the bytes our host inflate produces.
//   gt2run boot-trace <disc.bin> [max-instructions]
//     Boots the EXE in the Machine and reports how far it gets and what it touched.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

#include "gt2export/png_writer.h"
#include "guest/bus.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/inflate.h"
#include "interp/r3000.h"
#include "machine/machine.h"
#include "input_script.h"

using namespace gt2;

int CmdSceneProbe(const DiscImage& disc, uint64_t field); // scene_probe.cpp
int CmdCarProbe(const DiscImage& disc, uint64_t field);   // car_probe.cpp
int CmdCallTrace(const DiscImage& disc, uint64_t field, uint64_t fields, const std::string& outDir, const std::string& script); // calltrace.cpp
int CmdWatch(const DiscImage& disc, uint64_t field, uint32_t address, uint32_t length, uint64_t fields); // watch.cpp
int CmdMovieCheck(const DiscImage& disc, uint64_t fields, const std::string& outDir, const std::string& script,
                  const std::vector<std::string>& options); // movie_check.cpp
int CmdSession(const DiscImage& disc, uint64_t fields, const std::string& outDir, const std::string& script,
               const std::vector<std::string>& options); // session.cpp

namespace {

constexpr uint32_t kReturnSentinel = 0xBFC0DEA0;
constexpr uint32_t kStackTop = 0x801FFF00; // SYSTEM.CNF: STACK = 801fff00

uint32_t ReadU32(const std::vector<uint8_t>& d, size_t o) {
    return uint32_t(d[o] | (d[o + 1] << 8) | (d[o + 2] << 16) | (uint32_t(d[o + 3]) << 24));
}

std::vector<uint8_t> ReadRootFile(const DiscImage& disc, const std::string& name) {
    auto f = disc.FindRootFile(name);
    if (!f) throw std::runtime_error(name + " not found in disc root");
    std::vector<uint8_t> data(f->size);
    disc.ReadForm1(f->lba, 0, data.data(), data.size());
    return data;
}

std::string FindExeName(const DiscImage& disc) {
    for (const auto& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0 || f.name.rfind("SCPS_", 0) == 0 || f.name.rfind("SCES_", 0) == 0) return f.name;
    throw std::runtime_error("no PS-X EXE found in disc root");
}

void LoadExe(Bus& bus, const std::vector<uint8_t>& exe) {
    if (exe.size() < 0x800 || std::memcmp(exe.data(), "PS-X EXE", 8) != 0) throw std::runtime_error("not a PS-X EXE");
    uint32_t address = ReadU32(exe, 0x18), size = ReadU32(exe, 0x1C);
    if (0x800 + size > exe.size()) throw std::runtime_error("PS-X EXE is truncated");
    std::memcpy(bus.RamPointer(address, size), exe.data() + 0x800, size);
}

int CmdInflateTest(const DiscImage& disc) {
    const std::vector<uint8_t> exe = ReadRootFile(disc, FindExeName(disc));
    const std::vector<uint8_t> ovl = ReadRootFile(disc, "GT2.OVL");
    constexpr uint32_t kOvlAddress = 0x80100000, kOutAddress = 0x80150000;

    struct Candidate { uint32_t function; uint32_t skip; const char* note; };
    const Candidate candidates[] = {
        {0x80082FAC, 0, "scout address, src = gzip member"},
        {0x800847D0, 0, "CC0 symbol gzip_decompress, src = gzip member"},
        {0x80082FAC, 10, "scout address, src = raw deflate"},
        {0x800847D0, 10, "CC0 symbol gzip_decompress, src = raw deflate"},
    };

    int bestPassed = -1;
    for (const Candidate& cand : candidates) {
        int passed = 0;
        uint64_t instructions = 0;
        double seconds = 0;
        std::string failure;
        for (size_t m = 0; m < 6; m++) {
            uint32_t offset = ReadU32(ovl, m * 8), packed = ReadU32(ovl, m * 8 + 4);
            std::vector<uint8_t> expected = Gunzip(std::span<const uint8_t>(ovl).subspan(offset, packed));

            Bus bus;
            R3000 cpu(bus);
            LoadExe(bus, exe);
            std::memcpy(bus.RamPointer(kOvlAddress, uint32_t(ovl.size())), ovl.data(), ovl.size());
            cpu.SetPc(cand.function);
            cpu.gpr[4] = kOvlAddress + offset + cand.skip;
            cpu.gpr[5] = kOutAddress;
            cpu.gpr[29] = kStackTop;
            cpu.gpr[31] = kReturnSentinel;
            cpu.onException = [&](R3000::Exception cause, uint32_t epc) -> bool {
                throw std::runtime_error("guest exception " + std::to_string(cause) + " at " + Bus::Hex(epc));
            };
            try {
                const auto t0 = std::chrono::steady_clock::now();
                const bool returned = cpu.RunUntil(kReturnSentinel, 400'000'000);
                seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                if (!returned) throw std::runtime_error("did not return (pc " + Bus::Hex(cpu.pc) + ")");
                if (std::memcmp(bus.RamPointer(kOutAddress, uint32_t(expected.size())), expected.data(), expected.size()) != 0)
                    throw std::runtime_error("output differs from host inflate (v0 = " + Bus::Hex(cpu.gpr[2]) + ")");
                passed++;
                instructions += cpu.instructionCount;
            } catch (const std::exception& e) {
                if (failure.empty()) failure = "member " + std::to_string(m) + ": " + e.what();
            }
        }
        std::printf("%-52s  %d/6 members identical", cand.note, passed);
        if (passed) std::printf(", %.1f M guest instructions, %.0f MIPS", instructions / 1e6, instructions / 1e6 / seconds);
        if (!failure.empty()) std::printf("   [%s]", failure.c_str());
        std::printf("\n");
        bestPassed = passed > bestPassed ? passed : bestPassed;
    }
    return bestPassed == 6 ? 0 : 1;
}

// Anything the machine does not implement stops the run with a reason instead of being faked.
int CmdBootTrace(const DiscImage& disc, uint64_t maxInstructions) {
    Machine m;
    m.AttachDisc(&disc);
    if (const char* card = std::getenv("GT2_MEMCARD")) m.AttachMemoryCard(card);
    m.LoadExe(ReadRootFile(disc, FindExeName(disc)), kStackTop);
    m.traceIo = true;
    std::map<uint32_t, uint64_t> gp0Commands, gp1Commands;
    m.onGpuWord = [&](bool gp1, uint32_t word) { (gp1 ? gp1Commands : gp0Commands)[word >> 24]++; };

    std::string stop = m.Run(maxInstructions);
    const auto& r = m.report;
    auto n64 = [](uint64_t v) { return static_cast<unsigned long long>(v); };
    std::printf("stopped after %.2f M instructions at pc %s: %s\n", m.cpu.instructionCount / 1e6, Bus::Hex(m.cpu.pc).c_str(), stop.c_str());
    std::printf("vblanks %llu, interrupts delivered %llu, GP0 words %llu, GP1 words %llu\n", n64(r.vblanks),
                n64(r.interruptsDelivered), n64(r.gp0Words), n64(r.gp1Words));
    for (const auto& [ch, n] : r.dmaTransfers) std::printf("DMA channel %u: %llu transfers\n", ch, n64(n));

    std::printf("\nBIOS calls in order of first use (%zu distinct):\n", r.biosFirstUse.size());
    for (const auto& s : r.biosFirstUse) std::printf("  %s  x%llu\n", s.c_str(), n64(r.biosCalls.at(s.substr(0, 5))));

    std::printf("\nI/O registers in order of first use (%zu distinct):\n", r.ioFirstUse.size());
    for (const auto& s : r.ioFirstUse) {
        uint32_t address = uint32_t(std::strtoul(s.c_str() + 2, nullptr, 16));
        const auto& c = r.io.at(address);
        std::printf("  %-52s reads %llu writes %llu\n", s.c_str(), n64(c.first), n64(c.second));
    }
    if (!gp1Commands.empty() || !gp0Commands.empty()) {
        std::printf("\nGP1 commands:");
        for (const auto& [c, n] : gp1Commands) std::printf(" %02X x%llu", c, n64(n));
        std::printf("\nGP0 first bytes:");
        for (const auto& [c, n] : gp0Commands) std::printf(" %02X x%llu", c, n64(n));
        std::printf("\n");
    }
    if (const Cdrom* cd = m.Disc()) {
        std::printf("CD-ROM: %llu sectors delivered, %llu XA sectors skipped; commands:", n64(cd->sectorsDelivered), n64(cd->xaSectorsSkipped));
        for (const auto& [c, n] : cd->commandCounts) std::printf(" %02X x%llu", c, n64(n));
        for (const auto& u : cd->unknownCommands) std::printf(" [unknown: %s]", u.c_str());
        std::putchar(10);
        if (std::getenv("GT2_CDLOG")) {
            size_t shown = 0;
            for (const auto& a : cd->accessLog) {
                if (shown++ >= 120) break;
                std::printf("  cd %9.3fM %c reg%u idx%u = %02X\n", a.time / 1e6, a.kind, a.reg, a.index, a.value);
            }
        }
    }
    if (!r.guestLog.empty()) {
        std::printf("\nguest log:\n");
        for (const auto& line : r.guestLog) std::printf("  | %s%s", line.c_str(), line.ends_with('\n') ? "" : "\n");
    }
    return 0;
}

// Runs the game and saves the visible display every `everyVBlanks` fields: the first look at what the
// guest is actually drawing (software GPU, see src/machine/gpu.h).
// `script` = comma separated "field:button[:fields]" presses held for `fields` fields (default 6), e.g.
// "1500:start,1700:cross,1800:down,2400:cross:600" (input_script.h).
int CmdPlay(const DiscImage& disc, const std::string& outDir, uint64_t fields, uint64_t everyVBlanks, const std::string& script) {
    const std::vector<ScriptPress> presses = ParseInputScript(script);

    Machine m;
    m.AttachDisc(&disc);
    if (const char* card = std::getenv("GT2_MEMCARD")) m.AttachMemoryCard(card);
    m.LoadExe(ReadRootFile(disc, FindExeName(disc)), kStackTop);
    std::filesystem::create_directories(outDir);
    std::string stop;
    std::vector<int16_t> audio;
    m.gpu.skip3dRaster = std::getenv("GT2_NORASTER") != nullptr;
    // GT2_GPULOG=<first field>: print the GPU environment/display commands of 4 fields starting there.
    const uint64_t gpuLogFrom = std::getenv("GT2_GPULOG") ? std::strtoull(std::getenv("GT2_GPULOG"), nullptr, 10) : UINT64_MAX;
    uint64_t primitivesSinceEnv = 0;
    m.onGpuWord = [&](bool gp1, uint32_t word) {
        const uint64_t field = m.report.vblanks;
        if (field < gpuLogFrom || field >= gpuLogFrom + 4) return;
        const uint32_t cmd = word >> 24;
        if (gp1 && (cmd == 0x05 || cmd == 0x08)) std::printf("  f%llu [%llu prims] GP1 %02X %06X\n", (unsigned long long)field, (unsigned long long)primitivesSinceEnv, cmd, word & 0xFFFFFF);
        else if (!gp1 && cmd >= 0xE1 && cmd <= 0xE6 && cmd != 0xE2) std::printf("  f%llu [%llu prims] GP0 %02X %06X\n", (unsigned long long)field, (unsigned long long)primitivesSinceEnv, cmd, word & 0xFFFFFF);
        primitivesSinceEnv = m.gpu.primitivesDrawn;
    };
    // GT2_SIOLOG=<first field>[,<count>]: print every SIO0 / IRQ7 / timer-2 access of the pad driver in those fields.
    uint64_t sioLogFrom = UINT64_MAX, sioLogCount = 2;
    if (const char* s = std::getenv("GT2_SIOLOG")) {
        char* end = nullptr;
        sioLogFrom = std::strtoull(s, &end, 10);
        if (end && *end == ',') sioLogCount = std::strtoull(end + 1, nullptr, 10);
    }
    for (uint64_t field = 1; field <= fields && stop.empty(); field++) {
        m.padButtons = ScriptButtons(presses, field);
        m.traceSio = field >= sioLogFrom && field < sioLogFrom + sioLogCount;
        std::string reason = m.Run(Machine::kInstructionsPerVBlank);
        if (reason != "instruction budget exhausted") stop = reason;
        if (!m.report.sioLog.empty()) {
            for (const auto& line : m.report.sioLog) std::printf("  sio %s\n", line.c_str());
            m.report.sioLog.clear();
        }
        audio.insert(audio.end(), m.spu.output.begin(), m.spu.output.end());
        m.spu.output.clear();
        if (field % everyVBlanks != 0 && stop.empty()) continue;
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = m.gpu.DisplayRgba(w, h);
        char name[64];
        std::snprintf(name, sizeof(name), "/field_%06llu.png", static_cast<unsigned long long>(m.report.vblanks));
        WritePngRgba(outDir + name, w, h, rgba);
    }
    std::printf("ran %.1f M instructions, %llu fields, %llu primitives drawn, display %dx%d%s\n", m.cpu.instructionCount / 1e6,
                static_cast<unsigned long long>(m.report.vblanks), static_cast<unsigned long long>(m.gpu.primitivesDrawn),
                m.gpu.DisplayWidth(), m.gpu.DisplayHeight(), m.gpu.DisplayEnabled() ? "" : " (display disabled)");
    if (!stop.empty()) std::printf("stopped: %s\n", stop.c_str());

    // Audio: a WAV for listening plus per-5-second loudness so silence is visible in the text output.
    {
        const uint32_t dataBytes = uint32_t(audio.size() * 2), rate = Spu::kSampleRate;
        std::FILE* f = std::fopen((outDir + "/audio.wav").c_str(), "wb");
        if (f) {
            auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
            auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
            std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVEfmt ", 1, 8, f);
            u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
            std::fwrite("data", 1, 4, f); u32(dataBytes);
            std::fwrite(audio.data(), 2, audio.size(), f);
            std::fclose(f);
        }
        std::printf("audio: %.1f s, %llu key-ons, %llu XA sectors; RMS per 5 s:", audio.size() / 2.0 / rate,
                    static_cast<unsigned long long>(m.spu.keyOnCount), static_cast<unsigned long long>(m.spu.xaSectors));
        const size_t window = size_t(rate) * 2 * 5;
        for (size_t at = 0; at < audio.size(); at += window) {
            double sum = 0;
            const size_t end = std::min(audio.size(), at + window);
            for (size_t i = at; i < end; i++) sum += double(audio[i]) * audio[i];
            std::printf(" %.0f", std::sqrt(sum / double(end - at)));
        }
        std::printf("\n");
    }
    for (const auto& line : m.report.guestLog) std::printf("  | %s%s", line.c_str(), line.ends_with('\n') ? "" : "\n");
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::puts("usage: gt2run inflate-test <disc.bin>\n       gt2run boot-trace <disc.bin> [max-instructions]\n"
                  "       gt2run session <disc.bin> <fields> <outDir> \"<script>\" [card=<mcd>] [snap=f1+f2] [shots=N] [watch=a:len[:max]] [watchfrom=N]\n"
                  "                      [calls=A+B] [trace=from:count] [io] [aiplayer]\n"
                  "       gt2run movie-check <disc.bin> <fields> <outDir> \"<script>\" [pngs=N]   (Arcade disc: displayed movie frames vs the native decoder)\n"
                  "         aiplayer = DEV CAPTURE AID: the original's AI drives the player's car (changes guest state; oracle captures only)");
        return 2;
    }
    try {
        DiscImage disc(argv[2]);
        if (std::string(argv[1]) == "inflate-test") return CmdInflateTest(disc);
        if (std::string(argv[1]) == "boot-trace") return CmdBootTrace(disc, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 50'000'000ull);
        if (std::string(argv[1]) == "watch" && argc >= 6)
            return CmdWatch(disc, std::strtoull(argv[3], nullptr, 10), uint32_t(std::strtoul(argv[4], nullptr, 16)),
                            uint32_t(std::strtoul(argv[5], nullptr, 16)), argc > 6 ? std::strtoull(argv[6], nullptr, 10) : 4);
        if (std::string(argv[1]) == "calltrace" && argc >= 6)
            return CmdCallTrace(disc, std::strtoull(argv[3], nullptr, 10), std::strtoull(argv[4], nullptr, 10), argv[5], argc > 6 ? argv[6] : "");
        if (std::string(argv[1]) == "session" && argc >= 6)
            return CmdSession(disc, std::strtoull(argv[3], nullptr, 10), argv[4], argv[5], std::vector<std::string>(argv + 6, argv + argc));
        if (std::string(argv[1]) == "movie-check" && argc >= 6)
            return CmdMovieCheck(disc, std::strtoull(argv[3], nullptr, 10), argv[4], argv[5], std::vector<std::string>(argv + 6, argv + argc));
        if (std::string(argv[1]) == "car-probe") return CmdCarProbe(disc, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 3500);
        if (std::string(argv[1]) == "scene-probe") return CmdSceneProbe(disc, argc > 3 ? std::strtoull(argv[3], nullptr, 10) : 3500);
        if (std::string(argv[1]) == "play" && argc >= 4)
            return CmdPlay(disc, argv[3], argc > 4 ? std::strtoull(argv[4], nullptr, 10) : 1800,
                           argc > 5 ? std::strtoull(argv[5], nullptr, 10) : 120, argc > 6 ? argv[6] : "");
        std::puts("unknown command");
        return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
