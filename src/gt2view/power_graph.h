#pragma once
// The executable's power / torque graph widget (US Simulation v1.2, SCUS_944.88, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; US Arcade v1.1 SCUS_944.55 SHA-1 231f9dba...: the same code at -0xF0):
//   0x80073CE4 reset, 0x80073CFC open (0x80073DA4: the axes' scales), 0x80073D30 close, 0x80073D4C tick, 0x800745B0 one
//   curve, 0x800747D0 the axes (0x80073EDC power / torque axis, 0x80074274 rpm axis), and the curve sampler 0x8007489C
//   (0x80074844: linear interpolation) that turns the power figures of 0x80075930 into 80 samples every 250 rpm.
// Only the race overlay (GT2.OVL member 0) calls it: CHANGE PARTS (ovl0 0x80053CA8 / 0x800536A4, the widget object
// ovl0 0x8005C42C) shows the car's current curves and the selected stage's preview. No other code refers to these
// functions (no jal, no function pointer in the executable or any member of either disc); the arcade CAR SELECTION's
// graph is member 2's own widget (game/arcade/arcade_car_page.h ArcGraph) and the GT-mode menus (member 4) print the
// figures as text only. Evidence: our disassembly and Ghidra pseudo-C (work/re/change_parts/sim_decomp).
// docs/formats/race_screens.md section 8.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/gt_menu_list.h"
#include "gt2formats/hud_assets.h"
#include "gt2formats/overlay_data.h"

namespace gt2::screens {

constexpr int kPowerGraphSamples = 80; // 0x50 samples, 250 rpm apart

// 0x8007489C(count, rpm, power, torque, outPower, outTorque): the figures' `count` points (rpm ascending; 0x80075930
// +0xC / +0x2C / +0x4C) resampled every 250 rpm from 0 by 0x80074844 (x <= x0: v0; x < x1: v0 + (v1 - v0) * (x - x0) /
// (x1 - x0); else v1; x0 < 0: -1): at most 0x4F samples, the rest -1. Point 0 is (-1, -1, -1).
void BuildPowerGraphCurves(int count, const int16_t* rpm, const int16_t* power, const int16_t* torque, int16_t* outPower, int16_t* outTorque);

// The widget object (0x40 bytes; the race overlay's template 0x8005C42C).
struct PowerGraph {
    int16_t span = 8;                // +00 reveal: sample / tick i appears span * i / n fields after the open
    int16_t x = 0, y = 0, w = 0, h = 0; // +02 .. +08
    int8_t page = 6;                 // +0A the text context's page (0x8006AC68)
    int8_t labelGap = 0;             // +0B space between two value labels
    int8_t digitShift = 0;           // +0C the labels' digit shift / the rpm labels' spacing
    int8_t labelHeight = 0;          // +0D
    uint32_t font = 0;               // +10 font descriptor
    uint32_t powerColour = 0, torqueColour = 0, axisColour = 0; // +14 / +18 / +1C
    int16_t rpmMax = 0, powerMax = 0, torqueMax = 0; // +20 / +22 / +24 (set before Open)
    int16_t anim = -1;               // +26 -1 closed, -16..-2 closing, >= 0 open
    int16_t powerScale = 0, powerTicks = 0, powerStep = 0;   // +28 / +2A / +2C (0x80073DA4)
    int16_t torqueScale = 0, torqueTicks = 0, torqueStep = 0; // +2E / +30 / +32
    int16_t rpmTicks = 0, samples = 0; // +34 / +36
    int16_t animEnd = 0;             // +38

    static PowerGraph Read(const GuestImage& image, uint32_t address);
    void Reset();                                  // 0x80073CE4
    void Open(int rpm, int power, int torque);     // +0x20..+0x24 then 0x80073CFC (0x80073DA4)
    void Close();                                  // 0x80073D30
    void Tick();                                   // 0x80073D4C
    bool Shown() const { return anim >= 0; }
};

// What the widget draws with: its text font and the strings of data-global.txd ("hp" 0x801EF6B6, "lb-ft" 0x801EF6B0,
// "1000rpm" 0x801EF6B9, the EXE's "%d" 0x8008FB38).
struct PowerGraphText {
    const HudFont* font = nullptr;
    std::string hp, lbft, rpm1000;
};

// 0x800745B0(graph, ot, samples, which (0 power, 1 torque), alpha): the curve as LINE_F2s (colour = lerp(the curve's
// colour, 0x02DCDCDC, 8 - reveal, 8), then lerp(0x02000000, that, alpha, 0x80)), then E1 0x20.
void DrawPowerGraphCurve(const PowerGraph& g, MenuOtSlot& ot, const int16_t* samples, int which, int alpha);
// 0x800747D0(graph, ot): the power axis (left), the torque axis (right), the rpm axis, then E1 0x20.
void DrawPowerGraphAxes(const PowerGraph& g, MenuOtSlot& ot, const PowerGraphText& text);

} // namespace gt2::screens
