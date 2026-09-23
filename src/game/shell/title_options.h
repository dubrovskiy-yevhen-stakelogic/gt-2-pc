#pragma once
// The options of the title overlay (GT2.OVL member 1, "ovl1"): the rows of the GLOBAL OPTIONS and RACE OPTIONS
// pages, the career-block bytes they edit and the rules of reading / writing / stepping a value. Ported from US
// Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), member 1 loaded at 0x80010000;
// evidence: our disassembly / Ghidra pseudo-C of work/re/title/ram.bin and the readers of the bytes in the race
// overlay (member 0) and the EXE (docs/research/menus_gtmode.md section 10).
//
// Option ids (row byte +0) and their bytes (0x80017D74 get / 0x80017E68 set; offsets into the career block
// 0x801C98E0, which is saved on the card, so the options travel with the save):
//   0 arcade Car Damage   +0x02 (0 / 1)          7 Replay Info     +0xAE (None, Level1, Level2)
//   1 arcade Race Laps    +0x03 (1..99)          8 Camera Position +0xAF (Driver, Chase1, Chase2)
//   2 2P Tire Damage      +0x04 (None/Slow/Fast) 9 Chase View      +0xB0 (Type1, Type2) = the race's view mode
//   3 2P Car Damage       +0x05 (0 / 1)         10 Course Map      +0xB1 (Off, On)
//   4 2P Race Laps        +0x06 (1..99)         11 View Angle      +0xB2 (Narrow, Standard, Wide)
//   5 Handicap Start      +0x07 (s8, -100..100) 12 Music Volume    +0xB3 (0..254)
//   6 Slow Car Boost      +0x08 (None/Slow/Fast)13 SFX Volume      +0xB4 (0..254)
//                                               14 Vibration       +0x36 and +0x88 = (value != 1): the per-pad
//                                                  "no vibration" bytes (pad blocks of 0x52 at +0x0A / +0x5C)
// The GT-mode race setups copy +1..+4 to the race block 0x801D585C and +5..+8 to 0x801D5860 (ovl4 0x80012C6C /
// 0x80010078 / 0x80010A30, ovl1 0x800104E0), so 0x801D5860 = the 2P car-damage flag and the next three bytes.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/overlay_data.h"

