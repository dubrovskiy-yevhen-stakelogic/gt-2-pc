// The race overlay's memory-card views (race_card_screens.h).
#include "gt2view/race_card_screens.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "gt2view/race_record_screens.h"

namespace gt2::screens {

TitleAssets RaceCardAssets(const RaceMenuAssets& race, const TitleAssets& text) {
    TitleAssets a = text;
    a.fonts = race.fonts;
    a.vram = race.vram;
    return a;
}

CardView::CardView(const RaceMenuAssets& race, const TitleAssets& cardAssets, const shell::ReplayRowText& text, uint32_t view, std::array<shell::CardSlot, 2> slots)
    : view_(view) {
    title_ = race.Text(race.ovl0.Get<uint32_t>(view + 0x10));
    colour_ = race.ovl0.Get<uint32_t>(view + 0x0C);
    shell::CardManager::Mode mode = shell::CardManager::kSaveReplay; // 0x80072E7C / 0x80072EB4
    if (view == kLoadGhostView) mode = shell::CardManager::kLoadGhost; // 0x80072F54
    else if (view != kSaveReplayView && view != kSaveGhostView) throw std::runtime_error("CardView: not a card view of the race overlay");
    card_ = std::make_unique<shell::CardManager>(cardAssets, mode, unused_, std::move(slots));
    card_->SetReplayText(&text);
    card_->SetKeyboard(MakeTitleCardKeyboard(cardAssets));
}

int CardView::Update(const MenuListPad* pad, bool input) { // 0x800501BC (0x80050260 / 0x80050304 the same around 0x800728F0)
    sounds.clear();
    const bool running = card_->Update(input ? pad : nullptr);
    sounds = card_->sounds;
    if (running) return 0;
    if (card_->StateId() != shell::CardManager::kExit) return 0; // 0x28 (a loaded replay) is not an exit of these views
    sounds.push_back(4);
    return 2;
}

void CardView::Draw(MenuOt& ot) const { card_->Draw(ot[4], ot[5], ot[6]); } // 0x8005021C: 0x80072B78(OT + 4)

} // namespace gt2::screens
