// gt2game on the arcade disc: title -> arcade menus -> race -> menus (arcade_mode.h).
#include "arcade_mode.h"

#include "platform/os/keys.h"
#include "platform/os/paths.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <stdexcept>

#include "arcade_menu_check.h"
#include "arcade_post_race.h"
#include "arcade_race.h"
#include "arcade_title.h"
#include "title_attract.h"
#include "game/arcade/arcade_battle.h"
#include "game/arcade/arcade_car_page.h"
#include "game/arcade/arcade_menus.h"
#include "game/arcade/arcade_results.h"
#include "game/arcade/arcade_setup.h"
#include "game/audio/menu_audio.h"
#include "game/audio/menu_music.h"
#include "game/shell/title_replay.h"
#include "gt2formats/replay_card.h"
#include "gt2view/race_card_screens.h"
#include "game/career/career_state.h"
#include "game/career/tuning.h"
#include "game/shell/title_draw.h"
#include "game/shell/title_menu.h"
#include "game/shell/title_screens.h"
#include "game_window.h"
#include "movie_player.h"
#include "boot_screen.h"
#include "panel.h"
#include "split_race.h"
#include "gt2export/png_writer.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/car_info.h"
#include "gt2formats/course_data.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/png_reader.h"
#include "gt2formats/race_capture.h"
#include "gt2formats/replay.h"
#include "gt2formats/title_assets.h"
#include "game/menu/menu_car.h"
#include "gt2view/menu_view.h"
#include "gt2view/title_view.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

using namespace gt2;
namespace pad = gt2::menu_list_pad;

