#pragma once
#include "platform/input/input_system.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace gt2::input::detail {
class FakePad : public Device {
public:
    struct Item {
        int field = 0, hold = -1; // hold -1: until the next item of the control
        std::string control;
        int value = 0;
    };
    explicit FakePad(std::vector<Item> items, int port = 1) : items_(std::move(items)), port_(port) {
        std::stable_sort(items_.begin(), items_.end(), [](const Item& a, const Item& b) { return a.field < b.field; });
    }
    int Port() const { return port_; } // the PS1 port the script drives (--fake-pad 1, --fake-pad2 2)
    bool Poll(int field, Ps1PadFrame& out) override {
        out = Ps1PadFrame{};
        out.type = kTypeAnalog;
        out.pressure = true;
        // Per control: the latest item at or before this field wins; an item with a hold returns the control to rest
        // after `hold` fields.
        std::map<std::string, const Item*> latest;
        for (const Item& it : items_) {
            if (it.field > field) break;
            latest[it.control] = &it;
        }
        std::map<std::string, int> values;
        for (const auto& [name, it] : latest)
            if (it->hold < 0 || field < it->field + it->hold) values[name] = it->value;
        static const std::map<std::string, uint16_t> kButtons = {
            {"select", ps1::kSelect}, {"l3", ps1::kL3},     {"r3", ps1::kR3},           {"start", ps1::kStart},   {"up", ps1::kUp},
            {"right", ps1::kRight},   {"down", ps1::kDown}, {"left", ps1::kLeft},       {"l2", ps1::kL2},         {"r2", ps1::kR2},
            {"l1", ps1::kL1},         {"r1", ps1::kR1},     {"triangle", ps1::kTriangle}, {"circle", ps1::kCircle}, {"cross", ps1::kCross},
            {"square", ps1::kSquare}};
        for (const auto& [name, v] : values) {
            const auto b = kButtons.find(name);
            if (b != kButtons.end()) {
                if (v) out.buttons = uint16_t(out.buttons | b->second);
            } else if (name == "type") {
                out.type = uint8_t(v);
            } else if (name == "rx" || name == "a0") {
                out.analog[0] = uint8_t(v);
            } else if (name == "ry" || name == "a1") {
                out.analog[1] = uint8_t(v);
            } else if (name == "lx" || name == "a2") {
                out.analog[2] = uint8_t(v);
            } else if (name == "ly" || name == "a3") {
                out.analog[3] = uint8_t(v);
            } else if (name == "r2p") {
                out.pressureR2 = uint8_t(v);
            } else if (name == "l2p") {
                out.pressureL2 = uint8_t(v);
            } else if (name == "pressure") {
                out.pressure = v != 0;
            }
        }
        if (out.type == kTypeNegcon && !values.count("a1") && !values.count("a2") && !values.count("a3")) out.analog[1] = out.analog[2] = out.analog[3] = 0;
        field_ = field;
        return true;
    }
    void SetMotors(uint8_t smallMotor, uint8_t largeMotor, int strength) override {
        largeMotor = uint8_t(int(largeMotor) * strength / 100);
        if (strength == 0) smallMotor = 0;
        if (smallMotor == smallMotor_ && largeMotor == largeMotor_) return;
        smallMotor_ = smallMotor, largeMotor_ = largeMotor;
        std::printf("fake-pad f%d: motors small %u large %u\n", field_, unsigned(smallMotor), unsigned(largeMotor));
    }
    bool HasMotors() const override { return true; }
    std::string Name() const override { return "fake pad"; }
    bool Scripted() const override { return true; }

private:
    std::vector<Item> items_;
    int port_ = 1;
    int field_ = 0;
    uint8_t smallMotor_ = 0, largeMotor_ = 0;
};

inline std::vector<FakePad::Item> ParseFakePad(const std::string& scriptOrFile) {
    std::string text = scriptOrFile;
    if (std::ifstream f{scriptOrFile}; f) { // a file: items separated by commas, newlines or spaces; '#' comments
        std::ostringstream all;
        std::string line;
        while (std::getline(f, line)) {
            const size_t hash = line.find('#');
            if (hash != std::string::npos) line.resize(hash);
            all << line << ',';
        }
        text = all.str();
    }
    static const std::map<std::string, int> kTypes = {{"digital", kTypeDigital}, {"analog", kTypeAnalog}, {"negcon", kTypeNegcon}, {"none", kTypeNone}};
    std::vector<FakePad::Item> items;
    std::string item;
    std::istringstream in(text);
    auto flush = [&] {
        size_t a = item.find_first_not_of(" \t\r\n"), b = item.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) { item.clear(); return; }
        const std::string s = item.substr(a, b - a + 1);
        item.clear();
        const size_t c1 = s.find(':'), eq = s.find('=');
        if (c1 == std::string::npos || eq == std::string::npos || eq < c1) throw std::runtime_error("bad --fake-pad item " + s);
        const size_t c2 = s.find(':', eq);
        FakePad::Item it;
        it.field = std::atoi(s.substr(0, c1).c_str());
        it.control = s.substr(c1 + 1, eq - c1 - 1);
        for (char& ch : it.control) ch = char(std::tolower(uint8_t(ch)));
        const std::string value = s.substr(eq + 1, c2 == std::string::npos ? std::string::npos : c2 - eq - 1);
        if (it.control == "type") {
            const auto t = kTypes.find(value);
            it.value = t != kTypes.end() ? t->second : std::atoi(value.c_str());
        } else {
            it.value = int(std::strtol(value.c_str(), nullptr, 0));
        }
        if (c2 != std::string::npos) it.hold = std::max(1, std::atoi(s.substr(c2 + 1).c_str()));
        items.push_back(it);
    };
    for (char ch; in.get(ch);) {
        if (ch == ',' || ch == '\n' || ch == ';') flush();
        else item.push_back(ch);
    }
    flush();
    return items;
}

} // namespace gt2::input::detail
