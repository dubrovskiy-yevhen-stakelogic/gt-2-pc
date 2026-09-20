#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include "gt2formats/car_model.h"
#include "gt2formats/overlay_data.h"

namespace gt2 {

// One corner of a triangle list built from a car LOD, positions in metres
// (+Y up, -Z front, -X left - the file's own axes).
struct CarMeshVertex {
    float pos[3];
    float texel[2];  // texel-centre coordinates in the 256x224 texture
    float color[3];  // polygon colour / 255
    uint8_t palette; // CLUT 0..15
    bool textured;
    bool rawTexture; // texture is not modulated by `color`
    bool rim = false; // the rim of a generated wheel (its CLUT is the wheel's own: VRAM (slot * 64, 224), 0x80061308 / 0x800678E8)
    bool cullBack = false; // drawn only when front-facing (the wheel strips' two coloured faces, WheelQuadGrey)
};

// ---------------------------------------------------------------- wheels
// The .cdo carries no wheel geometry: the game writes it into the model's runtime area (file + 0x40..0x867) at car
// setup. US Simulation v1.2 (EXE SHA-1 3030aa27...), from our disassembly of the race overlay and the executable,
// checked against the areas of the six cars of the attract race dump (work/re/race_demo) and licence B-1:
//   0x80017FA0 (race car setup): per axle {R = body + 0x3E4 (wheelRadius), W = body + 0x3EC, ratio = 4096 -
//     (body + 0x3F0 (tyre height) << 12) / R, dish = s16 table 0x80091A70 [(car config word +0x00 >> 8) & 3]
//     (0 / 81 / 163 / 245)}; 0x80061504 (the defaults it overwrites): {.cdo 0x18 radius, 0x1A width, 3072, 122}.
//   0x80061308: area + 4 / + 6 = W / 2 - dish (the rim's axle offset, front / rear); + 8.. / + 28.. the rim
//     square per axle, corners (-r, r), (r, r), (r, -r), (-r, -r) with r = (R * ratio) >> 12; then per axle and
//     LOD (+48 / +568 LOD 0, +1088 / +1368 LOD 1, +1648 / +1868 LOD 2) the executable's template (0x80091670 /
//     0x80091878 / 0x80091990) scaled by 0x800611F8: groups {u32 count, count x 6 s16 T}, each record = two
//     vertices {xy (T0 R, T1 R) >> 12, z T2 W >> 12} and {z T3 W >> 12, xy (T4 k, T5 k) >> 12}, k = r in the first
//     group and R after it. LOD 0: 17 (outer +W/2, inner +W/2) pairs (the sidewall), 17 (outer +W/2, outer -W/2)
//     pairs (the tread), 8 (upper, lower) pairs at -W/2 (the inboard disc); a 16-gon whose APOTHEM is R (vertices
//     at +-11.25 + 22.5 k degrees). LOD 1 / 2 are the same with 8 / 6 sides.
//   0x800670F0 draws it: the strips as flat POLY_F4 (0x80066EF8), the rim as two POLY_FT4 (colour 0x808080) at
//     z = W / 2 - dish: (corner (r, r) uv (47, 0), (-r, r) (0, 0), centre (23, 23), (-r, -r) (0, 47)) and
//     ((r, r) (47, 0), (r, -r) (47, 47), centre, (-r, -r) (0, 47)), texture = the car texture, CLUT 0.
struct WheelAxleDims {
    int16_t radius = 0, width = 0; // R (the 16-gon's apothem) and W, 1/4096 m
    int16_t ratio = 3072;           // r / R in 1/4096
    int16_t dish = 122;             // the rim's depth from the outboard face
};
// 0x80061504: the defaults from the .cdo's 0x18 block (front, rear).
std::array<WheelAxleDims, 2> DefaultWheelDims(const CarModel& model);
// 0x80017FA0: one axle of a race car from its body (CarBody wheelRadius / 0x3EC / 0x3F0) and the dish of its config.
WheelAxleDims RaceWheelDims(int16_t wheelRadius, int16_t width, int16_t tyreHeight, int16_t dish);
// 0x80091A70: the dish depths, index (car config word +0x00 >> 8) & 3.
constexpr std::array<int16_t, 4> kWheelDishDepths = {0, 81, 163, 245}; // default when the executable is not at hand
int16_t WheelDishDepth(uint32_t configWord00, const std::array<int16_t, 4>& table = kWheelDishDepths);

// The wheel templates of the executable (count-prefixed s16 streams as stored: u32 count as two s16, records of 6).
struct WheelTemplates {
    std::array<std::vector<int16_t>, 3> lods;
};
WheelTemplates LoadWheelTemplates(const GuestImage& exe);   // 0x80091670 / 0x80091878 / 0x80091990 (and the dish table)
std::array<int16_t, 4> LoadWheelDishDepths(const GuestImage& exe);
// The templates rebuilt by formula (same structure, values within one unit) for tools without the executable.
WheelTemplates GeneratedWheelTemplates();

// The generated area, exactly as 0x80061308 leaves it (s16 words), per axle.
struct WheelArea {
    std::array<int16_t, 2> rimZ{};                           // area + 4 / + 6
    std::array<std::array<int16_t, 8>, 2> square{};          // area + 8 / + 28: 4 corners (x, y)
    std::array<std::array<std::vector<int16_t>, 3>, 2> lods; // [axle][lod]: the scaled template streams
};
WheelArea GenerateWheelArea(const std::array<WheelAxleDims, 2>& dims, const WheelTemplates& templates);
// Where 0x80061308 puts the streams, from the area (model + 0x40): [axle][lod].
constexpr uint32_t kWheelAreaStream[2][3] = {{48, 1088, 1648}, {568, 1368, 1868}};
constexpr uint32_t kWheelAreaRimZ = 4, kWheelAreaSquare[2] = {8, 28};

struct CarMeshOptions {
    size_t lod = 0;
    bool wheels = true;          // add the wheel meshes the game generates at runtime (see above)
    bool quadStripOrder = false; // experiments only; files use ring order
    const WheelArea* wheelArea = nullptr; // nullptr: DefaultWheelDims + GeneratedWheelTemplates
    // true: the wheel strips as two one-sided copies with the original's per-face colours (renderers with back-face culling);
    // false: one copy per quad (sidewall and tread black, inboard disc grey) for double-sided exports (glTF).
    bool wheelStripFaces = true;
    // true: each wheel is built around the origin (its orientation at steer 0 as the original's wheel matrix has it:
    // Ry(-1024), the right wheels mirrored), to be drawn with its own transform (WheelSpin); `wheelCentres` gets the rest
    // centres (car metres) and `wheelRanges` the vertex range [first, first + count) of each wheel in the output.
    bool wheelsAtOrigin = false;
    std::array<std::array<float, 3>, 4>* wheelCentres = nullptr;
    std::array<std::array<uint32_t, 2>, 4>* wheelRanges = nullptr;
};

std::vector<CarMeshVertex> BuildCarMesh(const CarModel& model, const CarMeshOptions& options);

// ---------------------------------------------------------------- the wheel transform of 0x800670F0
// 0x800140A4 fills per wheel w (0 FL, 1 FR, 2 RL, 3 RR) the record the wheel drawer 0x800670F0 turns into the wheel matrix
// T(x, y, z) * 0x80081374(yaw, pitch, roll) * mirror-z (odd wheels): x, y, z = car + 0x7C4 + w * 0x10 (0x800133F0: -/+ half
// track, ride reference - travel, the .cdo wheel entry's x), yaw = wheel + 0x0C (steer angle) - 1024 (+ 1024 + 2048 on odd
// wheels), pitch = -camber (body + 0x368 + axle * 2) on even wheels, +camber on odd, roll = wheel + 0x20 (the rolling angle
// of the drivetrain). 0x80081374: m = Ry(yaw) Rx(-pitch) Rz(roll), 4096 units per turn. A wheel mesh built at the origin
// (CarMeshOptions::wheelsAtOrigin) already holds Ry(-1024) * mirror; this returns the column-major car-space model matrix
// T(centre) * Ry(yaw) Rx(-pitch) Rz(roll) * Ry(1024) that turns it into the original's wheel (centre in car metres).
void WheelModelMatrix(int wheel, const float centre[3], int16_t steerAngle, int16_t camber, uint16_t rotation, float out[16]);

// ---------------------------------------------------------------- the strips' colour rule (0x80066EF8)
// 0x800670F0 draws each strip group of the stream (u32 count, count pair records) as count - 1 flat POLY_F4 (code 0x28) of
// consecutive pairs: GPU (A_k, B_k, A_k+1, B_k+1) with A = (T0, T1, T2), B = (T4, T5, T3) of a record. The colour of a quad is
// picked on the screen by 0x80066EF8 from the NCLIP of its two triangles n1 = NCLIP(A_k, B_k, A_k+1), n2 = NCLIP(B_k, A_k+1,
// B_k+1): grey (the word .cdo + 0x08 + carLod * 4 = scratch + 0x3B0, stored at 0x80067750; e.g. 0x888888) when n1 >= 0 and n2 < 0, black
// otherwise; the view's mirror byte (scratch + 0x398, negative = mirrored; 0x800672A4 inverts it for the wheels whose matrix
// 0x800670F0 mirrors) swaps the test to n1 < 0 and n2 >= 0. I.e. a quad is grey when its ring (A_k, A_k+1, B_k+1, B_k)
// faces the viewer the one way (the inner faces: the inboard disc seen through the rim) and black when it faces the other
// (the tread and sidewall seen from outside). The quad's ordering-table depth is the largest of its four SZ when grey, the
// smallest when black: ((z << scratch + 0x98) >> 13), at most 4095, one entry past scratch + 0x64.
bool WheelQuadGrey(int32_t nclip1, int32_t nclip2, int8_t mirror);

// The routine itself for the PS1 path (verification): the strips of the stream at `stream` in `ram` (2 MiB guest RAM image)
// through a GTE holding `rotation` / `translation` / `ofx` / `ofy` / `h` (DQA 4096, DQB 0, as 0x8007B778 loads them), with
// the packets written from `packet` and linked into the ordering table at `otBase` (+ 4, scratch + 0x64); returns the next
// free packet address (scratch + 0x68). `colourMask` = scratch + 0x3B0, `otShift` = scratch + 0x98, `mirror` = + 0x398.
struct WheelStripGte {
    int16_t rotation[3][3] = {};
    int32_t translation[3] = {};
    int32_t ofx = 0, ofy = 0;
    uint16_t h = 0;
};
uint32_t EmitWheelStrips(uint8_t* ram, uint32_t stream, const WheelStripGte& gte, uint32_t packet, uint32_t otBase, uint16_t otShift, int8_t mirror,
                         uint32_t colourMask);

// One wheel from the generated area (LOD 0 strips + the rim square) at a centre in car metres. The strip quads carry the
// colour rule above as two one-sided copies (CarMeshVertex::cullBack): the face the original colours grey in grey
// (`stripGrey`, the .cdo's LOD 0 word at + 0x08 / 255), the other face black; the rim textured (texels 0..47 of the car
// texture, CLUT 0).
struct WheelMeshParams {
    float centre[3];      // metres, car axes
    float side;           // -1 = left wheel (outboard = -X), +1 = right
    float stripGrey[3] = {128.0f / 255.0f, 128.0f / 255.0f, 128.0f / 255.0f};
    bool twoFaces = true; // false: one copy per quad, sidewall and tread black, inboard disc grey (double-sided exports)
};
void BuildWheelMesh(std::vector<CarMeshVertex>& out, const WheelArea& area, int axle, const WheelMeshParams& wheel);
// A wheel of given radius / width in metres (external meshes): the default ratio and dish of 0x80061504.
void BuildWheelMesh(std::vector<CarMeshVertex>& out, float centre[3], float side, float radius, float width);

// The ground shadow (CarShadow) as a triangle list in car metres on the plane y = groundY, with white corners
// where the original subtracts the full colour and black where it subtracts nothing; draw it with PS1 blend
// mode 2 (B - F). Empty when the model has no shadow block. groundY: the original places the shadow 271 LOD 0
// units (0.13 m) below the body origin of us36n (captured), i.e. about 4 cm above the wheel contact height
// (wheel centre y - radius = -0.17 m): pass CarShadowHeight(model).
std::vector<CarMeshVertex> BuildCarShadowMesh(const CarModel& model, float groundY);
float CarShadowHeight(const CarModel& model);

// Body units -> metres: 2^(lod.scale - 16) / 4096 (see docs/formats/car_cdo_cdp.md).
double CarBodyMetresPerUnit(const CarLod& lod);
constexpr double kCarWheelMetresPerUnit = 1.0 / 4096.0;

} // namespace gt2
