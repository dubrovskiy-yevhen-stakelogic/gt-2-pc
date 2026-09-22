#include "gt2formats/replay.h"

#include "gt2formats/replay_card.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace gt2 {

// ---------------------------------------------------------------- frames (0x80013C90)

ReplayFrame FrameOfPad(const LogicalPad& pad) {
    if (pad.wheel) {
        const uint16_t steer = std::min<uint16_t>(pad.steerAxis, 4095), throttle = std::min<uint16_t>(pad.throttle, 1023), brake = std::min<uint16_t>(pad.brake, 1023);
        uint8_t buttons = uint8_t((pad.buttons & 0xd0) | ((pad.clutch & 1) << 5) | (pad.wheelGear & 15));
        if (pad.ignoreShiftSpeed && pad.wheelGear >= 2 && pad.wheelGear <= 9) {
            // Codes 11/12 carry a direct unrestricted gear; bits 6/7 carry
            // the target's low bits instead of unused direct-mode paddles.
            const int target = pad.wheelGear - 2;
            buttons = uint8_t((buttons & 0x30) | (11 + target / 4) | ((target % 4) << 6));
        } else if (pad.ignoreShiftSpeed && pad.wheelGear == 15) buttons = uint8_t((buttons & 0xf0) | 13);
        return {uint8_t(0x80 | (pad.clutch >> 1)), buttons,
            uint8_t(steer >> 4), uint8_t(throttle >> 6), uint8_t(brake >> 6),
            uint16_t((steer & 15) | ((throttle & 63) << 4) | ((brake & 63) << 10))};
    }
    uint16_t flags = 0, steer = 0, throttle = 0, brake = 0;
    if (pad.analog & 1) {
        flags = 1;
        steer = pad.steerAxis;
    }
    if (pad.analog & 4) {
        flags |= 2;
        throttle = uint16_t(pad.throttle >> 4);
    }
    if (pad.analog & 8) {
        flags |= 4;
        brake = uint16_t(pad.brake >> 4);
    }
    if (pad.buttons & kPadStickCurve) flags |= 8;
    ReplayFrame f;
    f.flags = uint8_t(flags);
    f.buttons = uint8_t(pad.buttons);
    f.steer = uint8_t(steer);
    f.throttle = uint8_t(throttle);
    f.brake = uint8_t(brake);
    return f;
}

sim::PadRecord PadOfFrame(const ReplayFrame& frame, std::span<const uint16_t, 16> pedalTable) {
    sim::PadRecord pad{};
    const uint32_t flags = frame.flags, buttons = frame.buttons;
    if (flags & 0x80) {
        const uint8_t clutch = uint8_t(((flags & 127) << 1) | ((buttons >> 5) & 1));
        const int steering = (frame.steer << 4) | (frame.wheelFine & 15);
        pad.flags = uint16_t(0x107 | ((clutch & 15) << 4));
        pad.reserved = uint8_t((clutch & 0xf0) | (buttons & 15));
        pad.steer = int16_t(steering < 2048 ? (2048 - steering) * 2 : -(steering - 2048) * 4096 / 2047);
        pad.throttle = uint16_t((((frame.throttle & 15) << 6) | ((frame.wheelFine >> 4) & 63)) * 4096 / 1023);
        pad.brake = uint16_t((((frame.brake & 15) << 6) | ((frame.wheelFine >> 10) & 63)) * 4096 / 1023);
        pad.handbrake = uint8_t((buttons & 0x10) != 0);
        pad.shift = int8_t(int((buttons & 0x40) != 0) - int((buttons & 0x80) != 0));
        const int mode = buttons & 15;
        if (mode == 11 || mode == 12) {
            pad.flags |= sim::kWheelIgnoreShiftSpeed;
            pad.reserved = uint8_t((clutch & 0xf0) | (2 + (mode - 11) * 4 + (buttons >> 6)));
            pad.shift = 0;
        } else if (mode == 13) {
            pad.flags |= sim::kWheelIgnoreShiftSpeed;
            pad.reserved = uint8_t((clutch & 0xf0) | 15);
        }
        return pad;
    }
    pad.flags = uint16_t(flags);
    uint16_t steer;
    if ((flags & 1) == 0) {
        steer = uint16_t((buttons & 1) * 2);
        if (buttons & 2) steer = uint16_t(steer - 2);
    } else {
        steer = uint16_t((0x80u - frame.steer) * 0x20u);
    }
    pad.steer = int16_t(steer);
    pad.throttle = (flags & 2) == 0 ? uint16_t((buttons & 4) != 0) : pedalTable[frame.throttle & 15];
    pad.brake = (flags & 4) == 0 ? uint16_t((buttons & 8) != 0) : pedalTable[frame.brake & 15];
    pad.handbrake = uint8_t((buttons & 0x10) != 0);
    pad.reverse = uint8_t((buttons & 0x20) != 0);
    int8_t shift = int8_t((buttons & 0x40) != 0);
    if (buttons & 0x80) shift = int8_t(shift - 1);
    pad.shift = shift;
    return pad;
}

