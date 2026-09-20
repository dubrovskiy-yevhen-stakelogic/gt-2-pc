#pragma once
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "gt2vfs/disc_image.h"

namespace gt2 {

// PS1 CD-ROM controller at the register level (0x1F801800-0x1F801803), fed by the user's disc image.
// Behaviour follows psx-spx. Time is measured in guest instructions (see Machine).
class Cdrom {
public:
    explicit Cdrom(const DiscImage* disc) : disc_(disc) {}

    uint8_t Read(uint32_t reg);
    void Write(uint32_t reg, uint8_t value, uint64_t now);

    // Delivers a due response if no interrupt is pending. Returns true when the CD-ROM IRQ line must be raised.
    bool Tick(uint64_t now);
    uint64_t NextEventTime() const { return pending_.empty() || intFlag_ ? UINT64_MAX : pending_.front().due; }

    // DMA channel 3: pops bytes from the data FIFO.
    void DmaRead(uint8_t* dst, size_t bytes);

    struct Access { uint64_t time; char kind; uint8_t reg, index, value; };
    std::vector<Access> accessLog; // first 4096 register accesses and delivered interrupts (kind 'I')
    std::function<void(const uint8_t* rawSector)> onXaSector; // XA-ADPCM sectors selected for playback
    std::map<uint8_t, uint64_t> commandCounts;
    std::vector<std::string> unknownCommands;
    uint64_t sectorsDelivered = 0, xaSectorsSkipped = 0;
    // Dev aid (gt2play --cd-log): the commands that position / filter / start the stream and every change of the XA
    // (file, channel) sent to the SPU, as "<tag> <what> ..."; `logTag` is set by the host (the field number).
    mutable std::FILE* log = nullptr;
    mutable uint64_t logTag = 0;

private:
    struct Response { uint64_t due; uint8_t type; std::vector<uint8_t> bytes; bool sector = false; };

    void Execute(uint8_t command, uint64_t now);
    void Queue(uint64_t due, uint8_t type, std::vector<uint8_t> bytes, bool sector = false); // keeps pending_ sorted by due time
    uint8_t Stat() const { return uint8_t(0x02 | (reading_ ? 0x20 : 0)); } // motor on
    void ScheduleSector(uint64_t due);
    bool LoadNextSector();

    const DiscImage* disc_;
    uint8_t index_ = 0, intEnable_ = 0x1F /* state the BIOS leaves behind */, intFlag_ = 0, mode_ = 0, filterFile_ = 0, filterChannel_ = 0;
    std::vector<uint8_t> params_;
    std::deque<uint8_t> responseFifo_;
    std::deque<Response> pending_;
    std::vector<uint8_t> sectorBuffer_, dataFifo_;
    size_t dataPos_ = 0;
    uint32_t setloc_ = 0, readLba_ = 0;
    bool reading_ = false, setlocPending_ = false;
    uint8_t lastHeader_[8] = {};
    int lastXaChannel_ = -1; // --cd-log: the (file << 8 | channel) of the last XA sector sent to the SPU
};

} // namespace gt2
