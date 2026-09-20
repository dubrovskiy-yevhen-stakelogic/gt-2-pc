// gt2game's title (src/game/shell, gt2view/title_view). See title_mode.h.
#include "title_mode.h"
#include "boot_screen.h"

#include "platform/os/keys.h"
#include "platform/os/paths.h"


#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <thread>

#include "game/audio/audio_device.h"
#include "game/audio/menu_audio.h"
#include "game/audio/mixer.h"
#include "game/audio/music_player.h"
#include "game/career/career_state.h"
#include "game/shell/title_copy.h"
#include "game/shell/title_draw.h"
#include "game/shell/title_menu.h"
#include "game/shell/title_options.h"
#include "game/shell/title_replay.h"
#include "game/shell/title_screens.h"
#include "game/shell/title_transfer.h"
#include "gt2export/png_writer.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/png_reader.h"
#include "gt2formats/replay_card.h"
#include "gt2formats/title_assets.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "game_window.h"
#include "graphics_options.h"
#include "race_common.h"
#include "race_view.h"
#include "split_race.h"
#include "title_attract.h"
#include "ghost_replay.h"
#include "gt2view/race_record_screens.h"
#include "gt2view/title_view.h"
#include "gt2view/vk_scene_renderer.h"

using namespace gt2;
namespace pad = gt2::menu_list_pad;
namespace input = gt2::input;

namespace {

uint32_t TitleKeyBit(int key) {
    switch (key) {
    case gt2::keys::kUp: return pad::kUp;
    case gt2::keys::kDown: return pad::kDown;
    case gt2::keys::kLeft: return pad::kLeft;
    case gt2::keys::kRight: return pad::kRight;
    case gt2::keys::kReturn: return pad::kCross;
    case gt2::keys::kSpace: return pad::kCircle;
    case gt2::keys::kBack:
    case gt2::keys::kEscape: return pad::kTriangle;
    case 'S': return pad::kStart;
    case 'Q': return pad::kL1;
    case 'W': return pad::kR1;
    default: return 0;
    }
}

// The keyboard (and the global --script) through the shared game window (game_window.h).
uint32_t TitleHeldKeys(const gt2game::GameWindow& window) {
    uint32_t bits = 0;
    for (int k : {gt2::keys::kUp, gt2::keys::kDown, gt2::keys::kLeft, gt2::keys::kRight, gt2::keys::kReturn, gt2::keys::kSpace, gt2::keys::kBack, gt2::keys::kEscape, int('S'), int('Q'), int('W')})
        if (window.Held(k)) bits |= TitleKeyBit(int(k));
    return bits;
}

struct ScriptPress { int field = 0, hold = 6; uint32_t bits = 0; };
std::vector<ScriptPress> ParseTitleScript(const std::string& s) {
    static const std::map<std::string, uint32_t> kButtons = {{"up", pad::kUp},       {"down", pad::kDown},     {"left", pad::kLeft},     {"right", pad::kRight},
                                                             {"cross", pad::kCross}, {"circle", pad::kCircle}, {"square", pad::kSquare}, {"triangle", pad::kTriangle},
                                                             {"start", pad::kStart}, {"l1", pad::kL1},         {"r1", pad::kR1}};
    std::vector<ScriptPress> out;
    size_t at = 0;
    while (at < s.size()) {
        size_t end = s.find(',', at);
        if (end == std::string::npos) end = s.size();
        const std::string item = s.substr(at, end - at);
        const size_t c1 = item.find(':');
        if (c1 == std::string::npos) throw std::runtime_error("bad --title-script item " + item);
        const size_t c2 = item.find(':', c1 + 1);
        ScriptPress p;
        p.field = std::atoi(item.substr(0, c1).c_str());
        const auto b = kButtons.find(item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos : c2 - c1 - 1));
        if (b == kButtons.end()) throw std::runtime_error("bad --title-script button in " + item);
        p.bits = b->second;
        if (c2 != std::string::npos) p.hold = std::max(1, std::atoi(item.substr(c2 + 1).c_str()));
        out.push_back(p);
        at = end + 1;
    }
    return out;
}

// A frame (PNG of ours) against a gt2play --prims VRAM dump (1024 x 512 words): the 352 x 480 drawing area at (0, 0),
// 5-bit channels; writes ours | original | differences next to the shot.
size_t CompareWithCapture(const std::vector<uint8_t>& oursRgba, int w, int h, const std::string& capture, const std::string& sidePath) {
    std::FILE* f = std::fopen(capture.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + capture);
    std::vector<uint16_t> vram(1024 * 512);
    const size_t n = std::fread(vram.data(), 2, vram.size(), f);
    std::fclose(f);
    if (n != vram.size()) throw std::runtime_error(capture + ": not a 1024 x 512 VRAM dump");
    const int W = TitleAssets::kScreenWidth, H = TitleAssets::kScreenHeight;
    if (w < W || h < H) throw std::runtime_error("compare: the shot is smaller than 352 x 480");
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    size_t diff = 0;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t* o = &oursRgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            const uint16_t c = vram[size_t(y) * 1024 + size_t(x)];
            const int cr = c & 31, cg = (c >> 5) & 31, cb = (c >> 10) & 31;
            const bool d = (o[0] >> 3) != cr || (o[1] >> 3) != cg || (o[2] >> 3) != cb;
            if (d) {
                diff++;
                minX = std::min(minX, x), minY = std::min(minY, y), maxX = std::max(maxX, x), maxY = std::max(maxY, y);
            }
            uint8_t* a = &side[(size_t(y) * W * 3 + size_t(x)) * 4];
            std::memcpy(a, o, 3);
            uint8_t* b = &side[(size_t(y) * W * 3 + size_t(W + x)) * 4];
            b[0] = uint8_t(cr << 3), b[1] = uint8_t(cg << 3), b[2] = uint8_t(cb << 3);
            uint8_t* e = &side[(size_t(y) * W * 3 + size_t(2 * W + x)) * 4];
            const uint8_t grey = uint8_t((b[0] + b[1] + b[2]) / 12);
            e[0] = d ? 255 : grey, e[1] = d ? 0 : grey, e[2] = d ? 0 : grey;
        }
    std::printf("compare vs %s: %zu differing pixels", capture.c_str(), diff);
    if (diff) std::printf(" (box %d,%d - %d,%d)", minX, minY, maxX, maxY);
    std::printf(" -> %s\n", sidePath.c_str());
    WritePngRgba(sidePath, W * 3, H, side);
    return diff;
}

