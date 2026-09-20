#include "gt2formats/seqg.h"

#include <cstring>
#include <stdexcept>

namespace gt2 {

namespace {
uint32_t U32(std::span<const uint8_t> b, size_t o) {
    if (o + 4 > b.size()) throw std::runtime_error("SEQG / INST: short file");
    return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}
uint16_t U16(std::span<const uint8_t> b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("SEQG / INST: short file");
    return uint16_t(b[o] | (b[o + 1] << 8));
}
} // namespace

SeqgFile ParseSeqg(std::span<const uint8_t> file) {
    if (file.size() < 0x0C || std::memcmp(file.data(), "SEQG", 4) != 0) throw std::runtime_error("SEQG: bad magic");
    SeqgFile f;
    f.bytes.assign(file.begin(), file.end());
    const uint32_t count = U32(file, 8);
    if (count == 0 || count > 64) throw std::runtime_error("SEQG: bad sequence count");
    for (uint32_t i = 0; i < count; i++) {
        const size_t at = 0x0C + size_t(i) * 0x48;
        SeqgSequence s;
        s.volume = U16(file, at);
        s.word02 = U16(file, at + 2);
        s.tempo = U32(file, at + 4);
        for (size_t t = 0; t < 16; t++) {
            s.tracks[t] = U32(file, at + 8 + t * 4);
            if (s.tracks[t] >= file.size()) throw std::runtime_error("SEQG: track offset outside the file");
        }
        f.sequences.push_back(s);
    }
    return f;
}

std::vector<InstProgram> ParseInstPrograms(std::span<const uint8_t> file) {
    if (file.size() < 0x28 || std::memcmp(file.data(), "INST", 4) != 0) throw std::runtime_error("INST: bad magic");
    const uint32_t count = U32(file, 0x20), table = U32(file, 0x24);
    if (count > 1024) throw std::runtime_error("INST: bad program count");
    std::vector<InstProgram> out;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t at = U32(file, table + size_t(i) * 4);
        if (at + 4 > file.size()) throw std::runtime_error("INST: program outside the file");
        InstProgram p;
        const uint8_t n = file[at];
        p.pan = file[at + 1];
        p.volume = int16_t(U16(file, at + 2));
        if (at + 4 + n > file.size()) throw std::runtime_error("INST: program outside the file");
        p.samples.assign(file.begin() + std::ptrdiff_t(at + 4), file.begin() + std::ptrdiff_t(at + 4 + n));
        out.push_back(std::move(p));
    }
    return out;
}

} // namespace gt2
