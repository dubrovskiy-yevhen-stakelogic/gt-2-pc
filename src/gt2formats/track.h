#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace gt2 {

// GT2 course geometry (.tro, magic "@(#)GT-PS"). Container skeleton follows Nenkai's MIT 010
// template; vertex blocks, polygon bit layout, strides and the world placement rule were
// derived from the US v1.2 corpus (see docs/formats/track_tro.md).
//
// World space: 16.16 fixed-point metres, +Y up. Chunk-local vertices are 1/64 m relative to the
// origin of the 64 m grid cell that contains the chunk centre; local axes are (x, z, height).

struct TrackVertex {
    int16_t x, y, z; // local: x -> world X, y -> world Z, z -> world Y (height)
};

struct TrackPolygon {
    std::array<uint16_t, 4> vertex{}; // 9-bit indices into the owning shape's vertex block
    uint8_t renderOrder = 0;          // 5 bits
    uint16_t uvIndex = 0;             // word1 bits 9-22 (14 bits): index into Track::uvTable (textured polygons)
    uint32_t flags = 0;               // word1 >> 19 (overlaps the top of uvIndex): bits 23-31 = attributes / surface (ground.cpp)
    std::array<std::array<uint8_t, 3>, 4> color{}; // per corner; flat polygons repeat colour 0
    uint8_t primCode = 0;             // libgpu code: 0x20 F3 ... 0x3C GT4

    bool IsQuad() const { return (primCode & 0x08) != 0; }
    bool IsTextured() const { return (primCode & 0x04) != 0; }
    bool IsGouraud() const { return (primCode & 0x10) != 0; }
};

// One entry of the course-wide UV table: PS1 texture words for a near and a far variant.
struct TrackUvSet {
    std::array<uint8_t, 4> u{}, v{};
    uint16_t clut = 0;  // VRAM CLUT address: x = (clut & 0x3F) * 16, y = clut >> 6
    uint16_t tpage = 0; // VRAM page: x = (tpage & 0xF) * 64, y = ((tpage >> 4) & 1) * 256, bits 7-8 colour depth
};

struct TrackUvEntry {
    TrackUvSet nearSet, farSet;
    uint32_t extra = 0; // unknown (0x3F0, 0x7C0 ... seen)
};

// A glow ("light") record (20 bytes; chunk shapes: shape + 0x28, count u16 at shape + 0x42; scenery models: model + 0x28 /
// + 0x42): a point drawn as four additive star quads by the drawers 0x800205EC / 0x80020A00 (chunks) and 0x8001FBA8
// (models), US v1.2 race overlay; see gt2view/glow.h and docs/formats/track_tro.md "Glow records".
struct TrackGlow {
    std::array<int16_t, 3> position{}; // +0 x, +2 y, +4 z: the owning shape's vertex space (RTPS input)
    int16_t size = 0;                  // +6 loaded as the GTE's DQA (star radius)
    std::array<uint8_t, 8> unknown{};  // +8..+15 not read by the drawers
    uint32_t colour = 0;               // +16 0x00BBGGRR
};

struct TrackShape {
    std::vector<TrackVertex> vertices;
    std::vector<TrackPolygon> polygons;
    std::vector<TrackGlow> glows;      // shape + 0x28, count u16 at shape + 0x42
};

// A chunk's view-test box (0x80020EC4): four footprint corners {s32 x, s32 z} and two heights, 16.16 m world.
struct TrackChunkBounds {
    std::array<int32_t, 4> x{}, z{};
    int32_t yLow = 0, yHigh = 0;
};

// Wall segment of a chunk (data at chunk + 0x98): an edge between two road vertices with a normal.
// Established from the original collision code (0x80027FC4): the two indices address the chunk's road
// vertex block, the normal is stored (n0, n1) and used as (n1, -n0).
struct TrackBoundary {
    uint16_t vertexA = 0, vertexB = 0;
    int16_t normal0 = 0, normal1 = 0;
};

