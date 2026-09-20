#pragma once
// The race cameras of Gran Turismo 2, ported from US Simulation v1.2 (SCUS_944.88, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): the camera code of the race overlay (GT2.OVL member 0, loaded at
// 0x80010000) and the executable's camera / matrix library it calls. Evidence: our disassembly of the race overlay in
// work\re\license_race\ram.bin and work\re\race_demo\ram.bin (identical code), gt2run sessions (docs/formats/camera.md).
//
// One camera object per player (view object + 0xC4 + player * 0x110; 0x801FF97C in the dumps). Per race frame the
// frame driver 0x80015B64 runs the physics tick 0x8003EBF0 (which ends with the render transforms 0x800133F0, car +
// 0x81C) and then Update 0x800100F4 on each camera; the renderer 0x800294D4 turns the camera into the GTE state
// (0x8007B374). The cameras:
//   player's race (0x800A951C == 0), 0x800102D8:
//     start hold (0x800A9520 != 0) = the intro fly-around 0x80010608 (three phases on the hold counter);
//     else the chosen position 0x800103C0: 0 Driver (the car's matrix raised 0.8 m, rear-view mirror on, own car
//     hidden), 1 Chase 1 / 2 Chase 2 (behind the car at the car's attitude lagged by the physics' view offsets body +
//     0x73A / + 0x73C: 1.8 m up and 5.4 m / 6.8 m back); button 0x100 cycles the position, 0x200 held looks back;
//   replays / the attract race (0x800A951C != 0), 0x800109FC: the course's trackside cameras (.tro header + 0x1C,
//     chosen by lap and course distance of the followed car; four record types) or the onboard set 0x80011704.
// Projection: 320 x 240 frame, window (-160, 160) x (132, -132) at distance H (0x80010088 -> 0x8007B320), i.e. the
// y axis is scaled by 240 / 264; H = the View Angle option (0x8002F370: 277 / 216 / 190), zoomed by the replay
// cameras. The routines below carry the address of the routine they port; they are bit-exact (tools/gt2verify
// verify_camera.cpp compares the whole guest RAM after each).
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace gt2 {
struct Track;
struct GuestImage;
} // namespace gt2

