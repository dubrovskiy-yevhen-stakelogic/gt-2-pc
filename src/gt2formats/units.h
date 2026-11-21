#pragma once
#include <string>

namespace gt2 {

// PS instead of the US build's hp (the GT2+ metric-horsepower patch). Display only: stored, simulated and verified
// values stay in PS because that is what the executable's tables carry.
inline bool metricPower = false;

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
