#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include "ps1_pad.h"

namespace gt2::input {
// Our pedal resistance; the PS1 actuator bytes remain the source of grip rumble.
struct DualSenseState {
    uint8_t small = 0, large = 0, accelerator = 0, brake = 0;
    uint8_t rumblePercent = 100;
    bool operator==(const DualSenseState&) const = default;
};
std::array<uint8_t, 78> DualSenseReport(const DualSenseState& state, bool bluetooth, uint8_t sequence);
enum class DualSenseTransport { Unsupported, Usb, Bluetooth };
DualSenseTransport DualSenseReportTransport(uint16_t inputLength, uint16_t outputLength);

bool DecodeDualSenseInput(std::span<const uint8_t> report, bool bluetooth, Ps1PadFrame& pad);

class DualSenseEffects {
public:
    explicit DualSenseEffects(const wchar_t* devicePath);
    ~DualSenseEffects();
    bool Available() const;
    // 1: native input (neutral on stale data), 0: not initialized, -1: disconnected.
    int ReadPad(Ps1PadFrame& pad) const;
    void Motors(uint8_t small, uint8_t large, int strength);
    void Triggers(uint8_t accelerator, uint8_t brake);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
