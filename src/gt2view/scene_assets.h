#pragma once
#include "native_cockpit_fit.h"
#include "gt2view/course_surface.h"
#include "gt2view/paint_environment.h"
#include "gt2view/paint_probe_coverage.h"
#include "gt2view/billboard.h"
#include "gt2view/course_wall_visibility.h"
#include "gt2view/cockpit_eye_fit.h"
#include "gt2view/gt4_cabin_geometry.h"
#include "gt2view/gt4_showroom_pose.h"
#include "gt2view/model_window_cut.h"
#include "gt2view/model_glazing.h"
#include "gt2view/side_mirror_fit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "gt2export/car_mesh.h"
#include "gt2formats/backdrop.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/course_data.h"
#include "gt2formats/gltf_reader.h"
#include "gt2formats/model_set.h"
#include "gt2formats/sha1.h"
#include "gt2formats/psx_vram.h"
#include "gt2formats/sponsor_boards.h"
#include "gt2formats/track.h"
#include "gt2view/glow.h"
#include "gt2view/scenery_visibility.h"
#include "gt2view/scenery_replacements.h"
#include "gt2view/scenery_road_cut.h"
#include "gt2view/course_texture_seams.h"
#include "gt2view/particles.h"
#include "gt2view/scene_renderer.h"
#include "gt2vfs/gtfs.h"

namespace gt2view {

using namespace gt2;

// Owns what the native renderer needs on the GPU: the current course with its backdrop, the cars seen so far,
// the overlay.
class SceneAssets {
public:
    static constexpr uint32_t kCarSlots = 8, kCarVertexBase = 700'000, kCarVertexStride = 32'768;
    // Course vertices: static chunk shapes + scenery models below kTrackVertexLimit, this frame's billboards after.
    static constexpr uint32_t kTrackVertexLimit = 660'000, kBillboardVertexBase = 660'000, kBillboardVertexLimit = 40'000;
    static constexpr uint32_t kBackdropVertexBase = 970'000, kBackdropVertexLimit = 30'000, kOverlayVertexBase = 1'000'000;
    // This frame's tyre smoke sprites (6 vertices each, at most 256): between the car slots and the backdrop.
    static constexpr uint32_t kSmokeVertexBase = 963'000, kSmokeVertexLimit = 6'000;
    static constexpr uint32_t kMirrorVertexBase = 969'000; // the rear-view mirror's background and frame (60 vertices)
    static_assert(kCarVertexBase + kCarSlots * kCarVertexStride <= kSmokeVertexBase && kSmokeVertexBase + kSmokeVertexLimit <= kBackdropVertexBase);
    static constexpr uint32_t kOwnRowsBase = 512, kCarClutRow = 736, kOverlayRow = 1024;
    // The rims' CLUT column in a car slot's paint rows (car_mesh.h CarMeshVertex::rim; SetCarWheelTexture).
    static constexpr uint32_t kRimPalette = 16;
    // Backdrop units -> metres. The model is drawn around the camera with a zero translation, so only the
    // ratio to the far plane matters: 1 unit = 1 m puts the ~4000-unit dome inside zFar = 6000 m.
    static constexpr float kBackdropMetresPerUnit = 1.0f;
    static constexpr float kGroundDiscRadius = 4000.0f, kGroundDiscDepth = 460.0f;

    // One vertex range of the backdrop with its PS1 blend mode (kBlendOpaque for the opaque polygons).
    struct BackdropRange { uint32_t first, count, blend; };

    SceneAssets(SceneRenderer& renderer, const GtfsVolume& vol,bool cockpit=true) : renderer_(renderer), vol_(vol), ownRows_(size_t(512) * 1024, 0), loadCockpit_(cockpit) { renderer_.BeginModelTextures(); }

    void SetGt4Enabled(bool enabled) {
        gt4Enabled_ = enabled;
        if (enabled) for (size_t i = 0; i < slots_.size(); ++i) LoadGt4(int(i));
    }
    bool UsesGt4(int slot) const { return slot >= 0 && size_t(slot) < gt4Cars_.size() && (gt4Enabled_ || gt4Cars_[size_t(slot)].required) && gt4Cars_[size_t(slot)].count != 0; }
    bool HasNativeGt4Interior(int slot) const { return UsesGt4(slot) && gt4Cars_[size_t(slot)].nativeInterior; }
    uint32_t Gt4ExteriorVertices(int slot) const { return UsesGt4(slot)?gt4Cars_[size_t(slot)].count:0; }
    uint32_t Gt4CockpitVertices(int slot) const { return UsesGt4(slot)?gt4Cars_[size_t(slot)].cockpitCount:0; }
    uint32_t Gt4ReflectionVertices(int slot) const { return UsesGt4(slot)?gt4Cars_[size_t(slot)].reflectionCount:0; }
    double Gt4ExteriorRemovedArea(int slot) const { return UsesGt4(slot)?gt4Cars_[size_t(slot)].removedArea:0; }
    double Gt4UpperFrameCutArea(int slot) const { return UsesGt4(slot)?gt4Cars_[size_t(slot)].upperFrameCutArea:0; }
    uint32_t Gt4VertexCount(int slot) const { return UsesGt4(slot)?gt4Cars_[size_t(slot)].totalVertices:0; }

    // The car reflection map of a course (0x800274D4 at race load): crstim.arc entry 3, entry 5 when bit 0 of the
    // course's .crsinfo flags is set, entry 4 when bit 1 is set (bit 1 wins).
    static uint32_t EnvironmentMapEntry(uint16_t courseFlags) {
        uint32_t entry = (courseFlags & 1) ? 5 : 3;
        if (courseFlags & 2) entry = 4;
        return entry;
    }

    // Where a course's textures, backdrop and flags come from when they are not the disc course `name`'s own (a mod
    // course, docs/formats/track_json.md): the TIM pack bytes, the bgsobj/<backdrop>.bso name (empty = none) and the
    // .crsinfo-style flags (reflection map choice).
    struct TrackSources {
        std::vector<uint8_t> texturePack;
        std::string backdrop;
        uint16_t courseFlags = 0;
    };

    // `environmentMapEntry` overrides the crstim.arc entry of the car reflection map (-1 = the course's own, above).
    // `sponsors`: the race's sponsor boards (sponsor_boards.h PlaceSponsorBoards) written over the course textures
    // after the .trp, like the original's load order (0x800275E8 runs after the course pack is uploaded).
    // `sources`: a mod course's own textures / backdrop / flags instead of the disc course `name`'s.
    void UseTrack(const std::string& name, const Track& track, int environmentMapEntry = -1, const std::vector<SponsorUpload>* sponsors = nullptr,
                  const TrackSources* sources = nullptr) {
        if (name == trackName_) return;
        trackName_ = name;
        courseNight_=sources && (sources->courseFlags & 1);
        PsxVram vram;
        if (sources) vram.LoadTimPack(sources->texturePack);
        else vram.LoadTimPack(vol_.Read("crsobj/" + name + ".trp"));

        // Backdrop: .crsinfo entry -> bgsobj index (VOL directory order) -> .bso model + .bsp textures.
        backdropRanges_.clear();
        skyColor_ = {0.45f, 0.58f, 0.78f};
        try {
            if (sources) {
                if (environmentMapEntry < 0) environmentMapEntry = int(EnvironmentMapEntry(sources->courseFlags));
                if (!sources->backdrop.empty()) {
                    const Backdrop backdrop = ParseBackdrop(vol_.Read("bgsobj/" + sources->backdrop + ".bso"));
                    vram.LoadTimPack(vol_.Read("bgsobj/" + sources->backdrop + ".bsp"));
                    BuildBackdrop(backdrop);
                    std::printf("native scene: backdrop %s (%zu polygons)\n", sources->backdrop.c_str(), backdrop.polygons.size());
                }
            }
            const CourseInfoTable info = sources ? CourseInfoTable{} : ParseCourseInfo(vol_.Read(".crsinfo"));
            const int entry = sources ? -1 : info.FindByFileName(name);
            if(entry>=0)courseNight_=(info.entries[size_t(entry)].flags & 1)!=0;
            if (entry >= 0 && environmentMapEntry < 0) environmentMapEntry = int(EnvironmentMapEntry(info.entries[size_t(entry)].flags));
            std::vector<std::string> bsoPaths;
            for (const auto& f : vol_.Files())
                if (f.path.rfind("bgsobj/", 0) == 0 && f.path.size() > 7 && f.path.compare(f.path.size() - 7, 7, ".bso.gz") == 0) bsoPaths.push_back(f.path);
            if (entry >= 0) {
                const std::string bso = BackdropNameAt(bsoPaths, BackdropIndexOf(info.entries[size_t(entry)].rest));
                const Backdrop backdrop = ParseBackdrop(vol_.Read("bgsobj/" + bso + ".bso"));
                vram.LoadTimPack(vol_.Read("bgsobj/" + bso + ".bsp"));
                BuildBackdrop(backdrop);
                std::printf("native scene: backdrop %s (%zu polygons)\n", bso.c_str(), backdrop.polygons.size());
            }
        } catch (const std::exception& e) {
            std::printf("native scene: no backdrop for %s (%s)\n", name.c_str(), e.what());
        }
        // Car reflection map: a 4-bit 128 x 128 TIM of crstim.arc whose header the original rewrites to image (576, 0)
        // and CLUT (368, 511) before uploading it (0x800274D4; car_cdo_cdp.md).
        std::vector<uint16_t> words = vram.Words();
        if (sponsors && !sponsors->empty()) {
            ApplySponsorBoards(*sponsors, words);
            size_t logos = 0;
            for (const SponsorUpload& u : *sponsors) logos += u.logo >= 0 ? 1 : 0;
            std::printf("native scene: sponsor boards: %zu slots, %zu logos\n", sponsors->size(), logos);
        }
        try {
            const uint32_t arcEntry = environmentMapEntry < 0 ? 3u : uint32_t(environmentMapEntry);
            LoadEnvironmentMap(words, arcEntry);
            std::printf("native scene: reflection map crstim.arc entry %u\n", arcEntry);
        } catch (const std::exception& e) {
            std::printf("native scene: no reflection map (%s)\n", e.what());
        }
        smokeOpaque_ = {true, true};
        try {
            LoadSmokeTextures(words);
            for (size_t kind = 0; kind < 2; ++kind) {
                const auto& t = kSmokeTextures[kind];
                smokeOpaque_[kind] = SmokePaletteHasOpaque(std::span<const uint16_t>(words).subspan(size_t(t.clutY) * PsxVram::kWidth + size_t(t.clutX), 16));
            }
        } catch (const std::exception& e) {
            std::printf("native scene: no tyre smoke sprites (%s)\n", e.what());
        }
        renderer_.UploadVram(0, PsxVram::kHeight, words.data());

        nativeLights_.clear();
        auto addLight=[&](const TrackGlow& g,const std::array<float,3>& point) {
            if((g.colour & 0xffffffu)!=0)nativeLights_.push_back({point,g.colour});
        };
        for(const auto& chunk:track.chunks)for(const auto& g:chunk.road.glows)
            addLight(g,TrackVertexToWorld(chunk,{g.position[0],g.position[1],g.position[2]}));
        for(const auto& inst:track.sceneryInstances) {
            const auto& lods=track.sceneryLods.at(inst.lodList);const int lod=HighestSceneryLod(track,lods);
            if(lod<0)continue;const auto& model=track.sceneryModels.at(lods[size_t(lod)].model);
            const auto matrix=SceneryInstanceMatrix(inst,model);
            for(const auto& g:model.glows) {
                std::array<float,3> point{};
                for(size_t k=0;k<3;++k)point[k]=matrix[k]*g.position[0]+matrix[4+k]*g.position[1]+matrix[8+k]*g.position[2]+matrix[12+k];
                addLight(g,point);
            }
        }
        billboardUploads_.clear();
        BuildCourse(track, words);
        renderer_.ClearPaintProbes();paintProbeEyes_.clear();
        std::array<float,3> start{};for(size_t k=0;k<3;++k)start[k]=float(track.startGrid[0][k]/65536.);start[1]+=.8f;
        UpdatePaintProbe(start);
    }

    void UpdatePaintProbe(const std::array<float,3>& eye) {
        if(!track_)return;
        paintProbeEye_=eye;selectedPaintProbe_=0;
        NativeSceneLighting light;
        const bool covered=paintCoverage_.Covered(eye);
        const float sky=.2126f*skyColor_[0]+.7152f*skyColor_[1]+.0722f*skyColor_[2];
        const float daylight=courseNight_?0.f:std::clamp(sky*1.6f,.2f,1.f);
        const float exposure=covered?.35f:1.f;
        for(size_t k=0;k<3;++k) {
            light.ambient[k]=.24f+.34f*daylight*exposure;
            light.diffuse[k]=.12f+.52f*daylight*exposure;
            light.specular[k]=.18f+.35f*daylight*exposure;
        }
        // Glow records are the track's placed lamps, not an invented per-track
        // palette. Their position and colour affect the same frame's body light.
        float strongest=0;std::array<float,3> lampDirection{},lampColour{};
        for(const auto& lamp:nativeLights_) {
            std::array<float,3> delta{};float d2=0;
            for(size_t k=0;k<3;++k){delta[k]=lamp.point[k]-eye[k];d2+=delta[k]*delta[k];}
            const float strength=1.f/(1.f+d2/36.f);
            if(strength<=strongest)continue;strongest=strength;
            const float inverse=1.f/std::sqrt(std::max(d2,.01f));
            for(size_t k=0;k<3;++k){lampDirection[k]=delta[k]*inverse;lampColour[k]=float((lamp.colour>>(k*8))&255u)/255.f;}
        }
        const float local=std::min(.55f,strongest*.65f),mix=local/(light.diffuse[0]+local);
        for(size_t k=0;k<3;++k) {
            light.direction[k]=light.direction[k]*(1-mix)+lampDirection[k]*mix;
            // A modest colour component keeps paint recognisable under lamps.
            const float c=.85f+.15f*lampColour[k];
            light.diffuse[k]+=local*c;light.specular[k]+=local*c;
        }
        light.stockSky={skyColor_[0],skyColor_[1],skyColor_[2],exposure};
        renderer_.SetNativeSceneLighting(light);

        const auto cameras=NativeEnvironmentCameras(eye);
        std::array<std::vector<DrawItem>,3> faces;
        TrackView view;view.eye=eye;view.extendedDistance=180;view.maxDetail=false;
        view.glows=true;view.positionalBillboards=true;
        const auto oldBase=billboardBase_;
        for(size_t face=0;face<3;++face) {
            billboardBase_=SceneRenderer::kPaintProbeVertexBase+uint32_t(face)*kBillboardVertexLimit;
            const auto& m=cameras[face];
            for(size_t k=0;k<3;++k){view.forward[k]=m[k*4+3];view.right[k]=m[k*4]/.31980f;}
            std::vector<DrawItem> blended;
            // As in GT4, far sky artwork is not pasted into the local map. The
            // reduced course views draw actual visible roofs, lamps and scenery.
            AppendTrackItems(faces[face],blended,m.data(),view);
            faces[face].insert(faces[face].end(),blended.begin(),blended.end());
            for(auto& item:faces[face])item.space=kSpaceMirrorSource;
        }
        billboardBase_=oldBase;
        renderer_.QueueNativeEnvironment(faces,cameras);
    }
    bool PaintCoveredForAudit(const std::array<float,3>& eye) const {return paintCoverage_.Covered(eye);}
    int PaintProbeKeyForAudit() const {return selectedPaintProbe_;}
    bool SelectedPaintProbeCoveredForAudit() const {return paintCoverage_.Covered(paintProbeEye_);}

    // What the camera sees of the course in one frame.
    struct TrackView {
        std::array<float, 3> eye{};          // world metres (x, y up, z)
        bool positionalBillboards = false;
        std::array<float, 3> right{1, 0, 0}; // camera right axis in world axes (billboards face the camera's yaw)
        std::array<float, 3> forward{0, 0, 1}; // camera forward axis in world axes (the scenery LOD's camera space)
        double projectionDistance = 256;     // the view as a PS1 projection distance on the 320 x 240 frame (scenery LOD: track.h SceneryLodK)
        // true: local scenery uses its most detailed model without a distance cut-off.
        // Broad course proxies retain authored visibility (scenery_visibility.h).
        bool maxDetail = false;
        // 0x800A951C != 0 (attract race / replay): every render-list entry draws its chunk. 0 (a player's race): the
        // entries with flags 3 draw only their glow records (0x80020110: `flag800A951C == 0 && flags > 2`).
        bool fullDetail = true;
        int cameraChunk = -1;                // the chunk whose render list is used; -1 = the chunk nearest to the eye
        bool glows = true;                   // the chunks' glow records (night / highway courses; glow.h)
        // Ours (graphics option "Draw Distance"): 0 = the camera chunk's render list only (the original); N > 0 = plus
        // every chunk whose centre is within N m of the eye, plus scenery in that radius without PS1 masks/LOD cutoffs.
        // The added chunks draw like render-list entries.
        // < 0 = every chunk and authored near scenery entry, including empty distant-copy entries.
        float extendedDistance = 0;
    };
    struct TrackStats { int cameraChunk = -1; size_t chunks = 0, instances = 0, billboards = 0; uint32_t mask = 0; };