namespace gt2game {
namespace {

uint32_t KeyBit(int key) {
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
    case 'Q': return pad::kL1; // as the title's keys (title_mode.cpp): the card manager's delete mode (L1 + R1 held)
    case 'W': return pad::kR1;
    case gt2::keys::kDelete: return pad::kSquare;
    default: return 0;
    }
}

// The second controller as the menus' pad (the 2PLAYER BATTLE page's player 2): port 2 merged with player 2's keys
// (game_window.h Pad2), PS1 buttons -> the generic bits of 0x80083A4C.
uint32_t GenericButtons(uint16_t ps1Buttons) {
    namespace b = gt2::input::ps1;
    constexpr std::pair<uint16_t, uint32_t> kMap[] = {{b::kUp, pad::kUp},         {b::kDown, pad::kDown},     {b::kLeft, pad::kLeft},   {b::kRight, pad::kRight},
                                                      {b::kCross, pad::kCross},   {b::kCircle, pad::kCircle}, {b::kSquare, pad::kSquare}, {b::kTriangle, pad::kTriangle},
                                                      {b::kL1, pad::kL1},         {b::kL2, pad::kL2},         {b::kR1, pad::kR1},       {b::kR2, pad::kR2},
                                                      {b::kStart, pad::kStart},   {b::kSelect, pad::kSelect}};
    uint32_t out = 0;
    for (const auto& [from, to] : kMap)
        if (ps1Buttons & from) out |= to;
    return out;
}

// A frame against a gt2play --prims VRAM dump: the 352 x 480 drawing area at (0, 0), 5-bit channels (as title_mode.cpp).
size_t CompareWithCapture(const std::vector<uint8_t>& oursRgba, int w, const std::string& capture, const std::string& sidePath) {
    std::FILE* f = std::fopen(capture.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + capture);
    std::vector<uint16_t> vram(1024 * 512);
    const size_t n = std::fread(vram.data(), 2, vram.size(), f);
    std::fclose(f);
    if (n != vram.size()) throw std::runtime_error(capture + ": not a 1024 x 512 VRAM dump");
    const int W = TitleAssets::kScreenWidth, H = TitleAssets::kScreenHeight;
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    size_t diff = 0, outside = 0;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    // GT2_ARCADE_COMPARE_MASK="x0,y0,x1,y1" (inclusive): also count the differing pixels outside that rectangle (the 3D car's
    // drawing area of the car selection, 48,180 - 303,379: arcade_disc.md section 18)
    // (a second rectangle after ';': the 2PLAYER BATTLE page's two car areas 138,94 - 347,237;4,302 - 213,445)
    int mask[4] = {0, 0, -1, -1}, mask2[4] = {0, 0, -1, -1};
    if (const char* m = std::getenv("GT2_ARCADE_COMPARE_MASK"))
        std::sscanf(m, "%d,%d,%d,%d;%d,%d,%d,%d", &mask[0], &mask[1], &mask[2], &mask[3], &mask2[0], &mask2[1], &mask2[2], &mask2[3]);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t* o = &oursRgba[(size_t(y) * size_t(w) + size_t(x)) * 4];
            const uint16_t c = vram[size_t(y) * 1024 + size_t(x)];
            const int cr = c & 31, cg = (c >> 5) & 31, cb = (c >> 10) & 31;
            const bool d = (o[0] >> 3) != cr || (o[1] >> 3) != cg || (o[2] >> 3) != cb;
            if (d) {
                diff++;
                if ((x < mask[0] || x > mask[2] || y < mask[1] || y > mask[3]) && (x < mask2[0] || x > mask2[2] || y < mask2[1] || y > mask2[3])) outside++;
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
    if (mask[2] >= mask[0]) std::printf(", %zu outside %d,%d - %d,%d", outside, mask[0], mask[1], mask[2], mask[3]);
    std::printf(" -> %s\n", sidePath.c_str());
    WritePngRgba(sidePath, W * 3, H, side);
    return diff;
}

// The frame on the software canvas with the interpreter GPU's sprite rules (the captures' rasteriser; as title_mode.cpp).
MenuCanvas RenderInterpreter(const MenuVram& vram, const std::vector<MenuPrim>& prims) {
    MenuCanvas canvas;
    canvas.rules = MenuCanvas::Rules::kInterpreter;
    for (const MenuPrim& p : prims) {
        if (p.kind != MenuPrim::kSprite) {
            canvas.Draw(vram, p);
            continue;
        }
        const int mr = int(p.colour[0] & 0xFF), mg = int((p.colour[0] >> 8) & 0xFF), mb = int((p.colour[0] >> 16) & 0xFF);
        const int mode = (p.tpage >> 5) & 3;
        for (int j = 0; j < p.h; j++)
            for (int i = 0; i < p.w; i++) {
                const int x = p.x[0] + i, y = p.y[0] + j;
                if (x < 0 || y < 0 || x >= MenuCanvas::kWidth || y >= MenuCanvas::kHeight) continue;
                const uint16_t t = vram.Sample(p.tpage, p.clut, uint8_t(p.u + i), uint8_t(p.v + j));
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

// The career block the arcade menus read: the new game of 0x800104A0 (the arcade disc boots with the same block: options
// +1..+8 = 00 02 01 00 02 00 01 ..., licence records all unpassed = only the always-open course tier).
std::vector<uint8_t> NewCareerBlock(const DiscImage& disc, const GtfsVolume& vol) {
    career::NewGameDefaults defaults;
    try {
        defaults = career::ReadNewGameDefaults(disc);
    } catch (const std::exception& e) {
        std::printf("arcade: new-game button tables not read (%s); the pad tables stay 0 (unused by the arcade menus)\n", e.what());
        defaults.courseCount = uint16_t(ParseCourseInfo(vol.Read(".crsinfo")).entries.size());
    }
    const career::CareerState s = career::NewCareer(defaults);
    std::vector<uint8_t> block(sizeof(s));
    std::memcpy(block.data(), &s, sizeof(s));
    return block;
}

// Time Trial / Rally (game mode 6, the "ATT" event): the race overlay's arcade loop for mode 6 (member 0 0x80016F14 -> state
// machine 0x80015ED4; docs/research/arcade_disc.md 17.10). The race of the menus' block with the ghost of the best lap (race_shell.h
// GhostSession: lap 1 has no ghost; from lap 2 the best lap so far drives against the player; drawn by 0x800140A4's rule, the
// career's ghost option + 0xB5) and the course record of the career (*(0x800A9524) = career + 0x218 + course * 0x24: the HUD
// compares every lap and split with it, a faster valid lap replaces it). The race has 100 laps and never finishes on a circuit:
// Start / Esc = the pause menu, Exit ends it (0x800153B8: RaceSim::EndGhostRace). Then the post-race views of mode 6
// (arcade_post_race.h RunArcadeSessionViews: ENTER YOUR NAME after a new record, SESSION RESULTS, TIME TRIAL). Try Again runs the
// race again with the same ghost session (0x8002F4B4 keeps the reference; the race end handed the best lap to entry 1), Exit goes
// back to the menus. Between the pause Exit and the views the replay of the ring's laps plays (states 10 -> 12, 0x80016274;
// RaceViewConfig::ghostReplay, 17.10); the menu's Replay plays it again and shows the views again. Returns false when the window
// was closed.
bool RunArcadeGhostSession(GameWindow& window, const DiscImage& disc, const GtfsVolume& vol, const ArcadeModeOptions& options, const arcade::ArcadeRaceSetup& setup,
                           const std::string& course, arcade::ArcadeMenus& menus, const RaceMenuAssets* postAssets) {
    const std::unique_ptr<Panels> panels = LoadArcadeRacePanels(window.Renderer(), disc, vol);
    sim::GhostSession ghost; // one visit of the race overlay (0x8002F4B4 clears the reference once)
    std::array<uint8_t, 0x58C> raceBlock = setup.raceBlock; // + 0x53C changes with a new record's name entry
    std::optional<RaceMenuAssets> sessionAssets;
    if (postAssets) sessionAssets = WithCoursePicture(*postAssets, vol, course);
    const char* kind = raceBlock[0x588] & 1 ? "Time Trial" : "Rally";
    // The TIME TRIAL menu's memory-card rows (gt2view/race_card_screens.h): the arcade title's strings / executable in the
    // Simulation layout with the race menus' fonts, the course names of the row printer, the cards of --card / --card2.
    std::optional<TitleAssets> cardAssets;
    std::optional<shell::ReplayRowText> cardText;
    if (sessionAssets) {
        try {
            cardAssets = screens::RaceCardAssets(*sessionAssets, TitleAssets::LoadArcade(disc, vol).SimLayoutScreens());
            cardText = shell::ReplayRowText::Load(vol, *cardAssets);
        } catch (const std::exception& e) {
            std::printf("arcade: no memory-card views (%s)\n", e.what());
            cardAssets.reset();
        }
    }
    for (int race = 1;; race++) { // Try Again: 0x80016C58 (state 8) and the race again
        RaceOptions raceOptions = options.race;
        RaceData data;
        const size_t count = LoadRaceBlock(disc, vol, raceBlock, setup.settings, course, raceOptions, data);
        ApplyIntroHold(disc, data, raceOptions);
        if (data.courseIndex < 0) throw std::runtime_error("arcade: the course has no .crsinfo entry (its course record)");
        const size_t courseIndex = size_t(data.courseIndex);
        sim::LapEntry recordLap;
        std::memcpy(&recordLap, arcade::CourseRecord(menus.Career(), courseIndex).data(), sizeof recordLap);
        sim::LapEntry written = recordLap;
        data.shell.hasCourseRecord = true;
        data.shell.courseRecord = recordLap;
        data.shell.courseRecordTarget = &written;
        data.shell.ghost = &ghost;
        std::string courseName;
        for (size_t k = 0; k < 0x20 && raceBlock[0x20 + k]; k++) courseName.push_back(char(raceBlock[0x20 + k]));
        std::printf("arcade %s: %s, race %d of the session, course record %s, ghost %s\n", kind, courseName.c_str(), race,
                    recordLap.time == -1 ? "none" : FormatMs(recordLap.time).c_str(), ghost.entryHasLap ? "with the kept lap" : "none yet");
        RaceViewConfig cfg = options.view;
        if (cfg.hudMode < 0) cfg.hudMode = 6; // Rally / Time Trial: current lap only, no position or total time
        cfg.ghostOption = menus.Career()[0xB5];
        RaceFlow flow;
        flow.title = courseName;
        flow.raceEndMode = 4;
        flow.pauseExitEnds = true;
        flow.carNames = ArcadeRaceSlotNames(raceBlock, data.carIds.size());
        flow.finished = [&](const RaceViewResult&) {
            window.AddScript(std::to_string(window.Field() + 1) + ":enter");
            return std::vector<std::string>{};
        };
        window.AddScript(std::to_string(window.Field() + 1) + ":enter"); // the pre-race panel (ours): the race starts at once
        const RaceViewResult r = RunRaceView(window, panels.get(), disc, vol, data, count, raceOptions, cfg, &flow);
        std::printf("arcade %s ended (exit %d, laps %d, best lap %s, new record %u)\n", kind, int(r.exit), r.lapNumber, FormatMs(r.bestLap).c_str(),
                    unsigned(r.newRecord));
        if (r.exit == RaceExit::kClosed || window.Closed()) return false;
        uint32_t carId = 0;
        std::memcpy(&carId, raceBlock.data() + 0x5C, 4);
        if (ghost.newBest) raceBlock[0x5C + 0xD0 + 0x8C] = 0; // 0x800125BC: the player's entry is the ghost's again (a loaded one replaced)
        if (std::memcmp(&written, &recordLap, sizeof written) != 0) { // the shell's copy through *(0x800A9524), during the race
            std::array<uint8_t, arcade::kCourseRecordLap> lap{};
            std::memcpy(lap.data(), &written, lap.size());
            arcade::SetCourseRecordLap(menus.MutableCareer(), courseIndex, lap);
            std::printf("arcade: new course record %s on course %zu (career + 0x%zX)\n", FormatMs(written.time).c_str(), courseIndex,
                        arcade::kCourseRecords + courseIndex * arcade::kCourseRecordSize);
        }
        // The replay (states 10 .. 12 after the pause's Exit): player 1 alone on the ring's laps of this ghost session (0x800A951C;
        // ReplayRaceData, RaceSim pad slot 2), on the same session as the original (the replay's 0x8003F990 / 0x800132D0 write it).
        auto playReplay = [&]() -> bool {
            RaceData replayData = ReplayRaceData(data);
            replayData.shell.courseRecordTarget = nullptr;
            RaceOptions o = raceOptions;
            RaceViewConfig c = options.view;
            if (c.hudMode < 0) c.hudMode = 6;
            c.replayOut.clear();
            c.ghostReplay = true;
            c.replayEndLeaves = true;
            c.ghostOption = 0;
            std::printf("arcade %s: the replay, %d lap(s) of the ring\n", kind, int(int16_t(uint16_t(ghost.ring[0] | ghost.ring[1] << 8))));
            const RaceViewResult rr = RunRaceView(window, panels.get(), disc, vol, replayData, 1, o, c, nullptr); // Start: the pause menu
            return rr.exit != RaceExit::kClosed && !window.Closed();
        };
        if (r.exit == RaceExit::kExited && !playReplay()) return false;
        if (!sessionAssets) return true; // no post-race views without the race overlay's assets: back to the menus
        // 0x80016CBC mode 6: 0x8004A638(1), the views, 0x8004A658.
        auto text = [](const uint8_t* p, size_t n) {
            std::string t;
            for (size_t k = 0; k < n && p[k]; k++) t.push_back(char(p[k]));
            return t;
        };
        const std::span<const uint8_t> record = arcade::CourseRecord(menus.Career(), courseIndex);
        screens::SessionInput in;
        in.results = r.playerResults;
        in.newRecord = r.newRecord;
        std::memcpy(&in.courseRecord, record.data(), sizeof in.courseRecord);
        in.recordName = text(record.data() + 0x18, 12);
        in.recordCar = text(raceBlock.data() + 0x53C, 0x40);
        in.slotCar = text(raceBlock.data() + 0xEC, 0x40);
        in.course = courseName;
        in.ghostOption = menus.Career()[0xB5];
        in.enteredName = arcade::CareerName(menus.Career());
        in.savedBest = ghost.savedBest != 0;
        in.ghostLoaded = false; // 0x801D55AA: no ghost card manager
        {
            int16_t g = 0, gi = 0;
            std::memcpy(&g, raceBlock.data() + 0x582, 2);
            std::memcpy(&gi, raceBlock.data() + 0x584, 2);
            // 0x8004C0B0 (Sim): race block + 0x582 = 0 the home garage (career + 0x3C74), 1 the guest's; the row is enabled when
            // 0 <= + 0x584 < the garage's count (+ 0).
            int16_t n = -1;
            if (g == 0) std::memcpy(&n, menus.Career().data() + 0x3C74, 2);
            else if (g == 1 && menus.GuestGarage().size() >= 2) std::memcpy(&n, menus.GuestGarage().data(), 2);
            in.garageCar = (g == 0 || g == 1) && gi >= 0 && gi < n; // "Settings ..." (CHANGE PARTS)
        }
        std::unique_ptr<audio::MenuAudio> effects;
        if (!options.noSound) {
            try {
                effects = std::make_unique<audio::MenuAudio>();
                effects->LoadArcade(vol, LoadExeImage(disc));
                std::string error;
                if (!effects->OpenDevice(error)) effects.reset();
            } catch (const std::exception&) {
                effects.reset();
            }
        }
        auto sounds = [&](const std::vector<int>& ids) {
            if (!effects) return;
            for (int id : ids) effects->Sound(id);
            effects->Frame();
        };
        audio::MenuMusic viewMusic; // the views' CD track 8 (arcade_post_race.h ViewMusic: an XA track of MUSIC.DAT)
        if (!options.noSound) viewMusic.Open(options.discPath, LoadExeImage(disc));
        const ViewMusic music = [&](int track) {
            if (track < 0) viewMusic.Stop();
            else viewMusic.Play(track, menus.Career()[0xB3]); // 0x80080F24: the volume of career + 0xB3
        };
        const uint32_t paint = data.paints.empty() ? 0u : data.paints[0];
        // The session's payloads for Save Ghost (0x80069CC0: race block entry 1, its parameter record, the reference lap) and Save
        // Replay (0x80069948 mode 6: the race block, the results record, the ring's laps), as RAM holds them in the views.
        ArcadeCardContext cards;
        if (cardAssets) {
            cards.cardAssets = &*cardAssets;
            cards.text = &*cardText;
            cards.slots = {shell::CardSlot{options.card1Path}, shell::CardSlot{options.card2Path}};
            std::memcpy(&cards.courseId, raceBlock.data() + 0x40, 4);
            auto ramBlock = [&]() { // 0x801D585C as the race left it: the ghost entry (0x800125BC), the record car's name (0x8004A990)
                std::array<uint8_t, 0x58C> b = raceBlock;
                if (ghost.entryHasLap && b[0x5C + 0xD0 + 0x8C] == 0) {
                    std::copy(b.begin() + 0x5C, b.begin() + 0x5C + 0xD0, b.begin() + 0x5C + 0xD0);
                    b[0x5C + 0xD0 + 0x8C] = 1;
                    b[0x5C + 0xD0 + 0x8E] = 2;
                }
                if (r.newRecord == 1) std::copy(b.begin() + 0xEC, b.begin() + 0xEC + 0x40, b.begin() + 0x53C);
                return b;
            };
            cards.ghostPayload = [&]() { // 0x80072598
                const std::array<uint8_t, 0x58C> b = ramBlock();
                ReplayGhostRecord g;
                std::copy(b.begin() + 0x5C + 0xD0, b.begin() + 0x5C + 0x1A0, g.entry.begin());
                std::memcpy(g.params.data(), &data.params[0], g.params.size()); // 0x801DEA7A = the player's 0x801DE8BA (0x800125BC)
                std::copy(ghost.reference.begin(), ghost.reference.begin() + 0xE0, g.head.begin());
                const size_t used = size_t(ghost.reference[0xE0 + 0x10] | ghost.reference[0xE0 + 0x11] << 8);
                g.stream.assign(ghost.reference.begin() + 0xE0, ghost.reference.begin() + std::ptrdiff_t(0xE0 + std::min<size_t>(used + 0x19, 0x101C)));
                ReplayPayload forDesc;
                std::copy(b.begin(), b.end(), forDesc.race.begin());
                std::array<uint8_t, 0x50> desc = ReplayDescription(forDesc);
                desc[0x42] = 1;
                return std::make_pair(PackGhostRecord(g), desc);
            };
            cards.replayPayload = [&]() { // 0x800724F8 (0x80069948 in game mode 6)
                ReplayPayload p;
                const std::array<uint8_t, 0x58C> b = ramBlock();
                std::copy(b.begin(), b.end(), p.race.begin());
                p.results6.resize(0xFC);
                std::memcpy(p.results6.data(), &r.playerResults, 0xFC);
                const int16_t laps = int16_t(uint16_t(ghost.ring[0] | ghost.ring[1] << 8));
                for (int k = 0; k < laps && k < 4; k++) {
                    const uint8_t* lap = ghost.ring.data() + 4 + size_t(k) * sim::kGhostLapSize;
                    const size_t used = size_t(lap[0xE0 + 0x10] | lap[0xE0 + 0x11] << 8);
                    p.ghosts.emplace_back(std::vector<uint8_t>(lap, lap + 0xE0), std::vector<uint8_t>(lap + 0xE0, lap + 0xE0 + std::min<size_t>(used + 0x19, 0x101C)));
                }
                return std::make_pair(PackReplayPayload(p), ReplayDescription(p));
            };
        }
        ArcadeCardContext* cardsIn = cardAssets ? &cards : nullptr;
        // Settings ... (0xFB, CHANGE PARTS, gt2view/change_parts.h) for a garage car: race block + 0x582 (0 home, 1 guest) / + 0x584 the
        // index, + 0x586 the power limit, + 0x588 the course flags. The page edits the sheet 0x8016E894 of the car (0x800173E8 of the
        // garage car, career::LoadCarSheet) and 0x80056FF0 commits it into the race block's slot 0 (the next Try Again races it) and the
        // garage car (the career's home garage / the guest garage).
        ArcadePartsContext parts;
        std::unique_ptr<career::TuneSheet> partsSheet;
        std::unique_ptr<career::CareerData> partsData;
        std::vector<uint8_t> guest;
        int16_t garage = 0, garageIndex = 0;
        std::memcpy(&garage, raceBlock.data() + 0x582, 2);
        std::memcpy(&garageIndex, raceBlock.data() + 0x584, 2);
        auto garageBlock = [&]() -> career::GarageBlock& {
            if (garage == 0) return *reinterpret_cast<career::GarageBlock*>(menus.MutableCareer().data() + 0x3C74);
            guest.assign(menus.GuestGarage().begin(), menus.GuestGarage().end());
            if (guest.size() < sizeof(career::GarageBlock)) throw std::runtime_error("arcade CHANGE PARTS: no guest garage block");
            return *reinterpret_cast<career::GarageBlock*>(guest.data());
        };
        const bool garageCar = in.garageCar;
        if (garageCar) {
            parts.open = [&]() {
                if (!partsData) {
                    partsData = std::make_unique<career::CareerData>(career::CareerData{CarParamTables::Load(vol), CarInfoDirectory::Load(vol), GtModeRaceData::Load(vol), {},
                                                                                       LoadExeImage(disc), GuestImage{}});
                    partsData->strict = false;
                }
                career::GarageBlock& g = garageBlock();
                partsSheet = std::make_unique<career::TuneSheet>();
                career::LoadCarSheet(*partsSheet, g.cars[garageIndex], partsData->tables); // 0x800173E8
                std::printf("arcade CHANGE PARTS: garage %d car %d of %d (%s), sheet stages", int(garage), int(garageIndex), int(g.count), UnpackCarId(g.cars[garageIndex].carId).c_str());
                for (int16_t v : partsSheet->stage) std::printf(" %d", int(v));
                std::printf("\n");
                screens::ChangePartsContext c;
                c.sheet = partsSheet.get();
                c.data = partsData.get();
                c.garageCar = &g.cars[garageIndex];
                std::memcpy(&c.powerLimit, raceBlock.data() + 0x586, 2);
                std::memcpy(&c.restrictions, raceBlock.data() + 0x588, 2);
                return c;
            };
            parts.commit = [&]() { // 0x80056FF0
                career::GarageBlock& g = garage == 0 ? *reinterpret_cast<career::GarageBlock*>(menus.MutableCareer().data() + 0x3C74)
                                                     : *reinterpret_cast<career::GarageBlock*>(guest.data());
                sim::CarParams record{};
                CarConfig slotConfig;
                std::memcpy(&slotConfig, raceBlock.data() + 0x64, sizeof slotConfig);
                std::vector<uint8_t> scratch(0x400);
                career::SettingsCommit out;
                out.raceRecord = &record;
                out.raceSlotConfig = &slotConfig;
                out.garageCar = &g.cars[garageIndex];
                career::CommitSettings(*partsSheet, partsData->tables, career::BuildScratch{scratch.data()}, out);
                std::memcpy(raceBlock.data() + 0x64, &slotConfig, sizeof slotConfig);
                if (garage == 1) menus.SetGuestGarage(guest);
            };
        }
        ArcadeSessionOutcome o = RunArcadeSessionViews(window, *sessionAssets, vol, in, true, carId, int(paint), options.squarePixels, false, sounds, music, cardsIn,
                                                       garageCar ? &parts : nullptr);
        while (o.choice == ArcadeSessionOutcome::kReplay) { // state 11: the replay again, then the views without the race's (raceRun 0)
            menus.MutableCareer()[0xB5] = o.ghostOption;
            in.ghostOption = o.ghostOption;
            if (!playReplay()) return false;
            o = RunArcadeSessionViews(window, *sessionAssets, vol, in, false, carId, int(paint), options.squarePixels, false, sounds, music, cardsIn,
                                      garageCar ? &parts : nullptr);
        }
        effects.reset();
        if (o.choice == ArcadeSessionOutcome::kClosed) return false;
        menus.MutableCareer()[0xB5] = o.ghostOption; // 0x801C9995 (the ghost option list)
        if (o.named) { // 0x8004A990: the name buffer (edited in place), the record's name and car, race block + 0x53C = + 0xEC
            arcade::SetCareerName(menus.MutableCareer(), o.name);
            arcade::SetCourseRecordOwner(menus.MutableCareer(), courseIndex, carId);
            std::copy(raceBlock.begin() + 0xEC, raceBlock.begin() + 0xEC + 0x40, raceBlock.begin() + 0x53C);
        }
        if (!cards.loadedGhost.empty()) { // 0x8004A658 -> loop + 0x5D1; state 8: entry 1 = the loaded one, + 0x8C = 1, + 0x8D = 0, + 0x8E = 2
            const ReplayGhostRecord g = UnpackGhostRecord(cards.loadedGhost); // 0x80069D58
            std::copy(g.entry.begin(), g.entry.end(), raceBlock.begin() + 0x5C + 0xD0);
            raceBlock[0x5C + 0xD0 + 0x8C] = 1;
            raceBlock[0x5C + 0xD0 + 0x8D] = 0;
            raceBlock[0x5C + 0xD0 + 0x8E] = 2;
            std::copy(g.head.begin(), g.head.end(), ghost.reference.begin());
            std::fill(ghost.reference.begin() + 0xE0, ghost.reference.end(), uint8_t(0));
            std::copy(g.stream.begin(), g.stream.end(), ghost.reference.begin() + 0xE0);
            ghost.savedBest = 1;   // 0x8002F4B1
            ghost.entryHasLap = true;
            std::printf("arcade: the next race's ghost is the loaded one (%zu bytes of stream)\n", g.stream.size());
        }
        if (const char* dump = std::getenv("GT2_ARCADE_CAREER_DUMP")) { // dev aid: the career block (0x7C9C bytes) after the views and their write-backs
            if (std::FILE* f = std::fopen(dump, "wb")) {
                std::fwrite(menus.Career().data(), 1, menus.Career().size(), f);
                std::fwrite(raceBlock.data(), 1, raceBlock.size(), f); // then the race block (0x58C bytes)
                std::fclose(f);
                std::printf("arcade: career and race block written to %s\n", dump);
            }
        }
        if (o.choice != ArcadeSessionOutcome::kTryAgain) return true; // Exit
    }
}

// The race of the menus and what the race overlay's arcade loop runs after it (member 0 0x80016F14 / 0x80016CC0, mode 4;
// arcade_post_race.h): the race (the race-end display's X ends it), the career's course win flags (0x80050EF0 ->
// 0x8005DC64, game/arcade/arcade_results.h), the automatic replay (Esc leaves it, as Start -> Exit of the original's replay
// pause), RESULTS and the post-race menu; Replay plays the race again and comes back to the menu, Try Again runs the same
// race block again, Exit goes back to the menus. Returns false when the window was closed.
//
// The race runs with a RaceFlow and the race overlay's screens (Panels, arcade_post_race.h LoadArcadeRacePanels): Esc / Start
// = the pause menu 0x80029E2C (Continue / Exit), the player's finish = the race-end display 0x8002B11C of sub-mode 4 ("Finish",
// the badges, the results table with the race slots' names) held by the X wait; the flow's pre-race and result panels (ours,
// not the arcade's; drawn empty on this disc) are passed by a scripted Enter in the field they appear. Pause -> Exit ends the race
// as in the original's arcade loop (arcade_disc.md 17.10: the state machine goes on to the replay, state 12, and after it to the
// post-race views without RESULTS - the results record's place is 0: the menu shows "Retire"; gt2run work/re/mode6_post/road1).
bool RunArcadeRaceSession(GameWindow& window, const DiscImage& disc, const GtfsVolume& vol, const ArcadeModeOptions& options, const arcade::ArcadeRaceSetup& setup,
                          const std::string& course, arcade::ArcadeMenus& menus, const RaceMenuAssets* postAssets) {
    const std::unique_ptr<Panels> panels = LoadArcadeRacePanels(window.Renderer(), disc, vol);
    const std::string replayPath = (std::filesystem::temp_directory_path() / "gt2game_arcade_last.gmr").string();
    std::string courseName;
    for (size_t k = 0; k < 0x40 && setup.raceBlock[0x20 + k]; k++) courseName.push_back(char(setup.raceBlock[0x20 + k]));
    {
        const uint8_t* sel = setup.menuRegion.data() + (arcade::kSelectionAddress - arcade::kMenuRegionAddress);
        std::string display;
        for (size_t k = 0; k < 0x40 && sel[arcade::Sel::kCourseName + k]; k++) display.push_back(char(sel[arcade::Sel::kCourseName + k]));
        if (!display.empty()) courseName = display; // the course's display name (selection + 0xB8, what the views show)
    }
    for (;;) { // Try Again runs the same block again
        RaceOptions raceOptions = options.race;
        RaceData data;
        const size_t count = LoadRaceBlock(disc, vol, setup.raceBlock, setup.settings, course, raceOptions, data);
        ApplyIntroHold(disc, data, raceOptions);
        RaceViewConfig cfg = options.view;
        if (cfg.hudMode < 0) cfg.hudMode = 2;
        cfg.replayOut = replayPath;
        std::error_code ignored;
        std::filesystem::remove(replayPath, ignored);
        RaceFlow flow;
        flow.title = courseName;
        flow.raceEndMode = 4;
        flow.pauseExitEnds = true; // pause -> Exit: the race ends, the replay and the post-race menu follow (17.10)
        flow.carNames = ArcadeRaceSlotNames(setup.raceBlock, data.carIds.size());
        flow.finished = [&](const RaceViewResult&) { // the result panel (ours) is passed at once: the arcade loop goes on
            window.AddScript(std::to_string(window.Field() + 1) + ":enter");
            return std::vector<std::string>{};
        };
        window.AddScript(std::to_string(window.Field() + 1) + ":enter"); // the pre-race panel (ours): the arcade race starts at once
        const RaceViewResult r = RunRaceView(window, panels.get(), disc, vol, data, count, raceOptions, cfg, &flow);
        std::printf("arcade race ended (exit %d, finished %d, position %d, time %s)\n", int(r.exit), int(r.finished), r.position, FormatMs(r.finishTime).c_str());
        if (r.exit == RaceExit::kClosed || window.Closed()) return false;
        if (r.finished && menus.ApplyRaceResult(setup.raceBlock, r.position))
            std::printf("arcade: course record %d won at level byte %u - career + 0xB8 flags now %02X\n", int(int8_t(setup.raceBlock[0x57E])), setup.raceBlock[0x57F],
                        menus.Career()[0xB8 + size_t(uint8_t(setup.raceBlock[0x57E]))]);
        if (!postAssets) return true; // the post-race views need the race overlay's assets (LoadArcadeRaceMenuAssets)

        std::optional<ReplayFile> replay;
        if (std::filesystem::exists(replayPath)) {
            std::ifstream in(replayPath, std::ios::binary);
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            try {
                replay = ParseReplayFile(bytes);
            } catch (const std::exception& e) {
                std::printf("arcade: the race's replay is not readable (%s)\n", e.what());
            }
        }
        auto playReplay = [&]() -> bool { // the race again from the recorded stream (race_view.h), Esc leaves
            if (!replay) return true;
            RaceOptions o = options.race;
            RaceData d;
            const size_t n = LoadRaceBlock(disc, vol, setup.raceBlock, setup.settings, course, o, d);
            ApplyIntroHold(disc, d, o);
            RaceViewConfig c = options.view;
            if (c.hudMode < 0) c.hudMode = 2;
            c.replay = &*replay;
            c.replayOut.clear();
            const RaceViewResult rr = RunRaceView(window, panels.get(), disc, vol, d, n, o, c, nullptr); // Start: the pause menu
            return rr.exit != RaceExit::kClosed && !window.Closed();
        };
        // after the race-end display's X (or the pause's Exit: states 10 -> 12) the original plays the replay at once
        if ((r.finished || r.exit == RaceExit::kExited) && !playReplay()) return false;

        ArcadePostRaceInput in;
        in.finished = r.finished;
        in.place = r.position;
        in.totalTime = uint32_t(r.finishTime);
        in.fastestLap = uint32_t(r.bestLap);
        in.laps = r.lapTimes;
        in.firstLap = std::max(1, r.lapNumber - int(r.lapTimes.size()) + 1);
        in.course = courseName;
        in.carId = data.carIds.empty() ? 0u : PackCarId(data.carIds[0]);
        in.paint = data.paints.empty() ? 0 : int(data.paints[0]);
        in.vsync = gt2::os::TickCountMs();
        std::unique_ptr<audio::MenuAudio> effects;
        if (!options.noSound) {
            try {
                effects = std::make_unique<audio::MenuAudio>();
                effects->LoadArcade(vol, LoadExeImage(disc));
                std::string error;
                if (!effects->OpenDevice(error)) effects.reset();
            } catch (const std::exception&) {
                effects.reset();
            }
        }
        auto sounds = [&](const std::vector<int>& ids) {
            if (!effects) return;
            for (int id : ids) effects->Sound(id);
            effects->Frame();
        };
        audio::MenuMusic viewMusic; // the views' CD track 8 (arcade_post_race.h ViewMusic: an XA track of MUSIC.DAT)
        if (!options.noSound) viewMusic.Open(options.discPath, LoadExeImage(disc));
        const ViewMusic music = [&](int track) {
            if (track < 0) viewMusic.Stop();
            else viewMusic.Play(track, menus.Career()[0xB3]); // 0x80080F24: the volume of career + 0xB3
        };
        bool withResults = true;
        for (;;) {
            const ArcadePostRaceChoice c = RunArcadePostRace(window, *postAssets, vol, in, withResults, options.squarePixels, sounds, music);
            withResults = false;
            if (c == ArcadePostRaceChoice::kClosed) return false;
            if (c == ArcadePostRaceChoice::kReplay) {
                effects.reset();
                if (!playReplay()) return false;
                continue;
            }
            if (c == ArcadePostRaceChoice::kTryAgain) break;
            return true; // Exit: member 2
        }
    }
}

// The 2 player Battle (game mode 0; docs/research/arcade_disc.md section 19): the split-screen race of the menus' block
// (split_race.h: player 1 on the controller in use and the race keys, player 2 on port 2 and player 2's keys) with the career's
// pad blocks (+ 0x0A / + 0x5C), then what the arcade loop runs after it (member 0 0x80016CBC with race block + 0x0A == 0, the
// same states as mode 4, 19.7): the replay of both players' streams (states 10 -> 12, split screen), the RESULTS setup
// (0x80050EF0 mode 0: the winner's count career + 0xFC / + 0xFE, arcade_results.h ApplyBattleResult) with RESULTS on two columns
// when either player finished, the post-race menu "2PLAYER BATTLE" (Replay / Try Again / Save Replay ... / Exit with the win
// counts): Replay plays the replay again, Try Again runs the same block again, Save Replay stores the mode 0 record (0x80069948:
// the race block, both players' results records and streams) on a card, Exit returns to the menus. Returns false when the
// window was closed.
bool RunArcadeBattleSession(GameWindow& window, const DiscImage& disc, const GtfsVolume& vol, const ArcadeModeOptions& options, const arcade::ArcadeRaceSetup& setup,
                            const std::string& course, arcade::ArcadeMenus& menus, const RaceMenuAssets* postAssets) {
    const std::unique_ptr<Panels> panels = LoadArcadeRacePanels(window.Renderer(), disc, vol);
    std::string courseName;
    for (size_t k = 0; k < 0x20 && setup.raceBlock[0x20 + k]; k++) courseName.push_back(char(setup.raceBlock[0x20 + k]));
    {
        const uint8_t* sel = setup.menuRegion.data() + (arcade::kSelectionAddress - arcade::kMenuRegionAddress);
        std::string display;
        for (size_t k = 0; k < 0x40 && sel[arcade::Sel::kCourseName + k]; k++) display.push_back(char(sel[arcade::Sel::kCourseName + k]));
        if (!display.empty()) courseName = display; // the course's display name (selection + 0xB8, what the views show)
    }
    // The memory-card view of "Save Replay ..." (gt2view/race_card_screens.h, as the TIME TRIAL menu's).
    std::optional<TitleAssets> cardAssets;
    std::optional<shell::ReplayRowText> cardText;
    if (postAssets) {
        try {
            cardAssets = screens::RaceCardAssets(*postAssets, TitleAssets::LoadArcade(disc, vol).SimLayoutScreens());
            cardText = shell::ReplayRowText::Load(vol, *cardAssets);
        } catch (const std::exception& e) {
            std::printf("arcade: no memory-card views (%s)\n", e.what());
            cardAssets.reset();
        }
    }
    const std::vector<std::string> names = ArcadeRaceSlotNames(setup.raceBlock, 2);
    for (;;) { // Try Again runs the same block again
        RaceOptions raceOptions = options.race;
        RaceData data;
        LoadRaceBlock(disc, vol, setup.raceBlock, setup.settings, course, raceOptions, data);
        ApplyIntroHold(disc, data, raceOptions);
        SplitRaceConfig split;
        split.view = options.view;
        if (split.view.hudMode < 0) split.view.hudMode = 2;
        const std::span<const uint8_t> career = menus.Career();
        split.padBlock1.assign(career.begin() + 0x0A, career.begin() + 0x0A + 0x52);
        split.padBlock2.assign(career.begin() + 0x5C, career.begin() + 0x5C + 0x52);
        for (size_t i = 0; i < 2 && i < names.size(); i++) split.names[i] = names[i];
        const SplitRaceResult r = RunSplitRace(window, panels.get(), disc, vol, data, raceOptions, split);
        std::printf("arcade 2 player Battle ended (exit %d, finished %d, places %d / %d)\n", int(r.exit), int(r.finished), int(r.places[0]), int(r.places[1]));
        if (r.exit == RaceExit::kClosed || window.Closed()) return false;
        if (r.finished) {
            const int winner = arcade::ApplyBattleResult(menus.MutableCareer(), r.places[0], r.places[1]);
            uint16_t wins[2] = {};
            std::memcpy(wins, menus.Career().data() + arcade::kBattleWins, 4);
            std::printf("arcade: player %d wins - career + 0xFC / + 0xFE now %u / %u\n", winner + 1, unsigned(wins[0]), unsigned(wins[1]));
        }
        if (!postAssets) return true; // the post-race views need the race overlay's assets (LoadArcadeRaceMenuAssets)

        // The replay of the race just driven (the race block's cars, both recorded streams).
        ReplayFile replay;
        std::copy(setup.raceBlock.begin(), setup.raceBlock.begin() + kReplayRaceBlockSize, replay.raceBlock.begin());
        for (size_t i = 0; i < 2; i++) {
            ReplayEntry e;
            std::memcpy(e.slot.data(), setup.raceBlock.data() + kReplayRaceBlockSize + i * kReplayCarStride, kReplayCarStride);
            std::memcpy(&e.carId, e.slot.data(), 4);
            replay.cars.push_back(e);
        }
        replay.stream = r.streams[0];
        replay.stream2 = r.streams[1];
        auto playReplay = [&]() -> bool {
            RaceOptions o = options.race;
            RaceData d;
            LoadRaceBlock(disc, vol, setup.raceBlock, setup.settings, course, o, d);
            ApplyIntroHold(disc, d, o);
            SplitRaceConfig c = split;
            c.replay = &replay;
            const SplitRaceResult rr = RunSplitRace(window, panels.get(), disc, vol, d, o, c); // Start: the pause menu (Exit leaves)
            return rr.exit != RaceExit::kClosed && !window.Closed();
        };
        // after the race-end display's X (or the pause's Exit: states 10 -> 12) the original plays the replay at once
        if ((r.finished || r.exit == RaceExit::kExited) && !playReplay()) return false;

        std::array<uint16_t, 2> wins{};
        std::memcpy(wins.data(), menus.Career().data() + arcade::kBattleWins, 4);
        const std::array<uint32_t, 2> carIds = {data.carIds.size() > 0 ? PackCarId(data.carIds[0]) : 0u, data.carIds.size() > 1 ? PackCarId(data.carIds[1]) : 0u};
        const std::array<int, 2> paints = {data.paints.size() > 0 ? int(data.paints[0]) : 0, data.paints.size() > 1 ? int(data.paints[1]) : 0};
        ArcadePostRaceInput in = BattlePostRaceInput(r.results[0], r.results[1], wins, courseName, carIds, paints);
        in.finished = r.finished && (r.places[0] > 0 || r.places[1] > 0);
        in.vsync = gt2::os::TickCountMs();
        std::unique_ptr<audio::MenuAudio> effects;
        if (!options.noSound) {
            try {
                effects = std::make_unique<audio::MenuAudio>();
                effects->LoadArcade(vol, LoadExeImage(disc));
                std::string error;
                if (!effects->OpenDevice(error)) effects.reset();
            } catch (const std::exception&) {
                effects.reset();
            }
        }
        auto sounds = [&](const std::vector<int>& ids) {
            if (!effects) return;
            for (int id : ids) effects->Sound(id);
            effects->Frame();
        };
        audio::MenuMusic viewMusic; // the views' CD tracks (arcade_post_race.h ViewMusic: an XA track of MUSIC.DAT)
        if (!options.noSound) viewMusic.Open(options.discPath, LoadExeImage(disc));
        const ViewMusic music = [&](int track) {
            if (track < 0) viewMusic.Stop();
            else viewMusic.Play(track, menus.Career()[0xB3]); // 0x80080F24: the volume of career + 0xB3
        };
        // Save Replay ... (0x8005B51C -> 0x80072E7C mode 0): 0x800724F8 packs RAM's race block and, game mode 0, both players'
        // results records and streams (0x80069948).
        ArcadeCardContext cards;
        if (cardAssets) {
            cards.cardAssets = &*cardAssets;
            cards.text = &*cardText;
            cards.slots = {shell::CardSlot{options.card1Path}, shell::CardSlot{options.card2Path}};
            std::memcpy(&cards.courseId, setup.raceBlock.data() + 0x40, 4);
            cards.replayPayload = [&]() {
                ReplayPayload p;
                std::copy(setup.raceBlock.begin(), setup.raceBlock.end(), p.race.begin());
                for (size_t i = 0; i < 2; i++) {
                    ReplayPayload::Player pl;
                    pl.results.resize(0xFC);
                    std::memcpy(pl.results.data(), &r.results[i], 0xFC);
                    const std::vector<uint8_t>& s = r.streams[i];
                    const size_t used = s.size() >= 0x12 ? size_t(s[0x10] | s[0x11] << 8) : 0;
                    pl.stream.assign(s.begin(), s.begin() + std::ptrdiff_t(std::min(s.size(), used + ReplayStream::kHeaderSize)));
                    p.players.push_back(std::move(pl));
                }
                return std::make_pair(PackReplayPayload(p), ReplayDescription(p));
            };
        }
        bool withResults = true;
        for (;;) {
            const ArcadePostRaceChoice c = RunArcadePostRace(window, *postAssets, vol, in, withResults, options.squarePixels, sounds, music, cardAssets ? &cards : nullptr);
            withResults = false;
            if (c == ArcadePostRaceChoice::kClosed) return false;
            if (c == ArcadePostRaceChoice::kReplay) {
                effects.reset();
                if (!playReplay()) return false;
                continue;
            }
            if (c == ArcadePostRaceChoice::kTryAgain) break;
            return true; // Exit: member 2
        }
    }
}

} // namespace

int RunArcadeMode(const DiscImage& disc, const GtfsVolume& vol, const ArcadeModeOptions& options) {
    const TitleAssets titleAssets = TitleAssets::LoadArcade(disc, vol);
    const arcade::ArcadeMenuAssets menuAssets = arcade::ArcadeMenuAssets::Load(disc, vol);
    const ArcadeData arcadeData = ArcadeData::Load(vol);
    const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
    const CourseInfoTable courses = ParseCourseInfo(vol.Read(".crsinfo"));
    const std::vector<uint8_t> careerBlock = NewCareerBlock(disc, vol);
    const arcade::ArcadeSetupData setupData{arcadeData, menuAssets.data, cars, courses};
    std::optional<RaceMenuAssets> postAssets; // the race overlay's post-race views (arcade_post_race.h)
    try {
        postAssets = LoadArcadeRaceMenuAssets(disc, vol);
    } catch (const std::exception& e) {
        std::printf("arcade: no post-race views (%s); the race returns to the menus\n", e.what());
    }

    GameWindow window("gt2game (arcade)", options.windowWidth, options.windowHeight);
    window.SetPacing(options.pacing);
    if (!options.globalScript.empty()) window.AddScript(options.globalScript);
    for (const auto& s : options.anyShots) window.AddShot(s.first, s.second);
    gt2view::VkSceneRenderer* renderer = &window.Renderer();
    const MenuCanvas::Rules rules = options.compare.empty() ? MenuCanvas::Rules::kPs1 : MenuCanvas::Rules::kInterpreter;
    auto view = std::make_unique<gt2view::TitleView>(*renderer);
    view->SetRasterRules(rules);
    enum class Screen { kTitle, kNotice, kMenus, kTitleScreen };
    Screen screen = Screen::kTitle;
    // The title's Replay Theater / Options / Save Game / Load Game / Data Transfer and the first-boot auto-load on the session's
    // career (arcade_title.h).
    ArcadeTitleScreens titleScreens(titleAssets, vol, options, careerBlock);
    if (titleScreens.StartBoot()) screen = Screen::kTitleScreen; // 0x800112F4: the first-boot view (0x8004AE28)
    const MenuVram* uploaded = nullptr;
    auto use = [&](const MenuVram& vram) {
        if (uploaded == &vram) return;
        view->UploadVram(vram);
        uploaded = &vram;
    };
    auto resetView = [&] {
        view = std::make_unique<gt2view::TitleView>(*renderer);
        view->SetRasterRules(rules);
        uploaded = nullptr;
        renderer->clearColor[0] = renderer->clearColor[1] = renderer->clearColor[2] = 0.0f;
        window.SetTitle("gt2game (arcade)");
        window.ResetPacing();
    };
    renderer->clearColor[0] = renderer->clearColor[1] = renderer->clearColor[2] = 0.0f;

    // The menus' sound: the EXE's effects and member 2's music (sound/arcade.seq on sound/arcseq.ins, game/audio/menu_audio.h).
    // --arcade-record-audio <wav> records the menus' output and writes <wav>.events.txt (effects, sequence notes, voices).
    std::unique_ptr<audio::MenuAudio> sfx;
    std::FILE* audioEvents = nullptr;
    bool recordMenus = !options.recordAudio.empty();
    auto openSound = [&] {
        sfx.reset();
        if (options.noSound) return;
        try {
            sfx = std::make_unique<audio::MenuAudio>();
            sfx->LoadArcade(vol, titleAssets.exe);
            std::string error;
            if (recordMenus) {
                recordMenus = false; // the first menus session only
                if (!sfx->RecordTo(options.recordAudio)) std::printf("arcade: cannot record to %s\n", options.recordAudio.c_str());
                audioEvents = std::fopen((options.recordAudio + ".events.txt").c_str(), "w");
                if (audioEvents) sfx->SetEventLog(audioEvents);
            }
            if (!sfx->OpenDevice(error)) {
                std::printf("arcade: no sound device for the effects (%s)\n", error.c_str());
                sfx.reset();
            }
        } catch (const std::exception& e) {
            std::printf("arcade: menu sound effects disabled (%s)\n", e.what());
            sfx.reset();
        }
    };
    openSound();

    // The car selection's 3D car: the arcade car camera and floor of the car page (game/arcade/arcade_car_page.h: 0x8001E520 /
    // 0x8001E5BC / 0x80015F24 with the race overlay's model-view projection, menu_car.h) through gt2view MenuCarView.
    auto carView = std::make_unique<gt2view::MenuCarView>(*renderer, vol);
    carView->SetFrameSize(TitleAssets::kScreenWidth, TitleAssets::kScreenHeight);
    carView->LoadMenuReflection();
    uint32_t shownCar = 0;

    shell::TitleMenu title(titleAssets.ovl1, titleAssets.language);
    // Dev aid: GT2_ARCADE_POST_TEST=1 runs only the post-race views (RESULTS -> the menu) with the capture run's result
    // (work/re/arcade_results: 1st, 2:56.312, laps 1:34.417 / 1:21.895, the first road course, the Corvette) - automated comparisons.
    if (const char* test = std::getenv("GT2_ARCADE_POST_TEST"); test && *test && postAssets) {
        ArcadePostRaceInput in;
        in.finished = true;
        in.place = 1;
        in.totalTime = 176312;
        in.fastestLap = 81895;
        in.laps = {94417, 81895};
        in.firstLap = 1;
        in.course = menuAssets.data.courses[0].front().display; // the first road course (the capture's)
        in.carId = PackCarId("ccrcn");
        in.vsync = 0;
        const ArcadePostRaceChoice c = RunArcadePostRace(window, *postAssets, vol, in, true, options.squarePixels, nullptr);
        std::printf("post-race test: choice %d\n", int(c));
        return 0;
    }
    // Dev aid: GT2_ARCADE_BATTLE_POST_TEST=<ram.bin> runs only the mode 0 post-race views (2P RESULTS -> 2PLAYER BATTLE) on the
    // inputs read from a RAM image of the original taken during them (a gt2play --prims capture); GT2_ARCADE_POST_COMPARE
    // compares frames.
    if (const char* test = std::getenv("GT2_ARCADE_BATTLE_POST_TEST"); test && *test && postAssets) {
        std::ifstream f(test, std::ios::binary);
        const std::vector<uint8_t> ram((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const ArcadePostRaceInput in = BattlePostRaceInputFromRam(disc, vol, ram);
        const ArcadePostRaceChoice c = RunArcadePostRace(window, *postAssets, vol, in, true, options.squarePixels, nullptr);
        std::printf("battle post-race test: choice %d\n", int(c));
        return 0;
    }
    // Dev aid: GT2_ARCADE_SESSION_TEST=<ram.bin> runs only the mode 6 post-race views (arcade_post_race.h) on the inputs read from a
    // RAM image of the original taken during them (a gt2play --prims capture); GT2_ARCADE_SESSION_COMPARE compares frames.
    if (const char* test = std::getenv("GT2_ARCADE_SESSION_TEST"); test && *test && postAssets) {
        std::ifstream f(test, std::ios::binary);
        const std::vector<uint8_t> ram((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        uint32_t courseId = 0, carId = 0;
        const screens::SessionInput in = SessionInputFromRam(disc, ram, courseId, carId);
        const std::string courseFile = CourseNameOfId(vol, courseId);
        std::printf("session test: %s (%s), %d lap(s) kept, new record %u, record %s by \"%s\", ghost option %u\n", in.course.c_str(), courseFile.c_str(),
                    int(in.results.count), unsigned(in.newRecord), FormatMs(in.courseRecord.time).c_str(), in.recordName.c_str(), unsigned(in.ghostOption));
        const RaceMenuAssets assets = WithCoursePicture(*postAssets, vol, courseFile);
        const char* raceRun = std::getenv("GT2_ARCADE_SESSION_RACERUN");
        const ArcadeSessionOutcome o = RunArcadeSessionViews(window, assets, vol, in, !(raceRun && *raceRun == '0'), carId, 0, options.squarePixels, true, nullptr);
        std::printf("session test: choice %d\n", int(o.choice));
        return 0;
    }
    arcade::ArcadeMenus menus(menuAssets, arcadeData, cars, careerBlock);
    menus.SetCards({shell::CardSlot{options.card1Path}, shell::CardSlot{options.card2Path}}); // LOAD GUEST GARAGE
    // Dev aid: GT2_ARCADE_RACE_SETUP=<file> (a RaceCaptureSetup: gt2verify --race-capture's <capture>.setup or --arcade-setup-out)
    // runs that race block's race and what follows it (the mode 6 session or the road race session) on the new game's career,
    // without the menus - automated runs of the arcade loop.
    if (const char* setupFile = std::getenv("GT2_ARCADE_RACE_SETUP"); setupFile && *setupFile) {
        RaceCaptureSetup cs;
        std::FILE* f = std::fopen(setupFile, "rb");
        if (!f || std::fread(&cs, sizeof cs, 1, f) != 1 || cs.magic != kRaceCaptureSetupMagic) {
            if (f) std::fclose(f);
            throw std::runtime_error(std::string("arcade: cannot read the race setup ") + setupFile);
        }
        std::fclose(f);
        // The career of the first-boot load (0x8004AE28: "BASCUS-94455GAME" of card 1) when card 1 holds one, as the original's session
        // had it (e.g. its home garage for a garage car's race); else the new game's.
        if (!options.card1Path.empty() && std::filesystem::exists(options.card1Path)) {
            try {
                const career::CareerSave save = career::LoadCareer(options.card1Path);
                menus.SetCareer(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&save.state), sizeof save.state));
                std::printf("arcade: race setup on the career of %s (CRC %s)\n", options.card1Path.c_str(), save.CrcOk() ? "ok" : "MISMATCH");
            } catch (const std::exception& e) {
                std::printf("arcade: race setup on the new game's career (%s)\n", e.what());
            }
        }
        arcade::ArcadeRaceSetup setup;
        setup.raceBlock = cs.raceBlock;
        setup.settings = cs.settings;
        uint32_t courseId = 0;
        std::memcpy(&courseId, setup.raceBlock.data() + 0x40, 4);
        const std::string course = CourseNameOfId(vol, courseId);
        std::printf("arcade: race setup %s (mode %u, course %s)\n", setupFile, setup.raceBlock[0x0A], course.c_str());
        if (setup.raceBlock[0x0A] == 6) RunArcadeGhostSession(window, disc, vol, titleScreens.RaceOptions(options), setup, course, menus, postAssets ? &*postAssets : nullptr);
        else if (setup.raceBlock[0x0A] == 0) RunArcadeBattleSession(window, disc, vol, titleScreens.RaceOptions(options), setup, course, menus, postAssets ? &*postAssets : nullptr);
        else RunArcadeRaceSession(window, disc, vol, titleScreens.RaceOptions(options), setup, course, menus, postAssets ? &*postAssets : nullptr);
        return 0;
    }
    // The movies (movie_player.h): the course previews of COURSE SELECTION, the intro at boot / every fourth attract cycle, the
    // endings of ENDING CREDITS. Automated runs (a key script, shots, --arcade-frames) skip the boot intro unless --movies.
    std::unique_ptr<MovieLibrary> movies;
    try {
        // --arcade-compare runs compare with captures taken before the interpreter had an MDEC (an empty preview sprite):
        // no movies there unless --movies.
        if (!options.noMovies && (options.compare.empty() || options.forceMovies)) movies = MovieLibrary::Load(disc, options.discPath);
    } catch (const std::exception& e) {
        std::printf("arcade: movies disabled (%s)\n", e.what());
    }
    std::unique_ptr<CoursePreview> coursePreview;
    if (movies) {
        coursePreview = std::make_unique<CoursePreview>(disc, *movies);
        menus.SetCourseMovieSource(coursePreview.get());
    }
    auto playMovie = [&](int movie) { // member 5, then member 1 (the title); false = the window closed
        if (!movies) return true;
        sfx.reset();
        const MovieResult r = PlayMovie(window, disc, *movies, movie, !options.noSound);
        resetView();
        openSound();
        return r != MovieResult::kClosed;
    };
    const bool automated = !options.globalScript.empty() || !options.shots.empty() || !options.anyShots.empty() || options.quitAfter > 0 || options.noRace;
    if (!options.noMovies && (options.forceMovies || !automated)) {
        sfx.reset();
        if (!PlayBootScreens(window, disc, !options.noSound)) return 0;
        resetView();
        openSound();
    }
    if (movies && (options.forceMovies || !automated) && !playMovie(kMovieIntro)) return 0; // EXE main 0x8005D670: member 5 first
    std::unique_ptr<TitleAttractDemos> attract; // the attract cycle (0x801EF030, 0x801EF02E; title_attract.h)
    uint32_t menusVram = menus.VramVersion();
    std::string noticeLine;
    int lastShot = 0;
    for (const auto& s : options.shots) lastShot = std::max(lastShot, s.first);
    uint32_t previousHeld = 0, previousHeld2 = 0;
    int repeatTimer = 0, repeatTimer2 = 0, races = 0;
    const auto fieldTime = std::chrono::nanoseconds(16'683'333);
    for (;;) {
        if (!window.BeginFrame()) break;
        const int field = window.Field();
        uint32_t held = 0;
        for (int k : {gt2::keys::kUp, gt2::keys::kDown, gt2::keys::kLeft, gt2::keys::kRight, gt2::keys::kReturn, gt2::keys::kSpace, gt2::keys::kBack, gt2::keys::kEscape, int('S'), int('Q'), int('W'), gt2::keys::kDelete})
            if (window.Held(k)) held |= KeyBit(k);
        uint32_t keyPresses = 0;
        for (int k : window.PressedKeys()) keyPresses |= KeyBit(k);
        MenuListPad pad;
        pad.held = held;
        pad.pressed = (held & ~previousHeld) | keyPresses;
        pad.released = previousHeld & ~held;
        const uint32_t dirs = held & (pad::kUp | pad::kDown | pad::kLeft | pad::kRight);
        if (dirs && dirs == (previousHeld & dirs)) { // auto-repeat (ours, as the other gt2game menus: after 20 fields, every 5)
            if (++repeatTimer >= 20 && (repeatTimer - 20) % 5 == 0) pad.repeat = dirs;
        } else {
            repeatTimer = 0;
        }
        previousHeld = held;
        MenuListPad pad2; // the second controller (the 2PLAYER BATTLE page's player 2)
        pad2.held = GenericButtons(window.Pad2().buttons);
        pad2.pressed = pad2.held & ~previousHeld2;
        pad2.released = previousHeld2 & ~pad2.held;
        const uint32_t dirs2 = pad2.held & (pad::kUp | pad::kDown | pad::kLeft | pad::kRight);
        if (dirs2 && dirs2 == (previousHeld2 & dirs2)) {
            if (++repeatTimer2 >= 20 && (repeatTimer2 - 20) % 5 == 0) pad2.repeat = dirs2;
        } else {
            repeatTimer2 = 0;
        }
        previousHeld2 = pad2.held;

        if (sfx) sfx->Frame(); // the sequencer tick and the mixer's poll, once per field
        std::vector<MenuPrim> prims;
        const MenuVram* vram = &titleAssets.vram;
        switch (screen) {
        case Screen::kTitle: {
            use(titleAssets.vram);
            const int state = title.Update(&pad, held);
            if (sfx)
                for (int id : title.sounds) sfx->Sound(id);
            if (state == 4) {
                std::printf("arcade title f%d: result %d\n", field, title.result);
                if (title.result == shell::kStartGame) { // ovl1 -> member 2 (0x8005D9AC(2) at 0x80011434)
                    menus.SetCareer(titleScreens.Career()); // the career the title's options / Load left
                    // DEV AID (oracle comparisons with gt2play --poke 0x801C9340 + offset): GT2_ARCADE_CAREER_POKE="off=word,..." (hex)
                    if (const char* pk = std::getenv("GT2_ARCADE_CAREER_POKE"))
                        for (const char* p = pk; *p;) {
                            char* end = nullptr;
                            const size_t off = std::strtoul(p, &end, 16);
                            const uint32_t word = uint32_t(std::strtoul(end + 1, &end, 16));
                            if (off + 4 <= menus.MutableCareer().size()) std::memcpy(menus.MutableCareer().data() + off, &word, 4);
                            p = *end ? end + 1 : end;
                        }
                    menus.Reset(races > 0);
                    if (sfx) sfx->StartArcadeMusic(titleScreens.Career()[0xB3]); // the root view's first init (0x8001D584)
                    screen = Screen::kMenus;
                } else if (titleScreens.Open(title.result)) { // Replay Theater / Options / Save / Load / Data Transfer (0x80020CB8 cases 1..5)
                    screen = Screen::kTitleScreen;
                } else if (title.result == shell::kAttractDemo) {
                    // member 1 0x8001156C (title_attract.h): 0x801EF030 + 1, 4 -> 0 -> member 5 (the intro again), else the next
                    // replay of the demo file (0x80020A98) through 0x80010EDC and the race overlay with argument 1; then the title.
                    title.Reset();
                    screen = Screen::kTitle;
                    try {
                        if (!attract) attract = std::make_unique<TitleAttractDemos>(disc, vol, true, titleAssets.language);
                        ReplayPayload payload;
                        std::string demoTitle;
                        if (attract->Next(payload, demoTitle) == TitleAttractDemos::Kind::kMovie) {
                            if (!playMovie(kMovieIntro)) break;
                        } else {
                            std::printf("arcade title f%d: attract demo '%s' of %s\n", field, demoTitle.c_str(), attract->Path().c_str());
                            sfx.reset();
                            const ArcadeModeOptions ro = titleScreens.RaceOptions(options);
                            PlayTitleReplay(window, disc, vol, payload, demoTitle, ro.race, ro.view, false);
                            if (window.Closed()) break;
                            resetView();
                            openSound();
                        }
                    } catch (const std::exception& e) {
                        std::printf("arcade title: attract demo: %s\n", e.what());
                    }
                } else {
                    noticeLine = "Not available yet on the arcade disc";
                    screen = Screen::kNotice;
                }
            }
            prims = shell::BuildTitleFrame(title);
            break;
        }
        case Screen::kNotice:
            use(titleAssets.vram);
            if (pad.pressed & (pad::kCross | pad::kCircle | pad::kTriangle | pad::kSquare)) {
                title.Reset();
                screen = Screen::kTitle;
            }
            prims = shell::NoticeFrame(titleAssets, "ARCADE MODE DISC", 0x0010A0FFu, noticeLine);
            break;
        case Screen::kTitleScreen: // the options / card screens, the first-boot view (the title's VRAM)
            use(titleScreens.Vram());
            vram = &titleScreens.Vram();
            if (!titleScreens.Update(pad, window)) {
                title.Reset();
                screen = Screen::kTitle;
                prims = shell::BuildTitleFrame(title);
            } else {
                prims = titleScreens.Frame();
            }
            if (sfx)
                for (int id : titleScreens.sounds) sfx->Sound(id);
            {
                // Replay Theater: a loaded / chosen replay plays as the race overlay with argument 1 (0x8005D9EC(0, 0x80011F64, 1)
                // after 0x80010EDC), then member 1 starts in the theater again (0x801EF022 == 2).
                ReplayPayload payload;
                std::string replayTitle;
                if (titleScreens.TakeReplay(payload, replayTitle)) {
                    std::printf("arcade replay theater f%d: playing '%s'\n", field, replayTitle.c_str());
                    sfx.reset();
                    const ArcadeModeOptions ro = titleScreens.RaceOptions(options);
                    try {
                        PlayTitleReplay(window, disc, vol, payload, replayTitle, ro.race, ro.view, false);
                    } catch (const std::exception& e) {
                        std::printf("arcade replay theater: %s\n", e.what());
                    }
                    if (window.Closed()) break;
                    resetView();
                    openSound();
                    titleScreens.ReplayPlayed();
                    prims = titleScreens.Frame();
                }
            }
            break;
        case Screen::kMenus: {
            const arcade::ArcadeMenus::Outcome outcome = menus.Update(pad, &pad2);
            if (menus.VramVersion() != menusVram) { // the car page uploaded a name logo (arc_carlogo) into the menus' VRAM
                menusVram = menus.VramVersion();
                uploaded = nullptr;
            }
            use(menus.Vram());
            vram = &menus.Vram();
            if (sfx)
                for (int id : menus.sounds) sfx->Sound(id);
            prims = menus.Frame();
            if (outcome == arcade::ArcadeMenus::kEnding) { // result 5 / 6 -> member 5 0x800114E0(1 / 0) -> the title (member 1)
                if (sfx) sfx->StopMusic();
                if (!playMovie(menus.EndingRow() == 0 ? kMovieEndingA : kMovieEndingB)) break;
                titleScreens.SetCareer(menus.Career());
                title.Reset();
                screen = Screen::kTitle;
                prims.clear();
            } else if (outcome == arcade::ArcadeMenus::kTitle) { // result 4 -> 0x801EF024 = 0 -> member 1
                if (sfx) sfx->StopMusic();
                titleScreens.SetCareer(menus.Career()); // the race results' course flags go back to the title's Save
                title.Reset();
                screen = Screen::kTitle;
            } else if (outcome == arcade::ArcadeMenus::kRace) {
                // 0x80011868: the two numbers, then 0x80010C84 (ported, gt2verify ArcadeBuild) -> ovl3 -> ovl0.
                const uint32_t vsync = options.vsync >= 0 ? uint32_t(options.vsync) : uint32_t(gt2::os::TickCountMs());
                const std::array<uint32_t, 2> numbers = arcade::MenuEntryNumbers(vsync);
                arcade::ArcadeRaceSetup setup = menus.BuildRace(setupData, numbers[0], numbers[1], vsync); // + member 3 for a garage car
                if (const char* dump = std::getenv("GT2_ARCADE_BUILD_DUMP")) { // dev aid: race block, settings, record 0, home garage
                    if (std::FILE* f = std::fopen(dump, "wb")) {
                        std::fwrite(setup.raceBlock.data(), 1, setup.raceBlock.size(), f);
                        std::fwrite(setup.settings.data(), 1, setup.settings.size(), f);
                        std::fwrite(setup.records.data(), sizeof(setup.records[0]), setup.records.empty() ? 0 : 1, f);
                        std::fwrite(menus.GarageBlock(0).data(), 1, menus.GarageBlock(0).size(), f);
                        std::fclose(f);
                    }
                }
                const uint8_t* sel = setup.menuRegion.data() + (arcade::kSelectionAddress - arcade::kMenuRegionAddress);
                std::string course;
                uint32_t courseId = 0;
                std::memcpy(&courseId, sel + arcade::Sel::kCourseId, 4);
                for (const auto& list : menuAssets.data.courses)
                    for (const ArcadeCourse& c : list)
                        if (CourseFileId(c.file) == courseId) course = c.file;
                std::printf("arcade race: event %s, course %s, %u lap(s), vsync %u, cars", reinterpret_cast<const char*>(setup.raceBlock.data() + 0x10), course.c_str(),
                            setup.raceBlock[0x0F], vsync);
                for (size_t i = 0; i < setup.raceBlock[0x5A]; i++) {
                    uint32_t id;
                    std::memcpy(&id, setup.raceBlock.data() + 0x5C + i * 0xD0, 4);
                    std::printf(" %s", UnpackCarId(id).c_str());
                }
                std::printf(" (tyre table %d, transmission %u)\n", sel[arcade::Sel::kTyres] == 1 ? 33 : 32, sel[arcade::Sel::kTransmission]);
                if (!options.setupOut.empty()) {
                    RaceCaptureSetup out;
                    out.raceBlock = setup.raceBlock;
                    out.settings = setup.settings;
                    if (std::FILE* f = std::fopen(options.setupOut.c_str(), "wb")) {
                        std::fwrite(&out, sizeof out, 1, f);
                        std::fclose(f);
                        std::printf("arcade: race setup written to %s\n", options.setupOut.c_str());
                    }
                }
                if (options.noRace) return 0;
                if (course.empty()) throw std::runtime_error("arcade: the chosen course id is in no course list");
                sfx.reset();
                // Rally / Time Trial (game mode 6: the ghost of the best lap, race_shell.h GhostSession) or the Road Race.
                const bool open = setup.raceBlock[0x0A] == 6
                                      ? RunArcadeGhostSession(window, disc, vol, titleScreens.RaceOptions(options), setup, course, menus, postAssets ? &*postAssets : nullptr)
                                  : setup.raceBlock[0x0A] == 0 ? RunArcadeBattleSession(window, disc, vol, titleScreens.RaceOptions(options), setup, course, menus, postAssets ? &*postAssets : nullptr)
                                                               : RunArcadeRaceSession(window, disc, vol, titleScreens.RaceOptions(options), setup, course, menus, postAssets ? &*postAssets : nullptr);
                races++;
                if (!open || window.Closed()) break;
                resetView();
                carView = std::make_unique<gt2view::MenuCarView>(*renderer, vol);
                carView->SetFrameSize(TitleAssets::kScreenWidth, TitleAssets::kScreenHeight);
                carView->LoadMenuReflection();
                shownCar = 0;
                openSound();
                menus.Reset(true); // ovl0 -> member 2 again: the root view, the cursors kept
                if (sfx) sfx->StartArcadeMusic(menus.Career()[0xB3]);
            }
            break;
        }
        }

        std::optional<menu::MenuCarProjection> carProjection;
        std::array<std::optional<menu::MenuCarProjection>, 2> battleCars; // the 2PLAYER BATTLE page's two cars (arcade_battle.h)
        const arcade::ArcadeBattlePage* battle = screen == Screen::kMenus ? menus.BattleShown() : nullptr;
        if (battle && (battle->CarShown(0) || battle->CarShown(1))) {
            carView->UsePair(battle->CarShown(0) ? battle->CarId(0) : 0u, battle->CarShown(1) ? battle->CarId(1) : 0u);
            shownCar = 0;
            size_t at = 1;
            for (int p = 0; p < 2; p++) {
                if (!battle->CarShown(p)) continue;
                const std::vector<MenuPrim> floor = battle->Floor(p); // the environments are drawn before the view's OT
                prims.insert(prims.begin() + std::ptrdiff_t(at), floor.begin(), floor.end());
                at += floor.size();
                battleCars[size_t(p)] = battle->Projection(p, true);
            }
        } else if (screen == Screen::kMenus && menus.CarShown()) {
            if (menus.CarId() != shownCar) {
                shownCar = menus.CarId();
                carView->Use(shownCar);
            }
            const std::vector<MenuPrim> floor = menus.CarPage().Floor();
            prims.insert(prims.begin() + 1, floor.begin(), floor.end()); // the car environment is drawn before the view's OT
            carProjection = menus.CarPage().Projection(true);
        } else {
            shownCar = 0;
        }
        std::vector<gt2view::DrawItem> items;
        view->Build(prims, TitleAssets::kScreenWidth, renderer->AspectRatio(), items, options.squarePixels);
        for (int p = 0; p < 2; p++) {
            if (!battleCars[size_t(p)]) continue;
            const size_t before = items.size();
            carView->AppendPair(items, p, *battleCars[size_t(p)], battle->Paint(p), renderer->AspectRatio(), options.squarePixels);
            if (items.size() > before) items[before].clearDepth = 1;
        }
        if (carProjection) {
            const size_t before = items.size();
            carView->Append(items, *carProjection, menus.CarPaint(), renderer->AspectRatio(), options.squarePixels);
            // TitleView's 2D quads write depth 1 (vk_scene_renderer.h): clear the depth in the car's viewport first.
            if (items.size() > before) items[before].clearDepth = 1;
            if (std::getenv("GT2_ARCADE_TRACE")) std::printf("arcade f%d: car %s, %zu car item(s)\n", field, UnpackCarId(shownCar).c_str(), items.size() - before);
        }
        std::string shotPath, comparePath;
        for (size_t k = 0; k < options.shots.size(); k++)
            if (options.shots[k].first == field) {
                shotPath = options.shots[k].second;
                if (k < options.compare.size()) comparePath = options.compare[k];
            }
        window.EndFrame(items, shotPath, fieldTime);
        if (!shotPath.empty()) {
            std::printf("arcade f%d -> %s\n", field, shotPath.c_str());
            const MenuCanvas canvas = RenderInterpreter(*vram, prims);
            if (std::getenv("GT2_ARCADE_PRIMS")) { // dev aid: the frame's primitives in the gt2play --prims format (arcade_menu_check.h)
                std::filesystem::path listing = shotPath;
                listing.replace_filename(listing.stem().string() + "_prims.txt");
                WriteMenuPrimListing(listing.string(), prims);
            }
            std::filesystem::path canvasPath = shotPath;
            canvasPath.replace_filename(canvasPath.stem().string() + "_canvas.png");
            WritePngRgba(canvasPath.string(), MenuCanvas::kWidth, MenuCanvas::kHeight, canvas.Rgba());
            if (!comparePath.empty()) {
                std::filesystem::path side = shotPath;
                side.replace_filename(side.stem().string() + "_canvas_side.png");
                std::printf("canvas (interpreter rules) ");
                CompareWithCapture(canvas.Rgba(), MenuCanvas::kWidth, comparePath, side.string());
                const PngImage ours = ReadPngFile(shotPath);
                if (ours.width == TitleAssets::kScreenWidth && ours.height == TitleAssets::kScreenHeight) {
                    std::filesystem::path vside = shotPath;
                    vside.replace_filename(vside.stem().string() + "_vulkan_side.png");
                    std::printf("vulkan ");
                    CompareWithCapture(ours.rgba, ours.width, comparePath, vside.string());
                }
            }
            if (field >= lastShot && options.quitAfter == 0) break;
        }
        if (options.quitAfter > 0 && field + 1 >= options.quitAfter) break;
        const int lastAny = window.LastShotField();
        if (lastAny >= 0 && options.quitAfter == 0 && options.shots.empty() && field >= lastAny && field >= window.ScriptEnd()) break;
    }
    sfx.reset();
    if (audioEvents) std::fclose(audioEvents);
    return 0;
}

} // namespace gt2game
