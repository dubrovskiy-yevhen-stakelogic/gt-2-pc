#include "machine/memcard.h"

#include <cstdio>
#include <cstring>
#include <filesystem>

namespace gt2 {
namespace {

constexpr uint32_t kStateFirst = 0x51, kStateMiddle = 0x52, kStateLast = 0x53, kStateFree = 0xA0;

uint32_t ReadU32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
void WriteU32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (i * 8)); }

} // namespace

MemoryCard::MemoryCard(std::string path) : path_(std::move(path)), data_(kSize, 0) {
    std::FILE* f = std::fopen(path_.c_str(), "rb");
    bool loaded = false;
    if (f) {
        loaded = std::fread(data_.data(), 1, kSize, f) == kSize && data_[0] == 'M' && data_[1] == 'C';
        std::fclose(f);
    }
    if (!loaded) {
        Format();
        Flush();
    }
}

void MemoryCard::SealFrame(int index) {
    uint8_t* f = Frame(index);
    uint8_t x = 0;
    for (size_t i = 0; i < kFrameSize - 1; i++) x ^= f[i];
    f[kFrameSize - 1] = x;
}

void MemoryCard::Format() {
    std::fill(data_.begin(), data_.end(), uint8_t(0));
    data_[0] = 'M';
    data_[1] = 'C';
    SealFrame(0);
    for (int i = 1; i <= int(kBlocks); i++) { // directory frames
        uint8_t* f = Frame(i);
        WriteU32(f, kStateFree);
        f[8] = f[9] = 0xFF;
        SealFrame(i);
    }
    for (int i = 16; i < 36; i++) { // broken sector list: none
        uint8_t* f = Frame(i);
        WriteU32(f, 0xFFFFFFFFu);
        f[8] = f[9] = 0xFF;
        SealFrame(i);
    }
    std::memcpy(Frame(63), Frame(0), kFrameSize); // write-test frame mirrors the header
}

void MemoryCard::Flush() const {
    std::filesystem::path target(path_);
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path());
    const std::string temp = path_ + ".tmp";
    std::FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(data_.data(), 1, kSize, f) == kSize;
    std::fclose(f);
    if (!ok) return;
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) { // Windows: rename over an existing file can fail
        std::filesystem::remove(target, ec);
        std::filesystem::rename(temp, target, ec);
    }
}

std::vector<int> MemoryCard::Chain(int firstBlock) const {
    std::vector<int> chain;
    for (int block = firstBlock; block >= 1 && block <= int(kBlocks) && chain.size() < kBlocks;) {
        chain.push_back(block);
        const uint8_t* f = Frame(block);
        const uint16_t next = uint16_t(f[8] | (f[9] << 8));
        if (next == 0xFFFF) break;
        block = int(next) + 1;
    }
    return chain;
}

std::vector<MemoryCard::DirEntry> MemoryCard::List() const {
    std::vector<DirEntry> out;
    for (int i = 1; i <= int(kBlocks); i++) {
        const uint8_t* f = Frame(i);
        if (ReadU32(f) != kStateFirst) continue;
        DirEntry e;
        e.name.assign(reinterpret_cast<const char*>(f + 10), strnlen(reinterpret_cast<const char*>(f + 10), 20));
        e.size = ReadU32(f + 4);
        e.firstBlock = i;
        out.push_back(e);
    }
    return out;
}

std::optional<MemoryCard::DirEntry> MemoryCard::Find(const std::string& name) const {
    for (const DirEntry& e : List())
        if (e.name == name) return e;
    return std::nullopt;
}

bool MemoryCard::Create(const std::string& name, int blocks) {
    if (blocks < 1 || name.empty() || name.size() > 20 || Find(name)) return false;
    std::vector<int> freeBlocks;
    for (int i = 1; i <= int(kBlocks) && int(freeBlocks.size()) < blocks; i++)
        if ((ReadU32(Frame(i)) & 0xF0) == kStateFree) freeBlocks.push_back(i);
    if (int(freeBlocks.size()) < blocks) return false;

    for (int n = 0; n < blocks; n++) {
        uint8_t* f = Frame(freeBlocks[size_t(n)]);
        std::memset(f, 0, kFrameSize);
        WriteU32(f, n == 0 ? kStateFirst : (n == blocks - 1 ? kStateLast : kStateMiddle));
        if (n == 0) {
            WriteU32(f + 4, uint32_t(blocks) * kBlockSize);
            std::memcpy(f + 10, name.data(), name.size());
        }
        const uint16_t next = n + 1 < blocks ? uint16_t(freeBlocks[size_t(n) + 1] - 1) : 0xFFFF;
        f[8] = uint8_t(next);
        f[9] = uint8_t(next >> 8);
        SealFrame(freeBlocks[size_t(n)]);
    }
    return true;
}

bool MemoryCard::Delete(const std::string& name) {
    auto e = Find(name);
    if (!e) return false;
    for (int block : Chain(e->firstBlock)) {
        uint8_t* f = Frame(block);
        std::memset(f, 0, kFrameSize);
        WriteU32(f, kStateFree);
        f[8] = f[9] = 0xFF;
        SealFrame(block);
    }
    return true;
}

size_t MemoryCard::Read(const DirEntry& file, uint32_t offset, uint8_t* dst, size_t bytes) const {
    const std::vector<int> chain = Chain(file.firstBlock);
    size_t done = 0;
    for (; done < bytes; done++) {
        const uint32_t at = offset + uint32_t(done);
        if (at / kBlockSize >= chain.size()) break;
        dst[done] = data_[size_t(chain[at / kBlockSize]) * kBlockSize + at % kBlockSize];
    }
    return done;
}

size_t MemoryCard::Write(const DirEntry& file, uint32_t offset, const uint8_t* src, size_t bytes) {
    const std::vector<int> chain = Chain(file.firstBlock);
    size_t done = 0;
    for (; done < bytes; done++) {
        const uint32_t at = offset + uint32_t(done);
        if (at / kBlockSize >= chain.size()) break;
        data_[size_t(chain[at / kBlockSize]) * kBlockSize + at % kBlockSize] = src[done];
    }
    return done;
}

} // namespace gt2
