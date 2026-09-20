#include "dualsense.h"
#include <algorithm>

namespace gt2::input {
bool DecodeDualSenseInput(std::span<const uint8_t> report, bool bt, Ps1PadFrame& pad) {
    if (report.empty()) return false;
    const bool simple = bt && report[0] == 1;
    size_t offset = 1;
    if (simple) {
        if (report.size() < 10) return false;
    } else if (bt) {
        if (report[0] != 0x31 || report.size() < 78) return false;
        uint32_t crc = 0xFFFFFFFFu;
        auto byte = [&](uint8_t b) {
            crc ^= b;
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
        };
        byte(0xA1);
        for (size_t i = 0; i < 74; ++i) byte(report[i]);
        const uint32_t stored = uint32_t(report[74]) | uint32_t(report[75]) << 8 | uint32_t(report[76]) << 16 | uint32_t(report[77]) << 24;
        if (~crc != stored) return false;
        offset = 2;
    } else if (report[0] != 1 || report.size() < 64) return false;
    const auto p = report.subspan(offset);
    Ps1PadFrame result;
    result.type = kTypeAnalog;
    result.analog = {p[2], p[3], p[0], p[1]};
    result.pressure = true;
    result.pressureL2 = p[simple ? 7 : 4];
    result.pressureR2 = p[simple ? 8 : 5];
    const uint8_t face = p[simple ? 4 : 7], shoulders = p[simple ? 5 : 8];
    constexpr uint16_t directions[] = {ps1::kUp, ps1::kUp | ps1::kRight, ps1::kRight, ps1::kRight | ps1::kDown,
        ps1::kDown, ps1::kDown | ps1::kLeft, ps1::kLeft, ps1::kLeft | ps1::kUp};
    if ((face & 15) < 8) result.buttons = directions[face & 15];
    constexpr uint16_t faces[] = {ps1::kSquare, ps1::kCross, ps1::kCircle, ps1::kTriangle};
    constexpr uint16_t other[] = {ps1::kL1, ps1::kR1, ps1::kL2, ps1::kR2, ps1::kSelect, ps1::kStart, ps1::kL3, ps1::kR3};
    for (unsigned i = 0; i < 4; ++i) if (face & (0x10 << i)) result.buttons |= faces[i];
    for (unsigned i = 0; i < 8; ++i) if (shoulders & (1 << i)) result.buttons |= other[i];
    pad = result;
    return true;
}
DualSenseTransport DualSenseReportTransport(uint16_t inputLength, uint16_t outputLength) {
    // Output caps describe the longest report, including reports we never send.
    if (outputLength > 1024) return DualSenseTransport::Unsupported;
    if (inputLength == 64 && outputLength >= 48) return DualSenseTransport::Usb;
    if (inputLength == 78 && outputLength >= 78) return DualSenseTransport::Bluetooth;
    return DualSenseTransport::Unsupported;
}
std::array<uint8_t, 78> DualSenseReport(const DualSenseState& s, bool bt, uint8_t seq) {
    std::array<uint8_t, 78> r{};
    r[0] = bt ? 0x31 : 0x02;
    const size_t p = bt ? 3 : 1;
    if (bt) { r[1] = uint8_t((seq & 15) << 4); r[2] = 0x10; }
    r[p] = 0x0F; // compatible rumble, disable audio haptics, update both triggers
    const unsigned strength = std::min<unsigned>(s.rumblePercent, 100);
    r[p + 2] = uint8_t((s.small & 1) ? 127 * strength / 100 : 0);
    r[p + 3] = uint8_t(unsigned(s.large) * strength / 200);
    auto resistance = [&](size_t at, uint8_t strength, unsigned start) {
        if (!strength) { r[at] = 0x05; return; } // release
        r[at] = 0x21; // feedback: ten positions, each has a three-bit force
        uint16_t mask = 0;
        uint32_t forces = 0;
        for (unsigned zone = start; zone < 10; ++zone) {
            mask |= uint16_t(1u << zone);
            forces |= uint32_t(std::min<unsigned>(strength, 8) - 1) << (3 * zone);
        }
        r[at + 1] = uint8_t(mask); r[at + 2] = uint8_t(mask >> 8);
        for (unsigned i = 0; i < 4; ++i) r[at + 3 + i] = uint8_t(forces >> (8 * i));
    };
    resistance(p + 10, s.accelerator, 2);
    resistance(p + 21, s.brake, 1);
    if (bt) {
        uint32_t crc = 0xFFFFFFFFu;
        auto byte = [&](uint8_t b) {
            crc ^= b;
            for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0);
        };
        byte(0xA2); // Bluetooth HID output seed
        for (size_t i = 0; i < 74; ++i) byte(r[i]);
        crc = ~crc;
        for (unsigned i = 0; i < 4; ++i) r[74 + i] = uint8_t(crc >> (8 * i));
    }
    return r;
}
}
