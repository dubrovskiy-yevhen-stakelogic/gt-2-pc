#pragma once
// The car view of the GT-mode menus (pages with a type 0x0A viewport item: dealer / used-car / garage car pages),
// ported from US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); member 4 of
// GT2.OVL ("ovl4", the GT-mode menus at 0x80010000) + the EXE's camera library. Evidence: our disassembly of the RAM
// dumps work\re\gtmode\ram.bin / work\re\menu_car\ram_002700.bin and gt2play captures (docs/formats/gt_menu.md
// section 10).
//
//   0x8001D5C8 (show car) -> 0x8001D090: camera reset (pitch 0xA0, yaw 0x1500, position (0, 0, 0x94CCC) 16.16 m,
//                            floor on, floor colour 0x525252) + 0x8001AC20: model, texture, name logo, paint
//   0x8001D258 (every view update while no page / car / wheel change is pending): pitch = 110, yaw += 12
//                            (+ 113 while the car wash runs), yaw & 0x3FFF; the matrices use yaw & 0xFFF
//   0x8001B9AC case 0x0A:  viewport = the item's rectangle (draw area + offset), window (-a, a) x (179, -50) at
//                            distance H = 800 with a = ((w * 256) / h) >> 1 (0x8007B320 / 0x8007B374)
//   0x8001A8A4:            camera V = Ry(-yaw) Rx(-pitch), eye = V (0, 0, 0x94CCC); the car (0x80067444, LOD 0,
//                            wheels, shadow, reflection intensity 0x40) at (0, (model +0x18 - model +0x22) * 16, 0);
//                            then the floor disc (0x8006C31C) with a camera without the yaw
//   0x8001A654 (type 0x11): the name logo sprite, 0x8001AC20 uploads carlogo/<id>n--.tim (0x8005D950 index table)
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <span>
#include <vector>

#include "gt2formats/gt_menu.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/overlay_data.h"

namespace gt2 {
class GtfsVolume;
}

