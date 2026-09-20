#pragma once
// The PS1 controller as Gran Turismo 2 sees it, ported from US Simulation v1.2 (SCUS_944.88, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; race overlay = GT2.OVL member 0). Evidence: our objdump of
// work/re/license_race/ram.bin and work/re/title/ram.bin; docs/formats/pad_input.md has the details.
//
//   reader 0x8007FC30 (per field, VBlank):  the libpad receive buffer (0x801F0C98 + port * 0x22: status, id, ~buttons,
//     4 analogue bytes) -> the pad object's handler by controller type (the race's handler table 0x800A6F5C):
//       digital 4     0x80083818: buttons remapped (0x80083A4C, pairs at 0x800A6F3C) -> button tracker 0x800838B4
//       analog 5 / 7  0x800858CC: + axes 0..3 = 0x80085890(RX, RY, LX, LY): 0x80 dead zone 91..164
//       neGcon 2      0x800832C0: + I / II / L as buttons (0x80083A88, 0x800A6ECC) + the calibration 0x800A6EEC + port * 20
//                     (twist 0x800831BC, pedals 0x80083250; the 20-byte "analog configuration" of the pad block)
//     then the actuator post-handler 0x8008371C (DualShock motors) and the vibration timer (object + 0x60).
//   race logical pad 0x80014BB4 (per race frame, from BeginFrame 0x80015B64): the tracker snapshot (0x80083998) and the
//     axes through the 11-entry key table of the controller type (pad block = career + 0x0A / + 0x5C, pointer at object
//     + 0x64): logical buttons + 0x8C.., analogue flags + 0x9C, values + 0x9E + 2 * function.
//   vibration source: the tail of 0x800133F0 (per car and race frame) writes object + 0x5A / + 0x5C / + 0x5E / + 0x60.
//
// The functions work on the objects' bytes in the original layout so that tools/gt2verify can run them on a RAM image.
// Tables of the executable are read from the disc (PadTables::Load); only the hardware bit layout is spelled out here.
#include <array>
#include <cstdint>

#include "gt2formats/overlay_data.h"

