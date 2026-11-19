#pragma once

namespace gt2 {

// PS instead of the US build's hp (the GT2+ metric-horsepower patch). Display only: stored, simulated and verified
// values stay in PS because that is what the executable's tables carry.
inline bool metricPower = false;

// The original shows (PS * 1000) / 1014 with the "%dhp" strings.
inline int DisplayPower(int ps) { return metricPower ? ps : ps * 1000 / 1014; }

} // namespace gt2
