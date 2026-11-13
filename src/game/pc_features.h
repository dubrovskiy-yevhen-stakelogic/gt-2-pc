#pragma once
#include <cstdint>
namespace gt2::pc {
// Optional native menu overrides. Save-game bytes and earned records are never changed.
inline bool unlockCourses = false;
inline bool unlockCars = false;
inline bool unlockSimulationEvents = false;
inline bool simulationDamage = false; // Latched when loading a GT event; changing preferences must not change its replay.
inline bool skipRaceReplays = false; // no automatic replay after a race finish or a pause-menu exit
inline uint64_t unlockRevision = 0;
inline void SetUnlocks(bool courses, bool cars) {
    if (courses != unlockCourses || cars != unlockCars) ++unlockRevision;
    unlockCourses = courses;
    unlockCars = cars;
}
}