namespace gt2::menu {

// ---------------------------------------------------------------- name logo (type 0x11)

// A 4-bit TIM of carlogo/ as 0x8001AC20 uploads it: CLUT block to (576, 164) with its own size (0x8007BC1C), image
// block to (576, 165) (0x8007BC58); the sprite is (width words * 4) x height texels at uv (0, 0xA5) of texture page
// 9 with CLUT 0x2924.
struct MenuCarLogo {
    static constexpr int kClutX = 576, kClutY = 164, kImageX = 576, kImageY = 165;
    static constexpr uint16_t kTpage = 9, kClut = uint16_t((kClutX / 16) | (kClutY << 6)); // 0x2924
    uint16_t clutWords = 0, clutRows = 0;
    std::vector<uint8_t> clut;
    uint16_t imageWords = 0, imageRows = 0;
    std::vector<uint8_t> image;
    int Width() const { return imageWords * 4; }
    int Height() const { return imageRows; }
};
MenuCarLogo ParseMenuCarLogo(std::span<const uint8_t> tim);
// The logo of a car as the boot's car file table (0x801DF5D0: {u32 car id, u16 .cdo file, u16 logo file}, count
// s16 0x801C93C8, binary search 0x8005D950) names it: "carlogo/" + id + "n--.tim" (US GT), else id + "l--.tim"
// (JP GT: the race cars have only l / m), else the directory's first file (a-a7rl--.tim; 26 cars) - the rule holds
// for all 1110 entries of the table in work\re\gtmode\ram.bin. nullopt when carlogo/ is empty.
std::optional<MenuCarLogo> LoadMenuCarLogo(const GtfsVolume& vol, uint32_t carId);
void UploadMenuCarLogo(MenuVram& vram, const MenuCarLogo& logo);
// 0x8001A654(view, ot, cx, cy): SPRT 0x808080 at (cx - (w >> 1), cy - (h >> 1)), then DR_TPAGE 9.
MenuPrim MenuCarLogoSprite(const MenuCarLogo& logo, int cx, int cy);

// ---------------------------------------------------------------- bought wheels (carwheel/*.tim)

// The wheel files as the boot builds their table (EXE 0x8001194C: the carwheel/ directory's entries in VOL order,
// u32 table 0x801E30F0 = 0x80011570(name), count s16 0x801C93B4; the directory's first file number u16 0x801E2FC6).
// 0x80011570 packs the name "mmNNN-kc" like the wheel shop's 0x80013A28 (career::WheelIdOfCode) but with the EXE's
// maker pair list 0x80033DD0 ("bb", "br", "du", ...): ((maker << 12 | NNN) << 3 | k) << 13 | c.
struct MenuWheelFiles {
    std::vector<uint32_t> ids;     // the table, in directory order
    std::vector<std::string> paths; // the VOL paths of the same entries
};
uint32_t MenuWheelIdOfName(const GuestImage& exe, const std::string& name); // 0x80011570
MenuWheelFiles LoadMenuWheelFiles(const GtfsVolume& vol, const GuestImage& exe);
// 0x800615E8(word, buffer): word &= 0xFFFFE0FF (drops the dish / colour code << 8 of CarConfig +0x00, 0x80021B38); a
// wheel is loaded only when the rest is != 0; 0x80060D74(word): the file = base +
// the index of `word` in the table by the original's binary search (mid = (lo + hi) >> 1; equal -> mid; hi <= lo ->
// not found; below -> lo = mid + 1, else hi = mid); not found -> base, i.e. the directory's FIRST file.
// Returns the index into `files` (-1 = no wheel file: the car's own wheels).
int MenuWheelFile(const MenuWheelFiles& files, uint32_t word);
// The 0x80091A70 s16 entry `index` (the rim's dish; 0x8001AEF8 indexes it with the wheel row's colour byte unmasked).
int16_t MenuWheelDish(const GuestImage& exe, int index);
// A wheel TIM (4-bit, 48 x 48, CLUT 16 x 1): 0x800678E8(buffer, slot, 1) puts the image at ((slot & 15) * 64, (slot &
// 16) * 16) = over the top-left 48 x 48 texels of the car's texture and the CLUT at (slot * 64, 224 + ...) = the rims'
// CLUT (model +0x40, 0x8006101C); 0x8006155C(model, 0, CLUT entry 0) sets model +0x08 (reader not traced: not drawn).
struct MenuWheelTexture {
    std::array<uint16_t, 16> clut{};
    std::vector<uint16_t> image; // 12 halfwords x 48 rows (4-bit texels, 4 per halfword)
    int words = 0, rows = 0;
};
MenuWheelTexture LoadMenuWheelTexture(const GtfsVolume& vol, const std::string& path);

// ---------------------------------------------------------------- camera

// The camera object at page object + 0x478 (the fields the menus use).
struct MenuCarCamera {
    static constexpr int16_t kResetPitch = 0xA0, kResetYaw = 0x1500, kPitch = 110, kYawStep = 12, kWashYawStep = 113;
    static constexpr int32_t kDistance = 0x94CCC; // +0xA4 (x +0x9C, y +0xA0 = 0), 16.16 m
    static constexpr uint32_t kFloorColour = 0x525252;
    int16_t pitch = kResetPitch; // +0xA8
    int16_t yaw = kResetYaw;     // +0xAA
    bool floor = true;           // +0xD0
    bool floorSemi = false;      // +0xD1: POLY_G3 0x32 instead of 0x30
    uint32_t floorColour = kFloorColour; // +0xD4
    void Reset();                // 0x8001D090
    void Update(bool washing);   // 0x8001D258 (0x8001D0E8 with 12, and 113 while the wash timer runs)
};

// The GTE state of the view for one draw (0x8007B320 + 0x8007B374 with the viewport of the item): the rotation
// P * V^T (cam +8, 4096 = 1), the negated camera position (cam +40, 16.16 m), the screen offset (16.16) and H.
struct MenuCarProjection {
    int16_t x0 = 0, y0 = 0, w = 0, h = 0;    // drawing area and offset (0x8008034C with the item's rectangle)
    int16_t left = 0, right = 0, top = 179, bottom = -50, distance = 800; // 0x8001B9AC case 0x0A: cam +0xC4..+0xCC
    std::array<std::array<int16_t, 3>, 3> rotation{}; // cam +8
    std::array<std::array<int16_t, 3>, 3> view{};     // V (camera to world, 4096 = 1)
    std::array<int32_t, 3> eye{};                     // V.t: camera position (16.16 m)
    std::array<int32_t, 3> negEye{};                  // cam +40
    int32_t ofx = 0, ofy = 0;                         // cam +0x5C / +0x60 = w << 15 / h << 15
    int16_t H = 800;                                  // cam +0x64
};
// `withYaw`: the car's camera (Ry(-yaw) Rx(-pitch)); without: the floor's (Rx(-pitch) only, 0x8001A8A4's second part).
MenuCarProjection MenuCarProject(const MenuItem& viewport, const MenuCarCamera& camera, bool withYaw);

// One vertex through 0x8007E5E0 (relative to the camera, scaled down to 13 bits, RTPS with TR = 0): screen x / y
// relative to the drawing offset and the OT depth ((IR3 << shift) >> 13).
struct MenuCarScreenPoint { int16_t x = 0, y = 0, z = 0; };
MenuCarScreenPoint MenuCarTransform(const MenuCarProjection& p, const std::array<int32_t, 3>& world);

// 0x8006C274: the floor disc's 26 points (25 on a 4 m circle in 24 steps, 16.16 m, then the centre) and
// 0x8006C31C: 24 gouraud triangles centre (floor colour) -> rim (black), POLY_G3 0x30 (0x32 when semi), E1 0x200
// (dither), in frame coordinates (the drawing offset added), in the order the GPU draws them (the car's OT: far
// first). Triangles are MenuPrim quads with v3 = v2.
std::vector<MenuPrim> MenuCarFloor(const MenuCarProjection& p, const MenuCarCamera& camera);

// 0x80061544(model): the car's height above the floor, (model +0x18 - model +0x22) * 16 (16.16 m): the front wheel
// radius minus the front-left wheel's y, so that the wheels touch y = 0.
inline int32_t MenuCarLift(int16_t wheelRadiusFront, int16_t frontLeftWheelY) { return (int32_t(wheelRadiusFront) - int32_t(frontLeftWheelY)) * 16; }

// ---------------------------------------------------------------- the race overlay's model views (GT2.OVL member 0)

// The camera object that the race overlay's post-race views give 0x80048754(model, env, env2, camera, trophy) (0xDC
// bytes: RESULTS W+0x1D8, the post-race menu W+0x364, the championship end W+0x354; W = *(u32*)0x801C90A0). The draw
// (evidence: our disassembly of work/re/menu_race/ram.bin and of the capture dumps work/play/racescreens/cap):
// V = I, Ry(-yaw) (0x8007B14C), Rx(-pitch) (0x8007B0C4), Rz(-roll) (0x8007B1D4: [[c, -s, 0], [s, c, 0], [0, 0, 1]]),
// each V = V * R (0x8007B994); eye = V position (0x8007B050); then 0x8007B320 / 0x8007B374 with the window below and the
// rectangle's w / h (x / y are not read), the model at (0, 0x80061544(model), 0) unrotated, then (+0xD4) the floor
// disc with V = Rx(-pitch) only (no yaw, no roll) and the same position, all into the ordering table of the
// view's model environment (M+0xC0; its drawing area / offset is set by the view's draw: RESULTS (0, 0x96, 0x160, 300),
// the post-race menu (0x7C, 0xA0, 200, 200)).
struct OverlayModelCamera {
    std::array<int32_t, 3> position{};         // +0xA0 / +0xA4 / +0xA8, 16.16 m
    int16_t pitch = 0x5E, yaw = 0x1500, roll = 0; // +0xAC / +0xAE / +0xB0
    int16_t x = 0, y = 0, w = 0, h = 0;        // +0xC0..+0xC6
    int16_t left = 0, right = 0, top = 0, bottom = 0, distance = 400, farZ = 0x7FFF; // +0xC8..+0xD2 (0x8007B320)
    bool floor = true, floorSemi = false;      // +0xD4 / +0xD5
    uint32_t floorColour = 0;                  // +0xD8
};

// 0x80050BC4 (RESULTS): pitch 0x5E, yaw 0x1500, z 8 m, rectangle 0x160 x 300, window (-176, 176) x (133, -52) at H 400,
// floor colour 0x3E3E3E (opaque); then with the generator 0x80083AE0 seeded by the VSync counter (0x8007D23C(0)): yaw =
// r1 & 0xFFF, pitch = (r4 & 0x7F) + 0x50.
OverlayModelCamera ResultsModelCamera(uint32_t vsyncCounter);
// 0x80050CC4 (every RESULTS update) / 0x80049874 (every post-race menu update): yaw = (yaw + step) & 0x3FFF, step 12 /
// 16; z += 0.5 m when `frameLength` (M+0x234 = u32 0x801F0698, 256 / 257 in every capture) exceeds 0xFB90.
void TurnModelCamera(OverlayModelCamera& camera, int16_t step, int32_t frameLength);
// 0x80049780(camera, w, h) (the post-race menu 200 x 200, the championship end 0x160 x 0x1E0): semi-transparent floor
// 0xA2A2A2, pitch 0x5E, yaw 0x1500, z 11 m, window (-(w >> 1), w >> 1) x (a * 80 / 100, -(a / 5)) with a = h * 62 / 100,
// H 400.
OverlayModelCamera ModelViewCamera(int16_t w, int16_t h);
// 0x800593F4(camera, t) (the championship end's trophy, t = W+0x20C - 0x18 = 1..0x78): z = 4 m + t * 8 m / 90 until t =
// 90, then 12 m; roll = -((90 - t) * 6 + 64), then -64; y = 0x4CCC (0.3 m); yaw = (yaw + 8) & 0x3FFF.
void ChampionModelMotion(OverlayModelCamera& camera, int32_t t);
// The projection of 0x80048754 with the model environment's offset (envX, envY) as the frame position of the
// rectangle: `withYaw` = the model's camera, else the floor's.
MenuCarProjection OverlayModelProject(const OverlayModelCamera& camera, int16_t envX, int16_t envY, bool withYaw);
// The floor disc of 0x80048754 (0x8006C31C with +0xD5 / +0xD8; 0x8006C274 ignores its radius argument: always 4 m).
std::vector<MenuPrim> OverlayModelFloor(const MenuCarProjection& floorProjection, const OverlayModelCamera& camera);

// The car's projection as a float map for a renderer: frame pixel (x, y) = (x0 + ofx + H * a.x / a.z,
// y0 + ofy + H * a.y / a.z) with a = rows * (world_m, 1) (world in metres; a.z = distance in front of the camera, m).
struct MenuCarLinearView {
    float rows[3][4]{}; // P V^T / 4096 and -(P V^T) eye
    float originX = 0, originY = 0, H = 800;
};
inline MenuCarLinearView MenuCarLinear(const MenuCarProjection& p) { // header-only: renderers use it without linking gt2menu
    MenuCarLinearView v;
    for (int i = 0; i < 3; i++) {
        float t = 0;
        for (int j = 0; j < 3; j++) {
            v.rows[i][j] = float(p.rotation[size_t(i)][size_t(j)]) / 4096.0f;
            t += v.rows[i][j] * float(p.negEye[size_t(j)]) / 65536.0f;
        }
        v.rows[i][3] = t;
    }
    v.originX = float(p.x0) + float(p.ofx) / 65536.0f;
    v.originY = float(p.y0) + float(p.ofy) / 65536.0f;
    v.H = float(p.H);
    return v;
}

} // namespace gt2::menu
