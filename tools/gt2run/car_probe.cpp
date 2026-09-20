// car-probe: finds where the running game keeps car positions. The scene extractor already gives every
// visible car's world position (from GTE traffic); this tool scans guest RAM for 16.16 fixed-point
// X/Z pairs close to those positions and reports the addresses - the first step towards the car state
// structure and the physics code that writes it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "machine/machine.h"
#include "scene/scene_extractor.h"

using namespace gt2;

int CmdCarProbe(const DiscImage& disc, uint64_t field) {
    GtfsVolume vol(disc);
    std::string exeName;
    for (const auto& f : disc.RootFiles())
        if (f.name.rfind("SCUS_", 0) == 0) exeName = f.name;
    auto exeFile = disc.FindRootFile(exeName);
    if (!exeFile) throw std::runtime_error("EXE not found");
    std::vector<uint8_t> exe(exeFile->size);
    disc.ReadForm1(exeFile->lba, 0, exe.data(), exe.size());

    std::puts("indexing car models...");
    SceneExtractor extractor(vol);
    Machine m;
    m.AttachDisc(&disc);
    m.gpu.skip3dRaster = true;
    m.EnableSceneCapture();
    m.LoadExe(exe, 0x801FFF00);

    SceneExtractor::Scene scene;
    std::vector<uint8_t> ramAtScene;
    bool armed = false;
    m.onSceneFrame = [&](const Machine::SceneFrame& frame) {
        if (!armed) return;
        SceneExtractor::Scene s = extractor.Extract(frame.transforms, frame.vertices);
        if (s.valid && !s.cars.empty()) scene = s;
    };
    m.Run(Machine::kInstructionsPerVBlank * field);
    armed = true;
    m.Run(Machine::kInstructionsPerVBlank * 6);
    if (!scene.valid) { std::puts("no race scene at this field"); return 1; }
    const uint8_t* ram = m.bus.Ram();

    std::printf("course %s, camera %.1f %.1f %.1f, %zu cars\n", scene.trackName.c_str(), scene.cameraPosition[0], scene.cameraPosition[1],
                scene.cameraPosition[2], scene.cars.size());
    auto s32 = [&](uint32_t a) { int32_t v; std::memcpy(&v, ram + a, 4); return v; };

    std::map<uint32_t, int> hitsByAddress;
    for (const auto& car : scene.cars) {
        const double x = car.world[12], y = car.world[13], z = car.world[14];
        std::printf("car %s at %.2f %.2f %.2f m:\n", car.id.c_str(), x, y, z);
        const double tolerance = 4.0 * 65536; // the capture is up to a few frames old
        int shown = 0;
        for (uint32_t a = 0x10000; a + 16 <= Bus::kRamSize; a += 4) {
            if (std::abs(s32(a) - x * 65536) > tolerance) continue;
            for (int dz : {4, 8, -4, -8}) {
                if (std::abs(s32(a + uint32_t(dz)) - z * 65536) > tolerance) continue;
                hitsByAddress[a]++;
                if (shown++ < 12)
                    std::printf("    0x%08X: x %.2f, z(@%+d) %.2f, y-candidates %.2f %.2f\n", 0x80000000u + a, s32(a) / 65536.0, dz,
                                s32(a + uint32_t(dz)) / 65536.0, s32(a + 4) / 65536.0, s32(a - 4) / 65536.0);
                break;
            }
        }
    }
    return 0;
}
