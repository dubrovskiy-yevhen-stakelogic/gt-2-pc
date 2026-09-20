// The arcade races (arcade_race.h).
#include "arcade_race.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>

#include "game/career/results.h"
#include "game/sim/disc_data.h"
#include "gt2formats/arcade_data.h"
#include "gt2formats/car_info.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2formats/license_data.h"
#include "gt2formats/race_capture.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

using namespace gt2;

namespace gt2game {
namespace {

// The shell state of a race block (0x801D585C..: + 0x01 0x801D585D, + 0x04 0x801D5860, + 0x08 0x801D5864 frame-rate mode,
// + 0x09 0x801D5865, + 0x0A game mode 0x801D5866, + 0x0D 0x801D5869 countdown) of a live race (no demo flag).
sim::ShellState ShellOfRaceBlock(const uint8_t* block) {
    sim::ShellState s;
    s.modeFlag5D = block[0x01];
    s.modeFlag60 = block[0x04];
    s.frameRateMode = block[0x08];
    s.modeFlag65 = block[0x09];
    s.gameMode = block[0x0A];
    s.byte801D5869 = block[0x0D];
    s.flag800A951C = 0;
    return s;
}

// The draw of an opponent from the event's slots (0x80011EC0 on the arcade disc; its structure - a random used slot whose
// car is available in the region, redrawn when an earlier entry has the same opponent or car unless a 3-in-32 chance or
// the 64-try budget lets it through, a random paint for paint code 0 - is that of the GT-mode picker 0x80010714,
// game/career/events.cpp). The seed is the caller's; the draw is reproducible for a seed, not claimed equal to a run of
// the original (whose seed is the VSync counter at the race build).
struct Drawn { uint32_t number = 0, carId = 0; char paint = 0; };
Drawn DrawOpponent(const ArcadeData& ad, const RaceEvent& event, const CarInfoDirectory& cars, uint32_t& seed, const std::vector<Drawn>& earlier) {
    const uint32_t used = uint32_t(event.OpponentCount());
    if (used == 0) throw std::runtime_error("arcade event " + event.name + " has no opponents");
    auto carOf = [&](uint32_t number) { return ad.Opponent(number).spec.carId; };
    auto available = [&](uint32_t carId) { const CarInfoRecord* r = cars.Find(carId); return r && r->AvailableIn(kLanguageUsa); };
    int32_t budget = 0x40;
    uint32_t slot = 0, number = 0;
    for (;;) {
        do {
            slot = event.slots[career::NextRandom(seed) % used];
            number = RaceEvent::SlotOpponent(slot) & 0xFFFF;
        } while (!available(carOf(number)));
        bool reject = false;
        for (const Drawn& d : earlier)
            if ((d.number == number || d.carId == carOf(number)) && (career::NextRandom(seed) & 0x1F) < 0x1D && budget > 0) {
                reject = true;
                break;
            }
        if (!reject) break;
        budget--;
    }
    Drawn d;
    d.number = number;
    d.carId = carOf(number);
    const CarInfoRecord* info = cars.Find(d.carId);
    if ((slot >> 26) != 0) d.paint = RaceEvent::SlotPaint(slot);
    else if (info && info->PaintCount() > 0) d.paint = char(info->paintIds.at(career::NextRandom(seed) % info->PaintCount()));
    return d;
}

uint32_t PaintIndexOfId(const CarInfoDirectory& cars, uint32_t carId, char paint) {
    const CarInfoRecord* info = cars.Find(carId);
    const int index = info ? info->PaintIndex(uint8_t(paint)) : -1;
    return index < 0 ? 0u : uint32_t(index);
}

} // namespace

void BuildArcadeRace(const DiscImage& disc, const GtfsVolume& vol, const std::string& trackName, const std::string& carId, size_t carCount,
                     const ArcadeRaceOptions& arcade, RaceOptions& options, RaceData& data) {
    if (!ProfileOf(disc).arcade) std::printf("arcade: the disc is not the arcade disc; its carparam/usa_arcade_data.dat is used all the same\n");
    const ArcadeData ad = ArcadeData::Load(vol);
    const std::string eventName = ArcadeData::EventName(arcade.level, arcade.carClass);
    const int32_t row = ad.FindEvent(eventName);
    if (row < 0) throw std::runtime_error("arcade: no event " + eventName + " in usa_arcade_data.dat");
    const RaceEvent event = ad.EventAt(size_t(row));
    const uint32_t playerId = PackCarId(carId);
    const auto playerRow = ad.FindPlayerCar(playerId);
    if (!playerRow) throw std::runtime_error("arcade: " + carId + " is not a car of the arcade car table (usa_arcade_data.dat table 32)");

    // The race block of the arcade Road Race (the dump's bytes): mode 4, 0x801D585D / 0x801D5860 / 0x801D5865 = 0, frame-rate
    // mode 2, countdown = the event's standing start (settings + 0 = 0; ADT / ATT start rolling at 80 km/h).
    uint8_t block[0x10] = {};
    block[0x08] = 2;
    block[0x0A] = 4;
    block[0x0D] = event.StartSpeed() == 0 ? 1 : 0;
    const sim::ShellState shell = ShellOfRaceBlock(block);
    std::array<uint8_t, kLicenseSettingsSize> settings{};
    std::copy(event.settings.begin(), event.settings.end(), settings.begin());
    DataOptions o;
    o.shell = &shell;
    o.eventSettings = &settings;
    LoadRaceTrack(disc, vol, trackName, o, data);

    const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
    const size_t table = arcade.tyreTable == 33 ? kArcadePlayerCarTableAlt : kArcadePlayerCarTable;
    AddCarRecord(vol, ad.Tables(), playerId, ad.PlayerCarConfig(*playerRow, table), 0, data);
    uint32_t seed = arcade.seed;
    std::vector<Drawn> drawn;
    for (size_t i = 1; i < carCount; i++) {
        Drawn d;
        if (i - 1 < arcade.opponents.size()) {
            d.number = arcade.opponents[i - 1];
            d.carId = ad.Opponent(d.number).spec.carId;
        } else {
            d = DrawOpponent(ad, event, cars, seed, drawn);
        }
        drawn.push_back(d);
        AddCarRecord(vol, ad.Tables(), d.carId, ad.OpponentConfig(d.number), PaintIndexOfId(cars, d.carId, d.paint), data);
    }
    // 0x800121DC: entry i on grid slot count - 1 - i (the player last), kind 3 for the player, 1 for the AI; AT.
    for (size_t i = 0; i < carCount; i++) {
        data.gridSlots.push_back(uint8_t(carCount - 1 - i));
        data.entryKinds.push_back(i == 0 ? sim::kEntryPlayer1 : sim::kEntryAi);
        data.transmissions.push_back(options.manual && i == 0 ? 1 : 0);
    }
    options.countdown = block[0x0D] != 0;
    std::printf("arcade: event %s (level %d, class %c), %s from table %zu, opponents", event.name.c_str(), arcade.level, arcade.carClass, carId.c_str(), table);
    for (const Drawn& d : drawn) std::printf(" %u:%s", d.number, UnpackCarId(d.carId).c_str());
    std::printf("; AI corner grip %u %%, catch-up bytes %02X %02X %02X %02X %02X %02X\n", settings[0x0C], settings[0x17], settings[0x18], settings[0x19], settings[0x1A],
                settings[0x1B], settings[0x1C]);
}

size_t LoadCaptureRace(const DiscImage& disc, const GtfsVolume& vol, const std::string& setupPath, const std::string& trackName, RaceOptions& options, RaceData& data) {
    RaceCaptureSetup setup;
    if (!ReadRaceCaptureSetup(setupPath, setup)) throw std::runtime_error("cannot read the capture's race " + setupPath);
    return LoadRaceBlock(disc, vol, setup.raceBlock, setup.settings, trackName, options, data);
}

size_t LoadRaceBlock(const DiscImage& disc, const GtfsVolume& vol, std::span<const uint8_t, 0x58C> raceBlock, std::span<const uint8_t, 0x40> eventSettings,
                     const std::string& trackName, RaceOptions& options, RaceData& data, std::optional<bool> arcadeTables) {
    RaceCaptureSetup setup;
    std::copy(raceBlock.begin(), raceBlock.end(), setup.raceBlock.begin());
    std::copy(eventSettings.begin(), eventSettings.end(), setup.settings.begin());
    const uint8_t* block = setup.raceBlock.data();
    const sim::ShellState shell = ShellOfRaceBlock(block);
    std::array<uint8_t, kLicenseSettingsSize> settings{};
    std::copy(setup.settings.begin(), setup.settings.end(), settings.begin());
    // 0x8003C12C: the race load rewrites the settings of the mode (2 player Battle: Slow Car Boost = race block + 7, Tire Damage =
    // + 3) and enables the catch-up (race_sim.h RaceLoadSettings); the other modes' blocks of the menus are left as they are.
    const bool catchUp = sim::RaceLoadSettings(block[0x0A], block[0x03], block[0x07], settings);
    DataOptions o;
    o.shell = &shell;
    o.eventSettings = &settings;
    LoadRaceTrack(disc, vol, trackName, o, data);
    data.constants.catchUp = sim::CatchUpFromSettings(settings, catchUp); // 0x80041E4C
    const bool arcade = arcadeTables ? *arcadeTables : ProfileOf(disc).arcade;
    const CarParamTables tables = arcade ? ArcadeData::Load(vol).Tables() : CarParamTables::Load(vol);
    const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
    size_t count = block[0x5A];
    if (count == 0 || count > sim::kMaxCars) throw std::runtime_error("capture setup: bad car count");
    // Game mode 6 (Time Trial / Rally, the "ATT" event): 0x8001503C races two cars - entry 1 is the ghost (kind 2), a copy of
    // the player's entry unless it holds the lap of a previous race (its byte + 0x8C), with the player's parameter record.
    std::array<uint8_t, 0xD0> ghostEntry{};
    const bool mode6 = block[0x0A] == 6;
    if (mode6) {
        const uint8_t* player = block + 0x5C;
        const uint8_t* second = block + 0x5C + 0xD0;
        std::copy(second, second + 0xD0, ghostEntry.begin());
        if (second[0x8C] == 0) {
            std::copy(player, player + 0xD0, ghostEntry.begin());
            ghostEntry[0x8C] = 0;
        }
        ghostEntry[0x8E] = sim::kEntryGhost;
        count = 2;
        data.shell.pedalTable = PedalTable(disc);
    }
    for (size_t i = 0; i < count; i++) {
        const uint8_t* e = (mode6 && i == 1) ? ghostEntry.data() : block + 0x5C + i * 0xD0;
        uint32_t carId;
        std::memcpy(&carId, e, 4);
        CarConfig config;
        std::memcpy(&config, e + 8, sizeof(config));
        // A garage car of the arcade menus (game/arcade/arcade_setup.h RebuildGarageEntry: member 3 builds its record with the
        // GT-mode tables and sets the GT flag +0x7A bit 6 of its configuration); every other arcade entry uses the arcade tables.
        const bool gtEntry = arcade && (e[8 + 0x7A] & 0x40) != 0;
        static std::optional<CarParamTables> gtTables; // the GT-mode tables of the disc, loaded once
        if (gtEntry && !gtTables) gtTables.emplace(CarParamTables::Load(vol));
        AddCarRecord(vol, gtEntry ? *gtTables : tables, carId, config, PaintIndexOfId(cars, carId, char(e[4])), data);
        // 0x80012CD4: in game mode 6 every car of a circuit starts on grid slot 2 (sim::GridSlotOfEntry).
        data.gridSlots.push_back(sim::GridSlotOfEntry(e[0x8D], block[0x0A], data.shell.pointToPoint ? 0x20 : 0));
        data.entryKinds.push_back(e[0x8E]);
        data.transmissions.push_back(e[0x8F]);
    }
    options.laps = block[0x0F];
    options.countdown = block[0x0D] != 0;
    data.handicap = int8_t(block[0x06]); // race block + 6: Handicap Start (mode 0, sim::HandicapGridOffset)
    data.dirtLevel = uint16_t(block[0x58] | block[0x59] << 8); // race block + 0x58 (Sim 0x801D58B4, Arcade 0x801D5314): player 1's dirt (0x80012CD4)
    std::printf("capture race: mode %u, %u lap(s), countdown %u, %zu car(s) (%s car tables)\n", block[0x0A], block[0x0F], block[0x0D], count,
                arcade ? "arcade" : "GT-mode");
    return count;
}

namespace {

// The simulated part of a car record (FramesCompare's rule: body + 0 .. 0x798 and the HUD block + 0xA60 .. 0xA78; a word equal to
// ours + the original's body address is the same body-relative pointer). Returns the differing bytes and the first one.
size_t CompareCar(const uint8_t* theirs, const uint8_t* ours, size_t car, size_t& first) {
    constexpr size_t kBody = offsetof(sim::Car, body);
    const uint32_t bodyAddress = RaceAddress(0x800A9688u) + uint32_t(car) * kRaceCaptureCarSize + uint32_t(kBody);
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
        if (first == SIZE_MAX) first = i - kBody;
    }
    return diffs;
}

// A lap buffer: the head, the stream object's header and its coded bytes up to the larger of the two write positions (the
// data after it is whatever an earlier lap left in the buffer).
size_t CompareLap(const uint8_t* theirs, const uint8_t* ours, size_t& first) {
    size_t diffs = 0;
    first = SIZE_MAX;
    auto used = [](const uint8_t* stream) { uint16_t v; std::memcpy(&v, stream + 0x10, 2); return size_t(v); };
    const size_t streamBytes = std::min(sim::kGhostLapStreamBytes, ReplayStream::kHeaderSize + std::max(used(theirs + sim::kGhostLapStream), used(ours + sim::kGhostLapStream)));
    for (size_t i = 0; i < sim::kGhostLapStream + streamBytes; i++)
        if (theirs[i] != ours[i]) {
            diffs++;
            if (first == SIZE_MAX) first = i;
        }
    return diffs;
}

} // namespace

