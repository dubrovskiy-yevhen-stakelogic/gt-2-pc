#pragma once
#include "gt2view/billboard.h"
#include "gt2view/cockpit_eye_fit.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "gt2export/car_mesh.h"
#include "gt2formats/backdrop.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/course_data.h"
#include "gt2formats/gltf_reader.h"
#include "gt2formats/psx_vram.h"
#include "gt2formats/sponsor_boards.h"
#include "gt2formats/track.h"
#include "gt2view/glow.h"
#include "gt2view/scenery_visibility.h"
#include "gt2view/scenery_replacements.h"
#include "gt2view/course_texture_seams.h"
#include "gt2view/particles.h"
#include "gt2view/vk_scene_renderer.h"
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

    SceneAssets(VkSceneRenderer& renderer, const GtfsVolume& vol) : renderer_(renderer), vol_(vol), ownRows_(size_t(512) * 1024, 0) {}

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
        try {
            LoadSmokeTextures(words);
        } catch (const std::exception& e) {
            std::printf("native scene: no tyre smoke sprites (%s)\n", e.what());
        }
        renderer_.UploadVram(0, PsxVram::kHeight, words.data());

        BuildCourse(track, words);
    }

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
            addRanges(chunkRanges_[index], vp, distanceTo(centre));
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
        if (!all.empty()) renderer_.SetVertices(billboardBase_, all);
        ShapeRanges boardShape;
        boardShape.opaque = boardRanges[4];
        for (size_t mode = 0; mode < 4; mode++) boardShape.semi[mode] = boardRanges[mode];
        addRanges(boardShape, vp, 0.0f);
        // Semi-transparent parts back to front (the original's ordering table).
        std::stable_sort(blendedItems.begin(), blendedItems.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& b : blendedItems) blended.push_back(b.second);
    }
    const TrackStats& LastTrackStats() const { return stats_; }

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
    int UseCar(const std::string& id, const std::string& alias = std::string(), const WheelArea* wheels = nullptr) {
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
        slots_.back().shadowHeight = CarShadowHeight(slots_.back().model);
        slots_.back().cockpit = FitCockpit(slots_.back().model);
        if (slots_.back().cockpit.valid) {
            const auto body = BuildCockpitBody(slots_.back().model, slots_.back().texture, slots_.back().cockpit);
            FitCockpitSideSills(slots_.back().cockpit,body.windowOpenings);
            RefineCockpitEye(slots_.back().cockpit, body.vertices, slots_.back().texture, body.glassMasks);
            FitCockpitMirror(slots_.back().cockpit, slots_.back().model);
            slots_.back().cockpitExterior = body.exteriorPolygons;
            if (body.vertices.size() > VkSceneRenderer::kCockpitBodyVertexStride)
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
            renderer_.SetVertices(VkSceneRenderer::kCockpitBodyVertexBase + slot * VkSceneRenderer::kCockpitBodyVertexStride, bodyVertices);
        }
        renderer_.UploadVram(kOwnRowsBase, 512, ownRows_.data());
        if (wheelRanges[3][0] + wheelRanges[3][1] <= bodyCount && wheelRanges[0][1] != 0) {
            slots_.back().wheelRanges = wheelRanges;
            slots_.back().wheelCentres = wheelCentres;
            slots_.back().separateWheels = true;
        }
        std::printf("native scene: car %s -> slot %u (%u body + %u shadow vertices)\n", key.c_str(), slot, bodyCount, uint32_t(vertices.size()) - bodyCount);
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
            if (uint64_t(externalTexelsUsed_) + count > VkSceneRenderer::kMovieTexelBase)
                throw std::runtime_error("external textures: the store is full (" + std::to_string(VkSceneRenderer::kMovieTexelBase) + " texels)");
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
                vertices.push_back(o);
            }
        }
        if (cockpit) {
            if (s.cockpitBodyCount + vertices.size() > VkSceneRenderer::kCockpitBodyVertexStride)
                throw std::runtime_error("scene: cockpit body and reflection exceed reserved vertex range");
            renderer_.SetVertices(VkSceneRenderer::kCockpitBodyVertexBase + uint32_t(slot)*VkSceneRenderer::kCockpitBodyVertexStride +
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
    void AppendCarItems(std::vector<DrawItem>& items, int slot, const float* mvp, uint32_t paint, uint32_t brakeLit, bool body = true,
                        const std::array<std::array<float, 16>, 4>* wheelModels = nullptr, const float* shadowMvp = nullptr) const {
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
            item.paint = paint;
            item.brakeLit = brakeLit;
            std::copy(mvp, mvp + 16, item.mvp);
            items.push_back(item);
            if (s.separateWheels)
                for (size_t w = 0; w < 4; w++) {
                    float model[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, s.wheelCentres[w][0], s.wheelCentres[w][1], s.wheelCentres[w][2], 1};
                    if (wheelModels) std::copy((*wheelModels)[w].begin(), (*wheelModels)[w].end(), model);
                    DrawItem wheel = item;
                    wheel.firstVertex = base + s.wheelRanges[w][0];
                    wheel.vertexCount = s.wheelRanges[w][1];
                    for (int c = 0; c < 4; c++) // column-major mvp * model
                        for (int r = 0; r < 4; r++)
                            wheel.mvp[c * 4 + r] = mvp[r] * model[c * 4] + mvp[4 + r] * model[c * 4 + 1] + mvp[8 + r] * model[c * 4 + 2] + mvp[12 + r] * model[c * 4 + 3];
                    items.push_back(wheel);
                }
        }
        if (s.reflectionCount) {
            DrawItem reflection;
            reflection.firstVertex = base + s.vertexCount + s.shadowCount;
            reflection.vertexCount = s.reflectionCount;
            reflection.blend = 1; // B + F, as the original's tpage word (0x29 = page 9 | mode 1)
            std::copy(mvp, mvp + 16, reflection.mvp);
            items.push_back(reflection);
        }
    }

    // The original body with transparent glazing and an interior roof lining.
    // It has a separate immutable range: reflections and other cars sharing this slot cannot overwrite it.
    void AppendCockpitBodyItems(std::vector<DrawItem>& items, int slot, const float* mvp, uint32_t paint) const {
        if (slot < 0 || size_t(slot) >= slots_.size()) return;
        const Slot& s = slots_[size_t(slot)];
        if (!s.cockpitBodyCount) return;
        DrawItem item;
        item.firstVertex = VkSceneRenderer::kCockpitBodyVertexBase + uint32_t(slot) * VkSceneRenderer::kCockpitBodyVertexStride;
        item.vertexCount = s.cockpitBodyCount;
        item.paint = paint;
        std::copy(mvp, mvp + 16, item.mvp);
        items.push_back(item);
        if (s.cockpitReflectionCount) {
            item.firstVertex += s.cockpitBodyCount;
            item.vertexCount = s.cockpitReflectionCount;
            item.paint = 0;
            item.blend = 1;
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
        opaque.push_back(item);
        item.stpPass = 2;
        item.blend = 1;
        blended.push_back(item);
    }

    // The parsed .cdo of a slot (empty for an external mesh): its wheel entries place the smoke (particles.h).
    const CarModel& SlotModel(int slot) const { return slots_[size_t(slot)].model; }
    CockpitFit SlotCockpitFit(int slot) const { return slot >= 0 && size_t(slot) < slots_.size() ? slots_[size_t(slot)].cockpit : CockpitFit{}; }
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
    struct Range { uint32_t first = 0, count = 0; };
    // The polygons of one chunk shape or scenery model: the opaque ones, then the semi-transparent ones by PS1 mode.
    struct ShapeRanges { Range opaque; std::array<Range, 4> semi{}; };

    // Static course vertices: every chunk's road shape (world metres) and every scenery model (model units, drawn
    // per instance with SceneryInstanceMatrix). The chunks' "surround" shapes are the rear-view mirror's low-detail
    // copies (0x8002993C -> 0x80020110 with param_3 != 0 reads chunk + 0x94 instead of the road shape at + 0xA4) and
    // are not built: drawing them over the road was the z-fighting "second asphalt" and its large stretched quads.
    void BuildCourse(const Track& track, std::span<const uint16_t> words) {
        const auto seamUvs = CourseTextureSeams(track, words);
        track_ = std::make_unique<Track>(track);
        chunkRanges_.assign(track.chunks.size(), {});
        modelRanges_.assign(track.sceneryModels.size(), {});
        std::vector<SceneVertex> vertices;
        std::array<std::vector<SceneVertex>, 5> lists; // [4] opaque, [m] semi-transparent mode m
        auto flush = [&](ShapeRanges& r) {
            for (size_t l : {size_t(4), size_t(0), size_t(1), size_t(2), size_t(3)}) {
                Range range{uint32_t(vertices.size()), uint32_t(lists[l].size())};
                if (vertices.size() + lists[l].size() > kTrackVertexLimit) range.count = 0;
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
        auto emitShape = [&](size_t ci, const TrackChunk& chunk, const TrackShape& shape) {
            for (size_t pi=0; pi<shape.polygons.size(); ++pi) {
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
                for (size_t k = 0; k < (p.IsQuad() ? 6u : 3u); k++) {
                    const size_t i = p.IsQuad() ? kQuad[k] : kTri[k];
                    SceneVertex o{};
                    const auto w = TrackVertexToWorld(chunk, shape.vertices[p.vertex[i]]);
                    std::copy(w.begin(), w.end(), o.pos);
                    for (size_t c = 0; c < 3; c++) o.color[c] = p.color[i][c] / 255.0f;
                    if (uv) {
                        SetTexture(o, uv->tpage, uv->clut, uv->u[i], uv->v[i]);
                        if (p.primCode & 0x01) o.flags |= kRawTexture;
                    }
                    o.flags |= extra;
                    out.push_back(o);
                }
                check(ring, out, first);
            }
        };
        for (size_t ci = 0; ci < track.chunks.size(); ci++) {
            emitShape(ci, track.chunks[ci], track.chunks[ci].road);
            flush(chunkRanges_[ci]);
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
        instanceReplacements_.clear();instanceReplacements_.resize(track.sceneryInstances.size());
        size_t replacedPolygons=0;
        for(size_t ii=0;ii<track.sceneryInstances.size();++ii) {
            const auto& inst=track.sceneryInstances[ii];
            const auto& lods=track.sceneryLods[inst.lodList];
            const int lod=HighestSceneryLod(track,lods);
            if(lod<0) continue;
            auto& replacement=instanceReplacements_[ii];replacement.model=lods[lod].model;
            const auto& model=track.sceneryModels[replacement.model];
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
            std::sort(replacement.polygons.begin(),replacement.polygons.end(),[](const auto& a,const auto& b){return a.range.first<b.range.first;});
            replacedPolygons+=replacement.polygons.size();
            if(!replacement.polygons.empty()) {
                const auto& source=replacement.source;
                std::vector<SceneVertex> filtered;
                uint32_t cursor=source.opaque.first;
                for(const auto& polygon:replacement.polygons) {
                    filtered.insert(filtered.end(),vertices.begin()+cursor,vertices.begin()+polygon.range.first);
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
        float shadowHeight = 0;
        CockpitFit cockpit;
        uint32_t cockpitBodyCount = 0;
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

    VkSceneRenderer& renderer_;
    const GtfsVolume& vol_;
    std::vector<uint16_t> ownRows_;
    std::vector<Slot> slots_;
    uint32_t carSlotLimit_ = kCarSlots; // ReserveSecondView: kCarSlotsSplit
    uint32_t billboardBase_ = kBillboardVertexBase, billboardLimit_ = kBillboardVertexLimit, smokeBase_ = kSmokeVertexBase; // SelectView
    std::string trackName_;
    std::unique_ptr<Track> track_;
    std::vector<ShapeRanges> chunkRanges_, modelRanges_, mirrorRanges_;
    struct PolygonReplacement { Range range; std::vector<uint16_t> chunks; };
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
    std::vector<BackdropRange> backdropRanges_;
    std::array<float, 3> skyColor_{0.45f, 0.58f, 0.78f};
    std::array<float, 3> groundColor_{0.2f, 0.45f, 0.69f};
    int overlayWidth_ = 0, overlayHeight_ = 0;
    float overlayAspect_ = 0;
    uint32_t externalTexelsUsed_ = 0; // allocation cursor of the renderer's external texture store
};

} // namespace gt2view
