// Replays of game mode 6 records (ghost_replay.h).
#include "ghost_replay.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "arcade_race.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/race_capture.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

using namespace gt2;

namespace gt2game {

std::optional<ReplayPayload> GhostRecordOf(std::span<const uint8_t> bytes, int index) {
    std::vector<uint8_t> file;
    if (bytes.size() == 128 * 1024 && bytes[0] == 'M' && bytes[1] == 'C') file = ReadReplayCardFile(bytes);
    else if (bytes.size() >= kReplayDataStart && bytes[0] == 'S' && bytes[1] == 'C') file.assign(bytes.begin(), bytes.end());
    if (file.size() < kReplayDataStart) return std::nullopt;
    const ReplayCardFile f = ReplayCardFile::FromBytes(file);
    if (!f.Valid() || index < 0 || index >= f.Count() || !f.EntryCrcOk(index)) return std::nullopt;
    std::vector<uint8_t> data = f.EntryData(index);
    data.resize(size_t(f.Entry(index).size));
    ReplayPayload p = UnpackReplayPayload(data); // 0x80069AC4
    if (p.GameMode() != 6) return std::nullopt;
    return p;
}

size_t LoadGhostRecordRace(const DiscImage& disc, const GtfsVolume& vol, const ReplayPayload& record, RaceData& data, RaceOptions& options,
                           sim::GhostSession& session) {
    if (record.GameMode() != 6) throw std::runtime_error("ghost replay: not a game mode 6 record");
    // The title's 0x80010EDC by race block + 9: 0 (an arcade race) loads usa_arcade_data.dat (0x80076E04) and 0x80010C50 takes the
    // settings block from its race table (*0x80092E6C), 1 the GT-mode race table (*0x80092E70, 0x800109C0); the event row of the
    // race block's name (0x8007830C) + 0x44 -> 0x801C98A0, then 0x800771AC builds every slot's car with those tables.
    const bool arcadeRace = record.race[9] == 0;
    std::array<uint8_t, 0x40> settings{};
    std::string eventName;
    for (size_t k = 0; k < 0x10 && record.race[0x10 + k]; k++) eventName.push_back(char(record.race[0x10 + k]));
    std::optional<RaceEvent> event;
    if (arcadeRace) {
        const ArcadeData arcade = ArcadeData::Load(vol);
        if (const int32_t row = arcade.FindEvent(eventName); row >= 0) event = arcade.EventAt(size_t(row));
    } else {
        const GtModeRaceData table = GtModeRaceData::Load(vol);
        if (const int32_t row = table.FindEvent(eventName); row >= 0) event = table.EventAt(size_t(row));
    }
    if (event) std::copy(event->settings.begin(), event->settings.end(), settings.begin());
    else std::printf("ghost replay: event %s is not in the %s race table; zero settings\n", eventName.c_str(), arcadeRace ? "arcade" : "GT-mode");
    uint32_t courseId = 0;
    std::memcpy(&courseId, record.race.data() + 0x40, 4);
    const std::string course = CourseNameOfId(vol, courseId);
    RaceData full;
    LoadRaceBlock(disc, vol, std::span<const uint8_t, 0x58C>(record.race.data(), 0x58C), std::span<const uint8_t, 0x40>(settings.data(), 0x40), course, options, full,
                  arcadeRace);
    data = ReplayRaceData(full); // 0x8001503C with 0x800A951C: the block's one entry
    // The lap ring as 0x80069AC4 leaves it: s16 laps kept (the s8 count), the lap buffers in recording order.
    session = sim::GhostSession{};
    const int16_t count = int16_t(int8_t(uint8_t(record.ghosts.size())));
    session.ring[0] = uint8_t(uint16_t(count)), session.ring[1] = uint8_t(uint16_t(count) >> 8);
    for (size_t k = 0; k < record.ghosts.size() && k < 4; k++) {
        uint8_t* lap = session.ring.data() + 4 + k * sim::kGhostLapSize;
        const auto& [head, stream] = record.ghosts[k];
        std::memcpy(lap, head.data(), std::min(head.size(), sim::kGhostLapStream));
        std::memcpy(lap + sim::kGhostLapStream, stream.data(), std::min(stream.size(), sim::kGhostLapSize - sim::kGhostLapStream));
    }
    data.shell.ghost = &session;
    std::printf("ghost replay: %s, %s, %d lap(s) in the record\n", eventName.c_str(), course.c_str(), int(count));
    return 1;
}

namespace {

// FramesCompareGhost's rules (arcade_race.cpp): a car record's simulated part, a lap buffer up to the larger write position.
size_t CompareCarRecord(const uint8_t* theirs, const uint8_t* ours, size_t& first) {
    constexpr size_t kBody = offsetof(sim::Car, body);
    const uint32_t bodyAddress = RaceAddress(0x800A9688u) + uint32_t(kBody);
    auto word = [](const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    size_t diffs = 0;
    first = SIZE_MAX;
    for (size_t i = kBody; i < kRaceCaptureCarSize; i++) {
        const size_t b = i - kBody;
        if (!(b < 0x798 || (b >= 0xA60 && b < 0xA78))) continue;
        if (theirs[i] == ours[i]) continue;
        const size_t w = i & ~size_t(3);
        if (word(theirs + w) == word(ours + w) + bodyAddress && word(ours + w) < 0xB40) continue;
        diffs++;
        if (first == SIZE_MAX) first = b;
    }
    return diffs;
}

size_t CompareLapBuffer(const uint8_t* theirs, const uint8_t* ours, size_t& first) {
    auto used = [](const uint8_t* stream) { uint16_t v; std::memcpy(&v, stream + 0x10, 2); return size_t(v); };
    const size_t streamBytes = std::min(sim::kGhostLapStreamBytes, ReplayStream::kHeaderSize + std::max(used(theirs + sim::kGhostLapStream), used(ours + sim::kGhostLapStream)));
    size_t diffs = 0;
    first = SIZE_MAX;
    for (size_t i = 0; i < sim::kGhostLapStream + streamBytes; i++)
        if (theirs[i] != ours[i]) {
            diffs++;
            if (first == SIZE_MAX) first = i;
        }
    return diffs;
}

} // namespace

int FramesCompareGhostRecord(const RaceData& data, const RaceOptions& options, const std::string& capturePath, int maxFrames) {
    const std::vector<RaceCaptureFrame> all = ReadRaceCapture(capturePath);
    const std::vector<RaceCaptureGhost> ghosts = ReadRaceCaptureGhost(capturePath + ".ghost");
    if (all.empty()) throw std::runtime_error(capturePath + ": no frames");
    if (ghosts.size() < all.size()) throw std::runtime_error(capturePath + ".ghost: fewer ghost records than frames");
    if (!all[0].demo || all[0].gameMode != 6) throw std::runtime_error("frames: the capture's first race is not the replay of a game mode 6 record");
    sim::RaceSim race;
    SetupRace(race, data, 1, options);
    if (!race.HasGhost()) throw std::runtime_error("frames: the replay has no lap ring");
    const size_t count = maxFrames > 0 ? std::min(all.size(), size_t(maxFrames)) : all.size();
    std::printf("frames (mode 6 record): %zu captured frames (fields %u..%u)\n", all.size(), all.front().field, all.back().field);
    int differing = 0, shown = 0;
    size_t compared = 0;
    for (size_t k = 0; k < count; k++) {
        const RaceCaptureFrame& f = all[k];
        const RaceCaptureGhost& g = ghosts[k];
        if (f.race != all[0].race) break;
        if (g.field != f.field || g.frame != f.frame) throw std::runtime_error("frames: the ghost records are out of step with the frames");
        race.ReplayLapStartIfPending(); // the capture's frame is 0x8003EBF0's entry: after 0x80015B64's 0x8003F990
        compared++;
        std::string where;
        char text[200];
        size_t first = 0;
        const size_t carDiffs = CompareCarRecord(f.cars.data(), reinterpret_cast<const uint8_t*>(&race.CarAt(0)), first);
        if (carDiffs) {
            std::snprintf(text, sizeof text, " car 0: %zu byte(s) from body + 0x%zX;", carDiffs, first);
            where += text;
            if (const char* at = std::getenv("GT2_FRAMES_RANGES"); at && k == size_t(std::atoi(at))) { // dev: the differing ranges
                constexpr size_t kBody = offsetof(sim::Car, body);
                const uint8_t* theirs = f.cars.data();
                const uint8_t* ours = reinterpret_cast<const uint8_t*>(&race.CarAt(0));
                for (size_t i = kBody; i < kBody + 0x798;) {
                    if (theirs[i] == ours[i]) { i++; continue; }
                    size_t j = i;
                    while (j < kBody + 0x798 && theirs[j] != ours[j]) j++;
                    std::printf("    body + 0x%03zX..0x%03zX: original", i - kBody, j - 1 - kBody);
                    for (size_t q = i; q < j && q < i + 8; q++) std::printf(" %02X", theirs[q]);
                    std::printf(" ours");
                    for (size_t q = i; q < j && q < i + 8; q++) std::printf(" %02X", ours[q]);
                    std::printf("\n");
                    i = j;
                }
            }
        }
        const sim::GhostSession& ghost = race.Ghost();
        if (std::memcmp(f.stream.data(), ghost.ring.data(), 2) != 0) where += " ring count;"; // + 2 (the current buffer) is not the replay's
        const int laps = std::clamp<int>(int16_t(uint16_t(ghost.ring[0] | ghost.ring[1] << 8)), 0, 4);
        for (int lap = 0; lap < laps; lap++) {
            const size_t diffs = CompareLapBuffer(f.stream.data() + 4 + size_t(lap) * sim::kGhostLapSize, ghost.ring.data() + 4 + size_t(lap) * sim::kGhostLapSize, first);
            if (diffs) {
                std::snprintf(text, sizeof text, " lap buffer %d: %zu byte(s) from + 0x%zX;", lap, diffs, first);
                where += text;
            }
        }
        // the player's snapshot and the playback block (0x800A8D70.. up to the playback's end; the ghost's own snapshot is not
        // the replay's: car 1 is not set up)
        for (size_t i = 0; i < sim::kGhostPlayback + 0xC4; i++) {
            if (i >= sim::kGhostGhostSnapshot && i < sim::kGhostPlayback) continue;
            if (g.block[i] != ghost.block[i]) {
                std::snprintf(text, sizeof text, " block + 0x%zX (%02X, ours %02X);", i, g.block[i], ghost.block[i]);
                where += text;
                break;
            }
        }
        if (f.clock != race.RaceClock()) {
            std::snprintf(text, sizeof text, " clock %u (ours %u);", f.clock, race.RaceClock());
            where += text;
        }
        const bool same = where.empty();
        if (!same) differing++;
        const sim::CarBody& p = race.CarAt(0).body;
        if ((!same && shown++ < 12) || k % 600 == 0 || k + 1 == count)
            std::printf("  frame %5zu (field %5u) lap %d, %6.1f km/h, playing lap buffer %d  %s%s\n", k, f.field, p.lap, p.forwardSpeed / 4096.0 * 3.6,
                        int(ghost.playerStreamLap), same ? "= original" : "DIFFERS:", where.c_str());
        std::vector<sim::PadRecord> pads(race.CarCount());
        race.Step(pads.data());
    }
    std::printf("frames (mode 6 record): %zu compared, %d differ (car 0, lap ring, snapshot / playback, clock)\n", compared, differing);
    return differing;
}

} // namespace gt2game
