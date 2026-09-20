#pragma once
// Replays of GT2 (US Simulation v1.2, EXE SHA-1 3030aa27...; race overlay = GT2.OVL member 0): a replay re-runs the race
// from its start with the recorded pad input of the player cars (the AI cars are simulated again; the physics and the
// AI are deterministic). docs/formats/replay.md has the evidence. This file holds
//   - the input stream object of a player car (0x800163B8 init, 0x800166CC record, 0x80016598 flush, 0x800167D0 end,
//     0x80016428 read): 5-byte frames, run-length coded with a change mask;
//   - the frame <-> pad record conversion of the per-car input 0x80013C90 (the physics' pad record, sim::PadRecord);
//   - the replay file (arcade/demofile_us.gmr, the attract race; saved replays): the race block, the car slots and
//     player 1's stream at their file offsets.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/sim/drivetrain.h"

namespace gt2 {

// One recorded frame (0x80013C90 local sp + 0x18): flags (bit 0 analogue steering, 1 analogue throttle, 2 analogue
// brake, 3 = logical button 0x400), the low byte of the logical buttons (1 left, 2 right, 4 throttle, 8 brake,
// 0x10 handbrake, 0x20 reverse, 0x40 shift up, 0x80 shift down), the steering axis (0..255, 0x80 centre), throttle >> 4
// and brake >> 4 (0..15, analogue only).
struct ReplayFrame {
    uint8_t flags = 0, buttons = 0, steer = 0, throttle = 0, brake = 0;
    bool operator==(const ReplayFrame&) const = default;
};

// The logical pad state 0x80013C90 records from (the pad object 0x800A9528 of player 1): held buttons (+0x8C), the
// analogue flags (+0x9C: bit 0 steering, bit 2 throttle, bit 3 brake) and values (+0x9E steering axis, +0xA2 throttle,
// +0xA4 brake, 0..255).
struct LogicalPad {
    uint32_t buttons = 0;
    uint16_t analog = 0;
    uint16_t steerAxis = 0x80, throttle = 0, brake = 0;
};
// Logical button bits (the default key configuration maps them from the pad; gt2game from the keyboard).
constexpr uint32_t kPadLeft = 0x1, kPadRight = 0x2, kPadThrottle = 0x4, kPadBrake = 0x8, kPadHandbrake = 0x10, kPadReverse = 0x20,
                   kPadShiftUp = 0x40, kPadShiftDown = 0x80, kPadStickCurve = 0x400;

// 0x80013C90, record side: the frame of a logical pad state.
ReplayFrame FrameOfPad(const LogicalPad& pad);
// 0x80013C90, both sides: the physics' pad record of a frame. `pedalTable` = the race overlay's u16[16] at 0x8002F4D4
// (analogue throttle / brake by the 4-bit value).
sim::PadRecord PadOfFrame(const ReplayFrame& frame, std::span<const uint16_t, 16> pedalTable);

// The input stream object (the original's layout; player 1's lives at 0x801D5F84, player 2's at 0x801DA49C):
//   +0x00 s32 frames recorded          +0x04 s32 frames left to play      +0x08 s32 repeat count of the current frame
//   +0x0C s16 ended / full             +0x0E u16 read / write position    +0x10 u16 bytes used
//   +0x12 u16 capacity                 +0x14 u8 header of the current run +0x15..0x18 the current frame (flags,
//   buttons, steering, throttle | brake << 4)                             +0x19 the coded runs
// A run = header (bits 3..6: which of the four frame bytes follow; bits 0..2 = repeat count & 7; bit 7: the count >> 3
// follows in 1..4 bytes, 0x80 / 0xC0 / 0xE0 prefixes), then the changed bytes. A frame is used (count + 1) times.
class ReplayStream {
public:
    static constexpr size_t kHeaderSize = 0x19;
    static constexpr uint16_t kPlayerCapacity = 0x4400; // 0x80012CD4 passes 0x4400 for the players' streams

    explicit ReplayStream(uint16_t capacity = kPlayerCapacity);
    // An image of the object as the original holds it (at least kHeaderSize bytes; the data area grows to `capacity`).
    static ReplayStream FromBytes(std::span<const uint8_t> bytes);

    void Init(bool playback, uint16_t capacity);  // 0x800163B8
    void Record(const ReplayFrame& frame);        // 0x800166CC
    void End(bool noFlush);                       // 0x800167D0
    // 0x80016428: the next frame into `frame` (left unchanged when the stream has ended); the frame after the last one
    // ends the stream (and is not written either).
    void Read(ReplayFrame& frame);

