#pragma once
// The KEY CONFIGURATION page of the title's OPTIONS (GT2.OVL member 1, page 2 of the switcher 0x8004BFB8), ported from US
// Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a). Evidence: our objdump / Ghidra
// pseudo-C of work/re/title/ram.bin; rows of tools/gt2verify/verify_pad.cpp; docs/formats/pad_input.md section 5.
//
// The page edits the 11-byte key tables of the career's pad blocks (career + 0x0A for port 1, + 0x5C for port 2; 0x52
// bytes: four tables digital / analog / neGcon / Jogcon, vibration byte + 0x2C, page state + 0x2D (4 bytes per table),
// calibration + 0x3E) that the race's logical pad 0x80014BB4 reads (platform/input/ps1_pad.h). Every field: each port's
// list object is loaded from the block by the port's controller type (0x80019388), edited with that port's pad
// (0x80019D5C) and stored back (0x80019498). Rows (0x8004C0DC: the function of each row): Steering (0; left / right
// choose the analog steering source), Acceleration (2), Brake (3), Reverse (5), Emergency Brake (4), Shift Up (6),
// Shift Down (7), Rear View (9), Change Views (8) - press a button to assign it (a button already in use swaps, START
// held + a D-pad direction assigns the direction; on an analog pad left / right on Acceleration / Brake step through the
// stick pedal presets 0x8004C120) -, Default (-1: the executable's tables 0x80091570 / 0x8009157C / 0x80091588) and
// EXIT (-2). The page is left when either port chooses EXIT.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/gt_menu_list.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/title_assets.h"

