#pragma once
// Scripted pad input shared by the gt2run commands: "field:button[:fields],..." - a press starts at `field`
// (1-based, one field = one VBlank) and is held for `fields` fields (6 unless given), e.g.
// "1400:cross,2000:right,2060:cross,4300:cross:1300". A button prefixed "p2." is pressed on a second pad in port 2
// (e.g. "3000:p2.cross"); a script with such presses connects that pad (Machine::pad2Connected).
#include <cstdint>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace gt2 {

struct ScriptPress { uint64_t at = 0, length = 6; uint16_t mask = 0; int port = 0; };

inline std::vector<ScriptPress> ParseInputScript(const std::string& script) {
    static const std::map<std::string, int> kButtons = {{"select", 0}, {"start", 3}, {"up", 4}, {"right", 5}, {"down", 6}, {"left", 7},
                                                       {"l2", 8}, {"r2", 9}, {"l1", 10}, {"r1", 11}, {"triangle", 12}, {"circle", 13},
                                                       {"cross", 14}, {"square", 15}};
    std::vector<ScriptPress> presses;
    for (size_t pos = 0; pos < script.size();) {
        size_t comma = script.find(',', pos), colon = script.find(':', pos);
        if (comma == std::string::npos) comma = script.size();
        if (colon == std::string::npos || colon > comma) throw std::runtime_error("bad input script near: " + script.substr(pos));
        size_t colon2 = script.find(':', colon + 1);
        if (colon2 > comma) colon2 = comma;
        std::string name = script.substr(colon + 1, colon2 - colon - 1);
        ScriptPress p;
        if (name.rfind("p2.", 0) == 0) {
            p.port = 1;
            name = name.substr(3);
        }
        auto it = kButtons.find(name);
        if (it == kButtons.end()) throw std::runtime_error("unknown button in script: " + name);
        p.at = std::strtoull(script.c_str() + pos, nullptr, 10);
        p.mask = uint16_t(1u << it->second);
        if (colon2 < comma) p.length = std::strtoull(script.c_str() + colon2 + 1, nullptr, 10);
        presses.push_back(p);
        pos = comma + 1;
    }
    return presses;
}

// The pad state of one field under a script (bit set = pressed, PS1 bit order); `port` 0 = port 1, 1 = port 2.
inline uint16_t ScriptButtons(const std::vector<ScriptPress>& presses, uint64_t field, int port = 0) {
    uint16_t buttons = 0;
    for (const ScriptPress& p : presses)
        if (p.port == port && field >= p.at && field < p.at + p.length) buttons |= p.mask;
    return buttons;
}

// The script presses buttons of the pad in port 2.
inline bool ScriptUsesPort2(const std::vector<ScriptPress>& presses) {
    for (const ScriptPress& p : presses)
        if (p.port == 1) return true;
    return false;
}

} // namespace gt2
