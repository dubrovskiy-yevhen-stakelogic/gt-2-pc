#pragma once
// The 2 player Battle's race in the game window (game mode 0 of the US Arcade v1.1 disc; docs/research/arcade_disc.md section 19):
// the ported race (sim::RaceSim with the shell's mode-0 rules: the Slow Car Boost catch-up 0x80042038, the players' gap displays
// 0x800155C4, results records of both players) driven by two controllers, drawn as the original's split screen:
//   - two camera objects (view + 0xC4 + player * 0x110; 0x80010000(camera, player), + 0x103 = 1: half-height views - the
//     projection's window (-160, 160) x (66, -66) of 0x80010088 and the split chase offsets 0x8002F360), updated after every
//     step (0x800100F4 for both players in mode 0);
//   - the view's split flag (view + 0x2EA) follows the camera objects' + 0x103 every frame (Sim 0x800292A0, SplitViewFrame):
//     split, 0x800297F4 draws the two players' views (0x8002975C with the rectangles 0x8002F1FC = (0, 0, 320, 120) and (0, 120,
//     320, 120): player 1's view on the top half, player 2's on the bottom half of the window, each scissored to its half with
//     the camera's clip matrix mapped into it; no rear-view mirror, 0x800294D4: not while split), else 0x8002972C draws player
//     1's camera over the whole frame (a replay's Triangle, 0x800109FC, turns the split off: the trackside camera follows the
//     leader);
//   - the 2P HUD 0x8002E908 for car 0 then car 1 (gt2view/hud.h Hud::Build2P), the race-end display 0x8002B170 of mode 0 ("PLAYER 1
//     / 2 WINS!!!", race_overlay_screens.h), the pause menu 0x80029E80 (player 1's Start: the Arcade race frame reads pad 1 only).
// Controls: player 1 = port 1 (the controller in use) + the keyboard of the single race (arrows, Space, R, Q / A, C, V, Esc);
// player 2 = port 2 (a second controller, --fake-pad2) + player 2's keys (game_window.h kPlayer2Keys), both through the original's
// pad chain (0x8007FC30 reader, 0x80014BB4 logical pad with the career's pad blocks + 0x0A / + 0x5C, 0x80013C90 frames).
// Replays: both players' input streams are recorded (0x801D5F84 / 0x801DA49C, 0x80013EF0 / 0x80014030 -> 0x80013C90) and a
// replay plays both back with 0x800A951C set (the replay cameras 0x800109FC for both camera objects); the replay ends when
// either stream ran out (0x800A8D68).
// Sound: split, each player's car against its own camera (0x80014ED0); a full view, every car against player 1's camera
// (0x80014E6C) - RaceAudio::StepTwoPlayer, after the frame's camera update on the view values of the previous frame's draw.
// Presentation: frame-locked (one step per two fields) or, with the display frame rate (docs/formats/modern_graphics.md),
// every display refresh draws both views between the last two steps (frame_interp.h: the cars, their wheels, both cameras,
// the smoke, the HUD gauges).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "race_common.h"
#include "race_view.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
struct ReplayFile;
} // namespace gt2

namespace gt2game {

class GameWindow;
class Panels;

struct SplitRaceConfig {
    RaceViewConfig view;                  // sound / music / HUD / particles / detail / camera / shot / graphics options of the single race
    std::vector<uint8_t> padBlock1, padBlock2; // the career's pad blocks (+ 0x0A / + 0x5C, 0x52 bytes); empty = the new game's
    std::array<std::string, 2> names;     // the race slots' names (race block + 0x5C + car * 0xD0 + 0x90): the race-end display's rows
    // A two-player replay (ReplayFile::stream / stream2): the race again from its start with both streams played back.
    const gt2::ReplayFile* replay = nullptr;
    bool replayEndLeaves = true;          // the stream's end (0x800A8D68) ends the view (the arcade loop's replay, the theater)
    bool startLeaves = false;             // a replay from the title (argument 1, 0x800A9500): Start / Esc leave at once (no pause menu)
};

struct SplitRaceResult {
    RaceExit exit = RaceExit::kClosed;
    int steps = 0;
    bool finished = false;                        // the race task ended after a finish (0x8002A700)
    std::array<gt2::sim::PlayerResults, 2> results{}; // 0x801D5E88 / 0x801DA3A0 when the race ended
    std::array<int32_t, 2> places{};              // the results records' places (s16 + 0; 0 = not finished)
    std::array<std::vector<uint8_t>, 2> streams;  // the players' stream objects as recorded (the pending run flushed, 0x800167D0)
};

// Runs the race of `data` (two player entries of kinds 3 / 4: arcade_race.h LoadRaceBlock of a mode 0 race block) until the race
// task ends, the pause menu's Exit or the window closes; with `config.replay` the replay of such a race.
SplitRaceResult RunSplitRace(GameWindow& window, Panels* panels, const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, RaceData& data, const RaceOptions& options,
                             const SplitRaceConfig& config);

// --frames-compare of a two-player replay (the original playing it, gt2verify --race-capture): the race of `data` with both
// streams of `replay` played back and 0x800A951C set; every frame the car bodies byte for byte and player 1's stream object
// against the capture (race_common.h FramesCompare's comparison). Returns the number of differing frames.
int FramesCompareSplitReplay(const RaceData& data, const RaceOptions& options, const std::string& capturePath, const gt2::ReplayFile& replay,
                             const std::array<uint16_t, 16>& pedalTable, int maxFrames);

// Dev oracle of the split screen's sound (--sound-compare with a game mode 0 capture of gt2verify --race-capture, the capture aid
// driving both cars): the race and both cameras natively, the sound pass of the view (0x80014ED0 split / 0x80014E6C) after every
// step and camera update, and every frame both cars' sound objects (car + 0xAA4: the logic's fields, not the voice handles / SPU addresses) against
// the capture's car records. Returns the number of differing frames.
int SoundCompareSplit(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const RaceData& data, const RaceOptions& options, const std::string& capturePath,
                      int maxFrames);

} // namespace gt2game