// Road-surface lookup grid of a chunk (data at chunk + 0x9C): a 4 x 4 grid of cells over the chunk-local plane,
// each cell listing the road polygons that may cover it. In the file the lists hold offsets of the polygons
// inside the road shape's lists; here they are indices into TrackChunk::road.polygons. Established from the
// original's road lookup (0x80028830 / 0x80027C1C) and verified on the US v1.2 corpus by tools/gt2verify.
struct TrackSurfaceGrid {
    int16_t originX = 0, originZ = 0;   // chunk-local 1/64 m (same frame as the road vertices)
    int16_t shiftX = 0, shiftZ = 0;     // cell size = 1 << shift
    std::array<std::vector<uint16_t>, 16> cells; // row-major: cell = x + z * 4
};

// A camera-facing textured quad ("billboard": trees, poles). Chunk shapes (shape + 0x24, count at shape + 0x40,
// 16-byte records) and scenery models (model + 0x24, count at model + 0x40, 28-byte records) carry them; the
// original draws them before the shape's polygons (chunks: 0x80020110, models: 0x8001F7F8) as one POLY_FT4 each:
// the bottom edge at `position`, the top edge `height` above it, the horizontal half-extent width / 2 along the
// camera's yaw (GTE light matrix = (cos a, sin a) / 2 of the camera yaw, MVMVA on (width, 0, 0)). GPU corners:
// 0 = top-left, 1 = top-right, 2 = bottom-left, 3 = bottom-right with UV i on corner i.
//   chunk record: s16 x, s16 y, s16 z (height), u16 uvIndex (course UV table, near set), s16 width, s16 height,
//                 u32 code word (r, g, b, primitive code) - chunk-local 1/64 m like the shape's vertices
//   model record: s16 x, s16 y (up), u32 z (low 16 bits used), s16 width, s16 height, u32 code word, u32 T0 (u0 v0 clut),
//                 u32 T1 (u1 v1 tpage), u32 T2 (u2 v2 u3 v3) - model units
struct TrackBillboard {
    std::array<int32_t, 3> position{}; // chunk: (x, y = world Z, z = height) local 1/64 m; model: (x, y up, z) model units
    int16_t width = 0, height = 0;
    std::array<uint8_t, 3> color{};
    uint8_t primCode = 0x2C;
    std::array<uint8_t, 4> u{}, v{};
    uint16_t clut = 0, tpage = 0;
    uint16_t uvIndex = 0; // chunk records: the course UV table entry the texture words come from (0 for model records)
};

struct TrackChunk {
    uint16_t prev = 0, next = 0;
    std::vector<TrackBoundary> boundaries;
    int32_t distance = 0;               // chunk + 0x10  distance along the course, 16.16 m (course-distance base of 0x80028588)
    uint16_t weightNext = 0;            // chunk + 0x14  weight of the next chunk's plane in the distance interpolation
    uint16_t weightThis = 0;            // chunk + 0x16  weight of this chunk's plane
    int16_t vcoord = 0;                 // metres along the course at this chunk
    std::array<int32_t, 3> origin{};    // 16.16 m, start of the chunk on the course line
    std::array<int16_t, 3> direction{}; // 4096 = 1.0
    std::array<int32_t, 3> centre{};    // 16.16 m
    int32_t extent = 0;                 // chunk + 0x2C: added (squared) to the camera distance of the GTE precision choice (0x80026BB4)
    std::array<TrackChunkBounds, 2> bounds{}; // chunk + 0x44 (main view) / + 0x6C (rear-view mirror): the view-test boxes
    std::array<int32_t, 2> cellOrigin{}; // 16.16 m world X/Z origin of the local vertices
    TrackShape road;                    // drivable surface ("ChunkShape"): the chunk's full-detail geometry
    TrackShape surround;                // chunk + 0x94: the low-detail copy of the chunk drawn only by the rear-view mirror
                                        // (0x8002993C -> 0x80020110 with param_3 != 0); never part of the main view
    std::vector<TrackBillboard> billboards; // of the road shape (shape + 0x24 / + 0x40)
    uint16_t lightCount = 0;            // road shape + 0x42: 20-byte glow records (road.glows; none on seattle)
    TrackSurfaceGrid surfaceGrid;       // chunk + 0x9C
    uint32_t sceneryMask = 0;           // chunk + 0x0C: bit i = draw scenery instance list i while this chunk is in view (0x80020110)
    std::vector<uint16_t> renderList;   // chunk + 0xA0: chunk indices (bits 0-13) with flags (bits 14-15) drawn from this chunk (0x80020110)
};

