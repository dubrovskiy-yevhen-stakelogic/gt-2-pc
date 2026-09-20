#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/overlay_data.h"
#include "gt2view/vk_scene_renderer.h"

namespace gt2view {

// Tyre smoke of the race (US Simulation v1.2, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; race overlay;
// docs/formats/particles.md). The original keeps one pool of 256 sprite records at 0x800ADA0C (header 0x18 bytes,
// records 0x18 bytes at + 0x18):
//   0x8001681C  race load: clear, chain every record into the free list, header constants
//   0x80016978  once per frame (BeginFrame 0x80015B64): fade, rise, grow; unlink the records that faded out
//   0x800133F0  per car after the physics (frame driver 0x8003EBF0): for each wheel whose skid level (wheel + 0x1C)
//               is >= 0x80, a record at the wheel centre (car matrix x wheel record car + 0x7C4 + w * 0x10)
//   0x800168A4  allocation (only when the frame took >= 2 fields, 0x801D5864)
//   0x8002EB60  draw (main view only, from 0x8001545C): one screen-aligned rotated POLY_FT4 per record,
//               additive (tpage 0x29: page (576, 0), mode 1 = B + F), 4-bit texture crstim.arc entry 2
//               (image (608, 0), CLUT (352, 510)), grey = intensity >> 5, ordering table SZ3 >> 3.
// No other routine of the race overlay reads the effect levels for drawing: there are no skid marks, and the
// smoke / dust levels (wheel + 0x1D / 0x1F) feed only the sound (0x800149AC..). Kind 1 (crstim.arc entry 1,
// 48 x 48 at (624, 0), CLUT (368, 510)) is drawn by 0x8002EB60 but no race routine allocates it.

struct SmokeParticle {    // 0x18 bytes, the original's record
    int16_t kind;         // + 0x00  0 = tyre smoke (the only kind the race spawns), 1 = the 48 x 48 sprite
    int16_t next;         // + 0x02  next record of the active / free list, -1 = end
    int16_t size;         // + 0x04  half size of the sprite, 1/64 m (0 at the spawn: not drawn in that frame)
    int16_t intensity;    // + 0x06  skid level << 4 at the spawn; the record dies when it turns negative
    uint16_t angle;       // + 0x08  x ^ y ^ z of the spawn position (low 16 bits): sprite rotation = angle & 0xFFF
    int16_t fade;         // + 0x0A  intensity lost per frame: fadeRate[kind] x fadeScale[scaleIndex] >> 12
    int32_t position[3];  // + 0x0C  world position, 16.16 m (render axes: x, y up, z)
};
static_assert(sizeof(SmokeParticle) == 0x18);

class SmokePool {
public:
    static constexpr int kCapacity = 256;
    static constexpr uint32_t kFadeScaleAddress = 0x8002F4F8; // race overlay: 16 s16 factors (4096 = 1.0)

    // Per-allocation fade factors, cycled by scaleIndex: game data, read from the race overlay (LoadFadeScale).
    std::array<int16_t, 16> fadeScale{};
    // Throws std::out_of_range when the image does not hold the table.
    void LoadFadeScale(const gt2::GuestImage& raceOverlay);

    // Header (0x800ADA0C + 0x00 .. 0x17), set by 0x8001681C.
    int32_t rise = 0x444;       // + 0x00  added to position[1] per frame (16.16 m: ~1 cm)
    int32_t fadeRate[2] = {0x90, 0x48}; // + 0x04 / + 0x08  per kind
    int32_t growth = 8;         // + 0x0C  added to size per frame
    int16_t scaleIndex = 0;     // + 0x10  cycles 0..15 over the allocations
    int16_t activeHead = -1;    // + 0x12  most recent record first
    int16_t freeHead = 0;       // + 0x14
    std::array<SmokeParticle, kCapacity> records{};

    SmokePool() { Reset(); }
    void Reset();                                              // 0x8001681C
    SmokeParticle* Allocate(int kind, int fieldsPerFrame = 2); // 0x800168A4 (nullptr: pool full or a 60 Hz frame)
    void Update();                                             // 0x80016978
    size_t ActiveCount() const;

