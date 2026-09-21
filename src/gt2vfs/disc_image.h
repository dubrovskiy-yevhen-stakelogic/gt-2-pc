#pragma once
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace gt2 {

struct IsoFile {
    std::string name; // without the ";1" version suffix
    uint32_t lba = 0;
    uint32_t size = 0;
};

// Raw 2352-byte-sector MODE2 PS1 disc image (single data track .bin), opened read-only.
class DiscImage {
public:
    static constexpr uint32_t kRawSectorSize = 2352;
    static constexpr uint32_t kUserDataSize = 2048;
    static constexpr uint32_t kForm1DataOffset = 24;

    explicit DiscImage(const std::string& binPath);
    ~DiscImage();
    DiscImage(const DiscImage&) = delete;
    DiscImage& operator=(const DiscImage&) = delete;

    uint32_t SectorCount() const { return sectorCount_; }
    const std::string& DataDirectory() const { return dataDirectory_; }

    // Whole raw sector (sync, header, subheader, data, EDC/ECC) at any LBA.
    void ReadRawSector(uint32_t lba, uint8_t* out2352) const;
    void ReadRawSectors(uint32_t lba, uint32_t count, uint8_t* output) const;

    // Form1 user data of a byte range that starts at `lba`.
    void ReadForm1(uint32_t lba, uint64_t byteOffset, uint8_t* out, size_t size) const;

    const std::vector<IsoFile>& RootFiles() const { return rootFiles_; }
    std::optional<IsoFile> FindRootFile(const std::string& name) const;

private:
    void ParseIso();

    std::FILE* file_ = nullptr;
    std::string dataDirectory_;
    uint32_t sectorCount_ = 0;
    std::vector<IsoFile> rootFiles_;
};

} // namespace gt2