    int32_t Frames() const { return I32(0x00); }
    int32_t FramesLeft() const { return I32(0x04); }
    bool Ended() const { return I16(0x0C) != 0; }
    uint16_t Used() const { return U16(0x10); }
    uint16_t Capacity() const { return U16(0x12); }
    // The object's bytes (header + data area) - comparable with the original's RAM.
    const std::vector<uint8_t>& Bytes() const { return bytes_; }

private:
    void Flush(); // 0x80016598
    int32_t I32(size_t o) const;
    int16_t I16(size_t o) const;
    uint16_t U16(size_t o) const;
    void Put32(size_t o, int32_t v);
    void Put16(size_t o, uint16_t v);
    uint8_t& Data(size_t pos);
    std::vector<uint8_t> bytes_;
};

// ---------------------------------------------------------------- the replay file (.gmr)

// File offsets of the known sections (arcade/demofile_us.gmr against the attract race's RAM, work/re/race_demo): the
// file is a memory-card save ("SC" header). Race block 0x801D585C..0x801D58B8 at 0x1580, the six car slots
// 0x801D58B8 (0xD0 each: u32 car id, u32, CarConfig, u8 entry[4] {?, grid slot, kind, transmission}, char name[64])
// at 0x15DC, player 1's stream 0x801D5F84 at 0x1DC8 (the RAM image of the object after the race: ended, position 0).
constexpr size_t kReplayRaceBlockOffset = 0x1580, kReplayRaceBlockSize = 0x5C;
constexpr size_t kReplayCarsOffset = 0x15DC, kReplayCarStride = 0xD0, kReplayCars = 6;
constexpr size_t kReplayStreamOffset = 0x1DC8;
constexpr size_t kReplayFileSize = 0xE000;

struct ReplayEntry {
    uint32_t carId = 0;
    std::array<uint8_t, 0xD0> slot{}; // the whole car slot (CarConfig at +8, entry bytes at +0x8C)
    uint8_t gridSlot() const { return slot[0x8D]; }
    uint8_t kind() const { return slot[0x8E]; }
    uint8_t transmission() const { return slot[0x8F]; }
    uint8_t paint() const { return slot[4]; }  // the paint code (.carinfoa paint id)
    std::string name() const {                // + 0x90: the car's display name
        std::string n;
        for (size_t i = 0x90; i < slot.size() && slot[i] != 0; i++) n.push_back(char(slot[i]));
        return n;
    }
};

struct ReplayFile {
    // 0x801D585C.. (+1 mode flag 0x801D585D, +4 mode flag 0x801D5860, +8 frame rate mode, +9 mode flag 0x801D5865, +0xA game
    // mode, +0xB / +0xC licence / test, +0xD countdown, +0xF laps, +0x10 event name, +0x20 course name, +0x40 course file id,
    // +0x44 sponsor category, +0x54 sponsor seed, +0x58 dirt level of player 1, +0x5A car count)
    std::array<uint8_t, kReplayRaceBlockSize> raceBlock{};
    std::vector<ReplayEntry> cars;                         // the slots with a car (count = race block + 0x5A)
    std::vector<uint8_t> stream;                           // player 1's stream object (header + data)
    // Game mode 0 (the 2 player Battle): player 2's stream object (0x801DA49C; the payload's second player, replay_card.h),
    // played by the car of pad slot 3 (0x80014030 -> 0x80013C90 with the pad object 0x800A95D8). Empty in the other modes.
    std::vector<uint8_t> stream2;

    uint8_t GameMode() const { return raceBlock[0xA]; }
    uint8_t Laps() const { return raceBlock[0xF]; }
    uint8_t Countdown() const { return raceBlock[0xD]; }
    uint8_t CarCount() const { return raceBlock[0x5A]; }
    uint32_t CourseFileId() const;
    std::string CourseName() const;
    std::string EventName() const;
    uint32_t SponsorSeed() const;
};

// Parses a .gmr: a replay file of the card format (replay_card.h: its first replay) or, without a valid directory, the
// known sections at the offsets above (throws on a short file / a car count above 6).
ReplayFile ParseReplayFile(std::span<const uint8_t> bytes);
// Writes a replay as a replay file of the original's format (replay_card.h) holding it as entry 0 from sector 0 (so the
// offsets above hold): the save header of `base` when it has one, a new directory, the payload (0x80069948 layout; the
// parameter / results records zero).
std::vector<uint8_t> WriteReplayFile(const ReplayFile& replay, std::span<const uint8_t> base);

} // namespace gt2
