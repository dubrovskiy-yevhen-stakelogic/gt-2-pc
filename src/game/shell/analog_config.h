#pragma once
// The 1P / 2P ANALOG SETTINGS pages of the title's OPTIONS (GT2.OVL member 1, switcher pages 3 / 4), ported from US
// Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Evidence: our objdump / Ghidra
// pseudo-C of work/re/title/ram.bin; rows of tools/gt2verify/verify_pad.cpp; docs/formats/pad_input.md section 6.
//
// The calibration of a neGcon-type controller (type 2: the neGcon, the analogue wheels that answer as one): the 20-byte
// configuration of the port's pad block (+0x3E: twist {min, dead lo, dead hi, max}, I / II / L {min, max}) that the
// reader's neGcon handler 0x800832C0 uses (platform/input/ps1_pad.h NegconTwist / NegconPedal). Only the axes the neGcon
// key table (pad block + 0x16) gives to steering / accelerate / brake are calibrated. Entering (0x8001C690) needs such a
// controller in the page's port: first "turn the wheel to the far right, press R" then "far left" (the steering range),
// then a menu (up / down): Set the Center / the Lock Position / the Margin of Steering, the Lock Position / the Margin of
// Acceleration and of Braking (R or START takes the live value, left / right step it), Exit (Cross / Circle) stores
// the models into the calibration (0x8001C210 / 0x8001C270). Triangle / Square during the range steps cancel.
// The help lines (0x800B1408) are drawn by 0x8001AB40 only when the career's byte + 0 is 0 (not in the US game).
#include <array>
#include <cstdint>
#include <vector>

#include "game/shell/key_config.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/title_assets.h"

namespace gt2::shell {

#pragma pack(push, 1)
// The page object (0x800B12D8 port 1, 0x800B1358 port 2).
struct AnalogPageObject {
    int16_t item = 0;        // +00 the selected menu item
    int16_t port = 0;        // +02
    int16_t state = 3;       // +04 0 right end, 1 left end, 2 menu, 3 idle
    int16_t pad06 = 0;
    int32_t steer[5] = {};   // +08 centre, lock, margin, max, min (0x8001ACB4 keeps them consistent)
    int32_t accel[2] = {};   // +1C lock (max), margin (min)
    int32_t brake[2] = {};   // +24
    uint8_t bands[3][0x1C] = {}; // +2C the bands of the three gauges (templates 0x8004C350)
};
static_assert(sizeof(AnalogPageObject) == 0x80);

// The pages' shared globals 0x800B1408..0x800B14B0.
struct AnalogGlobals {
    uint32_t text = 0;        // 0x800B1408 the help line (drawn in the Japanese game only)
    uint32_t unknown140C = 0;
    struct Item { int16_t kind = 0, sub = 0; uint32_t text = 0; };
    std::array<Item, 16> items{}; // 0x800B1410 kind 0 steering, 1 accelerate, 2 brake, 3 exit; sub = which value
    int32_t count = 0;        // 0x800B1490
    int32_t steerOn = 0, accelOn = 0, brakeOn = 0; // 0x800B1494 / 98 / 9C
    uint16_t steerRaw = 0, accelRaw = 0, brakeRaw = 0, pad14A6 = 0; // 0x800B14A0.. the live bytes
    int32_t rightEnd = 0, leftEnd = 0; // 0x800B14A8 / AC
};
static_assert(sizeof(AnalogGlobals) == 0xA8);
#pragma pack(pop)

// Strings / data of member 1 the page uses.
struct AnalogConfigData {
    static constexpr uint32_t kBandTemplates = 0x8004C350u, kPalette = 0x8004C31Cu;
    static constexpr uint32_t kTurnRight = 0x801BA222u, kTurnLeft = 0x801BA259u, kCentre = 0x801BA29Fu, kSteerLock = 0x801BA2C0u,
                              kSteerMargin = 0x801BA2EFu, kAccelLock = 0x801BA30Fu, kAccelMargin = 0x801BA340u, kBrakeLock = 0x801BA364u,
                              kBrakeMargin = 0x801BA38Fu, kExit = 0x801BA292u, kPress = 0x801BA3B0u, kNa = 0x801BA212u, kExitLabel = 0x801BA0EEu,
                              kCentreLabel = 0x801BA1FEu, kMaxLabel = 0x801BA205u, kMarginLabel = 0x801BA20Au, kSteering = 0x801BA107u,
                              kAcceleration = 0x801BA111u, kBrake = 0x801BA120u;
    std::array<std::array<uint8_t, 0x1C>, 3> bandTemplates{};
    std::array<uint32_t, 13> palette{}; // 0x8004C31C..0x8004C34C
    KeyConfigData keys;                 // the neGcon icons of the pedals (0x80019308)
    static AnalogConfigData Read(const GuestImage& ovl1, const GuestImage& exe);
};

// 0x8001C2A0: the menu items of `port` (the neGcon key table's axis functions); true when there is more than Exit.
bool AnalogItems(AnalogGlobals& g, uint8_t padType, const uint8_t* block);
// 0x8001C48C: the page's models from the calibration (pad block + 0x3E) and fresh bands; state 3.
void AnalogReset(AnalogPageObject& o, int port, const uint8_t* block, const AnalogConfigData& data);
// 0x8001C690: enter the page; 0 without a neGcon-type controller (the caller plays sound 0 and does not edit).
int AnalogEnter(AnalogPageObject& o, AnalogGlobals& g, uint8_t padType, const uint8_t* block);
// 0x8001C7B8: one field. `padTypes` / `pads` of both ports (the range steps accept either port's R / START), `raw` = the
// port's four analogue bytes as received (buffer + 4..7). Returns -1 (continue), 0 (Exit: stored) or -2 (left without
// storing).
int AnalogUpdate(AnalogPageObject& o, AnalogGlobals& g, const std::array<uint8_t, 2>& padTypes, const std::array<MenuListPad, 2>& pads,
                 const std::array<uint8_t, 4>& raw, uint8_t* block, std::vector<int>& sounds);
// 0x8001C648 (the page slides out: bands fade) / 0x8001C610 (the page slides in: reset, bands appear).
void AnalogPageLeft(AnalogPageObject& o);
void AnalogPageEntered(AnalogPageObject& o, const uint8_t* block, const AnalogConfigData& data);
// 0x8001CE28 (+ 0x8001B2C0 / 0x8001AF6C steering gauge, 0x8001BC5C / 0x8001B9D8 pedal gauges, 0x8001AE34 / 0x8001B8A0
// "N/A"); the page callback draws it at (x, y + 0x2E). `block` = the page port's pad block.
void AnalogDraw(MenuOtSlot& ot, const TitleAssets& assets, const AnalogConfigData& data, const AnalogPageObject& o, const AnalogGlobals& g,
                uint8_t padType, const uint8_t* block, int x, int y, int alpha);

} // namespace gt2::shell