    // Appends the course's draw items for this frame, reproducing the original's selection (0x80020110 /
    // 0x8002002C / 0x8001F7F8): the chunks of the camera chunk's render list (road shape + billboards - never the
    // mirror's low-detail copy), the scenery instances of list 32 and of the lists whose bit is set in the OR of the
    // render-list chunks' masks (the level of detail the original picks, or the most detailed with `maxDetail`), plus their billboards. Opaque items go to `opaque`; the blended parts of semi-transparent
    // polygons to `blended`, back to front. `vp` = view-projection (column-major, world metres).
    void AppendTrackItems(std::vector<DrawItem>& opaque, std::vector<DrawItem>& blended, const float* vp, const TrackView& view) {
        stats_ = {};
        if (!track_) return;
        const Track& track = *track_;
        const int camera = view.cameraChunk >= 0 && size_t(view.cameraChunk) < track.chunks.size() ? view.cameraChunk : NearestChunk(view.eye);
        stats_.cameraChunk = camera;
        std::vector<uint16_t> entries = track.chunks[size_t(camera)].renderList;
        if (entries.empty())
            for (size_t i = 0; i < track.chunks.size(); i++) entries.push_back(uint16_t(i));
        std::vector<bool> authoredVisible(track.chunks.size(),false);
        for(auto e:entries)if((e&0x3fff)<authoredVisible.size() && (e>>14)<=2)authoredVisible[e&0x3fff]=true;
        ExtendTrackEntries(entries,track,view.eye,view.extendedDistance);
        // The camera's screen-down axis for the glow stars (glow.h): forward x right (the PS1 camera frame right, down, forward
        // is right-handed; e.g. forward +z, right -x -> down -y).
        const float down[3] = {view.forward[1] * view.right[2] - view.forward[2] * view.right[1], view.forward[2] * view.right[0] - view.forward[0] * view.right[2],
                               view.forward[0] * view.right[1] - view.forward[1] * view.right[0]};
        float rx = view.right[0], rz = view.right[2];
        const float rl = std::sqrt(rx * rx + rz * rz);
        if (rl > 1e-6f) { rx /= rl; rz /= rl; } else { rx = 1; rz = 0; }

        std::array<std::vector<SceneVertex>, 5> boards; // billboards: [4] opaque, [m] semi-transparent mode m
        std::vector<std::pair<float, DrawItem>> blendedItems;
        auto distanceTo = [&](const std::array<float, 3>& p) {
            const float dx = p[0] - view.eye[0], dy = p[1] - view.eye[1], dz = p[2] - view.eye[2];
            return std::sqrt(dx * dx + dy * dy + dz * dz);
        };
        auto addRanges = [&](const ShapeRanges& r, const float* mvp, float distance) {
            DrawItem item;
            std::copy(mvp, mvp + 16, item.mvp);
            if (r.opaque.count) {
                item.firstVertex = r.opaque.first;
                item.vertexCount = r.opaque.count;
                opaque.push_back(item);
            }
            for (uint32_t m = 0; m < 4; m++) {
                if (!r.semi[m].count) continue;
                DrawItem part = item;
                part.firstVertex = r.semi[m].first;
                part.vertexCount = r.semi[m].count;
                part.stpPass = 1; // texels without STP: opaque
                opaque.push_back(part);
                part.blend = m;
                part.stpPass = 2; // texels with STP (and untextured polygons): blended
                blendedItems.push_back({distance, part});
            }
        };
        auto addBoard = [&](const std::array<float, 3> (&corners)[4], const TrackBillboard& b) {
            const bool semi = (b.primCode & 0x02) != 0;
            std::vector<SceneVertex>& out = boards[semi ? size_t((b.tpage >> 5) & 3) : 4];
            static constexpr size_t kOrder[6] = {0, 1, 3, 0, 3, 2}; // GPU corners 0 TL, 1 TR, 2 BL, 3 BR
            for (size_t k : kOrder) {
                SceneVertex o{};
                std::copy(corners[k].begin(), corners[k].end(), o.pos);
                for (size_t c = 0; c < 3; c++) o.color[c] = b.color[c] / 255.0f;
                SetTexture(o, b.tpage, b.clut, b.u[k], b.v[k]);
                if (b.primCode & 0x01) o.flags |= kRawTexture;
                if (semi) o.flags |= kSemiTransparent;
                out.push_back(o);
            }
            stats_.billboards++;
        };

        uint32_t mask = 0;
        const bool retainMaskedWalls=std::getenv("GT2_COURSE_RETAIN_MASKED_WALLS")!=nullptr;
        std::vector<bool> detailedDrawn(track.chunks.size(),false);
        for (uint16_t e : entries) {
            const size_t index = e & 0x3FFF;
            const uint32_t flags = uint32_t(e >> 14);
            if (index >= track.chunks.size()) continue;
            const TrackChunk& chunk = track.chunks[index];
            mask |= chunk.sceneryMask; // before the view test, for every entry (0x80020110)
            if (view.glows) AppendChunkGlowSprites(boards[1], track, chunk, view.right.data(), down); // every drawn entry draws its glows (glow.h)
            if (view.extendedDistance >= 0 && !view.fullDetail && flags > 2) continue; // glow records only
            detailedDrawn[index]=true;
            stats_.chunks++;
            const std::array<float, 3> centre = {float(chunk.centre[0] / 65536.0), float(chunk.centre[1] / 65536.0), float(chunk.centre[2] / 65536.0)};
            const bool masked=view.extendedDistance!=0 && !authoredVisible[index] && !retainMaskedWalls &&
                std::any_of(wallConflicts_[index].begin(),wallConflicts_[index].end(),[&](size_t other){return authoredVisible[other];});
            addRanges(masked?extendedChunkRanges_[index]:chunkRanges_[index], vp, distanceTo(centre));
            for (const TrackBillboard& b : chunk.billboards) {
                const std::array<float, 3> base = TrackVertexToWorld(chunk, {int16_t(b.position[0]), int16_t(b.position[1]), int16_t(b.position[2])});
                const auto boardRight = view.positionalBillboards ? BillboardRight(view.eye,base,{1,0,0}) : std::array<float,3>{rx,0,rz};
                const float boardRx=boardRight[0],boardRz=boardRight[2];
                const float half = float(b.width) / 128.0f, top = float(b.height) / 64.0f; // 1/64 m, half of the width
                const std::array<float, 3> corners[4] = {{base[0] - half * boardRx, base[1] + top, base[2] - half * boardRz},
                                                         {base[0] + half * boardRx, base[1] + top, base[2] + half * boardRz},
                                                         {base[0] - half * boardRx, base[1], base[2] - half * boardRz},
                                                         {base[0] + half * boardRx, base[1], base[2] + half * boardRz}};
                addBoard(corners, b);
            }
        }
        stats_.mask = mask;
        // The extended scenery range bypasses PS1 visibility masks and the final LOD cutoff.
        // The camera rows of the original's camera matrix (right, down, forward) for the scenery LOD measure.
        std::array<std::array<double, 3>, 3> cameraAxes{};
        {
            const std::array<float, 3>& r = view.right;
            const std::array<float, 3>& f = view.forward;
            const std::array<float, 3> d = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]}; // right x forward = down
            for (size_t c = 0; c < 3; c++) { cameraAxes[0][c] = r[c]; cameraAxes[1][c] = d[c]; cameraAxes[2][c] = f[c]; }
        }
        const std::array<double, 3> eye = {view.eye[0], view.eye[1], view.eye[2]};
        const int32_t projection = int32_t(std::lround(view.projectionDistance));
        for (size_t instanceIndex=0;instanceIndex<track.sceneryInstances.size();++instanceIndex) {
            const auto& inst=track.sceneryInstances[instanceIndex];
            const std::array<double, 3> offset = {inst.position[0] / 65536.0 - view.eye[0], inst.position[1] / 65536.0 - view.eye[1],
                                                  inst.position[2] / 65536.0 - view.eye[2]};
            // Level of detail as the original picks it (0x8001F7F8 -> 0x8007AE38 / 0x8007AEF4, track.h): the camera-space
            // translation squared in quarter metres, scaled by k(h, lodDivisor), against the list's thresholds; beyond the
            // last threshold the instance is not drawn. gt2play --prims checks this rule against the original's own calls.
            const std::vector<TrackLodEntry>& lods = track.sceneryLods[inst.lodList];
            const int entry = SceneryEntry(track, inst, mask,
                SceneryLodMeasure(SceneryCameraSpace(inst, eye, cameraAxes), SceneryLodK(projection, inst.lodDivisor)),
                offset[0]*offset[0] + offset[1]*offset[1] + offset[2]*offset[2], view.extendedDistance, view.maxDetail);
            if (entry < 0) continue;
            const size_t modelIndex = lods[entry].model;
            const TrackSceneryModel& model = track.sceneryModels[modelIndex];
            const std::array<float, 16> m = SceneryInstanceMatrix(inst, model);
            float mvp[16];
            MultiplyColumnMajor(vp, m.data(), mvp);
            const float instanceDistance=float(std::sqrt(offset[0]*offset[0]+offset[1]*offset[1]+offset[2]*offset[2]));
            const auto& replacement=instanceReplacements_[instanceIndex];
            if(replacement.model!=modelIndex || replacement.polygons.empty()) addRanges(modelRanges_[modelIndex],mvp,instanceDistance);
            else if(replacement.hasDetailed && DetailedReplacementVisible(replacement.chunks,detailedDrawn))
                addRanges(replacement.detailed,mvp,instanceDistance);
            else {
                const auto& source=replacement.source;
                uint32_t cursor=source.opaque.first;
                for(const auto& polygon:replacement.polygons) {
                    if(!DetailedReplacementVisible(polygon.chunks,detailedDrawn)) continue;
                    if(polygon.range.first>cursor) {
                        ShapeRanges part{};part.opaque={cursor,polygon.range.first-cursor};
                        addRanges(part,mvp,instanceDistance);
                    }
                    if(polygon.clipped.count){ShapeRanges part{};part.opaque=polygon.clipped;addRanges(part,mvp,instanceDistance);}
                    cursor=polygon.range.first+polygon.range.count;
                }
                ShapeRanges tail=source;
                tail.opaque={cursor,source.opaque.first+source.opaque.count-cursor};
                addRanges(tail,mvp,instanceDistance);
            }
            stats_.instances++;
            if (view.glows) AppendModelGlowSprites(boards[1], model, m.data(), view.right.data(), down); // 0x8001FBA8 (glow.h)
            for (const TrackBillboard& b : model.billboards) {
                // 0x8001F7F8: the half-extent is built in the model's axes from the camera yaw (hx, -hz), so a
                // rotated instance turns its billboards with it, like the original.
                float hx = float(b.width) * 0.5f * rx, hz = -float(b.width) * 0.5f * rz;
                if(view.positionalBillboards) {
                    std::array<float,3> base{};
                    for(int k=0;k<3;++k)base[k]=m[k]*b.position[0]+m[4+k]*b.position[1]+m[8+k]*b.position[2]+m[12+k];
                    const auto axis=BillboardRight(view.eye,base,{1,0,0});
                    const float scale=std::sqrt(m[0]*m[0]+m[1]*m[1]+m[2]*m[2]);
                    if(scale>1e-6f) {
                        hx=float(b.width)*.5f*(axis[0]*m[0]+axis[2]*m[2])/scale;
                        hz=float(b.width)*.5f*(axis[0]*m[8]+axis[2]*m[10])/scale;
                    }
                }
                const float px = float(b.position[0]), py = float(b.position[1]), pz = float(b.position[2]), top = py + float(b.height);
                const float local[4][3] = {{px - hx, top, pz - hz}, {px + hx, top, pz + hz}, {px - hx, py, pz - hz}, {px + hx, py, pz + hz}};
                std::array<float, 3> corners[4];
                for (size_t k = 0; k < 4; k++)
                    for (size_t r = 0; r < 3; r++) corners[k][r] = m[r] * local[k][0] + m[4 + r] * local[k][1] + m[8 + r] * local[k][2] + m[12 + r];
                addBoard(corners, b);
            }
        }
        // Billboards: this frame's vertices in their own range, one item per list.
        std::vector<SceneVertex> all;
        std::array<Range, 5> boardRanges{};
        for (size_t l : {size_t(4), size_t(0), size_t(1), size_t(2), size_t(3)}) {
            if (all.size() + boards[l].size() > billboardLimit_) break;
            boardRanges[l] = {billboardBase_ + uint32_t(all.size()), uint32_t(boards[l].size())};
            all.insert(all.end(), boards[l].begin(), boards[l].end());
        }
        if(!all.empty() && billboardBase_>=SceneRenderer::kPaintProbeVertexBase) {
            auto& previous=billboardUploads_[billboardBase_];
            if(previous.size()!=all.size() || std::memcmp(previous.data(),all.data(),all.size()*sizeof(SceneVertex))) {
                renderer_.SetVertices(billboardBase_,all);previous=all;
            }
        } else if(!all.empty())renderer_.SetVertices(billboardBase_,all);
        ShapeRanges boardShape;
        boardShape.opaque = boardRanges[4];
        for (size_t mode = 0; mode < 4; mode++) boardShape.semi[mode] = boardRanges[mode];
        addRanges(boardShape, vp, 0.0f);
        // Semi-transparent parts back to front (the original's ordering table).
        std::stable_sort(blendedItems.begin(), blendedItems.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& b : blendedItems) blended.push_back(b.second);
    }
    const TrackStats& LastTrackStats() const { return stats_; }
    size_t CourseVertexCount() const {return courseVertexCount_;}
    size_t DroppedCourseVertices() const {return droppedCourseVertices_;}

    // The rear-view mirror's course (0x8002993C -> 0x80020110 with param_3 = 1): the low-detail copies (chunk + 0x94) of
    // the camera chunk's render-list chunks nearer than `maxDistance` metres, no scenery, no billboards; all items get the
    // mirror's scissor rectangle (window fractions). `vp` = the mirror's clip matrix mapped into that rectangle.
    void AppendMirrorTrackItems(std::vector<DrawItem>& opaque, std::vector<DrawItem>& blended, const float* vp, const std::array<float, 3>& eye,
                                int cameraChunk, float maxDistance, const float scissor[4]) const {
        if (!track_ || mirrorRanges_.size() != track_->chunks.size()) return;
        const Track& track = *track_;
        const int camera = cameraChunk >= 0 && size_t(cameraChunk) < track.chunks.size() ? cameraChunk : NearestChunk(eye);
        std::vector<uint16_t> entries = track.chunks[size_t(camera)].renderList;
        if (entries.empty())
            for (size_t i = 0; i < track.chunks.size(); i++) entries.push_back(uint16_t(i));
        std::vector<std::pair<float, DrawItem>> semi;
        for (uint16_t e : entries) {
            const size_t index = e & 0x3FFF;
            if (index >= track.chunks.size()) continue;
            const TrackChunk& chunk = track.chunks[index];
            const float dx = float(chunk.centre[0] / 65536.0) - eye[0], dy = float(chunk.centre[1] / 65536.0) - eye[1], dz = float(chunk.centre[2] / 65536.0) - eye[2];
            const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (distance > maxDistance) continue;
            const ShapeRanges& r = mirrorRanges_[index];
            DrawItem item;
            std::copy(vp, vp + 16, item.mvp);
            std::copy(scissor, scissor + 4, item.scissor);
            if (r.opaque.count) {
                item.firstVertex = r.opaque.first;
                item.vertexCount = r.opaque.count;
                opaque.push_back(item);
            }
            for (uint32_t m = 0; m < 4; m++) {
                if (!r.semi[m].count) continue;
                DrawItem part = item;
                part.firstVertex = r.semi[m].first;
                part.vertexCount = r.semi[m].count;
                part.stpPass = 1;
                opaque.push_back(part);
                part.blend = m;
                part.stpPass = 2;
                semi.push_back({distance, part});
            }
        }
        std::stable_sort(semi.begin(), semi.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& s : semi) blended.push_back(s.second);
    }

    // The mirror's background and frame (0x800294D4): the backdrop's two flat colours (0x800AF234 + 4 above the horizon,
    // + 8 below; the mirror draws no dome) split at the horizon row `horizonY` (window fraction, y down) and the black
    // outlines of (0, 0, 120, 32) and (1, 1, 118, 30) (0x8007E780), inside `rect` (window fractions x0, y0, x1, y1).
    // `border` = one frame pixel in window fractions (x, y). The background carries clearDepth (the course and cars of
    // the mirror are drawn after it); the frame goes to `frame` (drawn after the mirror's scene).
    void AppendMirrorFrameItems(std::vector<DrawItem>& background, std::vector<DrawItem>& frame, const float rect[4], float horizonY, const float border[2]) {
        std::vector<SceneVertex> v;
        auto quad = [&](float x0, float y0, float x1, float y1, const std::array<float, 3>& c, float z) {
            const float xs[6] = {x0, x1, x1, x0, x1, x0}, ys[6] = {y0, y0, y1, y0, y1, y1};
            for (int k = 0; k < 6; k++) {
                SceneVertex o{};
                o.pos[0] = xs[k] * 2 - 1;
                o.pos[1] = ys[k] * 2 - 1;
                o.pos[2] = z;
                std::copy(c.begin(), c.end(), o.color);
                v.push_back(o);
            }
        };
        const float hy = std::clamp(horizonY, rect[1], rect[3]);
        quad(rect[0], rect[1], rect[2], hy, skyColor_, 0.0f);       // z 0 = the far end of reversed Z: behind everything
        quad(rect[0], hy, rect[2], rect[3], groundColor_, 0.0f);
        const size_t backgroundCount = v.size();
        const std::array<float, 3> black{0, 0, 0};
        for (int ring = 0; ring < 2; ring++) {
            const float x0 = rect[0] + border[0] * float(ring), y0 = rect[1] + border[1] * float(ring);
            const float x1 = rect[2] - border[0] * float(ring), y1 = rect[3] - border[1] * float(ring);
            quad(x0, y0, x1, y0 + border[1], black, 1.0f);
            quad(x0, y1 - border[1], x1, y1, black, 1.0f);
            quad(x0, y0, x0 + border[0], y1, black, 1.0f);
            quad(x1 - border[0], y0, x1, y1, black, 1.0f);
        }
        renderer_.SetVertices(kMirrorVertexBase, v);
        DrawItem item;
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::copy(identity, identity + 16, item.mvp);
        std::copy(rect, rect + 4, item.scissor);
        item.firstVertex = kMirrorVertexBase;
        item.vertexCount = uint32_t(backgroundCount);
        item.clearDepth = 1;
        background.push_back(item);
        item.clearDepth = 0;
        item.firstVertex = kMirrorVertexBase + uint32_t(backgroundCount);
        item.vertexCount = uint32_t(v.size() - backgroundCount);
        frame.push_back(item);
    }

    // Returns the slot of the car, loading it on first sight; -1 when all slots are taken. `alias` (optional) loads the car
    // into a slot of its own under that name (the game mode 6 ghost: the player's car with its own reflection pass).
    // `wheels` (optional): the wheel area the race's car setup generates (car_mesh.h GenerateWheelArea from the body's
    // wheel dimensions, 0x80017FA0); without it the .cdo defaults of 0x80061504. It applies when the slot is loaded.
    struct RaceWheelGeometry {
        std::array<float,2> radius{},rideHeight{};
    };
    int UseCar(const std::string& id, const std::string& alias = std::string(), const WheelArea* wheels = nullptr,
               const RaceWheelGeometry* raceWheels = nullptr) {
        const std::string& key = alias.empty() ? id : alias;
        for (size_t i = 0; i < slots_.size(); i++)
            if (slots_[i].id == key) return int(i);
        if (slots_.size() >= carSlotLimit_) return -1;
        const uint32_t slot = uint32_t(slots_.size());
        CarModel model = ParseCarModel(vol_.Read("carobj/" + id + ".cdo"));
        CarTexture texture = ParseCarTexture(vol_.Read("carobj/" + id + ".cdp"));

        std::vector<SceneVertex> vertices;
        CarMeshOptions meshOptions;
        meshOptions.wheelArea = wheels;
        // the wheels around their own origin, each drawn with its transform (AppendCarItems: steer, camber, rolling angle)
        std::array<std::array<float, 3>, 4> wheelCentres{};
        std::array<std::array<uint32_t, 2>, 4> wheelRanges{};
        meshOptions.wheelsAtOrigin = true;
        meshOptions.wheelCentres = &wheelCentres;
        meshOptions.wheelRanges = &wheelRanges;
        for (const CarMeshVertex& v : BuildCarMesh(model, meshOptions)) {
            SceneVertex o{};
            std::copy(v.pos, v.pos + 3, o.pos);
            std::copy(v.texel, v.texel + 2, o.texel);
            std::copy(v.color, v.color + 3, o.color);
            o.page = (slot * 64) | (kOwnRowsBase << 16);
            o.clut = uint32_t(v.rim ? kRimPalette : v.palette) * 16 | ((kCarClutRow + slot * 16) << 16);
            o.flags = (v.textured ? kTextured : 0u) | (v.rawTexture ? kRawTexture : 0u) | kCarPaint | (v.cullBack ? kCullBack : 0u); // cullBack: wheel strips (car_mesh.h)
            if (std::getenv("GT2_DEBUG_CABIN") && wheelRanges[0][1] && vertices.size() >= wheelRanges[0][0]) {
                o.flags=0;o.color[0]=0;o.color[1]=.45f;o.color[2]=1;
            }
            if (std::getenv("GT2_AUDIT_WHEEL_COLOR") && wheelRanges[0][1] && vertices.size() >= wheelRanges[0][0]) {
                // Keep the rim's transparent corners and the tread's winding.
                // Recolour only fragments that the normal renderer retains.
                o.flags|=kAuditWheelColor;
            }
            vertices.push_back(o);
        }
        if (vertices.size() > kCarVertexStride) vertices.resize(kCarVertexStride);
        // The ground shadow follows the body in the slot's range (drawn separately, blend mode 2).
        const uint32_t bodyCount = uint32_t(vertices.size());
        for (const CarMeshVertex& v : BuildCarShadowMesh(model, CarShadowHeight(model))) {
            if (vertices.size() >= kCarVertexStride) break;
            SceneVertex o{};
            std::copy(v.pos, v.pos + 3, o.pos);
            std::copy(v.color, v.color + 3, o.color);
            vertices.push_back(o);
        }
        if(raceWheels){
            const std::vector<SceneVertex> shadow(vertices.begin()+bodyCount,vertices.end());
            vertices.resize(bodyCount);
            const auto subdivide=[&](auto&& self,const SceneVertex& a,const SceneVertex& b,const SceneVertex& c,int depth)->void {
                const auto length=[](const auto& p,const auto& q){return std::hypot(p.pos[0]-q.pos[0],p.pos[2]-q.pos[2]);};
                const float ab=length(a,b),bc=length(b,c),ca=length(c,a);
                if(depth==0 || std::max({ab,bc,ca})<=.4f){vertices.insert(vertices.end(),{a,b,c});return;}
                const SceneVertex *p=&a,*q=&b,*other=&c;
                if(bc>ab && bc>=ca){p=&b;q=&c;other=&a;}else if(ca>ab && ca>bc){p=&c;q=&a;other=&b;}
                SceneVertex mid=*p;
                for(size_t k=0;k<3;++k){mid.pos[k]=(p->pos[k]+q->pos[k])*.5f;mid.color[k]=(p->color[k]+q->color[k])*.5f;}
                self(self,*p,mid,*other,depth-1);self(self,mid,*q,*other,depth-1);
            };
            for(size_t i=0;i+2<shadow.size();i+=3)subdivide(subdivide,shadow[i],shadow[i+1],shadow[i+2],8);
            if(vertices.size()>kCarVertexStride)throw std::runtime_error("road shadow exceeds car slot");
        }
        renderer_.SetVertices(kCarVertexBase + slot * kCarVertexStride, vertices);

        for (size_t i = 0; i < texture.indices.size(); i++) {
            const size_t x = i % CarTexture::kWidth, y = i / CarTexture::kWidth;
            uint16_t& word = ownRows_[y * 1024 + slot * 64 + x / 4];
            if ((x & 3) == 0) word = 0;
            word |= uint16_t(texture.indices[i] << ((x & 3) * 4));
        }
        for (size_t p = 0; p < texture.paints.size() && p < 16; p++)
            for (size_t c = 0; c < 16; c++)
                for (size_t k = 0; k < 16; k++)
                    ownRows_[(kCarClutRow - kOwnRowsBase + slot * 16 + p) * 1024 + c * 16 + k] = texture.paints[p].cluts[c][k];
        for (size_t p = 0; p < texture.paints.size() && p < 16; p++) // the rims' own CLUT: CLUT 0 of the paint (the car's wheels)
            for (size_t k = 0; k < 16; k++) ownRows_[(kCarClutRow - kOwnRowsBase + slot * 16 + p) * 1024 + kRimPalette * 16 + k] = texture.paints[p].cluts[0][k];
        slots_.push_back({key, bodyCount, uint32_t(vertices.size()) - bodyCount, 0, std::move(texture), std::move(model)});
        if(raceWheels) {slots_.back().raceWheels=*raceWheels;slots_.back().hasRaceWheels=true;}
        slots_.back().roadShadow.assign(vertices.begin()+bodyCount,vertices.end());
        {
            auto& s=slots_.back();std::map<std::array<float,3>,size_t> samples;
            for(const auto& v:s.roadShadow){
                const std::array<float,3> p{v.pos[0],v.pos[1],v.pos[2]};
                const auto [it,inserted]=samples.emplace(p,s.shadowSamples.size());
                if(inserted)s.shadowSamples.push_back(p);
                s.shadowSampleIndex.push_back(it->second);
            }
        }
        gt4Cars_[slot].sourceId = id;
        gt4Cars_[slot].required = vol_.IsExpansionAsset("carobj/" + id + ".cdo");
        slots_.back().shadowHeight = CarShadowHeight(slots_.back().model);
        slots_.back().cockpit = FitCockpit(slots_.back().model);
        if (slots_.back().cockpit.valid && loadCockpit_) {
            const auto body = BuildCockpitBody(slots_.back().model, slots_.back().texture, slots_.back().cockpit,
                !(std::getenv("GT2_AUDIT_COCKPIT") && std::getenv("GT2_AUDIT_REMOVE_STRUCTURE")));
            FitCockpitSideSills(slots_.back().cockpit,body.windowOpenings);
            RefineCockpitEye(slots_.back().cockpit, body.vertices, slots_.back().texture, body.glassMasks);
            FitCockpitMirror(slots_.back().cockpit, slots_.back().model);
            // The retained GT2 roof/pillars already define the window frame.
            // A generic rounded reveal cuts across curved authored apertures.
            slots_.back().cockpit.authoredUpperShell=true;
            slots_.back().cockpitExterior = body.exteriorPolygons;
            if (body.vertices.size() > SceneRenderer::kCockpitBodyVertexStride-384)
                throw std::runtime_error("scene: cockpit body exceeds reserved vertex range");
            const auto& paints = slots_.back().texture.paints;
            for (size_t p = 0; p < paints.size() && p < 16; ++p)
                for (size_t c = 0; c < 16; ++c)
                    for (size_t k = 0; k < 16; ++k)
                        ownRows_[(kCarClutRow - kOwnRowsBase + slot * 16 + p) * 1024 + (kCockpitGlazingPalette + c) * 16 + k] =
                            (body.glassMasks[c] & (1u << k)) ? 0 : paints[p].cluts[c][k];
            for (size_t p = 0; p < paints.size() && p < 16; ++p)
                for (size_t c = 0; c < 16; ++c)
                    for (size_t k = 0; k < 16; ++k)
                        ownRows_[(kCarClutRow - kOwnRowsBase + slot * 16 + p) * 1024 + (kCockpitLiningPalette + c) * 16 + k] =
                            paints[p].cluts[c][k] ? kCockpitTrimColor : 0;
            std::vector<SceneVertex> bodyVertices;
            bodyVertices.reserve(body.vertices.size());
            for (const auto& v : body.vertices) {
                SceneVertex o{};
                std::copy(v.pos, v.pos + 3, o.pos);
                std::copy(v.texel, v.texel + 2, o.texel);
                std::copy(v.color, v.color + 3, o.color);
                o.page = (slot * 64) | (kOwnRowsBase << 16);
                o.clut = uint32_t(v.palette) * 16 | ((kCarClutRow + slot * 16) << 16);
                o.flags = (v.textured ? kTextured : 0u) | (v.rawTexture ? kRawTexture : 0u) | kCarPaint;
                bodyVertices.push_back(o);
            }
            slots_.back().cockpitBodyCount = uint32_t(bodyVertices.size());
            renderer_.SetVertices(SceneRenderer::kCockpitBodyVertexBase + slot * SceneRenderer::kCockpitBodyVertexStride, bodyVertices);
            const auto mirrors=StockSideMirrorGlass(slots_.back().model,slots_.back().texture,slots_.back().cockpit);
            if(mirrors.size()>384)throw std::runtime_error("stock side mirrors exceed reserved range");
            slots_.back().cockpitMirrorCount=uint32_t(mirrors.size());
            FitMirrorSightlines(slots_.back().cockpit,mirrors);
            renderer_.SetVertices(SceneRenderer::kCockpitBodyVertexBase+(slot+1)*SceneRenderer::kCockpitBodyVertexStride-384,mirrors);
        }
        renderer_.UploadVram(kOwnRowsBase, 512, ownRows_.data());
        if (wheelRanges[3][0] + wheelRanges[3][1] <= bodyCount && wheelRanges[0][1] != 0) {
            slots_.back().wheelRanges = wheelRanges;
            slots_.back().wheelCentres = wheelCentres;
            slots_.back().separateWheels = true;
            // Race tyre upgrades can be wider than the .cdo defaults. Fit the
            // lining to the wheel mesh actually uploaded, including its caps.
            std::vector<std::array<float,3>> shell;
            shell.reserve(wheelRanges[0][0]);
            for (uint32_t v=0;v<wheelRanges[0][0];++v)
                shell.push_back({vertices[v].pos[0],vertices[v].pos[1],vertices[v].pos[2]});
            std::map<std::pair<int,int>,std::array<float,3>> support;
            for(const auto& p:shell)if(p[1]<.35f){
                const auto cell=std::make_pair(int(std::floor(p[0]/.3f)),int(std::floor(p[2]/.3f)));
                const auto it=support.find(cell);
                if(it==support.end() || p[1]<it->second[1])support[cell]=p;
            }
            for(const auto& [cell,p]:support){(void)cell;slots_.back().roadSupport.push_back(p);}
            for (size_t w=0;w<4;++w) {
                auto& well=slots_.back().cockpit.wheelWells[w];
                well[3]=well[4]=0;
                for (uint32_t v=wheelRanges[w][0];v<wheelRanges[w][0]+wheelRanges[w][1];++v) {
                    const auto& p=vertices[v].pos;
                    well[3]=std::max(well[3],std::sqrt(p[1]*p[1]+p[2]*p[2]));
                    well[4]=std::max(well[4],2.f*std::abs(p[0]));
                }
                slots_.back().wheelBumpStop[w]=ModelWheelBumpStop(shell,well);
            }
        }
        std::printf("native scene: car %s -> slot %u (%u body + %u shadow vertices)\n", key.c_str(), slot, bodyCount, uint32_t(vertices.size()) - bodyCount);
        if (gt4Enabled_ || gt4Cars_[slot].required) LoadGt4(int(slot));
        return int(slot);
    }

    // Fitted wheels on a loaded car slot, as 0x800678E8 loads a carwheel/ TIM (game/menu/menu_car.h MenuWheelTexture):
    // the 4-bit image (`words` halfwords x `rows`) over the top-left texels of the slot's texture and the 16-colour CLUT
    // as the rims' CLUT in every paint row. `image` empty = the car's own texels and CLUT 0 back.
    void SetCarWheelTexture(int slot, const std::vector<uint16_t>& image, int words, int rows, const std::array<uint16_t, 16>& clut) {
        if (slot < 0 || size_t(slot) >= slots_.size()) return;
        const CarTexture& texture = slots_[size_t(slot)].texture;
        const size_t s = size_t(slot);
        if (image.empty()) { // restore: the texels of the region the wheels cover and CLUT 0
            for (size_t y = 0; y < 48 && y < size_t(CarTexture::kHeight); y++)
                for (size_t x = 0; x < 48; x++) {
                    uint16_t& word = ownRows_[y * 1024 + s * 64 + x / 4];
                    word = uint16_t((word & ~(0xF << ((x & 3) * 4))) | (texture.indices[y * size_t(CarTexture::kWidth) + x] << ((x & 3) * 4)));
                }
            for (size_t p = 0; p < texture.paints.size() && p < 16; p++)
                for (size_t k = 0; k < 16; k++) ownRows_[(kCarClutRow - kOwnRowsBase + s * 16 + p) * 1024 + kRimPalette * 16 + k] = texture.paints[p].cluts[0][k];
        } else {
            for (int y = 0; y < rows && y < int(CarTexture::kHeight); y++)
                for (int x = 0; x < words && x < 64; x++) ownRows_[size_t(y) * 1024 + s * 64 + size_t(x)] = image[size_t(y) * size_t(words) + size_t(x)];
            for (size_t p = 0; p < 16; p++)
                for (size_t k = 0; k < 16; k++) ownRows_[(kCarClutRow - kOwnRowsBase + s * 16 + p) * 1024 + kRimPalette * 16 + k] = clut[k];
        }
        renderer_.UploadVram(kOwnRowsBase, 512, ownRows_.data());
    }

    // A car whose body is an external glTF mesh (mods, docs/formats/car_json.md). Positions are metres in the car
    // axes (+X right, +Y up, -Z front) after `scale`; textured primitives sample their material's baseColor image
    // from the renderer's external texture store (kExternalTexture; nearest texel, alpha mask); untextured ones
    // use vertex colour x baseColorFactor. Wheels are generated like the disc cars' (car_mesh.h BuildWheelMesh)
    // at the given left-wheel centres (the right side mirrors x) with a flat grey rim face; the ground shadow is
    // a soft rectangle under the bounding box. A mesh larger than one slot's vertex range takes the following
    // slots' ranges too (placeholder slots). Returns the slot, -1 when the slots are exhausted.
    struct ExternalCar {
        const GltfMesh* mesh = nullptr;
        float scale = 1.0f;
        std::array<float, 3> wheelFront{-0.75f, -0.1f, -1.3f}, wheelRear{-0.75f, -0.1f, 1.3f}; // metres
        std::array<float, 2> wheelRadius{0.3f, 0.3f}, wheelWidth{0.2f, 0.2f};                 // front, rear
        bool wheels = true;
        // Which triangles take the reflection pass (UpdateCarReflection; car_json.h CarJson::MeshReflection): 0 none,
        // 1 those of materials with gt2pc.reflection in their extras, 2 all.
        int reflection = 1;
    };
    int UseExternalCar(const std::string& id, const ExternalCar& car) {
        for (size_t i = 0; i < slots_.size(); i++)
            if (slots_[i].id == id) return int(i);
        if (slots_.size() >= carSlotLimit_) return -1;
        const uint32_t slot = uint32_t(slots_.size());
        const GltfMesh& mesh = *car.mesh;

        // Images on first use by a material.
        std::vector<uint32_t> imageBase(mesh.images.size(), UINT32_MAX);
        auto uploadImage = [&](int index) -> uint32_t {
            if (index < 0 || size_t(index) >= mesh.images.size()) return UINT32_MAX;
            if (imageBase[size_t(index)] != UINT32_MAX) return imageBase[size_t(index)];
            const PngImage& img = mesh.images[size_t(index)];
            if (img.width <= 0 || img.height <= 0 || img.width > 0xFFFF || img.height > 0xFFFF) return UINT32_MAX;
            const uint32_t count = uint32_t(img.width) * uint32_t(img.height);
            if (uint64_t(externalTexelsUsed_) + count > SceneRenderer::kMovieTexelBase)
                throw std::runtime_error("external textures: the store is full (" + std::to_string(SceneRenderer::kMovieTexelBase) + " texels)");
            std::vector<uint32_t> texels(count);
            for (uint32_t i = 0; i < count; i++) {
                const uint8_t* p = &img.rgba[size_t(i) * 4];
                texels[i] = uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
            }
            renderer_.UploadExternalTexture(externalTexelsUsed_, count, texels.data());
            imageBase[size_t(index)] = externalTexelsUsed_;
            externalTexelsUsed_ += count;
            return imageBase[size_t(index)];
        };

        std::vector<SceneVertex> vertices;
        std::vector<float> reflect; // reflection-pass corners: x, y, z (metres), nx, ny, nz
        for (const GltfPrimitive& p : mesh.primitives) {
            const GltfMaterial* mat = p.material >= 0 && size_t(p.material) < mesh.materials.size() ? &mesh.materials[size_t(p.material)] : nullptr;
            const uint32_t base = uploadImage(mat ? mat->baseColorImage : -1);
            const bool textured = base != UINT32_MAX && p.hasUvs;
            const PngImage* img = textured ? &mesh.images[size_t(mat->baseColorImage)] : nullptr;
            const std::array<float, 4> factor = mat ? mat->baseColorFactor : std::array<float, 4>{1, 1, 1, 1};
            for (const uint32_t index : p.indices) {
                const GltfVertex& v = p.vertices[index];
                SceneVertex o{};
                for (size_t k = 0; k < 3; k++) {
                    o.pos[k] = v.position[k] * car.scale;
                    o.color[k] = std::clamp(v.color[k] * factor[k], 0.0f, 1.0f);
                }
                if (textured) {
                    o.texel[0] = v.uv[0] * float(img->width);
                    o.texel[1] = v.uv[1] * float(img->height);
                    o.page = base;
                    o.clut = uint32_t(img->width) | (uint32_t(img->height) << 16);
                    o.flags = kTextured | kExternalTexture;
                }
                vertices.push_back(o);
            }
            // The reflection pass's source (the .cdo's bit-15 polygons with their normals): position + unit normal per corner,
            // the vertex normals of the mesh or, without them, the face normal.
            if (car.reflection == 2 || (car.reflection == 1 && mat && mat->reflection)) {
                for (size_t t = 0; t + 2 < p.indices.size(); t += 3) {
                    const GltfVertex* c[3] = {&p.vertices[p.indices[t]], &p.vertices[p.indices[t + 1]], &p.vertices[p.indices[t + 2]]};
                    float face[3] = {0, 1, 0};
                    {
                        const float a[3] = {c[1]->position[0] - c[0]->position[0], c[1]->position[1] - c[0]->position[1], c[1]->position[2] - c[0]->position[2]};
                        const float b[3] = {c[2]->position[0] - c[0]->position[0], c[2]->position[1] - c[0]->position[1], c[2]->position[2] - c[0]->position[2]};
                        const float n[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
                        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        if (l > 1e-12f) for (int k = 0; k < 3; k++) face[k] = n[k] / l;
                    }
                    for (const GltfVertex* v : c) {
                        for (size_t k = 0; k < 3; k++) reflect.push_back(v->position[k] * car.scale);
                        for (size_t k = 0; k < 3; k++) reflect.push_back(p.hasNormals ? v->normal[k] : face[k]);
                    }
                }
            }
        }
        if (car.wheels) {
            for (int i = 0; i < 4; i++) {
                const bool front = i < 2, left = (i & 1) == 0;
                const std::array<float, 3>& c = front ? car.wheelFront : car.wheelRear;
                float centre[3] = {left ? -std::fabs(c[0]) : std::fabs(c[0]), c[1], c[2]};
                std::vector<CarMeshVertex> wheel;
                BuildWheelMesh(wheel, centre, left ? -1.0f : 1.0f, car.wheelRadius[front ? 0 : 1], car.wheelWidth[front ? 0 : 1]);
                for (const CarMeshVertex& v : wheel) {
                    SceneVertex o{};
                    std::copy(v.pos, v.pos + 3, o.pos);
                    if (v.textured) o.color[0] = o.color[1] = o.color[2] = 0.6f; // no .cdp rim texture: flat light grey
                    else std::copy(v.color, v.color + 3, o.color);
                    if (v.cullBack) o.flags |= kCullBack; // the wheel strips' two coloured faces (car_mesh.h)
                    vertices.push_back(o);
                }
            }
        }
        // The slot ranges this car needs (body + shadow), placeholders for the extra ones.
        const uint32_t strides = uint32_t((vertices.size() + 64 + reflect.size() / 6 + kCarVertexStride - 1) / kCarVertexStride);
        if (slot + strides > kCarSlots) {
            std::printf("native scene: external car %s needs %u vertex ranges, %u free - truncated\n", id.c_str(), strides, kCarSlots - slot);
            vertices.resize(size_t(kCarSlots - slot) * kCarVertexStride - 64);
        }
        const uint32_t bodyCount = uint32_t(vertices.size());
        { // soft rectangular shadow: an inner rectangle at full shade feathered to nothing over kFeather metres
            const float mn[3] = {mesh.boundsMin[0] * car.scale, mesh.boundsMin[1] * car.scale, mesh.boundsMin[2] * car.scale};
            const float mx[3] = {mesh.boundsMax[0] * car.scale, mesh.boundsMax[1] * car.scale, mesh.boundsMax[2] * car.scale};
            const float y = car.wheelFront[1] - car.wheelRadius[0] + 0.04f, kFeather = 0.35f;
            const float ix0 = mn[0] + kFeather, ix1 = mx[0] - kFeather, iz0 = mn[2] + kFeather, iz1 = mx[2] - kFeather;
            if (ix1 > ix0 && iz1 > iz0) {
                const float xs[4] = {mn[0] - 0.05f, ix0, ix1, mx[0] + 0.05f}, zs[4] = {mn[2] - 0.05f, iz0, iz1, mx[2] + 0.05f};
                for (int cx = 0; cx < 3; cx++)
                    for (int cz = 0; cz < 3; cz++) {
                        auto shade = [&](int gx, int gz) { return (gx == 1 || gx == 2) && (gz == 1 || gz == 2) ? 1.0f : 0.0f; };
                        auto corner = [&](int gx, int gz) {
                            SceneVertex o{};
                            o.pos[0] = xs[gx];
                            o.pos[1] = y;
                            o.pos[2] = zs[gz];
                            o.color[0] = o.color[1] = o.color[2] = shade(gx, gz);
                            return o;
                        };
                        const int q[6][2] = {{cx, cz}, {cx + 1, cz}, {cx + 1, cz + 1}, {cx, cz}, {cx + 1, cz + 1}, {cx, cz + 1}};
                        for (const auto& g : q) vertices.push_back(corner(g[0], g[1]));
                    }
            }
        }
        renderer_.SetVertices(kCarVertexBase + slot * kCarVertexStride, vertices);
        slots_.push_back({id, bodyCount, uint32_t(vertices.size()) - bodyCount, 0, CarTexture{}, CarModel{}, std::move(reflect), std::min(strides, kCarSlots - slot)});
        slots_.back().shadowHeight = car.wheelFront[1] - car.wheelRadius[0] + 0.04f;
        for (uint32_t extra = 1; extra < strides && slots_.size() < kCarSlots; extra++) slots_.push_back({std::string(), 0, 0, 0, CarTexture{}, CarModel{}});
        std::printf("native scene: external car %s -> slot %u (%u body + %u shadow vertices, %zu triangles, %zu image(s), %u vertex range(s))\n", id.c_str(), slot,
                    bodyCount, uint32_t(vertices.size()) - bodyCount, mesh.TriangleCount(), mesh.images.size(), strides);
        return int(slot);
    }

    // Reflection pass of a car for this frame (the original: 0x800616C4 builds a (u, v) per LOD normal, 0x80061798
    // draws every body polygon whose flag word has bit 15 set a second time as an additive POLY_FT4 into the
    // 128 x 128 environment map). u, v = 64 + ((R512 . n) >> 12) with n the file normal (bits 2-11, 12-21, 22-31
    // of the packed word, |n| = 512) and R512 = trunc(R / 8), R = the car's rotation in the camera frame at 4096 =
    // 1.0 - i.e. the reflection is the view-space normal mapped onto the map's disc. `world` = the car's world
    // matrix (column-major, metres); `cameraAxes` rows = camera right, down, forward in world axes. Verified
    // against the captured second-pass primitives of the Seattle attract race (190 of 204 corner UVs exact, the
    // rest off by one; tools/gt2play --prims).
    // `tpage` / `clut` / `colour`: the map's place and the pass's colour (the race: page 9, CLUT (368, 511), 0x60; the
    // GT-mode menus' car view: the map inside arcade/gt_cursor.tim, CLUT 0x2624, colour 0x40 from 0x8001A8A4).
    // `lodIndex`: the LOD whose polygons are passed (the game mode 6 ghost look: LOD 2, race_shell.h CarDrawRule).
    void UpdateCarReflection(int slot, const float* world, const std::array<std::array<float, 3>, 3>& cameraAxes, uint16_t tpage = kEnvMapTpage,
                             uint16_t clut = kEnvMapClut, uint8_t colour = 0x60, size_t lodIndex = 0, uint32_t rowBase = 0,
                             bool cockpit = false) {
        Slot& s = slots_[size_t(slot)];
        if (UsesGt4(slot)) {
            // Store the car's world rotation for mono reflection lookups. The
            // draw MVP alone cannot distinguish camera rotation from car yaw.
            s.nativeModelRotation=RotationQuaternion(world);
            return;
        }
        if (s.model.lods.empty()) {
            UpdateExternalReflection(slot, world, cameraAxes, tpage, clut, colour, rowBase);
            return;
        }
        const CarLod& lod = s.model.lods[std::min(lodIndex, s.model.lods.size() - 1)];
        const float mu = static_cast<float>(CarBodyMetresPerUnit(lod));
        // R = cameraAxes * worldRotation (rows of the result = camera axes expressed in car axes), at 4096.
        int32_t r512[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += cameraAxes[size_t(i)][size_t(k)] * world[j * 4 + k];
                r512[i][j] = int32_t(std::lround(v * 4096.0f)) / 8; // the original's matrix scaled to 512, truncated
            }
        std::vector<SceneVertex> vertices;
        for (size_t polygon=0; polygon<lod.polygons.size(); ++polygon) {
            if (cockpit && !std::binary_search(s.cockpitExterior.begin(),s.cockpitExterior.end(),polygon)) continue;
            const CarPolygon& p=lod.polygons[polygon];
            if (!(p.renderFlags & 0x8000)) continue;
            static constexpr int kTri[3] = {0, 1, 2}, kQuadRing[6] = {0, 1, 3, 1, 2, 3}; // the PS1's split (GPU slots v0 v1 v3 v2)
            const int* order = p.IsQuad() ? kQuadRing : kTri;
            for (int k = 0; k < (p.IsQuad() ? 6 : 3); k++) {
                const int c = order[k];
                const CarVertex& v = lod.vertices[p.vertex[size_t(c)]];
                const CarNormal& n = lod.normals[p.normal[size_t(c)] % lod.normals.size()];
                // CarNormal stores (bits 22-31, 12-21, 2-11) as (x, y, z); the original feeds (bits 2-11, 12-21, 22-31).
                const int32_t gn[3] = {n.z, n.y, n.x};
                const int32_t u = 64 + ((r512[0][0] * gn[0] + r512[0][1] * gn[1] + r512[0][2] * gn[2]) >> 12);
                const int32_t vv = 64 + ((r512[1][0] * gn[0] + r512[1][1] * gn[1] + r512[1][2] * gn[2]) >> 12);
                SceneVertex o{};
                o.pos[0] = v.x * mu;
                o.pos[1] = v.y * mu;
                o.pos[2] = v.z * mu;
                for (float& ch : o.color) ch = colour / 255.0f;
                SetTexture(o, tpage, clut, uint8_t(u & 0xFF), uint8_t(vv & 0xFF));
                o.page += rowBase << 16; o.clut += rowBase << 16;
                if(gt2::modernCarReflections && lodIndex==0) {
                    o.flags=kModelReflection|kCullBack;
                    for(size_t j=0;j<3;++j)o.color[j]=float(gn[j])/512.f;
                }
                vertices.push_back(o);
            }
        }
        if (cockpit) {
            if (s.cockpitBodyCount + vertices.size() > SceneRenderer::kCockpitBodyVertexStride-384)
                throw std::runtime_error("scene: cockpit body and reflection exceed reserved vertex range");
            renderer_.SetVertices(SceneRenderer::kCockpitBodyVertexBase + uint32_t(slot)*SceneRenderer::kCockpitBodyVertexStride +
                                  s.cockpitBodyCount, vertices);
            s.cockpitReflectionCount=uint32_t(vertices.size());
            return;
        }
        const uint32_t base = kCarVertexBase + uint32_t(slot) * kCarVertexStride + s.vertexCount + s.shadowCount;
        if (base + vertices.size() > kCarVertexBase + uint32_t(slot + 1) * kCarVertexStride) vertices.resize(kCarVertexBase + size_t(slot + 1) * kCarVertexStride - base);
        renderer_.SetVertices(base, vertices);
        s.reflectionCount = uint32_t(vertices.size());
    }

    void UpdateCockpitReflection(int slot, const float* world, const std::array<std::array<float, 3>, 3>& cameraAxes) {
        UpdateCarReflection(slot, world, cameraAxes, kEnvMapTpage, kEnvMapClut, 0x60, 0, 0, true);
    }

    // Draw items of one car: the ground shadow (subtractive, before the body so the body paints over it), the
    // body with its wheels and, when UpdateCarReflection was called for the slot, the additive reflection pass.
    // `mvp` = view-projection x car world matrix.
    // `body` = false: the shadow and the reflection pass only (the game mode 6 ghost look: its CLUT rows are empty, no wheels).
    // `wheelModels` (optional, car-space column-major 4x4 per wheel, car_mesh.h WheelModelMatrix): the wheels' transforms of
    // the frame (steer, camber, rolling angle, suspension); without it each wheel stands at its rest centre, not turned.
    // Imported models may have more colours than their original GT2 donor.
    // A saved colour survives switching model sets; never sample an unuploaded CLUT.
    uint32_t StockPaint(int slot, uint32_t paint) const {
        const auto count=std::min(size_t(16),slots_.at(size_t(slot)).texture.paints.size());
        return paint<count?paint:0;
    }
    void AppendCarItems(std::vector<DrawItem>& items, int slot, const float* mvp, uint32_t paint, uint32_t brakeLit, bool body = true,
                        const std::array<std::array<float, 16>, 4>* wheelModels = nullptr, const float* shadowMvp = nullptr) const {
        if(body) UpdateGt4Paint(slot,paint);
        const Slot& s = slots_[size_t(slot)];
        const uint32_t base = kCarVertexBase + uint32_t(slot) * kCarVertexStride;
        if (s.shadowCount) {
            DrawItem shadow;
            shadow.firstVertex = base + s.vertexCount;
            shadow.vertexCount = s.shadowCount;
            shadow.blend = 2;
            std::copy(shadowMvp ? shadowMvp : mvp, (shadowMvp ? shadowMvp : mvp) + 16, shadow.mvp);
            items.push_back(shadow);
        }
        if (body) {
            DrawItem item;
            item.firstVertex = base;
            item.vertexCount = s.separateWheels ? s.wheelRanges[0][0] : s.vertexCount;
            if (UsesGt4(slot)) {
                item.firstVertex = SceneRenderer::kModelSetVertexBase + uint32_t(slot) * SceneRenderer::kModelSetVertexStride;
                item.vertexCount = gt4Cars_[size_t(slot)].opaqueCount;
            }
            item.nativeModelRotation=s.nativeModelRotation;
            item.paint = StockPaint(slot,paint);
            item.brakeLit = brakeLit;
            std::copy(mvp, mvp + 16, item.mvp);
            items.push_back(item);
            AppendCarWheels(items,slot,mvp,paint,wheelModels,brakeLit);
            if(UsesGt4(slot)) {
                const auto& car=gt4Cars_[size_t(slot)];
                if(car.count>car.opaqueCount) {
                    item.firstVertex+=car.opaqueCount;item.vertexCount=car.count-car.opaqueCount;
                    item.blend=kBlendAlpha;item.stpPass=3;items.push_back(item);
                }
                AppendGt4LampItems(items,slot,mvp,(brakeLit?1u:0u)+(courseNight_?2u:0u));
            }
        }
        if (UsesGt4(slot) && !std::getenv("GT2_DEBUG_CABIN") && !std::getenv("GT2_DEBUG_EXTERIOR")) {
            const auto& car=gt4Cars_[size_t(slot)];
            DrawItem reflection;
            reflection.firstVertex=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride+car.count+car.cockpitCount+car.mirrorCount;
            reflection.vertexCount=car.reflectionCount-car.surfaceReflectionCount;
            reflection.nativeModelRotation=s.nativeModelRotation;
            reflection.blend=car.nativeMaterials.empty()?1:kBlendNativeEnvironment;
            std::copy(mvp,mvp+16,reflection.mvp);
            items.push_back(reflection);
            if(car.surfaceReflectionCount){reflection.firstVertex+=reflection.vertexCount;reflection.vertexCount=car.surfaceReflectionCount;reflection.blend=kBlendNativeSurfaceEnvironment;items.push_back(reflection);}
        }
        if (s.reflectionCount && !UsesGt4(slot)) {
            DrawItem reflection;
            reflection.firstVertex = base + s.vertexCount + s.shadowCount;
            reflection.vertexCount = s.reflectionCount;
            reflection.blend = body ? kBlendStockSurfaceEnvironment : 1; // Ghosts have no opaque base.
            std::copy(mvp, mvp + 16, reflection.mvp);
            items.push_back(reflection);
        }
    }

    // GT4 callbacks 0 (brake) and 7 (lights) select the same six material
    // branches for exterior and seated views.
    void AppendGt4LampItems(std::vector<DrawItem>& items,int slot,const float* mvp,uint32_t state,bool seated=false) const {
        if(!UsesGt4(slot) || state>=6)return;
        const auto& car=gt4Cars_[size_t(slot)];
        for(const auto& range:(seated?car.cabinLamps:car.lamps))if(range.states & (1u<<state)) {
            DrawItem lamp;lamp.nativeModelRotation=slots_[size_t(slot)].nativeModelRotation;lamp.firstVertex=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride+range.first;
            lamp.vertexCount=range.count;lamp.blend=range.blend;lamp.stpPass=range.blend==kBlendAlpha?3u:0u;
            lamp.cabinDepth=seated;std::copy_n(mvp,16,lamp.mvp);items.push_back(lamp);
        }
    }

    // The same fitted wheel matrix is used by drawing and road-contact fitting.
    void CarWheelMatrix(int slot,size_t w,const std::array<std::array<float,16>,4>* wheelModels,float* model) const {
        const auto& s=slots_[size_t(slot)];
                    const float rest[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, s.wheelCentres[w][0], s.wheelCentres[w][1], s.wheelCentres[w][2], 1};
                    std::copy_n(rest,16,model);
                    if (wheelModels) std::copy((*wheelModels)[w].begin(), (*wheelModels)[w].end(), model);
                    if (UsesGt4(slot)) {
                        const auto& car=gt4Cars_[size_t(slot)];
                        if(!wheelModels && car.nativeWheelPose)
                            for(size_t c=0;c<3;++c)for(size_t r=0;r<3;++r)
                                model[c*4+r]=car.wheelBasis[w][c*3+r];
                        model[12]+=car.wheelShift[w];model[14]+=car.wheelZShift[w];
                        const auto& well=car.cockpit.wheelWells[w];
                        if(wheelModels && s.hasRaceWheels) {
                            // The simulation has already placed the axle from
                            // tyre contact and suspension travel (0x800312FC).
                            // Preserve that contact when substituting GT4's tyre
                            // radius. Adding the CDO/showroom centre displacement
                            // a second time pushes the wheels into the road.
                            const float support=well[4]*.5f*std::abs(model[1])+well[3]*std::hypot(model[5],model[9]);
                            model[13]+=support-s.raceWheels.radius[w>>1];
                        } else {
                            model[13]+=car.wheelYShift[w];
                            model[13]=std::min(model[13],car.wheelBumpStop[w]);
                        }
                        // Keep the steered cylinder under the fitted outer wing.
                        // Its lateral extent grows with steering/camber; using
                        // only the rest half-width lets the tread cross paint.
                        const float extent=well[4]*.5f*std::abs(model[0])+well[3]*std::hypot(model[4],model[8]);
                        const float lateral=std::max(.1f,std::abs(well[0])+well[4]*.5f-extent);
                        model[12]=std::copysign(std::min(std::abs(model[12]),lateral),model[12]);
                        for(size_t row=0;row<3;++row) {
                            model[row]*=car.wheelWidthScale[w];
                            model[4+row]*=car.wheelScale[w];model[8+row]*=car.wheelScale[w];
                        }
                    }
                    if (!UsesGt4(slot) && wheelModels) {
                        const auto& well=s.cockpit.wheelWells[w];
                        // Stock shells have tight authored arches too. Keep the
                        // presentation tyre below the wing at full compression
                        // and inside its rest outer edge while steering. Physics
                        // travel/contact points and the wheel mesh stay unchanged.
                        const float vertical=well[4]*.5f*std::abs(model[1])+well[3]*std::hypot(model[5],model[9]);
                        model[13]=std::min(model[13],s.wheelBumpStop[w]-std::max(0.f,vertical-well[3]));
                        const float lateral=well[4]*.5f*std::abs(model[0])+well[3]*std::hypot(model[4],model[8]);
                        const float limit=std::max(.1f,std::abs(well[0])+well[4]*.5f-lateral);
                        model[12]=std::copysign(std::min(std::abs(model[12]),limit),model[12]);
                    }
                    if(std::getenv("GT2_AUDIT_INJECT_WHEEL_OVERLAP"))for(size_t row=0;row<3;++row){model[4+row]*=1.8f;model[8+row]*=1.8f;}
    }

    // Keep shells and tyres above the actual road without changing simulation.
    // Stock arch limits and road contact must be solved together on landing.
    // The sampler returns vertical clearance, or infinity outside the surface.
    template<class Clearance>
    void FitCarRoadContact(int slot,float* world,std::array<std::array<float,16>,4>& wheels,Clearance clearance) const {
        if(!slots_[size_t(slot)].hasRaceWheels || world[5]<.5f)return;
        const auto point=[&](const std::array<float,3>& p){
            std::array<float,3> q{};
            for(size_t r=0;r<3;++r)q[r]=world[12+r]+world[r]*p[0]+world[4+r]*p[1]+world[8+r]*p[2];
            return q;
        };
        const auto bottomAt=[&](size_t w,const auto& axleModels){
            float model[16];CarWheelMatrix(slot,w,&axleModels,model);
            const auto centre=point({model[12],model[13],model[14]});
            const float h=clearance(centre);
            if(!std::isfinite(h))return std::numeric_limits<float>::infinity();
            auto px=centre,pz=centre;px[0]+=.1f;pz[2]+=.1f;
            const std::array<float,3> normal{(clearance(px)-h)*10.f,1.f,(clearance(pz)-h)*10.f};
            if(!std::isfinite(normal[0]) || !std::isfinite(normal[2]))return std::numeric_limits<float>::infinity();
            std::array<std::array<float,3>,3> basis{};
            for(size_t c=0;c<3;++c)for(size_t r=0;r<3;++r)
                basis[c][r]=world[r]*model[c*4]+world[4+r]*model[c*4+1]+world[8+r]*model[c*4+2];
            const auto dot=[&](const std::array<float,3>& a){return a[0]*normal[0]+a[1]*normal[1]+a[2]*normal[2];};
            const float by=dot(basis[1]),bz=dot(basis[2]),length=std::hypot(by,bz);
            if(length<.001f)return std::numeric_limits<float>::infinity();
            const auto& well=slots_[size_t(slot)].cockpit.wheelWells[w];
            float bottom=1e9f;
            for(float side:{-1.f,1.f}){
                auto tread=centre;
                for(size_t r=0;r<3;++r)tread[r]+=side*well[4]*.5f*basis[0][r]-well[3]*(by*basis[1][r]+bz*basis[2][r])/length;
                bottom=std::min(bottom,clearance(tread));
            }
            return bottom;
        };
        float lift=0;
        const auto& support=UsesGt4(slot)?gt4Cars_[size_t(slot)].roadSupport:slots_[size_t(slot)].roadSupport;
        for(const auto& p:support)lift=std::max(lift,.005f-clearance(point(p)));
        if(!UsesGt4(slot)){
            // Raising only a raw axle is ineffective once its arch clamp is
            // reached. First ensure the highest permitted tyre clears ground.
            auto limits=wheels;
            for(auto& wheel:limits)wheel[13]=1e4f;
            for(size_t w=0;w<4;++w)lift=std::max(lift,.002f-bottomAt(w,limits));
        }
        const float localLift=lift/world[5];
        for(size_t r=0;r<3;++r)world[12+r]+=world[4+r]*localLift;
        for(auto& wheel:wheels)wheel[13]-=localLift;
        for(size_t w=0;w<4;++w)for(int pass=0;pass<2;++pass){
            const float bottom=bottomAt(w,wheels);
            // Correct penetration only. An airborne wheel must retain its gap.
            if(bottom>=.002f)break;
            wheels[w][13]+=(.002f-bottom)/world[5];
        }
    }

    template<class Clearance>
    void FitCarRoadShadow(int slot,const float* ground,Clearance clearance) {
        auto& s=slots_[size_t(slot)];
        if(s.roadShadow.empty() || !s.hasRaceWheels)return;
        if(s.shadowProjected && std::equal(ground,ground+16,s.shadowPose.begin()))return;
        auto projected=s.roadShadow;
        std::vector<float> offsets(s.shadowSamples.size());
        for(size_t i=0;i<s.shadowSamples.size();++i){
            const auto& v=s.shadowSamples[i];
            std::array<float,3> p{};
            for(size_t r=0;r<3;++r)p[r]=ground[12+r]+ground[r]*v[0]+ground[4+r]*v[1]+ground[8+r]*v[2];
            const float h=clearance(p);
            if(!std::isfinite(h))continue;
            offsets[i]=.035f-h;
        }
        for(size_t i=0;i<projected.size();++i){auto& v=projected[i];
            // Project vertically onto the actual receiver, including after a
            // jump. Small triangles follow changes in slope at hill crests.
            for(size_t k=0;k<3;++k)v.pos[k]+=ground[k*4+1]*offsets[s.shadowSampleIndex[i]];
        }
        renderer_.SetVertices(kCarVertexBase+uint32_t(slot)*kCarVertexStride+s.vertexCount,projected);
        std::copy_n(ground,16,s.shadowPose.begin());s.shadowProjected=true;
    }

    void AppendCarWheels(std::vector<DrawItem>& items, int slot, const float* mvp, uint32_t paint,
                         const std::array<std::array<float,16>,4>* wheelModels=nullptr,uint32_t brakeLit=0) const {
        const auto& s=slots_[size_t(slot)];
        const uint32_t base=kCarVertexBase+uint32_t(slot)*kCarVertexStride;
        if (s.separateWheels)
            for (size_t w = 0; w < 4; w++) {
                    float model[16];CarWheelMatrix(slot,w,wheelModels,model);
                    // GT4 colours index a different palette from the GT2 donor
                    // wheel. In particular, extra colours read empty CLUT rows
                    // and erase the rim. Native brake attachments keep their own
                    // material selection; the donor rim keeps its original paint.
                    DrawItem wheel; wheel.paint=UsesGt4(slot)?0:StockPaint(slot,paint);
                    wheel.firstVertex = base + s.wheelRanges[w][0];
                    wheel.vertexCount = s.wheelRanges[w][1];
                    for (int c = 0; c < 4; c++) // column-major mvp * model
                        for (int r = 0; r < 4; r++)
                            wheel.mvp[c * 4 + r] = mvp[r] * model[c * 4] + mvp[4 + r] * model[c * 4 + 1] + mvp[8 + r] * model[c * 4 + 2] + mvp[12 + r] * model[c * 4 + 3];
                    items.push_back(wheel);
                    if(UsesGt4(slot))for(const auto& part:gt4Cars_[size_t(slot)].attachments) {
                        if(part.wheel!=int(w))continue;
                        const uint32_t lampState=(brakeLit?1u:0u)+(courseNight_?2u:0u);
                        if(part.lampStates && !(part.lampStates & (1u<<lampState)))continue;
                        float attachment[16];std::copy_n(model,16,attachment);
                        for(size_t k=0;k<3;++k)attachment[k]/=gt4Cars_[size_t(slot)].wheelWidthScale[w];
                        if(!part.spinning) {
                            // WheelModelMatrix's first column is the axle and
                            // does not rotate with the tyre. Rebuild its upright
                            // frame so brake calipers steer but never spin.
                            const float n=std::hypot(attachment[0],attachment[2]);
                            if(n<.001f)throw std::runtime_error("invalid attachment axle");
                            const std::array<float,3> forward{-attachment[2]/n,0,attachment[0]/n};
                            const std::array<float,3> up{-forward[2]*attachment[1],
                                forward[2]*attachment[0]-forward[0]*attachment[2],forward[0]*attachment[1]};
                            for(size_t k=0;k<3;++k){attachment[4+k]=up[k];attachment[8+k]=forward[k];}
                        }
                        const auto& well=gt4Cars_[size_t(slot)].cockpit.wheelWells[w];
                        const float edge=std::copysign(well[4]*.5f,well[0]);
                        for(size_t k=0;k<3;++k)attachment[12+k]+=attachment[k]*edge;
                        DrawItem attached;attached.nativeModelRotation=ComposeRotation(s.nativeModelRotation,RotationQuaternion(attachment));attached.firstVertex=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride+part.first;
                        attached.vertexCount=part.count;attached.blend=part.blend;attached.stpPass=part.blend==kBlendAlpha?3:0;
                        for(int c=0;c<4;++c)for(int r=0;r<4;++r)
                            attached.mvp[c*4+r]=mvp[r]*attachment[c*4]+mvp[4+r]*attachment[c*4+1]+mvp[8+r]*attachment[c*4+2]+mvp[12+r]*attachment[c*4+3];
                        items.push_back(attached);
                    }
                }
    }

    void AppendSideMirrorItems(std::vector<DrawItem>& items, int slot, const float* mvp, bool enabled=true) const {
        if(!enabled || slot<0 || size_t(slot)>=slots_.size())return;
        if (!UsesGt4(slot)) {
            if(!slots_[size_t(slot)].cockpitMirrorCount)return;
            DrawItem item;item.cabinDepth=true;item.vertexCount=slots_[size_t(slot)].cockpitMirrorCount;
            item.firstVertex=SceneRenderer::kCockpitBodyVertexBase+(uint32_t(slot)+1)*SceneRenderer::kCockpitBodyVertexStride-384;
            std::copy_n(mvp,16,item.mvp);items.push_back(item);return;
        }
        const auto& car=gt4Cars_[size_t(slot)];
        if (!car.mirrorCount) return;
        DrawItem item;
        item.firstVertex=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride+car.count+car.cockpitCount;
        item.vertexCount=car.mirrorCount;
        item.cabinDepth=true;
        std::copy(mvp,mvp+16,item.mvp);
        items.push_back(item);
    }

    // The original body with transparent glazing and an interior roof lining.
    // It has a separate immutable range: reflections and other cars sharing this slot cannot overwrite it.
    void AppendCockpitBodyItems(std::vector<DrawItem>& items, int slot, const float* mvp, uint32_t paint,
                                const std::array<std::array<float,16>,4>* wheelModels=nullptr, bool replaceSteering=false,uint32_t brakeLit=0) const {
        if (slot < 0 || size_t(slot) >= slots_.size()) return;
        UpdateGt4Paint(slot,paint);
        const Slot& s = slots_[size_t(slot)];
        AppendCarWheels(items,slot,mvp,paint,wheelModels,brakeLit);
        if (UsesGt4(slot)) {
            const auto& car = gt4Cars_[size_t(slot)];
            DrawItem item;
            item.cabinDepth=true;item.nativeModelRotation=s.nativeModelRotation;
            std::copy(mvp, mvp + 16, item.mvp);
            for(const auto& range:car.cabinRanges) {
                if(replaceSteering && range.steering)continue;
                item.firstVertex=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride+range.first;
                item.vertexCount=range.count;item.blend=range.alpha?kBlendAlpha:kBlendOpaque;
                item.stpPass=range.alpha?3u:0u;items.push_back(item);
            }
            AppendGt4LampItems(items,slot,mvp,(brakeLit?1u:0u)+(courseNight_?2u:0u),true);
            if(car.cockpitReflectionCount && !std::getenv("GT2_DEBUG_CABIN") && !std::getenv("GT2_DEBUG_EXTERIOR")) {
                item.firstVertex=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride+car.cockpitReflectionFirst;
                item.vertexCount=car.cockpitReflectionCount-car.cockpitSurfaceReflectionCount;item.blend=car.nativeMaterials.empty()?1:kBlendNativeEnvironment;
                item.stpPass=0;items.push_back(item);
                if(car.cockpitSurfaceReflectionCount){item.firstVertex+=item.vertexCount;item.vertexCount=car.cockpitSurfaceReflectionCount;item.blend=kBlendNativeSurfaceEnvironment;items.push_back(item);}
            }
            return;
        }
        if (!s.cockpitBodyCount) return;
        DrawItem item;
        item.cabinDepth=true;
        item.firstVertex = SceneRenderer::kCockpitBodyVertexBase + uint32_t(slot) * SceneRenderer::kCockpitBodyVertexStride;
        item.vertexCount = s.cockpitBodyCount;
        item.paint = StockPaint(slot,paint);
        std::copy(mvp, mvp + 16, item.mvp);
        items.push_back(item);
        if (s.cockpitReflectionCount) {
            item.firstVertex += s.cockpitBodyCount;
            item.vertexCount = s.cockpitReflectionCount;
            item.paint = 0;
            item.blend = kBlendStockSurfaceEnvironment;
            items.push_back(item);
        }
    }

    // The paint the original is using for a car, found by comparing its 16 CLUTs in the console's VRAM with the
    // car's .cdp paints. Runtime layout (Seattle attract race, US v1.2): car texture n at (320 + 64 n, 256),
    // its CLUTs at (320 + 64 n + 16 (c % 4), 480 + c / 4). Returns the paint index, 0 when nothing matches.
    uint32_t FindPaint(int slot, const std::vector<uint16_t>& consoleVram) const {
        const CarTexture& texture = slots_[size_t(slot)].texture;
        for (int n = 0; n < 6; n++)
            for (size_t p = 0; p < texture.paints.size(); p++) {
                bool same = true;
                for (int c = 0; c < 16 && same; c++) {
                    const size_t at = size_t(480 + c / 4) * 1024 + size_t(320 + 64 * n + 16 * (c % 4));
                    same = std::memcmp(&consoleVram[at], texture.paints[p].cluts[size_t(c)].data(), 32) == 0;
                }
                if (same) return uint32_t(p);
            }
        return 0;
    }

    // RGBA (alpha = pixel present) -> overlay rows + a 4:3 quad centred in the window (uniform scale at any aspect).
    void SetOverlay(const std::vector<uint8_t>& rgba, int width, int height, float windowAspect) {
        std::vector<uint16_t> rows(size_t(height) * 1024, 0);
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++) {
                const uint8_t* p = &rgba[(size_t(y) * width + x) * 4];
                if (p[3]) rows[size_t(y) * 1024 + x] = uint16_t(0x8000 | (p[0] >> 3) | ((p[1] >> 3) << 5) | ((p[2] >> 3) << 10));
            }
        renderer_.UploadVram(kOverlayRow, uint32_t(height), rows.data());
        if (width == overlayWidth_ && height == overlayHeight_ && windowAspect == overlayAspect_) return;
        overlayWidth_ = width;
        overlayHeight_ = height;
        overlayAspect_ = windowAspect;
        const float k = std::min(1.0f, (4.0f / 3.0f) / windowAspect), ky = std::min(1.0f, windowAspect / (4.0f / 3.0f)); // uniform 4:3
        const float corners[4][4] = {{-k, -ky, 0, 0}, {k, -ky, float(width), 0}, {k, ky, float(width), float(height)}, {-k, ky, 0, float(height)}};
        std::vector<SceneVertex> quad;
        for (int i : {0, 1, 2, 0, 2, 3}) {
            SceneVertex v{};
            v.pos[0] = corners[i][0];
            v.pos[1] = corners[i][1];
            v.pos[2] = 1.0f; // reversed Z: in front of the scene
            v.texel[0] = corners[i][2];
            v.texel[1] = corners[i][3];
            v.page = 0 | (kOverlayRow << 16);
            v.flags = kTextured | kOverlay;
            quad.push_back(v);
        }
        renderer_.SetVertices(kOverlayVertexBase, quad);
    }

    // Tyre smoke of this frame (particles.h, the original's 0x8002EB60): the pool's sprites in world space as one
    // semi-transparent range: its opaque texels (none in the smoke sprite's CLUT) with the opaque items, the STP
    // texels additively (tpage 0x29: mode 1) with the blended ones. `eye` / `right` / `up` / `forward` = the camera.
    void AppendSmokeItems(std::vector<DrawItem>& opaque, std::vector<DrawItem>& blended, const SmokePool& pool, const float* vp, const float eye[3],
                          const float right[3], const float up[3], const float forward[3], float projectionDistance) {
        std::vector<SceneVertex> vertices;
        pool.AppendBillboards(vertices, eye, right, up, forward, projectionDistance);
        if (vertices.empty()) return;
        if (vertices.size() > kSmokeVertexLimit) vertices.resize(kSmokeVertexLimit);
        renderer_.SetVertices(smokeBase_, vertices);
        DrawItem item;
        item.firstVertex = smokeBase_;
        item.vertexCount = uint32_t(vertices.size());
        std::copy(vp, vp + 16, item.mvp);
        item.stpPass = 1;
        if (SmokeOpaquePassRequired(vertices, smokeOpaque_)) opaque.push_back(item);
        item.stpPass = 2;
        item.blend = 1;
        item.softParticle = true;
        blended.push_back(item);
    }

    // The parsed .cdo of a slot (empty for an external mesh): its wheel entries place the smoke (particles.h).
    const CarModel& SlotModel(int slot) const { return slots_[size_t(slot)].model; }
    CockpitFit SlotCockpitFit(int slot) const {
        if (slot<0 || size_t(slot)>=slots_.size()) return {};
        return UsesGt4(slot) ? gt4Cars_[size_t(slot)].cockpit : slots_[size_t(slot)].cockpit;
    }
    // Diagnostic near-side reference retains the real textures/materials. A
    // flat silhouette also masks black recesses which legitimately hold tyres.
    void AppendBonnetItemsForAudit(std::vector<DrawItem>& items,int slot,const float* mvp) const {
        if(UsesGt4(slot)) {
            for(auto i:gt4Cars_[size_t(slot)].auditSurfaceTriangles){DrawItem d;d.space=kSpaceWorld;
                d.firstVertex=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride+i;
                d.vertexCount=3;std::copy_n(mvp,16,d.mvp);items.push_back(d);}return;
        }
        gt2::CarMeshOptions options;options.wheels=false;
        const auto mesh=gt2::BuildCarMesh(slots_.at(size_t(slot)).model,options);
        const auto fit=SlotCockpitFit(slot);
        for(size_t i=0;i+2<mesh.size();i+=3){
            std::array<std::array<float,3>,3> triangle{};
            for(size_t j=0;j<3;++j)std::copy_n(mesh[i+j].pos,3,triangle[j].begin());
            if(!cockpit_detail::TriangleAheadOfCowl(triangle,fit.cowl,-.06f))continue;
            DrawItem d;d.firstVertex=kCarVertexBase+uint32_t(slot)*kCarVertexStride+uint32_t(i);
            d.vertexCount=3;d.space=kSpaceWorld;std::copy_n(mvp,16,d.mvp);items.push_back(d);
        }
    }
    void AppendGt4BonnetItemsForAudit(std::vector<DrawItem>& items,int slot,const float* mvp) const {
        const auto& car=gt4Cars_.at(size_t(slot));
        const uint32_t base=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride;
        for(const auto first:car.auditBonnetTriangles){
            // A deliberately removed tinted pane is not a solid bonnet witness.
            // Native GS blending can saturate even a translucent texture to an
            // opaque colour, so a background-colour difference alone is unsafe.
            if(first>=car.opaqueCount)continue;
            const bool alpha=false;
            if(!items.empty() && items.back().firstVertex+items.back().vertexCount==base+first && bool(items.back().stpPass)==alpha)items.back().vertexCount+=3;
            else{DrawItem item;item.firstVertex=base+first;item.vertexCount=3;item.space=kSpaceWorld;item.blend=alpha?kBlendAlpha:kBlendOpaque;item.stpPass=alpha?3:0;std::copy_n(mvp,16,item.mvp);items.push_back(item);}
        }
    }
    void AppendGt4PanelItemsForAudit(std::vector<DrawItem>& items,int slot,int side,const float* mvp) const {
        const auto& car=gt4Cars_.at(size_t(slot));
        if(car.auditPanelCentres.empty())throw std::runtime_error("panel audit requires GT2_AUDIT_WHEEL_COLOR");
        const uint32_t base=SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride;
        for(size_t i=0;i<car.auditPanelCentres.size();++i) {
            const auto& centre=car.auditPanelCentres[i];
            if((side==0 && centre[0]<=0) || (side==1 && centre[0]>=0) || (side==2 && centre[2]<=0))continue;
            // An exposed tyre can legitimately occlude an inboard suspension
            // arm or central tub. Only the outer flank is an occlusion witness;
            // treating every triangle in that half-car as its skin rejects
            // open-wheel cars even when no surfaces intersect.
            if(side<2) {
                bool inboard=false;
                for(const auto& well:car.cockpit.wheelWells)
                    if((well[0]>0)==(side==0) && std::abs(centre[2]-well[2])<well[3]*1.5f &&
                       std::abs(centre[0])<std::abs(well[0]))inboard=true;
                if(inboard)continue;
            }
            const uint32_t first=base+uint32_t(i)*3;
            if(!items.empty() && items.back().firstVertex+items.back().vertexCount==first)items.back().vertexCount+=3;
            else {DrawItem item;item.firstVertex=first;item.vertexCount=3;item.space=kSpaceWorld;std::copy_n(mvp,16,item.mvp);items.push_back(item);}
        }
    }
    float SlotShadowHeight(int slot) const { return slots_[size_t(slot)].shadowHeight; }
    // The split screen of the 2 player Battle (tools/gt2game/split_race.cpp): the frame's billboards and tyre smoke are written
    // per view, so the second view needs its own vertex ranges - the last two car slots (kCarSlotsSplit..kCarSlots) are reserved
    // for them (call before the cars are loaded; false when those slots are taken). SelectView(1) makes AppendTrackItems /
    // AppendSmokeItems write there, SelectView(0) back to the normal ranges.
    static constexpr uint32_t kCarSlotsSplit = kCarSlots - 2;
    bool ReserveSecondView() {
        if (slots_.size() > kCarSlotsSplit) return false;
        carSlotLimit_ = kCarSlotsSplit;
        return true;
    }
    void SelectView(int view) {
        if (view == 1 && carSlotLimit_ != kCarSlotsSplit) throw std::logic_error("scene: the second view's vertex ranges are not reserved");
        const uint32_t second = kCarVertexBase + kCarSlotsSplit * kCarVertexStride;
        billboardBase_ = view == 1 ? second : kBillboardVertexBase;
        billboardLimit_ = view == 1 ? 2 * kCarVertexStride - kSmokeVertexLimit : kBillboardVertexLimit;
        smokeBase_ = view == 1 ? second + billboardLimit_ : kSmokeVertexBase;
    }
    // The rest centres of a slot's wheels (car metres) and whether they are drawn with their own transforms.
    bool SlotSeparateWheels(int slot) const { return slots_[size_t(slot)].separateWheels; }
    const std::array<std::array<float, 3>, 4>& SlotWheelCentres(int slot) const { return slots_[size_t(slot)].wheelCentres; }

    uint32_t CarVertexCount(int slot) const { return slots_[size_t(slot)].vertexCount; }
    const std::vector<BackdropRange>& BackdropRanges() const { return backdropRanges_; }
    const std::array<float, 3>& SkyColor() const { return skyColor_; }

    // Model matrix (column-major) of the backdrop for a camera at `eye` (world metres, +Y up): the model is
    // centred on the camera with x and z mirrored (its axes are -world X, up, -world Z - see backdrop.h).
    static void BackdropModel(const float eye[3], float* out) {
        std::fill(out, out + 16, 0.0f);
        out[0] = -kBackdropMetresPerUnit;
        out[5] = kBackdropMetresPerUnit;
        out[10] = -kBackdropMetresPerUnit;
        out[12] = eye[0];
        out[13] = eye[1];
        out[14] = eye[2];
        out[15] = 1.0f;
    }

    // Appends the draw items of the backdrop: the ground disc and dome to `opaque`; each semi-transparent range twice,
    // like the PS1 GPU treats textured semi-transparent polygons: the texels without the STP bit opaque (stpPass 1,
    // with the opaque items), the texels with it blended (stpPass 2, to `blended`). Blending every texel of the cloud
    // layers showed their rectangles as light panels over the sky.
    void AppendBackdropItems(std::vector<DrawItem>& opaque, std::vector<DrawItem>& blended, const float* mvp) const {
        for (const BackdropRange& r : backdropRanges_) {
            DrawItem item;
            item.firstVertex = r.first;
            item.vertexCount = r.count;
            std::copy(mvp, mvp + 16, item.mvp);
            if (r.blend == kBlendOpaque) {
                opaque.push_back(item);
                continue;
            }
            item.stpPass = 1;
            opaque.push_back(item);
            item.stpPass = 2;
            item.blend = r.blend;
            blended.push_back(item);
        }
    }

