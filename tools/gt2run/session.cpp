// session: one scripted run of the game for menu / career RE. Runs `fields` fields with scripted pad input and
// an optional memory card, and records what the shell does:
//   - every call of the overlay loader 0x8005DAD8 / 0x8005DA3C (field, member index, caller) - always on;
//   - calls of chosen functions with their arguments (`calls=A+B+...`; a0..a3 as the callee sees them: the
//     delay slot of the jal is evaluated for the common argument-setting instructions);
//   - writes into a RAM range (`watch=addr:len[:max]`: field, pc, ra, offset, size, value);
//   - RAM dumps + screenshots at chosen fields (`snap=f1+f2+...` -> ram_<f>.bin, shot_<f>.png);
//   - screenshots every N fields (`shots=N`);
//   - a call graph of one window (`trace=from:count` -> functions_<from>.txt, edges_<from>.txt; same format as
//     `calltrace`).
//   - `watchfrom=N`: ignore watched writes before field N (boot clears BSS); `io`: I/O register + BIOS call statistics.
//   - `aiplayer`: DEV CAPTURE AID - the original's AI drives the player's car (ai_player.h; changes guest state, so the
//     run is an oracle capture aid only, never a reference for the player's own driving).
//   - `auto=<from>[:<to>]`: DEV CAPTURE AID - from field <from> the race autopilot (race_autopilot.h) presses through the
//     race overlay's screens (results waits, replay exit, post-race menu, next session); its presses are logged and
//     written as a gt2play --script (autoscript.txt) so the run can be repeated for GP0 captures.
// Usage: gt2run session <disc> <fields> <outDir> "<script>" [card=<mcd>] [card2=<mcd>] [snap=..] [shots=N] [watch=..] [watchfrom=N]
//        [calls=..] [trace=..] [io] [aiplayer]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2export/png_writer.h"
#include "gt2vfs/disc_image.h"
#include "ai_player.h"
#include "ai_player_arcade.h"
#include "input_script.h"
#include "race_autopilot.h"
#include "machine/machine.h"

using namespace gt2;

namespace {

std::vector<uint64_t> SplitNumbers(const std::string& s, int base) {
    std::vector<uint64_t> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t plus = s.find('+', pos);
        if (plus == std::string::npos) plus = s.size();
        out.push_back(std::strtoull(s.substr(pos, plus - pos).c_str(), nullptr, base));
        pos = plus + 1;
    }
    return out;
}

// Value of register `reg` after the delay-slot instruction `insn` of a jal executes (registers are the state
// before it). Handles the instructions compilers put there to set arguments; anything else leaves the register.
uint32_t AfterDelaySlot(const Machine& m, uint32_t insn, int reg) {
    const uint32_t op = insn >> 26, rs = (insn >> 21) & 31, rt = (insn >> 16) & 31, rd = (insn >> 11) & 31;
    const uint32_t s = m.cpu.gpr[rs], t = m.cpu.gpr[rt];
    const int32_t simm = int16_t(insn & 0xFFFF);
    const uint32_t uimm = insn & 0xFFFF;
    auto load = [&](int size, bool sign) -> uint32_t {
        try {
            uint32_t v = m.bus.Read(s + uint32_t(simm), size);
            if (sign && size == 1) v = uint32_t(int32_t(int8_t(v)));
            if (sign && size == 2) v = uint32_t(int32_t(int16_t(v)));
            return v;
        } catch (...) { return 0xDEADDEAD; }
    };
    if (op == 0) {
        if (int(rd) != reg) return m.cpu.gpr[reg];
        switch (insn & 63) {
        case 0x00: return t << ((insn >> 6) & 31);
        case 0x02: return t >> ((insn >> 6) & 31);
        case 0x03: return uint32_t(int32_t(t) >> ((insn >> 6) & 31));
        case 0x21: case 0x20: return s + t;
        case 0x23: case 0x22: return s - t;
        case 0x24: return s & t;
        case 0x25: return s | t;
        default: return m.cpu.gpr[reg];
        }
    }
    if (int(rt) != reg) return m.cpu.gpr[reg];
    switch (op) {
    case 0x08: case 0x09: return s + uint32_t(simm);
    case 0x0C: return s & uimm;
    case 0x0D: return s | uimm;
    case 0x0F: return uimm << 16;
    case 0x20: return load(1, true);
    case 0x24: return load(1, false);
    case 0x21: return load(2, true);
    case 0x25: return load(2, false);
    case 0x23: return load(4, false);
    default: return m.cpu.gpr[reg];
    }
}

} // namespace

