#include "gt2formats/sound_bank.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace gt2 {
namespace {

constexpr uint32_t kMagicEngn = 0x4E474E45u; // "ENGN"
constexpr uint32_t kMagicInst = 0x54534E49u; // "INST"

uint32_t U32(std::span<const uint8_t> file, size_t offset) {
    if (offset + 4 > file.size()) throw std::runtime_error("sound bank: header truncated");
    uint32_t v;
    std::memcpy(&v, file.data() + offset, 4);
    return v;
}

struct Header {
    uint32_t headerBytes, dataBytes, count0, table0;
};

Header ReadHeader(std::span<const uint8_t> file, uint32_t magic, const char* what) {
    if (file.size() < 0x28 || U32(file, 0) != magic) throw std::runtime_error(std::string("sound bank: not a ") + what + " file");
    Header h{U32(file, 0x14), U32(file, 0x10), U32(file, 0x18), U32(file, 0x1C)};
    if (h.headerBytes < 0x28 || h.headerBytes > file.size() || h.dataBytes > file.size() - h.headerBytes)
        throw std::runtime_error(std::string("sound bank: bad ") + what + " sizes");
    return h;
}

} // namespace

EngineBank ParseEngineBank(std::span<const uint8_t> file) {
    const Header h = ReadHeader(file, kMagicEngn, "ENGN");
    if (h.count0 > 64 || h.table0 + h.count0 * sizeof(EngineLayer) > h.headerBytes) throw std::runtime_error("sound bank: bad ENGN layer table");
    EngineBank bank;
    bank.layers.resize(h.count0);
    std::memcpy(bank.layers.data(), file.data() + h.table0, h.count0 * sizeof(EngineLayer));
    for (const EngineLayer& l : bank.layers)
        if (l.sampleAddress >= h.dataBytes || (l.sampleAddress & 15)) throw std::runtime_error("sound bank: ENGN layer sample outside the data");
    bank.data.assign(file.begin() + h.headerBytes, file.begin() + h.headerBytes + h.dataBytes);
    return bank;
}

InstBank ParseInstBank(std::span<const uint8_t> file) {
    const Header h = ReadHeader(file, kMagicInst, "INST");
    if (h.count0 > 1024 || h.table0 + h.count0 * sizeof(InstSample) > h.headerBytes) throw std::runtime_error("sound bank: bad INST sample table");
    InstBank bank;
    bank.samples.resize(h.count0);
    std::memcpy(bank.samples.data(), file.data() + h.table0, h.count0 * sizeof(InstSample));
    for (const InstSample& s : bank.samples)
        if (uint32_t(s.address) * 8 >= h.dataBytes) throw std::runtime_error("sound bank: INST sample outside the data");
    bank.data.assign(file.begin() + h.headerBytes, file.begin() + h.headerBytes + h.dataBytes);
    return bank;
}

std::string EngineSoundPath(uint32_t soundId) {
    char name[32];
    std::snprintf(name, sizeof(name), "engine/%05u.es", soundId);
    return name;
}

std::string ExhaustSoundPath(uint32_t soundId, uint8_t exhaustByte) {
    char name[32];
    std::snprintf(name, sizeof(name), "engine/%05u_%c%u.es", soundId, exhaustByte >= 4 ? 't' : 'n', unsigned(exhaustByte & 3));
    return name;
}

} // namespace gt2
