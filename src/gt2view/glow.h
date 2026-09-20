#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/track.h"
#include "gt2view/vk_scene_renderer.h"

namespace gt2view {

// Glow records of the course chunks ("lights" of the night / highway courses), ported from US Simulation v1.2
// (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), race overlay (GT2.OVL member 0) and EXE; our
// disassembly of work/re/race_demo/ram.bin (docs/formats/track_tro.md "Glow records"):
//   0x80020110  the chunk pass of one view (main view: a2 = 0; rear-view mirror: a2 = 1): copies view + 0x08..+0x6F to
//               the scratchpad, swaps the rotation's columns 1 / 2 (0x80020E38; chunk vertices are (x, z, height)),
//               loads the GTE (0x80020E84 -> 0x8007B778 with shift 0, TR = 0), then for every entry of the camera chunk's
//               render list: the distance key 0x80020FD8 (mirror: skipped beyond 0x63FFFF), the scenery mask, the view
//               test 0x80020EC4 (-> 0x8007B640: the 8 box corners against the frustum planes in the light / colour
//               matrices and the screen), an insertion sort by key (ascending, stable); then per sorted chunk the GTE
//               setup 0x80026BB4 (precision shift 0..2 from the squared distance, rotation << shift, translation of the
//               64 m cell through MVMVA) and the glow drawer: 0x80020A00 (render-list flags > 2 in a player's race:
//               glows only) or 0x800205EC (before the chunk's polygons).
//   Glow drawer: per record RTPS (DQA = size, DQB = 0), skipped on FLAG bit 31; r = (MAC0 << shift) >> 15; ordering
//               table entry min(SZ3 >> (shift + 3), 4095), the packets go 8 entries nearer (base - 32 bytes); four
//               POLY_GT4 (code 0x3E, tpage 0x29, CLUT 0x7F57) around the projected centre with s = sin 22.5, c = cos
//               22.5 of the sine table (1567 / 3784); colours c / 8, c / 64, c / 2, c / 8.
// Verified against the original's GP0 output (tools/gt2play --prims, "# glow-ours" lines).

// The scratchpad block 0x1F800000.. as 0x80020110 prepares it from view + 0x08 (= camera::RaceCamera::gteState): the
// camera rotation (columns 1 / 2 already swapped), translation, the camera offset (view + 0x28: the negated camera
// position, 16.16 m), the light / colour matrices holding the frustum planes, the screen offset, H and the clip limits.
struct GlowView {
    int16_t rotation[3][3] = {};  // scratch + 0x00 (after 0x80020E38)
    int32_t translation[3] = {};  // scratch + 0x14
    int32_t offset[3] = {};       // scratch + 0x20
    int16_t llm[3][3] = {};       // scratch + 0x2C
    int16_t lcm[3][3] = {};       // scratch + 0x40
    int32_t ofx = 0, ofy = 0;     // scratch + 0x54 / + 0x58 (16.16)
    uint16_t h = 0;               // scratch + 0x5C
    uint16_t clipX = 0, clipY = 0, nearZ = 0; // scratch + 0x5E / + 0x60 / + 0x62 (0x8007B640)
    // From the 0x68 bytes at view + 0x08 (guest RAM or camera::RaceCamera::gteState).
    static GlowView FromViewBytes(const uint8_t* viewPlus8);
};

// One POLY_GT4 of a glow star as the GPU receives it.
struct GlowPrimitive {
    uint32_t rgb[4] = {};     // 0x00BBGGRR per vertex
    int16_t x[4] = {}, y[4] = {};
    uint8_t u[4] = {}, v[4] = {};
    uint16_t clut = 0x7F57, tpage = 0x0029;
    int32_t otEntry = 0;      // ordering-table entry relative to the view's base (SZ3 >> (shift + 3), at most 4095, minus 8)
    uint16_t chunk = 0;       // the chunk it belongs to (diagnostics)
    std::string ToString() const; // the format of gt2play --prims ("POLY 3E quad tex gouraud semi ...")
};

struct GlowFrame {
    std::vector<uint16_t> chunks;          // the chunks in the order the pass draws them (render-list entries: index | flags << 14)
    std::vector<GlowPrimitive> submitted;  // in the order the drawers add them to the ordering table
    std::vector<GlowPrimitive> primitives; // in the order the GPU draws them (ordering table far to near, later packets first)
};

// The GPU's order of packets added to one ordering table in `submitted` order: far entries first, within one entry the
// packet added last first (addPrim prepends).
std::vector<GlowPrimitive> SortGlowPrimitives(const std::vector<GlowPrimitive>& submitted);

// The scenery models' glow drawer 0x8001FBA8 (inline in the instance pass 0x8001F7F8, after the billboards, before the
// polygons 0x80019B58 / 0x8001C17C): the same four packets per record with the model's GTE state (0x8007B8A0 -> 0x8007B778:
// rotation scratch + 0x84, translation + 0x78, the view's OFX / OFY / H) and its precision shift s = scratch + 0x98:
// r = MAC0 >> (s - 10) >> 15 (MAC0 << (10 - s) when s < 10), ordering-table entry min((SZ3 << s) >> 13, 4095), 8 nearer.
struct GlowGte {
    int16_t rotation[3][3] = {};
    int32_t translation[3] = {};
    int32_t ofx = 0, ofy = 0;
    uint16_t h = 0;
};
std::vector<GlowPrimitive> DrawModelGlows(const GlowGte& gte, uint16_t shift, const std::vector<gt2::TrackGlow>& glows);

// The glow primitives of one chunk pass (0x80020110) of `track` for the view: `cameraChunk` = view + 0xA0, `replay` =
// 0x800A951C != 0 (every entry draws its full chunk; irrelevant to the glows, which every drawn entry draws), `mirror` =
// the rear-view mirror pass (surround shapes, chunks with key <= 0x63FFFF, bounds + 0x6C).
GlowFrame BuildChunkGlows(const gt2::Track& track, const GlowView& view, int cameraChunk, bool mirror);

// Native: the glow stars of the chunks of `renderList` (all entries draw their glows) as camera-facing geometry in world
// metres: the PS1 star (four quads around the point, half extents r c / 2 and r s / 2 on the screen with r = size * H / SZ
// * 2) becomes quads in the camera plane with r = size / 32 m; `right` / `down` = the camera's screen axes in world axes.
// Semi-transparent mode 1 (additive), texture crstim.arc record 0 at (608, 64) with CLUT (368, 509).
void AppendChunkGlowSprites(std::vector<SceneVertex>& out, const gt2::Track& track, const gt2::TrackChunk& chunk, const float right[3], const float down[3]);

// The same for a scenery model's records at an instance: `matrix` = column-major model units -> world metres
// (track.h SceneryInstanceMatrix); the radius is size / 32 m as for the chunks (the model drawer's shift rules cancel out).
void AppendModelGlowSprites(std::vector<SceneVertex>& out, const gt2::TrackSceneryModel& model, const float* matrix, const float right[3], const float down[3]);

// crstim.arc record 0 goes to image (608, 64), CLUT (368, 509) (0x8002EB08; particles.md).
struct GlowTexturePlacement { uint32_t arcEntry; int imageX, imageY, clutX, clutY; };
inline constexpr GlowTexturePlacement kGlowTexture = {0, 608, 64, 368, 509};

} // namespace gt2view