// ---- Scenery (header 0x14 / 0x18 / 0x118) ----
// Established from the original's scenery pass (0x8002002C -> 0x8001F7F8 per instance list, 0x80019B58 per model)
// and verified against the captured GTE traffic of the Seattle attract race (tools/gt2play --prims); see
// docs/formats/track_tro.md "Scenery".

// Polygon of a scenery model: 10-bit vertex indices, inline PS1 texture words.
struct TrackSceneryPolygon {
    std::array<uint16_t, 4> vertex{};  // word0 bits 0-9 / 10-19 / 20-29, word1 bits 0-9 (quads)
    bool sortNearest = false;          // word0 bit 30: ordering-table depth from the nearest corner (else the farthest) plus a bias
    bool cullBackface = false;         // word0 bit 31: drawn only when the screen winding is front-facing
    std::array<std::array<uint8_t, 3>, 4> color{}; // per corner; flat polygons repeat colour 0
    uint8_t primCode = 0;              // libgpu code: 0x20 F3 ... 0x3C GT4
    std::array<uint8_t, 4> u{}, v{};   // corner i uses (u[i], v[i]); words: (u0 v0 clut), (u1 v1 tpage), (u2 v2 u3 v3)
    uint16_t clut = 0, tpage = 0;

    bool IsQuad() const { return (primCode & 0x08) != 0; }
    bool IsTextured() const { return (primCode & 0x04) != 0; }
    bool IsGouraud() const { return (primCode & 0x10) != 0; }
};

// A scenery model (header 0x18 table). Vertices are model-local (x, y up, z); the instance's z scale is negated
// by the original, so the model's +z points to world -Z for an unrotated instance.
struct TrackSceneryModel {
    std::vector<TrackVertex> vertices;  // s16 x, y, z; here TrackVertex's fields are literally x, y, z of the model
    std::vector<TrackSceneryPolygon> polygons;
    std::array<int16_t, 3> boundsMin{}, boundsMax{}; // model + 0x44 / 0x4C: the 8 corners are transformed first for the view test
    int16_t scaleExponent = 0;          // model + 0x54: metres per unit = 2^(scaleExponent - 16) / 4096 (23 -> 1/32 m, 17 -> 1/2048 m)
    uint16_t billboardCount = 0;        // model + 0x40: 28-byte records at model + 0x24 (see TrackBillboard)
    std::vector<TrackBillboard> billboards;
    uint16_t lightCount = 0;            // model + 0x42: 20-byte records at model + 0x28 (four fixed-texture glow quads per point)
    std::vector<TrackGlow> glows;       // those records (TrackGlow)
};

// One level of detail of a scenery object (header 0x14 table): the model is used while the squared view
// distance measure (0x8007AE38) is below `threshold`; entries are sorted by threshold, the first is the nearest.
struct TrackLodEntry {
    uint32_t threshold = 0;
    uint16_t model = 0;                 // index into Track::sceneryModels
};

// One placed scenery object (header 0x118: 33 lists of 28-byte records; list 32 is always drawn, lists 0-31 when
// the bit is set in the sceneryMask of a chunk in view). World = position + Ry(-angle[1]) Rx(-angle[0]) Rz(angle[2])
// diag(scale[0], scale[1], -scale[2]) / 4096 * vertex * metresPerUnit, angles in 1/4096 turn (0x80081374 order).
struct TrackSceneryInstance {
    std::array<int16_t, 3> angle{};     // +0, +2, +4
    uint16_t lodList = 0;               // +6: index into Track::sceneryLods
    std::array<int16_t, 3> scale{};     // +8, +10, +12: 4096 = 1.0
    int16_t lodDivisor = 4096;          // +14: divides the view-distance factor of the LOD choice
    std::array<int32_t, 3> position{};  // +16: 16.16 m world (x, y up, z)
    uint8_t list = 0;                   // which of the 33 lists the record came from
};

