// DEVELOPMENT ONLY - see the header. Every address below is a fact of the US Simulation v1.2 executable / race
// overlay established by tools/gt2verify (verify_core.cpp, verify_ground.cpp, verify_setup.cpp) and re/structs/car.yaml.
#include "game/sim/dev_dump_constants.h"

#include <cstdio>
#include <cstring>

#include "gt2formats/exe_profile.h"

namespace gt2::sim::dev {

namespace {

constexpr size_t kRamSize = 0x200000;

// The dump's RAM. At / Get take US Simulation v1.2 addresses and translate them to the dump's build (the process-wide
// address profile, gt2formats/exe_profile.h: identity for the Simulation build); AtP / GetP take pointers read from the
// dump (already the build's addresses).
struct Ram {
    std::vector<uint8_t> bytes;
    const uint8_t* AtP(uint32_t address) const { return bytes.data() + (address & 0x1FFFFF); }
    template <typename T> T GetP(uint32_t address) const {
        T v;
        std::memcpy(&v, AtP(address), sizeof(T));
        return v;
    }
    const uint8_t* At(uint32_t simAddress) const { return AtP(RaceAddress(simAddress)); }
    template <typename T> T Get(uint32_t simAddress) const { return GetP<T>(RaceAddress(simAddress)); }
};

// 0x800418E8: the shell's control class from the game-mode globals (see the decomp; the dirt flag is the course
// table entry's bit 2). 2 = player-controlled race car with damage and pit flags, 0 otherwise.
int32_t ShellControlClass(const Ram& ram, bool dirtCourse) {
    const uint8_t mode = ram.Get<uint8_t>(0x801D5866u);
    switch (mode) {
    case 0: {
        if (dirtCourse) return 0;
        return ram.Get<uint8_t>(0x801D5860u) == 1 ? 2 : 0;
    }
    case 2: case 4: case 0xC: {
        if (dirtCourse) return 0;
        if (ram.Get<uint8_t>(0x801D5865u) == 1) return 0;
        return ram.Get<uint8_t>(0x801D585Du) == 1 ? 2 : 0;
    }
    case 3: return 1;
    default: return 0;
    }
}

} // namespace

bool LoadRaceFromDump(const std::string& path, DumpRace& out, std::string& error) {
    std::vector<uint8_t> bytes(kRamSize);
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { error = "cannot open " + path; return false; }
    const size_t n = std::fread(bytes.data(), 1, kRamSize, f);
    std::fclose(f);
    if (n != kRamSize) { error = path + " is not a 2 MB RAM dump"; return false; }
    return LoadRaceFromImage(bytes, out, error);
}

bool LoadRaceFromImage(std::span<const uint8_t> image, DumpRace& out, std::string& error) {
    if (image.size() != kRamSize) { error = "the RAM image is not 2 MB"; return false; }
    Ram ram;
    ram.bytes.assign(image.begin(), image.end());

    SimConstants& c = out.constants;
    c.step.frameTime = ram.Get<int32_t>(0x801C856Cu);
    c.step.rate = ram.Get<int32_t>(0x801C8570u);
    c.step.draftDragFloor = ram.Get<int32_t>(0x80046EF4u);
    c.wear.wearLimit = ram.Get<int32_t>(0x80046F48u);
    c.wear.wornGripLoss = ram.Get<int32_t>(0x80046F4Cu);
    c.wear.pitGripFactor = ram.Get<int32_t>(0x80046F50u);
    c.wear.coldLimit = ram.Get<int32_t>(0x80046F54u);
    c.wear.coldGripLoss = ram.Get<int32_t>(0x80046F58u);
    c.wear.wearKnee = ram.Get<int32_t>(0x80046F5Cu);
    c.wear.kneeGripLoss = ram.Get<int32_t>(0x80046F60u);
    {
        const uint32_t count = ram.Get<uint16_t>(0x801C8730u), xs = ram.Get<uint32_t>(0x801C8734u), ys = ram.Get<uint32_t>(0x801C8738u);
        for (uint32_t i = 0; i < count; i++) {
            c.rollingXs.push_back(ram.GetP<int32_t>(xs + i * 4));
            c.rollingYs.push_back(ram.GetP<int32_t>(ys + i * 4));
        }
    }
    for (uint32_t i = 0; i < 8; i++) c.surfaceRolling[i] = ram.Get<int32_t>(0x80046E00u + i * 4);
    for (uint32_t i = 0; i < 4; i++) std::memcpy(&c.classTuning[i], ram.At(0x801C8690u + i * 0x28u), sizeof(DriveClassTuning));
    c.slideSensitivity = ram.Get<uint8_t>(0x80046EE8u);
    c.word80046F64 = ram.Get<uint32_t>(0x80046F64u);
    c.steerSpringGain = ram.Get<int32_t>(0x80046F3Cu);
    c.steerDamping = ram.Get<int32_t>(0x80046F40u);
    c.steerCentring = ram.Get<int32_t>(0x80046F44u);
    {
        const uint32_t count = ram.Get<uint16_t>(0x80046DA4u), xs = ram.Get<uint32_t>(0x80046DA8u), ys = ram.Get<uint32_t>(0x80046DACu);
        for (uint32_t i = 0; i < count; i++) {
            c.steerCurveXs.push_back(ram.GetP<int16_t>(xs + i * 2));
            c.steerCurveYs.push_back(ram.GetP<int16_t>(ys + i * 2));
        }
    }
    for (uint32_t i = 0; i < 16; i++) c.pedalRates[i] = ram.Get<uint16_t>(0x80046DB0u + i * 2);
    for (uint32_t i = 0; i < 16; i++) {
        c.ground.roughnessAmplitude[i] = ram.Get<int16_t>(0x80046F88u + i * 2);
        c.ground.roughnessFrequency[i] = ram.Get<int16_t>(0x80046F98u + i * 2);
        c.ground.roughnessSpeedScaled[i] = ram.Get<uint8_t>(0x80046FA8u + i);
    }
    for (uint32_t i = 0; i < 2; i++) {
        c.ground.viewYawGain[i] = ram.Get<int32_t>(0x80046C94u + i * 4);
        c.ground.viewYawDamping[i] = ram.Get<int32_t>(0x80046C9Cu + i * 4);
    }
    std::memcpy(&c.ground.effects, ram.At(0x80046EACu), sizeof(c.ground.effects));
    c.dragConstant = ram.Get<int32_t>(0x80046EF0u);
    c.springRateRange[0] = ram.Get<uint8_t>(0x80046DC8u);
    c.springRateRange[1] = ram.Get<uint8_t>(0x80046DC9u);
    std::memcpy(c.diffTypeCodes.data(), ram.At(0x80046DCCu), 8);
    std::memcpy(c.gearAutoTable.data(), ram.At(0x800923E2u), c.gearAutoTable.size());
    std::memcpy(c.aiGripPercent.data(), ram.At(0x801C98A4u), 8);
    std::memcpy(c.raceStateTable.data(), ram.At(0x80046DD4u), c.raceStateTable.size());
    c.gameMode = ram.Get<uint8_t>(0x801D5866u);
    c.viewMode = ram.Get<uint8_t>(0x801C9990u);
    c.flag800A951C = ram.Get<uint8_t>(0x800A951Cu);
    c.flag801C9995 = ram.Get<uint8_t>(0x801C9995u);
    c.flag800AF232 = ram.Get<uint8_t>(0x800AF232u);
    c.word801C98A0 = ram.Get<uint32_t>(0x801C98A0u);
    c.byte801D5869 = ram.Get<uint8_t>(0x801D5869u);

    RaceCourseData& course = out.course;
    out.courseIndex = ram.Get<uint8_t>(0x800AF230u);
    course.dirtCourse = (ram.Get<uint16_t>(0x801E18E8u + uint32_t(out.courseIndex) * 24u + 8u) & 4) != 0; // course table of 0x80060E94
    c.shellControlClass = ShellControlClass(ram, course.dirtCourse);
    {
        const int32_t count = ram.Get<int32_t>(0x800B4A58u);
        for (int32_t i = 0; i < count && i < 16; i++) course.startLineDistances.push_back(ram.Get<int32_t>(0x800B4A5Cu + uint32_t(i) * 4u));
    }
    {
        const uint32_t raceObject = ram.Get<uint32_t>(0x801C8568u);
        for (uint32_t i = 0; i < 7; i++) {
            const uint32_t list = ram.GetP<uint32_t>(raceObject + 8 + i * 4);
            if (list == 0) continue;
            const int32_t count = ram.GetP<int32_t>(list);
            for (int32_t k = 0; k < count && k < 4096; k++) {
                RaceSection section;
                std::memcpy(&section, ram.AtP(list + 4 + uint32_t(k) * sizeof(RaceSection)), sizeof(RaceSection));
                course.sections[i].push_back(section);
            }
        }
        const uint32_t grid = ram.GetP<uint32_t>(raceObject + 0x18);
        course.grid.count = grid ? ram.GetP<int32_t>(grid) : 0;
        for (uint32_t slot = 0; slot < 12; slot++) { // the dump's list has 12 records; the reset path indexes them without a count check
            course.grid.distance.push_back(grid ? ram.GetP<int32_t>(grid + 4 + slot * 0x28 + 0x14) : 0);
            course.grid.heading.push_back(grid ? ram.GetP<int32_t>(grid + 4 + slot * 0x28 + 0x24) : 0);
        }
    }

    const uint32_t carCount = ram.Get<uint8_t>(0x800AF231u);
    for (uint32_t slot = 0; slot < carCount && slot < kMaxCars; slot++) {
        CarParams params;
        std::memcpy(&params, ram.At(0x801DE8BAu + slot * uint32_t(sizeof(CarParams))), sizeof(CarParams)); // 0x801C98E0 + 0x14FDA
        out.params.push_back(params);
        Car car;
        std::memcpy(&car, ram.At(0x800A9688u + slot * uint32_t(sizeof(Car))), sizeof(Car));
        out.cars.push_back(car);
    }
    std::memcpy(&out.contact, ram.At(0x801C8608u), sizeof(CarContactState));
    out.holdFrames = ram.Get<uint16_t>(0x800A9520u);

    // The shell's inputs of the derivations, as dumped (disc_data.h names the fields).
    RaceSettings& s = out.settings;
    s.controlWord = c.word801C98A0;
    s.aiGripPercent = c.aiGripPercent;
    for (uint32_t i = 0; i < 4; i++) {
        s.cornerGripPercent[i] = ram.Get<uint8_t>(0x801C98ACu + i);
        s.speedScalePercent[i] = ram.Get<uint8_t>(0x801C98B0u + i);
    }
    s.wearLimit = ram.Get<uint8_t>(0x801C98BDu);
    s.wornGripLossPercent = ram.Get<uint8_t>(0x801C98BEu);
    s.pitGripPercent = ram.Get<uint8_t>(0x801C98BFu);
    s.coldLimit = ram.Get<uint8_t>(0x801C98C0u);
    s.coldGripLossPercent = ram.Get<uint8_t>(0x801C98C1u);
    s.wearKnee = ram.Get<uint8_t>(0x801C98C2u);
    s.kneeGripLossPercent = ram.Get<uint8_t>(0x801C98C3u);
    ShellState& sh = out.shell;
    sh.gameMode = c.gameMode;
    sh.modeFlag5D = ram.Get<uint8_t>(0x801D585Du);
    sh.modeFlag60 = ram.Get<uint8_t>(0x801D5860u);
    sh.modeFlag65 = ram.Get<uint8_t>(0x801D5865u);
    sh.frameRateMode = ram.Get<uint8_t>(0x801D5864u);
    sh.viewMode = c.viewMode;
    sh.flag800A951C = c.flag800A951C;
    sh.flag801C9995 = c.flag801C9995;
    sh.flag800AF232 = c.flag800AF232;
    sh.byte801D5869 = c.byte801D5869;
    sh.raceClock = c.word80046F64;
    return true;
}

namespace {

std::string Hex(const void* p, size_t n) {
    static const char* const digits = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        const uint8_t b = static_cast<const uint8_t*>(p)[i];
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

struct Differ {
    std::vector<std::string> lines;
    template <typename T> void Scalar(const char* name, const T& a, const T& b) {
        if (a != b) lines.push_back(std::string(name) + ": " + std::to_string(int64_t(a)) + " vs " + std::to_string(int64_t(b)));
    }
    void Bytes(const char* name, const void* a, const void* b, size_t n) {
        if (std::memcmp(a, b, n) != 0) lines.push_back(std::string(name) + ": " + Hex(a, n) + " vs " + Hex(b, n));
    }
    template <typename T> void Vector(const char* name, const std::vector<T>& a, const std::vector<T>& b) {
        if (a.size() != b.size()) { lines.push_back(std::string(name) + ": " + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + " entries"); return; }
        for (size_t i = 0; i < a.size(); i++)
            if (a[i] != b[i]) { lines.push_back(std::string(name) + "[" + std::to_string(i) + "]: " + std::to_string(int64_t(a[i])) + " vs " + std::to_string(int64_t(b[i]))); return; }
    }
};

} // namespace

std::vector<std::string> DiffSimConstants(const SimConstants& a, const SimConstants& b) {
    Differ d;
    d.Scalar("step.frameTime 0x801C856C", a.step.frameTime, b.step.frameTime);
    d.Scalar("step.rate 0x801C8570", a.step.rate, b.step.rate);
    d.Scalar("step.draftDragFloor 0x80046EF4", a.step.draftDragFloor, b.step.draftDragFloor);
    d.Bytes("wear 0x80046F48..", &a.wear, &b.wear, sizeof(TyreWearConstants));
    d.Vector("rollingXs 0x80046EF8", a.rollingXs, b.rollingXs);
    d.Vector("rollingYs 0x80046F18", a.rollingYs, b.rollingYs);
    d.Bytes("surfaceRolling 0x80046E00", a.surfaceRolling.data(), b.surfaceRolling.data(), sizeof(a.surfaceRolling));
    for (size_t i = 0; i < 4; i++) {
        const std::string name = "classTuning[" + std::to_string(i) + "] 0x801C8690";
        d.Bytes(name.c_str(), &a.classTuning[i], &b.classTuning[i], sizeof(DriveClassTuning));
    }
    d.Scalar("slideSensitivity 0x80046EE8", a.slideSensitivity, b.slideSensitivity);
    d.Scalar("steerSpringGain 0x80046F3C", a.steerSpringGain, b.steerSpringGain);
    d.Scalar("steerDamping 0x80046F40", a.steerDamping, b.steerDamping);
    d.Scalar("steerCentring 0x80046F44", a.steerCentring, b.steerCentring);
    d.Vector("steerCurveXs 0x80046D7C", a.steerCurveXs, b.steerCurveXs);
    d.Vector("steerCurveYs 0x80046D90", a.steerCurveYs, b.steerCurveYs);
    d.Bytes("pedalRates 0x80046DB0", a.pedalRates.data(), b.pedalRates.data(), sizeof(a.pedalRates));
    d.Bytes("ground.roughnessAmplitude 0x80046F88", a.ground.roughnessAmplitude, b.ground.roughnessAmplitude, sizeof(a.ground.roughnessAmplitude));
    d.Bytes("ground.roughnessFrequency 0x80046F98", a.ground.roughnessFrequency, b.ground.roughnessFrequency, sizeof(a.ground.roughnessFrequency));
    d.Bytes("ground.roughnessSpeedScaled 0x80046FA8", a.ground.roughnessSpeedScaled, b.ground.roughnessSpeedScaled, sizeof(a.ground.roughnessSpeedScaled));
    d.Bytes("ground.viewYawGain 0x80046C94", a.ground.viewYawGain, b.ground.viewYawGain, sizeof(a.ground.viewYawGain));
    d.Bytes("ground.viewYawDamping 0x80046C9C", a.ground.viewYawDamping, b.ground.viewYawDamping, sizeof(a.ground.viewYawDamping));
    d.Bytes("ground.effects 0x80046EAC", &a.ground.effects, &b.ground.effects, sizeof(a.ground.effects));
    d.Scalar("dragConstant 0x80046EF0", a.dragConstant, b.dragConstant);
    d.Bytes("springRateRange 0x80046DC8", a.springRateRange.data(), b.springRateRange.data(), 2);
    d.Bytes("diffTypeCodes 0x80046DCC", a.diffTypeCodes.data(), b.diffTypeCodes.data(), 8);
    d.Bytes("gearAutoTable 0x800923E2", a.gearAutoTable.data(), b.gearAutoTable.data(), a.gearAutoTable.size());
    d.Bytes("aiGripPercent 0x801C98A4", a.aiGripPercent.data(), b.aiGripPercent.data(), 8);
    d.Bytes("raceStateTable 0x80046DD4", a.raceStateTable.data(), b.raceStateTable.data(), a.raceStateTable.size());
    d.Scalar("word801C98A0", a.word801C98A0, b.word801C98A0);
    return d.lines;
}

std::vector<std::string> DiffShellState(const SimConstants& a, const SimConstants& b) {
    Differ d;
    d.Scalar("gameMode 0x801D5866", a.gameMode, b.gameMode);
    d.Scalar("shellControlClass 0x800418E8", a.shellControlClass, b.shellControlClass);
    d.Scalar("viewMode 0x801C9990", a.viewMode, b.viewMode);
    d.Scalar("flag800A951C", a.flag800A951C, b.flag800A951C);
    d.Scalar("flag801C9995", a.flag801C9995, b.flag801C9995);
    d.Scalar("flag800AF232", a.flag800AF232, b.flag800AF232);
    d.Scalar("byte801D5869", a.byte801D5869, b.byte801D5869);
    d.Scalar("word80046F64 (race clock)", a.word80046F64, b.word80046F64);
    return d.lines;
}

std::vector<std::string> DiffRaceCourseData(const RaceCourseData& a, const RaceCourseData& b) {
    Differ d;
    d.Scalar("dirtCourse", a.dirtCourse, b.dirtCourse);
    d.Vector("startLineDistances 0x800B4A58", a.startLineDistances, b.startLineDistances);
    for (size_t li = 0; li < 7; li++) {
        const std::string name = "sections[" + std::to_string(li) + "]";
        if (a.sections[li].size() != b.sections[li].size()) {
            d.lines.push_back(name + ": " + std::to_string(a.sections[li].size()) + " vs " + std::to_string(b.sections[li].size()) + " records");
            continue;
        }
        for (size_t i = 0; i < a.sections[li].size(); i++)
            if (std::memcmp(&a.sections[li][i], &b.sections[li][i], sizeof(RaceSection)) != 0) {
                d.lines.push_back(name + "[" + std::to_string(i) + "]: " + Hex(&a.sections[li][i], sizeof(RaceSection)) + " vs " + Hex(&b.sections[li][i], sizeof(RaceSection)));
                break;
            }
    }
    d.Scalar("grid.count", a.grid.count, b.grid.count);
    d.Vector("grid.distance", a.grid.distance, b.grid.distance);
    d.Vector("grid.heading", a.grid.heading, b.grid.heading);
    return d.lines;
}

} // namespace gt2::sim::dev
