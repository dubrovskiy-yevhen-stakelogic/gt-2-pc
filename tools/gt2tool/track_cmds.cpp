// Course mod layer commands: export-tracks (every course to JSON + glTF, round trip checked) and import-track.
#include "track_cmds.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2export/track_gltf.h"
#include "gt2formats/gltf_reader.h"
#include "gt2formats/psx_vram.h"
#include "gt2formats/sha1.h"
#include "gt2formats/track_json.h"
#include "gt2vfs/gtfs.h"

using namespace gt2;

namespace gt2tool {
namespace {

std::vector<std::string> DiscCourses(const GtfsVolume& vol) {
    std::vector<std::string> names;
    for (const GtfsEntry& f : vol.Files()) {
        if (f.path.rfind("crsobj/", 0) != 0 || f.path.find(".tro") == std::string::npos) continue;
        names.push_back(f.path.substr(7, f.path.find('.') - 7));
    }
    return names;
}

// The camera list as the game reads it from a re-laid-out blob (CourseCameras::Blob) must be the same records.
bool CameraBlobRoundTrip(const CourseCameras& c) {
    if (!c.present) return true;
    constexpr uint32_t kBase = 0x1000;
    const std::vector<uint8_t> blob = c.Blob(kBase);
    return CanonicalCameraBytes(ParseCameraList(blob, kBase, kBase)) == CanonicalCameraBytes(c);
}

} // namespace

int CmdExportTracks(const GtfsVolume& vol, int argc, char** argv) {
    const std::string outDir = argv[3];
    std::string only;
    bool gltf = true;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--course" && i + 1 < argc) only = argv[++i];
        else if (a == "--no-gltf") gltf = false;
        else {
            std::printf("export-tracks: unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    std::filesystem::create_directories(outDir);
    size_t courses = 0, written = 0, failures = 0, mismatches = 0, meshes = 0;
    for (const std::string& name : DiscCourses(vol)) {
        if (!only.empty() && name != only) continue;
        courses++;
        try {
            CourseFile file = MakeCourseFile(vol, name);
            if (gltf) {
                PsxVram vram;
                vram.LoadTimPack(vol.Read("crsobj/" + name + ".trp"));
                const TrackGltfStats s = ExportTrackGltf(file.track, vram, outDir, name);
                file.meshFile = name + ".gltf";
                meshes++;
                // The glTF must be readable by the game's own reader (the mod objects use it).
                const GltfMesh back = ReadGltf(outDir + "/" + name + ".gltf");
                if (back.TriangleCount() != s.sceneTriangles)
                    throw std::runtime_error("glTF read back " + std::to_string(back.TriangleCount()) + " of " + std::to_string(s.sceneTriangles) + " scene triangles");
                std::printf("%-20s glTF %zu chunk meshes, %zu models, %zu instances, %zu scene triangles (read back %zu), atlas %dx%d from %zu regions\n", name.c_str(),
                            s.chunkMeshes, s.modelMeshes, s.instances, s.sceneTriangles, back.TriangleCount(), s.atlasWidth, s.atlasHeight, s.textureRegions);
            }
            const std::string path = outDir + "/" + name + ".json";
            WriteCourseFile(path, file);
            written++;
            // Round trip: the file as gt2game --mods reads it (ReadCourseFile + ResolveCourseFile) against the disc.
            const CourseFile back = ReadCourseFile(path);
            const ResolvedCourse r = ResolveCourseFile(vol, back, path);
            const std::vector<uint8_t> a = CanonicalTrackBytes(file.track), b = CanonicalTrackBytes(r.track);
            const bool trackSame = a == b;
            const bool raceSame = CanonicalRaceBytes(file.race) == CanonicalRaceBytes(r.race);
            const bool camSame = CanonicalCameraBytes(file.cameras) == CanonicalCameraBytes(r.cameras) && CameraBlobRoundTrip(r.cameras);
            const bool infoSame = CanonicalInfoBytes(file.info) == CanonicalInfoBytes(r.info);
            const bool texSame = r.texturePack == vol.Read("crsobj/" + name + ".trp");
            const bool clean = back.warnings.empty() && back.blocks.All();
            const bool ok = trackSame && raceSame && camSame && infoSame && texSame && clean;
            size_t records = 0;
            for (const auto& l : file.race.lists) records += l.size();
            std::printf("%-20s %s  track %s (%zu bytes)  %zu chunks, %zu instances, %zu race records, %zu cameras, %s\n", name.c_str(), ok ? "ok      " : "MISMATCH",
                        Sha1Hex(a).substr(0, 12).c_str(), a.size(), file.track.chunks.size(), file.track.sceneryInstances.size(), records, file.cameras.records.size(),
                        file.info.hasEntry ? ("\"" + file.info.name + "\"").c_str() : "(no .crsinfo entry)");
            if (!ok) {
                mismatches++;
                std::printf("  track %s, race %s, cameras %s, info %s, textures %s, file %s\n", trackSame ? "same" : "DIFFERS", raceSame ? "same" : "DIFFERS",
                            camSame ? "same" : "DIFFER", infoSame ? "same" : "DIFFERS", texSame ? "same" : "DIFFER", clean ? "clean" : "warnings / missing blocks");
                for (const std::string& d : DiffCourses(file.track, file.race, r.track, r.race, 10)) std::printf("  %s\n", d.c_str());
                for (const std::string& w : back.warnings) std::printf("  warning: %s\n", w.c_str());
            }
        } catch (const std::exception& e) {
            std::printf("%-20s FAIL: %s\n", name.c_str(), e.what());
            failures++;
        }
    }
    std::printf("export-tracks: %zu course(s), %zu json written, %zu glTF, %zu failure(s), %zu round-trip mismatch(es) -> %s\n", courses, written, meshes, failures,
                mismatches, outDir.c_str());
    return failures || mismatches || courses == 0 ? 1 : 0;
}

int CmdImportTrack(const GtfsVolume& vol, int argc, char** argv) {
    if (argc < 4) return 2;
    const std::string path = argv[3];
    const CourseFile f = ReadCourseFile(path);
    for (const std::string& w : f.warnings) std::printf("warning: %s\n", w.c_str());
    std::printf("%s: course \"%s\", base %s; blocks:%s%s%s%s%s%s%s; %zu object(s)\n", path.c_str(), f.course.c_str(), f.baseCourse.empty() ? "(none)" : f.baseCourse.c_str(),
                f.blocks.info ? " info" : "", f.blocks.start ? " start" : "", f.blocks.uvTable ? " uvTable" : "", f.blocks.chunks ? " chunks" : "",
                f.blocks.scenery ? " scenery" : "", f.blocks.race ? " race" : "", f.blocks.cameras ? " replayCameras" : "", f.objects.size());
    const ResolvedCourse r = ResolveCourseFile(vol, f, path);
    for (const std::string& n : r.notes) std::printf("note: %s\n", n.c_str());
    std::printf("resolved: %zu chunks, %zu scenery models, %zu instances, textures %s, backdrop %s, course map %s, flags 0x%04X\n", r.track.chunks.size(),
                r.track.sceneryModels.size(), r.track.sceneryInstances.size(), r.textureSource.c_str(), r.info.backdrop.empty() ? "(none)" : r.info.backdrop.c_str(),
                r.courseMap.c_str(), r.info.flags);
    for (const CourseObject& o : r.objects)
        std::printf("object %s: %zu triangles, %zu image(s), at (%.2f, %.2f, %.2f) m, rotation (%.1f, %.1f, %.1f) deg, scale %.3f\n", o.mesh.c_str(), o.loaded->TriangleCount(),
                    o.loaded->images.size(), o.position[0], o.position[1], o.position[2], o.rotation[0], o.rotation[1], o.rotation[2], o.scale);
    if (r.baseCourse.empty()) {
        std::puts("stand-alone course (no base course): nothing to diff against");
        return 0;
    }
    const CourseFile base = MakeCourseFile(vol, r.baseCourse);
    const std::vector<std::string> diff = DiffCourses(base.track, base.race, r.track, r.race, 200);
    const bool camerasSame = CanonicalCameraBytes(base.cameras) == CanonicalCameraBytes(r.cameras);
    const bool infoSame = CanonicalInfoBytes(base.info) == CanonicalInfoBytes(r.info);
    std::printf("vs disc course %s: %zu difference(s) in the course data%s%s\n", r.baseCourse.c_str(), diff.size(), camerasSame ? "" : ", replay cameras differ",
                infoSame ? "" : ", course info differs");
    for (const std::string& d : diff) std::printf("  %s\n", d.c_str());
    return 0;
}

} // namespace gt2tool