int CmdSession(const DiscImage& disc, uint64_t fields, const std::string& outDir, const std::string& script,
               const std::vector<std::string>& options) {
    std::string card, card2;
    std::set<uint64_t> snaps;
    uint64_t shotEvery = 0, traceFrom = UINT64_MAX, traceCount = 0;
    uint32_t watchAddr = 0, watchLen = 0;
    uint64_t watchMax = 4000, watchFrom = 0;
    bool traceIo = false, aiPlayer = false;
    uint64_t autoFrom = UINT64_MAX, autoTo = UINT64_MAX;
    std::set<uint32_t> callSet = {0x8005DAD8u, 0x8005DA3Cu};
    for (const std::string& o : options) {
        const size_t eq = o.find('=');
        const std::string key = o.substr(0, eq), value = eq == std::string::npos ? "" : o.substr(eq + 1);
        if (key == "card") card = value;
        else if (key == "card2") card2 = value;
        else if (key == "snap") for (uint64_t f : SplitNumbers(value, 10)) snaps.insert(f);
        else if (key == "shots") shotEvery = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "calls") for (uint64_t a : SplitNumbers(value, 16)) callSet.insert(uint32_t(a));
        else if (key == "watch") {
            char* end = nullptr;
            watchAddr = uint32_t(std::strtoul(value.c_str(), &end, 16));
            if (end && *end == ':') watchLen = uint32_t(std::strtoul(end + 1, &end, 16));
            if (end && *end == ':') watchMax = std::strtoull(end + 1, nullptr, 10);
        } else if (key == "trace") {
            char* end = nullptr;
            traceFrom = std::strtoull(value.c_str(), &end, 10);
            if (end && *end == ':') traceCount = std::strtoull(end + 1, nullptr, 10);
        } else if (key == "watchfrom") watchFrom = std::strtoull(value.c_str(), nullptr, 10);
        else if (key == "io") traceIo = true;
        else if (key == "aiplayer") aiPlayer = true;
        else if (key == "auto") {
            char* end = nullptr;
            autoFrom = std::strtoull(value.c_str(), &end, 10);
            if (end && *end == ':') autoTo = std::strtoull(end + 1, nullptr, 10);
        }
        else throw std::runtime_error("unknown session option: " + o);
    }

    std::string exeName;
    for (const auto& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0) exeName = f.name;
    auto exeFile = disc.FindRootFile(exeName);
    if (!exeFile) throw std::runtime_error("EXE not found");
    std::vector<uint8_t> exe(exeFile->size);
    disc.ReadForm1(exeFile->lba, 0, exe.data(), exe.size());
    const std::vector<ScriptPress> presses = ParseInputScript(script);

    Machine m;
    m.AttachDisc(&disc);
    if (!card.empty()) m.AttachMemoryCard(card);
    if (!card2.empty()) m.AttachMemoryCard(card2, 1);
    m.gpu.skip3dRaster = true;
    m.traceIo = traceIo;
    m.LoadExe(exe, 0x801FFF00);
    std::filesystem::create_directories(outDir);

    uint64_t field = 0;
    std::FILE* log = std::fopen((outDir + "/session.txt").c_str(), "w");
    if (!log) throw std::runtime_error("cannot write " + outDir + "/session.txt");
    std::fprintf(log, "script: %s\n", script.c_str());

    // Call graph window.
    struct Function { uint64_t calls = 0, selfInstructions = 0; };
    std::map<uint32_t, Function> functions;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> edges;
    struct Frame { uint32_t function; uint64_t enteredAt, childInstructions; };
    std::vector<Frame> stack{{0, 0, 0}};
    bool tracing = false;

    RaceAutopilot autopilot;
    uint16_t autoPad = 0;
    m.cpu.onCall = [&](uint32_t from, uint32_t to) {
        autopilot.OnCall(to);
        if (aiPlayer && (AiPlayerSwitch(m, from, to) || ArcadeAiPlayerSwitch(m, from, to))) std::fprintf(log, "f%llu ai-player: the player's car starts with control class 2 (dev capture aid)\n", (unsigned long long)field);
        if (callSet.count(to)) {
            uint32_t slot = 0;
            m.bus.FastRead32(from + 4, slot);
            std::fprintf(log, "f%llu call %08X from %08X ra %08X a0 %08X a1 %08X a2 %08X a3 %08X\n", (unsigned long long)field, to, from,
                         m.cpu.gpr[31], AfterDelaySlot(m, slot, 4), AfterDelaySlot(m, slot, 5), AfterDelaySlot(m, slot, 6),
                         AfterDelaySlot(m, slot, 7));
        }
        if (!tracing) return;
        functions[to].calls++;
        edges[{stack.back().function, to}]++;
        if (stack.size() < 512) stack.push_back({to, m.cpu.instructionCount, 0});
    };
    m.cpu.onReturn = [&](uint32_t) {
        if (!tracing || stack.size() <= 1) return;
        const Frame f = stack.back();
        stack.pop_back();
        const uint64_t total = m.cpu.instructionCount - f.enteredAt;
        functions[f.function].selfInstructions += total - f.childInstructions;
        stack.back().childInstructions += total;
    };
    uint64_t watchLines = 0;
    if (watchLen) {
        m.bus.watchBase = watchAddr & 0x1FFFFFFF;
        m.bus.watchSize = watchLen;
        m.bus.onWatchedWrite = [&](uint32_t physical, uint32_t value, int size) {
            if (field < watchFrom || watchLines++ >= watchMax) return;
            std::fprintf(log, "f%llu write +%05X size %d value %08X pc %08X ra %08X\n", (unsigned long long)field,
                         physical - m.bus.watchBase, size, value, m.cpu.pc - 4, m.cpu.gpr[31]);
        };
    }

    auto shot = [&](const std::string& name) {
        int w = 0, h = 0;
        std::vector<uint8_t> rgba = m.gpu.DisplayRgba(w, h);
        WritePngRgba(outDir + "/" + name, w, h, rgba);
    };
    for (field = 1; field <= fields; field++) {
        if (field == traceFrom) { tracing = true; stack.assign(1, {0, m.cpu.instructionCount, 0}); }
        if (traceCount && field == traceFrom + traceCount) tracing = false;
        m.padButtons = uint16_t(ScriptButtons(presses, field) | autoPad);
        m.pad2Connected = ScriptUsesPort2(presses);
        m.pad2Buttons = ScriptButtons(presses, field, 1);
        const std::string reason = m.Run(Machine::kInstructionsPerVBlank);
        if (reason != "instruction budget exhausted") {
            std::fprintf(log, "guest stopped at field %llu: %s\n", (unsigned long long)field, reason.c_str());
            break;
        }
        autoPad = (field >= autoFrom && field < autoTo) ? autopilot.Next(m, field, log) : 0;
        char name[64];
        if (snaps.count(field)) {
            std::snprintf(name, sizeof(name), "/ram_%06llu.bin", (unsigned long long)field);
            if (std::FILE* f = std::fopen((outDir + name).c_str(), "wb")) { std::fwrite(m.bus.Ram(), 1, Bus::kRamSize, f); std::fclose(f); }
            std::snprintf(name, sizeof(name), "shot_%06llu.png", (unsigned long long)field);
            shot(name);
        }
        if (shotEvery && field % shotEvery == 0) {
            std::snprintf(name, sizeof(name), "field_%06llu.png", (unsigned long long)field);
            shot(name);
        }
    }
    m.cpu.onCall = nullptr;
    m.cpu.onReturn = nullptr;
    if (autoFrom != UINT64_MAX)
        if (std::FILE* f = std::fopen((outDir + "/autoscript.txt").c_str(), "w")) { // the whole run as one gt2play --script
            std::fprintf(f, "%s%s%s\n", script.c_str(), script.empty() || autopilot.Script().empty() ? "" : ",", autopilot.Script().c_str());
            std::fclose(f);
        }
    m.bus.onWatchedWrite = nullptr;
    if (traceCount) {
        char name[96];
        std::snprintf(name, sizeof(name), "/functions_%06llu.txt", (unsigned long long)traceFrom);
        if (std::FILE* f = std::fopen((outDir + name).c_str(), "w")) {
            for (const auto& [address, fn] : functions)
                std::fprintf(f, "%08X %llu %llu\n", address, (unsigned long long)fn.calls, (unsigned long long)fn.selfInstructions);
            std::fclose(f);
        }
        std::snprintf(name, sizeof(name), "/edges_%06llu.txt", (unsigned long long)traceFrom);
        if (std::FILE* f = std::fopen((outDir + name).c_str(), "w")) {
            for (const auto& [edge, count] : edges) std::fprintf(f, "%08X %08X %llu\n", edge.first, edge.second, (unsigned long long)count);
            std::fclose(f);
        }
    }
    if (traceIo) {
        for (const auto& [address, rw] : m.report.io)
            std::fprintf(log, "io %08X reads %llu writes %llu\n", address, (unsigned long long)rw.first, (unsigned long long)rw.second);
        for (const auto& line : m.report.ioFirstUse) std::fprintf(log, "io first use: %s\n", line.c_str());
        for (const auto& [name, count] : m.report.biosCalls) std::fprintf(log, "bios %s x%llu\n", name.c_str(), (unsigned long long)count);
    }
    for (const auto& line : m.report.guestLog) std::fprintf(log, "  | %s%s", line.c_str(), line.ends_with('\n') ? "" : "\n");
    std::fclose(log);
    std::printf("session: %llu fields, %llu watched writes, log %s/session.txt\n", (unsigned long long)(field - 1),
                (unsigned long long)watchLines, outDir.c_str());
    return 0;
}
