#include "gt2vfs/gtfs.h"

#include <cstring>
#include <functional>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <cstdlib>
#include <algorithm>

#include "gt2vfs/inflate.h"

namespace gt2 {
namespace {

constexpr uint32_t kBlockSize = 2048;
constexpr uint32_t kRecordSize = 32;
constexpr uint32_t kHeaderSize = 0x10;

uint16_t ReadU16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t ReadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24));
}

} // namespace

GtfsVolume::GtfsVolume(const DiscImage& disc) : disc_(disc) {
    auto vol = disc.FindRootFile("GT2.VOL");
    if (!vol) throw std::runtime_error("GT2.VOL not found in disc root");
    vol_ = *vol;

    uint8_t header[kHeaderSize];
    disc_.ReadForm1(vol_.lba, 0, header, sizeof(header));
    if (std::memcmp(header, "GTFS", 4) != 0) throw std::runtime_error("GT2.VOL: bad magic");
    uint32_t offsetCount = ReadU16(header + 0x08);
    uint32_t recordCount = ReadU16(header + 0x0A);
    if (offsetCount < 3) throw std::runtime_error("GT2.VOL: offset table too small");

    std::vector<uint8_t> table(offsetCount * 4);
    disc_.ReadForm1(vol_.lba, kHeaderSize, table.data(), table.size());
    // Validate the offset table before subtracting sizes or allocating file buffers.
    for (uint32_t i = 0; i + 1 < offsetCount; ++i) {
        const uint64_t a = ReadU32(&table[i * 4]) & ~uint64_t(0x7FF);
        const uint64_t b = ReadU32(&table[(i + 1) * 4]) & ~uint64_t(0x7FF);
        const uint32_t padding = ReadU32(&table[i * 4]) & 0x7FF;
        if (b < a || padding > b - a || b > vol_.size) throw std::runtime_error("GT2.VOL: invalid file offsets");
    }
    auto start = [&](uint32_t i) { return static_cast<uint64_t>(ReadU32(&table[i * 4]) >> 11) * kBlockSize; };
    auto pad = [&](uint32_t i) { return ReadU32(&table[i * 4]) & 0x7FF; };
    auto size = [&](uint32_t i) { return static_cast<uint32_t>(start(i + 1) - start(i) - pad(i)); };

    if (size(1) != recordCount * kRecordSize) throw std::runtime_error("GT2.VOL: TOC size mismatch");
    std::vector<uint8_t> toc(size(1));
    disc_.ReadForm1(vol_.lba, start(1), toc.data(), toc.size());

    std::function<void(uint32_t, const std::string&, int)> walk = [&](uint32_t first, const std::string& prefix, int depth) {
        if (depth > 16) throw std::runtime_error("GT2.VOL: directory nesting too deep");
        for (uint32_t i = first; i < recordCount; i++) {
            const uint8_t* r = &toc[i * kRecordSize];
            uint16_t index = ReadU16(r + 4);
            uint8_t flags = r[6];
            std::string name(reinterpret_cast<const char*>(r + 7), strnlen(reinterpret_cast<const char*>(r + 7), 25));
            if (name != "..") {
                if (flags & 0x01) {
                    walk(index, prefix + name + "/", depth + 1);
                } else if (index >= 2 && index + 1u < offsetCount) {
                    files_.push_back({prefix + name, ReadU32(r), start(index), size(index), index});
                }
            }
            if (flags & 0x80) return;
        }
        throw std::runtime_error("GT2.VOL: unterminated directory");
    };
    walk(0, "", 0);
    std::filesystem::path expansion;
    if (const char* configured = std::getenv("GT2_CAR_EXPANSION")) expansion = configured;
    else if (!disc_.DataDirectory().empty()) expansion = std::filesystem::path(disc_.DataDirectory()) / "expansions" / "gt4";
    if (!expansion.empty() && std::filesystem::is_regular_file(expansion / "catalogue.version")) {
        std::ifstream version(expansion / "catalogue.version", std::ios::binary);
        std::string format; std::getline(version, format);
        // Packs created on Windows may use CRLF; getline only removes LF on Android.
        if (!format.empty() && format.back() == '\r') format.pop_back();
        if (format != "gt2-car-expansion-1") throw std::runtime_error("unsupported car expansion");
        const auto root = std::filesystem::canonical(expansion / "assets");
        std::vector<std::pair<std::string,std::string>> added;
        for (const auto& file : std::filesystem::recursive_directory_iterator(root)) {
            if (!file.is_regular_file()) continue;
            const auto actual = std::filesystem::canonical(file.path());
            const auto relative = actual.lexically_relative(root);
            if (relative.empty() || *relative.begin() == "..") throw std::runtime_error("car expansion asset escapes root");
            const auto name = file.path().lexically_relative(root).generic_string();
            // No executable, audio or track overrides through the car catalogue layer.
            if (!(name.starts_with("carobj/") || name.starts_with("carparam/") || name.starts_with(".carinfo") || name == ".carcolor" || name == ".cclatain"))
                throw std::runtime_error("unsupported car expansion asset: " + name);
            if (file.file_size() > 32 * 1024 * 1024 || added.size() >= 8192)
                throw std::runtime_error("car expansion exceeds asset limits");
            added.emplace_back(name, actual.string());
        }
        std::sort(added.begin(), added.end());
        uint32_t index = 0x10000;
        for (const auto& [name, path] : added) {
            expansionFiles_.emplace(name,path);
            if (!Find(name)) files_.push_back({name,0,0,uint32_t(std::filesystem::file_size(path)),index++});
        }
    }
}

