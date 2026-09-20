#pragma once
// The mod layer of gt2game (--mods <dir>): mod courses (<dir>/tracks/<name>.json, docs/formats/track_json.md), mod cars
// as the player's car and as AI opponents (<dir>/cars/<id>.json, docs/formats/car_json.md), and the course objects of a
// mod course. The race view's scene helpers are in mod_scene.h; the physics check compares a mod course with its disc course.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gt2formats/car_json.h"
#include "gt2formats/track_json.h"
#include "race_common.h"

namespace gt2game {

// <dir>/tracks/<course>.json when it exists, else empty.
std::string ModCoursePath(const std::string& modsDir, const std::string& course);

// The mod course at `jsonPath` into `data` like LoadRaceTrack does for a disc course: the resolved course (its base
// course's blocks where the file names none), the course extras, the disc-derived constants and the race data of the
// file; data.modCourse keeps what the race view needs (textures, backdrop, flags, cameras, course map, objects).
void LoadModRaceTrack(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::string& jsonPath, const std::string& course, const DataOptions& options,
                      RaceData& data);

// A mod car (<dir>/cars/<id>.json) resolved for race slot `slot` and appended to data's per-slot vectors (params, body
// dimensions, config, id, paint 0, sound). Slot 0 fills data.mod / data.modCar (the player); other slots fill
// data.opponentMods. Sound: the file's engine set / exhaust / turbo; a set the disc does not have falls back to the
// base car's, then to the first disc car's (a player's engine / exhaust bank must exist; an AI car only uses the shared
// AI exhaust bank of its turbo flag). Returns false (nothing appended) when the file does not exist.
bool AddModCarRecord(const gt2::GtfsVolume& vol, const gt2::CarParamTables& tables, const std::string& modsDir, const std::string& id, size_t slot, RaceData& data);

// The mod opponent of race slot `car` (nullptr = a disc car or slot 0).
const gt2::ResolvedCar* OpponentMod(const RaceData& data, size_t car);

// --mod-physics-check <steps>: for every course of <dir>/tracks (or the one named by --track) a race on the mod course and
// the same race on its disc base course (same cars, the player on the scripted self-test pad, the AI cars on their lines)
// are stepped side by side; the full simulation state (RaceSim::Snapshot) must be identical after every step. Returns the
// number of courses that differ.
int ModCoursePhysicsCheck(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const std::string& modsDir, const std::string& onlyCourse, size_t carCount,
                          int steps);

} // namespace gt2game
