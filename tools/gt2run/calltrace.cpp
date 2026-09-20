// calltrace: dynamic call graph of one stretch of the running game + a RAM dump taken at the same moment.
// Output (in <outDir>): ram.bin (2 MB, load at 0x80000000), functions.txt (every called entry point with
// call count and the instructions spent in it, self time), edges.txt (caller function -> callee, count).
// The function list seeds the Ghidra import (re/ghidra/DecompileSeeds.java).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2formats/course_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "input_script.h"
#include "machine/machine.h"

using namespace gt2;

// `script`: optional scripted pad input ("field:button[:fields],...", input_script.h) so that a player race
// can be reached; the same script syntax as `play`.
int CmdCallTrace(const DiscImage& disc, uint64_t field, uint64_t fields, const std::string& outDir, const std::string& script) {
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
    m.gpu.skip3dRaster = true;
    m.LoadExe(exe, 0x801FFF00);
    uint64_t currentField = 0; // 1-based like `play`: the pad state of field n is applied before its VBlank
    auto runFields = [&](uint64_t count) {
        for (uint64_t i = 0; i < count; i++) {
            currentField++;
            m.padButtons = ScriptButtons(presses, currentField);
            m.pad2Connected = ScriptUsesPort2(presses);
            m.pad2Buttons = ScriptButtons(presses, currentField, 1);
            const std::string reason = m.Run(Machine::kInstructionsPerVBlank);
            if (reason != "instruction budget exhausted") throw std::runtime_error("guest stopped at field " + std::to_string(currentField) + ": " + reason);
        }
    };
    runFields(field);

    struct Function { uint64_t calls = 0, selfInstructions = 0; };
    std::map<uint32_t, Function> functions;
    std::map<std::pair<uint32_t, uint32_t>, uint64_t> edges;
    struct Frame { uint32_t function; uint64_t enteredAt, childInstructions; };
    std::vector<Frame> stack;
    stack.push_back({0, m.cpu.instructionCount, 0});

    m.cpu.onCall = [&](uint32_t, uint32_t to) {
        functions[to].calls++;
        edges[{stack.back().function, to}]++;
        if (stack.size() < 512) stack.push_back({to, m.cpu.instructionCount, 0});
    };
    m.cpu.onReturn = [&](uint32_t) {
        if (stack.size() <= 1) return;
        const Frame f = stack.back();
        stack.pop_back();
        const uint64_t total = m.cpu.instructionCount - f.enteredAt;
        functions[f.function].selfInstructions += total - f.childInstructions;
        stack.back().childInstructions += total;
    };
    runFields(fields);
    m.cpu.onCall = nullptr;
    m.cpu.onReturn = nullptr;

    std::filesystem::create_directories(outDir);
    if (std::FILE* f = std::fopen((outDir + "/ram.bin").c_str(), "wb")) {
        std::fwrite(m.bus.Ram(), 1, Bus::kRamSize, f);
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen((outDir + "/functions.txt").c_str(), "w")) {
        for (const auto& [address, fn] : functions)
            std::fprintf(f, "%08X %llu %llu\n", address, static_cast<unsigned long long>(fn.calls), static_cast<unsigned long long>(fn.selfInstructions));
        std::fclose(f);
    }
    if (std::FILE* f = std::fopen((outDir + "/edges.txt").c_str(), "w")) {
        for (const auto& [edge, count] : edges) std::fprintf(f, "%08X %08X %llu\n", edge.first, edge.second, static_cast<unsigned long long>(count));
        std::fclose(f);
    }
    size_t overlay = 0;
    for (const auto& [address, fn] : functions) overlay += address < 0x8005D600u;
    std::printf("%zu functions called (%zu in the overlay), %zu edges; wrote %s/{ram.bin,functions.txt,edges.txt}\n", functions.size(),
                overlay, edges.size(), outDir.c_str());

    // The race of the dump: course index 0x800AF230 -> .crsinfo entry (loaded at 0x801E18E8, 24 bytes each) ->
    // file id -> "crsobj/<name>.tro" whose base name hashes to it (gt2formats/course_data.h); cars 0x800AF231.
    const uint8_t* ram = m.bus.Ram();
    const uint32_t course = ram[0x800AF230u & 0x1FFFFF], cars = ram[0x800AF231u & 0x1FFFFF];
    uint32_t fileId = 0;
    std::memcpy(&fileId, ram + ((0x801E18E8u + course * 24u + 4u) & 0x1FFFFF), 4);
    std::string courseName = "?", displayName;
    try {
        GtfsVolume vol(disc);
        for (const GtfsEntry& e : vol.Files()) {
            if (e.path.rfind("crsobj/", 0) != 0) continue;
            const std::string base = e.path.substr(7, e.path.find('.', 7) - 7);
            if (CourseFileId(base) == fileId) { courseName = base; break; }
        }
        const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
        if (course < info.entries.size()) displayName = info.entries[course].name;
    } catch (const std::exception& e) {
        displayName = std::string("[") + e.what() + "]";
    }
    std::printf("race: course index %u \"%s\", file id 0x%08X = crsobj/%s.tro, %u car(s) (0x800AF231), game mode %u (0x801D5864)\n", course,
                displayName.c_str(), fileId, courseName.c_str(), cars, unsigned(ram[0x801D5864u & 0x1FFFFF]));
    return 0;
}