    // The spawn loop of 0x800133F0 for one car. `rotation` / `translation` = the car's render matrix (car + 0x81C,
    // s16 at 4096 = 1.0, row-major; car + 0x830, 16.16 m); per wheel (front left, front right, rear left, rear
    // right) its record car + 0x7C4 + w * 0x10 in 1/4096 m model axes: x = -/+ half track (body + 0x18 + axle * 2),
    // y = ride reference - travel (body + 0x134 + axle * 0x34, wheel + 0x10), z = the .cdo wheel entry's third s16
    // (file + 0x24 + w * 8) - and its skid level (wheel + 0x1C).
    struct CarInput {
        int16_t rotation[3][3] = {};
        int32_t translation[3] = {};
        struct Wheel { int16_t x = 0, y = 0, z = 0; uint8_t skidLevel = 0; } wheels[4];
    };
    void SpawnFromCar(const CarInput& car, int fieldsPerFrame = 2);

    // ---- drawing
    // The original's projection (0x8002EB60) for a PS1 view: every record's primitive as the GPU receives it.
    // `rotation` = the camera matrix (view + 0x08), `offset` = the negated camera position added before >> 10
    // (view + 0x28, 16.16 m), `h` = projection distance (view + 0x64), `ofx` / `ofy` = screen offset 16.16
    // (view + 0x5C / + 0x60). RTPS runs with TR = 0, DQA = size, DQB = 0.
    struct PsxView {
        int16_t rotation[3][3] = {};
        int32_t offset[3] = {};
        uint16_t h = 0;
        int32_t ofx = 0, ofy = 0;
    };
    struct Primitive {         // POLY_FT4, code 0x2E (textured, semi-transparent, modulated)
        uint8_t shade = 0;     // r = g = b
        int16_t x[4] = {}, y[4] = {};
        uint8_t u[4] = {}, v[4] = {};
        uint16_t clut = 0, tpage = 0;
        uint32_t otIndex = 0;  // SZ3 >> 3, at most 0xFFF
        std::string ToString() const; // the format of gt2play --prims ("POLY 2E quad tex semi ...")
    };
    std::vector<Primitive> BuildPrimitives(const PsxView& view) const;

    // The same sprites for the native renderer: screen-aligned squares in world metres (`right` / `up` = the
    // camera's axes in world axes), half side = size / 64 m, rotated by angle & 0xFFF like the PS1 quad, grey
    // intensity >> 5 modulating the texture, to be drawn additively (DrawItem::blend = 1). The original skips a
    // sprite whose radius on the 240-line frame is 256 pixels or more (or whose camera-space position does not
    // fit in 16 bits of 1/64 m); `projectionDistance` = the view's PS1-equivalent H for that test.
    void AppendBillboards(std::vector<SceneVertex>& out, const float eye[3], const float right[3], const float up[3], const float forward[3],
                          float projectionDistance) const;
};

// The sprite textures' VRAM places (0x8002EB08 rewrites the TIM headers of crstim.arc entries 1 and 2).
struct SmokeTexturePlacement { uint32_t arcEntry; int imageX, imageY, clutX, clutY; };
inline constexpr SmokeTexturePlacement kSmokeTextures[2] = {{2, 608, 0, 352, 510}, {1, 624, 0, 368, 510}}; // kind 0, kind 1

// Dev aid for tools/gt2play (--smoke-log): the pool mirrored from the running original. At the original's update
// call (0x80016978) the mirror runs Update(); at its draw call (0x8002EB60) the mirror spawns from the guest's cars
// (car records in guest RAM) and compares its whole pool with the guest's; a mismatch is reported and the mirror
// is resynchronised. DrawLines lists BuildPrimitives of the GUEST pool with the guest view, for comparing with the
// captured GP0 primitives. `ram` = the 2 MiB of guest RAM.
class SmokeGuestMirror {
public:
    void OnUpdate();
    // Returns a one-line report.
    std::string OnDraw(const uint8_t* ram);
    static std::vector<std::string> DrawLines(const uint8_t* ram, uint32_t viewAddress);
    static SmokePool ReadGuestPool(const uint8_t* ram);
    size_t frames = 0, matched = 0, spawnsSeen = 0;

private:
    SmokePool pool_;
    bool synced_ = false;
    bool updatePending_ = false;
};

} // namespace gt2view
