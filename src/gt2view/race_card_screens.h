#pragma once
// The race overlay's memory-card views over the executable's card manager (shell::CardManager, title_screens.h): US
// Simulation v1.2 addresses (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; ovl0 = GT2.OVL member 0), the
// same code in the US Arcade v1.1 build (member 0 0x90 lower in data, 0xE0 in code; the EXE 0xF0 lower; the post-race views of
// arcade_disc.md 17.10 - the TIME TRIAL menu's "Save Ghost ...", "Load Ghost ...", "Save Replay ..." rows):
//   view 0x8005B51C "SAVE REPLAY" (colour 0x6F): init 0x8005019C -> EXE 0x80072E7C (mode 0: 0x800724F8 packs the replay with
//        0x80069948 and describes it), update 0x800501BC, draw 0x8005021C;
//   view 0x8005B540 "SAVE GHOST" (0xC0): init 0x80050240 -> 0x80072EB4 (mode 0: 0x80072598 packs the ghost with 0x80069CC0 -
//        race block entry 1 0x801D5988 (0xD0), its parameter record 0x801DEA7A (0x1C0), the reference lap's head 0x801DA4A0
//        (0xE0) and stream object 0x801DA580 (0x19 + its used bytes) - and describes it with + 0x42 = 1);
//   view 0x8005B564 "LOAD GHOST" (0x6F0000): init 0x800502E4 -> 0x80072F54 (mode 3), update 0x80050304: on the manager's exit
//        0x80072F8C (the loaded flag 0x801C94E0) -> 0x801D55AA = 1 and 0x8002F4B1 = 1 (the next race's ghost: 0x80069D58 unpacked
//        the entry into those addresses), sound 4, back.
// The updates call 0x800728F0 with the view manager's pad block (M + 0x1A8) and go back (2) when it exits; the draws call
// 0x80072B78 with the view's OT + 4 (the manager's OT slots 4 / 5 / 6). The manager itself was set up by 0x80050494 with the
// object 0x8005B500: fonts +0xC 0x801C9110 (tiny), +0x10 0x801C9120 (small), +0x14 0x801C9150 (medium), page 6.
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/title_replay.h"
#include "game/shell/title_screens.h"
#include "gt2formats/race_menu_assets.h"
#include "gt2formats/title_assets.h"
#include "gt2view/race_result_screens.h"

namespace gt2::screens {

// The card manager's assets in the race overlay: `text` (the title's assets of the disc's build in the Simulation layout,
// TitleAssets::SimLayoutScreens: the executable image and the global text block data-global.txd the manager's strings are in)
// with the race menus' fonts (in the TitleAssets font order: header, medium 0x801C9150, small 0x801C9120, tiny 0x801C9110) and
// VRAM (the fonts on page 6).
TitleAssets RaceCardAssets(const RaceMenuAssets& race, const TitleAssets& text);

class CardView : public PostRaceView {
public:
    static constexpr uint32_t kSaveReplayView = 0x8005B51Cu, kSaveGhostView = 0x8005B540u, kLoadGhostView = 0x8005B564u;
    // `cardAssets` (RaceCardAssets) and `text` must outlive the view.
    CardView(const RaceMenuAssets& race, const TitleAssets& cardAssets, const shell::ReplayRowText& text, uint32_t view, std::array<shell::CardSlot, 2> slots);
    // 0x800501BC / 0x80050260 / 0x80050304: 0 running, 2 = the manager exited (back to the view below; sound 4).
    int Update(const MenuListPad* pad, bool input = true) override;
    void Draw(MenuOt& ot) const override; // 0x80072B78 into ot[4] / ot[5] / ot[6]
    std::string Title() const override { return title_; }
    uint32_t Colour() const override { return colour_; }
    shell::CardManager& Manager() { return *card_; }
    const shell::CardManager& Manager() const { return *card_; }
    uint32_t View() const { return view_; }

private:
    uint32_t view_;
    std::string title_;
    uint32_t colour_ = 0;
    career::CareerState unused_{}; // the save modes' career (not used by modes 0 / 3)
    std::unique_ptr<shell::CardManager> card_;
};

} // namespace gt2::screens
