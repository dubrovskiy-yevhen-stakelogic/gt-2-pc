#pragma once
// Replays of game mode 6 races (Time Trial / Rally) from a replay file: the title's Replay Theater ("Demo 02" / "Demo 05" of the
// demo files, a Time Trial saved with "Save Replay ..." of the TIME TRIAL menu). US Simulation v1.2 / US Arcade v1.1 (the race
// overlay's mode 6 code is the same in both builds, arcade_disc.md 17.9 / 17.10).
//
// The record (0x80069948 mode 6 layout, gt2formats/replay_card.h ReplayPayload): the race block 0x801D585C, the results record,
// the s8 lap count -> s16 0x801D5F84 and per lap the 0xE0-byte head and the stream object of the lap buffer 0x801D5F88 + k * 0x10FC
// (0x80069AC4 unpacks them there: player 1's lap ring in recording order, as 0x800153B8 / 0x80012570 left it before the save).
// The title (0x80010EDC) then runs the race overlay with argument 1: 0x800A951C set, the race block's one entry (0x8001503C),
// player 1 on pad slot 2 playing the ring's laps from lap buffer 0 (0x80013EF0 with 0x800132D0 at each lap's end, 0x8003F990 the
// lap's start state; 0x800A8D68 after the last lap) - the replay the arcade loop plays after a mode 6 race (race_shell.h
// GhostReplayInput / GhostReplayLapStart, arcade_race.h ReplayRaceData).
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "game/sim/race_shell.h"
#include "gt2formats/replay_card.h"
#include "race_common.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2game {

// Entry `index` of a replay file / memory card image / demo file when it is a game mode 6 record (its payload), else nothing.
std::optional<gt2::ReplayPayload> GhostRecordOf(std::span<const uint8_t> bytes, int index);

// The race of a mode 6 record: the race block's race (LoadRaceBlock with the settings block the title builds for it: the event
// row of the race block's event name in carparam/usa_gtmode_race.dat + 0x44 when present, else zero), cut to player 1 with
// 0x800A951C set (ReplayRaceData), and `session` holding the record's lap ring (the lap count, lap buffers 0 .. count - 1);
// data.shell.ghost points at `session`. Returns the car count (1).
size_t LoadGhostRecordRace(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const gt2::ReplayPayload& record, RaceData& data, RaceOptions& options,
                           gt2::sim::GhostSession& session);

// --replay <record> --frames-compare <capture>: the replay against a gt2verify --race-capture of the original playing the same
// record (the title's theater): per frame car 0's simulated record (FramesCompare's rule), the ring's counters and lap buffers,
// the playback part of the ghost block (<capture>.ghost) and the race clock. Returns the frames that differ.
int FramesCompareGhostRecord(const RaceData& data, const RaceOptions& options, const std::string& capturePath, int maxFrames);

} // namespace gt2game