const GtfsEntry* GtfsVolume::Find(const std::string& path) const {
    for (const auto& f : files_)
        if (f.path == path) return &f;
    if (path.rfind("gtmenu/usa/", 0) == 0) {
        const std::string english = "gtmenu/eng/" + path.substr(11);
        for (const auto& f : files_) if (f.path == english) return &f;
    }
    return nullptr;
}

std::vector<uint8_t> GtfsVolume::ReadStored(const GtfsEntry& e) const {
    if (const auto found = expansionFiles_.find(e.path); found != expansionFiles_.end()) {
        std::ifstream in(found->second, std::ios::binary);
        const auto size = std::filesystem::file_size(found->second);
        if (size > 32 * 1024 * 1024) throw std::runtime_error("car expansion asset too large");
        std::vector<uint8_t> data(static_cast<size_t>(size));
        if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())))
            throw std::runtime_error("cannot read car expansion asset: " + e.path);
        return data;
    }
    std::vector<uint8_t> data(e.size);
    disc_.ReadForm1(vol_.lba, e.offset, data.data(), data.size());
    return data;
}

std::vector<uint8_t> GtfsVolume::Read(const GtfsEntry& e) const {
    if (const auto found = expansionFiles_.find(e.path); found != expansionFiles_.end()) {
        auto data = ReadStored(e);
        return IsGzip(data) ? Gunzip(data) : data;
    }
    if (!disc_.DataDirectory().empty()) {
        const auto path = std::filesystem::path(disc_.DataDirectory()) / "assets" / e.path;
        if (std::filesystem::is_regular_file(path)) {
            const auto size = std::filesystem::file_size(path);
            if (size > 256 * 1024 * 1024) throw std::runtime_error("installed asset too large: " + e.path);
            std::ifstream in(path, std::ios::binary);
            std::vector<uint8_t> data(static_cast<size_t>(size));
            if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) throw std::runtime_error("cannot read installed asset: " + e.path);
            return data; // installation stores already-inflated bytes, including entries named *.gz
        }
    }
    auto data = ReadStored(e);
    return IsGzip(data) ? Gunzip(data) : data;
}

std::vector<uint8_t> GtfsVolume::Read(const std::string& path) const {
    const GtfsEntry* e = Find(path);
    if (!e) e = Find(path + ".gz");
    if (!e) throw std::runtime_error("not found in GT2.VOL: " + path);
    return Read(*e);
}

} // namespace gt2