struct Track {
    int32_t courseLength = 0;                            // chunk table + 0: length of the course line, 16.16 m
    int16_t lengthMetres = 0;
    int16_t startAngle = 0;                              // 4096 = 360 degrees (3072 on testline, where the road heads -X)
    std::array<std::array<int32_t, 3>, 16> startGrid{};  // 16.16 m; slot 0 = pole, unused slots are zero
    std::vector<TrackChunk> chunks;
    std::vector<TrackUvEntry> uvTable;
    std::vector<TrackSceneryModel> sceneryModels;
    std::vector<std::vector<TrackLodEntry>> sceneryLods;
    std::vector<TrackSceneryInstance> sceneryInstances;
};

Track ParseTrack(std::span<const uint8_t> data);

// Local vertex -> world metres (x, y up, z).
std::array<float, 3> TrackVertexToWorld(const TrackChunk& chunk, const TrackVertex& v);

// Metres per model unit of a scenery model.
double SceneryMetresPerUnit(const TrackSceneryModel& model);

// Column-major 4x4 matrix taking a scenery model's vertices (model units) to world metres (x, y up, z) for the
// given instance, reproducing the original's matrix build (0x8001F7F8: rotation 0x80081374, scale 0x8007B25C
// with the z scale negated, translation = record position).
std::array<float, 16> SceneryInstanceMatrix(const TrackSceneryInstance& instance, const TrackSceneryModel& model);

// Level-of-detail choice of a scenery instance (0x8001F7F8), established from the original's own values
// (gt2play --prims traces every call: "# lod-measure ... measure M choice C" and "# scenery-lod" checks):
//   t = the instance position in camera space, 16.16 m: 0x8007B008 composes the camera matrix of the view
//       (view + 8: the camera rotation with the y row scaled by the screen aspect 3723 / 4096, and its translation)
//       with the instance matrix, t = R_cam * position + T_cam (0x8008220C, exact 32-bit arithmetic);
//   k = (view + 0x78 << 12) / lodDivisor, view + 0x78 = ((max(320, 240) >> 1) << 12) / h (0x8007B374, h = the GTE
//       projection distance of the view - the replay cameras zoom, h 160 .. ~1000, so far scenery gets finer);
//   measure = 0x8007AE38: s = sum((t >> 14)^2) through the GTE (IR registers = 16-bit, SQR, 32-bit sum), then
//       ((s * k) >> 12) * k >> 12 in 64 bits, low word unsigned;
//   entry = 0x8007AEF4: the first whose threshold exceeds the measure, -1 = beyond the last (not drawn).
int32_t SceneryLodK(int32_t projectionDistance, int16_t lodDivisor);
uint32_t SceneryLodMeasure(const std::array<int32_t, 3>& cameraSpace16_16, int32_t k);
// Camera-space translation (16.16 m) of an instance for a camera at `eye` (metres, x, y up, z) with the PS1 camera
// rows `axes` (right, down, forward; unit vectors in world space): the rows are applied like the original's camera
// matrix, the down row carrying the aspect factor 3723 / 4096.
std::array<int32_t, 3> SceneryCameraSpace(const TrackSceneryInstance& instance, const std::array<double, 3>& eye,
                                          const std::array<std::array<double, 3>, 3>& axes);
// The threshold the game compares with: at course load 0x8007ADC8 replaces every file word w of a LOD list by
// (w >> 14)^2 (the same quarter-metre units as the measure; e.g. seattle 0x0096C0C6 -> 603^2 = 363609).
uint32_t SceneryLodThreshold(uint32_t fileWord);
// Index into the instance's LOD list (file words, converted by SceneryLodThreshold), -1 = beyond the last
// threshold (not drawn).
int SceneryLodIndex(const std::vector<TrackLodEntry>& lods, uint32_t measure);

} // namespace gt2
