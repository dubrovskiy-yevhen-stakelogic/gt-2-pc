#pragma once
#include <cstdint>
namespace gt2::pc {
// Optional native menu overrides. Save-game bytes and earned records are never changed.
inline bool unlockCourses = false;
inline bool unlockCars = false;
inline bool unlockSimulationEvents = false;
inline bool simulationDamage = false; // Latched when loading a GT event; changing preferences must not change its replay.
inline bool skipRaceReplays = false; // no automatic replay after a race finish or a pause-menu exit
// GT2+'s stock-turbo Turbo section (Change Parts, 0x8005F7C8). Auto applies the mod's fix only on a GT2+ disc;
// On applies it everywhere (A-Spec and retail included); Off keeps the retail menu on every disc.
enum class TurboSectionFix : uint8_t { kAuto, kOn, kOff };
inline TurboSectionFix turboSectionFix = TurboSectionFix::kAuto;
inline bool AppliesTurboSectionFix(bool isGt2PlusDisc) {
    return turboSectionFix == TurboSectionFix::kOn || (turboSectionFix == TurboSectionFix::kAuto && isGt2PlusDisc);
}
inline uint64_t unlockRevision = 0;
inline void SetUnlocks(bool courses, bool cars) {
    if (courses != unlockCourses || cars != unlockCars) ++unlockRevision;
    unlockCourses = courses;
    unlockCars = cars;
}
}
