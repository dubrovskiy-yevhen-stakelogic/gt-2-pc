// The mod layer of gt2game (see mods.h).
#include "mods.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <stdexcept>

#include "gt2formats/car_info.h"
#include "gt2formats/gltf_reader.h"
#include "gt2formats/sound_bank.h"
#include "gt2vfs/gtfs.h"

using namespace gt2;

namespace gt2game {

std::string ModCoursePath(const std::string& modsDir, const std::string& course) {
    if (modsDir.empty() || course.empty()) return {};
    const std::filesystem::path p = std::filesystem::path(modsDir) / "tracks" / (course + ".json");
    return std::filesystem::exists(p) ? p.string() : std::string();
}

void LoadModRaceTrack(const DiscImage& disc, const GtfsVolume& vol, const std::string& jsonPath, const std::string& course, const DataOptions& options,
                      RaceData& data) {
    const CourseFile file = ReadCourseFile(jsonPath);
    for (const std::string& w : file.warnings) std::printf("mod course %s: warning: %s\n", course.c_str(), w.c_str());
    auto resolved = std::make_shared<ResolvedCourse>(ResolveCourseFile(vol, file, jsonPath));
    for (const std::string& n : resolved->notes) std::printf("mod course %s: %s\n", course.c_str(), n.c_str());
    data.trackName = course;
    data.track = resolved->track;
    data.extras = sim::BuildCourseExtras(data.track, {});
    CourseSource source;
    source.courseIndex = resolved->baseIndex;
    source.haveFlags = resolved->info.hasEntry;
    source.flags = resolved->info.flags;
    source.race = resolved->race;
    LoadCourseData(disc, vol, course, source, options, data);
    size_t records = 0;
    for (const auto& l : resolved->race.lists) records += l.size();
    std::printf("mod course %s: %s, base course %s, %zu chunks, %zu scenery instances, %zu race records, %zu replay cameras, textures %s, backdrop %s, %zu object(s)\n",
                course.c_str(), jsonPath.c_str(), resolved->baseCourse.empty() ? "(none)" : resolved->baseCourse.c_str(), data.track.chunks.size(),
                data.track.sceneryInstances.size(), records, resolved->cameras.records.size(), resolved->textureSource.c_str(),
                resolved->info.backdrop.empty() ? "(none)" : resolved->info.backdrop.c_str(), resolved->objects.size());
    if (!resolved->baseCourse.empty()) {
        const CourseFile base = MakeCourseFile(vol, resolved->baseCourse);
        const std::vector<std::string> diff = DiffCourses(base.track, base.race, data.track, resolved->race, 12);
        std::printf("mod course %s: %zu difference(s) from the disc course %s%s\n", course.c_str(), diff.size(), resolved->baseCourse.c_str(), diff.size() == 12 ? " (first 12)" : "");
        for (const std::string& d : diff) std::printf("  %s\n", d.c_str());
    }
    data.modCourse = std::move(resolved);
}

// ---------------------------------------------------------------- mod cars

namespace {

bool HaveFile(const GtfsVolume& vol, const std::string& path) { return vol.Find(path) != nullptr || vol.Find(path + ".gz") != nullptr; }

} // namespace

bool AddModCarRecord(const GtfsVolume& vol, const CarParamTables& tables, const std::string& modsDir, const std::string& id, size_t slot, RaceData& data) {
    if (modsDir.empty()) return false;
    const std::string path = (std::filesystem::path(modsDir) / "cars" / (id + ".json")).string();
    if (!std::filesystem::exists(path)) return false;
    CheckCarJsonDescriptors();
    const CarJson car = ReadCarJson(path);
    for (const std::string& w : car.warnings) std::printf("mod %s: warning: %s\n", id.c_str(), w.c_str());
    auto m = std::make_shared<ResolvedCar>(ResolveCarJson(vol, tables, car, path));
    for (const std::string& n : m->notes) std::printf("mod %s: %s\n", id.c_str(), n.c_str());
    std::printf("mod %s (slot %zu%s): %s%s, body %s%s\n", id.c_str(), slot, slot == 0 ? ", player" : ", AI", m->baseCar.empty() ? "stand-alone" : "base car ",
                m->baseCar.c_str(), m->externalMesh ? m->meshPath.c_str() : ("carobj/" + m->modelId + ".cdo").c_str(), m->externalMesh ? " (glTF)" : "");
    if (!m->baseCar.empty()) {
        const std::vector<std::string> configDiff = DiffCarConfig(m->stockConfig, m->config), paramsDiff = DiffCarParams(m->stockParams, m->params);
        std::printf("mod %s: config %zu field(s) differ from stock, params %zu field(s) differ from stock\n", id.c_str(), configDiff.size(), paramsDiff.size());
        for (const std::string& d : configDiff) std::printf("  config %s\n", d.c_str());
        for (const std::string& d : paramsDiff) std::printf("  params %s\n", d.c_str());
    }
    // Sound fallback: the set must exist for a player's car (its intake and exhaust banks are loaded).
    audio::CarSoundSetup sound;
    sound.soundId = m->engineSoundId;
    sound.exhaustByte = m->exhaustByte;
    sound.turbo = m->turbo;
    auto haveSet = [&](uint32_t soundId, uint8_t exhaust) { return HaveFile(vol, EngineSoundPath(soundId)) && HaveFile(vol, ExhaustSoundPath(soundId, exhaust)); };
    if (!haveSet(sound.soundId, sound.exhaustByte)) {
        uint32_t fallbackId = 0;
        uint8_t fallbackExhaust = 0;
        std::string from;
        if (!m->baseCar.empty() && haveSet(m->stockConfig.engineWord, m->stockConfig.exhaustByte)) {
            fallbackId = m->stockConfig.engineWord;
            fallbackExhaust = m->stockConfig.exhaustByte;
            from = "base car " + m->baseCar;
        } else {
            const CarConfig first = StockCarConfig(tables, tables.RowAs<ChassisRow>(kTableChassis, 0).carId);
            fallbackId = first.engineWord;
            fallbackExhaust = first.exhaustByte;
            from = "disc car " + UnpackCarId(tables.RowAs<ChassisRow>(kTableChassis, 0).carId);
        }
        std::printf("mod %s: engine set %05u / exhaust %u is not on the disc; the %s's sound (%05u / %u) is used\n", id.c_str(), unsigned(sound.soundId),
                    unsigned(sound.exhaustByte), from.c_str(), fallbackId, unsigned(fallbackExhaust));
        sound.soundId = fallbackId;
        sound.exhaustByte = fallbackExhaust;
    }
    data.params.push_back(m->params);
    data.bodies.push_back(m->dims);
    data.configs.push_back(m->config);
    data.paints.push_back(0);
    data.sound.push_back(sound);
    if (slot == 0) {
        data.carIds.push_back(id);
        data.mod = *m;
        data.modCar = true;
    } else {
        // The race id of an opponent is the mod id; the scene loads its body under that name (a glTF body:
        // PreloadModOpponents; a disc body: the model under the mod id as alias, race_view.cpp).
        data.carIds.push_back(id);
        if (data.opponentMods.size() <= slot) data.opponentMods.resize(slot + 1);
        data.opponentMods[slot] = m;
    }
    return true;
}

const ResolvedCar* OpponentMod(const RaceData& data, size_t car) {
    return car > 0 && car < data.opponentMods.size() ? data.opponentMods[car].get() : nullptr;
}

// ---------------------------------------------------------------- physics check

int ModCoursePhysicsCheck(const DiscImage& disc, const GtfsVolume& vol, const std::string& modsDir, const std::string& onlyCourse, size_t carCount, int steps) {
    std::vector<std::string> courses;
    const std::filesystem::path dir = std::filesystem::path(modsDir) / "tracks";
    if (!onlyCourse.empty()) courses.push_back(onlyCourse);
    else
        for (const auto& e : std::filesystem::directory_iterator(dir))
            if (e.path().extension() == ".json") courses.push_back(e.path().stem().string());
    std::sort(courses.begin(), courses.end());
    int differ = 0, same = 0, skipped = 0;
    for (const std::string& name : courses) {
        const std::string path = ModCoursePath(modsDir, name);
        if (path.empty()) {
            std::printf("%-20s FAIL: no %s\n", name.c_str(), (dir / (name + ".json")).string().c_str());
            differ++;
            continue;
        }
        RaceData mod, base;
        std::string baseCourse;
        try {
            LoadModRaceTrack(disc, vol, path, name, DataOptions{}, mod);
            baseCourse = mod.modCourse->baseCourse;
            if (baseCourse.empty()) throw std::runtime_error("stand-alone course: no disc course to compare with");
            LoadRaceTrack(disc, vol, baseCourse, DataOptions{}, base);
        } catch (const std::exception& e) {
            std::printf("%-20s FAIL: %s\n", name.c_str(), e.what());
            differ++;
            continue;
        }
        RaceOptions options;
        options.countdown = false;
        std::vector<uint8_t> a, b;
        std::string outcome;
        try {
            BuildCarRecords(vol, "us36n", carCount, std::string(), base);
            BuildCarRecords(vol, "us36n", carCount, std::string(), mod);
            sim::RaceSim raceBase, raceMod;
            bool baseOk = true;
            std::string baseError;
            try {
                SetupRace(raceBase, base, carCount, options);
            } catch (const std::exception& e) {
                baseOk = false;
                baseError = e.what();
            }
            if (!baseOk) { // the disc course itself cannot host this race (e.g. no grid): the mod course must fail the same way
                try {
                    SetupRace(raceMod, mod, carCount, options);
                    outcome = "DIFFERS: the disc course fails to set up (" + baseError + ") but the mod course does not";
                    differ++;
                } catch (const std::exception& e) {
                    outcome = std::string(e.what()) == baseError ? "skip (the disc course does not set up a race: " + baseError + "; the mod course fails the same way)"
                                                                 : "DIFFERS: set-up errors differ";
                    if (std::string(e.what()) == baseError) skipped++;
                    else differ++;
                }
                std::printf("%-20s %s\n", name.c_str(), outcome.c_str());
                continue;
            }
            SetupRace(raceMod, mod, carCount, options);
            a = raceBase.Snapshot();
            b = raceMod.Snapshot();
            int firstDiff = a == b ? -1 : 0;
            std::vector<sim::PadRecord> pads(carCount);
            for (int step = 1; step <= steps && firstDiff < 0; step++) {
                pads[0] = ScriptedPad(step - 1);
                std::vector<sim::PadRecord> padsB = pads;
                raceBase.Step(pads.data());
                raceMod.Step(padsB.data());
                a = raceBase.Snapshot();
                b = raceMod.Snapshot();
                if (a != b) firstDiff = step;
            }
            if (firstDiff >= 0) {
                size_t byte = 0;
                while (byte < std::min(a.size(), b.size()) && a[byte] == b[byte]) byte++;
                std::printf("%-20s DIFFERS from %s at step %d (state byte %zu of %zu)\n", name.c_str(), baseCourse.c_str(), firstDiff, byte, a.size());
                differ++;
            } else {
                const sim::CarTelemetry t = raceMod.Telemetry(0);
                std::printf("%-20s identical to %s: %d steps x %zu cars, %zu state bytes per step; player %.1f km/h, %.1f m along the course\n", name.c_str(),
                            baseCourse.c_str(), steps, carCount, a.size(), t.forwardSpeed / 4096.0 * 3.6, t.courseDistance / 65536.0);
                same++;
            }
        } catch (const std::exception& e) {
            std::printf("%-20s FAIL: %s\n", name.c_str(), e.what());
            differ++;
        }
    }
    std::printf("mod physics check: %zu course(s): %d identical, %d skipped (no race on the disc course either), %d differ / fail\n", courses.size(), same, skipped, differ);
    return differ;
}

} // namespace gt2game
