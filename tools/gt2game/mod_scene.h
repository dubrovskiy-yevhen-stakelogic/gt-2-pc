#pragma once
// The race view's side of the mod layer (mods.h): a mod course's textures / backdrop / replay cameras, the mod opponents'
// glTF bodies and the course objects (docs/formats/track_json.md, car_json.md).
#include <cstdint>
#include <string>
#include <vector>

#include "game/camera/race_camera.h"
#include "gt2formats/track_json.h"
#include "gt2view/scene_assets.h"
#include "race_common.h"

namespace gt2game {

// The mod course's textures / backdrop / flags for SceneAssets::UseTrack.
gt2view::SceneAssets::TrackSources ModTrackSources(const gt2::ResolvedCourse& course);
// The mod course's replay cameras as the camera code reads them; `storage` keeps the bytes.
gt2::camera::ReplayCameraData ModReplayCameras(const gt2::ResolvedCourse& course, std::vector<uint8_t>& storage);

// The external glTF opponents of the race loaded into their slots under their race id (data.carIds[car]) before any
// UseCar of that id, so that every later look-up finds the glTF body. Prints the slots.
void PreloadModOpponents(gt2view::SceneAssets& assets, const RaceData& data);

// Course objects of a mod course: one scene slot per distinct mesh (the renderer's external-mesh path), a world matrix per
// object. Objects that find no free slot are skipped with a message.
struct CourseObjectDraw {
    int slot = -1;
    bool shadow = false;
    float model[16] = {};
};
std::vector<CourseObjectDraw> LoadCourseObjects(gt2view::SceneAssets& assets, const gt2::ResolvedCourse& course);
void AppendCourseObjects(const gt2view::SceneAssets& assets, std::vector<gt2view::DrawItem>& items, const float* vp, const std::vector<CourseObjectDraw>& objects);

} // namespace gt2game