// ---------------------------------------------------------------- the stream object

ReplayStream::ReplayStream(uint16_t capacity) : bytes_(kHeaderSize + capacity, 0) { Init(false, capacity); }

ReplayStream ReplayStream::FromBytes(std::span<const uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) throw std::runtime_error("replay stream: shorter than its header");
    ReplayStream s(0);
    s.bytes_.assign(bytes.begin(), bytes.end());
    const size_t size = kHeaderSize + s.Capacity();
    if (s.bytes_.size() < size) s.bytes_.resize(size, 0);
    if (s.Capacity() >= 2 && (s.bytes_[0x15] & 0x80)) s.wheelFine_ = uint16_t(s.bytes_[size - 2] | (s.bytes_[size - 1] << 8));
    return s;
}

int32_t ReplayStream::I32(size_t o) const { int32_t v; std::memcpy(&v, bytes_.data() + o, 4); return v; }
int16_t ReplayStream::I16(size_t o) const { int16_t v; std::memcpy(&v, bytes_.data() + o, 2); return v; }
uint16_t ReplayStream::U16(size_t o) const { uint16_t v; std::memcpy(&v, bytes_.data() + o, 2); return v; }
void ReplayStream::Put32(size_t o, int32_t v) { std::memcpy(bytes_.data() + o, &v, 4); }
void ReplayStream::Put16(size_t o, uint16_t v) { std::memcpy(bytes_.data() + o, &v, 2); }
uint8_t& ReplayStream::Data(size_t pos) {
    // The original writes past its object near a full stream (up to 10 bytes after the full test); ours grows instead.
    if (kHeaderSize + pos >= bytes_.size()) bytes_.resize(kHeaderSize + pos + 16, 0);
    return bytes_[kHeaderSize + pos];
}

void ReplayStream::Init(bool playback, uint16_t capacity) { // 0x800163B8(object, playback, capacity)
    wheelFine_ = 0;
    if (bytes_.size() < kHeaderSize + capacity) bytes_.resize(kHeaderSize + capacity, 0);
    if (!playback) {
        std::fill(bytes_.begin(), bytes_.begin() + 0x1C, uint8_t(0)); // memset(object, 0, 0x1C): the first 3 data bytes too
    } else {
        std::fill(bytes_.begin() + 0x14, bytes_.begin() + 0x19, uint8_t(0));
        Put16(0x0C, 0);
        Put16(0x0E, 0);
        Put32(0x04, I32(0x00));
    }
    Put16(0x12, capacity);
    Put32(0x08, -1);
}

void ReplayStream::Record(const ReplayFrame& frame) { // 0x800166CC
    if (I16(0x0C) != 0) return;
    uint8_t header = 0x78;
    Put32(0x00, I32(0x00) + 1);
    const uint8_t b3 = uint8_t(frame.throttle | frame.brake << 4);
    if (I32(0x08) >= 0) {
        header = uint8_t((frame.flags != bytes_[0x15]) << 3);
        if (frame.buttons != bytes_[0x16]) header |= 0x10;
        if (frame.steer != bytes_[0x17]) header |= 0x20;
        if (b3 != bytes_[0x18]) header |= 0x40;
        if ((frame.flags & 0x80) && (!(bytes_[0x15] & 0x80) || frame.wheelFine != wheelFine_)) header |= 0x40;
        if (header == 0) {
            Put32(0x08, I32(0x08) + 1);
            return;
        }
        Flush();
    }
    bytes_[0x14] = header;
    bytes_[0x15] = frame.flags;
    bytes_[0x16] = frame.buttons;
    bytes_[0x17] = frame.steer;
    bytes_[0x18] = b3;
    CacheWheelFine(frame.wheelFine);
    Put32(0x08, 0);
}

