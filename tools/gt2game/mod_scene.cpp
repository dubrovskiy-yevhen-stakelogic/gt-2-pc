// The race view's side of the mod layer (see mod_scene.h).
#include "mod_scene.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "gt2formats/gltf_reader.h"
#include "mods.h"

using namespace gt2;

namespace gt2game {

// ---------------------------------------------------------------- scene

gt2view::SceneAssets::TrackSources ModTrackSources(const ResolvedCourse& course) {
    gt2view::SceneAssets::TrackSources s;
    s.texturePack = course.texturePack;
    s.backdrop = course.info.backdrop;
    s.courseFlags = course.info.flags;
    return s;
}

camera::ReplayCameraData ModReplayCameras(const ResolvedCourse& course, std::vector<uint8_t>& storage) {
    constexpr uint32_t kBase = 0x1000; // any non-zero pointer value (0 = no list)
    camera::ReplayCameraData d;
    storage = course.cameras.Blob(kBase);
    d.bytes = storage;
    d.base = kBase;
    d.list = course.cameras.present ? kBase : 0;
    return d;
}

void PreloadModOpponents(gt2view::SceneAssets& assets, const RaceData& data) {
    for (size_t i = 1; i < data.carIds.size(); i++) {
        const ResolvedCar* m = OpponentMod(data, i);
        if (!m || !m->externalMesh) continue;
        gt2view::SceneAssets::ExternalCar external;
        external.mesh = m->mesh.get();
        external.scale = float(m->meshScale);
        external.reflection = int(m->meshReflection);
        for (size_t k = 0; k < 3; k++) { external.wheelFront[k] = float(m->wheelFront[k]); external.wheelRear[k] = float(m->wheelRear[k]); }
        for (size_t k = 0; k < 2; k++) { external.wheelRadius[k] = float(m->wheelRadius[k]); external.wheelWidth[k] = float(m->wheelWidth[k]); }
        const int slot = assets.UseExternalCar(data.carIds[i], external);
        std::printf("mod opponent %zu (%s): glTF body in scene slot %d\n", i, data.carIds[i].c_str(), slot);
    }
}

namespace {

// T * Ry(yaw) * Rx(pitch) * Rz(roll), column-major, degrees; positions in metres (world x, y up, z).
void ObjectMatrix(const CourseObject& o, float* m) {
    const double k = 3.14159265358979323846 / 180.0;
    const double a = o.rotation[0] * k, b = o.rotation[1] * k, c = o.rotation[2] * k;
    const double ca = std::cos(a), sa = std::sin(a), cb = std::cos(b), sb = std::sin(b), cc = std::cos(c), sc = std::sin(c);
    const double ry[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
    const double rx[3][3] = {{1, 0, 0}, {0, cb, -sb}, {0, sb, cb}};
    const double rz[3][3] = {{cc, -sc, 0}, {sc, cc, 0}, {0, 0, 1}};
    double t[3][3] = {}, r[3][3] = {};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int l = 0; l < 3; l++) t[i][j] += ry[i][l] * rx[l][j];
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int l = 0; l < 3; l++) r[i][j] += t[i][l] * rz[l][j];
    for (int col = 0; col < 3; col++) {
        for (int row = 0; row < 3; row++) m[col * 4 + row] = float(r[row][col]);
        m[col * 4 + 3] = 0;
    }
    for (int row = 0; row < 3; row++) m[12 + row] = float(o.position[size_t(row)]);
    m[15] = 1;
}

} // namespace

std::vector<CourseObjectDraw> LoadCourseObjects(gt2view::SceneAssets& assets, const ResolvedCourse& course) {
    std::vector<CourseObjectDraw> out;
    std::map<std::pair<std::string, double>, int> slots; // (mesh, scale) -> scene slot
    for (size_t i = 0; i < course.objects.size(); i++) {
        const CourseObject& o = course.objects[i];
        const auto key = std::make_pair(o.meshPath, o.scale);
        auto it = slots.find(key);
        if (it == slots.end()) {
            gt2view::SceneAssets::ExternalCar ext;
            ext.mesh = o.loaded.get();
            ext.scale = float(o.scale);
            ext.wheels = false;
            ext.reflection = 0; // course objects are drawn without the cars' reflection pass
            // The soft shadow sits at the mesh's lowest point (UseExternalCar: wheelFront y - wheelRadius + 0.04 m).
            ext.wheelRadius = {0.04f, 0.04f};
            ext.wheelFront[1] = o.loaded->boundsMin[1] * float(o.scale);
            const int slot = assets.UseExternalCar("object:" + o.meshPath + ":" + std::to_string(o.scale), ext);
            it = slots.emplace(key, slot).first;
        }
        if (it->second < 0) {
            std::printf("mod course: object %zu (%s) skipped: no free scene slot\n", i, o.mesh.c_str());
            continue;
        }
        CourseObjectDraw d;
        d.slot = it->second;
        d.shadow = o.shadow;
        ObjectMatrix(o, d.model);
        out.push_back(d);
        std::printf("mod course: object %zu %s in scene slot %d at (%.2f, %.2f, %.2f) m\n", i, o.mesh.c_str(), d.slot, o.position[0], o.position[1], o.position[2]);
    }
    return out;
}

void AppendCourseObjects(const gt2view::SceneAssets& assets, std::vector<gt2view::DrawItem>& items, const float* vp, const std::vector<CourseObjectDraw>& objects) {
    for (const CourseObjectDraw& o : objects) {
        float mvp[16];
        Multiply(vp, o.model, mvp);
        if (o.shadow) {
            assets.AppendCarItems(items, o.slot, mvp, 0, 0);
            continue;
        }
        gt2view::DrawItem item; // the body only (the slot's range starts with it; the shadow follows)
        item.firstVertex = gt2view::SceneAssets::kCarVertexBase + uint32_t(o.slot) * gt2view::SceneAssets::kCarVertexStride;
        item.vertexCount = assets.CarVertexCount(o.slot);
        std::copy(mvp, mvp + 16, item.mvp);
        items.push_back(item);
    }
}

} // namespace gt2game
