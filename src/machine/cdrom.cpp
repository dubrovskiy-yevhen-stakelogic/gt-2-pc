#include "machine/cdrom.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace gt2 {
namespace {

// Delays in guest instructions (the machine runs ~16.9 M instructions per second).
constexpr uint64_t kAckDelay = 12'000;
constexpr uint64_t kShortDelay = 40'000;
constexpr uint64_t kLongDelay = 300'000;
constexpr uint64_t kSectorPeriodSingle = 225'792; // 1/75 s
constexpr uint64_t kSectorPeriodDouble = 112'896; // 1/150 s

uint8_t FromBcd(uint8_t v) { return uint8_t((v >> 4) * 10 + (v & 15)); }

} // namespace

uint8_t Cdrom::Read(uint32_t reg) {
    switch (reg) {
    case 0: {
        uint8_t status = index_;
        if (params_.empty()) status |= 0x08;
        if (!responseFifo_.empty()) status |= 0x20;
        if (dataPos_ < dataFifo_.size()) status |= 0x40;
        return status;
    }
    case 1: {
        if (responseFifo_.empty()) return 0;
        uint8_t v = responseFifo_.front();
        responseFifo_.pop_front();
        return v;
    }
    case 2: return dataPos_ < dataFifo_.size() ? dataFifo_[dataPos_++] : 0;
    default: return uint8_t(((index_ & 1) ? intFlag_ : intEnable_) | 0xE0);
    }
}

void Cdrom::Write(uint32_t reg, uint8_t value, uint64_t now) {
    if (accessLog.size() < 4096) accessLog.push_back({now, 'W', uint8_t(reg), index_, value});
    if (reg == 0) { index_ = value & 3; return; }
    switch ((reg << 4) | index_) {
    case 0x10: Execute(value, now); params_.clear(); break;
    case 0x20: params_.push_back(value); break;
    case 0x21: intEnable_ = value & 0x1F; break;
    case 0x30: // request register: BFRD moves the sector buffer into the data FIFO
        if (value & 0x80) {
            if (dataPos_ >= dataFifo_.size()) { dataFifo_ = sectorBuffer_; dataPos_ = 0; }
        } else {
            dataFifo_.clear();
            dataPos_ = 0;
        }
        break;
    case 0x31:
        intFlag_ &= ~(value & 0x1F);
        if (value & 0x40) params_.clear();
        if (intFlag_ == 0) responseFifo_.clear();
        break;
    default: break; // audio volume / sound map registers: not needed yet
    }
}

bool Cdrom::Tick(uint64_t now) {
    if (intFlag_ || pending_.empty() || pending_.front().due > now) return false;
    Response r = std::move(pending_.front());
    pending_.pop_front();
    if (r.sector) {
        if (!reading_) return false;
        // Skip XA audio sectors while XA playback is on: they go to the SPU, not to the CPU.
        const bool delivered = LoadNextSector(); // exactly one sector per sector period keeps XA pacing right
        ScheduleSector(now + ((mode_ & 0x80) ? kSectorPeriodDouble : kSectorPeriodSingle));
        if (!delivered) return false;
        sectorsDelivered++;
        r.bytes = {Stat()};
    }
    responseFifo_.assign(r.bytes.begin(), r.bytes.end());
    intFlag_ = r.type;
    if (accessLog.size() < 4096) accessLog.push_back({now, 'I', 0, intEnable_, r.type});
    return (intFlag_ & intEnable_) != 0;
}

void Cdrom::Queue(uint64_t due, uint8_t type, std::vector<uint8_t> bytes, bool sector) {
    auto at = std::upper_bound(pending_.begin(), pending_.end(), due, [](uint64_t d, const Response& r) { return d < r.due; });
    pending_.insert(at, Response{due, type, std::move(bytes), sector});
}

void Cdrom::ScheduleSector(uint64_t due) { Queue(due, 1, {}, true); }

bool Cdrom::LoadNextSector() {
    uint8_t raw[DiscImage::kRawSectorSize];
    if (!disc_ || readLba_ >= disc_->SectorCount()) { reading_ = false; return false; }
    disc_->ReadRawSector(readLba_++, raw);
    std::memcpy(lastHeader_, raw + 12, 8);
    const uint8_t file = raw[16], channel = raw[17], submode = raw[18];
    const bool xaAudio = (submode & 0x44) == 0x44; // audio + real-time
    if (xaAudio) { // XA-ADPCM goes to the SPU, never to the CPU
        const bool matches = !(mode_ & 0x08) || (file == filterFile_ && channel == filterChannel_);
        if ((mode_ & 0x40) && matches && onXaSector) {
            if (log && ((file << 8) | channel) != lastXaChannel_) {
                std::fprintf(log, "%llu xa lba %u file %u channel %u coding %02X\n", static_cast<unsigned long long>(logTag), readLba_ - 1, unsigned(file),
                             unsigned(channel), unsigned(raw[19]));
                lastXaChannel_ = (file << 8) | channel;
            }
            onXaSector(raw);
        } else {
            xaSectorsSkipped++;
        }
        return false;
    }
    if (mode_ & 0x20) sectorBuffer_.assign(raw + 12, raw + 12 + 0x924);
    else sectorBuffer_.assign(raw + 24, raw + 24 + 0x800);
    return true;
}

