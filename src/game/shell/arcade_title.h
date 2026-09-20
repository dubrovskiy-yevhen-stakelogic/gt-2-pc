#pragma once
// Pieces of the title overlay (GT2.OVL member 1) that the Simulation port runs inside tools/gt2game/title_mode.cpp and that the
// US Arcade v1.1 disc's title needs as well (its member 1 is the Simulation title's code, shifted: docs/research/arcade_disc.md
// sections 16.1 and 17.8).
//
// The first-boot view: Simulation 0x8004B900 {0x80016BD8, 0x80016C28, 0x80016F6C}, Arcade 0x8004AE28 (the arcade entry
// 0x800112D0 pushes it while 0x801EF020 == 0, then sets the byte; Simulation entry 0x80011384 with 0x801EF5F0). Slot 1 status 0
// and "BASCUS-94455GAME" present -> read (0x8006A1B4) with "Loading Save Data..." and the progress bar of the object 0x8004B82C
// ((96, 320), 4 x 24, pitch 5, colour 0x90500C); CRC ok -> 0x8006A278 (the block replaces the career) and "Auto Loading
// Complete" for 180 fields; mismatch -> "Save Data is Corrupt!" 240 fields; read error -> "Auto Loading Failed" 240 fields; a
// face button (0xF00) ends the message early; no card / no file -> the view ends at once (docs/research/menus_gtmode.md 10.1).
// The strings are named by their Simulation addresses (TitleAssets::Text maps them on the arcade disc).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/title_screens.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/title_assets.h"

namespace gt2::shell {

class TitleBootLoad {
public:
    // The view's start (0x80016BD8): active only when card 1 is ready and holds the game's file.
    TitleBootLoad(const TitleAssets& assets, const CardSlot& slot1);
    bool Active() const { return state_ != 0; }
    // 0x80016C28: one field. Returns false once the view has ended. After "Auto Loading Complete" `state` holds the card's block.
    bool Update(const MenuListPad& pad, career::CareerState& state);
    // 0x80016F6C over the view manager's clear: the message, "Memory Card 1" and the progress bar.
    std::vector<MenuPrim> Frame() const;
    bool Loaded() const { return loaded_; }
    std::vector<std::string> log;
    int sectorsPerField = 8; // ours: the card transfer shown over a short progress run (CardManager::sectorsPerField)

private:
    const TitleAssets& assets_;
    CardSlot slot_;
    int state_ = 0, timer_ = 0, remaining_ = 0;
    uint32_t colour_ = 0x604610;
    std::string line_;
    std::array<int8_t, 32> segments_{};
    bool progress_ = false, loaded_ = false;
};

} // namespace gt2::shell
