#pragma once
// LOAD GUEST GARAGE of the arcade menus (view 0x800523DC of GT2.OVL member 2 of US Arcade v1.1, SCUS_944.55 SHA-1
// 231f9dba7191b9ef915621662afdc40a7c66df95; ARCADE v1.1 addresses): init 0x80023478, update 0x800234C8, draw 0x80023550 over the
// member's own memory-card manager (object *(u32*)0x800F36E0; set-up 0x80025178 with the block 0x80051FD0, open 0x8002521C ->
// 0x80025030, per field 0x80025258, draw 0x80025390; the state handlers h(0 enter / 1 step / 2 draw) of the table 0x80052810).
// Its states are copies of the executable's card manager (Simulation 0x800728F0, shell::CardManager) restricted to reading:
//   0 0x80024A24 (Simulation 0x80071888)  wait 24 fields
//   1 0x80024A7C (0x800718E0)  error: the text of +0x24, bar 0 (ERROR!, Change Slot / Exit)
//   2 0x80024BF0 (0x80071A54)  "Select a Slot": bar 1 (Memory Card Slot, Slot1 / Slot2)
//   3 0x80024CCC               checking the slot: status 1 wait, 2 -> 4, 4 -> 1, 0 with "BASCUS-94455GAME" -> 6, else -> 5
//   4 0x80024DCC (0x80071E54)  no card: bar 2
//   5 0x80024EB8 (0x80072380)  no game file: bar 0
//   6 0x800246B8 (0x80071530)  start loading?: bar 4
//   7 0x800247D0 (~0x80071648) loading (0x8006A0C4 read of the 0x7EA0-byte file, progress 0x800527D0), CRC 0x8006A224 -> the file's
//                              home garage (file + 0x200 + 0x3C74, 0x4028 bytes) to the guest block RAM 0x801D0FDC (0x8006A1C4)
//   8 0x80024930               loaded: "No Cars Found" (0x800F8475) when the garage is empty; bar 3
// Returning 8 makes the view rebuild the guest garage's summary (0x80019F44 at 0x800EFEA8); 10 = exit (the view goes back).
// Ours: the card slots are .mcd images (shell::CardSlot / CardStatus); the transfer runs over `sectorsPerField` (the original's
// rate is the card hardware's).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/arcade/arcade_course_page.h"
#include "game/arcade/arcade_menus.h"
#include "game/arcade/arcade_widgets.h"
#include "game/shell/title_screens.h"
#include "gt2view/race_menus.h"

namespace gt2::arcade {

// The executable's two-button bar (US Arcade v1.1 EXE: init 0x8006E0DC, open 0x8006E298, close 0x8006E30C, update 0x8006E34C,
// draw 0x8006E4C8, text template 0x80091BC0, zero colour 0x80091BDC - Simulation 0x8006E1CC .. instruction for instruction,
// screens::ResultBar). Template (member 2): +0 x, +2 y, +4 title, +8 / +0xC labels, +0x10 / +0x14 colours, +0x18 fill,
// +0x1C gradient, +0x20 flags, +0x22 label spacing, +0x24 height, +0x28 font, +0x2C sound.
struct ArcExeBar {
    int16_t x = 0, y = 0;
    screens::TextObject title, label0, label1;
    int8_t sound = -1, cursor = 0;
    int16_t w = 80, h = 12, anim = -1, slide = 0;
    uint16_t flags = 0;
    uint32_t fill = 0, gradient = 0, zero = 0;
    void Init(const ArcadeMenuAssets& a, uint32_t templateAddress, int cursor = 0);
    void Open();
    void Close();
    int Update(const MenuListPad* pad, std::vector<int>& sounds); // -2 nothing, -1 back, 0 / 1 chosen, -3 moved
    void Draw(const ArcadeMenuAssets& a, ViewOt& ot, int slot) const; // into slots slot .. slot + 2
};

class ArcadeGuestLoad {
public:
    ArcadeGuestLoad(const ArcadeMenuAssets& a, std::array<shell::CardSlot, 2> slots);
    void Enter(); // 0x80023478
    // 0x800234C8: 0 stay, 2 back (after the exit), 3 the guest garage was loaded (Garage() holds it).
    int Update(const MenuListPad* pad, std::vector<int>& sounds);
    void Draw(ViewOt& ot) const; // 0x80023550 (-> 0x80025390 into slot 4)
    std::span<const uint8_t> Garage() const { return garage_; }
    std::vector<std::string> log;
    int sectorsPerField = 8;

private:
    int EnterState(int id);                 // h(0, 0)
    int Step();                             // h(1, 0)
    void Switch(int id);                    // 0x80024FCC
    int Status() const;                     // *(s16 *)(+0xC)[slot]
    std::string Text(uint32_t address) const;
    const HudFont& Font(uint32_t descriptor) const; // the manager's font copies (0x800F3708 / 0x800F36E8 / 0x800F36F8)

    const ArcadeMenuAssets& a_;
    std::array<shell::CardSlot, 2> slots_;
    std::array<ArcExeBar, 5> bars_;            // +0x50 + k * 0x98
    std::array<int, 5> results_{};          // bar k's update result (+0xE4 + k * 0x98)
    ArcGrowBox band_;                       // +0x34 "Memory Card N"
    int state_ = -1, slot_ = 0, delay_ = 0; // +0x10 slot, +0x32 delay
    uint32_t line1_ = 0, line2_ = 0, error_ = 0; // +0x28 / +0x2C / +0x24
    // the progress object +0x348 (template 0x800527D0: x, y, w, h, pitch; colour, total, visible, 32 segments)
    int16_t progX_ = 0, progY_ = 0, progW_ = 0, progH_ = 0, progPitch_ = 0, progVisible_ = -1;
    uint32_t progColour_ = 0, progTotal_ = 0;
    std::array<int8_t, 32> progress_{};
    int transferLeft_ = 0;
    std::vector<uint8_t> garage_;           // the loaded guest garage (0x4028 bytes)
    std::vector<int>* sounds_ = nullptr;
};

} // namespace gt2::arcade