// Dev aid for comparisons with gt2play captures: the frame on the software canvas with the interpreter GPU's SPRITE
// rules (src/machine/gpu.cpp DrawRectangle / PlotPixel: texel << 3, * colour / 128, blend in 8 bits against the
// background << 3, >> 3), which differ from the PS1 rules of MenuCanvas::Sprite (5-bit modulation and blending) for
// dark modulated sprites under subtractive blending (the title's unselected rows). Everything else goes through
// MenuCanvas with its kInterpreter rules.
MenuCanvas RenderWithInterpreterSprites(const TitleAssets& assets, const std::vector<MenuPrim>& prims) {
    MenuCanvas canvas;
    canvas.rules = MenuCanvas::Rules::kInterpreter;
    for (const MenuPrim& p : prims) {
        if (p.kind != MenuPrim::kSprite) {
            canvas.Draw(assets.vram, p);
            continue;
        }
        const int mr = int(p.colour[0] & 0xFF), mg = int((p.colour[0] >> 8) & 0xFF), mb = int((p.colour[0] >> 16) & 0xFF);
        const int mode = (p.tpage >> 5) & 3;
        for (int j = 0; j < p.h; j++)
            for (int i = 0; i < p.w; i++) {
                const int x = p.x[0] + i, y = p.y[0] + j;
                if (x < 0 || y < 0 || x >= MenuCanvas::kWidth || y >= MenuCanvas::kHeight) continue;
                const uint16_t t = assets.vram.Sample(p.tpage, p.clut, uint8_t(p.u + i), uint8_t(p.v + j));
                if (t == 0) continue;
                int c[3] = {((t & 31) << 3) * mr / 128, (((t >> 5) & 31) << 3) * mg / 128, (((t >> 10) & 31) << 3) * mb / 128};
                if (p.semi && (t & 0x8000)) {
                    const uint16_t d = canvas.At(x, y);
                    const int back[3] = {(d & 31) << 3, ((d >> 5) & 31) << 3, ((d >> 10) & 31) << 3};
                    for (int k = 0; k < 3; k++) {
                        switch (mode) {
                        case 0: c[k] = (back[k] + c[k]) / 2; break;
                        case 1: c[k] = back[k] + c[k]; break;
                        case 2: c[k] = back[k] - c[k]; break;
                        default: c[k] = back[k] + c[k] / 4; break;
                        }
                    }
                }
                for (int& v : c) v = (std::clamp(v, 0, 255) >> 3) << 3;
                canvas.Fill(x, y, 1, 1, uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2]));
            }
    }
    return canvas;
}

// The title music of each screen (0x80012414 -> 0x80080F24(track, loop 1)): options 6, save / load 7, replay theater 0;
// the title itself is silent (0x80012434 stops the track when a view ends). The volume is the career's +0xB3 at the
// start of a track, as 0x80080F24 reads it.
class TitleMusic {
public:
    bool Open(const std::string& discPath, const GuestImage& exe) {
        try {
            player_ = std::make_unique<audio::MusicPlayer>();
            player_->Open(discPath, exe);
            mixer_.SetStream(player_.get());
            std::string error;
            if (!device_.Open(mixer_, error)) {
                std::printf("title: no sound device (%s)\n", error.c_str());
                player_.reset();
                return false;
            }
            return true;
        } catch (const std::exception& e) {
            std::printf("title: music disabled (%s)\n", e.what());
            player_.reset();
            return false;
        }
    }
    void Play(int track, uint8_t volume) {
        if (!player_) return;
        player_->SetVolume(volume);
        player_->Play(track, true);
    }
    void Stop() {
        if (player_) player_->Stop();
    }
    ~TitleMusic() { device_.Close(); }

private:
    audio::Mixer mixer_;
    audio::AudioDevice device_;
    std::unique_ptr<audio::MusicPlayer> player_;
};

// kTheaterEnter / kTheaterLeave = the replay theater's empty 20-field views (0x8004B2CC entering / leaving, 0x8004B320 /
// 0x8004B278 before a replay plays).
// kTransferEnter = DATA TRANSFER's 20-field view 0x8004C4A0 (entering / leaving), kTransfer the list 0x8004C548, kTransferCard
// member 1's card manager (TRADE / MIX RECORDS / CONVERT), kTrade the TRADE list 0x8004C5F0.
enum class Screen { kBootLoad, kTitle, kOptions, kCard, kTheaterEnter, kTheater, kDemo, kTheaterLeave, kTransferEnter, kTransfer, kTransferCard, kTrade, kCopy };

// The first-boot view of member 1 (0x8004B900: 0x80016BD8 / 0x80016C28 / 0x80016F6C): loads "BASCUS-94455GAME" from
// memory card 1 when it holds one ("Loading Save Data..." + progress, then "Auto Loading Complete" for 180 fields or
// until a face button; "Save Data is Corrupt!" on a CRC mismatch, "Auto Loading Failed" on a read error, 240 fields).
struct TitleBoot {
    int state = 0, timer = 0, remaining = 0;
    uint32_t colour = 0x604610;
    std::string line;
    std::array<int8_t, 32> segments{};
    bool progress = false;
};

} // namespace

bool WriteReplayOut(const std::string& path, const ReplayFile& replay, const sim::CarParams* params, const DiscImage& disc) {
    try {
        const GuestImage exe = LoadExeImage(disc);
        std::span<const uint8_t> record;
        if (params) record = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(params), sizeof(sim::CarParams));
        const ReplayPayload payload = PayloadOfReplay(replay, record, {});
        const std::vector<uint8_t> packed = PackReplayPayload(payload);
        std::string lower = path;
        for (char& c : lower) c = char(std::tolower(uint8_t(c)));
        if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".mcd") {
            std::vector<uint8_t> card = std::filesystem::exists(path) ? career::ReadFileBytes(path) : career::FormatMemoryCard();
            std::vector<uint8_t> existing = ReadReplayCardFile(card);
            ReplayCardFile file = existing.size() >= kReplayDataStart ? ReplayCardFile::FromBytes(existing) : ReplayCardFile::Create(3, BuildReplayCardHeader(exe, 3));
            if (!file.Valid()) throw std::runtime_error("the card's replay file is not valid (0x800691DC)");
            const int fits = file.Fits(-1, int32_t(packed.size()));
            if (fits != 0) throw std::runtime_error(fits == 1 ? "the replay file holds 32 replays" : "not enough free sectors in the replay file");
            if (existing.empty() && CardImageFreeBlocks(card) < file.Blocks()) throw std::runtime_error("not enough free blocks on the card");
            file.Store(-1, ReplayDescription(payload), packed);
            StoreReplayCardFile(card, file.Bytes());
            career::WriteFileBytes(path, card);
            std::printf("replay: added as replay %d of %s (%d of %d sectors free)\n", file.Count() - 1, path.c_str(), file.FreeSectors(), file.Total());
            return true;
        }
        const std::vector<uint8_t> bytes = WriteReplayFile(replay, BuildReplayCardHeader(exe, 3));
        career::WriteFileBytes(path, bytes);
        std::printf("replay: written to %s (%zu bytes, %zu payload bytes)\n", path.c_str(), bytes.size(), packed.size());
        return true;
    } catch (const std::exception& e) {
        std::printf("replay: cannot write %s (%s)\n", path.c_str(), e.what());
        return false;
    }
}