namespace gt2::shell {

// The list object of one port (0x800B13D8 port 1, 0x800B13F0 port 2) as the original lays it out.
#pragma pack(push, 1)
struct KeyConfigList {
    int32_t type = 3;                 // +00: 0 digital, 1 analog, 2 neGcon, 3 none / other controller
    uint8_t steering = 0;             // +04: analog 0 left stick, 1 right stick, 2 D-pad; neGcon 0 twist, 1 D-pad
    uint8_t preset = 0;               // +05: analog pedal preset (0 = buttons, 1..8 = 0x8004C120)
    uint8_t savedAccel = 0;           // +06: the buttons accelerate / brake return to when the preset goes back to 0
    uint8_t savedBrake = 0;           // +07
    std::array<uint8_t, 11> table{};  // +08: the key table (ps1_pad.h: entries by function)
    uint8_t pad13 = 0;
};
#pragma pack(pop)
static_assert(sizeof(KeyConfigList) == 0x14);

// Member 1 / EXE data of the page (read from the disc).
struct KeyConfigData {
    static constexpr uint32_t kRowFunctions = 0x8004C0DCu, kRowLabels = 0x8004C0F4u, kPresets = 0x8004C120u, kIcons = 0x8004C144u;
    static constexpr uint32_t kButtonIconOf = 0x8004C2DCu, kNegconIconOf = 0x8004C2FCu, kColours = 0x8004C0D0u;
    static constexpr uint32_t kDefaults = 0x80091570u; // EXE: digital, analog (+0x0C), neGcon (+0x18)
    static constexpr int kRows = 11;
    // Icon records (12 bytes: u, v, clut, w, h, tpage): buttons 0..11, neGcon 12..21, analog axes 22..29, steering
    // D-pad 30, left stick 31, right stick 32, twist 33.
    struct Icon { uint8_t u = 0, v = 0; uint16_t clut = 0, w = 0, h = 0, tpage = 0; };
    static constexpr int kIconCount = 34, kNegconIcons = 12, kAxisIcons = 22, kSteerDpad = 30, kSteerLeft = 31, kSteerRight = 32, kSteerTwist = 33;
    std::array<int16_t, kRows> rowFunction{};
    std::array<uint32_t, kRows> rowLabel{};
    std::array<std::array<uint8_t, 2>, 9> presets{};
    std::array<std::array<uint8_t, 11>, 3> defaults{};
    std::array<Icon, kIconCount> icons{};
    std::array<int16_t, 16> buttonIconOf{}, negconIconOf{};
    std::array<uint32_t, 3> colours{}; // base, label, Default / EXIT
    static KeyConfigData Read(const GuestImage& ovl1, const GuestImage& exe);
};

// 0x80019218 / 0x80019250: a neGcon table keeps I / II / L as axes 0x81 / 0x82 / 0x83 where a pad has Cross / Square / L1.
uint8_t NegconEntryOf(uint8_t button);
uint8_t NegconButtonOf(uint8_t entry);
// 0x80019288: the axis icon of an analog pedal entry (0..7).
int AxisIconOf(uint8_t entry);

// 0x80019388: the list of a port from its pad block (`block` = career + 0x0A + port * 0x52) by the controller type
// (ps1_pad.h PadType: 4 -> 0, 5 / 7 -> 1, 2 -> 2, else 3 and nothing loaded).
void LoadKeyList(KeyConfigList& list, uint8_t padType, const uint8_t* block);
// 0x80019498: back into the pad block (types 0..2).
void StoreKeyList(const KeyConfigList& list, uint8_t* block);
// 0x80019598: the function (2..9) that uses generic button `button`, or -1.
int FindKeyUse(const KeyConfigList& list, uint8_t button);
// 0x800196E8: function `function` to `button` (the function that had it takes the old button).
void AssignKey(KeyConfigList& list, int function, uint8_t button);
// 0x80019934 / 0x80019B24 / 0x80019C4C / 0x80019D5C. `pad` = the port's generic pad (held, pressed); `sounds` gets the
// 0x80060840 requests. EditKeyRow returns 0 (nothing: the row may move), 1 (handled) or 2 (EXIT chosen).
int SteeringInput(KeyConfigList& list, const MenuListPad& pad, std::vector<int>& sounds);
int PedalPresetInput(KeyConfigList& list, const MenuListPad& pad, const KeyConfigData& data, std::vector<int>& sounds);
void DefaultKeys(KeyConfigList& list, const KeyConfigData& data);
int EditKeyRow(KeyConfigList& list, const MenuListPad& pad, int row, const KeyConfigData& data, std::vector<int>& sounds);

// The page object 0x800B12D0 (s16 selected row of port 1 / port 2, blink counter, input delay) and its lists.
class KeyConfigPage {
public:
    void Init();  // 0x8001A7E0
    void Enter(); // 0x8001A834: both ports on EXIT, 4 fields without input
    void Leave(); // 0x8001A84C
    // 0x8001A860: `blocks` = the career's pad blocks (career + 0x0A / + 0x5C), `padTypes` = the ports' controller types,
    // `pads` = the ports' pads (null while the page is not edited). Returns 0 when a port chose EXIT, else -1.
    int Update(uint8_t* blocks[2], const std::array<uint8_t, 2>& padTypes, const MenuListPad* pads[2], const KeyConfigData& data, std::vector<int>& sounds);
    // 0x8001AA84 -> 0x8001A624 / 0x80019F44 (the page callback draws it at (x, y + 0x40)).
    void Draw(MenuOtSlot& ot, const TitleAssets& assets, const KeyConfigData& data, int x, int y, int alpha) const;
    int Row(int port) const { return rows_[size_t(port)]; }
    // The original's objects (tools/gt2verify): the page 0x800B12D0 (4 x s16) and the lists 0x800B13D8 / 0x800B13F0.
    void SetState(const std::array<int16_t, 4>& page, const std::array<KeyConfigList, 2>& lists) {
        rows_ = {page[0], page[1]};
        blink_ = page[2], delay_ = page[3];
        lists_ = lists;
    }
    std::array<int16_t, 4> PageState() const { return {rows_[0], rows_[1], blink_, delay_}; }
    const std::array<KeyConfigList, 2>& Lists() const { return lists_; }

private:
    std::array<int16_t, 2> rows_{-1, -1};
    int16_t blink_ = 0, delay_ = -1;
    std::array<KeyConfigList, 2> lists_{};
};

} // namespace gt2::shell
