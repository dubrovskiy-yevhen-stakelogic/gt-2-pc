#pragma once
#include <cstdint>
#include <string>

namespace gt2 {

// Power units: Auto shows PS on a GT2+ disc (the mod's metric-horsepower patch) and the US build's hp otherwise;
// hp / PS force one. Display only: stored, simulated and verified values stay in PS because that is what the
// executable's tables carry.
enum class PowerUnits : uint8_t { kAuto, kHp, kPs };
inline PowerUnits powerUnits = PowerUnits::kAuto;
inline bool gt2PlusDisc = false; // the disc's executable is GT2+ Complete (set when it is loaded, exe_profile.h)
inline bool metricPower = false;

inline void ApplyPowerUnits() {
    metricPower = powerUnits == PowerUnits::kPs || (powerUnits == PowerUnits::kAuto && gt2PlusDisc);
}

// The original shows (PS * 1000) / 1014 with the "%dhp" strings.
inline int DisplayPower(int ps) { return metricPower ? ps : ps * 1000 / 1014; }

// Preserve the disc's format and spacing while matching its power label to the selected unit.
inline std::string DisplayPowerText(std::string text) {
    if (!metricPower) return text;
    for (size_t i = 0; i + 1 < text.size(); ++i)
        if ((text[i] == 'h' || text[i] == 'H') && (text[i + 1] == 'p' || text[i + 1] == 'P')) {
            text[i] = 'P'; text[i + 1] = 'S'; ++i;
        }
    return text;
}

} // namespace gt2
