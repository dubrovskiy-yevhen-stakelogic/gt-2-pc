#pragma once
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace gt2 {

// PS1 memory card (128 KB, 15 blocks of 8 KB) with the standard directory layout, stored as a raw
// .mcd image so saves are interchangeable with emulators. This is the file-system half of the BIOS
// "bu" device; Machine exposes it through the HLE file calls.
class MemoryCard {
public:
    static constexpr size_t kSize = 128 * 1024, kBlockSize = 8192, kFrameSize = 128, kBlocks = 15;

    struct DirEntry {
        std::string name;
        uint32_t size = 0;
        int firstBlock = 0; // 1..15
    };

    // Loads the image or creates a freshly formatted one.
    explicit MemoryCard(std::string path);

    std::vector<DirEntry> List() const;
    std::optional<DirEntry> Find(const std::string& name) const;

    // Creates a file of `blocks` blocks. Returns false when the name exists or space is short.
    bool Create(const std::string& name, int blocks);
    bool Delete(const std::string& name);

    // Byte-level access inside a file (offsets are file-relative). Returns bytes transferred.
    size_t Read(const DirEntry& file, uint32_t offset, uint8_t* dst, size_t bytes) const;
    size_t Write(const DirEntry& file, uint32_t offset, const uint8_t* src, size_t bytes);

    // Raw 128-byte frame access for the BIOS sector calls (frame 0..1023).
    void ReadFrame(uint32_t frame, uint8_t* dst) const { std::copy_n(&data_[size_t(frame & 1023) * kFrameSize], kFrameSize, dst); }
    void WriteFrame(uint32_t frame, const uint8_t* src) { std::copy_n(src, kFrameSize, &data_[size_t(frame & 1023) * kFrameSize]); }

    void Format();
    void Flush() const; // atomic: write to a temp file, then rename

private:
    uint8_t* Frame(int index) { return &data_[size_t(index) * kFrameSize]; }
    const uint8_t* Frame(int index) const { return &data_[size_t(index) * kFrameSize]; }
    void SealFrame(int index);
    std::vector<int> Chain(int firstBlock) const;

    std::string path_;
    std::vector<uint8_t> data_;
};

} // namespace gt2