std::filesystem::path ExecutableDirectory() { return gt2::os::ExecutableDir(); }

int RunTitleMode(const DiscImage& disc, const GtfsVolume& vol, const TitleModeOptions& options, const TitleGtModeHook& gtMode) {
    const TitleAssets assets = TitleAssets::Load(disc, vol);
    const input::PadTables padTables = input::PadTables::Load(assets.exe); // the controller's buttons as the menus' generic bits
    // The career: a save given on the command line, else the new game of 0x800104A0 (boot); the first-boot view may
    // replace it with the save on card 1.
    career::CareerSave save;
    if (!options.careerPath.empty()) {
        save = career::LoadCareer(options.careerPath);
        std::printf("title: career %s (CRC %s)\n", options.careerPath.c_str(), save.CrcOk() ? "ok" : "MISMATCH");
    } else {
        save.state = career::NewCareer(career::ReadNewGameDefaults(disc));
    }
    save.header = career::BuildSaveHeader(assets.exe);
    std::array<shell::CardSlot, 2> slots{shell::CardSlot{options.card1Path}, shell::CardSlot{options.card2Path}};
    shell::PcSettings settings = shell::PcSettings::Load(options.settingsPath);
    auto storeSettings = [&] {
        gt2game::SetGraphicsFromSettings(settings.graphics); // the PC SETTINGS page's graphics for the next race (unless the command line pinned them)
        if (options.settingsPath.empty()) return;
        settings.options = shell::ReadGameOptions(save.state);
        settings.haveCareerOptions = true;
        { // the pad blocks (key tables, vibration, calibration) for races without a career
            const uint8_t* b = reinterpret_cast<const uint8_t*>(&save.state);
            std::copy(b + 0x0A, b + 0x0A + 0x52, settings.padBlocks[0].begin());
            std::copy(b + 0x5C, b + 0x5C + 0x52, settings.padBlocks[1].begin());
            settings.havePadBlocks = true;
        }
        try {
            const std::filesystem::path p = options.settingsPath;
            if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
            settings.Save(options.settingsPath);
        } catch (const std::exception& e) {
            std::printf("title: cannot write %s (%s)\n", options.settingsPath.c_str(), e.what());
        }
    };

    // One window for the title, the GT-mode menus and the races (game_window.h): Start Game runs the menus in it.
    gt2game::GameWindow window("gt2game", options.windowWidth, options.windowHeight);
    window.SetPacing(options.pacing);
    if (!options.globalScript.empty()) window.AddScript(options.globalScript);
    for (const auto& s : options.anyShots) window.AddShot(s.first, s.second);
    const bool automatedBoot = !options.script.empty() || !options.globalScript.empty() ||
        !options.shots.empty() || !options.anyShots.empty() || !options.compare.empty() || options.quitAfter > 0;
    if (!options.noMovies && (options.forceMovies || !automatedBoot) &&
        !gt2game::PlayBootScreens(window, disc)) return 0;
    gt2view::VkSceneRenderer* renderer = &window.Renderer();
    renderer->clearColor[0] = renderer->clearColor[1] = renderer->clearColor[2] = 0.0f;
    auto view = std::make_unique<gt2view::TitleView>(*renderer);
    const MenuCanvas::Rules rules = options.compare.empty() ? MenuCanvas::Rules::kPs1 : MenuCanvas::Rules::kInterpreter;
    view->SetRasterRules(rules);
    view->UploadVram(assets.vram);

    // Sound: the title music and the menu sound effects (EXE 0x80060840, the same routine in the title:
    // game/audio/menu_audio.h). Closed while the GT mode runs (it opens its own output) and opened again afterwards.
    std::unique_ptr<TitleMusic> music = std::make_unique<TitleMusic>();
    std::unique_ptr<audio::MenuAudio> sfx;
    auto openSound = [&] {
        music = std::make_unique<TitleMusic>();
        if (options.noSound) return;
        music->Open(options.discPath, assets.exe);
        try {
            sfx = std::make_unique<audio::MenuAudio>();
            sfx->Load(vol, assets.exe, LoadOverlayImage(disc, 4));
            std::string error;
            if (!sfx->OpenDevice(error)) {
                std::printf("title: no sound device for the effects (%s)\n", error.c_str());
                sfx.reset();
            }
        } catch (const std::exception& e) {
            std::printf("title: sound effects disabled (%s)\n", e.what());
            sfx.reset();
        }
    };
    openSound();
    auto playSounds = [&](const std::vector<int>& ids) {
        if (sfx)
            for (int id : ids) sfx->Sound(id);
    };

    shell::TitleMenu title(assets.ovl1, assets.language);
    std::unique_ptr<shell::OptionsScreen> optionsScreen;
    std::unique_ptr<shell::CardManager> card;
    std::unique_ptr<shell::CopyReplayScreen> copyScreen; // the theater's Copy Replay (title_copy.h)
    TitleBoot boot;
    Screen screen = Screen::kTitle;
    // The replay theater (title_replay.h): loaded when first entered.
    std::unique_ptr<shell::ReplayRowText> replayText;
    std::unique_ptr<ReplayCardFile> demoFile;
    std::unique_ptr<shell::ReplayTheaterMenu> theater;
    std::unique_ptr<shell::DemonstrationScreen> demo;
    std::unique_ptr<gt2game::TitleAttractDemos> attract; // the attract cycle (title result 6), kept across the title's restarts
    // DATA TRANSFER (title_transfer.h)
    std::unique_ptr<shell::ReplayTheaterMenu> transferMenu;
    std::unique_ptr<shell::TransferManager> transferCard;
    std::unique_ptr<shell::TradeScreen> trade;
    std::unique_ptr<CarInfoDirectory> carInfo;
    std::optional<career::CareerState> tradeSource; // the save the TRADE list sells from
    bool transferLeaving = false;
    std::optional<ReplayPayload> pendingReplay; // chosen / loaded, played after the 20-field view
    std::string pendingTitle;
    int theaterDelay = 0;                       // the 20 fields of 0x8004B2CC / 0x8004B320 / 0x8004B278
    bool theaterLeaving = false;                // 0x8004B2CC init(1): the theater was left, the title follows
    auto enterTheater = [&](bool fresh) { // 0x8001167C: views 0x8004B2CC (music 0; the demo file) -> 0x8004B374
        if (!replayText) {
            replayText = std::make_unique<shell::ReplayRowText>(shell::ReplayRowText::Load(vol, assets));
            const std::string demoPath = shell::DemoFilePath(assets.ovl1, assets.language);
            demoFile = std::make_unique<ReplayCardFile>(ReplayCardFile::FromBytes(vol.Read(demoPath)));
            theater = std::make_unique<shell::ReplayTheaterMenu>(assets);
            std::printf("replay theater: demo file %s (%d replays)\n", demoPath.c_str(), demoFile->Count());
        }
        if (fresh) music->Play(0, uint8_t(shell::OptionValue(save.state, shell::kMusicVolume)));
        theaterDelay = 0x14;
        theaterLeaving = false;
        screen = Screen::kTheaterEnter;
    };
    // The race overlay with argument 1 (0x80011384 after 0x80010EDC): the replay in this window; the theater afterwards.
    auto playReplay = [&](const ReplayPayload& payload, const std::string& title) {
        const uint8_t mode = payload.GameMode();
        if (!options.raceView || !options.raceOptions) {
            std::printf("replay theater: no race options, the replay cannot play\n");
            return;
        }
        if (mode == 6) { // a Time Trial / Rally record (0x80069948 mode 6 layout): player 1 on its lap ring (ghost_replay.h)
            music.reset();
            sfx.reset();
            {
                gt2game::RaceData data;
                gt2game::RaceOptions ro = *options.raceOptions;
                sim::GhostSession session;
                gt2game::LoadGhostRecordRace(disc, vol, payload, data, ro, session);
                gt2game::ApplyIntroHold(disc, data, ro);
                gt2game::RaceViewConfig v = *options.raceView;
                v.replay = nullptr;
                v.ghostReplay = true;
                v.replayEndLeaves = true;
                v.exitFade = true; // Start: the exit fade (title_attract.h)
                v.replayOut.clear();
                v.ghostOption = 0;
                std::printf("replay theater: playing '%s' (game mode 6 record, %zu lap(s))\n", title.c_str(), payload.ghosts.size());
                // The race overlay runs with argument 1 (0x800A9500): Start leaves the replay (no pause menu), as the theater's other replays.
                const gt2game::RaceViewResult r = gt2game::RunRaceView(window, nullptr, disc, vol, data, 1, ro, v, nullptr);
                std::printf("replay theater: replay left after %d steps\n", r.steps);
            }
            if (window.Closed()) return;
            view = std::make_unique<gt2view::TitleView>(*renderer);
            view->SetRasterRules(rules);
            view->UploadVram(assets.vram);
            renderer->clearColor[0] = renderer->clearColor[1] = renderer->clearColor[2] = 0.0f;
            window.SetTitle("gt2game");
            openSound();
            window.ResetPacing();
            theater->Start(false);
            enterTheater(true);
            return;
        }
        ReplayFile file;
        try {
            file = payload.ToReplayFile();
        } catch (const std::exception& e) {
            std::printf("replay theater: %s\n", e.what());
            return;
        }
        bool twoPlayer = false; // a player-2 slot (0x801D5946 = 4): the 2 player Battle's record (game mode 0, both streams)
        for (const ReplayEntry& e : file.cars) twoPlayer = twoPlayer || e.kind() == 4;
        music.reset(); // the race opens its own output
        sfx.reset();
        if (twoPlayer) { // the split screen with both players' streams (split_race.h); Start leaves (argument 1, no pause menu)
            gt2game::RaceData data;
            gt2game::RaceOptions ro = *options.raceOptions;
            gt2game::LoadReplayRace(disc, vol, file, data, ro);
            gt2game::ApplyIntroHold(disc, data, ro);
            gt2game::SplitRaceConfig split;
            split.view = *options.raceView;
            split.view.replayOut.clear();
            split.view.sponsorSeed = file.SponsorSeed(); // race block + 0x54
            split.view.sponsorSeedGiven = true;
            split.replay = &file;
            split.startLeaves = true;
            std::printf("replay theater: playing '%s' (%s, 2 player Battle, %zu car(s))\n", title.c_str(), file.CourseName().c_str(), file.cars.size());
            const gt2game::SplitRaceResult r = gt2game::RunSplitRace(window, nullptr, disc, vol, data, ro, split);
            std::printf("replay theater: replay left after %d steps\n", r.steps);
        } else {
            gt2game::RaceData data;
            gt2game::RaceOptions ro = *options.raceOptions;
            gt2game::LoadReplayRace(disc, vol, file, data, ro);
            gt2game::ApplyIntroHold(disc, data, ro);
            gt2game::RaceViewConfig v = *options.raceView;
            v.replay = &file;
            v.replayEndLeaves = true;
            v.exitFade = true; // Start: the exit fade (title_attract.h)
            v.replayOut.clear();
            v.sponsorSeed = file.SponsorSeed(); // race block + 0x54
            v.sponsorSeedGiven = true;
            if (!options.hudModeGiven && file.GameMode() == 3) v.hudMode = 3;
            std::printf("replay theater: playing '%s' (%s, game mode %u, %zu car(s))\n", title.c_str(), file.CourseName().c_str(), unsigned(mode), file.cars.size());
            // Argument 1 (0x800A9500, the title's replays): Start leaves the replay - no pause menu.
            const gt2game::RaceViewResult r = gt2game::RunRaceView(window, nullptr, disc, vol, data, data.params.size(), ro, v, nullptr);
            std::printf("replay theater: replay left after %d steps\n", r.steps);
        }
        if (window.Closed()) return;
        // Back in member 1 (0x801EF5F2 == 2): the title's VRAM and sound, then the theater from its start.
        view = std::make_unique<gt2view::TitleView>(*renderer);
        view->SetRasterRules(rules);
        view->UploadVram(assets.vram);
        renderer->clearColor[0] = renderer->clearColor[1] = renderer->clearColor[2] = 0.0f;
        window.SetTitle("gt2game");
        openSound();
        window.ResetPacing();
        theater->Start(false);
        enterTheater(true);
    };
    { // 0x80011384: the first-boot view runs while 0x801EF5F0 == 0 (our process start)
        std::vector<uint8_t> image;
        if (shell::CardStatus(slots[0], &image) == 0 && shell::CardFindFile(image, "BASCUS-94455GAME") >= 0 && options.careerPath.empty()) {
            screen = Screen::kBootLoad;
            boot.state = 3;
            boot.remaining = 0x7F00 + 0x80;
            boot.segments.fill(-1);
            boot.progress = true;
        }
    }

    const std::vector<ScriptPress> script = ParseTitleScript(options.script);
    int lastShot = 0;
    for (const auto& s : options.shots) lastShot = std::max(lastShot, s.first);
    uint32_t previousHeld = 0;
    int repeatTimer = 0;
    const auto fieldTime = std::chrono::nanoseconds(16'683'333);
    int field = 0;
    for (;;) {
        if (!window.BeginFrame()) break;
        field = window.Field();
        uint32_t held = TitleHeldKeys(window);
        held |= input::RemapButtons(window.Pad().buttons, padTables); // every pad button (L2 / R2 / Select / Square too: key configuration)
        uint32_t keyPresses = 0;
        for (int k : window.PressedKeys()) keyPresses |= TitleKeyBit(int(k));
        for (const ScriptPress& p : script)
            if (field >= p.field && field < p.field + p.hold) held |= p.bits;
        MenuListPad pad;
        pad.held = held;
        pad.pressed = (held & ~previousHeld) | keyPresses;
        pad.released = previousHeld & ~held;
        const uint32_t dirs = held & (pad::kUp | pad::kDown | pad::kLeft | pad::kRight);
        if (dirs && dirs == (previousHeld & dirs)) { // auto-repeat (ours: after 20 fields, every 5, as the GT-mode menus of gt2game)
            if (++repeatTimer >= 20 && (repeatTimer - 20) % 5 == 0) pad.repeat = dirs;
        } else {
            repeatTimer = 0;
        }
        previousHeld = held;

        std::vector<MenuPrim> prims;
        switch (screen) {
        case Screen::kBootLoad: { // 0x80016C28 states 3 (read) .. 10
            if (boot.state == 3) {
                boot.remaining = std::max(0, boot.remaining - 0x80 * 8);
                const uint32_t total = 0x7F80, done = total - uint32_t(boot.remaining), p = (done * done) / total;
                for (int i = 0; i < 32; i++) {
                    int8_t& s = boot.segments[size_t(i)];
                    if (s < 0) { if (((uint32_t(i) * total) >> 5) <= p) s = 0; }
                    else if (++s > 0x18) s = 0x18;
                }
                if (boot.remaining == 0) {
                    boot.progress = false;
                    try {
                        const career::CareerSave loaded = career::LoadCareerFromCard(career::ReadFileBytes(slots[0].path));
                        if (loaded.CrcOk()) {
                            save.state = loaded.state; // 0x8006A278
                            boot.state = 5, boot.timer = 0xB4, boot.line = assets.Text(0x801B9956u), boot.colour = 0x604610; // "Auto Loading Complete"
                            std::printf("title: auto-loaded %s (day %u, money %d, %d car(s))\n", slots[0].path.c_str(), save.state.record.days, save.state.garage.money,
                                        save.state.garage.count);
                        } else {
                            boot.state = 6, boot.timer = 0xF0, boot.line = assets.Text(0x801B998Du), boot.colour = 0x020A50F0; // "Save Data is Corrupt!"
                        }
                    } catch (const std::exception& e) {
                        std::printf("title: auto-load failed: %s\n", e.what());
                        boot.state = 8, boot.timer = 0xF0, boot.line = assets.Text(0x801B9971u), boot.colour = 0x020A50F0; // "Auto Loading Failed"
                    }
                }
            } else if (boot.state == 5 || boot.state == 6 || boot.state == 8) {
                boot.timer--;
                if (pad.pressed & 0xF00) boot.timer = 0;
                if (boot.timer <= 0) boot.state = 9;
            } else if (boot.state == 9) {
                boot.timer = 6, boot.state = 10;
            } else if (--boot.timer == 0) {
                screen = Screen::kTitle;
                title.Reset();
            }
            prims = shell::TitleFrameStart();
            MenuOtSlot ot;
            const HudFont& medium = assets.fonts[TitleAssets::kMediumFont];
            if (boot.state == 3) shell::AddText(ot, medium, assets.Text(0x801B9934u), 0xB0, 0xF0, 1, boot.colour, 1, shell::TextAlign::kCentre); // "Loading Save Data..."
            else if (boot.state == 5 || boot.state == 6 || boot.state == 8) shell::AddText(ot, medium, boot.line, 0xB0, 0xF0, 1, boot.colour, 1, shell::TextAlign::kCentre);
            if (boot.state >= 3 && boot.state <= 8) shell::AddText(ot, medium, assets.Text(0x801EFB39u), 0xB0, 0xB4, 1, boot.colour, 1, shell::TextAlign::kCentre);
            if (boot.progress) // 0x8006C174 with the object 0x8004B82C: (96, 320), 4 x 24, pitch 5, colour 0x90500C
                for (int i = 0; i < 32; i++) {
                    const int k = boot.segments[size_t(i)];
                    const uint32_t c = k < 0 ? MenuListLerp(0x90500C, 0, 0x60, 0x80) : MenuListLerp(0x90500C, 0xD4D4D4, 0x18 - k, 0x18);
                    MenuPrim t;
                    t.kind = MenuPrim::kTile;
                    t.x[0] = int16_t(0x60 + 5 * i), t.y[0] = 0x140, t.w = 4, t.h = 0x18;
                    t.colour[0] = c & 0xFFFFFF;
                    ot.Add(t);
                }
            ot.Emit(prims, 0x200);
            break;
        }
        case Screen::kTitle: {
            const int titleState = title.Update(&pad, held);
            playSounds(title.sounds);
            if (titleState == 4) {
                const int result = title.result;
                std::printf("title f%d: result %d\n", field, result);
                switch (result) {
                case shell::kStartGame: {
                    // 0x801EF5F1 = 4, 0x801EF5F2 = 3, 0x8005DA3C(4): the GT-mode menus in this window on this career (in
                    // memory: the title's Save Game writes what the GT mode did).
                    music.reset(); // the GT mode opens its own output
                    sfx.reset();
                    const bool ran = gtMode && gtMode(window, save);
                    if (!ran) std::printf("title: the GT-mode menus did not run\n");
                    if (window.Closed()) break;
                    // Back from ovl4 (0x80013A00 -> member 1): the title's VRAM and sound again.
                    view = std::make_unique<gt2view::TitleView>(*renderer);
                    view->SetRasterRules(rules);
                    view->UploadVram(assets.vram);
                    renderer->clearColor[0] = renderer->clearColor[1] = renderer->clearColor[2] = 0.0f;
                    window.SetTitle("gt2game");
                    openSound();
                    title.Reset();
                    window.ResetPacing();
                    break;
                }
                case shell::kOptions:
                    optionsScreen = std::make_unique<shell::OptionsScreen>(assets, save.state, &settings);
                    music->Play(6, uint8_t(shell::OptionValue(save.state, shell::kMusicVolume)));
                    screen = Screen::kOptions;
                    break;
                case shell::kSaveGame:
                case shell::kLoadGame:
                    card = std::make_unique<shell::CardManager>(assets, result == shell::kSaveGame ? shell::CardManager::kSaveGame : shell::CardManager::kLoadGame,
                                                                save.state, slots);
                    music->Play(7, uint8_t(shell::OptionValue(save.state, shell::kMusicVolume)));
                    screen = Screen::kCard;
                    break;
                case shell::kReplayTheater: // 0x801EF5F1 = 2, 0x801EF5F2 = 2
                    if (theater) theater->Start(false);
                    enterTheater(true);
                    break;
                case shell::kDataTransfer: // view 0x8004C4A0 (20 fields, CD track 7) -> 0x8004C548 (title_transfer.h)
                    if (!transferMenu) transferMenu = std::make_unique<shell::ReplayTheaterMenu>(assets, shell::ReplayTheaterMenu::TransferLayout());
                    transferMenu->Start(false);
                    music->Play(7, uint8_t(shell::OptionValue(save.state, shell::kMusicVolume)));
                    theaterDelay = 0x14;
                    transferLeaving = false;
                    screen = Screen::kTransferEnter;
                    break;
                default: { // 6: the attract demo (0x80011624, title_attract.h): the next demo file replay, then the title again
                    title.Reset();
                    if (!options.raceView || !options.raceOptions) break;
                    try {
                        if (!attract) attract = std::make_unique<gt2game::TitleAttractDemos>(disc, vol, false, assets.language);
                        ReplayPayload payload;
                        std::string demoTitle;
                        attract->Next(payload, demoTitle);
                        std::printf("title f%d: attract demo '%s' of %s\n", field, demoTitle.c_str(), attract->Path().c_str());
                        music.reset(); // the race opens its own output
                        sfx.reset();
                        gt2game::PlayTitleReplay(window, disc, vol, payload, demoTitle, *options.raceOptions, *options.raceView, options.hudModeGiven);
                    } catch (const std::exception& e) {
                        std::printf("title: attract demo: %s\n", e.what());
                    }
                    if (window.Closed()) break;
                    // 0x801EF5F1 = 0x801EF5F2 = 0: member 1 again with the title list.
                    view = std::make_unique<gt2view::TitleView>(*renderer);
                    view->SetRasterRules(rules);
                    view->UploadVram(assets.vram);
                    renderer->clearColor[0] = renderer->clearColor[1] = renderer->clearColor[2] = 0.0f;
                    window.SetTitle("gt2game");
                    openSound();
                    window.ResetPacing();
                    break;
                }
                }
            }
            if (screen == Screen::kTitle) prims = shell::BuildTitleFrame(title);
            break;
        }
        case Screen::kOptions: {
            // The ports' controllers for the KEY CONFIGURATION page: port 1 = the controller in use (the keyboard alone
            // acts as a digital pad), port 2 = none.
            optionsScreen->SetPadTypes(window.Pad().type != input::kTypeNone ? window.Pad().type : uint8_t(input::kTypeDigital), input::kTypeNone);
            optionsScreen->SetAnalogRaw(0, window.Pad().analog); // the ANALOG pages' live bytes (a neGcon-type controller)
            const bool optionsRunning = optionsScreen->Update(&pad, nullptr);
            playSounds(optionsScreen->sounds);
            if (!optionsRunning) {
                music->Stop();
                storeSettings(); // ours: the options also go to the PC settings file for races without a career
                optionsScreen.reset();
                screen = Screen::kTitle;
                title.Reset();
                prims = shell::BuildTitleFrame(title);
                break;
            }
            for (const std::string& note : optionsScreen->notes) std::printf("options: %s\n", note.c_str());
            optionsScreen->notes.clear();
            prims = optionsScreen->Frame();
            break;
        }
        case Screen::kCard: {
            const bool running = card->Update(&pad);
            playSounds(card->sounds);
            for (const std::string& line : card->log) std::printf("card f%d: %s\n", field, line.c_str());
            card->log.clear();
            if (!running) {
                if (card->CardMode() == shell::CardManager::kRenameReplay) { // 0x80012A20: exit -> sound 4, back to the menu
                    playSounds({4});
                    theater->Start(true);
                    screen = Screen::kTheater;
                    card.reset();
                    prims = shell::TitleFrameStart();
                    break;
                }
                if (card->CardMode() == shell::CardManager::kLoadReplay) { // 0x800128F8: 2 = loaded -> 0x8004B320, else back to the menu
                    if (card->Loaded() && card->LoadedReplay()) {
                        pendingReplay = card->LoadedReplay();
                        pendingTitle = card->LoadedReplayTitle();
                        playSounds({3});
                        music->Stop(); // 0x8001255C -> 0x80012434
                        theaterDelay = 0x14;
                        screen = Screen::kTheaterLeave;
                    } else {
                        playSounds({4});
                        theater->Start(true);
                        screen = Screen::kTheater;
                    }
                    card.reset();
                    prims = shell::TitleFrameStart();
                    break;
                }
                if (card->Loaded()) storeSettings();
                music->Stop();
                card.reset();
                screen = Screen::kTitle;
                title.Reset();
                prims = shell::BuildTitleFrame(title);
                break;
            }
            prims = card->Frame();
            break;
        }
        case Screen::kCopy: {
            const bool running = copyScreen->Update(&pad);
            playSounds(copyScreen->sounds);
            for (const std::string& line : copyScreen->log) std::printf("copy replay f%d: %s\n", field, line.c_str());
            copyScreen->log.clear();
            if (!running) { // 0x80012B00: the screen's exit -> sound 4, back to the theater
                playSounds({4});
                copyScreen.reset();
                theater->Start(true);
                screen = Screen::kTheater;
                prims = shell::TitleFrameStart();
                break;
            }
            prims = copyScreen->Frame();
            break;
        }
        case Screen::kTheaterEnter: // 0x800124F0: 20 empty fields, then the menu 0x8004B374 (or, when leaving, the title)
            prims = shell::TitleFrameStart();
            if (--theaterDelay <= 0) {
                if (theaterLeaving) {
                    music->Stop();
                    screen = Screen::kTitle;
                    title.Reset();
                } else {
                    screen = Screen::kTheater;
                }
            }
            break;
        case Screen::kTheater: {
            const int r = theater->Update(&pad);
            playSounds(theater->sounds);
            if (r == 2) { // back: 0x8004B2CC init(1) -> 20 fields -> the title
                theaterDelay = 0x14;
                theaterLeaving = true;
                screen = Screen::kTheaterEnter;
                prims = shell::TitleFrameStart();
                break;
            }
            if (r == 1) {
                std::printf("replay theater f%d: row %d\n", field, theater->choice);
                switch (theater->choice) {
                case shell::ReplayTheaterMenu::kLoadReplay: // view 0x8004B3C8: the card manager in mode 1
                    card = std::make_unique<shell::CardManager>(assets, shell::CardManager::kLoadReplay, save.state, slots);
                    card->SetReplayText(replayText.get());
                    screen = Screen::kCard;
                    break;
                case shell::ReplayTheaterMenu::kRenameDelete: // view 0x8004B41C: the card manager in mode 2 (0x80072F20)
                    card = std::make_unique<shell::CardManager>(assets, shell::CardManager::kRenameReplay, save.state, slots);
                    card->SetReplayText(replayText.get());
                    card->SetKeyboard(gt2::screens::MakeTitleCardKeyboard(assets));
                    screen = Screen::kCard;
                    break;
                case shell::ReplayTheaterMenu::kDemonstration: // view 0x8004B224
                    demo = std::make_unique<shell::DemonstrationScreen>(assets, *replayText, *demoFile);
                    screen = Screen::kDemo;
                    break;
                default: // Copy Replay: view 0x8004B470 over member 1's copy screen 0x80015CF8 (card 1 -> card 2)
                    copyScreen = std::make_unique<shell::CopyReplayScreen>(assets, *replayText, slots);
                    screen = Screen::kCopy;
                    break;
                }
            }
            prims = screen == Screen::kTheater ? theater->Frame() : shell::TitleFrameStart();
            break;
        }
        case Screen::kDemo: {
            const int r = demo->Update(&pad);
            playSounds(demo->sounds);
            if (r == 2) {
                demo.reset();
                theater->Start(true);
                screen = Screen::kTheater;
                prims = theater->Frame();
                break;
            }
            if (r == 1) { // 0x8004B278: 20 fields, then 0x80020E14 gathers and unpacks the chosen replay
                try {
                    std::vector<uint8_t> data = demoFile->EntryData(demo->choice);
                    data.resize(size_t(demoFile->Entry(demo->choice).size));
                    pendingReplay = UnpackReplayPayload(data);
                    pendingTitle = demoFile->Entry(demo->choice).Title();
                } catch (const std::exception& e) {
                    std::printf("replay theater: demo %d: %s\n", demo->choice, e.what());
                    pendingReplay.reset();
                }
                demo.reset();
                theaterDelay = 0x14;
                screen = Screen::kTheaterLeave;
                prims = shell::TitleFrameStart();
                break;
            }
            prims = demo->Frame();
            break;
        }
        case Screen::kTransferEnter: // 0x8001DB58: 20 empty fields, then the list 0x8004C548 (or, when leaving, the title)
            prims = shell::TitleFrameStart();
            if (--theaterDelay <= 0) {
                if (transferLeaving) {
                    music->Stop();
                    screen = Screen::kTitle;
                    title.Reset();
                } else {
                    screen = Screen::kTransfer;
                }
            }
            break;
        case Screen::kTransfer: { // 0x8001DD98 (the code of 0x80012730)
            const int r = transferMenu->Update(&pad);
            playSounds(transferMenu->sounds);
            if (r == 2) {
                theaterDelay = 0x14;
                transferLeaving = true;
                screen = Screen::kTransferEnter;
                prims = shell::TitleFrameStart();
                break;
            }
            if (r == 1) { // the views of 0x8004C3A4: the manager in mode = the row
                std::printf("data transfer f%d: row %d\n", field, transferMenu->choice);
                transferCard = std::make_unique<shell::TransferManager>(assets, shell::TransferManager::Mode(transferMenu->choice), slots);
                screen = Screen::kTransferCard;
                prims = transferCard->Frame();
                break;
            }
            prims = transferMenu->Frame();
            break;
        }
        case Screen::kTransferCard: { // 0x8001DF54 / 0x8001E550 / 0x8001E754 over 0x80020868
            const int r = transferCard->Update(&pad);
            playSounds(transferCard->sounds);
            for (const std::string& line : transferCard->log) std::printf("data transfer f%d: %s\n", field, line.c_str());
            transferCard->log.clear();
            if (r == 1) { // left: sound 4, back to the list
                playSounds({4});
                transferCard.reset();
                transferMenu->Start(true);
                screen = Screen::kTransfer;
                prims = transferMenu->Frame();
                break;
            }
            if (r == 2) {
                playSounds({3});
                const shell::TransferManager::Mode mode = transferCard->TransferMode();
                if (mode == shell::TransferManager::kTrade) { // view 0x8004C5F0 over the loaded save's garage
                    if (!carInfo) carInfo = std::make_unique<CarInfoDirectory>(CarInfoDirectory::Load(vol));
                    tradeSource = *transferCard->LoadedSave();
                    trade = std::make_unique<shell::TradeScreen>(assets, *carInfo, save.state, *tradeSource);
                    screen = Screen::kTrade;
                    prims = trade->Frame();
                    break;
                }
                if (mode == shell::TransferManager::kMix) { // 0x8001E014 / 0x8001E284 / 0x8001E38C.. into the career in memory
                    shell::MixRecords(save.state, *transferCard->LoadedSave());
                    std::printf("data transfer: records of the loaded save mixed into the career\n");
                } else { // 0x8001E61C
                    shell::ConvertGt1Licences(save.state, transferCard->Gt1Data());
                    std::printf("data transfer: Gran Turismo licences converted (B: %s, A: %s)\n", save.state.licences[5][0].passed ? "passed" : "-",
                                save.state.licences[4][0].passed ? "passed" : "-");
                }
            }
            prims = transferCard->Frame();
            break;
        }
        case Screen::kTrade: { // 0x8001F11C
            const int bought = trade->bought;
            const int r = trade->Update(&pad);
            playSounds(trade->sounds);
            if (trade->bought != bought) std::printf("data transfer: car bought (money %d, %d car(s))\n", save.state.garage.money, save.state.garage.count);
            if (r == 2) { // back to the manager view (0x8001DF04: a new manager in mode 0)
                trade.reset();
                transferCard = std::make_unique<shell::TransferManager>(assets, shell::TransferManager::kTrade, slots);
                screen = Screen::kTransferCard;
                prims = transferCard->Frame();
                break;
            }
            prims = trade->Frame();
            break;
        }
        case Screen::kTheaterLeave: // 0x8001258C / 0x80012EC4: 20 fields, then the race overlay plays the replay
            prims = shell::TitleFrameStart();
            if (--theaterDelay <= 0) {
                if (pendingReplay) {
                    const ReplayPayload payload = *pendingReplay;
                    pendingReplay.reset();
                    playReplay(payload, pendingTitle);
                    if (window.Closed()) break;
                    if (screen == Screen::kTheaterLeave) { // not played (not available): back to the menu
                        theater->Start(true);
                        screen = Screen::kTheater;
                    }
                } else {
                    theater->Start(true);
                    screen = Screen::kTheater;
                }
            }
            break;
        }

        if (sfx) sfx->Frame();
        std::vector<gt2view::DrawItem> items;
        view->Build(prims, TitleAssets::kScreenWidth, renderer->AspectRatio(), items, options.squarePixels);
        std::string shotPath, comparePath;
        for (size_t k = 0; k < options.shots.size(); k++)
            if (options.shots[k].first == field) {
                shotPath = options.shots[k].second;
                if (k < options.compare.size()) comparePath = options.compare[k];
            }
        window.EndFrame(items, shotPath, fieldTime);
        if (!shotPath.empty()) {
            std::printf("title f%d: screen %d -> %s\n", field, int(screen), shotPath.c_str());
            { // dev aid: the frame's primitives (GPU order) next to the shot, for comparing with a gt2play --prims listing
                std::filesystem::path listPath = shotPath;
                listPath.replace_extension(".prims.txt");
                if (std::FILE* f = std::fopen(listPath.string().c_str(), "w")) {
                    static const char* const kKinds[] = {"SPRT", "TILE", "POLYF4", "POLYG4", "LINE"};
                    for (size_t i = 0; i < prims.size(); i++) {
                        const MenuPrim& p = prims[i];
                        std::fprintf(f, "%zu %s tpage=%03X rgb=%06X%s xy=(%d,%d) size=(%d,%d) uv=(%d,%d) clut=%04X", i, kKinds[p.kind], p.tpage, p.colour[0], p.semi ? " semi" : "",
                                     p.x[0], p.y[0], p.w, p.h, p.u, p.v, p.clut);
                        if (p.kind >= MenuPrim::kPolyF4) std::fprintf(f, " v1=(%d,%d) v2=(%d,%d) v3=(%d,%d) c1=%06X", p.x[1], p.y[1], p.x[2], p.y[2], p.x[3], p.y[3], p.colour[1]);
                        std::fprintf(f, "\n");
                    }
                    std::fclose(f);
                }
            }
            // The same primitives through the software canvas: separates state from GPU differences.
            const MenuCanvas canvas = shell::RenderTitleFrame(assets, prims, rules);
            std::filesystem::path canvasPath = shotPath;
            canvasPath.replace_filename(canvasPath.stem().string() + "_canvas.png");
            WritePngRgba(canvasPath.string(), MenuCanvas::kWidth, MenuCanvas::kHeight, canvas.Rgba());
            if (!comparePath.empty()) {
                std::filesystem::path side = shotPath;
                side.replace_filename(side.stem().string() + "_side.png");
                const PngImage ours = ReadPngFile(shotPath);
                if (ours.width == TitleAssets::kScreenWidth && ours.height == TitleAssets::kScreenHeight) {
                    std::printf("vulkan ");
                    CompareWithCapture(ours.rgba, ours.width, ours.height, comparePath, side.string());
                } else {
                    std::printf("compare: the Vulkan shot is %d x %d (needs --window 352x480 --title-square)\n", ours.width, ours.height);
                }
                std::filesystem::path canvasSide = shotPath;
                canvasSide.replace_filename(canvasSide.stem().string() + "_canvas_side.png");
                std::printf("canvas ");
                CompareWithCapture(canvas.Rgba(), MenuCanvas::kWidth, MenuCanvas::kHeight, comparePath, canvasSide.string());
                std::filesystem::path interpSide = shotPath;
                interpSide.replace_filename(interpSide.stem().string() + "_interp_side.png");
                std::printf("canvas (interpreter sprite rules) ");
                CompareWithCapture(RenderWithInterpreterSprites(assets, prims).Rgba(), MenuCanvas::kWidth, MenuCanvas::kHeight, comparePath, interpSide.string());
            }
            if (field >= lastShot && options.quitAfter == 0) break;
        }
        if (options.quitAfter > 0 && field + 1 >= options.quitAfter) break;
        const int lastAny = window.LastShotField(); // --shot-at: leave after the last global shot once the script is done
        if (lastAny >= 0 && options.quitAfter == 0 && field >= lastAny && field >= window.ScriptEnd()) break;
    }
    view.reset();
    std::printf("title: left after %d fields (day %u, money %d, %d car(s))\n", field, save.state.record.days, save.state.garage.money, save.state.garage.count);
    return 0;
}
