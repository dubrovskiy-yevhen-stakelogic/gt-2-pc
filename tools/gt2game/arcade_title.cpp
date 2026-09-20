// The arcade title's Options / Save Game / Load Game and the first-boot auto-load (arcade_title.h).
#include "arcade_title.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <stdexcept>

#include "game/audio/audio_device.h"
#include "game/audio/mixer.h"
#include "game/audio/music_player.h"
#include "game/shell/title_attract.h"
#include "game/shell/title_copy.h"
#include "game/shell/title_draw.h"
#include "game/shell/title_menu.h"
#include "game/shell/title_replay.h"
#include "game/shell/title_transfer.h"
#include "game_window.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/race_record_screens.h"
#include "platform/input/ps1_pad.h"

using namespace gt2;

namespace gt2game {

// The title views' CD music (as title_mode.cpp's TitleMusic): the arcade EXE's music table, the volume = career + 0xB3 at the
// start of a track (0x80080E34 = Simulation 0x80080F24).
struct ArcadeTitleScreens::Music {
    audio::Mixer mixer;
    audio::AudioDevice device;
    std::unique_ptr<audio::MusicPlayer> player;
    ~Music() { device.Close(); }
};

ArcadeTitleScreens::ArcadeTitleScreens(const TitleAssets& arcadeTitle, const GtfsVolume& vol, const ArcadeModeOptions& options, std::span<const uint8_t> career)
    : assets_(arcadeTitle.SimLayoutScreens()), discPath_(options.discPath), noSound_(options.noSound), nativeExe_(arcadeTitle.exe), vol_(vol), nativeOvl1_(arcadeTitle.ovl1) {
    SetCareer(career);
    if (options.view.pcSettings) settings_ = *options.view.pcSettings;
    slots_ = {shell::CardSlot{options.card1Path}, shell::CardSlot{options.card2Path}};
    SyncSettings();
}

ArcadeTitleScreens::~ArcadeTitleScreens() = default;

void ArcadeTitleScreens::SetCareer(std::span<const uint8_t> career) {
    if (career.size() < sizeof(career_)) throw std::invalid_argument("arcade title: the career block is shorter than 0x7C9C bytes");
    std::memcpy(&career_, career.data(), sizeof(career_));
    SyncSettings();
}

void ArcadeTitleScreens::SyncSettings() {
    settings_.options = shell::ReadGameOptions(career_);
    settings_.haveCareerOptions = true;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(&career_);
    std::copy(b + 0x0A, b + 0x0A + 0x52, settings_.padBlocks[0].begin()); // the pad blocks (key tables, vibration, calibration)
    std::copy(b + 0x5C, b + 0x5C + 0x52, settings_.padBlocks[1].begin());
    settings_.havePadBlocks = true;
}

ArcadeModeOptions ArcadeTitleScreens::RaceOptions(const ArcadeModeOptions& base) const {
    ArcadeModeOptions o = base;
    o.view.pcSettings = &settings_;
    o.view.metric = settings_.metric || (base.view.metric && !(base.view.pcSettings && base.view.pcSettings->metric)); // PC SETTINGS or --kmh
    o.view.padBlock.assign(settings_.padBlocks[0].begin(), settings_.padBlocks[0].end());
    o.view.triggerPedals = settings_.triggerPedals;
    return o;
}

void ArcadeTitleScreens::PlayMusic(int track) {
    if (noSound_) return;
    if (!music_) {
        music_ = std::make_unique<Music>();
        try {
            music_->player = std::make_unique<audio::MusicPlayer>();
            music_->player->Open(discPath_, nativeExe_);
            music_->mixer.SetStream(music_->player.get());
            std::string error;
            if (!music_->device.Open(music_->mixer, error)) {
                std::printf("arcade title: no sound device for the music (%s)\n", error.c_str());
                music_->player.reset();
            }
        } catch (const std::exception& e) {
            std::printf("arcade title: music disabled (%s)\n", e.what());
            music_->player.reset();
        }
    }
    if (!music_->player) return;
    music_->player->SetVolume(uint8_t(shell::OptionValue(career_, shell::kMusicVolume)));
    music_->player->Play(track, true);
}

void ArcadeTitleScreens::StopMusic() {
    if (music_ && music_->player) music_->player->Stop();
}

bool ArcadeTitleScreens::StartBoot() {
    boot_ = std::make_unique<shell::TitleBootLoad>(assets_, slots_[0]);
    if (!boot_->Active()) boot_.reset();
    return boot_ != nullptr;
}

bool ArcadeTitleScreens::Open(int titleResult) {
    switch (titleResult) {
    case shell::kReplayTheater: // 0x800115E8 (Simulation 0x8001167C): 0x801EF022 = 2, the theater's views
        if (theater_) theater_->Start(false);
        EnterTheater(true);
        return true;
    case shell::kDataTransfer: // view 0x8004B974 (Simulation 0x8004C4A0: 20 fields, music 7) -> "DATA TRANSFER"
        if (!transferMenu_) transferMenu_ = std::make_unique<shell::ReplayTheaterMenu>(assets_, shell::ReplayTheaterMenu::TransferLayout());
        transferMenu_->Start(false);
        PlayMusic(7);
        delay_ = 0x14;
        leaving_ = false;
        sub_ = Sub::kTransferEnter;
        return true;
    case shell::kOptions: // view 0x8004B4A8 -> 0x8004B550 (Simulation 0x8004BFD4 -> 0x8004C07C), music 6
        options_ = std::make_unique<shell::OptionsScreen>(assets_, career_, &settings_);
        PlayMusic(6);
        return true;
    case shell::kSaveGame: // views 0x8004AA38 -> 0x8004AA8C: EXE 0x8007275C + 0x80072EAC(0) (mode 4), music 7
    case shell::kLoadGame: // views 0x8004AAE0 -> 0x8004AB34: EXE 0x8007275C + 0x80072F20(0) (mode 5), music 7
        card_ = std::make_unique<shell::CardManager>(assets_, titleResult == shell::kSaveGame ? shell::CardManager::kSaveGame : shell::CardManager::kLoadGame,
                                                     career_, slots_);
        PlayMusic(7);
        return true;
    default:
        return false;
    }
}

void ArcadeTitleScreens::EnterTheater(bool fresh) { // 0x800115E8: the 20-field view (music 0; the demo file), then the menu
    if (!replayText_) {
        replayText_ = std::make_unique<shell::ReplayRowText>(shell::ReplayRowText::Load(vol_, assets_));
        // The demo file of the language: member 1's table 0x8004BD7C (Simulation 0x8004C8A8) through the arcade boot's file table.
        const std::string demoPath = shell::AttractDemoFilePath(nativeOvl1_, vol_, assets_.language);
        demoFile_ = std::make_unique<ReplayCardFile>(ReplayCardFile::FromBytes(vol_.Read(demoPath)));
        theater_ = std::make_unique<shell::ReplayTheaterMenu>(assets_);
        std::printf("arcade replay theater: demo file %s (%d replays)\n", demoPath.c_str(), demoFile_->Count());
    }
    if (fresh) PlayMusic(0);
    delay_ = 0x14;
    leaving_ = false;
    sub_ = Sub::kTheaterEnter;
}

bool ArcadeTitleScreens::TakeReplay(ReplayPayload& payload, std::string& title) {
    if (sub_ != Sub::kReplayReady || !pendingReplay_) return false;
    payload = *pendingReplay_;
    title = pendingTitle_;
    pendingReplay_.reset();
    return true;
}

void ArcadeTitleScreens::ReplayPlayed() { // back in member 1 with 0x801EF022 == 2: the theater from its start
    if (!theater_) return;
    theater_->Start(false);
    EnterTheater(true);
}

// The theater's and DATA TRANSFER's views: title_mode.cpp's flow of the Simulation title on the arcade assets (the arcade
// member 1 runs the Simulation code of these views shifted, docs/formats/title.md section 12).
bool ArcadeTitleScreens::UpdateSub(const MenuListPad& pad, GameWindow& window) {
    const int field = window.Field();
    auto toTheater = [&](bool back) {
        theater_->Start(back);
        sub_ = Sub::kTheater;
    };
    switch (sub_) {
    case Sub::kNone:
        return false;
    case Sub::kReplayReady: // the caller did not take it: back to the menu
        pendingReplay_.reset();
        toTheater(true);
        return true;
    case Sub::kTheaterEnter: // 20 empty fields, then the menu (or, when leaving, the title)
        if (--delay_ <= 0) {
            if (leaving_) {
                StopMusic();
                sub_ = Sub::kNone;
                return false;
            }
            sub_ = Sub::kTheater;
        }
        return true;
    case Sub::kTheater: {
        const int r = theater_->Update(&pad);
        sounds = theater_->sounds;
        if (r == 2) { // back: 20 fields -> the title
            delay_ = 0x14;
            leaving_ = true;
            sub_ = Sub::kTheaterEnter;
        } else if (r == 1) {
            std::printf("arcade replay theater f%d: row %d\n", field, theater_->choice);
            switch (theater_->choice) {
            case shell::ReplayTheaterMenu::kLoadReplay: // the EXE card manager in mode 1
                card_ = std::make_unique<shell::CardManager>(assets_, shell::CardManager::kLoadReplay, career_, slots_);
                card_->SetReplayText(replayText_.get());
                sub_ = Sub::kCard;
                break;
            case shell::ReplayTheaterMenu::kRenameDelete: // mode 2
                card_ = std::make_unique<shell::CardManager>(assets_, shell::CardManager::kRenameReplay, career_, slots_);
                card_->SetReplayText(replayText_.get());
                card_->SetKeyboard(gt2::screens::MakeTitleCardKeyboard(assets_));
                sub_ = Sub::kCard;
                break;
            case shell::ReplayTheaterMenu::kDemonstration:
                demo_ = std::make_unique<shell::DemonstrationScreen>(assets_, *replayText_, *demoFile_);
                sub_ = Sub::kDemo;
                break;
            default: // Copy Replay (card 1 -> card 2)
                copy_ = std::make_unique<shell::CopyReplayScreen>(assets_, *replayText_, slots_);
                copy_->arcadeMessageContext = true; // member 1 0x8001503C: no 0x8006AC68 before the message (title_copy.h)
                sub_ = Sub::kCopy;
                break;
            }
        }
        return true;
    }
    case Sub::kCard: {
        const bool running = card_->Update(&pad);
        sounds = card_->sounds;
        for (const std::string& line : card_->log) std::printf("arcade card f%d: %s\n", field, line.c_str());
        card_->log.clear();
        if (running) return true;
        if (card_->CardMode() == shell::CardManager::kLoadReplay && card_->Loaded() && card_->LoadedReplay()) { // loaded -> 20 fields
            pendingReplay_ = card_->LoadedReplay();
            pendingTitle_ = card_->LoadedReplayTitle();
            sounds.push_back(3);
            StopMusic();
            delay_ = 0x14;
            sub_ = Sub::kTheaterLeave;
        } else { // the manager's exit -> sound 4, back to the menu
            sounds.push_back(4);
            toTheater(true);
        }
        card_.reset();
        return true;
    }
    case Sub::kCopy: {
        const bool running = copy_->Update(&pad);
        sounds = copy_->sounds;
        for (const std::string& line : copy_->log) std::printf("arcade copy replay f%d: %s\n", field, line.c_str());
        copy_->log.clear();
        if (!running) {
            sounds.push_back(4);
            copy_.reset();
            toTheater(true);
        }
        return true;
    }
    case Sub::kDemo: {
        const int r = demo_->Update(&pad);
        sounds = demo_->sounds;
        if (r == 2) {
            demo_.reset();
            toTheater(true);
        } else if (r == 1) { // 20 fields, then 0x80020A98 (Simulation 0x80020E14) gathers and unpacks the chosen replay
            try {
                std::vector<uint8_t> data = demoFile_->EntryData(demo_->choice);
                data.resize(size_t(demoFile_->Entry(demo_->choice).size));
                pendingReplay_ = UnpackReplayPayload(data);
                pendingTitle_ = demoFile_->Entry(demo_->choice).Title();
            } catch (const std::exception& e) {
                std::printf("arcade replay theater: demo %d: %s\n", demo_->choice, e.what());
                pendingReplay_.reset();
            }
            demo_.reset();
            delay_ = 0x14;
            sub_ = Sub::kTheaterLeave;
        }
        return true;
    }
    case Sub::kTheaterLeave: // 20 fields, then the race overlay plays the replay (TakeReplay)
        if (--delay_ <= 0) {
            if (pendingReplay_) sub_ = Sub::kReplayReady;
            else toTheater(true);
        }
        return true;
    case Sub::kTransferEnter: // 20 empty fields, then the list (or, when leaving, the title)
        if (--delay_ <= 0) {
            if (leaving_) {
                StopMusic();
                sub_ = Sub::kNone;
                return false;
            }
            sub_ = Sub::kTransfer;
        }
        return true;
    case Sub::kTransfer: {
        const int r = transferMenu_->Update(&pad);
        sounds = transferMenu_->sounds;
        if (r == 2) {
            delay_ = 0x14;
            leaving_ = true;
            sub_ = Sub::kTransferEnter;
        } else if (r == 1) { // the manager in mode = the row (TRADE / MIX RECORDS / CONVERT)
            std::printf("arcade data transfer f%d: row %d\n", field, transferMenu_->choice);
            transferCard_ = std::make_unique<shell::TransferManager>(assets_, shell::TransferManager::Mode(transferMenu_->choice), slots_);
            sub_ = Sub::kTransferCard;
        }
        return true;
    }
    case Sub::kTransferCard: {
        const int r = transferCard_->Update(&pad);
        sounds = transferCard_->sounds;
        for (const std::string& line : transferCard_->log) std::printf("arcade data transfer f%d: %s\n", field, line.c_str());
        transferCard_->log.clear();
        if (r == 1) { // left: sound 4, back to the list
            sounds.push_back(4);
            transferCard_.reset();
            transferMenu_->Start(true);
            sub_ = Sub::kTransfer;
        } else if (r == 2) {
            sounds.push_back(3);
            const shell::TransferManager::Mode mode = transferCard_->TransferMode();
            if (mode == shell::TransferManager::kTrade) { // the loaded save's garage
                if (!carInfo_) carInfo_ = std::make_unique<CarInfoDirectory>(CarInfoDirectory::Load(vol_));
                tradeSource_ = *transferCard_->LoadedSave();
                trade_ = std::make_unique<shell::TradeScreen>(assets_, *carInfo_, career_, *tradeSource_);
                sub_ = Sub::kTrade;
            } else if (mode == shell::TransferManager::kMix) {
                shell::MixRecords(career_, *transferCard_->LoadedSave());
                std::printf("arcade data transfer: records of the loaded save mixed into the career\n");
            } else {
                shell::ConvertGt1Licences(career_, transferCard_->Gt1Data());
                std::printf("arcade data transfer: Gran Turismo licences converted\n");
            }
        }
        return true;
    }
    case Sub::kTrade: {
        const int bought = trade_->bought;
        const int r = trade_->Update(&pad);
        sounds = trade_->sounds;
        if (trade_->bought != bought) std::printf("arcade data transfer: car bought (money %d, %d car(s))\n", career_.garage.money, career_.garage.count);
        if (r == 2) { // back to the manager view (a new manager in mode 0)
            trade_.reset();
            transferCard_ = std::make_unique<shell::TransferManager>(assets_, shell::TransferManager::kTrade, slots_);
            sub_ = Sub::kTransferCard;
        }
        return true;
    }
    }
    return false;
}

bool ArcadeTitleScreens::Update(const MenuListPad& pad, GameWindow& window) {
    sounds.clear();
    if (sub_ != Sub::kNone) return UpdateSub(pad, window);
    if (boot_) {
        const bool running = boot_->Update(pad, career_);
        for (const std::string& line : boot_->log) std::printf("arcade title: %s\n", line.c_str());
        boot_->log.clear();
        if (boot_->Loaded()) SyncSettings();
        if (!running) boot_.reset();
        return running;
    }
    if (options_) {
        // The ports' controllers for the KEY CONFIGURATION page (as title_mode.cpp): port 1 = the controller in use (the
        // keyboard alone acts as a digital pad), port 2 = none.
        options_->SetPadTypes(window.Pad().type != input::kTypeNone ? window.Pad().type : uint8_t(input::kTypeDigital), input::kTypeNone);
        options_->SetAnalogRaw(0, window.Pad().analog);
        const bool running = options_->Update(&pad, nullptr);
        sounds = options_->sounds;
        for (const std::string& note : options_->notes) std::printf("arcade options: %s\n", note.c_str());
        options_->notes.clear();
        if (!running) {
            StopMusic();
            SyncSettings();
            options_.reset();
        }
        return running;
    }
    if (card_) {
        const bool running = card_->Update(&pad);
        sounds = card_->sounds;
        for (const std::string& line : card_->log) std::printf("arcade card f%d: %s\n", window.Field(), line.c_str());
        card_->log.clear();
        if (!running) {
            if (card_->Loaded()) SyncSettings(); // 0x8006A188 (= Simulation 0x8006A278): the loaded block is the career
            StopMusic();
            card_.reset();
        }
        return running;
    }
    return false;
}

std::vector<MenuPrim> ArcadeTitleScreens::Frame() const {
    switch (sub_) {
    case Sub::kNone: break;
    case Sub::kTheater: return theater_->Frame();
    case Sub::kCard: return card_ ? card_->Frame() : shell::TitleFrameStart();
    case Sub::kCopy: return copy_ ? copy_->Frame() : shell::TitleFrameStart();
    case Sub::kDemo: return demo_ ? demo_->Frame() : shell::TitleFrameStart();
    case Sub::kTransfer: return transferMenu_->Frame();
    case Sub::kTransferCard: return transferCard_ ? transferCard_->Frame() : shell::TitleFrameStart();
    case Sub::kTrade: return trade_ ? trade_->Frame() : shell::TitleFrameStart();
    default: return shell::TitleFrameStart(); // the empty 20-field views
    }
    if (boot_) return boot_->Frame();
    if (options_) return options_->Frame();
    if (card_) return card_->Frame();
    return {};
}

} // namespace gt2game
