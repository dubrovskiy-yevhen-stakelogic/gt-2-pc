#pragma once
// The machine settings before a race: the race overlay's PARTS SETTING page (view 0x8005D1E4; enter 0x8005747C, update
// 0x800574C0 -> 0x80056194, draw 0x800575F8 -> 0x80056810), ported in gt2view/race_menus.h (MachineSettingsPage), on
// the car's tune sheet (0x800173E8) with the original's settings rules (EXE 0x8005FC9C get, 0x8005F9DC set, 0x80060410
// default, the slider 0x80054D10) and its commit (0x80056FF0: the garage slot; the race car's configuration is rebuilt
// by the caller). The frame goes full screen on the panels (Panels::Screen::kSettings).
// Keys -> pad bits of the page: arrows = d-pad, Enter = cross, Backspace / Esc = triangle, Q or PageUp = L1, W or
// PageDown = R1 (held with Left / Right: x10, as 0x80054D10), Home = start (the row's setting back to its default);
// also the script names of gt2game: Space = circle, Delete = square, S = start.
// Directions auto-repeat as observed in the original (work/re/spec_msettings/run2: a held direction repeats 28 fields
// after the press, then every 8). Triangle / square in the group selection leaves (commit, Update returns false).
// As the event menu's "Settings ..." of the original, the screen opens with CHANGE PARTS (view 0x8005D1C0, gt2view/change_parts.h)
// on the car's sheet: the parts' stages with the power / torque graph; its L1 switches here (view 0x8005D1E4), R1 here switches
// back (-8); triangle / square on either leaves with the commit. The race block's restrictions of the page (+ 0x586 power limit, + 0x588 course flags) are the ones of
// a tarmac race without a limit (0 / 1, as the Simulation dumps' race blocks) until the caller passes the event's (SetRaceLimits).
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "game/career/garage.h"
#include "game/career/tuning.h"

namespace gt2 {
struct MenuListPad;
struct RaceMenuAssets;
namespace screens {
class MachineSettingsPage;
class ChangePartsView;
}
} // namespace gt2

namespace gt2game {

class GameWindow;
class Panels;

class SettingsScreen {
public:
    SettingsScreen(gt2::career::GarageCar& car, const gt2::career::CareerData& data);
    ~SettingsScreen();
    // Loads the sheet of the car (0x800173E8) and enters the page (0x8005747C: the page opens 12 fields later).
    void Open();
    // One field of input; false once the page was left (the settings are committed then).
    bool Update(GameWindow& window);
    // The page's frame (0x80047024 header + 0x80056810) full screen on the panels.
    void Draw(Panels& panels) const;
    bool Changed() const { return changed_; }
    // The race block's + 0x586 (power limit) / + 0x588 (course flags) for CHANGE PARTS.
    void SetRaceLimits(int16_t powerLimit, uint16_t flags) { powerLimit_ = powerLimit, raceFlags_ = flags; }
    // The effects (0x80060840 ids) of the last Update: the result code's (0x800574C0) and the slider's.
    std::vector<int> sounds;
    // Settings entries the fitted parts allow (0x8005FC9C count >= 1 per setting).
    size_t RowCount() const { return rows_.size(); }
    // Text lines of all settings (console log / tests).
    std::vector<std::string> Describe() const;

private:
    struct Row {
        int32_t setting = 0, entry = 0;
    };
    void Reload();  // rows_ and values_ from the sheet
    gt2::MenuListPad ReadPad(GameWindow& window);
    void Commit();  // 0x80056FF0 into the garage slot

    gt2::career::GarageCar& car_;
    const gt2::career::CareerData& data_;
    std::unique_ptr<gt2::career::TuneSheet> sheet_;
    std::unique_ptr<gt2::screens::MachineSettingsPage> page_;
    std::vector<Row> rows_;
    std::vector<std::vector<gt2::career::SettingValue>> values_; // per setting kind (empty = not available)
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(0x400, 0);
    // The page's image (ovl0) and pictures: the panels' settings assets, known after the first Draw.
    mutable const gt2::RaceMenuAssets* assets_ = nullptr;
    bool entered_ = false;       // 0x8005747C done (page initialised)
    int countdown_ = 0;          // view +0x14: 12 at the enter, 0x80055FD0 when it reaches 0
    std::array<int, 32> heldFor_{}; // fields since each pad bit's press while held (-1 = not held)
    bool changed_ = false;
    void OpenParts();
    std::unique_ptr<gt2::screens::ChangePartsView> parts_; // CHANGE PARTS while it is shown
    bool partsFirst_ = true;
    int16_t powerLimit_ = 0;
    uint16_t raceFlags_ = 1;
};

} // namespace gt2game