// The per-car vectors of a race's data cut to player 1 and the replay flag 0x800A951C set: the replay of a mode 6 race
// (0x8001503C keeps the race block's one entry when 0x800A951C is set; arcade_disc.md 17.10).
RaceData ReplayRaceData(const RaceData& data) {
    RaceData r = data;
    auto cut = [](auto& v) {
        if (v.size() > 1) v.resize(1);
    };
    cut(r.params), cut(r.carIds), cut(r.paints), cut(r.sound), cut(r.bodies), cut(r.gridSlots), cut(r.entryKinds), cut(r.transmissions), cut(r.configs);
    r.constants.flag800A951C = 1;
    return r;
}

int FramesCompareGhost(const RaceData& data, size_t carCount, const RaceOptions& options, const std::string& capturePath, const std::array<uint16_t, 16>& pedalTable,
                       int maxFrames) {
    const std::vector<RaceCaptureFrame> all = ReadRaceCapture(capturePath);
    const std::vector<RaceCaptureGhost> ghosts = ReadRaceCaptureGhost(capturePath + ".ghost");
    if (all.empty()) throw std::runtime_error(capturePath + ": no frames");
    if (ghosts.size() < all.size()) throw std::runtime_error(capturePath + ".ghost: fewer ghost records than frames (a game mode 6 capture of gt2verify --race-capture)");
    sim::RaceSim live;
    SetupRace(live, data, carCount, options);
    if (!live.HasGhost()) throw std::runtime_error("frames: the capture's race is not a game mode 6 race");
    ReplayDriver driver;
    driver.pedalTable = pedalTable;
    driver.StartRecording(data.constants.gameMode);
    driver.Attach(live);
    // A second race of the capture = the replay the arcade loop runs after the pause's Exit (states 10 .. 12): the race end
    // 0x800153B8 on the live race, then the replay on the same ghost session (RaceSim with 0x800A951C, player 1 alone).
    sim::RaceSim replay;
    RaceData replayData;
    sim::GhostSession replaySession;
    sim::RaceSim* race = &live;
    size_t cars = carCount;
    const size_t count = maxFrames > 0 ? std::min(all.size(), size_t(maxFrames)) : all.size();
    std::printf("frames (game mode 6): %zu captured frames (fields %u..%u), %zu car(s)\n", all.size(), all.front().field, all.back().field, carCount);
    int differing = 0, shown = 0;
    size_t compared = 0, replayFrames = 0;
    for (size_t k = 0; k < count; k++) {
        const RaceCaptureFrame& f = all[k];
        const RaceCaptureGhost& g = ghosts[k];
        if (f.race != all[0].race && !(race == &replay && f.race == all[0].race + 1)) {
            if (race != &live || f.race != all[0].race + 1 || !f.demo) break;
            live.EndGhostRace(); // 0x800153B8 (the Exit of the pause)
            replaySession = live.Ghost();
            replayData = ReplayRaceData(data);
            replayData.shell.ghost = &replaySession;
            SetupRace(replay, replayData, 1, options);
            race = &replay;
            cars = 1;
            std::printf("frames (game mode 6): the replay (race %u, field %u): player 1 on the ring of %d lap(s)\n", f.race, f.field,
                        int(int16_t(uint16_t(replaySession.ring[0] | replaySession.ring[1] << 8))));
        }
        if (g.field != f.field || g.frame != f.frame) throw std::runtime_error("frames: the ghost records are out of step with the frames");
        if (race == &replay) replay.ReplayLapStartIfPending(); // the capture's frame is 0x8003EBF0's entry: after 0x80015B64's 0x8003F990
        compared++;
        if (race == &replay) replayFrames++;
        std::string where;
        char text[200];
        for (size_t car = 0; car < cars && car < kRaceCaptureCars; car++) {
            size_t first = 0;
            const uint8_t* theirs = f.cars.data() + car * kRaceCaptureCarSize;
            const uint8_t* ours = reinterpret_cast<const uint8_t*>(&race->CarAt(car));
            const size_t diffs = CompareCar(theirs, ours, car, first);
            if (diffs && std::getenv("GT2_FRAMES_RANGES") && k == size_t(std::atoi(std::getenv("GT2_FRAMES_RANGES")))) { // dev: the differing ranges
                constexpr size_t kBody = offsetof(sim::Car, body);
                for (size_t i = kBody; i < kBody + 0x798;) {
                    if (theirs[i] == ours[i]) { i++; continue; }
                    size_t j = i;
                    while (j < kBody + 0x798 && theirs[j] != ours[j]) j++;
                    std::printf("    car %zu body + 0x%03zX..0x%03zX: original", car, i - kBody, j - 1 - kBody);
                    for (size_t q = i; q < j && q < i + 8; q++) std::printf(" %02X", theirs[q]);
                    std::printf(" ours");
                    for (size_t q = i; q < j && q < i + 8; q++) std::printf(" %02X", ours[q]);
                    std::printf("\n");
                    i = j;
                }
            }
            if (diffs) {
                std::snprintf(text, sizeof text, " car %zu: %zu byte(s) from body + 0x%zX;", car, diffs, first);
                where += text;
            }
        }
        const sim::GhostSession& ghost = race->Ghost();
        if (std::memcmp(f.stream.data(), ghost.ring.data(), 4) != 0) where += " ring counters;";
        for (size_t lap = 0; lap < 4; lap++) {
            size_t first = 0;
            const size_t diffs = CompareLap(f.stream.data() + 4 + lap * sim::kGhostLapSize, ghost.ring.data() + 4 + lap * sim::kGhostLapSize, first);
            if (diffs) {
                std::snprintf(text, sizeof text, " lap buffer %zu: %zu byte(s) from + 0x%zX;", lap, diffs, first);
                where += text;
                if (std::getenv("GT2_FRAMES_RANGES") && k == size_t(std::atoi(std::getenv("GT2_FRAMES_RANGES")))) { // dev: the bytes
                    const uint8_t* t = f.stream.data() + 4 + lap * sim::kGhostLapSize;
                    const uint8_t* o = ghost.ring.data() + 4 + lap * sim::kGhostLapSize;
                    std::printf("    lap buffer %zu + 0x%zX: original", lap, first);
                    for (size_t q = first; q < first + 16; q++) std::printf(" %02X", t[q]);
                    std::printf(" ours");
                    for (size_t q = first; q < first + 16; q++) std::printf(" %02X", o[q]);
                    std::printf("\n");
                }
            }
        }
        {
            size_t first = 0;
            const size_t diffs = CompareLap(g.reference.data(), ghost.reference.data(), first);
            if (diffs) {
                std::snprintf(text, sizeof text, " reference: %zu byte(s) from + 0x%zX;", diffs, first);
                where += text;
            }
        }
        constexpr size_t kBlockCompared = sim::kGhostPlayback + 0xC4; // the rest of 0x800A8D70..0x800A94D0 is not the ghost's
        for (size_t i = 0; i < kBlockCompared; i++)
            if (g.block[i] != ghost.block[i]) {
                std::snprintf(text, sizeof text, " block + 0x%zX (%02X, ours %02X);", i, g.block[i], ghost.block[i]);
                where += text;
                break;
            }
        uint32_t initialised;
        std::memcpy(&initialised, g.flags.data() + 4, 4);
        if (g.flags[0] != ghost.newBest || g.flags[1] != ghost.savedBest || initialised != ghost.initialised) where += " flags;";
        if (race == &replay && f.clock != race->RaceClock()) {
            std::snprintf(text, sizeof text, " clock %u (ours %u);", f.clock, race->RaceClock());
            where += text;
        }
        const bool same = where.empty();
        if (!same) differing++;
        const sim::CarBody& p = race->CarAt(0).body;
        const sim::CarBody& q = race->CarAt(cars > 1 ? 1 : 0).body;
        if ((!same && shown++ < 12) || k % 600 == 0 || k + 1 == count)
            std::printf("  frame %5zu (field %5u) lap %d / ghost lap %d, player %6.1f km/h, ghost %6.1f km/h  %s%s\n", k, f.field, p.lap, q.lap, p.forwardSpeed / 4096.0 * 3.6,
                        q.forwardSpeed / 4096.0 * 3.6, same ? "= original" : "DIFFERS:", where.c_str());
        if (race == &live) {
            driver.pad.buttons = f.padButtons;
            driver.pad.analog = f.padAnalog;
            driver.pad.steerAxis = f.padSteer;
            driver.pad.throttle = f.padThrottle;
            driver.pad.brake = f.padBrake;
        }
        std::vector<sim::PadRecord> pads(race->CarCount());
        race->Step(pads.data());
    }
    std::printf("frames (game mode 6): %zu compared (%zu of the replay), %d differ (car records, lap ring, reference lap, snapshots / playback, flags)\n", compared,
                replayFrames, differing);
    return differing;
}

} // namespace gt2game
