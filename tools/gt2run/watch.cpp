// watch: who writes to a guest memory range? Runs to a field, then records for a few fields every write
// into [address, address + length): writer pc, return address, how often, which offsets.
#include <cstdio>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2vfs/disc_image.h"
#include "machine/machine.h"

using namespace gt2;

int CmdWatch(const DiscImage& disc, uint64_t field, uint32_t address, uint32_t length, uint64_t fields) {
    std::string exeName;
    for (const auto& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0) exeName = f.name;
    auto exeFile = disc.FindRootFile(exeName);
    if (!exeFile) throw std::runtime_error("EXE not found");
    std::vector<uint8_t> exe(exeFile->size);
    disc.ReadForm1(exeFile->lba, 0, exe.data(), exe.size());

    Machine m;
    m.AttachDisc(&disc);
    m.gpu.skip3dRaster = true;
    m.LoadExe(exe, 0x801FFF00);
    m.Run(Machine::kInstructionsPerVBlank * field);

    struct Writer { uint64_t count = 0; std::set<uint32_t> offsets; uint32_t ra = 0, lastValue = 0; };
    std::map<uint32_t, Writer> writers; // by pc
    const uint32_t base = address & 0x1FFFFFFF;
    m.bus.watchBase = base;
    m.bus.watchSize = length;
    m.bus.onWatchedWrite = [&](uint32_t physical, uint32_t value, int) {
        Writer& w = writers[m.cpu.pc - 4]; // pc already points past the store (or past its branch when in a delay slot)
        w.count++;
        w.offsets.insert(physical - base);
        w.ra = m.cpu.gpr[31];
        w.lastValue = value;
    };
    m.Run(Machine::kInstructionsPerVBlank * fields);

    std::printf("writes to 0x%08X..+0x%X during %llu fields after field %llu:\n", address, length, static_cast<unsigned long long>(fields),
                static_cast<unsigned long long>(field));
    for (const auto& [pc, w] : writers) {
        std::printf("  pc 0x%08X  ra 0x%08X  x%-6llu last 0x%08X  offsets:", pc, w.ra, static_cast<unsigned long long>(w.count), w.lastValue);
        int shown = 0;
        for (uint32_t o : w.offsets) { if (shown++ < 12) std::printf(" +%X", o); }
        if (w.offsets.size() > 12) std::printf(" ... (%zu)", w.offsets.size());
        std::printf("\n");
    }
    return 0;
}
