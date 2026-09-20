#pragma once
// gt2game on the US Arcade v1.1 disc: the title's Replay Theater, Options / Save Game / Load Game, Data Transfer and the first-boot
// auto-load (arcade_mode.h runs the title list and the menus). The theater / transfer views are member 1's Simulation code shifted
// (docs/formats/title.md section 12): the Simulation ports (title_replay / title_copy / title_screens / title_transfer) run here on
// the arcade assets. The arcade member 1 is the Simulation title's code (shifted: docs/research/arcade_disc.md
// section 17.8), so the screens are the Simulation port's (game/shell: OptionsScreen, CardManager, TitleBootLoad) on the arcade
// assets in the Simulation layout (TitleAssets::SimLayoutScreens: the tables of member 1 / the executable at their Simulation
// addresses, the arcade texts through the build profile). One career block is the session's: the arcade menus read it (laps
// + 3 / + 6, course flags + 0xB8, music volume + 0xB3), the options edit it, Save writes it ("BASCUS-94455GAME", the same file
// as the Simulation disc's), Load / the auto-load replace it. The views' music: CD tracks 6 (options) / 7 (save, load) of the
// arcade EXE's music table (member 1 0x80012380 -> EXE 0x80080E34, as the Simulation 0x80012414 -> 0x80080F24).
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "arcade_mode.h"
#include "game/career/career_state.h"
#include "game/shell/arcade_title.h"
#include "game/shell/title_options.h"
#include "game/shell/title_screens.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/replay_card.h"
#include "gt2formats/title_assets.h"

namespace gt2 {
class GtfsVolume;
class CarInfoDirectory;
namespace shell {
class ReplayTheaterMenu;
class DemonstrationScreen;
class CopyReplayScreen;
class TransferManager;
class TradeScreen;
struct ReplayRowText;
} // namespace shell
} // namespace gt2

namespace gt2game {

class GameWindow;

class ArcadeTitleScreens {
public:
    // `arcadeTitle` = TitleAssets::LoadArcade of the disc; `career` = the session's career block (0x7C9C bytes).
    ArcadeTitleScreens(const gt2::TitleAssets& arcadeTitle, const gt2::GtfsVolume& vol, const ArcadeModeOptions& options, std::span<const uint8_t> career);
    ~ArcadeTitleScreens();
    ArcadeTitleScreens(const ArcadeTitleScreens&) = delete;
    ArcadeTitleScreens& operator=(const ArcadeTitleScreens&) = delete;

    // The first-boot view (0x8004AE28): true when card 1 holds the game's file and the view runs (Update / Frame until it ends).
    bool StartBoot();
    // A title result (0x801EF023): Replay Theater (1), Options (2), Save Game (3), Load Game (4), Data Transfer (5) open their
    // screen and return true; others false.
    bool Open(int titleResult);
    // Replay Theater: a replay chosen by Load Replay / Demonstration after the 20-field view 0x8004B320 / 0x8004B278 (arcade
    // 0x8004A894 / 0x8004A7EC): the caller plays it (the race overlay with argument 1: PlayTitleReplay) and then calls
    // ReplayPlayed(), which enters the theater again (0x801EF022 == 2: member 1 starts in the theater).
    bool TakeReplay(gt2::ReplayPayload& payload, std::string& title);
    void ReplayPlayed();
    // One field of the open screen; false once it has ended (the title list comes back, 0x800113A4).
    bool Update(const gt2::MenuListPad& pad, GameWindow& window);
    std::vector<gt2::MenuPrim> Frame() const;
    const gt2::MenuVram& Vram() const { return assets_.vram; }
    std::vector<int> sounds; // the menu sound requests of the last Update (0x80060750)

    // The session's career block (the arcade menus get it at Start Game and give it back at their exit).
    std::span<const uint8_t> Career() const { return {reinterpret_cast<const uint8_t*>(&career_), sizeof(career_)}; }
    void SetCareer(std::span<const uint8_t> career);
    // The options of the career for the races (race_view.h RaceViewConfig: volumes, camera, view angle, chase view, the pad
    // block of player 1) and ours (PC SETTINGS: units, trigger pedals, ...).
    ArcadeModeOptions RaceOptions(const ArcadeModeOptions& base) const;

private:
    // The theater / transfer views (as title_mode.cpp's Screen for the Simulation title; the arcade member 1 runs the same code,
    // docs/formats/title.md section 12).
    enum class Sub { kNone, kTheaterEnter, kTheater, kCard, kCopy, kDemo, kTheaterLeave, kReplayReady, kTransferEnter, kTransfer, kTransferCard, kTrade };
    void EnterTheater(bool fresh);
    bool UpdateSub(const gt2::MenuListPad& pad, GameWindow& window);
    void PlayMusic(int track);
    void StopMusic();
    void SyncSettings();

    gt2::TitleAssets assets_;          // the arcade title's assets in the Simulation layout (SimLayoutScreens)
    std::string discPath_;
    bool noSound_ = false;
    gt2::GuestImage nativeExe_;        // the arcade executable in its own layout (the music table through its profile)
    gt2::career::CareerState career_{};
    gt2::shell::PcSettings settings_;  // main's settings (units, graphics, ...) with the career's options and pad blocks
    std::array<gt2::shell::CardSlot, 2> slots_;
    std::unique_ptr<gt2::shell::OptionsScreen> options_;
    std::unique_ptr<gt2::shell::CardManager> card_;
    std::unique_ptr<gt2::shell::TitleBootLoad> boot_;
    struct Music;
    std::unique_ptr<Music> music_;
    const gt2::GtfsVolume& vol_;
    gt2::GuestImage nativeOvl1_;       // member 1 in its own layout (the demo file table 0x8004BD7C)
    Sub sub_ = Sub::kNone;
    int delay_ = 0;                    // the 20 fields of 0x8004B2CC / 0x8004B320 / 0x8004B278 / 0x8004C4A0 (Simulation addresses)
    bool leaving_ = false;
    std::unique_ptr<gt2::shell::ReplayRowText> replayText_;
    std::unique_ptr<gt2::ReplayCardFile> demoFile_;
    std::unique_ptr<gt2::shell::ReplayTheaterMenu> theater_, transferMenu_;
    std::unique_ptr<gt2::shell::DemonstrationScreen> demo_;
    std::unique_ptr<gt2::shell::CopyReplayScreen> copy_;
    std::unique_ptr<gt2::shell::TransferManager> transferCard_;
    std::unique_ptr<gt2::shell::TradeScreen> trade_;
    std::unique_ptr<gt2::CarInfoDirectory> carInfo_;
    std::optional<gt2::career::CareerState> tradeSource_;
    std::optional<gt2::ReplayPayload> pendingReplay_;
    std::string pendingTitle_;
};

} // namespace gt2game