namespace gt2::shell {

enum OptionId : uint8_t {
    kArcadeDamage = 0, kArcadeLaps = 1, kBattleTyreDamage = 2, kBattleDamage = 3, kBattleLaps = 4, kBattleHandicap = 5,
    kBattleBoost = 6, kReplayInfo = 7, kCameraPosition = 8, kChaseView = 9, kCourseMap = 10, kViewAngle = 11,
    kMusicVolume = 12, kSfxVolume = 13, kVibration = 14,
};

// Row kinds (row byte +1): how the value steps and how 0x8001805C draws it.
enum OptionKind : uint8_t {
    kChoice = 0,  // value = index of the label table (+0x10), `max` labels; left / right +-1
    kVolume = 1,  // 16-step bar: the value rounds up to a multiple of 16 and moves by 16
    kSlider = 2,  // the handicap bar: +-1 (left / right) and +-1 per frame while L1 / R1 is held
    kLaps = 3,    // "1Lap" / "%dLap": as the slider
};

// One row (0x14 bytes; the GLOBAL OPTIONS rows at member 1 0x8004BD3C (8), RACE OPTIONS at 0x8004BE88 (7)).
struct OptionRow {
    uint8_t id = 0;            // +00 OptionId
    uint8_t kind = 0;          // +01 OptionKind
    int16_t min = 0, max = 0;  // +02 / +04: the value is clamped to [min, max - 1]
    int16_t labelX = 0;        // +06 x of the label from the list's centre
    int16_t valueX = 0;        // +08 x of the value column from the list's centre
    uint32_t label = 0;        // +0C guest address of the label (data-title.txd block, 0x801B9630..)
    uint32_t values = 0;       // +10 guest address of the kChoice label pointers
    std::vector<uint32_t> valueLabels; // the pointers of +0x10 (kChoice rows), read from member 1
};

constexpr uint32_t kGlobalOptionRows = 0x8004BD3Cu, kRaceOptionRows = 0x8004BE88u;
constexpr int kGlobalOptionCount = 8, kRaceOptionCount = 7;
std::vector<OptionRow> ReadOptionRows(const GuestImage& ovl1, uint32_t address, int count);

// 0x80017D74 / 0x80017E68 on the career block (byte access as the original; the bool rows store value != 0).
int OptionValue(const career::CareerState& state, uint8_t id);
void SetOptionValue(career::CareerState& state, uint8_t id, int value);
// 0x80017F2C: steps the row's value by `step` (left -1 / right +1) and `fast` (L1 -1 / R1 +1, slider and laps rows
// only); returns true when the stored value changed.
bool StepOption(career::CareerState& state, const OptionRow& row, int step, int fast);
// The per-frame input of the selected row (0x80018574 command 3): left / right from pressed | repeat, L1 / R1 from
// held. Returns the change and whether the original plays the "move" sound (5): the value changed by a left / right.
struct OptionInput {
    bool changed = false, sound = false;
};
OptionInput OptionRowInput(career::CareerState& state, const OptionRow& row, const MenuListPad& pad);

// What the options mean for the rest of the game (the readers of the bytes):
struct GameOptions {
    uint8_t musicVolume = 0xF0;  // +0xB3 -> 0x80080F24: CD input volume (musicVolume * 0x8000 / 0xFF) * 0x8CC >> 12 at a track start
    uint8_t sfxVolume = 0xC0;    // +0xB4 -> car sound master volume (0x8001826C / 0x800785A8 / 0x800784A0)
    bool courseMap = true;       // +0xB1 -> race HUD 0x80029064 draws the map only when set
    uint8_t viewAngle = 1;       // +0xB2 -> race overlay 0x80010298 / 0x80010608: projection distance table 0x8002F370
    uint8_t chaseView = 0;       // +0xB0 -> 0x8003E8E4 view yaw constants (sim::RaceSim viewMode)
    uint8_t cameraPosition = 0;  // +0xAF -> race camera object +0x10E (0x80010000)
    uint8_t replayInfo = 0;      // +0xAE -> camera object +0x107 in replays (0x80010000; sub-mode 3 forces 2)
    bool vibration = true;       // +0x36 == 0 (pad 1) -> 0x800133F0 feeds the actuators only then
};
GameOptions ReadGameOptions(const career::CareerState& state);
// The view-angle table of the race overlay (member 0, s16 at 0x8002F370 + 2 * angle): GTE projection distances.
int16_t ViewAngleProjection(const GuestImage& raceOverlay, uint8_t viewAngle);

// The PC graphics settings (ours; docs/formats/modern_graphics.md). Presentation only: the simulation, the replays
// and every verified state never read them. Vanilla() is the renderer's output before these options existed (the
// `--vanilla` preset: the same frames byte for byte).
struct GraphicsSettings {
    enum : uint8_t { kFrameRateOriginal = 0, kFrameRateDisplay = 1 };
    uint8_t frameRate = kFrameRateDisplay; // Original: each 30 Hz state as it is; Display: every display refresh,
                                           // interpolated between the last two 30 Hz states
    int frameCap = 0;                      // frames per second (Display), 0 = none (the display's refresh)
    bool vsync = true;                     // false: MAILBOX / IMMEDIATE presentation (with a cap: paced by the cap)
    int renderHeight = 0;                  // fixed 16:9 scene: 720/1080/1440/2160; 0 uses window scale
    int renderScale = 100;                 // internal resolution of the 3D scene, percent of the window (50..200)
    int msaa = 1;                          // multisampling of the 3D scene: 1 (off), 2, 4, 8
    bool smoothTextures = false;           // false: the PS1's nearest texel; true: CLUT-aware bilinear
    bool affine = false;                   // true: the PS1's screen-linear texture mapping; false: perspective-correct
    bool maxDetail = false;                // every scenery instance at LOD 0 without the distance cut-off
    int drawDistance = 0;                  // 0: the course's render lists (original); N > 0: + every chunk within N m;
                                           // -1: every chunk of the course
    static GraphicsSettings Vanilla() {
        GraphicsSettings g;
        g.frameRate = kFrameRateOriginal;
        return g;
    }
    static GraphicsSettings Modern() {
        GraphicsSettings g;
        g.msaa = 4;
        g.smoothTextures = true;
        g.maxDetail = true;
        g.drawDistance = 1000;
        return g;
    }
    bool operator==(const GraphicsSettings&) const = default;
    // key=value of settings.txt; false when `key` is not a graphics key.
    bool Parse(const std::string& key, const std::string& value);
    std::string Serialize() const; // the lines of settings.txt
};

// PC-side settings (ours, not in the original): a small key=value text file next to the saves, mirroring the
// career's audio / view options for processes that run without a career (a plain race) and holding the settings
// the US game does not have (speed units: the US build shows mph only; the graphics settings).
struct VrSettings {
    bool stereo = true;        // vr_stereo: the race as a stereo projection layer (0 = M1's mono cinema quad)
    bool multiview = false;    // vr_multiview: one pass with VkRenderingInfo::viewMask instead of one pass per eye
                               // (default off - on an Adreno 740 multiview was slower for few draws)
    int horizonLock = 60;      // vr_horizon_lock: percent of the camera's pitch and roll that is stripped
    int worldScale = 100;      // vr_world_scale: percent - game metres per real metre
    int renderScale = 130;     // vr_render_scale: percent of the runtime's recommended per-eye image (50..200)
    int nearMm = 50;           // vr_near_mm: the near plane in millimetres
    int seatMm[3] = {0, 0, 0}; // vr_seat_x / _y / _z: millimetres along the levelled camera's right / up / back axes
    bool operator==(const VrSettings&) const = default;
    bool Parse(const std::string& key, const std::string& value); // false when `key` is not a VR key
    std::string Serialize() const;
};

struct PcSettings {
    bool metric = true;          // km/h instead of mph
    bool haveCareerOptions = false;
    GameOptions options;
    GraphicsSettings graphics;
    VrSettings vr;
    // The controller settings: the career's two pad blocks (+0x0A / +0x5C, 0x52 bytes each: the KEY CONFIGURATION tables,
    // the vibration byte, the ANALOG calibration) mirrored for races without a career, and ours: an analog pad's
    // triggers as the pedals (platform/input/ps1_pad.h TriggerPedalTable), the motor strength in percent.
    bool havePadBlocks = false;
    std::array<std::array<uint8_t, 0x52>, 2> padBlocks{};
    bool triggerPedals = true;
    int rumbleScale = 100;
    static PcSettings Load(const std::string& path); // defaults when missing / unreadable
    void Save(const std::string& path) const;
};

} // namespace gt2::shell