private:
    struct PaintProbe {std::array<float,3> eye;int key;bool covered;};
    std::vector<PaintProbe> paintProbeEyes_;
    PaintProbeCoverage paintCoverage_;
    int selectedPaintProbe_=-1;
    std::array<float,3> paintProbeEye_{};
    bool courseNight_=false;
    struct NativeLamp {std::array<float,3> point;uint32_t colour;};
    std::vector<NativeLamp> nativeLights_;
    struct Gt4Car {
        struct PaintTexture {uint32_t base;PngImage original;std::string name;};
        std::vector<PaintTexture> paintTextures;
        std::vector<std::map<std::string,std::filesystem::path>> paints;
        std::vector<std::vector<std::array<float,17>>> nativePaintMaterials;
        std::vector<GltfMaterial> nativeMaterials;
        struct LampRange {uint32_t first,count,states,blend;};
        std::vector<LampRange> lamps,cabinLamps;
        struct AttachmentRange {uint32_t first,count,blend;int wheel;bool spinning;uint32_t lampStates;};
        std::vector<AttachmentRange> attachments;
        std::vector<std::array<float,3>> roadSupport;
        bool glass=false,cockpitGlass=false;
        uint32_t selectedPaint=0;
        std::string sourceId;
        uint32_t count = 0, cockpitCount = 0, mirrorCount = 0, reflectionCount = 0;
        uint32_t opaqueCount = 0, cockpitOpaqueCount = 0;
        uint32_t cockpitReflectionFirst=0,cockpitReflectionCount=0;
        uint32_t surfaceReflectionCount=0,cockpitSurfaceReflectionCount=0;
        struct CabinRange {uint32_t first,count;bool alpha,steering;};
        std::vector<CabinRange> cabinRanges;
        bool attempted = false, nativeInterior = false, required = false;
        CockpitFit cockpit;
        double removedArea = 0;
        double upperFrameCutArea = 0;
        uint32_t totalVertices = 0;
        std::array<float,4> wheelShift{};
        std::array<float,4> wheelYShift{},wheelScale{1,1,1,1};
        std::array<float,4> wheelZShift{},wheelWidthScale{1,1,1,1};
        std::array<float,4> wheelBumpStop{1e9f,1e9f,1e9f,1e9f};
        bool nativeWheelPose=false;
        std::array<std::array<float,9>,4> wheelBasis{};
        std::vector<std::array<float,3>> auditPanelCentres;
        std::vector<uint32_t> auditBonnetTriangles,auditSurfaceTriangles;
    };
    static std::array<float,4> RotationQuaternion(const float* world) {
        std::array<float,16> rotation{};
        for(size_t c=0;c<3;++c) {
            const float length=std::sqrt(world[c*4]*world[c*4]+world[c*4+1]*world[c*4+1]+world[c*4+2]*world[c*4+2]);
            if(length<1e-6f)throw std::runtime_error("degenerate native model rotation");
            for(size_t r=0;r<3;++r)rotation[c*4+r]=world[c*4+r]/length;
        }
        world=rotation.data();std::array<float,4> q{};
            const float trace=world[0]+world[5]+world[10];
            if(trace>0) {
                const float t=2.f*std::sqrt(trace+1.f);q[3]=.25f*t;
                q[0]=(world[6]-world[9])/t;q[1]=(world[8]-world[2])/t;q[2]=(world[1]-world[4])/t;
            } else {
                size_t i=0;if(world[5]>world[0])i=1;if(world[10]>world[i*5])i=2;
                const size_t j=(i+1)%3,k=(i+2)%3;const float t=2.f*std::sqrt(1.f+world[i*5]-world[j*5]-world[k*5]);
                q[i]=.25f*t;q[j]=(world[j*4+i]+world[i*4+j])/t;q[k]=(world[k*4+i]+world[i*4+k])/t;
                q[3]=(world[j*4+k]-world[k*4+j])/t;
            }
            float norm=0;for(float v:q)norm+=v*v;norm=std::sqrt(norm);
        for(auto& value:q)value/=norm;return q;
    }
    static std::array<float,4> ComposeRotation(const std::array<float,4>& a,const std::array<float,4>& b) {
        return {a[3]*b[0]+a[0]*b[3]+a[1]*b[2]-a[2]*b[1],
                a[3]*b[1]-a[0]*b[2]+a[1]*b[3]+a[2]*b[0],
                a[3]*b[2]+a[0]*b[1]-a[1]*b[0]+a[2]*b[3],
                a[3]*b[3]-a[0]*b[0]-a[1]*b[1]-a[2]*b[2]};
    }
    static std::vector<NativeModelMaterial> NativeMaterials(const std::vector<GltfMaterial>& source,
            const std::vector<std::array<float,17>>* paint=nullptr) {
        std::vector<NativeModelMaterial> result;
        for(size_t i=0;i<source.size();++i) {
            const auto& m=source[i];NativeModelMaterial out;
            const auto& values=paint?paint->at(i):m.nativeGt4Values;
            std::copy_n(values.begin(),16,out.values.begin());
            out.values[16]=float(m.nativeGt4Type);out.values[17]=values[16];
            out.values[18]=float(m.nativeAlphaFunc);out.values[19]=float(m.nativeAlphaRef);
            out.values[20]=float(m.nativeAlphaFail);out.values[21]=float(m.nativeBlendEquation | (m.nativeTextureFunction<<8) | (m.nativeTextureAlpha<<10));
            out.values[22]=m.nativeCull && !std::getenv("GT2_AUDIT_RETAIN_NATIVE_BACKFACES")?1.f:0.f;
            out.values[23]=m.nativeDepthWrite?1.f:0.f;
            result.push_back(out);
        }return result;
    }
    void UpdateGt4Paint(int slot,uint32_t paint) const {
        auto& state=gt4Cars_.at(size_t(slot));
        if (!UsesGt4(slot) || state.paints.size()<2) return;
        const uint32_t selected=paint%uint32_t(state.paints.size());
        if (selected==state.selectedPaint) return;
        // Decode and validate every replacement before changing any GPU texture.
        std::vector<std::pair<uint32_t,std::vector<uint32_t>>> uploads;
        for(const auto& texture:state.paintTextures) {
            PngImage image=texture.original;
            if(const auto it=state.paints[selected].find(texture.name);it!=state.paints[selected].end()) image=ReadPngFile(it->second.string());
            if(image.width>texture.original.width || image.height>texture.original.height || image.width<=0 || image.height<=0 || image.rgba.size()!=size_t(image.width)*image.height*4)
                throw std::runtime_error("GT4 paint texture size mismatch");
            const auto& original=texture.original;
            std::vector<uint32_t> pixels(original.rgba.size()/4);
            for(size_t i=0;i<pixels.size();++i) {
                const size_t x=i%size_t(original.width),y=i/size_t(original.width);
                const auto* p=x<size_t(image.width) && y<size_t(image.height)?image.rgba.data()+(y*size_t(image.width)+x)*4:original.rgba.data()+i*4;
                // The old RGB patch export cropped REGION_CLAMP textures and
                // carried its own mask. Native GS coverage remains authoritative.
                pixels[i]=uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(state.nativeMaterials.empty()?original.rgba[i*4+3]:p[3])<<24;
            }
            uploads.emplace_back(texture.base,std::move(pixels));
        }
        for(const auto& [base,pixels]:uploads) renderer_.UploadExternalTexture(base,uint32_t(pixels.size()),pixels.data());
        if(!state.nativePaintMaterials.empty())renderer_.SetNativeModelMaterials(uint32_t(slot)*SceneRenderer::kNativeMaterialsPerSlot,
            NativeMaterials(state.nativeMaterials,&state.nativePaintMaterials.at(selected)));
        state.selectedPaint=selected;
    }
    void LoadGt4(int slot) {
        auto& state = gt4Cars_[size_t(slot)];
        if (state.attempted || state.sourceId.empty() || (!state.required && slots_[size_t(slot)].id.starts_with("ghost:"))) return;
        state.attempted = true;
        try {
            if (!gt4Set_.Has(state.sourceId)) {
                if (state.required) throw std::runtime_error("required GT4 expansion model is missing");
                return;
            }
            auto car = gt4Set_.Read(state.sourceId,loadCockpit_);
            ApplyNativeBodyPose(car);
            // Some GT4 bodies put all clearcoat in a separate environment
            // branch and leave diffuse MTL Ks at zero. Keep a soft clearcoat
            // fallback for those bodies instead of making the entire car matte.
            const size_t total = 3 * (car.exterior.TriangleCount() + car.cockpit.TriangleCount()+car.lamps.TriangleCount()+car.attachments.TriangleCount());
            if (total > SceneRenderer::kModelSetVertexStride) throw std::runtime_error("car exceeds vertex budget");
            const auto& stock = slots_[size_t(slot)].model;
            CarMeshOptions options; options.wheels = false;
            const auto original = BuildCarMesh(stock, options);
            if (original.empty()) throw std::runtime_error("missing stock alignment geometry");
            std::array<float,3> lo{1e9f,1e9f,1e9f}, hi{-1e9f,-1e9f,-1e9f};
            for (const auto& v : original) for (size_t k=0;k<3;++k) { lo[k]=std::min(lo[k],v.pos[k]); hi[k]=std::max(hi[k],v.pos[k]); }
            const float length = car.exterior.boundsMax[2] - car.exterior.boundsMin[2];
            if (!(length > 1.f && length < 12.f)) throw std::runtime_error("invalid car dimensions");
            const float scale = (hi[2]-lo[2])/length;
            if (!(scale > .7f && scale < 1.3f)) throw std::runtime_error("model dimensions do not match GT2 car");
            const std::array<float,3> offset{
                (lo[0]+hi[0]-(car.exterior.boundsMin[0]+car.exterior.boundsMax[0])*scale)*.5f,
                // GT4 car space uses the tyre contact plane as Y=0. Body
                // minima depend on exhausts and splitters and are not ground.
                // Native CAR4 meshes without a regional SpecDB pose are still
                // body-local, not ground-local. Retain the GT2 body reference:
                // subtracting ride height here sinks the shell, then the arch
                // fitter hides the error by shrinking its donor tyres.
                (!car.nativeMaterials.empty() && !car.nativeWheels ? 0.f :
                 (slots_[size_t(slot)].hasRaceWheels?
                 -.5f*(slots_[size_t(slot)].raceWheels.rideHeight[0]+slots_[size_t(slot)].raceWheels.rideHeight[1]):
                 (stock.wheels[0].y+stock.wheels[1].y+stock.wheels[2].y+stock.wheels[3].y-
                  2.f*stock.wheelRadiusFront-2.f*stock.wheelRadiusRear)/16384.f)-(car.nativeWheels?car.nativeGround*scale:0.f)),
                (lo[2]+hi[2]-(car.exterior.boundsMin[2]+car.exterior.boundsMax[2])*scale)*.5f};
            if (std::getenv("GT2_DEBUG_CABIN"))
                std::printf("GT4 align: offset %.3f %.3f %.3f wheel %.3f %.3f %.3f radius %.3f\n",
                    offset[0],offset[1],offset[2],stock.wheels[0].z/4096.f,stock.wheels[0].y/4096.f,
                    stock.wheels[0].x/4096.f,stock.wheelRadiusFront/4096.f);
            const auto nativeWheel=car.nativeInterior?FindNativeCockpitWheel(car.cockpit):NativeCockpitWheel{};
            CockpitFit importedFit = slots_[size_t(slot)].cockpit;
            if (loadCockpit_) {
                std::vector<CarMeshVertex> shell;
                for (const auto& p : car.exterior.primitives) for (auto index : p.indices) {
                    CarMeshVertex v{};
                    for (size_t k=0;k<3;++k) v.pos[k]=p.vertices.at(index).position[k]*scale+offset[k];
                    shell.push_back(v);
                }
                // This is only the dense-import profile bootstrap; the native
                // aperture is refitted below. GT2 polygon connectivity is not
                // an ownership boundary for split native material surfaces.
                importedFit=FitCockpit(stock,shell,false);
                const auto& stockFit=slots_[size_t(slot)].cockpit;
                if(std::getenv("GT2_DEBUG_CABIN"))std::printf("Native fit %s required %d valid %d eye %.3f %.3f %.3f windshield %.3f %.3f roof %.3f %.3f; stock eye %.3f %.3f %.3f\n",state.sourceId.c_str(),int(state.required),int(importedFit.valid),importedFit.eye[0],importedFit.eye[1],importedFit.eye[2],importedFit.windshieldY,importedFit.windshieldZ,importedFit.roofFrontY,importedFit.roofFrontZ,stockFit.eye[0],stockFit.eye[1],stockFit.eye[2]);
                // Some race meshes omit their glass planes, so a roof-profile
                // search can mistake the rear screen for the windshield.
                // Expansion physics donors need not resemble the visible car
                // (e.g. a road-going microcar can use an open racer donor).
                if (!importedFit.valid || (!state.required && (importedFit.windshieldZ>stockFit.eye[2]-.25f ||
                    std::abs(importedFit.windshieldZ-stockFit.windshieldZ)>.40f)))
                    importedFit=stockFit;
                if (!importedFit.valid) throw std::runtime_error("cannot fit imported cabin");
                if (importedFit.roofFrontY-importedFit.windshieldY>.60f) {
                    const auto profile=cockpit_detail::CentreProfile(shell);
                    float best=1e9f;
                    for (const auto& point:profile) {
                        if (point[0]>importedFit.roofFrontZ-.20f || point[0]<importedFit.windshieldZ-.25f) continue;
                        const float error=std::abs(point[1]-(importedFit.roofFrontY-.45f));
                        if (error<best) { best=error;importedFit.windshieldZ=point[0];importedFit.windshieldY=point[1]; }
                    }
                    importedFit.dashboardY=importedFit.windshieldY+.025f;
                }
                if(!car.nativeInterior && importedFit.roofRearZ<stockFit.eye[2]+.25f) {
                    // The rear roof knee must lie behind the occupied seat.
                    // Accessories/caps can produce a false knee ahead of it.
                    const float z=std::max(stockFit.roofRearZ,stockFit.eye[2]+.25f);
                    const float y=cockpit_detail::CabinSurfaceHeight(shell,z);
                    if(std::isfinite(y) && y>stockFit.eye[1]+.12f) {
                        importedFit.roofRearZ=z;importedFit.roofRearY=y;
                    } else {
                        importedFit.roofRearZ=stockFit.roofRearZ;
                        importedFit.roofRearY=stockFit.roofRearY;
                    }
                }
                if(car.nativeInterior && car.openTop) {
                    // An open body's roll bar/headrests are not a rear window.
                    // The backing must enclose the authored GT4 seating point.
                    const float rear=car.eye[2]*scale+offset[2]+.35f;
                    if(importedFit.rearWindowZ<rear) {
                        importedFit.rearWindowZ=rear;
                        const float left=cockpit_detail::SurfaceHeight(shell,rear,-importedFit.halfWidth*.85f);
                        const float right=cockpit_detail::SurfaceHeight(shell,rear,importedFit.halfWidth*.85f);
                        if(std::isfinite(left) && std::isfinite(right))importedFit.rearWindowY=std::min(left,right);
                    }
                }
                // Sparse triangles and interior caps must not drag a frame
                // corner onto a seat, mirror stalk or the middle of the glass.
                auto corners=[&](auto& pair,float y,float z,float width) {
                    for (size_t side=0;side<2;++side) {
                        const float sign=side?1.f:-1.f;
                        std::array<float,3> point{sign*width,y,z};float extent=0.f;
                        for (const auto& vertex:shell) {
                            const auto* v=vertex.pos;
                            if (v[0]*sign<=extent || v[0]*sign>importedFit.halfWidth*.97f ||
                                std::abs(v[1]-y)>.055f || std::abs(v[2]-z)>.22f) continue;
                            extent=v[0]*sign;point={v[0],v[1],v[2]};
                        }
                        pair[side]=point;
                    }
                };
                const auto fittedCowl=importedFit.cowl;
                corners(importedFit.frontCowl,importedFit.windshieldY,importedFit.windshieldZ,importedFit.halfWidth*.86f);
                corners(importedFit.frontHeader,importedFit.roofFrontY,importedFit.roofFrontZ,importedFit.halfWidth*.72f);
                corners(importedFit.rearCowl,importedFit.rearWindowY,importedFit.rearWindowZ,importedFit.halfWidth*.86f);
                corners(importedFit.rearHeader,importedFit.roofRearY,importedFit.roofRearZ,importedFit.halfWidth*.72f);
                for (size_t i=0;i<importedFit.cowl.size();++i) {
                    const float t=float(i)/float(importedFit.cowl.size()-1);
                    auto& point=importedFit.cowl[i];
                    // Preserve the measured windshield/body boundary. A chord
                    // through its centre cuts curved bonnet edges away and
                    // cannot be repaired by raising a generic dashboard strip.
                    const float x=importedFit.frontCowl[0][0]*(1-t)+importedFit.frontCowl[1][0]*t;
                    for(size_t j=1;j<fittedCowl.size();++j)if(x<=fittedCowl[j][0] || j+1==fittedCowl.size()){
                        const auto& a=fittedCowl[j-1];const auto& b=fittedCowl[j];const float u=std::clamp((x-a[0])/std::max(.001f,b[0]-a[0]),0.f,1.f);
                        point={x,a[1]+(b[1]-a[1])*u,a[2]+(b[2]-a[2])*u};break;
                    }
                    // Join trim to the real bonnet/screen surface, not a chord
                    // between corners: a bowed cowl leaves a visible slit.
                    const float surface=cockpit_detail::SurfaceHeight(shell,point[2]+.025f,point[0],point[1]+.012f);
                    if(std::isfinite(surface) && surface>=point[1]-.12f)point[1]=std::max(point[1],surface);
                    auto join=point;join[2]-=.18f;
                    const float bonnet=cockpit_detail::SurfaceHeight(shell,join[2],join[0],point[1]+.012f);
                    if(std::isfinite(bonnet))join[1]=bonnet;
                    importedFit.bonnetJoin[i]=join;
                }
                importedFit.bonnetJoinValid=true;
                cockpit_detail::RemoveTrimSpikes(importedFit.cowl);
                cockpit_detail::RemoveTrimSpikes(importedFit.bonnetJoin,true);
                cockpit_detail::FitBonnetSeal(importedFit,shell);
                // The source surface fit owns the aperture. The initial corner
                // search is only a width hint; retaining its old Z places the
                // generated A-pillar ahead of the actual cowl on curved bodies.
                importedFit.frontCowl={importedFit.cowl.front(),importedFit.cowl.back()};
                if(!car.nativeInterior)for(auto& header:importedFit.frontHeader) {
                    // A wide accessory can win the corner search on a donor
                    // fit. Keep the upper windscreen boundary behind the
                    // measured cowl, so the headliner does not cross the
                    // bonnet clipping prism and leave an overhead slit.
                    header[2]=std::max(header[2],cockpit_detail::CowlZAt(importedFit,header[0])+.025f);
                }
                importedFit.sideSillValid=false;
                importedFit.smoothShell=!car.nativeInterior;
                importedFit.authoredUpperShell=(!car.nativeInterior || !car.openTop) && !std::getenv("GT2_AUDIT_CUT_UPPER_SHELL");
                importedFit.openTop=car.openTop;
                importedFit.floorY=std::max(importedFit.floorY,
                    std::min(importedFit.wheelWells[0][1],importedFit.wheelWells[2][1])-.07f);
                // Restore the pre-v19 seated origin. Cabin furniture changes
                // must not silently move the player or the interactive wheel.
                const float seatedY=std::min(importedFit.eye[1],stockFit.eye[1]);
                importedFit.eye=stockFit.eye;
                importedFit.eye[1]=seatedY;
                importedFit.eye[1]=std::min(importedFit.eye[1],
                    std::min(importedFit.frontHeader[0][1],importedFit.frontHeader[1][1])-.14f);
                if(!car.nativeInterior) {
                    // A GT2 donor's eye can sit below the replacement's belt
                    // line. Preserve it when usable, otherwise fit inside the
                    // actual glass aperture instead of looking through a door.
                    const float ceiling=std::min(importedFit.frontHeader[0][1],importedFit.frontHeader[1][1])-.14f;
                    const float sill=importedFit.windshieldY+.08f;
                    importedFit.eye[1]=std::max(importedFit.eye[1],std::min(sill,ceiling));
                }
                const float headerZ=std::max(importedFit.frontHeader[0][2],importedFit.frontHeader[1][2]);
                importedFit.eye[2]=std::max(importedFit.eye[2],headerZ+.16f);
                if(!car.nativeInterior)for(size_t side=0;side<2;++side) {
                    if(importedFit.rearHeader[side][2]<importedFit.eye[2]+.15f) {
                        auto& p=importedFit.rearHeader[side];p[2]=std::max(importedFit.roofRearZ,importedFit.eye[2]+.25f);
                        const float y=cockpit_detail::CabinSurfaceHeight(shell,p[2],p[0]*.8f);
                        p[1]=std::isfinite(y)?std::max(y,importedFit.eye[1]+.14f):std::max(importedFit.roofRearY,importedFit.eye[1]+.14f);
                    }
                }
                importedFit.mirror={0,std::min(importedFit.frontHeader[0][1],importedFit.frontHeader[1][1])-.075f,headerZ+.035f};
                if (std::getenv("GT2_DEBUG_CABIN")) {
                    for (const auto* pair:{&importedFit.frontCowl,&importedFit.frontHeader,&importedFit.rearHeader,&importedFit.rearCowl})
                        for (const auto& v:*pair) std::printf("GT4 fit corner: %.3f %.3f %.3f\n",v[0],v[1],v[2]);
                }
                std::printf("GT4 cabin fit: %s eye %.3f %.3f %.3f cowl %.3f %.3f roof %.3f %.3f\n",state.sourceId.c_str(),
                    importedFit.eye[0],importedFit.eye[1],importedFit.eye[2],importedFit.windshieldY,importedFit.windshieldZ,
                    importedFit.roofFrontY,importedFit.roofFrontZ);
            }
            if(!car.nativeInterior && car.cabinHeightOffset!=0) {
                const float dy=car.cabinHeightOffset*scale;
                importedFit.roofY+=dy;importedFit.dashboardY+=dy;importedFit.windshieldY+=dy;
                importedFit.roofFrontY+=dy;importedFit.roofRearY+=dy;importedFit.rearWindowY+=dy;
                for(auto* pair:{&importedFit.frontCowl,&importedFit.frontHeader,&importedFit.rearHeader,&importedFit.rearCowl})
                    for(auto& point:*pair)point[1]+=dy;
                for(auto& point:importedFit.cowl)point[1]+=dy;
                // These two surfaces are fitted to actual exterior metal.
                // A seating-height calibration must not lift them through the bonnet.
                importedFit.eye[1]+=dy;importedFit.mirror[1]+=dy;
            }
            std::array<float,4> wheelShift{};
            std::array<float,4> wheelYShift{},wheelScale{1,1,1,1};
            std::array<float,4> wheelZShift{},wheelWidthScale{1,1,1,1};
            std::array<float,4> wheelBumpStop{};
            importedFit.wheelWells=slots_[size_t(slot)].cockpit.wheelWells;
            std::vector<std::array<float,3>> painted;
            for(const auto& p:car.exterior.primitives) {
                const auto* material=p.material>=0?&car.exterior.materials.at(size_t(p.material)):nullptr;
                const auto* image=material && material->baseColorImage>=0?&car.exterior.images.at(size_t(material->baseColorImage)):nullptr;
                for(size_t i=0;i+2<p.indices.size();i+=3) {
                    const std::array<GltfVertex,3> triangle{p.vertices.at(p.indices[i]),p.vertices.at(p.indices[i+1]),p.vertices.at(p.indices[i+2])};
                    if(ModelWheelBacking(triangle,material,image))continue;
                    for(const auto& v:triangle)painted.push_back({v.position[0]*scale+offset[0],v.position[1]*scale+offset[1],v.position[2]*scale+offset[2]});
                }
            }
            for (size_t w=0;w<4;++w) {
                auto& well=importedFit.wheelWells[w];
                auto target=well;
                if(car.nativeWheels)for(size_t k=0;k<5;++k)target[k]=car.wheelWells[w][k]*scale+(k<3?offset[k]:0.f);
                auto fitted=car.nativeWheelPose?target:FitModelWheel(painted,target);
                if(!car.nativeMaterials.empty() && !car.nativeWheels) {
                    // No recovered tyre size: keep the actual donor dimensions
                    // and contact reference, rather than inventing smaller tyres.
                    fitted[1]=target[1];fitted[3]=target[3];
                }
                wheelShift[w]=fitted[0]-well[0];wheelYShift[w]=fitted[1]-well[1];wheelScale[w]=fitted[3]/well[3];
                wheelZShift[w]=fitted[2]-well[2];wheelWidthScale[w]=fitted[4]/well[4];
                well=fitted;
                wheelBumpStop[w]=ModelWheelBumpStop(painted,well);
                if(std::getenv("GT2_AUDIT_WHEEL_COLOR"))std::printf("GT4 wheel %zu radius %.4f bump stop %.4f\n",w,well[3],wheelBumpStop[w]);
            }
            // Stage everything first; a failed import must leave the stock slot
            // and the shared texture allocation cursor untouched.
            auto imageCache = gt4Images_;
            uint32_t cursor = gt4TexelsUsed_;
            struct Upload { uint32_t base; std::vector<uint32_t> texels; };
            std::vector<Upload> uploads;
            std::vector<Gt4Car::PaintTexture> paintTextures;
            std::vector<SceneVertex> vertices;
            vertices.reserve(total);
            std::vector<SceneVertex> mirrors, reflections, cabinReflections;
            std::vector<NativeModelVertex> nativeVertices,nativeMirrors,nativeReflections,nativeCabinReflections;
            std::vector<SceneVertex> surfaceReflections,cabinSurfaceReflections;
            std::vector<NativeModelVertex> nativeSurfaceReflections,nativeCabinSurfaceReflections;
            using SurfaceTriangle=std::array<std::array<float,3>,3>;
            const auto surfaceKey=[](const GltfPrimitive& p,size_t i) {
                SurfaceTriangle key;for(size_t k=0;k<3;++k)key[k]=p.vertices.at(p.indices.at(i+k)).position;
                std::sort(key.begin(),key.end());return key;
            };
            std::map<SurfaceTriangle,SurfaceTriangle> opaqueSurfaces;
            for(const auto& p:car.exterior.primitives) {
                const auto& m=car.exterior.materials.at(size_t(p.material));
                if(m.nativeGt4Type<0 || !m.nativeDepthWrite || m.alphaBlend)continue;
                for(size_t i=0;i+2<p.indices.size();i+=3) {
                    SurfaceTriangle ordered;for(size_t k=0;k<3;++k)ordered[k]=p.vertices.at(p.indices[i+k]).position;
                    opaqueSurfaces.emplace(surfaceKey(p,i),ordered);
                }
            }
            double sourceArea=0,loadedArea=0;
            const auto triangleArea=[](const auto& a,const auto& b,const auto& c) {
                std::array<double,3> u{},v{};
                for(size_t k=0;k<3;++k){u[k]=double(b.position[k])-a.position[k];v[k]=double(c.position[k])-a.position[k];}
                const double x=u[1]*v[2]-u[2]*v[1],y=u[2]*v[0]-u[0]*v[2],z=u[0]*v[1]-u[1]*v[0];
                return .5*std::sqrt(x*x+y*y+z*z);
            };
            std::unordered_map<std::string,bool> mirrorPaintNeutral;
            auto mirrorPatchesFor=[&](const GltfPrimitive& p,const GltfMesh& mesh,const GltfMaterial& m) {
                std::vector<GltfPrimitive> patches;
                if(m.baseColorImage<0 || m.reflection || m.wheelAttachment>=0 || m.lampStates)return patches;
                const size_t image=size_t(m.baseColorImage);
                patches=SideMirrorPatches(p,mesh.images.at(image),scale,offset,importedFit,m.clampU,m.clampV);
                if(!patches.empty() && image<car.imageNames[1].size())for(const auto& palette:car.paints) {
                    const auto found=palette.find(car.imageNames[1][image]);
                    if(found==palette.end())continue;
                    const auto path=found->second.string();auto cached=mirrorPaintNeutral.find(path);
                    if(cached==mirrorPaintNeutral.end())cached=mirrorPaintNeutral.emplace(path,NeutralMirrorImage(ReadPngFile(path))).first;
                    if(!cached->second){patches.clear();break;}
                }
                return patches;
            };
            std::vector<ModelGlazingOpening> glazing,mirrorGaps;
            if(loadCockpit_ && !(std::getenv("GT2_AUDIT_COCKPIT") && std::getenv("GT2_AUDIT_RETAIN_TINT"))) {
                // GT4's light callback batches also carry window tint. A
                // lamp-state tag selects a callback, not exclusively a lamp.
                for(const auto* source:{&car.cockpit,&car.lamps}){
                const auto& mesh=*source;
                std::vector<ModelWindowHoles> panes(mesh.images.size()),cutouts(mesh.images.size()),clearWindows(mesh.images.size()),darkTint(mesh.images.size()),colourTint(mesh.images.size());
                for(size_t i=0;i<mesh.images.size();++i){
                    const auto& img=mesh.images[i];std::vector<uint8_t> tint(size_t(img.width)*img.height),empty(tint.size()),zero(tint.size()),dark(tint.size());
                    for(size_t p=0;p<tint.size();++p){const auto* c=&img.rgba[p*4];tint[p]=ModelGlassPixel(c);empty[p]=!MirrorPixelCovered(c);zero[p]=c[3]==0;
                        dark[p]=ModelBlendGlassPixel(c);}
                    // An entirely translucent neutral layer is itself glazing,
                    // including tiled tint ramps. Do not cut each repeated dark
                    // texel into hundreds of tiny holes in one physical pane.
                    bool tintLayer=!img.rgba.empty();
                    for(size_t p=0;p<tint.size();++p){const auto* c=&img.rgba[p*4];
                        tintLayer=tintLayer && std::max({c[0],c[1],c[2]})-std::min({c[0],c[1],c[2]})<=24 && c[3]>0 && c[3]<250;}
                    if(tintLayer)std::fill(dark.begin(),dark.end(),1);
                    if(i<car.imageNames[1].size())for(const auto& palette:car.paints){
                        const auto found=palette.find(car.imageNames[1][i]);if(found==palette.end())continue;
                        const auto variant=ReadPngFile(found->second.string());
                        if(variant.width!=img.width || variant.height!=img.height){std::fill(tint.begin(),tint.end(),0);std::fill(empty.begin(),empty.end(),0);std::fill(zero.begin(),zero.end(),0);std::fill(dark.begin(),dark.end(),0);break;}
                        for(size_t p=0;p<tint.size();++p){empty[p]&=variant.rgba[p*4+3]<128;zero[p]&=variant.rgba[p*4+3]==0;
                            // Paint variants can change the tint ramp while the
                            // same texel remains glass. Require glass in every
                            // palette, not identical RGB/alpha bytes.
                            dark[p]=uint8_t(dark[p]!=0 && ModelBlendGlassPixel(&variant.rgba[p*4]));
                            for(size_t c=0;c<3;++c)tint[p]&=variant.rgba[p*4+c]==img.rgba[p*4+c];}
                    }
                    panes[i]=ModelGlassHoles(img,tint);cutouts[i]=ExactModelMask(img,empty);clearWindows[i]=ReflectionMaskParts(img,zero);darkTint[i]=ReflectionMaskParts(img,dark);
                    // Bright alpha texels in a coloured non-depth overlay
                    // are often sponsor lettering. A wholly neutral tint ramp
                    // can still contain bright glass highlights.
                    const bool neutralRamp=ClassifyNeutralOverlay(img).translucent;
                    for(size_t p=0;p<dark.size();++p)dark[p]=uint8_t(dark[p]!=0 &&
                        (ModelBlendColourGlassPixel(&img.rgba[p*4]) || (neutralRamp && ModelBrightGlassPixel(&img.rgba[p*4]))));
                    colourTint[i]=ReflectionMaskParts(img,dark);
                }
                std::map<std::array<int,4>,ModelWindowHoles> alphaApertures;
                for(const auto& p:mesh.primitives){
                    // Steering spoke masks are control geometry, not windows.
                    if(source==&car.cockpit && nativeWheel.valid &&
                       nativeWheel.primitives.at(size_t(&p-mesh.primitives.data())))continue;
                    if(!p.hasUvs || p.material<0)continue;const auto& m=mesh.materials.at(size_t(p.material));
                    if(m.baseColorImage<0 || m.reflection || m.wheelAttachment>=0)continue;
                    // Mirror reflection passes have no diffuse UV mask. Clip
                    // their rectangular carrier by the actual source cutout.
                    const auto patches=mirrorPatchesFor(p,mesh,m);
                    for(const auto& patch:patches)for(size_t i=0;i+2<patch.indices.size();i+=3){
                        const std::array<GltfVertex,3> t{patch.vertices.at(patch.indices[i]),patch.vertices.at(patch.indices[i+1]),patch.vertices.at(patch.indices[i+2])};
                        const auto inside=GlazingInside(t,cutouts.at(size_t(m.baseColorImage)),m.clampU,m.clampV);
                        for(size_t j=0;j+2<inside.size();j+=3){auto opening=GlazingOpening({inside[j],inside[j+1],inside[j+2]});
                            float n=0;for(float a:opening.normal)n+=a*a;if(n>.5f)mirrorGaps.push_back(opening);}
                    }
                    size_t mirrorIndices=0;for(const auto& patch:patches)mirrorIndices+=patch.indices.size();
                    if(mirrorIndices==p.indices.size())continue;
                    // Already transparent source texels only mask a coplanar
                    // environment pass. Never carve their convex boundary out
                    // of the opaque source frame (lettering/pillars survive).
                    for(bool reflectionOnly:{false,true}){
                    auto holes=reflectionOnly?ModelWindowHoles{}:panes.at(size_t(m.baseColorImage));
                    if(reflectionOnly){
                        if((!m.alphaBlend && !NativeAlphaAcceptsZero(m)) ||
                           (car.nativeInterior && m.alphaBlend && m.nativeDepthWrite &&
                            m.nativeTextureAlpha && m.nativeBlendEquation==0x44))holes=clearWindows.at(size_t(m.baseColorImage));
                        if(car.nativeInterior && !m.alphaBlend && m.nativeGt4Type==1 && m.nativeTextureAlpha &&
                           m.nativeTextureFunction<=1 && (m.nativeAlphaFail==0 || m.nativeAlphaFail==2)) {
                            const std::array<int,4> key{m.baseColorImage,m.nativeAlphaFunc,m.nativeAlphaRef,m.nativeTextureFunction};
                            auto found=alphaApertures.find(key);
                            if(found==alphaApertures.end()) {
                                const size_t image=size_t(m.baseColorImage);const auto& img=mesh.images[image];
                                std::vector<uint8_t> mask(img.rgba.size()/4);
                                for(size_t pixel=0;pixel<mask.size();++pixel)mask[pixel]=NativeTextureAlphaDiscard(m,img.rgba[pixel*4+3]);
                                if(image<car.imageNames[1].size())for(const auto& palette:car.paints) {
                                    const auto variant=palette.find(car.imageNames[1][image]);if(variant==palette.end())continue;
                                    const auto png=ReadPngFile(variant->second.string());
                                    if(png.width!=img.width || png.height!=img.height){std::fill(mask.begin(),mask.end(),0);break;}
                                    for(size_t pixel=0;pixel<mask.size();++pixel)mask[pixel]=uint8_t(mask[pixel]!=0 && NativeTextureAlphaDiscard(m,png.rgba[pixel*4+3]));
                                }
                                found=alphaApertures.emplace(key,ReflectionMaskParts(img,mask)).first;
                            }
                            holes=found->second;
                        }
                    }
                    else if(m.alphaBlend) {
                        // Neutral non-depth overlays cut only their own UVs
                        // below; they must not stamp hundreds of shadow texels
                        // into unrelated body and reflection geometry.
                        const bool flatPane=ClassifyNeutralOverlay(mesh.images.at(size_t(m.baseColorImage))).flat;
                        holes=(m.nativeGt4Type>=0 && !m.nativeDepthWrite && !flatPane?colourTint:darkTint).at(size_t(m.baseColorImage));
                    }
                    if(holes.empty())continue;
                    const size_t beforeGlazing=glazing.size();
                    for(size_t i=0;i+2<p.indices.size();i+=3){
                        const std::array<GltfVertex,3> triangle{p.vertices.at(p.indices[i]),p.vertices.at(p.indices[i+1]),p.vertices.at(p.indices[i+2])};
                        auto inside=GlazingInside(triangle,holes,m.clampU,m.clampV);
                        if(!reflectionOnly && m.alphaBlend && m.nativeGt4Type>=0 &&
                           (!m.nativeDepthWrite || !std::getenv("GT2_AUDIT_UNBOUNDED_DEPTH_GLASS"))){
                            float front=importedFit.windshieldZ;
                            for(const auto& station:importedFit.cowl)front=std::min(front,station[2]);
                            if(car.nativeInterior)front=std::min(front,car.eye[2]*scale+offset[2]-.85f);
                            // Blended trim and headlamp lenses can contain
                            // glass-coloured texels even when they write depth.
                            // Lenses below the cowl cannot define cabin holes.
                            // Keep depth-writing windscreens above it: a low
                            // racing screen can extend ahead of the fitted knee.
                            std::array<ModelClipVolume,1> ahead{{{{0,0,1,(front-.05f-offset[2])/scale}}}};
                            if(m.nativeDepthWrite && !std::getenv("GT2_AUDIT_FLAT_DEPTH_GLASS_BOUNDARY")) {
                                const float belt=std::min(importedFit.frontCowl[0][1],importedFit.frontCowl[1][1]);
                                ahead[0].push_back({0,1,0,(belt-.05f-offset[1])/scale});
                            }
                            std::vector<GltfVertex> bounded;
                            for(size_t j=0;j+2<inside.size();j+=3){
                                const std::array<GltfVertex,3> t{inside[j],inside[j+1],inside[j+2]};
                                const auto pieces=SubtractModelVolumes(t,ahead);bounded.insert(bounded.end(),pieces.begin(),pieces.end());
                            }
                            inside=std::move(bounded);
                        }
                        if(!m.alphaBlend && !reflectionOnly){
                            std::vector<GltfVertex> split;
                            for(size_t j=0;j+2<inside.size();j+=3){
                                const std::array<GltfVertex,3> t{inside[j],inside[j+1],inside[j+2]};
                                const auto pieces=SplitModelCowl(t,importedFit.cowl,scale,offset);
                                split.insert(split.end(),pieces.begin(),pieces.end());
                            }
                            inside=std::move(split);
                        }
                        for(size_t j=0;j+2<inside.size();j+=3){
                            std::array<float,3> center{};for(size_t v=0;v<3;++v)for(size_t k=0;k<3;++k)center[k]+=(inside[j+v].position[k]*scale+offset[k])/3.f;
                            const size_t side=center[0]<0?0:1;const auto& f=importedFit;
                            // Dark teal paint can share the glass palette. An
                            // opaque hood ahead of the cowl is not a window.
                            if(!m.alphaBlend && !reflectionOnly && center[2]<cockpit_detail::CowlZAt(f,center[0]))continue;
                            const float belt=std::min(f.frontCowl[side][1],f.rearCowl[side][1]);
                            // Alpha-cut decals below the passenger-cell sill
                            // do not define holes in the environment layer.
                            if(reflectionOnly && center[1]<belt-.05f)continue;
                            // The procedural furniture fit is inside the shell;
                            // its narrow rear corners are not the window border.
                            // Open native bodies can have their profile knee at
                            // a headrest. Their authored seating point also bounds
                            // the windscreen, independently of procedural trim.
                            const float front=car.nativeInterior?std::min(f.frontCowl[side][2]-.65f,car.eye[2]*scale+offset[2]-.85f):f.frontCowl[side][2]-.65f;
                            // Depth-writing windscreens were already clipped
                            // against the cowl-height/front prism above. A
                            // second flat front test would reject the surviving
                            // upper pane on low/open cars whose fitted knee is
                            // a rear headrest, leaving tinted fragments behind.
                            const bool boundedUpperGlass=car.nativeInterior && m.alphaBlend &&
                                m.nativeGt4Type>=0 && m.nativeDepthWrite &&
                                center[1]>=std::min(f.frontCowl[0][1],f.frontCowl[1][1])-.05f;
                            if(center[1]<belt-.25f || center[1]>car.exterior.boundsMax[1]*scale+offset[1]+.01f || (center[2]<front && !boundedUpperGlass) ||
                               center[2]>car.exterior.boundsMax[2]*scale+offset[2]-.10f ||
                               std::abs(center[0]-offset[0])>std::max(std::abs(car.exterior.boundsMin[0]),std::abs(car.exterior.boundsMax[0]))*scale+.01f){

                                continue;
                            }
                            auto opening=GlazingOpening({inside[j],inside[j+1],inside[j+2]});opening.reflectionOnly=reflectionOnly;
                            // Neutral translucent shadows can lie on the bonnet.
                            // Authored teal glazing is unambiguous even on very
                            // shallow racing windscreens; do not reject it by slope.
                            if(m.alphaBlend && !reflectionOnly && !IsWindowLayerNormal(opening.normal)) {
                                const auto& img=mesh.images.at(size_t(m.baseColorImage));
                                float uv[2]{};for(size_t v=0;v<3;++v)for(size_t k=0;k<2;++k)uv[k]+=inside[j+v].uv[k]/3.f;
                                for(size_t k=0;k<2;++k)uv[k]=(k?m.clampV:m.clampU)?std::clamp(uv[k],0.f,1.f):uv[k]-std::floor(uv[k]);
                                const size_t x=std::min(size_t(img.width-1),size_t(uv[0]*img.width)),y=std::min(size_t(img.height-1),size_t(uv[1]*img.height));
                                const auto* pixel=&img.rgba[(y*img.width+x)*4];
                                // `inside` already follows the palette-invariant
                                // glass mask. Sampling its centroid again can
                                // hit a tiny opaque island and reject the whole
                                // curved roof fragment, leaving a triangular rim.
                                const bool overheadGlass=car.nativeInterior && !car.openTop && m.nativeDepthWrite &&
                                    center[1]>=car.eye[1]*scale+offset[1];
                                if(!ModelBlendColourGlassPixel(pixel) && !overheadGlass)continue;
                            }
                            float n=0;for(float a:opening.normal)n+=a*a;if(n>.5f)glazing.push_back(opening);
                        }
                    }
                    if(std::getenv("GT2_DEBUG_CABIN"))std::printf("glazing material %d holes %zu openings %zu\n",p.material,holes.size(),glazing.size()-beforeGlazing);
                    }
                }
            }
            }
            if(std::getenv("GT2_DEBUG_CABIN"))std::fprintf(stderr,"glazing total %zu mirror %zu\n",glazing.size(),mirrorGaps.size());
            auto append = [&](const GltfMesh& mesh, bool seated) {
                std::vector<uint32_t> images(mesh.images.size());
                std::vector<ModelWindowHoles> windowHoles(mesh.images.size());
                if(seated && !car.nativeInterior)for(size_t i=0;i<mesh.images.size();++i){
                    const auto& img=mesh.images[i];std::vector<uint8_t> mask(size_t(img.width)*img.height);
                    for(size_t p=0;p<mask.size();++p)mask[p]=ModelGlassPixel(&img.rgba[p*4]);
                    if(std::none_of(mask.begin(),mask.end(),[](auto v){return v!=0;}))continue;
                    const auto& names=car.imageNames[1];
                    if(i<names.size())for(const auto& palette:car.paints){
                        const auto found=palette.find(names[i]);if(found==palette.end())continue;
                        const auto variant=ReadPngFile(found->second.string());
                        if(variant.width!=img.width || variant.height!=img.height){std::fill(mask.begin(),mask.end(),0);break;}
                        for(size_t p=0;p<mask.size();++p)for(size_t c=0;c<3;++c)mask[p]&=variant.rgba[p*4+c]==img.rgba[p*4+c];
                    }
                    windowHoles[i]=ModelGlassHoles(img,mask);
                }
                std::vector<ModelWindowHoles> neutralEdges(mesh.images.size());
                if(seated && !std::getenv("GT2_AUDIT_RETAIN_TINT"))for(size_t i=0;i<mesh.images.size();++i){
                    const auto& img=mesh.images[i];std::vector<uint8_t> mask(size_t(img.width)*img.height);
                    for(size_t p=0;p<mask.size();++p)mask[p]=ModelNeutralGlassPixel(&img.rgba[p*4]);
                    neutralEdges[i]=ReflectionMaskParts(img,mask);
                }
                std::vector<bool> fractionalAlpha(mesh.images.size(),false),zeroAlpha(mesh.images.size(),false);
                for (size_t i=0;i<mesh.images.size();++i) {
                    const auto& img=mesh.images[i];
                    if (img.width<=0 || img.height<=0 || img.width>4096 || img.height>4096 ||
                        img.rgba.size()!=size_t(img.width)*img.height*4) throw std::runtime_error("invalid model texture");
                    for(size_t a=3;a<img.rgba.size();a+=4){fractionalAlpha[i]=fractionalAlpha[i] || (img.rgba[a]>0 && img.rgba[a]<255);zeroAlpha[i]=zeroAlpha[i] || img.rgba[a]==0;}
                    const auto& names=car.imageNames[seated?1:0];
                    const std::string name=(&mesh==&car.lamps && car.nativeMaterials.empty())?std::string():i<names.size()?names[i]:std::string();
                    const bool varying=std::any_of(car.paints.begin(),car.paints.end(),[&](const auto& palette){return palette.contains(name);});
                    const std::string key=(varying?"paint-slot:"+std::to_string(slot)+":"+name+":":"")+
                        std::to_string(img.width)+":"+std::to_string(img.height)+":"+Sha1Hex(img.rgba);
                    if (const auto found=imageCache.find(key); found!=imageCache.end()) { images[i]=found->second; continue; }
                    const uint32_t count=uint32_t(img.width)*uint32_t(img.height);
                    if (uint64_t(cursor)+count>SceneRenderer::kModelSetTexelLimit) throw std::runtime_error("model set texture budget exhausted");
                    Upload upload{SceneRenderer::kModelSetTexelBase+cursor,{}};
                    upload.texels.resize(count);
                    for (size_t j=0;j<count;++j) {
                        const auto* p=&img.rgba[j*4];
                        upload.texels[j]=uint32_t(p[0])|uint32_t(p[1])<<8|uint32_t(p[2])<<16|uint32_t(p[3])<<24;
                    }
                    images[i]=upload.base; imageCache.emplace(key,upload.base); cursor+=count; uploads.push_back(std::move(upload));
                    if(varying)paintTextures.push_back({images[i],img,name});
                }
                // A closed authored cabin extends behind the foremost cowl
                // station, including the door jamb beside the dashboard. The
                // generated inset cowl's side corners are not its outer wall.
                const bool closedNativeLining=car.nativeInterior && !car.openTop &&
                    !(std::getenv("GT2_AUDIT_COCKPIT") && std::getenv("GT2_AUDIT_LEGACY_NATIVE_LINING"));
                float closedNativeFront=importedFit.cowl.front()[2];
                for(const auto& station:importedFit.cowl)closedNativeFront=std::min(closedNativeFront,station[2]);
                for (const auto& p : mesh.primitives) {
                    // PS2 strip/reflection branches can reverse triangle order.
                    // Use authored normals to give the inner-side material and
                    // diagnostic colours a consistent meaning on every triangle.
                    auto indices=p.indices;
                    auto points=p.vertices;
                    if(!seated && &mesh!=&car.reflections)for(size_t i=0;i+2<indices.size();i+=3)
                        sourceArea+=triangleArea(points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2]));
                    if (car.nativeMaterials.empty() && p.hasNormals) for (size_t i=0;i+2<indices.size();i+=3) {
                        const auto& a=p.vertices.at(indices[i]);
                        const auto& b=p.vertices.at(indices[i+1]);
                        const auto& c=p.vertices.at(indices[i+2]);
                        std::array<float,3> u{},v{},n{};
                        for (size_t k=0;k<3;++k) {
                            u[k]=b.position[k]-a.position[k];v[k]=c.position[k]-a.position[k];
                            n[k]=a.normal[k]+b.normal[k]+c.normal[k];
                        }
                        const float dot=(u[1]*v[2]-u[2]*v[1])*n[0]+(u[2]*v[0]-u[0]*v[2])*n[1]+(u[0]*v[1]-u[1]*v[0])*n[2];
                        if (dot<0) std::swap(indices[i+1],indices[i+2]);
                    }
                    const auto* mat=p.material>=0 && size_t(p.material)<mesh.materials.size()?&mesh.materials[size_t(p.material)]:nullptr;
                    bool surfaceReflection=mat && mat->reflection && mat->nativeGt4Type>=0 && !p.indices.empty();
                    for(size_t i=0;surfaceReflection && i+2<p.indices.size();i+=3)
                        surfaceReflection=opaqueSurfaces.contains(surfaceKey(p,i));
                    if(surfaceReflection)for(size_t i=0;i+2<indices.size();i+=3) {
                        // The same positions with a different strip order can
                        // rasterize to different float depth values. Match the
                        // opaque triangle's order for the exact-depth overlay.
                        const auto& ordered=opaqueSurfaces.at(surfaceKey(p,i));
                        for(size_t k=0;k<3;++k)for(size_t j=k;j<3;++j)
                            if(points.at(indices[i+j]).position==ordered[k]) {std::swap(indices[i+k],indices[i+j]);break;}
                    }
                    if(seated && mat && mat->exteriorOnly)continue;
                    const int image=mat?mat->baseColorImage:-1;
                    const bool textured=p.hasUvs && image>=0 && size_t(image)<mesh.images.size();
                    // rearView is a texture-level import hint, not a surface ID:
                    // GT4 can reuse that texture on the floor and seats. Validate
                    // every draw geometrically, including explicitly tagged ones.
                    bool rearView=false;
                    std::vector<GltfPrimitive> mirrorPatches;
                    if(seated && textured && mat && !mat->reflection && mat->wheelAttachment<0 && !mat->lampStates) {
                        mirrorPatches=mirrorPatchesFor(p,mesh,*mat);
                        rearView=!mirrorPatches.empty();
                    }
                    // Binary cutouts whose alpha test rejects zero remain in
                    // the depth-writing mask pass for early occlusion.
                    // GS lit-car alpha is always 127/128. Keep that destination
                    // alpha, but depth-write a binary/opaque diffuse surface so
                    // cabin walls can reject the course before it is shaded.
                    const bool vertexCoverage=(!mat || mat->nativeGt4Type!=1) &&
                        std::any_of(points.begin(),points.end(),[](const auto& v){return v.color[3]<.99f;});
                    // Native blend draws can contain binary transparent texels
                    // with alpha testing disabled (e.g. open-cabin covers).
                    // Promoting them to opaque turns those holes into black caps.
                    const bool alphaBlend=mat && mat->alphaBlend &&
                        ((textured && (fractionalAlpha[size_t(image)] ||
                          (zeroAlpha[size_t(image)] && (std::getenv("GT2_AUDIT_LEGACY_ALPHA_FAIL")?NativeAlphaAcceptsZero(*mat):NativeAlphaBlendsZero(*mat))))) || vertexCoverage || mat->baseColorFactor[3]<.99f);
                    for(const auto& patch:mirrorPatches) {
                        std::array<std::array<float,2>,2> lo{{{1e9f,1e9f},{1e9f,1e9f}}},hi{{{-1e9f,-1e9f},{-1e9f,-1e9f}}};
                        for(auto index:patch.indices){const auto& v=patch.vertices.at(index);const size_t side=v.position[0]<0?0:1;
                            for(size_t k=0;k<2;++k){lo[side][k]=std::min(lo[side][k],v.position[k]);hi[side][k]=std::max(hi[side][k],v.position[k]);}}
                        for(size_t i=0;i+2<patch.indices.size();i+=3) {
                            if(!RearFacingMirrorTriangle(patch.vertices.at(patch.indices[i]).position,patch.vertices.at(patch.indices[i+1]).position,patch.vertices.at(patch.indices[i+2]).position))continue;
                            for(size_t j=0;j<3;++j) {
                                const auto& v=patch.vertices.at(patch.indices[i+j]);const auto& img=mesh.images[size_t(image)];SceneVertex out{};
                                for(size_t k=0;k<3;++k)out.pos[k]=v.position[k]*scale+offset[k];out.pos[2]+=.002f*scale;
                                out.texel[0]=v.uv[0]*img.width;out.texel[1]=v.uv[1]*img.height;
                                out.page=images[size_t(image)];out.clut=uint32_t(img.width)|uint32_t(img.height)<<16;
                                out.flags=kTextured|kExternalTexture|kModelMirrorMask|kMirrorTexture;
                                if(mat->clampU)out.flags|=kModelClampU;if(mat->clampV)out.flags|=kModelClampV;
                                const size_t side=v.position[0]<0?0:1;
                                for(size_t k=0;k<2;++k)out.color[k]=(v.position[k]-lo[side][k])/std::max(.001f,hi[side][k]-lo[side][k]);
                                out.color[1]=1.f-out.color[1];mirrors.push_back(out);nativeMirrors.push_back({});
                            }
                        }
                    }
                    // Only a whole mirror draw bypasses cabin cutting. Other
                    // batched surfaces keep their normal material and geometry.
                    size_t mirrorIndices=0;for(const auto& patch:mirrorPatches)mirrorIndices+=patch.indices.size();
                    rearView=mirrorIndices==p.indices.size();
                    // Neutral translucent body overlays are baked tint/shadow
                    // passes, including depth-writing window overlays. From
                    // inside they float over the reconstructed apertures. Keep
                    // depth-writing lamp callbacks, live mirrors and native
                    // furniture; the original exterior retains every pass.
                    if(seated && !car.nativeInterior && mirrorPatches.empty() && textured && mat &&
                       mat->nativeGt4Type>=0 && mat->alphaBlend && (!mat->nativeDepthWrite || !mat->lampStates) &&
                       ClassifyNeutralOverlay(mesh.images[size_t(image)]).translucent)continue;
                    bool blackInterior=textured;
                    bool blackMaskFrame=textured;
                    if (textured) {
                        const auto& rgba=mesh.images[size_t(image)].rgba;
                        for (size_t i=0;i<rgba.size();i+=4) {
                            if (rgba[i]>32 || rgba[i+1]>32 || rgba[i+2]>32 || rgba[i+3]!=255)blackInterior=false;
                            if (rgba[i+3] && (rgba[i]>32 || rgba[i+1]>32 || rgba[i+2]>32))blackMaskFrame=false;
                        }
                    }
                    // A dark texture does not prove that a surface is wheel
                    // backing: sills, bumpers and closed cabin caps use it too.
                    // Preserve authored wheel arches in both exterior and
                    // seated views. Removing dark triangles also removes bonnet
                    // closures visible from the driving position.
                    // The closed LOD's black inner shell contains the roof,
                    // floor and dashboard caps used to hide an empty cabin
                    // from outside. Our seated hull replaces those caps.
                    // Keep black closure outside the seating volume: removing the
                    // whole material exposes lamp housings through the bonnet.
                    const size_t auditSourceCount=indices.size();
                    if (seated && !car.nativeInterior && !rearView) {
                        const auto& f=importedFit;
                        const float back=std::max({f.rearWindowZ,f.rearCowl[0][2],f.rearCowl[1][2]})+.025f;
                        // Replace the entire seating volume, including low
                        // seat/dashboard fragments crossing the new furniture.
                        // Follow the actual curved cowl. A single box starting
                        // at its foremost corner cuts away the bonnet closure
                        // behind that corner and leaves a slit above the dash.
                        std::vector<ModelClipVolume> volumes;
                        for(size_t strip=1;strip<f.cowl.size();++strip) {
                            const auto& a=f.cowl[strip-1];const auto& b=f.cowl[strip];
                            const float slope=(b[2]-a[2])/std::max(.001f,b[0]-a[0]);
                            ModelClipVolume planes{{1,0,0,b[0]},{-1,0,0,-a[0]},{0,-1,0,-f.floorY},
                                {slope,0,-1,slope*a[0]-a[2]-.025f},{0,0,1,back}};
                            // Preserve structural diffuse geometry, not an
                            // untextured environment carrier whose inner cap
                            // or pane has been removed from this seating cell.
                            if(f.authoredUpperShell && !blackInterior && !alphaBlend && !(mat && mat->reflection)) {
                                const float frontY=std::min(a[1],b[1]);
                                const float rearY=std::min(f.rearCowl[0][1],f.rearCowl[1][1]);
                                const float rise=(rearY-frontY)/std::max(.1f,back-std::min(a[2],b[2]));
                                planes.insert(planes.begin(),{0,1,-rise,frontY-rise*std::min(a[2],b[2])});
                            }
                            for(auto& plane:planes)plane[3]=(plane[3]-plane[0]*offset[0]-plane[1]*offset[1]-plane[2]*offset[2])/scale;
                            volumes.push_back(std::move(planes));
                        }
                        std::vector<GltfVertex> clipped;
                        for(size_t i=0;i+2<indices.size();i+=3){
                            const std::array<GltfVertex,3> t{points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2])};
                            const auto pieces=SubtractModelVolumes(t,volumes);
                            if(std::getenv("GT2_AUDIT_COCKPIT") && !blackInterior && !alphaBlend && !(mat && mat->reflection)) {
                                // Independent preservation witness: all geometry above
                                // the highest belt-line corner must survive furniture
                                // removal. Glazing is tested separately, after this stage.
                                float highest=std::max(f.rearCowl[0][1],f.rearCowl[1][1]);
                                for(const auto& station:f.cowl)highest=std::max(highest,station[1]);
                                const float y=(highest+.02f-offset[1])/scale;
                                const std::array<ModelClipVolume,1> lower{{{{0,1,0,y}}}};
                                auto upperArea=[&](std::span<const GltfVertex> triangles){double sum=0;
                                    for(size_t j=0;j+2<triangles.size();j+=3){const auto upper=SubtractModelVolumes(triangles.subspan(j,3),lower);
                                        for(size_t k=0;k+2<upper.size();k+=3)sum+=triangleArea(upper[k],upper[k+1],upper[k+2]);}
                                    return sum;
                                };
                                state.upperFrameCutArea+=std::max(0.,upperArea(t)-upperArea(pieces))*double(scale)*scale;
                            }
                            clipped.insert(clipped.end(),pieces.begin(),pieces.end());
                        }
                        points=std::move(clipped);indices.resize(points.size());
                        for(size_t i=0;i<indices.size();++i)indices[i]=uint32_t(i);
                    }
                    if(seated && !car.nativeInterior && !rearView && textured && !windowHoles[size_t(image)].empty() && std::getenv("GT2_AUDIT_RETAIN_TINT")) {
                        std::vector<GltfVertex> clear;
                        for(size_t i=0;i+2<indices.size();i+=3){
                            const std::array<GltfVertex,3> triangle{points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2])};
                            const auto pieces=CutModelWindows(triangle,windowHoles[size_t(image)],importedFit,scale,offset);
                            clear.insert(clear.end(),pieces.begin(),pieces.end());
                        }
                        points=std::move(clear);indices.resize(points.size());
                        for(size_t i=0;i<indices.size();++i)indices[i]=uint32_t(i);
                    }
                    if(seated && !rearView && textured && mat && mat->alphaBlend && !mat->nativeDepthWrite &&
                       !neutralEdges[size_t(image)].empty()) {
                        std::vector<GltfVertex> clipped;
                        for(size_t i=0;i+2<indices.size();i+=3){
                            const std::array<GltfVertex,3> triangle{points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2])};
                            const auto plane=GlazingOpening(triangle);
                            std::vector<ModelGlazingOpening> own;
                            if(IsWindowLayerNormal(plane.normal)){
                                const auto inside=GlazingInside(triangle,neutralEdges[size_t(image)],mat->clampU,mat->clampV);
                                for(size_t j=0;j+2<inside.size();j+=3){
                                    std::array<float,3> center{};
                                    for(size_t v=0;v<3;++v)for(size_t k=0;k<3;++k)center[k]+=(inside[j+v].position[k]*scale+offset[k])/3.f;
                                    const size_t side=center[0]<0?0:1;
                                    if(center[1]<CockpitSideSillAt(importedFit,side,center[2])[1]-.10f ||
                                       center[2]<cockpit_detail::CowlZAt(importedFit,center[0])-.25f ||
                                       center[2]>importedFit.rearWindowZ+.10f)continue;
                                    own.push_back(GlazingOpening({inside[j],inside[j+1],inside[j+2]}));
                                }
                            }
                            const auto pieces=CutGlazingLayers(triangle,own);clipped.insert(clipped.end(),pieces.begin(),pieces.end());
                        }
                        points=std::move(clipped);indices.resize(points.size());for(size_t i=0;i<indices.size();++i)indices[i]=uint32_t(i);
                    }
                    const size_t auditVolumeCount=indices.size();
                    if(!mirrorGaps.empty() && (seated || (mat && mat->reflection))) {
                        std::vector<GltfVertex> masked;
                        for(size_t i=0;i+2<indices.size();i+=3){const std::array<GltfVertex,3> t{points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2])};
                            const auto pieces=CutGlazingLayers(t,mirrorGaps);masked.insert(masked.end(),pieces.begin(),pieces.end());}
                        points=std::move(masked);indices.resize(points.size());for(size_t i=0;i<indices.size();++i)indices[i]=uint32_t(i);
                    }
                    if(seated && !rearView) {
                        std::vector<GltfVertex> open;
                        for(size_t i=0;i+2<indices.size();i+=3){
                            const std::array<GltfVertex,3> t{points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2])};
                            // A transparent diffuse aperture also clears its
                            // coplanar tint layer, including horizontal glass
                            // roofs. Keep the opaque frame itself untouched.
                            const bool overlay=mat && (mat->reflection || mat->alphaBlend);
                            const bool masksApertures=mat && (mat->reflection || (car.nativeInterior && mat->alphaBlend));
                            const auto pieces=CutGlazingLayers(t,glazing,overlay?.025f:.003f,masksApertures);open.insert(open.end(),pieces.begin(),pieces.end());
                        }
                        points=std::move(open);indices.resize(points.size());for(size_t i=0;i<indices.size();++i)indices[i]=uint32_t(i);
                        const size_t auditGlazingCount=indices.size();
                        auto lightingCowl=importedFit.cowl;
                        if(car.nativeInterior) {
                            const float front=(nativeWheel.valid?nativeWheel.center[2]-.25f:car.eye[2]-.85f)*scale+offset[2];
                            for(auto& station:lightingCowl)station[2]=std::min(station[2]-.005f,front-.025f);
                        }
                        if(closedNativeLining && !(mat && mat->interiorOnly))
                            for(auto& station:lightingCowl)station[2]=closedNativeFront;
                        std::vector<GltfVertex> split;
                        for(size_t i=0;i+2<indices.size();i+=3) {
                            const std::array<GltfVertex,3> triangle{points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2])};
                            const auto pieces=SplitModelCowl(triangle,lightingCowl,scale,offset);
                            split.insert(split.end(),pieces.begin(),pieces.end());
                        }
                        points=std::move(split);indices.resize(points.size());
                        for(size_t i=0;i<indices.size();++i)indices[i]=uint32_t(i);
                        if(std::getenv("GT2_DEBUG_CABIN"))std::printf("glazing counts material %d: %zu source %zu volume %zu glass %zu cowl\n",p.material,auditSourceCount,auditVolumeCount,auditGlazingCount,indices.size());
                        if(std::getenv("GT2_DEBUG_CABIN"))std::fflush(stdout);
                    }
                    // Interior volume and UV glazing cuts retain the frame. Bounding
                    // boxes/triangle centres cannot distinguish the remaining
                    // cowl closure from discarded interior furniture.
                    if(!seated && &mesh!=&car.reflections)for(size_t i=0;i+2<indices.size();i+=3)
                        loadedArea+=triangleArea(points.at(indices[i]),points.at(indices[i+1]),points.at(indices[i+2]));
                    const uint32_t primitiveFirst=uint32_t(vertices.size());
                    bool interiorSurface=false,cabinSurface=false;
                    for (size_t corner=0;corner<indices.size();++corner) {
                        if (seated && (!car.nativeInterior || (!car.openTop && !(mat && mat->interiorOnly))) && corner%3==0) {
                            const auto& fit=importedFit;
                            cabinSurface=true;
                            for(size_t j=0;j<3;++j) {
                                const auto& q=points.at(indices[corner+j]).position;
                                const float x=q[0]*scale+offset[0],z=q[2]*scale+offset[2];
                                // Rear-quarter frames can extend beyond the fitted
                                // rear glass knee. Their inner side still closes
                                // the occupied shell; culling it leaves only the
                                // translucent environment pass in a side view.
                                const float front=closedNativeLining?closedNativeFront:cockpit_detail::CowlZAt(fit,x);
                                cabinSurface=cabinSurface && z>=front-.025f;
                            }
                            std::array<float,3> center{};
                            for (size_t j=0;j<3;++j) for (size_t k=0;k<3;++k)
                                center[k]+=(points.at(indices[corner+j]).position[k]*scale+offset[k])/3.f;
                            const size_t side=center[0]<0?0:1;
                            const auto sill=CockpitSideSillAt(fit,side,center[2]);
                            interiorSurface=center[2]>fit.frontCowl[side][2] && center[2]<fit.rearCowl[side][2] &&
                                center[1]<sill[1]+.025f && std::abs(center[0])<fit.halfWidth+.05f;
                            // Long wing/door triangles can have their centre in
                            // the cabin while extending over the painted bonnet.
                            for(size_t j=0;j<3 && interiorSurface;++j) {
                                const auto& q=points.at(indices[corner+j]).position;
                                interiorSurface=q[2]*scale+offset[2]>=cockpit_detail::CowlZAt(fit,q[0]*scale+offset[0])+.024f;
                            }
                        }
                        const auto& v=points.at(indices[corner]); SceneVertex out{};
                        // Modulate the imported surface by a soft directional light.
                        const float light=mat && (mat->emissive || mat->nativeGt4Type>=0)?1.f:p.hasNormals?std::clamp(.72f+.22f*v.normal[1]-.12f*v.normal[0],.45f,1.f):1.f;
                        for (size_t k=0;k<3;++k) { out.pos[k]=v.position[k]*scale+(mat && mat->wheelAttachment>=0?0.f:offset[k]); out.color[k]=std::clamp(v.color[k]*(mat?mat->baseColorFactor[k]:1.f)*light,0.f,mat && mat->nativeGt4Type>=0?255.f/128.f:1.f); }
                        if (textured) {
                            const auto& img=mesh.images[size_t(image)];
                            out.texel[0]=v.uv[0]*img.width; out.texel[1]=v.uv[1]*img.height;
                            out.page=images[size_t(image)];out.clut=uint32_t(img.width)|uint32_t(img.height)<<16;
                            out.flags=kTextured|kExternalTexture;
                            if (mat && mat->clampU) out.flags|=kModelClampU;
                            if (mat && mat->clampV) out.flags|=kModelClampV;
                            if (mat && mat->explicitOpaque) out.flags|=kModelOpaque;
                            if (!car.nativeInterior) out.flags|=kModelAlphaNonzero;
                            if (rearView) out.flags|=kModelMirrorMask;
                        }
                        // Retain alpha-tested window borders. Interior trim is
                        // applied in the shader only after testing transparency.
                        // Back-facing bonnet/intake panels are still exterior paint.
                        // Only the occupied cabin receives an inner-side lining.
                        if (seated && (!car.nativeInterior || (!car.openTop && !(mat && mat->interiorOnly))) && cabinSurface) {
                            out.flags |= kCabinLining;
                            if(blackMaskFrame)out.flags|=kCabinSolid;
                        }
                        if(seated && car.nativeInterior) {
                            const auto primitive=size_t(&p-mesh.primitives.data());
                            if(&mesh==&car.cockpit && nativeWheel.valid && nativeWheel.primitives.at(primitive))out.flags|=kNativeSteering;
                            // Only authored furniture behind the cowl receives the
                            // seated exposure lift. Paint, glass and mirrors do not.
                            const size_t first=corner-corner%3;
                            bool furniture=!rearView && !alphaBlend;
                            for(size_t j=0;j<3 && furniture;++j) {
                                const auto& q=points.at(indices[first+j]).position;
                                furniture=q[2]>=car.eye[2]-.851f && q[2]<car.eye[2]+.8f &&
                                    q[1]<car.eye[1]-.10f && std::abs(q[0])<car.exterior.boundsMax[0]*.86f;
                                const float x=q[0]*scale+offset[0],z=q[2]*scale+offset[2];
                                const float front=std::min(cockpit_detail::CowlZAt(importedFit,x)+.02f,
                                    (nativeWheel.valid?nativeWheel.center[2]-.25f:car.eye[2]-.85f)*scale+offset[2]);
                                furniture=furniture && z>=front-.001f;

                            }
                            if(furniture)out.flags|=kNativeCabin;
                        }
                        if (interiorSurface) out.flags |= kCabinSolid;
                        if (seated && std::getenv("GT2_DEBUG_CABIN")) out.flags |= kDebugBackfaces;
                        // Exterior audit: keep authored alpha masks; colour the
                        // inside green and the outside magenta. Black closure
                        // material also becomes green, so it cannot hide a cut.
                        if (!seated && std::getenv("GT2_DEBUG_EXTERIOR")) {
                            out.flags |= kDebugBackfaces;
                            if (blackInterior) out.flags |= kCabinSolid;
                        }
                        NativeModelVertex attribute;
                        if(mat && mat->nativeGt4Type>=0) {
                            out.flags|=kModelNative;attribute.normal=v.normal;
                            const uint32_t material=uint32_t(slot)*SceneRenderer::kNativeMaterialsPerSlot+uint32_t(p.material);
                            attribute.materialAlpha=material | (uint32_t(std::lround(std::clamp(v.color[3]*mat->baseColorFactor[3],0.f,2.f)*32767.5f))<<16);
                            if(mat->nativeCull && !seated && !std::getenv("GT2_AUDIT_RETAIN_NATIVE_BACKFACES"))out.flags|=kCullBack;
                            if(mat->reflection) {
                                out.flags=(out.flags & ~kModelBlend)|kModelReflection;
                                if(mat->wheelAttachment<0 && !mat->lampStates){
                                    if(surfaceReflection) {
                                        (seated?cabinSurfaceReflections:surfaceReflections).push_back(out);
                                        (seated?nativeCabinSurfaceReflections:nativeSurfaceReflections).push_back(attribute);
                                    } else {
                                        (seated?cabinReflections:reflections).push_back(out);
                                        (seated?nativeCabinReflections:nativeReflections).push_back(attribute);
                                    }
                                    continue;
                                }
                            }
                        }
                        if(alphaBlend) {out.flags|=kModelBlend;if(!mat->lampStates) {if(seated)state.cockpitGlass=true;else state.glass=true;}}
                        vertices.push_back(out);
                        nativeVertices.push_back(attribute);
                        if (textured && p.hasNormals && !blackInterior && mat &&
                            mat->nativeGt4Type<0 && !mat->emissive && mat->reflection) {
                            SceneVertex reflection=out;
                            reflection.flags=(reflection.flags & ~kModelBlend)|kModelReflection;
                            for (size_t k=0;k<3;++k) reflection.color[k]=v.normal[k];
                            (seated?cabinReflections:reflections).push_back(reflection);
                            (seated?nativeCabinReflections:nativeReflections).push_back({});
                        }

                    }
                    if(mat && mat->lampStates && mat->wheelAttachment<0 && vertices.size()>primitiveFirst)
                        (seated?state.cabinLamps:state.lamps).push_back({primitiveFirst,uint32_t(vertices.size())-primitiveFirst,mat->lampStates,mat->reflection?kBlendNativeEnvironment:alphaBlend?kBlendAlpha:kBlendOpaque});
                    if(mat && mat->wheelAttachment>=0 && vertices.size()>primitiveFirst)
                        state.attachments.push_back({primitiveFirst,uint32_t(vertices.size())-primitiveFirst,
                            mat->reflection?kBlendNativeEnvironment:alphaBlend?kBlendAlpha:kBlendOpaque,mat->wheelAttachment,mat->wheelSpinning,mat->lampStates});
                }
            };
            append(car.exterior, false);
            // Lowest shell witness in each small X/Z cell. Wheel attachments
            // are appended later and must not raise the body with the tyres.
            std::map<std::pair<int,int>,std::array<float,3>> support;
            const auto collectSupport=[&](size_t first){
                for(size_t i=first;i<vertices.size();++i){const auto& v=vertices[i];
                    if(v.pos[1]>=.35f || (v.flags & kModelBlend))continue;
                    const auto key=std::make_pair(int(std::floor(v.pos[0]/.3f)),int(std::floor(v.pos[2]/.3f)));
                    const std::array<float,3> p{v.pos[0],v.pos[1],v.pos[2]};
                    const auto it=support.find(key);
                    if(it==support.end() || p[1]<it->second[1])support[key]=p;
                }
            };
            collectSupport(0);
            const auto exteriorCount=uint32_t(vertices.size());
            const auto partition=[&](size_t first) {
                std::vector<SceneVertex> sorted;std::vector<NativeModelVertex> attributes;
                size_t opaque=0;
                for(int blend=0;blend<2;++blend)for(size_t i=first;i<vertices.size();++i)if(bool(vertices[i].flags & kModelBlend)==bool(blend)) {
                    sorted.push_back(vertices[i]);attributes.push_back(nativeVertices[i]);if(!blend)++opaque;
                }
                std::copy(sorted.begin(),sorted.end(),vertices.begin()+ptrdiff_t(first));
                std::copy(attributes.begin(),attributes.end(),nativeVertices.begin()+ptrdiff_t(first));return uint32_t(opaque);
            };
            // Partition immutable triangle ranges once. Glass must not cause a
            // second full-body vertex/raster pass at headset resolution.
            state.opaqueCount=partition(0);
            if(std::getenv("GT2_AUDIT_COCKPIT"))for(uint32_t i=0;i+2<exteriorCount;i+=3){
                std::array<std::array<float,3>,3> triangle{};
                for(size_t j=0;j<3;++j)std::copy_n(vertices[i+j].pos,3,triangle[j].begin());
                if(cockpit_detail::TriangleAheadOfCowl(triangle,importedFit.cowl))state.auditBonnetTriangles.push_back(i);
                if(cockpit_detail::TriangleAheadOfCowl(triangle,importedFit.cowl,-.06f))state.auditSurfaceTriangles.push_back(i);
            }
            if(loadCockpit_)append(car.cockpit, true);
            const auto cockpitCount=uint32_t(vertices.size())-exteriorCount;
            state.cockpitOpaqueCount=partition(exteriorCount);
            for(uint32_t i=exteriorCount;i<exteriorCount+cockpitCount;++i) {
                const bool alpha=(vertices[i].flags & kModelBlend)!=0,steering=(vertices[i].flags & kNativeSteering)!=0;
                auto& ranges=state.cabinRanges;
                if(ranges.empty() || ranges.back().alpha!=alpha || ranges.back().steering!=steering)
                    ranges.push_back({i,0,alpha,steering});
                ++ranges.back().count;
            }
            append(car.reflections,false);
            if(loadCockpit_)append(car.reflections,true);
            state.surfaceReflectionCount=uint32_t(surfaceReflections.size());
            state.cockpitSurfaceReflectionCount=uint32_t(cabinSurfaceReflections.size());
            reflections.insert(reflections.end(),surfaceReflections.begin(),surfaceReflections.end());
            nativeReflections.insert(nativeReflections.end(),nativeSurfaceReflections.begin(),nativeSurfaceReflections.end());
            cabinReflections.insert(cabinReflections.end(),cabinSurfaceReflections.begin(),cabinSurfaceReflections.end());
            nativeCabinReflections.insert(nativeCabinReflections.end(),nativeCabinSurfaceReflections.begin(),nativeCabinSurfaceReflections.end());
            vertices.insert(vertices.end(),mirrors.begin(),mirrors.end());
            nativeVertices.insert(nativeVertices.end(),nativeMirrors.begin(),nativeMirrors.end());
            vertices.insert(vertices.end(),reflections.begin(),reflections.end());
            nativeVertices.insert(nativeVertices.end(),nativeReflections.begin(),nativeReflections.end());
            state.cockpitReflectionFirst=uint32_t(vertices.size());state.cockpitReflectionCount=uint32_t(cabinReflections.size());
            vertices.insert(vertices.end(),cabinReflections.begin(),cabinReflections.end());
            nativeVertices.insert(nativeVertices.end(),nativeCabinReflections.begin(),nativeCabinReflections.end());
            const auto lampFirst=vertices.size();
            append(car.lamps,false);
            // GT4 callback branches can contain lower bumper/body fragments,
            // not just light lenses. Include their shell support too.
            collectSupport(lampFirst);
            for(const auto& [key,p]:support){(void)key;state.roadSupport.push_back(p);}
            // Some callback branches also contain glazing and shell fragments.
            // They need the same seating-volume/window cuts as the main body.
            if(loadCockpit_)append(car.lamps,true);
            append(car.attachments,false);
            if (vertices.size()>SceneRenderer::kModelSetVertexStride)
                throw std::runtime_error("car exceeds vertex budget: "+std::to_string(vertices.size()));
            state.totalVertices=uint32_t(vertices.size());
            for (const auto& upload:uploads) renderer_.UploadExternalTexture(upload.base,uint32_t(upload.texels.size()),upload.texels.data());
            renderer_.SetVertices(SceneRenderer::kModelSetVertexBase+uint32_t(slot)*SceneRenderer::kModelSetVertexStride,vertices);
            if(nativeVertices.size()!=vertices.size())throw std::runtime_error("native model attribute alignment");
            renderer_.SetNativeModelVertices(uint32_t(slot)*SceneRenderer::kModelSetVertexStride,nativeVertices);
            if(car.nativeMaterials.size()>SceneRenderer::kNativeMaterialsPerSlot)throw std::runtime_error("native material budget");
            renderer_.SetNativeModelMaterials(uint32_t(slot)*SceneRenderer::kNativeMaterialsPerSlot,NativeMaterials(car.nativeMaterials));
            gt4TexelsUsed_=cursor;gt4Images_=std::move(imageCache);
            state.paintTextures=std::move(paintTextures);state.paints=std::move(car.paints);state.selectedPaint=0;
            state.nativeMaterials=std::move(car.nativeMaterials);state.nativePaintMaterials=std::move(car.nativePaintMaterials);
            state.wheelShift=wheelShift;
            state.wheelYShift=wheelYShift;state.wheelScale=wheelScale;
            state.wheelZShift=wheelZShift;state.wheelWidthScale=wheelWidthScale;
            state.wheelBumpStop=wheelBumpStop;
            state.nativeWheelPose=car.nativeWheelPose;state.wheelBasis=car.nativeWheelBasis;
            if(std::getenv("GT2_AUDIT_WHEEL_COLOR")) {
                state.auditPanelCentres.clear();
                for(size_t i=0;i+2<state.opaqueCount;i+=3) {
                    std::array<float,3> centre{};
                    for(size_t v=0;v<3;++v)for(size_t k=0;k<3;++k)centre[k]+=vertices[i+v].pos[k]/3.f;
                    state.auditPanelCentres.push_back(centre);
                }
            }
            state.reflectionCount=uint32_t(reflections.size());
            state.count=exteriorCount;state.cockpitCount=cockpitCount;state.mirrorCount=uint32_t(mirrors.size());state.nativeInterior=car.nativeInterior;
            state.removedArea=(sourceArea-loadedArea)*double(scale)*scale;
            state.cockpit=importedFit;
            state.cockpit.nativeInterior=car.nativeInterior;
            if (car.nativeInterior) {
                // Keep the fitted GT4 boundaries; only the authored eye and
                // instruments replace their presentation defaults.
                for (size_t k=0;k<3;++k) {
                    state.cockpit.eye[k]=car.eye[k]*scale+offset[k];
                    state.cockpit.mirror[k]=car.mirror[k]*scale+offset[k];
                    state.cockpit.speedometer[k]=car.speedometer[k]*scale+offset[k];
                }
                state.cockpit.speedometer[3]=car.speedometer[3]*scale;
                if(nativeWheel.valid) {
                    for(size_t k=0;k<3;++k)state.cockpit.nativeWheel[k]=nativeWheel.center[k]*scale+offset[k];
                    // A tiller/grip is still replaced by a usable virtual wheel.
                    state.cockpit.nativeWheel[3]=std::clamp((nativeWheel.hi[0]-nativeWheel.lo[0])*.5f,.14f,.26f)*scale;
                    state.cockpit.eye[0]=state.cockpit.nativeWheel[0];
                    const float reach=state.cockpit.eye[2]-state.cockpit.nativeWheel[2];
                    if(reach<.30f*scale || reach>.65f*scale)state.cockpit.eye[2]=state.cockpit.nativeWheel[2]+.45f*scale;
                    state.cockpit.speedometer[0]=state.cockpit.nativeWheel[0];
                }
                state.cockpit.openTop=car.openTop;
            }
            FitMirrorSightlines(state.cockpit,mirrors,!car.nativeInterior);
            std::printf("GT4 model set: %s loaded, %u exterior / %u cockpit vertices, scale %.4f\n",state.sourceId.c_str(),state.count,state.cockpitCount,scale);
        } catch (const std::exception& e) {
            if (state.required) throw std::runtime_error("GT4 expansion " + state.sourceId + ": " + e.what());
            std::printf("GT4 model set: %s -> original GT2 (%s)\n",state.sourceId.c_str(),e.what());
        }
    }
    struct Range { uint32_t first = 0, count = 0; };
    // The polygons of one chunk shape or scenery model: the opaque ones, then the semi-transparent ones by PS1 mode.
    struct ShapeRanges { Range opaque; std::array<Range, 4> semi{}; };

    // Static course vertices: every chunk's road shape (world metres) and every scenery model (model units, drawn
    // per instance with SceneryInstanceMatrix). The chunks' "surround" shapes are the rear-view mirror's low-detail
    // copies (0x8002993C -> 0x80020110 with param_3 != 0 reads chunk + 0x94 instead of the road shape at + 0xA4) and
    // are not built: drawing them over the road was the z-fighting "second asphalt" and its large stretched quads.
    void BuildCourse(const Track& track, std::span<const uint16_t> words) {
        droppedCourseVertices_=0;courseVertexCount_=0;paintCoverage_.Clear();
        const auto seamUvs = CourseTextureSeams(track, words);
        track_ = std::make_unique<Track>(track);
        chunkRanges_.assign(track.chunks.size(), {});
        extendedChunkRanges_.assign(track.chunks.size(), {});
        wallConflicts_.assign(track.chunks.size(),{});
        modelRanges_.assign(track.sceneryModels.size(), {});
        std::vector<SceneVertex> vertices;
        std::array<std::vector<SceneVertex>, 5> lists; // [4] opaque, [m] semi-transparent mode m
        auto flush = [&](ShapeRanges& r) {
            for (size_t l : {size_t(4), size_t(0), size_t(1), size_t(2), size_t(3)}) {
                Range range{uint32_t(vertices.size()), uint32_t(lists[l].size())};
                if (vertices.size() + lists[l].size() > kTrackVertexLimit){droppedCourseVertices_+=lists[l].size();range.count = 0;}
                else vertices.insert(vertices.end(), lists[l].begin(), lists[l].end());
                if (l == 4) r.opaque = range;
                else r.semi[l] = range;
                lists[l].clear();
            }
        };
        // Quads as the PS1 draws them: the drawers emit the ring (v0, v1, v2, v3) as GPU slots (v0, v1, v3, v2) and the
        // GPU splits a quad into slots (0, 1, 2) + (1, 2, 3), i.e. triangles (v0, v1, v3) + (v1, v2, v3) - the v1-v3
        // diagonal. The other diagonal interpolates non-parallelogram UV sets differently (fans of stretched texels
        // on trapezoid ground / kerb polygons).
        static constexpr size_t kQuad[6] = {0, 1, 3, 1, 2, 3}, kTri[3] = {0, 1, 2};
        // Self-check: the emitted triangles of a polygon must cover exactly the polygon's own area (the ring's fan
        // area); a wrong corner (e.g. a triangle reading the quad table -> the shape's vertex 0), a strip / ring mix-up
        // or a mis-decoded index shows up as a mismatch. Printed per course; 0 expected.
        size_t checked = 0, mismatched = 0;
        auto area3 = [](const float* a, const float* b, const float* c) {
            const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
            const float x = u[1] * v[2] - u[2] * v[1], y = u[2] * v[0] - u[0] * v[2], z = u[0] * v[1] - u[1] * v[0];
            return 0.5f * std::sqrt(x * x + y * y + z * z);
        };
        auto check = [&](const std::vector<std::array<float, 3>>& ring, const std::vector<SceneVertex>& out, size_t first) {
            float expected = 0, emitted = 0;
            for (size_t k = 1; k + 1 < ring.size(); k++) expected += area3(ring[0].data(), ring[k].data(), ring[k + 1].data());
            for (size_t t = first; t + 2 < out.size(); t += 3) emitted += area3(out[t].pos, out[t + 1].pos, out[t + 2].pos);
            // A non-planar quad has a different area per diagonal: the v1-v3 split (the GPU's) is accepted too.
            const float other = ring.size() == 4 ? area3(ring[0].data(), ring[1].data(), ring[3].data()) + area3(ring[1].data(), ring[2].data(), ring[3].data()) : expected;
            checked++;
            auto differs = [&](float a) { return std::fabs(emitted - a) > 1e-3f * std::max(1.0f, a); };
            if (differs(expected) && differs(other)) mismatched++;
        };
        const CourseWallVisibility maskedWalls(track);
        std::vector<std::vector<bool>> maskedPolygons(track.chunks.size());
        auto emitShape = [&](size_t ci, const TrackChunk& chunk, const TrackShape& shape,bool extended=false) {
            std::vector<bool> physical(shape.polygons.size(),false);
            for(const auto& cell:chunk.surfaceGrid.cells)for(auto pi:cell)if(pi<physical.size())physical[pi]=true;
            for (size_t pi=0; pi<shape.polygons.size(); ++pi) {
                if(extended && maskedPolygons[ci][pi])continue;
                const TrackPolygon& p=shape.polygons[pi];
                const TrackUvSet* uv = p.IsTextured() ? &(ci<seamUvs.size()?seamUvs[ci][pi]:track.uvTable[p.uvIndex].nearSet) : nullptr;
                const bool semi = (p.primCode & 0x02) != 0;
                // Word0 bits 27-28: the ordering-table offset (16 entries per step, larger = drawn earlier); bit 31:
                // back-face culling (0x8002106C / 0x800234F8).
                uint32_t extra = (uint32_t(3 - (p.renderOrder & 3)) << kNearTierShift) | ((p.renderOrder & 0x10) ? kCullBack : 0u);
                if (semi) extra |= kSemiTransparent;
                std::vector<SceneVertex>& out = lists[semi ? size_t(uv ? (uv->tpage >> 5) & 3 : 0) : 4];
                const size_t first = out.size();
                std::vector<std::array<float, 3>> ring;
                for (size_t c = 0; c < (p.IsQuad() ? 4u : 3u); c++) ring.push_back(TrackVertexToWorld(chunk, shape.vertices[p.vertex[c]]));
                CourseSurfaceMesh mesh;
                if(&shape==&chunk.road)mesh=CoursePolygonMesh(chunk,p,physical[pi]);
                else{mesh.positions.assign(ring.begin(),ring.end());mesh.weights.resize(4);for(size_t k=0;k<4;++k)mesh.weights[k][k]=1;const auto order=CoursePolygonTriangles(p.IsQuad());mesh.indices.assign(order.begin(),order.end());}
                for (size_t i : mesh.indices) {
                    SceneVertex o{};
                    const auto w = mesh.positions[i];
                    std::copy(w.begin(), w.end(), o.pos);
                    for (size_t c = 0; c < 3; c++)for(size_t k=0;k<4;++k) o.color[c] += mesh.weights[i][k]*p.color[k][c]/255.0f;
                    if (uv) {
                        float u=0,v=0;for(size_t k=0;k<4;++k){u+=mesh.weights[i][k]*uv->u[k];v+=mesh.weights[i][k]*uv->v[k];}
                        SetTexture(o, uv->tpage, uv->clut, 0, 0);o.texel[0]=u+.5f;o.texel[1]=v+.5f;
                        if (p.primCode & 0x01) o.flags |= kRawTexture;
                    }
                    o.flags |= extra;
                    // The witness is asphalt (ground.cpp surface 0). Physical
                    // grass/kerbs also contain legitimate guardrail footprints.
                    if(std::getenv("GT2_COURSE_WALL_CHECK") && &shape==&chunk.road && physical[pi] &&
                       (((p.flags>>9)&15)==0 || std::getenv("GT2_COURSE_WALL_SHOULDERS"))) {
                        o.flags=extra;o.color[0]=1;o.color[1]=0;o.color[2]=1;
                    }
                    out.push_back(o);
                }
                if(!extended && &shape==&chunk.road && !semi)for(size_t t=0;t+2<mesh.indices.size();t+=3)
                    paintCoverage_.Add(mesh.positions[mesh.indices[t]],mesh.positions[mesh.indices[t+1]],mesh.positions[mesh.indices[t+2]]);
                if(!mesh.contactFan)check(ring, out, first);
            }
        };
        for (size_t ci = 0; ci < track.chunks.size(); ci++) {
            const auto& chunk=track.chunks[ci];auto& masked=maskedPolygons[ci];masked=maskedWalls.Classify(track,ci,&wallConflicts_[ci]);
            emitShape(ci, track.chunks[ci], track.chunks[ci].road);
            flush(chunkRanges_[ci]);
            extendedChunkRanges_[ci]=chunkRanges_[ci];
            if(std::any_of(masked.begin(),masked.end(),[](bool value){return value;})) {
                emitShape(ci,chunk,chunk.road,true);flush(extendedChunkRanges_[ci]);
            }
        }
        const size_t chunkVertices = vertices.size();
        std::vector<std::vector<Range>> polygonRanges(track.sceneryModels.size());
        for (size_t mi = 0; mi < track.sceneryModels.size(); mi++) {
            const TrackSceneryModel& model = track.sceneryModels[mi];
            for (const TrackSceneryPolygon& p : model.polygons) {
                const bool semi = (p.primCode & 0x02) != 0;
                std::vector<SceneVertex>& out = lists[semi ? size_t(p.IsTextured() ? (p.tpage >> 5) & 3 : 0) : 4];
                const size_t first = out.size();
                std::vector<std::array<float, 3>> ring;
                for (size_t c = 0; c < (p.IsQuad() ? 4u : 3u); c++) {
                    const TrackVertex& v = model.vertices[p.vertex[c]];
                    ring.push_back({float(v.x), float(v.y), float(v.z)});
                }
                for (size_t k = 0; k < (p.IsQuad() ? 6u : 3u); k++) {
                    const size_t i = p.IsQuad() ? kQuad[k] : kTri[k];
                    SceneVertex o{};
                    const TrackVertex& v = model.vertices[p.vertex[i]];
                    o.pos[0] = v.x;
                    o.pos[1] = v.y;
                    o.pos[2] = v.z;
                    for (size_t c = 0; c < 3; c++) o.color[c] = p.color[i][c] / 255.0f;
                    if (p.IsTextured()) {
                        SetTexture(o, p.tpage, p.clut, p.u[i], p.v[i]);
                        if (p.primCode & 0x01) o.flags |= kRawTexture;
                    }
                    if (p.cullBackface) o.flags |= kCullBack;
                    if (semi) o.flags |= kSemiTransparent;
                    out.push_back(o);
                }
                check(ring, out, first);
                polygonRanges[mi].push_back({uint32_t(first),uint32_t(out.size()-first)});
            }
            flush(modelRanges_[mi]);
            for(size_t pi=0;pi<model.polygons.size();++pi)
                if(!(model.polygons[pi].primCode&2)) polygonRanges[mi][pi].first+=modelRanges_[mi].opaque.first;
        }
        const SceneryDetailIndex detailIndex(track);
        const SceneryRoadCut roadCut(track);
        instanceReplacements_.clear();instanceReplacements_.resize(track.sceneryInstances.size());
        size_t replacedPolygons=0;
        for(size_t ii=0;ii<track.sceneryInstances.size();++ii) {
            const auto& inst=track.sceneryInstances[ii];
            const auto& lods=track.sceneryLods[inst.lodList];
            const int lod=HighestSceneryLod(track,lods);
            if(lod<0) continue;
            auto& replacement=instanceReplacements_[ii];replacement.model=lods[lod].model;
            const auto& model=track.sceneryModels[replacement.model];
            const auto matrix=SceneryInstanceMatrix(inst,model);
            for(const auto& p:model.polygons)if(!(p.primCode&2)) {
                std::array<std::array<float,3>,4> corners{};
                for(size_t c=0;c<(p.IsQuad()?4u:3u);++c) {
                    const auto& v=model.vertices[p.vertex[c]];
                    for(size_t k=0;k<3;++k)corners[c][k]=matrix[k]*v.x+matrix[4+k]*v.y+matrix[8+k]*v.z+matrix[12+k];
                }
                paintCoverage_.Add(corners[0],corners[1],corners[p.IsQuad()?3:2]);
                if(p.IsQuad())paintCoverage_.Add(corners[1],corners[2],corners[3]);
            }
            std::vector<std::array<float,3>> joined;
            const auto matches=detailIndex.Match(inst,model,&joined);
            replacement.source=modelRanges_[replacement.model];
            bool needsJoin=false;
            for(size_t vi=0;vi<joined.size();++vi) {
                const auto& v=model.vertices[vi];
                needsJoin|=std::abs(joined[vi][0]-v.x)>1e-4f || std::abs(joined[vi][1]-v.y)>1e-4f || std::abs(joined[vi][2]-v.z)>1e-4f;
            }
            auto ranges=polygonRanges[replacement.model];
            // Keep the boundary of surviving scenery on the detailed course's
            // vertices. Otherwise removing a coarse face leaves quantisation-sized
            // cracks along its neighbours, especially visible against the sky.
            if(needsJoin) {
                const auto original=replacement.source;
                size_t count=original.opaque.count;
                for(const auto& range:original.semi) count+=range.count;
                if(vertices.size()+count<=kTrackVertexLimit) {
                    auto copyRange=[&](Range& range) {
                        const std::vector<SceneVertex> copy(vertices.begin()+range.first,vertices.begin()+range.first+range.count);
                        range.first=uint32_t(vertices.size());vertices.insert(vertices.end(),copy.begin(),copy.end());
                    };
                    copyRange(replacement.source.opaque);
                    for(auto& range:replacement.source.semi) copyRange(range);
                    for(size_t pi=0;pi<model.polygons.size();++pi) {
                        const auto& p=model.polygons[pi];
                        const bool semi=(p.primCode&2)!=0;
                        const size_t blend=p.IsTextured()?((p.tpage>>5)&3):0;
                        ranges[pi].first=semi?replacement.source.semi[blend].first+ranges[pi].first:
                            replacement.source.opaque.first+ranges[pi].first-original.opaque.first;
                        for(size_t k=0;k<ranges[pi].count;++k) {
                            const auto& position=joined[p.vertex[p.IsQuad()?kQuad[k]:kTri[k]]];
                            std::copy(position.begin(),position.end(),vertices[ranges[pi].first+k].pos);
                        }
                    }
                }
            }
            for(size_t pi=0;pi<matches.size();++pi) if(!matches[pi].empty()) {
                const auto range=ranges[pi];
                if(range.first+range.count<=replacement.source.opaque.first+replacement.source.opaque.count)
                    replacement.polygons.push_back({range,matches[pi]});
            }
            for(size_t pi=0;pi<model.polygons.size();++pi)if(matches[pi].empty()){
                const auto cut=roadCut.Cut(inst,model,pi);if(cut.chunks.empty())continue;
                const auto range=ranges[pi];const auto& p=model.polygons[pi];
                if(range.first+range.count>replacement.source.opaque.first+replacement.source.opaque.count)continue;
                if(vertices.size()+cut.vertices.size()>kTrackVertexLimit)throw std::runtime_error("course road clipping exceeds vertex budget");
                std::array<SceneVertex,4> corners{};
                for(size_t k=0;k<range.count;++k)corners[p.IsQuad()?kQuad[k]:kTri[k]]=vertices[range.first+k];
                const Range clipped{uint32_t(vertices.size()),uint32_t(cut.vertices.size())};
                for(const auto& v:cut.vertices){SceneVertex o=corners[0];std::fill(std::begin(o.pos),std::end(o.pos),0.f);std::fill(std::begin(o.color),std::end(o.color),0.f);o.texel[0]=o.texel[1]=0;
                    for(size_t i=0;i<4;++i){const float w=float(v.weights[i]);for(size_t k=0;k<3;++k){o.pos[k]+=w*corners[i].pos[k];o.color[k]+=w*corners[i].color[k];}for(size_t k=0;k<2;++k)o.texel[k]+=w*corners[i].texel[k];}vertices.push_back(o);
                }
                replacement.polygons.push_back({range,cut.chunks,clipped});
            }
            std::sort(replacement.polygons.begin(),replacement.polygons.end(),[](const auto& a,const auto& b){return a.range.first<b.range.first;});
            replacedPolygons+=replacement.polygons.size();
            if(!replacement.polygons.empty()) {
                const auto& source=replacement.source;
                std::vector<SceneVertex> filtered;
                uint32_t cursor=source.opaque.first;
                for(const auto& polygon:replacement.polygons) {
                    filtered.insert(filtered.end(),vertices.begin()+cursor,vertices.begin()+polygon.range.first);
                    filtered.insert(filtered.end(),vertices.begin()+polygon.clipped.first,vertices.begin()+polygon.clipped.first+polygon.clipped.count);
                    cursor=polygon.range.first+polygon.range.count;
                    replacement.chunks.insert(replacement.chunks.end(),polygon.chunks.begin(),polygon.chunks.end());
                }
                filtered.insert(filtered.end(),vertices.begin()+cursor,vertices.begin()+source.opaque.first+source.opaque.count);
                if(vertices.size()+filtered.size()<=kTrackVertexLimit) {
                    replacement.hasDetailed=true;replacement.detailed=source;
                    replacement.detailed.opaque={uint32_t(vertices.size()),uint32_t(filtered.size())};
                    vertices.insert(vertices.end(),filtered.begin(),filtered.end());
                }
                std::sort(replacement.chunks.begin(),replacement.chunks.end());
                replacement.chunks.erase(std::unique(replacement.chunks.begin(),replacement.chunks.end()),replacement.chunks.end());
            }
        }
        std::printf("native scene: %zu coarse scenery polygons have detailed course replacements\n",replacedPolygons);
        // The rear-view mirror's low-detail copies of the chunks (chunk + 0x94; 0x80020110 with param_3 != 0).
        mirrorRanges_.assign(track.chunks.size(), {});
        for (size_t ci = 0; ci < track.chunks.size(); ci++) {
            emitShape(seamUvs.size(), track.chunks[ci], track.chunks[ci].surround);
            flush(mirrorRanges_[ci]);
        }
        renderer_.SetVertices(0, vertices);
        courseVertexCount_=vertices.size();
        std::printf("native scene: course polygon check: %zu polygons, %zu whose triangles do not cover the polygon's area\n", checked, mismatched);
        std::printf("native scene: course, %zu chunk + %zu scenery-model vertices (%zu chunks, %zu models, %zu instances)\n", chunkVertices,
                    vertices.size() - chunkVertices, track.chunks.size(), track.sceneryModels.size(), track.sceneryInstances.size());
    }

    int NearestChunk(const std::array<float, 3>& eye) const {
        int best = 0;
        double bestD = 1e30;
        for (size_t i = 0; i < track_->chunks.size(); i++) {
            const auto& c = track_->chunks[i].centre;
            const double dx = c[0] / 65536.0 - eye[0], dy = c[1] / 65536.0 - eye[1], dz = c[2] / 65536.0 - eye[2];
            const double d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) { bestD = d; best = int(i); }
        }
        return best;
    }

    static void MultiplyColumnMajor(const float* a, const float* b, float* out) {
        float r[16];
        for (int c = 0; c < 4; c++)
            for (int row = 0; row < 4; row++) {
                float s = 0;
                for (int k = 0; k < 4; k++) s += a[k * 4 + row] * b[c * 4 + k];
                r[c * 4 + row] = s;
            }
        std::copy(r, r + 16, out);
    }

    struct Slot {
        std::string id; uint32_t vertexCount; uint32_t shadowCount; uint32_t reflectionCount; CarTexture texture; CarModel model;
        std::vector<float> reflect; // an external mesh's reflection-pass corners (UseExternalCar): x, y, z, nx, ny, nz
        uint32_t strides = 1;       // vertex ranges the slot occupies (an external mesh may take the following slots' ranges)
        // Disc cars: the wheels are the last four vertex ranges of the body, built around the origin (car_mesh.h
        // CarMeshOptions::wheelsAtOrigin); each is drawn with its own model matrix (WheelModelMatrix, rest = at its centre).
        bool separateWheels = false;
        std::array<std::array<uint32_t, 2>, 4> wheelRanges{};
        std::array<std::array<float, 3>, 4> wheelCentres{};
        std::array<float,4> wheelBumpStop{1e9f,1e9f,1e9f,1e9f};
        float shadowHeight = 0;
        std::array<float,4> nativeModelRotation{0,0,0,1};
        RaceWheelGeometry raceWheels{};
        bool hasRaceWheels=false;
        std::vector<std::array<float,3>> roadSupport;
        std::vector<SceneVertex> roadShadow;
        std::vector<std::array<float,3>> shadowSamples;
        std::vector<size_t> shadowSampleIndex;
        std::array<float,16> shadowPose{};
        bool shadowProjected=false;
        CockpitFit cockpit;
        uint32_t cockpitBodyCount = 0;
        uint32_t cockpitMirrorCount = 0;
        uint32_t cockpitReflectionCount = 0;
        std::vector<size_t> cockpitExterior;
    };

    // The reflection pass of an external mesh (UseExternalCar with reflective triangles): the original's rule of
    // UpdateCarReflection with the mesh's unit normals scaled to the .cdo's 512 (uv = 64 + (R512 . n512) >> 12).
    void UpdateExternalReflection(int slot, const float* world, const std::array<std::array<float, 3>, 3>& cameraAxes, uint16_t tpage, uint16_t clut, uint8_t colour, uint32_t rowBase) {
        Slot& s = slots_[size_t(slot)];
        s.reflectionCount = 0;
        if (s.reflect.empty()) return;
        int32_t r512[3][3];
        for (int i = 0; i < 3; i++)
            for (int j = 0; j < 3; j++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += cameraAxes[size_t(i)][size_t(k)] * world[j * 4 + k];
                r512[i][j] = int32_t(std::lround(v * 4096.0f)) / 8;
            }
        std::vector<SceneVertex> vertices;
        for (size_t at = 0; at + 6 <= s.reflect.size(); at += 6) {
            const float* c = &s.reflect[at];
            const int32_t n[3] = {int32_t(std::lround(c[3] * 512.0f)), int32_t(std::lround(c[4] * 512.0f)), int32_t(std::lround(c[5] * 512.0f))};
            const int32_t u = 64 + ((r512[0][0] * n[0] + r512[0][1] * n[1] + r512[0][2] * n[2]) >> 12);
            const int32_t v = 64 + ((r512[1][0] * n[0] + r512[1][1] * n[1] + r512[1][2] * n[2]) >> 12);
            SceneVertex o{};
            std::copy(c, c + 3, o.pos);
            for (float& ch : o.color) ch = colour / 255.0f;
            SetTexture(o, tpage, clut, uint8_t(u & 0xFF), uint8_t(v & 0xFF));
            o.page += rowBase << 16; o.clut += rowBase << 16;
            vertices.push_back(o);
        }
        const uint32_t base = kCarVertexBase + uint32_t(slot) * kCarVertexStride + s.vertexCount + s.shadowCount;
        const uint32_t end = kCarVertexBase + (uint32_t(slot) + s.strides) * kCarVertexStride;
        if (base >= end) return;
        if (base + vertices.size() > end) vertices.resize(end - base);
        renderer_.SetVertices(base, vertices);
        s.reflectionCount = uint32_t(vertices.size());
    }

    // Runtime VRAM place of the reflection map (page 9 = x 576, y 0; CLUT x 368, y 511), as in the captured
    // second-pass primitives (tpage 0x0029 without the blend bits, clut 0x7FD7).
    static constexpr uint16_t kEnvMapTpage = 9, kEnvMapClut = uint16_t((368 / 16) | (511 << 6));
    static constexpr int kEnvMapImageX = 576, kEnvMapImageY = 0, kEnvMapClutX = 368, kEnvMapClutY = 511; // 0x800274D4

    // crstim.arc: "@(#)GT-ARC", u16 version, u16 count at 0x0E, then {u32 offset, u32 size, u32 size} per entry.
    // Entries 3..5 are the three reflection maps (4-bit TIM with CLUT); the blocks are copied to the destinations
    // the original writes into the TIM header, into `words` (1024 x 512).
    void LoadEnvironmentMap(std::vector<uint16_t>& words, uint32_t entry) const {
        LoadArcTim(words, entry, kEnvMapImageX, kEnvMapImageY, kEnvMapClutX, kEnvMapClutY);
    }

    // The tyre smoke sprites (particles.h kSmokeTextures): crstim.arc entries 2 and 1 at the places 0x8002EB08
    // writes into their TIM headers at the race load (0x800274D4).
    void LoadSmokeTextures(std::vector<uint16_t>& words) const {
        for (const SmokeTexturePlacement& t : kSmokeTextures) LoadArcTim(words, t.arcEntry, t.imageX, t.imageY, t.clutX, t.clutY);
        LoadArcTim(words, kGlowTexture.arcEntry, kGlowTexture.imageX, kGlowTexture.imageY, kGlowTexture.clutX, kGlowTexture.clutY); // glow.h
    }

    // One 4-bit TIM (with CLUT) of crstim.arc copied into `words` at the given image / CLUT places.
    void LoadArcTim(std::vector<uint16_t>& words, uint32_t entry, int imageX, int imageY, int clutX, int clutY) const {
        const std::vector<uint8_t> arc = vol_.Read("crstim.arc");
        auto u16At = [&](size_t o) { if (o + 2 > arc.size()) throw std::runtime_error("crstim.arc: short"); return uint16_t(arc[o] | (arc[o + 1] << 8)); };
        auto u32At = [&](size_t o) { return uint32_t(u16At(o)) | (uint32_t(u16At(o + 2)) << 16); };
        if (arc.size() < 0x10 || std::memcmp(arc.data(), "@(#)GT-ARC", 10) != 0) throw std::runtime_error("crstim.arc: bad magic");
        const uint32_t count = u16At(0x0E);
        if (entry >= count) throw std::runtime_error("crstim.arc: no such entry");
        size_t offset = u32At(0x10 + size_t(entry) * 12);
        if (u32At(offset) != 0x10) throw std::runtime_error("crstim.arc: entry is not a TIM");
        const uint32_t flags = u32At(offset + 4);
        offset += 8;
        for (int block = 0; block < ((flags & 8) ? 2 : 1); block++) {
            const uint32_t length = u32At(offset);
            const bool clut = (flags & 8) && block == 0;
            const int x = clut ? clutX : imageX, y = clut ? clutY : imageY;
            const int w = u16At(offset + 8), h = u16At(offset + 10);
            if (length < 12 || size_t(w) * h * 2 > length - 12 || x + w > PsxVram::kWidth || y + h > PsxVram::kHeight) throw std::runtime_error("crstim.arc: TIM block does not fit");
            for (int row = 0; row < h; row++)
                for (int col = 0; col < w; col++) words[size_t(y + row) * PsxVram::kWidth + size_t(x + col)] = u16At(offset + 12 + (size_t(row) * w + col) * 2);
            offset += length;
        }
    }

    static void SetTexture(SceneVertex& o, uint16_t tpage, uint16_t clut, uint8_t u, uint8_t v) {
        o.texel[0] = u + 0.5f;
        o.texel[1] = v + 0.5f;
        o.page = uint32_t((tpage & 0xF) * 64) | (uint32_t(((tpage >> 4) & 1) * 256) << 16);
        o.clut = uint32_t((clut & 0x3F) * 16) | (uint32_t(clut >> 6) << 16);
        o.flags = kTextured | (uint32_t((tpage >> 7) & 3) << 8);
    }

    void BuildBackdrop(const Backdrop& b) {
        skyColor_ = {b.skyColor[0] / 255.0f, b.skyColor[1] / 255.0f, b.skyColor[2] / 255.0f};
        groundColor_ = {b.groundColor[0] / 255.0f, b.groundColor[1] / 255.0f, b.groundColor[2] / 255.0f};
        // One vertex list per blend mode: opaque first (with the ground disc), then modes 0..3 in list order.
        std::array<std::vector<SceneVertex>, 5> lists;
        {
            // The original fills the screen below the horizon with the ground colour: a large disc well below
            // the camera projects to exactly that (the dome's lowest ring is at -459 units).
            const float R = kGroundDiscRadius, y = -kGroundDiscDepth;
            constexpr int kSides = 16;
            for (int i = 0; i < kSides; i++) {
                const float a0 = 6.2831853f * float(i) / kSides, a1 = 6.2831853f * float(i + 1) / kSides;
                const float pts[3][3] = {{0, y, 0}, {R * std::cos(a0), y, R * std::sin(a0)}, {R * std::cos(a1), y, R * std::sin(a1)}};
                for (const auto& pt : pts) {
                    SceneVertex o{};
                    std::copy(pt, pt + 3, o.pos);
                    for (int k = 0; k < 3; k++) o.color[k] = b.groundColor[size_t(k)] / 255.0f;
                    lists[4].push_back(o);
                }
            }
        }
        for (const BackdropPolygon& p : b.polygons) {
            const uint32_t mode = p.IsSemiTransparent() ? uint32_t((p.tpage >> 5) & 3) : 4u;
            // p.vertex is in ring order, but the colours and UVs come from the record's ready-made GPU packet, whose
            // corners are (v0, v1, v3, v2) of the ring: ring corner 2 takes packet slot 3 and vice versa (checked on
            // the captured licence test: 64 of 66 backdrop primitives of the frame are file polygons with exactly
            // these corner colours / UVs; reading slot i for ring corner i swapped the dome's gradient and the
            // mountain panels' texture halves).
            static constexpr size_t kPacketSlot[4] = {0, 1, 3, 2};
            auto corner = [&](size_t i) {
                SceneVertex o{};
                const BackdropVertex& v = b.vertices[p.vertex[i]];
                o.pos[0] = v.x;
                o.pos[1] = v.y;
                o.pos[2] = v.z;
                const size_t slot = p.IsQuad() ? kPacketSlot[i] : i;
                for (int k = 0; k < 3; k++) o.color[k] = p.color[slot][size_t(k)] / 255.0f;
                if (p.IsTextured()) SetTexture(o, p.tpage, p.clut, p.u[slot], p.v[slot]);
                return o;
            };
            static constexpr size_t kQuad[6] = {0, 1, 3, 1, 2, 3}, kTri[3] = {0, 1, 2}; // the PS1's split, as in BuildCourse
            for (size_t k = 0; k < (p.IsQuad() ? 6u : 3u); k++) lists[mode].push_back(corner(p.IsQuad() ? kQuad[k] : kTri[k]));
        }
        std::vector<SceneVertex> all;
        uint32_t first = kBackdropVertexBase;
        for (uint32_t mode : {4u, 0u, 1u, 2u, 3u}) {
            if (lists[mode].empty()) continue;
            backdropRanges_.push_back({first, uint32_t(lists[mode].size()), mode == 4 ? kBlendOpaque : mode});
            all.insert(all.end(), lists[mode].begin(), lists[mode].end());
            first += uint32_t(lists[mode].size());
        }
        if (all.size() > kBackdropVertexLimit) throw std::runtime_error("backdrop: too many vertices");
        renderer_.SetVertices(kBackdropVertexBase, all);
    }

    SceneRenderer& renderer_;
    const GtfsVolume& vol_;
    std::vector<uint16_t> ownRows_;
    bool loadCockpit_ = true;
    std::vector<Slot> slots_;
    ModelSet gt4Set_{gt4ModelSetRoot};
    mutable std::array<Gt4Car, kCarSlots> gt4Cars_{};
    std::map<std::string,uint32_t> gt4Images_;
    uint32_t gt4TexelsUsed_ = 0;
    bool gt4Enabled_ = false;
    uint32_t carSlotLimit_ = kCarSlots; // ReserveSecondView: kCarSlotsSplit
    std::unordered_map<uint32_t,std::vector<SceneVertex>> billboardUploads_;
    uint32_t billboardBase_ = kBillboardVertexBase, billboardLimit_ = kBillboardVertexLimit, smokeBase_ = kSmokeVertexBase; // SelectView
    std::array<bool, 2> smokeOpaque_{true, true};
    std::string trackName_;
    std::unique_ptr<Track> track_;
    std::vector<ShapeRanges> chunkRanges_, extendedChunkRanges_, modelRanges_, mirrorRanges_;
    std::vector<std::vector<size_t>> wallConflicts_;
    struct PolygonReplacement { Range range; std::vector<uint16_t> chunks; Range clipped{}; };
    struct InstanceReplacement {
        size_t model=size_t(-1);
        ShapeRanges source;
        std::vector<PolygonReplacement> polygons;
        ShapeRanges detailed;
        std::vector<uint16_t> chunks;
        bool hasDetailed=false;
    };
    std::vector<InstanceReplacement> instanceReplacements_;
    TrackStats stats_;
    size_t courseVertexCount_=0,droppedCourseVertices_=0;
    std::vector<BackdropRange> backdropRanges_;
    std::array<float, 3> skyColor_{0.45f, 0.58f, 0.78f};
    std::array<float, 3> groundColor_{0.2f, 0.45f, 0.69f};
    int overlayWidth_ = 0, overlayHeight_ = 0;
    float overlayAspect_ = 0;
    uint32_t externalTexelsUsed_ = 0; // allocation cursor of the renderer's external texture store
};

} // namespace gt2view

