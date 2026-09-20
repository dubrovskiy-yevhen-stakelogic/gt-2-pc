#pragma once
// The arcade races of the US Arcade v1.1 disc (docs/research/arcade_disc.md section 11): the race the arcade menus
// (GT2.OVL member 2) hand to the race launcher (member 3, 0x800121DC) - an event of carparam/usa_arcade_data.dat chosen by
// level and car class, the player's car from the player car table, the opponents drawn from the event's slots - and the
// race of a frame capture (gt2verify --race-capture writes <capture>.setup with the original's race block).
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "race_common.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2game {

struct ArcadeRaceOptions {
    int level = 0;                  // --arcade-level: 0 Easy (the menu's default cursor; the dump's event A0A), 1 Normal, 2 Difficult
    char carClass = 'A';            // --arcade-class A / B / C / S: the event's car class
    std::vector<uint32_t> opponents; // --opponents n,n,..: opponent rows (table 31, number = row + 1) instead of the draw
    int tyreTable = 32;             // --arcade-tyres 32 | 33: the player car table (they differ in the tyre rows of 37 cars)
    uint32_t seed = 0;              // the draw's seed (the original's: the VSync counter at the race build, 0x8007D14C(0))
};

// The race of the arcade menus on `trackName` with the player's `carId` (a car of the player car table): game mode 4
// (Road Race; race block bytes of the arcade race dump: 0x801D585D / 0x801D5860 / 0x801D5865 = 0, frame-rate mode 2), the
// event's settings block (AI corner grip by level and class, catch-up tuning), the player on the last grid slot and the
// opponents in front (entry i on slot count - 1 - i: 0x800121DC). Fills `data` (track, constants, cars) and `options`.
void BuildArcadeRace(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::string& trackName, const std::string& carId, size_t carCount,
                     const ArcadeRaceOptions& arcade, RaceOptions& options, RaceData& data);

// The race of a capture's setup file (gt2formats/race_capture.h RaceCaptureSetup): the race block's shell bytes, laps,
// countdown, the entries' configurations (built with the arcade car tables on the arcade disc, the GT-mode tables
// otherwise), grid slots, kinds and transmissions, and the settings block. Returns the car count.
size_t LoadCaptureRace(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::string& setupPath, const std::string& trackName, RaceOptions& options,
                       RaceData& data);
// The same from a race block (0x58C bytes, the layout of 0x801D585C) and its settings block (0x40 bytes): the race the arcade
// menus built (game/arcade/arcade_setup.h) or a capture's setup. `arcadeTables`: the car tables (usa_arcade_data.dat or the
// GT-mode data) when not the disc's own - the title's replays of arcade races on the Simulation disc (race block + 9 == 0:
// 0x80010EDC loads the arcade data, 0x80076E04, before 0x800771AC builds the cars).
size_t LoadRaceBlock(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, std::span<const uint8_t, 0x58C> raceBlock, std::span<const uint8_t, 0x40> eventSettings,
                     const std::string& trackName, RaceOptions& options, RaceData& data, std::optional<bool> arcadeTables = std::nullopt);

// --frames-compare of a game mode 6 capture (Time Trial / Rally): FramesCompare's car records (the player and the ghost) plus,
// per frame, the ghost state against <capture>.ghost - player 1's lap ring 0x801D5F84 (heads, stream headers and the coded
// bytes written), the reference lap 0x801DA4A0, the snapshots / playback block 0x800A8D70 and the overlay's ghost flags.
// A second race of the capture with the replay flag (the replay the arcade loop runs after the pause's Exit) is compared too:
// the live race's end (RaceSim::EndGhostRace), then the replay on the same ghost session (ReplayRaceData, 17.10).
// Returns the number of frames that differ.
// The data of the replay of a mode 6 race: player 1 alone (the per-car vectors cut to one), 0x800A951C set.
RaceData ReplayRaceData(const RaceData& data);
int FramesCompareGhost(const RaceData& data, size_t carCount, const RaceOptions& options, const std::string& capturePath, const std::array<uint16_t, 16>& pedalTable,
                       int maxFrames);

} // namespace gt2game
