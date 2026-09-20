// The arcade menus' race build (GT2.OVL member 2 of US Arcade v1.1, src/game/arcade/arcade_setup.*) against the original
// in a RAM image taken with member 2 loaded (work/re/arcade_menu/ram.bin: gt2run session snapshot at the race build, field
// 4226). The rows run only on such a dump of the arcade disc; elsewhere they skip. ARCADE v1.1 addresses.
//   ArcadeBuild 0x80010C84(p1, p2, 0x801C3010), game modes 4 (Road Race) and 6 (Rally / Time Trial), a car of the class lists
//     (mode 6 also: the course record car of the career): random selections (level,
//     class, car, tyre table, colour, transmission, laps, course, the two numbers of the menu entry, the career's race
//     option bytes, the VSync counter that seeds the opponent draw) - ALL of RAM compared (the guest stack excepted): race
//     block 0x801D52BC, settings block 0x801C9300, the six records 0x801DE31A.., the race context + selection 0x801C2EB0..
//   ArcadeAvail 0x8001D120 (+ 0x8002357C) on every course list and 0x8001D418 with random career flags / prize records.
//   ArcadeGarageBuild 0x80010C84 with a garage car (selection + 6 = home / guest garage, + 8 = its index): random garage blocks
//     (home = career + 0x3C74 = 0x801CCFB4, guest 0x801D0FDC; random car bytes with a player-table model id), modes 4 and 6 -
//     ALL of RAM compared as ArcadeBuild.
//   ArcadeGarageInfo 0x80019F44 (-> 0x80019E88 per car, 0x80019E44 the class test) over random garage blocks: the header and all
//     100 summaries against GarageCount / GarageEntryOf.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/arcade/arcade_car_page.h"
#include "game/arcade/arcade_setup.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/arcade_menu_data.h"
#include "gt2formats/car_info.h"
#include "gt2formats/course_data.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "guest.h"

namespace gt2::verify {
namespace {

constexpr uint32_t kBuild = 0x80010C84u, kCourseCount = 0x8001D120u, kClassUnlock = 0x8001D418u, kGarageInfo = 0x80019F44u;
constexpr uint32_t kHomeGarageAddress = 0x801CCFB4u; // career + 0x3C74; the guest block follows at + 0x4028 (0x801D0FDC)
constexpr uint32_t kGarageInfoOut = 0x800EFAB8u;     // the home summary of the menus (8 + 100 x 10 bytes)
constexpr uint32_t kCourseListAddress[ArcadeMenuData::kCourseListCount] = {0x80050730u, 0x800509F0u, 0x80050CB0u, 0x80050FB0u,
                                                                            0x800512B0u, 0x800513F0u, 0x800516B0u};

uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
template <typename T> void Put(uint8_t* ram, uint32_t address, T v) { std::memcpy(At(ram, address), &v, sizeof(T)); }
template <typename T> T Get(const uint8_t* ram, uint32_t address) {
    T v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), sizeof(T));
    return v;
}

const char* RegionOf(uint32_t address) {
    using namespace gt2::arcade;
    if (address >= kRaceBlockAddress && address < kRaceBlockAddress + kRaceBlockSize) return "race block";
    if (address >= kSettingsAddress && address < kSettingsAddress + 0x40) return "settings block";
    if (address >= kRecordsAddress && address < kRecordsAddress + 6 * 0x1C0) return "records";
    if (address >= kMenuRegionAddress && address < kSelectionAddress) return "race context";
    if (address >= kSelectionAddress && address < kSelectionAddress + kSelectionSize) return "selection";
    return "elsewhere";
}

} // namespace