void ReplayStream::CacheWheelFine(uint16_t value) {
    wheelFine_ = value;
    // GhostSession reconstructs a stream from bytes every tick. Reserve the
    // final two capacity bytes for the native cache; the 17-byte full margin
    // keeps encoded runs below them. Original streams never write this cache.
    if ((bytes_[0x15] & 0x80) && Capacity() >= 2) {
        const size_t end = kHeaderSize + Capacity();
        bytes_[end - 2] = uint8_t(value); bytes_[end - 1] = uint8_t(value >> 8);
    }
}

void ReplayStream::Flush() { // 0x80016598
    const int32_t run = I32(0x08);
    size_t p = U16(0x0E);
    uint32_t header = uint32_t(bytes_[0x14] & 0xF8) | (uint32_t(run) & 7);
    if (run > 7) header |= 0x80;
    Data(p++) = uint8_t(header);
    if (header & 0x08) Data(p++) = bytes_[0x15];
    if (header & 0x10) Data(p++) = bytes_[0x16];
    if (header & 0x20) Data(p++) = bytes_[0x17];
    if (header & 0x40) {
        Data(p++) = bytes_[0x18];
        if (bytes_[0x15] & 0x80) { Data(p++) = uint8_t(wheelFine_); Data(p++) = uint8_t(wheelFine_ >> 8); }
    }
    const int32_t count = run >> 3;
    if (header & 0x80) {
        uint8_t prefix3 = 0, prefix2 = 0;
        bool three = false, two = false;
        if (count < 0x200000) {
            if (count > 0x3FFF) {
                prefix3 = 0xC0;
                three = true;
            } else {
                prefix2 = 0x80;
                two = count > 0x7F;
            }
        } else {
            Data(p++) = uint8_t(uint32_t(run) >> 27 | 0xE0);
            three = true;
        }
        if (three) {
            Data(p++) = uint8_t(uint32_t(run) >> 19 | prefix3);
            two = true;
        }
        if (two) Data(p++) = uint8_t(uint32_t(count) >> 8 | prefix2);
        Data(p++) = uint8_t(count);
    }
    Put16(0x0E, uint16_t(p));
    Put16(0x10, uint16_t(p));
    if (int32_t(U16(0x12)) - 0x11 <= int32_t(p)) Put16(0x0C, 1);
}

void ReplayStream::End(bool noFlush) { // 0x800167D0
    const int16_t was = I16(0x0C);
    Put16(0x0C, 1);
    if (was == 0 && !noFlush && I32(0x08) >= 0) Flush();
}

void ReplayStream::Read(ReplayFrame& frame) { // 0x80016428
    const int32_t left = I32(0x04) - 1;
    if (I16(0x0C) != 0) return;
    Put32(0x04, left);
    if (left == 0) {
        Put16(0x0C, 1);
        Put32(0x08, 0);
        return;
    }
    uint8_t b0 = bytes_[0x15], b1 = bytes_[0x16], b2 = bytes_[0x17];
    uint32_t b3 = bytes_[0x18];
    uint32_t run = uint32_t(I32(0x08) - 1);
    if (I32(0x08) < 1) {
        size_t p = U16(0x0E);
        const uint8_t header = Data(p++);
        bytes_[0x14] = header;
        if (header & 0x08) bytes_[0x15] = b0 = Data(p++);
        if (header & 0x10) bytes_[0x16] = b1 = Data(p++);
        if (header & 0x20) bytes_[0x17] = b2 = Data(p++);
        if (header & 0x40) {
            bytes_[0x18] = uint8_t(b3 = Data(p++));
            if (b0 & 0x80) { const uint8_t low = Data(p++); CacheWheelFine(uint16_t(low | (uint16_t(Data(p++)) << 8))); }
        }
        run = 0;
        if (header & 0x80) {
            const uint32_t c0 = Data(p++);
            run = c0;
            if (c0 & 0x80) {
                const uint32_t c1 = Data(p++);
                const uint32_t high = (c0 & 0x7F) << 8;
                run = high | c1;
                if (high & 0x4000) {
                    const uint32_t c2 = Data(p++);
                    run = ((high & 0x3FFF) | c1) << 8 | c2;
                    if (high & 0x2000) {
                        const uint32_t c3 = Data(p++);
                        run = (((high & 0x1FFF) | c1) << 8 | c2) << 8 | c3;
                    }
                }
            }
            run <<= 3;
        }
        run |= header & 7u;
        Put16(0x0E, uint16_t(p));
    }
    Put32(0x08, int32_t(run));
    frame.throttle = uint8_t(b3 & 0xF);
    frame.flags = b0;
    frame.buttons = b1;
    frame.steer = b2;
    frame.brake = uint8_t(b3 >> 4);
    frame.wheelFine = (b0 & 0x80) ? wheelFine_ : 0;
}