namespace gt2::input {

// PS1 controller buttons: the libpad receive buffer bytes 2..3 inverted (the reader's `~u16`), i.e. set = pressed.
namespace ps1 {
constexpr uint16_t kSelect = 0x0001, kL3 = 0x0002, kR3 = 0x0004, kStart = 0x0008, kUp = 0x0010, kRight = 0x0020, kDown = 0x0040,
                   kLeft = 0x0080, kL2 = 0x0100, kR2 = 0x0200, kL1 = 0x0400, kR1 = 0x0800, kTriangle = 0x1000, kCircle = 0x2000,
                   kCross = 0x4000, kSquare = 0x8000;
}

// Controller types: receive buffer id byte >> 4 (0x41 digital, 0x73 DualShock analog, 0x23 neGcon, 0x53 analog joystick,
// 0xE3 Jogcon). kTypeNone: no controller (buffer status != 0).
enum PadType : uint8_t { kTypeNone = 0, kTypeMouse = 1, kTypeNegcon = 2, kTypeDigital = 4, kTypeAnalogJoystick = 5, kTypeAnalog = 7, kTypeJogcon = 14 };

// One field of a controller as the reader receives it.
struct Ps1PadFrame {
    uint8_t type = kTypeNone;
    uint16_t buttons = 0;                             // ps1::k*, set = pressed
    std::array<uint8_t, 4> analog{0x80, 0x80, 0x80, 0x80}; // buffer bytes 4..7: DualShock RX, RY, LX, LY (0x80 centre, 0 = left / up);
                                                      // neGcon twist (0x80 centre), I, II, L (0 released)
    // Ours (no PS1 controller has them; DualShock 2 pressure buttons are not used by GT2): analogue trigger travel of a
    // PC pad (0 released .. 255 full), offered to the key tables as extra axes 4 (R2) and 5 (L2) - see kAxisR2Pressure.
    bool pressure = false;
    uint8_t pressureR2 = 0, pressureL2 = 0;
};

// Generic button bits the handlers produce (0x80083A4C with the pairs at 0x800A6F3C): what menus and the key tables see.
namespace generic {
constexpr uint32_t kUp = 1u << 0, kDown = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3, kL1 = 1u << 4, kL2 = 1u << 5, kL3 = 1u << 6,
                   kTriangle = 1u << 8, kCross = 1u << 9, kSquare = 1u << 10, kCircle = 1u << 11, kR1 = 1u << 12, kR2 = 1u << 13,
                   kR3 = 1u << 14, kStart = 1u << 16, kSelect = 1u << 17;
}

// Tables of the executable the handlers use (read from the disc image, never stored in the repository).
struct PadTables {
    static constexpr uint32_t kRemap = 0x800A6F3Cu;          // 16 pairs {raw bit, generic bit}
    static constexpr uint32_t kNegconButtons = 0x800A6ECCu;  // 3 x {u16 threshold, u8 axis, u8 generic bit}
    static constexpr uint32_t kCalibrationDefault = 0x800A6ED8u; // 20 bytes (the new game's pad configuration)
    static constexpr uint32_t kCalibration = 0x800A6EECu;    // + port * 20: the live copy (0x800117B4 / 0x8006A348)
    static constexpr uint32_t kRepeat = 0x800A6FBCu;         // 2 bytes: the tracker's repeat reload / first delay
    std::array<std::array<uint8_t, 2>, 16> remap{};
    struct NegconButton { uint16_t threshold = 0; uint8_t axis = 0, bit = 0; };
    std::array<NegconButton, 3> negconButtons{};
    std::array<uint8_t, 2> repeat{};
    static PadTables Load(const GuestImage& exe);
};

// ---- the pad object (0xB0 bytes: player 1 at 0x800A9528, player 2 at 0x800A95D8 in a race)
namespace pad_object {
constexpr uint32_t kSize = 0xB0;
constexpr uint32_t kPort = 0x00;        // s16
constexpr uint32_t kType = 0x02;        // u8 controller type of the last read
constexpr uint32_t kConnected = 0x03;   // u8
constexpr uint32_t kHandlers = 0x04;    // u32 handler table (race: 0x800A6F5C)
constexpr uint32_t kTracker = 0x0C;     // button tracker (0x3A bytes, below)
constexpr uint32_t kAxisMask = 0x48;    // u16 valid axes
constexpr uint32_t kAxes = 0x4A;        // u16[8]
constexpr uint32_t kVibration = 0x5A;   // s16 x3: +0x5A single-motor strength, +0x5C large motor << 4, +0x5E small motor << 4
constexpr uint32_t kVibrationTimer = 0x60; // s16 polls left
constexpr uint32_t kPadBlock = 0x64;    // u32 the career's pad block (0x52 bytes: key tables, vibration byte, calibration)
constexpr uint32_t kSnapshot = 0x68;    // u32[4] tracker snapshot, then +0x78 axis mask, +0x7A axes[8] (the +0x48..+0x59 copy)
constexpr uint32_t kLogical = 0x8C;     // u32[4] logical held / pressed / released / repeat
constexpr uint32_t kAnalogFlags = 0x9C; // u16: bit f = function f takes an axis
constexpr uint32_t kValues = 0x9E;      // u16 per function f at + 2f (only the axis functions are written)
} // namespace pad_object

// ---- the button tracker (pad object + 0x0C): the generic bits of every poll, accumulated until the reader takes them
namespace tracker {
constexpr uint32_t kLast = 0x00, kCurrent = 0x04, kHeld = 0x08, kPressed = 0x0C, kReleased = 0x10, kRepeat = 0x14, kRepeatReload = 0x18,
                   kRepeatFirst = 0x19, kCounters = 0x1A; // u8[32]
constexpr uint32_t kSize = 0x3A;
}

// The pad block of the career (0x52 bytes; pad 1 at career + 0x0A, pad 2 at + 0x5C).
namespace pad_block {
constexpr uint32_t kSize = 0x52;
constexpr uint32_t kTables = 0x00;      // 4 x 11 bytes: digital, analog, neGcon, Jogcon (0x80014BB4 picks by type)
constexpr uint32_t kTableSize = 11;
constexpr uint32_t kVibrationOff = 0x2C; // u8: 0 = vibration on (option 14)
constexpr uint32_t kKeyPages = 0x2D;    // 3 x 4 bytes: the key configuration page state per table (0x80019388)
constexpr uint32_t kCalibration = 0x3E; // 20 bytes: neGcon twist {min, dead lo, dead hi, max} + I / II / L {min, max} (u16 each)
}

// Key table entries (0x80014BB4): < 0x80 a generic button bit; 0x80 + axis = the axis value as is; 0xA0 + axis = 127 - v,
// 0xC0 + axis = v - 128, 0xE0 + axis = |v - 128| style, each of the last three clamped to 0..127 and doubled.
// Functions (logical bit 1 << f): 0 left, 1 right, 2 accelerate, 3 brake, 4 handbrake, 5 reverse, 6 shift up,
// 7 shift down, 8 view change (camera 0x100), 9 look back (0x200), 10 steering curve (0x400).
constexpr int kFunctions = 11;
// Ours: the extra axes of a pad with analogue triggers (Ps1PadFrame::pressure), raw 0..255 like the neGcon pedals.
constexpr uint8_t kAxisR2Pressure = 4, kAxisL2Pressure = 5;

// 0x80083A4C: raw buttons through the remap pairs.
uint32_t RemapButtons(uint32_t raw, const PadTables& t);
// 0x80083A88: neGcon analogue bytes as buttons (threshold rows).
uint32_t NegconButtons(const std::array<uint16_t, 4>& analog, const PadTables& t);
// 0x800838B4 on the tracker at `tr`.
void TrackButtons(uint8_t* tr, uint32_t bits);
// 0x80083868 (without the clear, done by the caller): a fresh tracker (last = -1 ... repeat bytes from the table).
void InitTracker(uint8_t* tr, const PadTables& t);
// 0x80085890: a DualShock stick byte -> 0..127 / 128 (dead zone) / 128..254.
uint16_t DualShockAxis(uint16_t raw);
// 0x800831BC / 0x80083250: the neGcon calibration (20 bytes, u16 fields read as the original does).
uint16_t NegconTwist(const uint8_t* calibration, uint16_t raw);
uint16_t NegconPedal(const uint8_t* calibration, int axis, uint16_t raw);

// The race's handlers (table 0x800A6F5C) on the object: digital 0x80083818, analog 0x800858CC (types 5 / 7), neGcon
// 0x800832C0 (`calibration` = the port's 20 bytes). `analog` = the reader's words of the buffer bytes 4..7.
void DigitalHandler(uint8_t* obj, uint32_t raw, const PadTables& t);
void AnalogHandler(uint8_t* obj, uint32_t raw, const std::array<uint16_t, 4>& analog, const PadTables& t);
void NegconHandler(uint8_t* obj, uint32_t raw, const std::array<uint16_t, 4>& analog, const PadTables& t, const uint8_t* calibration);
// 0x8007FC30 with the race's handler table: one field of `frame` into the pad object `obj` (tracker, type, axes, the
// actuators of this poll and the vibration timer). `calibration` = the port's 20 bytes (neGcon only). Returns the actuator
// bytes sent this poll (act[0] small motor, act[1] large motor) as 0x8008371C computes them for a stable DualShock.
struct Actuators {
    uint8_t smallMotor = 0, largeMotor = 0;
    bool any() const { return (smallMotor & 1) != 0 || largeMotor != 0; }
};
Actuators PollPad(uint8_t* obj, const Ps1PadFrame& frame, const PadTables& t, const uint8_t* calibration, uint8_t actuatorCount);
// 0x8008371C, the value part: `actuatorCount` = 0x801F0C89 + port * 8 (PadInfoAct(port, -1) in the stable state, 1 for a
// controller without the extended protocol, 0 without a controller).
Actuators ActuatorsOf(const uint8_t* obj, uint8_t actuatorCount);
// 0x80083998 + 0x80083958: the tracker's accumulated words into `out` (4 words) and cleared.
void SnapshotTracker(uint8_t* tr, uint8_t* out);
// 0x80014BB4 on the object; `tables` = the 4 x 11 key tables (pad block + 0; the original reads them through + 0x64).
// Writes + 0x68 .. + 0x9E + 2f like the original (an axis entry for a function above 8 writes past the object).
void BuildRaceLogical(uint8_t* obj, const uint8_t* tables);
// The table index 0x80014BB4 uses for a controller type (5 / 7 -> 1, 2 -> 2, 14 -> 3, else 0).
int TableOfType(uint8_t type);
// The tail of 0x800133F0 for one car of a race frame: `obj` = the car's pad object (car + 0x0C selects it).
struct VibrationSource {
    bool demo = false;          // 0x800A951C
    int16_t padSlot = 0;        // car + 0x18: 2 / 3 = a live player
    uint8_t vibrationOff = 0;   // pad block + 0x2C (0x801C9916 + pad * 0x52)
    int32_t finishTime = 0;     // car + 0x24
    int16_t word760 = 0;        // body + 0x760 (car + 0x78C)
    uint8_t loadLevel = 0;      // body + 0x762 (the load / skid / impact level of ground.cpp)
    uint8_t impactFlag = 0;     // body + 0x763 (a wall impact in progress)
};
void FeedVibration(uint8_t* obj, const VibrationSource& s);

// Ours (not in the original): the key table of an analog pad whose triggers drive the pedals. Accelerate / brake take the
// R2 / L2 travel (kAxisR2Pressure / kAxisL2Pressure, raw like the neGcon pedals); a function that was on R2 / L2 moves
// to the button that accelerate / brake had.
std::array<uint8_t, 11> TriggerPedalTable(const uint8_t* analogTable);

} // namespace gt2::input
