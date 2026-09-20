#include "gt2vfs/disc_image.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#define gt2_fseek64 _fseeki64
#define gt2_ftell64 _ftelli64
#else
#define gt2_fseek64 fseeko
#define gt2_ftell64 ftello
#endif

namespace gt2 {
namespace {

uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

} // namespace

DiscImage::DiscImage(const std::string& binPath) {
    std::filesystem::path imagePath(binPath);
    if (std::filesystem::is_directory(imagePath)) {
        dataDirectory_ = imagePath.string();
        imagePath /= "disc.raw2352";
    }
    file_ = std::fopen(imagePath.string().c_str(), "rb");
    if (!file_) throw std::runtime_error("cannot open disc image: " + binPath);
    try {
    gt2_fseek64(file_, 0, SEEK_END);
    int64_t length = gt2_ftell64(file_);
    if (length <= 0 || length % kRawSectorSize != 0 || uint64_t(length / kRawSectorSize) > UINT32_MAX)
        throw std::runtime_error("a complete 2352-byte/sector PS1 BIN is required (2048-byte ISO files lose XA music/video): " + binPath);
    sectorCount_ = static_cast<uint32_t>(length / kRawSectorSize);
    ParseIso();
    } catch (...) { std::fclose(file_); file_ = nullptr; throw; }
}

DiscImage::~DiscImage() {
    if (file_) std::fclose(file_);
}

void DiscImage::ReadRawSector(uint32_t lba, uint8_t* out2352) const {
    if (lba >= sectorCount_) throw std::runtime_error("disc read past end of image");
    gt2_fseek64(file_, static_cast<int64_t>(lba) * kRawSectorSize, SEEK_SET);
    if (std::fread(out2352, 1, kRawSectorSize, file_) != kRawSectorSize)
        throw std::runtime_error("disc read failed");
}

void DiscImage::ReadForm1(uint32_t lba, uint64_t byteOffset, uint8_t* out, size_t size) const {
    uint8_t raw[kRawSectorSize];
    const uint64_t capacity = uint64_t(sectorCount_) * kUserDataSize;
    const uint64_t start = uint64_t(lba) * kUserDataSize;
    if (start > capacity || byteOffset > capacity - start || size > capacity - start - byteOffset)
        throw std::runtime_error("disc Form1 read past end of image");
    uint32_t sector = lba + static_cast<uint32_t>(byteOffset / kUserDataSize);
    size_t inSector = static_cast<size_t>(byteOffset % kUserDataSize);
    while (size > 0) {
        ReadRawSector(sector++, raw);
        size_t n = std::min(size, static_cast<size_t>(kUserDataSize) - inSector);
        std::memcpy(out, raw + kForm1DataOffset + inSector, n);
        out += n;
        size -= n;
        inSector = 0;
    }
}

void DiscImage::ParseIso() {
    uint8_t pvd[kUserDataSize];
    ReadForm1(16, 0, pvd, sizeof(pvd));
    if (pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
        throw std::runtime_error("ISO9660 primary volume descriptor not found");

    const uint8_t* root = pvd + 156;
    uint32_t dirLba = ReadU32(root + 2);
    uint32_t dirSize = ReadU32(root + 10);
    if (root[0] < 34 || dirSize > 16 * 1024 * 1024 || dirLba >= sectorCount_ ||
        uint64_t(dirSize) > uint64_t(sectorCount_ - dirLba) * kUserDataSize)
        throw std::runtime_error("invalid ISO9660 root directory");
    std::vector<uint8_t> dir(dirSize);
    ReadForm1(dirLba, 0, dir.data(), dir.size());

    size_t pos = 0;
    while (pos < dir.size()) {
        uint8_t recLen = dir[pos];
        if (recLen == 0) { // records never span sectors
            pos = (pos / kUserDataSize + 1) * kUserDataSize;
            continue;
        }
        const uint8_t* rec = dir.data() + pos;
        if (recLen < 34 || recLen > dir.size() - pos || (pos % kUserDataSize) + recLen > kUserDataSize)
            throw std::runtime_error("truncated ISO9660 directory record");
        uint8_t flags = rec[25];
        uint8_t nameLen = rec[32];
        if (size_t(33) + nameLen > recLen) throw std::runtime_error("invalid ISO9660 filename length");
        if (!(flags & 0x02) && nameLen > 0 && !(nameLen == 1 && rec[33] <= 1)) {
            std::string name(reinterpret_cast<const char*>(rec + 33), nameLen);
            if (auto semi = name.find(';'); semi != std::string::npos) name.resize(semi);
            const uint32_t lba = ReadU32(rec + 2), size = ReadU32(rec + 10);
            if (lba > sectorCount_ || uint64_t(size) > uint64_t(sectorCount_ - lba) * kRawSectorSize)
                throw std::runtime_error("ISO9660 file outside disc");
            rootFiles_.push_back({name, lba, size});
        }
        pos += recLen;
    }
}

std::optional<IsoFile> DiscImage::FindRootFile(const std::string& name) const {
    for (const auto& f : rootFiles_)
        if (f.name == name) return f;
    return std::nullopt;
}

} // namespace gt2