// ---------------------------------------------------------------- the file

uint32_t ReplayFile::CourseFileId() const {
    uint32_t v;
    std::memcpy(&v, raceBlock.data() + 0x40, 4);
    return v;
}

uint32_t ReplayFile::SponsorSeed() const {
    uint32_t v;
    std::memcpy(&v, raceBlock.data() + 0x54, 4);
    return v;
}

namespace {
std::string CString(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n && p[i] != 0; i++) s.push_back(char(p[i]));
    return s;
}
} // namespace

std::string ReplayFile::CourseName() const { return CString(raceBlock.data() + 0x20, 0x20); }
std::string ReplayFile::EventName() const { return CString(raceBlock.data() + 0x10, 0x10); }

ReplayFile ParseReplayFile(std::span<const uint8_t> bytes) {
    // A replay file with a valid directory (replay_card.h): its first replay. Entry 0 of a file written from sector 0 is
    // the legacy layout below byte for byte.
    if (bytes.size() >= kReplayDataStart && bytes[0] == 'S' && bytes[1] == 'C') {
        const ReplayCardFile file = ReplayCardFile::FromBytes(bytes);
        if (file.Valid() && file.Count() > 0) return LoadReplayEntry(file, 0);
    }
    if (bytes.size() < kReplayStreamOffset + ReplayStream::kHeaderSize) throw std::runtime_error("replay file: too short");
    ReplayFile r;
    std::memcpy(r.raceBlock.data(), bytes.data() + kReplayRaceBlockOffset, kReplayRaceBlockSize);
    if (r.CarCount() == 0 || r.CarCount() > kReplayCars) throw std::runtime_error("replay file: car count " + std::to_string(r.CarCount()));
    for (size_t i = 0; i < r.CarCount(); i++) {
        ReplayEntry e;
        std::memcpy(e.slot.data(), bytes.data() + kReplayCarsOffset + i * kReplayCarStride, kReplayCarStride);
        std::memcpy(&e.carId, e.slot.data(), 4);
        r.cars.push_back(e);
    }
    uint16_t capacity;
    std::memcpy(&capacity, bytes.data() + kReplayStreamOffset + 0x12, 2);
    const size_t end = std::min(bytes.size(), kReplayStreamOffset + ReplayStream::kHeaderSize + capacity);
    r.stream.assign(bytes.begin() + std::ptrdiff_t(kReplayStreamOffset), bytes.begin() + std::ptrdiff_t(end));
    return r;
}

std::vector<uint8_t> WriteReplayFile(const ReplayFile& replay, std::span<const uint8_t> base) {
    // A replay file of the original's format (replay_card.h) with this replay as entry 0 from sector 0: the header of
    // `base` when it has one ("SC", else a bare "SC" header without title and icons), the fewest blocks (>= 3, the
    // original's own minimum on "Select Number of Blocks") that hold the payload.
    std::vector<uint8_t> header(0x200, 0);
    if (base.size() >= 0x200 && base[0] == 'S' && base[1] == 'C') std::memcpy(header.data(), base.data(), 0x200);
    const ReplayPayload payload = PayloadOfReplay(replay, {}, {});
    const std::vector<uint8_t> packed = PackReplayPayload(payload);
    const size_t sectors = (packed.size() + kReplaySectorSize - 1) / kReplaySectorSize;
    const int blocks = std::max(3, int((kReplayDataStart + sectors * kReplaySectorSize + kReplayCardBlock - 1) / kReplayCardBlock));
    ReplayCardFile file = ReplayCardFile::Create(blocks, header);
    if (!file.Store(-1, ReplayDescription(payload), packed)) throw std::runtime_error("replay file: the replay does not fit 15 blocks");
    return file.Bytes();
}

} // namespace gt2