void Cdrom::DmaRead(uint8_t* dst, size_t bytes) {
    for (size_t i = 0; i < bytes; i++) dst[i] = dataPos_ < dataFifo_.size() ? dataFifo_[dataPos_++] : 0;
}

void Cdrom::Execute(uint8_t command, uint64_t now) {
    commandCounts[command]++;
    if (log) {
        switch (command) {
        case 0x02: if (params_.size() >= 3) std::fprintf(log, "%llu setloc lba %u\n", static_cast<unsigned long long>(logTag),
                                                          (uint32_t(FromBcd(params_[0])) * 60 + FromBcd(params_[1])) * 75 + FromBcd(params_[2]) - 150); break;
        case 0x0D: if (params_.size() >= 2) std::fprintf(log, "%llu setfilter file %u channel %u\n", static_cast<unsigned long long>(logTag), unsigned(params_[0]), unsigned(params_[1])); break;
        case 0x0E: if (!params_.empty()) std::fprintf(log, "%llu setmode %02X\n", static_cast<unsigned long long>(logTag), unsigned(params_[0])); break;
        case 0x06: case 0x1B: std::fprintf(log, "%llu %s from %u\n", static_cast<unsigned long long>(logTag), command == 0x06 ? "readn" : "reads", setlocPending_ ? setloc_ : readLba_); break;
        case 0x09: std::fprintf(log, "%llu pause at %u\n", static_cast<unsigned long long>(logTag), readLba_); break;
        default: break;
        }
    }
    auto ack = [&] { Queue(now + kAckDelay, 3, {Stat()}); };
    auto complete = [&](uint64_t delay) { Queue(now + kAckDelay + delay, 2, {Stat()}); };

    switch (command) {
    case 0x01: ack(); break; // Getstat
    case 0x02:               // Setloc (BCD mm:ss:ff)
        if (params_.size() >= 3) {
            setloc_ = (uint32_t(FromBcd(params_[0])) * 60 + FromBcd(params_[1])) * 75 + FromBcd(params_[2]) - 150;
            setlocPending_ = true;
        }
        ack();
        break;
    case 0x06: case 0x1B: // ReadN / ReadS
        if (setlocPending_) { readLba_ = setloc_; setlocPending_ = false; }
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Response& r) { return r.sector; }), pending_.end());
        reading_ = true;
        ack();
        ScheduleSector(now + kAckDelay + ((mode_ & 0x80) ? kSectorPeriodDouble : kSectorPeriodSingle));
        break;
    case 0x09: // Pause
        ack();
        reading_ = false;
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [](const Response& r) { return r.sector; }), pending_.end());
        complete(kShortDelay);
        break;
    case 0x0A: // Init
        ack();
        mode_ = 0;
        reading_ = false;
        complete(kLongDelay);
        break;
    case 0x07: ack(); complete(kShortDelay); break; // MotorOn
    case 0x08: ack(); reading_ = false; complete(kShortDelay); break; // Stop
    case 0x0B: case 0x0C: ack(); break;             // Mute / Demute
    case 0x0D:                                      // Setfilter
        if (params_.size() >= 2) { filterFile_ = params_[0]; filterChannel_ = params_[1]; }
        ack();
        break;
    case 0x0E: if (!params_.empty()) mode_ = params_[0]; ack(); break; // Setmode
    case 0x0F: Queue(now + kAckDelay, 3, {Stat(), mode_, 0, filterFile_, filterChannel_}); break; // Getparam
    case 0x10: Queue(now + kAckDelay, 3, std::vector<uint8_t>(lastHeader_, lastHeader_ + 8)); break; // GetlocL
    case 0x13: Queue(now + kAckDelay, 3, {Stat(), 0x01, 0x01}); break; // GetTN: one data track (BCD first, last)
    case 0x14: {                                                       // GetTD(track): BCD mm, ss
        auto bcd = [](uint32_t v) { return uint8_t((v / 10) * 16 + v % 10); };
        uint32_t track = params_.empty() ? 0 : FromBcd(params_[0]);
        uint32_t lba = track == 0 && disc_ ? disc_->SectorCount() : 0;
        uint32_t seconds = (lba + 150) / 75;
        Queue(now + kAckDelay, 3, {Stat(), bcd(seconds / 60), bcd(seconds % 60)});
        break;
    }
    case 0x15: case 0x16: // SeekL / SeekP
        if (setlocPending_) { readLba_ = setloc_; setlocPending_ = false; }
        ack();
        complete(kLongDelay);
        break;
    case 0x19: // Test
        if (!params_.empty() && params_[0] == 0x20) Queue(now + kAckDelay, 3, {0x94, 0x09, 0x19, 0xC0});
        else { unknownCommands.push_back("Test sub-function"); Queue(now + kAckDelay, 5, {0x11, 0x10}); }
        break;
    case 0x1A: // GetID: licensed Mode2 disc, region SCEA
        ack();
        Queue(now + kAckDelay + kShortDelay, 2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'});
        break;
    case 0x1E: ack(); complete(kLongDelay); break; // ReadTOC
    default: {
        char text[32];
        std::snprintf(text, sizeof(text), "command %02X", command);
        unknownCommands.push_back(text);
        Queue(now + kAckDelay, 5, {0x11, 0x40}); // error: invalid command
        break;
    }
    }
}

} // namespace gt2