namespace gt2::camera {

#pragma pack(push, 1)
// The library's MATRIX: s16 rotation (4096 = 1), s32 translation (16.16 m, world +Y up).
struct Matrix {
    int16_t m[3][3];
    int16_t pad;
    int32_t t[3];
};
static_assert(sizeof(Matrix) == 0x20);

// The race camera object (0x110 bytes). The first 0x9C bytes are the executable's camera ("library camera":
// drawing rectangle, window, GTE state of 0x8007B374); the race code writes only the rectangle and the window.
struct RaceCamera {
    int16_t rectX, rectY, rectW, rectH;       // 0x000  drawing rectangle (0x80010088: 0, 0, 320, 240 >> split)
    uint8_t gteState[0x88];                   // 0x008  0x8007B374's output (P V^T, translation, clip planes, H, ...)
    int16_t centreX, centreY;                 // 0x090  window centre << 12 / H (0x8007B320)
    int16_t spanX, spanY;                     // 0x094  window width / height at distance H (320 / 264)
    int16_t H;                                // 0x098  projection distance
    uint8_t reserved09A[2];
    uint32_t pad;                             // 0x09C  guest pointer of the pad object (logical buttons +0x6C/+0x8C/+0x90)
    int32_t chunk;                            // 0x0A0  course chunk of the camera
    Matrix view;                              // 0x0A4  camera to world: columns = right, up, back (looks along -z)
    int16_t angles[3];                        // 0x0C4  yaw, pitch, roll of `view` (0x800811B0)
    int16_t reserved0CA;
    int32_t previous[3];                      // 0x0CC  position of the last frame
    int32_t velocity[3];                      // 0x0D8  position - previous
    int16_t direction[3];                     // 0x0E4  velocity normalised (4096 = 1)
    int16_t reserved0EA;
    int16_t backAxis[3];                      // 0x0EC  column 2 of `view` (the sound's listener axes)
    int16_t reserved0F2;
    int16_t rightAxis[3];                     // 0x0F4  column 0 of `view`
    int16_t reserved0FA;
    int32_t speed;                            // 0x0FC  |velocity|
    uint16_t replayFlags;                     // 0x100  word 0 of the replay camera record in use (type = bits 0..3)
    uint8_t reserved102;
    uint8_t split;                            // 0x103  half-height view (two players)
    uint8_t reserved104;
    uint8_t replayMode;                       // 0x105  replays: 0 trackside, 1 onboard set, 2 trackside following the leader
    uint8_t onboardView;                      // 0x106  replays: onboard set entry 0..8
    uint8_t replayInfo;                       // 0x107  Replay Info option (career + 0xAE; 2 in licence replays)
    uint8_t hideTarget;                       // 0x108  the followed car is not drawn (driver / onboard views)
    uint8_t mirror;                           // 0x109  rear-view mirror drawn (driver view, not looking back)
    uint8_t inCarSound;                       // 0x10A  in-car sound mix
    uint8_t external;                         // 0x10B  camera not attached to the car (trackside) - sound
    uint8_t target;                           // 0x10C  followed car index
    uint8_t reserved10D;
    uint8_t position;                         // 0x10E  Camera Position: 0 Driver, 1 Chase 1, 2 Chase 2
    uint8_t lookBack;                         // 0x10F  look-back button held this frame
};
static_assert(sizeof(RaceCamera) == 0x110);
static_assert(offsetof(RaceCamera, centreX) == 0x90);
static_assert(offsetof(RaceCamera, pad) == 0x9C);
static_assert(offsetof(RaceCamera, view) == 0xA4);
static_assert(offsetof(RaceCamera, angles) == 0xC4);
static_assert(offsetof(RaceCamera, previous) == 0xCC);
static_assert(offsetof(RaceCamera, direction) == 0xE4);
static_assert(offsetof(RaceCamera, backAxis) == 0xEC);
static_assert(offsetof(RaceCamera, rightAxis) == 0xF4);
static_assert(offsetof(RaceCamera, speed) == 0xFC);
static_assert(offsetof(RaceCamera, replayFlags) == 0x100);
static_assert(offsetof(RaceCamera, split) == 0x103);
static_assert(offsetof(RaceCamera, replayMode) == 0x105);
static_assert(offsetof(RaceCamera, hideTarget) == 0x108);
static_assert(offsetof(RaceCamera, target) == 0x10C);
static_assert(offsetof(RaceCamera, position) == 0x10E);
static_assert(offsetof(RaceCamera, lookBack) == 0x10F);

// One entry of the onboard view table (race overlay 0x8002F378, 13 x 24 bytes): offset in the car's frame and two
// rotations (yaw, pitch, roll) before / after it (0x800113C0). Entries 8..11 (side views) get their x from the car's
// half width at every use.
struct OnboardView {
    int32_t offset[3];
    int16_t before[3];
    int16_t after[3];
};
static_assert(sizeof(OnboardView) == 24);
#pragma pack(pop)

// Logical pad bits the camera reads (the pad object's words; with the default key configuration 0x100 = R1 and
// 0x200 = L1 - measured with gt2run session on the licence race).
constexpr uint32_t kButtonView = 0x100;      // + 0x90 (pressed): next camera position
constexpr uint32_t kButtonLookBack = 0x200;  // + 0x8C (held): look back
// Replay buttons (+ 0x6C): 0x800 replay mode, 0x400 onboard view, 0x200 Replay Info, 0x100 split view (mode 0),
// 0x001 / 0x002 previous / next car by race position, 0x010 / 0x1000 the mode 6 ghost's +0x21 byte.

// Tables of the race overlay the cameras read. GAME DATA: loaded from the disc's GT2.OVL at run time.
struct CameraConstants {
    std::array<int32_t, 8> chase{};             // 0x8002F350: {y1, y2, z1, z2} full view, {y1, y2, z1, z2} split view (16.16 m)
    std::array<int16_t, 3> viewAngle{};         // 0x8002F370: H of View Angle Narrow / Standard / Wide
    std::array<OnboardView, 13> onboard{};      // 0x8002F378 (initial contents; the camera writes entries 8..11)
};
constexpr uint32_t kChaseTableAddress = 0x8002F350u, kViewAngleTableAddress = 0x8002F370u, kOnboardTableAddress = 0x8002F378u;
CameraConstants LoadCameraConstants(const GuestImage& raceOverlay);

// The trackside camera list of a course: .tro header + 0x1C -> { u16 count, u16 lapModulus, u32 record[count] }.
// Records (by type = word 0 bits 0..3): u16 flags, u16 firstLap, u16 lastLap, u16 chunkHint, s32 start, s32 end
// (course distance, 16.16 m), u16 H0, u16 H1, then type 0: u32 ?, path at + 0x18; type 1: s32 position[3] at
// + 0x14, s16 pitch, yaw, roll at + 0x20; type 2: H0 = onboard view index, H1 = H; type 3: u16 zoom at + 0x14,
// path at + 0x1C. Path: u16 kind (0 polyline, 1 cubic), u16 count, segments of 16 / 56 bytes (docs/formats/camera.md).
// The data is read like the original reads it (by address), so `bytes` is either the .tro file (pointers are file
// offsets, base 0) or a RAM image (base 0x80000000; the loader relocated the pointers).
struct ReplayCameraData {
    std::span<const uint8_t> bytes;
    uint32_t base = 0;   // pointer value of bytes[0]
    uint32_t list = 0;   // pointer of the list (0 = the course has none)
    bool Valid() const { return list != 0; }
    uint16_t U16(uint32_t pointer) const;  // throw std::out_of_range outside `bytes`
    int16_t S16(uint32_t pointer) const { return int16_t(U16(pointer)); }
    int32_t S32(uint32_t pointer) const;
    // Record pointers of the list (file offsets / guest addresses; walks the list header).
    std::vector<uint32_t> Records() const;
};
// The list of a .tro file (header + 0x1C; empty when the offset is 0 or the list is empty).
ReplayCameraData ReplayCamerasOfTro(std::span<const uint8_t> tro);

// What the camera reads of one car record (0x800A9688 + car * 0xB40).
struct CameraCar {
    Matrix pose{};                    // car + 0x81C  render transform (0x800133F0 = sim::PhysicsPoseOf; pad copied along)
    int32_t chunk = 0;                // car + 0x14   = body + 0x600 as 0x800133F0 copied it
    int16_t viewYawOffset = 0;        // car + 0x766  body + 0x73A (heading - view yaw, 0x8003E8E4)
    int16_t viewPitchOffset = 0;      // car + 0x768  body + 0x73C
    int32_t courseDistance = 0;       // car + 0x630  body + 0x604
    int16_t lap = 0;                  // car + 0x634  body + 0x608
    int8_t racePosition = 0;          // car + 0x77C  body + 0x750
    int32_t halfWidth = 0;            // car + 0x87C  (CarHalfWidth, set by 0x80017E74)
    uint16_t lodScale = 16;           // LOD 0 block + 0x4C (car + 0x878 -> + 0x870 -> LOD 0) - scale exponent
    uint16_t lodSize = 1;             // LOD 0 block + 0x4E
    uint8_t* ghostByte = nullptr;     // car + 0x21, written by the replay controls of mode 6 (null = not kept)
};
// car + 0x87C from the car's LOD 0 block (0x80017E74): max(|bbox[0]|, bbox[4]) scaled by 2^(scale - 16), << 4.
int32_t CarHalfWidth(const std::array<int16_t, 8>& lod0Bbox, int16_t lod0Scale);

// The world the camera update reads and the state outside the camera object it writes.
struct CameraWorld {
    std::span<const CameraCar> cars;  // the race's cars (count = 0x800AF231)
    uint8_t gameMode = 2;             // 0x801D5866 (0 two players, 3 licence, 6 time trial with ghost)
    uint8_t replay = 0;               // 0x800A951C (attract race / replay)
    uint16_t hold = 0;                // 0x800A9520 start hold, fields left
    uint16_t holdInitial = 0;         // 0x800A951E
    uint8_t viewAngle = 1;            // 0x801C9992 (career + 0xB2)
    const Track* track = nullptr;     // 0x80028394 (camera chunk of the trackside cameras) and the course length
    int32_t courseLength = 0;         // chunk table + 0 (16.16 m)
    ReplayCameraData replayCameras;   // 0x800B4A50
    const CameraConstants* constants = nullptr;
    int32_t* replayProgress = nullptr;  // 0x800A8D6C: progress of the car through the current trackside camera (4096 = 1)
    OnboardView* onboard = nullptr;     // 0x8002F378: the live table (13 entries; entries 8..11 are written)
};

// The pad object's logical buttons (+ 0x8C held, + 0x90 pressed, + 0x6C pressed for the replay controls).
struct CameraPad {
    uint32_t held = 0, pressed = 0, replayPressed = 0;
};

// 0x80010000: clears the object, follows `car`, position = Camera Position option (career + 0xAF), Replay Info =
// 1 (in replays career + 0xAE, 2 in licence replays). The caller sets `pad` (+0x9C) and `split` afterwards.
void InitCamera(RaceCamera& camera, uint8_t car, uint8_t cameraPositionOption, uint8_t replayInfoOption, const CameraWorld& world);
// 0x800100F4: the per-frame update (after the physics tick).
void UpdateCamera(RaceCamera& camera, const CameraPad& pad, const CameraWorld& world);

// The individual routines (verified one by one).
void SetProjection(RaceCamera& camera, int32_t H, int32_t centreX, int32_t centreY);   // 0x80010088
void SetViewAngleProjection(RaceCamera& camera, const CameraWorld& world);             // 0x80010298
void LoadTarget(RaceCamera& camera, const CameraWorld& world);                         // 0x800101FC
void RaceView(RaceCamera& camera, uint8_t position, const CameraWorld& world);         // 0x800103C0
void PlayerCamera(RaceCamera& camera, const CameraPad& pad, const CameraWorld& world);  // 0x800102D8
void StartIntro(RaceCamera& camera, const CameraWorld& world);                         // 0x80010608
void ReplayCamera(RaceCamera& camera, const CameraPad& pad, const CameraWorld& world);  // 0x800109FC
void OnboardCamera(RaceCamera& camera, int32_t view, const CameraWorld& world);        // 0x800113C0
void OnboardSet(RaceCamera& camera, const CameraWorld& world);                         // 0x80011704
// 0x800117C4: the trackside record for a lap / course distance (pointer, 0 = none).
uint32_t FindReplayCamera(const ReplayCameraData& data, int32_t lap, int32_t distance, int32_t courseLength);
// 0x80011890: the position on a camera path at progress `progress` (4096 = 1) into `inout`, then z negated (world);
// a path kind other than 0 / 1 leaves `inout` as it was (and still negates z), like the original.
void PathPosition(const ReplayCameraData& data, uint32_t path, int32_t progress, int32_t inout[3]);
// 0x80010E20: rotation looking along `direction` (camera -z towards it, no roll).
void LookAlong(Matrix& out, const int32_t direction[3]);

// Library routines (EXE) the cameras are built from.
void RotationFromAngles(int16_t out[3][3], int32_t yaw, int32_t pitch, int32_t roll);   // 0x80081374
void AnglesFromRotation(int16_t out[3], const int16_t m[3][3]);                          // 0x800811B0
int32_t VectorLength(int16_t direction[3], const int32_t v[3]);                          // 0x80081164
// 0x80082DA8: arc sine (4096 = 1 in, 4096 units per turn out) by the executable's table 0x800A2AC4, which is
// round(asin(i / 4096) * 4096 / 2 pi) (generated here; gt2verify compares it with the executable's).
int32_t ArcSine(int16_t x);

// The rear-view mirror of the renderer 0x800294D4: drawn when the camera's + 0x109 is set, the view is not split, the
// game mode is not 0 and the frame-rate mode 0x801D5864 is above 1 (30 Hz).
bool MirrorShown(const RaceCamera& camera, uint8_t gameMode, uint8_t frameRateMode);
// 0x800294D4's mirror camera: a copy of the camera object, its view moved (0, 0x3333, 0x8000) = 0.2 m up, 0.5 m back
// along its own axes (0x8007B050), pitched by -32 (0x8007B088 with (0, -32, 0)), z mirrored (0x8007B25C with (0x1000,
// 0x1000, -0x1000): the third column negated - the view looks backwards, mirrored), rectangle (0, 0, 120, 32) and window
// (-60, 60) x (16, -16) at H 120 (0x8007B320). Drawn into the frame's rectangle kMirrorRect (0x8008034C: the draw area
// (100, 20) 120 x 32; 0x8007E780 then outlines (0, 0, 120, 32) and (1, 1, 118, 30) in black).
RaceCamera MirrorCamera(const RaceCamera& camera);
constexpr int kMirrorRect[4] = {100, 20, 120, 32}; // x, y, w, h on the 320 x 240 frame

// ---------------------------------------------------------------- renderer side (float)

// The camera as the renderer needs it: 0x8007B374's GTE rotation P V^T (P = screen matrix of the window) and the
// translation, as float rows over world metres, and the frame geometry. Pixel on the original's frame:
// x = rectW / 2 + H * a.x / a.z, y = rectH / 2 + H * a.y / a.z with a = rows * (p, 1).
struct CameraProjection {
    float rows[3][4]{};
    float H = 216, halfWidth = 160, halfHeight = 120;
    float eye[3]{};                      // metres
    float right[3]{}, up[3]{}, forward[3]{}; // world axes of the camera
};
CameraProjection ProjectionOf(const RaceCamera& camera);
// Column-major clip matrix (Vulkan: y down, reversed Z with an infinite far plane, z_ndc = zNear / depth) of the
// camera for a window of aspect `aspect` (width / height): the original's vertical field of view (and its 240 / 264
// vertical scale) is kept, the horizontal one widens with the aspect (4:3 = exactly the original's frame).
void ClipMatrix(const RaceCamera& camera, float aspect, float zNear, float out[16]);

} // namespace gt2::camera