int VerifyArcadeMenu(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng, const DiscImage* disc, const GtfsVolume* vol) {
    using namespace gt2::arcade;
    if (!disc || !vol || !ProfileOf(*disc).arcade || !OverlayMemberLoaded(guest.Ram(), *disc, 2, kBuild, 0x400)) {
        std::puts("ArcadeBuild / ArcadeGarageBuild / ArcadeGarageInfo / ArcadeAvail skipped (the dump does not hold the arcade menus: GT2.OVL member 2 of US Arcade v1.1)");
        return 0;
    }
    const auto savedMap = gCodeAddressMap; // member-2 addresses are the dump's own: no translation
    gCodeAddressMap = nullptr;
    int failures = 0;
    try {
        const ArcadeData data = ArcadeData::Load(*vol);
        const ArcadeMenuData menu = ArcadeMenuData::Load(*disc, pristine[kCareerAddress & 0x1FFFFF]);
        const CarInfoDirectory cars = CarInfoDirectory::Load(*vol);
        const CourseInfoTable courses = ParseCourseInfo(vol->Read(".crsinfo"));
        const ArcadeSetupData d{data, menu, cars, courses};

        // ---- ArcadeBuild
        {
            size_t cases = 0, bad = 0, battleCases = 0;
            std::vector<uint8_t> ours(Bus::kRamSize);
            const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
            for (size_t i = 0; i < 300; i++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                std::memset(guest.Scratch(), 0, kScratchSize);
                uint8_t* ram = guest.Ram();
                uint8_t* sel = At(ram, kSelectionAddress);
                // A third of the cases are Rally / Time Trial builds (mode 6): the rally list's cars (class list 6) or a class
                // car, the time trial / rally course lists, a random career course record car (0x8005E674 + 0x14).
                // A quarter of the cases are 2 player Battle builds (mode 0): both players' cars, the 2P course lists.
                const bool battle = rng() % 4 == 0;
                const bool timeTrial = !battle && rng() % 3 == 0, rally = timeTrial && rng() % 2 == 0;
                const int level = int(rng() % 3), cls = rally ? 6 : int(rng() % 4);
                sel[Sel::kLevel] = uint8_t(level);
                sel[Sel::kClass] = uint8_t(cls);
                sel[Sel::kMode] = battle ? 0 : timeTrial ? 6 : 4;
                Put<int16_t>(ram, kSelectionAddress + Sel::kLaps, int16_t(1 + rng() % 99));
                Put<int16_t>(ram, kSelectionAddress + Sel::kGarage, -1);
                if (battle)
                    for (uint32_t pl = 0; pl < 2; pl++) {
                        Put<int16_t>(ram, kSelectionAddress + Sel2P::kGarage + 2 * pl, -1);
                        uint32_t c2;
                        if (rng() % 5 == 0) c2 = data.PlayerCarId(rng() % data.PlayerCarCount());
                        else {
                            const auto& l2 = menu.classes[size_t(rng() % 4)].cars;
                            c2 = PackCarId(l2[rng() % l2.size()]);
                        }
                        Put<uint32_t>(ram, kSelectionAddress + Sel2P::kCar + 4 * pl, c2);
                        sel[Sel2P::kTyres + pl] = uint8_t(rng() % 2);
                        const CarInfoRecord* i2 = cars.Find(c2);
                        Put<int16_t>(ram, kSelectionAddress + Sel2P::kColour + 2 * pl, int16_t(i2 ? rng() % i2->PaintCount() : 0));
                        sel[Sel2P::kTransmission + pl] = uint8_t(rng() % 2);
                    }
                uint32_t car;
                if (rng() % 5 == 0) car = data.PlayerCarId(rng() % data.PlayerCarCount()); // any car of the player table
                else {
                    const auto& list = menu.classes[size_t(cls)].cars;
                    car = PackCarId(list[rng() % list.size()]);
                }
                Put<uint32_t>(ram, kSelectionAddress + Sel::kCar, car);
                Put<uint32_t>(ram, kSelectionAddress + Sel::kCarShown, car);
                sel[Sel::kTyres] = uint8_t(rng() % 2);
                const CarInfoRecord* info = cars.Find(car);
                Put<int16_t>(ram, kSelectionAddress + Sel::kColour, int16_t(info ? rng() % info->PaintCount() : 0));
                sel[Sel::kTransmission] = uint8_t(rng() % 2);
                const auto& list = menu.courses[battle ? (rng() % 4 == 0 ? 6 : 5) : rally ? 4 : timeTrial ? (rng() % 4 == 0 ? 3 : 2) : (rng() % 4 == 0 ? 1 : 0)];
                const ArcadeCourse& course = list[rng() % list.size()];
                std::memset(sel + Sel::kCourseName, 0, 0x40);
                std::memcpy(sel + Sel::kCourseName, course.display.c_str(), course.display.size() + 1);
                Put<uint32_t>(ram, kSelectionAddress + Sel::kCourseId, CourseFileId(course.file));
                Put<int16_t>(ram, kSelectionAddress + Sel::kCourseRecord, int16_t(course.record));
                const uint32_t w2cc = rng() % 3 == 0 ? rng() : rally ? 1u : rng() % 2;
                if (timeTrial) // the career's course records: the record car of every course (0 = none) - the build reads its course's
                    for (uint32_t c = 0; c < uint32_t(courses.entries.size()); c++)
                        Put<uint32_t>(ram, kCareerAddress + 0x218 + c * 0x24 + 0x14, rng() % 2 ? 0u : data.PlayerCarId(rng() % data.PlayerCarCount()));
                Put<uint32_t>(ram, kSelectionAddress + Sel::kWord2CC, w2cc);
                Put<uint32_t>(ram, kSelectionAddress + Sel::kWord2D0, rng());
                for (uint32_t k = 1; k <= 8; k++) ram[(kCareerAddress + k) & 0x1FFFFF] = uint8_t(rng());
                const uint32_t vsync = rng();
                Put<uint32_t>(ram, kVsyncCounterAddress, vsync);
                const uint32_t p1 = rng(), p2 = rng();
                std::memcpy(ours.data(), ram, Bus::kRamSize);
                try {
                    guest.CallWithBios(kBuild, p1, p2, kSelectionAddress);
                } catch (const std::exception& e) {
                    std::printf("    ArcadeBuild case %zu: original trapped (%s)\n", i, e.what());
                    bad++;
                    continue;
                }
                ArcadeRaceSetup s;
                try {
                    s = BuildArcadeRace(d, std::span<const uint8_t>(At(ours.data(), kMenuRegionAddress), kMenuRegionSize),
                                        std::span<const uint8_t>(At(ours.data(), kCareerAddress), 0x7C9C), p1, p2,
                                        vsync + (std::getenv("GT2_VERIFY_PERTURB") ? 1u : 0u)); // self-test of the row: a wrong seed must fail
                } catch (const std::logic_error& e) {
                    std::printf("    ArcadeBuild case %zu: port refused: %s\n", i, e.what());
                    bad++;
                    continue;
                }
                std::memcpy(At(ours.data(), kRaceBlockAddress), s.raceBlock.data(), s.raceBlock.size());
                std::memcpy(At(ours.data(), kSettingsAddress), s.settings.data(), s.settings.size());
                for (size_t r = 0; r < s.records.size(); r++) // the records of the entries the build made (a mode-0 garage player's entry is empty)
                    if (s.raceBlock[0x5C + r * 0xD0 + 0x8E] != 0) std::memcpy(At(ours.data(), kRecordsAddress + uint32_t(r) * 0x1C0), &s.records[r], 0x1C0);
                std::memcpy(At(ours.data(), kMenuRegionAddress), s.menuRegion.data(), s.menuRegion.size());
                cases++;
                battleCases += battle ? 1 : 0;
                if (std::getenv("GT2_VERIFY_TRACE") && i < 4) {
                    std::printf("      ArcadeBuild case %zu: %s car %s laps %u course %s:", i, reinterpret_cast<const char*>(ram + ((kRaceBlockAddress + 0x10) & 0x1FFFFF)),
                                UnpackCarId(car).c_str(), ram[(kRaceBlockAddress + 0x0F) & 0x1FFFFF], reinterpret_cast<const char*>(ram + ((kRaceBlockAddress + 0x20) & 0x1FFFFF)));
                    for (uint32_t e = 0; e < 6; e++) std::printf(" %s", UnpackCarId(Get<uint32_t>(ram, kRaceBlockAddress + 0x5C + e * 0xD0)).c_str());
                    std::printf("\n");
                }
                size_t differ = 0;
                uint32_t first = 0;
                for (uint32_t a = 0; a < Bus::kRamSize; a++) {
                    if (a >= stackLow && a < stackHigh) continue;
                    if (ours[a] != ram[a]) {
                        if (differ++ == 0) first = a;
                    }
                }
                if (differ) {
                    if (bad < 4)
                        std::printf("    MISMATCH ArcadeBuild case %zu (%s, %s, tyres %u, laps %d): %zu bytes differ, first 0x%08X (%s): original %02X ours %02X\n", i,
                                    menu.eventNames[size_t(level)][size_t(cls)].c_str(), UnpackCarId(car).c_str(), sel[Sel::kTyres],
                                    int(Get<int16_t>(ram, kSelectionAddress + Sel::kLaps)), differ, 0x80000000u + first, RegionOf(0x80000000u + first), ram[first],
                                    ours[first]);
                    bad++;
                }
            }
            std::printf("    ArcadeBuild: %zu of the cases are 2 player Battle builds (mode 0)\n", battleCases);
            Report("ArcadeBuild", kBuild, cases, bad, failures);
        }

        // ---- ArcadeGarageBuild
        {
            size_t cases = 0, bad = 0, battleCases = 0;
            std::vector<uint8_t> ours(Bus::kRamSize);
            const uint32_t stackLow = (kStack & 0x1FFFFF) - 0x2000, stackHigh = (kStack & 0x1FFFFF) + 0x100;
            for (size_t i = 0; i < 200; i++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                std::memset(guest.Scratch(), 0, kScratchSize);
                uint8_t* ram = guest.Ram();
                uint8_t* sel = At(ram, kSelectionAddress);
                const bool battle = rng() % 3 == 0; // mode 0: one or both players on a garage car (the other from the class lists)
                const bool timeTrial = !battle && rng() % 3 == 0, rally = timeTrial && rng() % 2 == 0;
                const int level = int(rng() % 3), cls = rally ? 6 : int(rng() % 4);
                const int g = int(rng() % 2);
                const uint32_t block = kHomeGarageAddress + uint32_t(g) * uint32_t(kGarageStride);
                const int count = 1 + int(rng() % 100), index = int(rng() % uint32_t(count));
                Put<int16_t>(ram, block, int16_t(count));
                for (int c = 0; c < count; c++) {
                    const uint32_t carAt = block + 4 + uint32_t(c) * uint32_t(kGarageCarSize);
                    for (uint32_t k = 0; k < kGarageCarSize; k++) ram[(carAt + k) & 0x1FFFFF] = uint8_t(rng());
                    const uint32_t model = data.PlayerCarId(rng() % data.PlayerCarCount());
                    const CarInfoRecord* info = cars.Find(model);
                    Put<uint32_t>(ram, carAt + 0x8C, model);
                    Put<int32_t>(ram, carAt + 4, int32_t(info ? rng() % info->PaintCount() : 0));
                }
                sel[Sel::kLevel] = uint8_t(level);
                sel[Sel::kClass] = uint8_t(cls);
                sel[Sel::kMode] = battle ? 0 : timeTrial ? 6 : 4;
                Put<int16_t>(ram, kSelectionAddress + Sel::kLaps, int16_t(1 + rng() % 99));
                Put<int16_t>(ram, kSelectionAddress + Sel::kGarage, int16_t(g));
                Put<int16_t>(ram, kSelectionAddress + Sel::kGarageSlot, int16_t(index));
                if (battle) {
                    // Player `pg` on this garage's car `index`; the other on the other garage (filled the same way) or a class car.
                    const uint32_t pg = rng() % 2, other = 1 - pg;
                    const int mode2 = int(rng() % 3); // 0 class car, 1 the same garage, 2 the other garage (random bytes)
                    Put<int16_t>(ram, kSelectionAddress + Sel2P::kGarage + 2 * pg, int16_t(g));
                    Put<int16_t>(ram, kSelectionAddress + Sel2P::kGarageSlot + 2 * pg, int16_t(index));
                    sel[Sel2P::kTransmission + pg] = uint8_t(rng() % 2);
                    Put<uint32_t>(ram, kSelectionAddress + Sel2P::kCar + 4 * pg, rng());
                    if (mode2 == 0) {
                        Put<int16_t>(ram, kSelectionAddress + Sel2P::kGarage + 2 * other, -1);
                        const auto& l2 = menu.classes[size_t(rng() % 4)].cars;
                        const uint32_t c2 = PackCarId(l2[rng() % l2.size()]);
                        Put<uint32_t>(ram, kSelectionAddress + Sel2P::kCar + 4 * other, c2);
                        sel[Sel2P::kTyres + other] = uint8_t(rng() % 2);
                        const CarInfoRecord* i2 = cars.Find(c2);
                        Put<int16_t>(ram, kSelectionAddress + Sel2P::kColour + 2 * other, int16_t(i2 ? rng() % i2->PaintCount() : 0));
                    } else {
                        const int g2 = mode2 == 1 ? g : 1 - g;
                        const uint32_t block2 = kHomeGarageAddress + uint32_t(g2) * uint32_t(kGarageStride);
                        if (g2 != g) {
                            for (int c = 0; c < 100; c++) {
                                const uint32_t carAt = block2 + 4 + uint32_t(c) * uint32_t(kGarageCarSize);
                                for (uint32_t k = 0; k < kGarageCarSize; k++) ram[(carAt + k) & 0x1FFFFF] = uint8_t(rng());
                                Put<uint32_t>(ram, carAt + 0x8C, data.PlayerCarId(rng() % data.PlayerCarCount()));
                                Put<int32_t>(ram, carAt + 4, 0);
                            }
                            Put<int16_t>(ram, block2, 100);
                        }
                        Put<int16_t>(ram, kSelectionAddress + Sel2P::kGarage + 2 * other, int16_t(g2));
                        Put<int16_t>(ram, kSelectionAddress + Sel2P::kGarageSlot + 2 * other, int16_t(rng() % uint32_t(g2 == g ? count : 100)));
                    }
                    sel[Sel2P::kTransmission + other] = uint8_t(rng() % 2);
                }
                const uint32_t car = Get<uint32_t>(ram, block + 4 + uint32_t(index) * uint32_t(kGarageCarSize) + 0x8C);
                Put<uint32_t>(ram, kSelectionAddress + Sel::kCar, car);
                Put<uint32_t>(ram, kSelectionAddress + Sel::kCarShown, car);
                sel[Sel::kTyres] = uint8_t(rng() % 2);
                Put<int16_t>(ram, kSelectionAddress + Sel::kColour, int16_t(rng() % 4));
                sel[Sel::kTransmission] = uint8_t(rng() % 2);
                const auto& list = menu.courses[rally ? 4 : timeTrial ? (rng() % 4 == 0 ? 3 : 2) : (rng() % 4 == 0 ? 1 : 0)];
                const ArcadeCourse& course = list[rng() % list.size()];
                std::memset(sel + Sel::kCourseName, 0, 0x40);
                std::memcpy(sel + Sel::kCourseName, course.display.c_str(), course.display.size() + 1);
                Put<uint32_t>(ram, kSelectionAddress + Sel::kCourseId, CourseFileId(course.file));
                Put<int16_t>(ram, kSelectionAddress + Sel::kCourseRecord, int16_t(course.record));
                Put<uint32_t>(ram, kSelectionAddress + Sel::kWord2CC, rng() % 3 == 0 ? rng() : rally ? 1u : rng() % 2);
                Put<uint32_t>(ram, kSelectionAddress + Sel::kWord2D0, rng());
                for (uint32_t k = 1; k <= 8; k++) ram[(kCareerAddress + k) & 0x1FFFFF] = uint8_t(rng());
                const uint32_t vsync = rng();
                Put<uint32_t>(ram, kVsyncCounterAddress, vsync);
                const uint32_t p1 = rng(), p2 = rng();
                std::memcpy(ours.data(), ram, Bus::kRamSize);
                try {
                    guest.CallWithBios(kBuild, p1, p2, kSelectionAddress);
                } catch (const std::exception& e) {
                    std::printf("    ArcadeGarageBuild case %zu: original trapped (%s)\n", i, e.what());
                    bad++;
                    continue;
                }
                ArcadeRaceSetup s;
                try {
                    const GarageBlocks garages{std::span<const uint8_t>(At(ours.data(), kHomeGarageAddress), kGarageStride),
                                               std::span<const uint8_t>(At(ours.data(), kHomeGarageAddress + uint32_t(kGarageStride)), kGarageStride)};
                    s = BuildArcadeRace(d, std::span<const uint8_t>(At(ours.data(), kMenuRegionAddress), kMenuRegionSize),
                                        std::span<const uint8_t>(At(ours.data(), kCareerAddress), 0x7C9C), p1, p2,
                                        vsync + (std::getenv("GT2_VERIFY_PERTURB") ? 1u : 0u), garages);
                } catch (const std::logic_error& e) {
                    std::printf("    ArcadeGarageBuild case %zu: port refused: %s\n", i, e.what());
                    bad++;
                    continue;
                }
                std::memcpy(At(ours.data(), kRaceBlockAddress), s.raceBlock.data(), s.raceBlock.size());
                std::memcpy(At(ours.data(), kSettingsAddress), s.settings.data(), s.settings.size());
                for (size_t r = 0; r < s.records.size(); r++) // the records of the entries the build made (a mode-0 garage player's entry is empty)
                    if (s.raceBlock[0x5C + r * 0xD0 + 0x8E] != 0) std::memcpy(At(ours.data(), kRecordsAddress + uint32_t(r) * 0x1C0), &s.records[r], 0x1C0);
                std::memcpy(At(ours.data(), kMenuRegionAddress), s.menuRegion.data(), s.menuRegion.size());
                cases++;
                battleCases += battle ? 1 : 0;
                size_t differ = 0;
                uint32_t first = 0;
                for (uint32_t a = 0; a < Bus::kRamSize; a++) {
                    if (a >= stackLow && a < stackHigh) continue;
                    if (ours[a] != ram[a] && differ++ == 0) first = a;
                }
                if (differ) {
                    if (bad < 4)
                        std::printf("    MISMATCH ArcadeGarageBuild case %zu (mode %u, garage %d car %d of %d, %s): %zu bytes differ, first 0x%08X (%s): original %02X ours %02X\n", i,
                                    sel[Sel::kMode], g, index, count, UnpackCarId(car).c_str(), differ, 0x80000000u + first, RegionOf(0x80000000u + first), ram[first], ours[first]);
                    bad++;
                }
            }
            std::printf("    ArcadeGarageBuild: %zu of the cases are 2 player Battle builds (mode 0)\n", battleCases);
            Report("ArcadeGarageBuild", kBuild, cases, bad, failures);
        }

        // ---- ArcadeGarageInfo: 0x80019F44(out, block)
        {
            size_t cases = 0, bad = 0;
            for (size_t i = 0; i < 200; i++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                uint8_t* ram = guest.Ram();
                const uint32_t block = kHomeGarageAddress + uint32_t(rng() % 2) * uint32_t(kGarageStride);
                const uint32_t mode = rng() % 3; // 0: any bytes, 1: power < 4 (incl. the no-power path), 2: plausible cars
                for (uint32_t k = 0; k < kGarageStride; k++) ram[(block + k) & 0x1FFFFF] = uint8_t(rng());
                Put<int16_t>(ram, block, int16_t(rng() % 4 == 0 ? rng() : rng() % 101));
                for (uint32_t c = 0; c < 100; c++) {
                    const uint32_t carAt = block + 4 + c * uint32_t(kGarageCarSize);
                    if (mode == 1) Put<uint16_t>(ram, carAt + 0x98, uint16_t((rng() & 0xC000u) | (rng() % 4)));
                    if (mode == 2) {
                        Put<uint16_t>(ram, carAt + 0x98, uint16_t((rng() & 0xC000u) | (50 + rng() % 900)));
                        Put<uint16_t>(ram, carAt + 0x94, uint16_t((rng() & 0xE000u) | (600 + rng() % 1400)));
                    }
                }
                const std::vector<uint8_t> before(ram, ram + Bus::kRamSize);
                guest.CallWithBios(kGarageInfo, kGarageInfoOut, block);
                const std::span<const uint8_t> garage(before.data() + (block & 0x1FFFFF), kGarageStride);
                bool same = Get<int16_t>(ram, kGarageInfoOut) == GarageCount(garage) && Get<int16_t>(ram, kGarageInfoOut + 2) == 0 &&
                            Get<uint32_t>(ram, kGarageInfoOut + 4) == block;
                for (uint32_t c = 0; c < 100; c++) {
                    const ArcGarageEntry e = GarageEntryOf(garage.subspan(4 + c * kGarageCarSize, kGarageCarSize));
                    const uint32_t at = kGarageInfoOut + 8 + c * 10;
                    same = same && ram[(at + 1) & 0x1FFFFF] == e.rally && ram[(at + 2) & 0x1FFFFF] == e.cls && ram[(at + 3) & 0x1FFFFF] == e.drive &&
                           Get<int16_t>(ram, at + 4) == e.power && Get<int16_t>(ram, at + 6) == e.torque && Get<int16_t>(ram, at + 8) == e.weight;
                }
                cases++;
                if (!same) {
                    if (bad < 3) std::printf("    MISMATCH ArcadeGarageInfo case %zu (mode %u)\n", i, mode);
                    bad++;
                }
            }
            Report("ArcadeGarageInfo", kGarageInfo, cases, bad, failures);
        }

        // ---- ArcadeAvail: 0x8001D120 per list (reverse lists with flag 1, as 0x8001D210 calls it) and 0x8001D418.
        {
            size_t cases = 0, bad = 0;
            for (size_t i = 0; i < 200; i++) {
                std::memcpy(guest.Ram(), pristine.data(), Bus::kRamSize);
                uint8_t* ram = guest.Ram();
                // Random prize records (byte +1 of the 0xA4 records of the six tiers) and course record flags.
                const uint32_t density = rng() % 4;
                for (uint32_t t = 0; t < 6; t++)
                    for (uint32_t k = 0; k < 10; k++) ram[(kCareerAddress + 0x1418 + t * 0x668 + k * 0xA4 + 1) & 0x1FFFFF] = uint8_t(rng() % 4 < density + (t < 2 ? 1u : 0u) ? 1 : 0);
                for (uint32_t k = 0; k < 0x20; k++) ram[(kCareerAddress + 0xB8 + k) & 0x1FFFFF] = uint8_t(rng() % 3 == 0 ? rng() : 0);
                const std::vector<uint8_t> before(ram, ram + Bus::kRamSize);
                const std::span<const uint8_t> careerSpan(before.data() + (kCareerAddress & 0x1FFFFF), 0x7C9C);
                bool same = true;
                for (size_t l = 0; l < ArcadeMenuData::kCourseListCount; l++) {
                    const bool reverse = l == 1 || l == 3;
                    const uint32_t n = guest.CallWithBios(kCourseCount, kCourseListAddress[l], reverse ? 1u : 0u);
                    const std::vector<uint8_t> flags = CourseAvailability(menu.courses[l], reverse, careerSpan);
                    uint32_t ourCount = 0;
                    for (size_t r = 0; r < flags.size(); r++) {
                        ourCount += flags[r] ? 1u : 0u;
                        const uint32_t row = kCourseListAddress[l] + uint32_t(r) * 0x20;
                        if (ram[(row + 0x1C) & 0x1FFFFF] != flags[r] || Get<uint32_t>(ram, row + 0x10) != CourseFileId(menu.courses[l][r].file)) same = false;
                    }
                    if (n != ourCount) same = false;
                }
                guest.CallWithBios(kClassUnlock);
                const ClassUnlocks u = ComputeClassUnlocks(menu, careerSpan);
                for (size_t k = 0; k < u.classS.size(); k++) same = same && ram[(menu.classes[0].flagsAddress + k) & 0x1FFFFF] == u.classS[k];
                for (size_t k = 0; k < u.bonus.size(); k++) same = same && ram[(menu.classes[6].flagsAddress + k) & 0x1FFFFF] == u.bonus[k];
                same = same && (Get<uint16_t>(ram, 0x800F365Cu) != 0) == u.classSOpen;
                cases++;
                if (!same) {
                    if (bad < 3) std::printf("    MISMATCH ArcadeAvail case %zu\n", i);
                    bad++;
                }
            }
            Report("ArcadeAvail", kCourseCount, cases, bad, failures);
        }
    } catch (...) {
        gCodeAddressMap = savedMap;
        throw;
    }
    gCodeAddressMap = savedMap;
    return failures;
}

} // namespace gt2::verify
