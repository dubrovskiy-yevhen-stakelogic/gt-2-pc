#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <functional>
#include <string>
#include <stdexcept>
#include <vector>

namespace gt2 {

// Guest physical address space of the game: 2 MB RAM, 1 KB scratchpad, hardware I/O window.
// KUSEG/KSEG0/KSEG1 all map to the same physical space (the game runs in KSEG0).
class Bus {
public:
    static constexpr uint32_t kRamSize = 2 * 1024 * 1024;
    static constexpr uint32_t kScratchBase = 0x1F800000, kScratchSize = 0x400;
    static constexpr uint32_t kIoBase = 0x1F801000, kIoEnd = 0x1F803000;

    // Hardware register hooks (size in bytes: 1, 2 or 4). Unset -> reads return 0, writes are dropped.
    std::function<uint32_t(uint32_t physical, int size)> ioRead;
    std::function<void(uint32_t physical, uint32_t value, int size)> ioWrite;

    Bus() : ram_(kRamSize, 0), scratch_(kScratchSize, 0) {}

    static uint32_t Physical(uint32_t address) {
        static constexpr uint32_t kMask[8] = {0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, // KUSEG
                                              0x7FFFFFFF,                                     // KSEG0
                                              0x1FFFFFFF,                                     // KSEG1
                                              0xFFFFFFFF, 0xFFFFFFFF};                        // KSEG2
        return address & kMask[address >> 29];
    }

    uint8_t* Ram() { return ram_.data(); }
    const uint8_t* Ram() const { return ram_.data(); }
    uint8_t* Scratch() { return scratch_.data(); }
    const uint8_t* Scratch() const { return scratch_.data(); }

    // Direct pointer for bulk host access; throws if the range is not inside RAM.
    uint8_t* RamPointer(uint32_t address, uint32_t size) {
        uint32_t p = Physical(address);
        if (p >= kRamSize || size > kRamSize - p) throw std::runtime_error("guest range is outside RAM");
        return ram_.data() + p;
    }

    // Hot path: aligned 32-bit access to main RAM (instruction fetch, most loads/stores).
    bool FastRead32(uint32_t address, uint32_t& value) const {
        const uint32_t p = address & 0x1FFFFFFFu;
        if (p >= kRamSize) return false;
        std::memcpy(&value, ram_.data() + p, 4);
        return true;
    }

    // RE aid: reports every write into [watchBase, watchBase + watchSize) of physical RAM or the scratchpad (0x1F8000xx).
    uint32_t watchBase = 0, watchSize = 0;
    std::function<void(uint32_t physical, uint32_t value, int size)> onWatchedWrite;

    bool FastWrite32(uint32_t address, uint32_t value) {
        const uint32_t p = address & 0x1FFFFFFFu;
        if (p >= kRamSize) return false;
        if (p - watchBase < watchSize && onWatchedWrite) onWatchedWrite(p, value, 4);
        std::memcpy(ram_.data() + p, &value, 4);
        return true;
    }

    uint32_t Read(uint32_t address, int size) const {
        uint32_t p = Physical(address);
        const uint8_t* m = nullptr;
        if (p < kRamSize * 4) m = ram_.data() + (p & (kRamSize - 1)); // RAM is mirrored in the first 8 MB
        else if (p >= kScratchBase && p < kScratchBase + kScratchSize) m = scratch_.data() + (p - kScratchBase);
        else if (p >= kIoBase && p < kIoEnd) return ioRead ? ioRead(p, size) : 0;
        else if (p == 0xFFFE0130) return cacheControl_;
        else throw std::runtime_error("guest read from unmapped address " + Hex(address));
        uint32_t v = 0;
        std::memcpy(&v, m, size_t(size)); // host is little-endian, like the guest
        return v;
    }

    void Write(uint32_t address, uint32_t value, int size) {
        uint32_t p = Physical(address);
        uint8_t* m = nullptr;
        if (p < kRamSize * 4) {
            m = ram_.data() + (p & (kRamSize - 1));
            if ((p & (kRamSize - 1)) - watchBase < watchSize && onWatchedWrite) onWatchedWrite(p & (kRamSize - 1), value, size);
        }
        else if (p >= kScratchBase && p < kScratchBase + kScratchSize) {
            m = scratch_.data() + (p - kScratchBase);
            if (p - watchBase < watchSize && onWatchedWrite) onWatchedWrite(p, value, size); // physical scratchpad address
        }
        else if (p >= kIoBase && p < kIoEnd) { if (ioWrite) ioWrite(p, value, size); return; }
        else if (p == 0xFFFE0130) { cacheControl_ = value; return; }
        else throw std::runtime_error("guest write to unmapped address " + Hex(address));
        std::memcpy(m, &value, size_t(size));
    }

    static std::string Hex(uint32_t v) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "0x%08X", v);
        return buf;
    }

private:
    std::vector<uint8_t> ram_, scratch_;
    uint32_t cacheControl_ = 0;
};

} // namespace gt2
