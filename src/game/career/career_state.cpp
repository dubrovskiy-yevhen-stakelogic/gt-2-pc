#include "game/career/career_state.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "gt2formats/course_data.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2::career {

namespace {

constexpr size_t kCardSize = 128 * 1024, kFrame = 128, kBlock = 8192, kDirFrames = 15;

uint8_t FrameChecksum(const uint8_t* frame) {
    uint8_t x = 0;
    for (size_t i = 0; i < kFrame - 1; i++) x ^= frame[i];
    return x;
}

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void PutLe32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i)); }
void PutLe16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); }

} // namespace

// ---------------------------------------------------------------- new game

NewGameDefaults ReadNewGameDefaults(const GuestImage& exe, uint16_t courseCount) {
    NewGameDefaults d;
    static constexpr uint32_t kTables[4] = {0x80091570u, 0x8009157Cu, 0x80091588u, 0x80091594u};
    for (size_t t = 0; t < 4; t++) std::memcpy(d.buttonTables[t].data(), exe.At(exe.Sim(kTables[t]), 11), 11);
    std::memcpy(d.padConfig.data(), exe.At(exe.Sim(0x800A6ED8u), 20), 20);
    d.courseCount = courseCount;
    return d;
}

NewGameDefaults ReadNewGameDefaults(const DiscImage& disc) {
    const GtfsVolume vol(disc);
    const CourseInfoTable info = ParseCourseInfo(vol.Read(".crsinfo"));
    return ReadNewGameDefaults(LoadExeImage(disc), uint16_t(info.entries.size()));
}

void InitTimeRecord(TimeRecord& r) { // 0x8005DD68
    for (int32_t& t : r.time) t = -1;
    r.word10 = 0;      // the fifth u32 -1 (+0x10) ...
    r.word12 = 0xFFFF; // ... whose low half is then cleared by `sh zero, 16(a0)`
}

void InitLicenceTestRecord(LicenceTestRecord& r) { // 0x8005DE1C
    r.byte0 = 0;
    r.passed = 0;
    r.byte2 = 0;
    for (int i = 0; i < 5; i++) {
        InitTimeRecord(r.times[i]);
        r.entries[i][0] = 0;
    }
}

void InitMachineTestRecord(MachineTestRecord& r) { // 0x8005E07C
    std::memset(r.bytes, 0, sizeof(r.bytes));
    for (int i = 0; i < 8; i++) PutLe32(r.bytes + 8 + i * 0x14, 0xFFFFFFFFu);
}

void InitCareerRecord(CareerRecord& r) { // 0x800107B4
    std::memset(&r, 0, sizeof(r));
    r.days = 1;
}

void InitGarage(GarageBlock& g) { // 0x80010798
    g.currentCar = -1;
    g.count = 0;
    g.money = kNewGameMoney;
    g.byte401B = 0;
}

void InitNewCareer(CareerState& s, GarageBlock* guestGarage, const NewGameDefaults& d) { // 0x800104A0
    uint8_t* b = reinterpret_cast<uint8_t*>(&s);
    std::memset(b, 0, 0xB6);
    // Both pads: four 11-byte button tables at +10 / +92 (+ 0, 11, 22, 33) and the 20-byte pad configuration at
    // +0x48 / +0x9A (= base + 62).
    for (size_t pad = 0; pad < 2; pad++) {
        uint8_t* base = b + 10 + pad * 82;
        for (size_t t = 0; t < 4; t++) std::memcpy(base + t * 11, d.buttonTables[t].data(), 11);
        std::memcpy(base + 62, d.padConfig.data(), 20);
    }
    b[0xB3] = 240;
    b[0xB4] = 192;
    b[0xB1] = 1;
    b[0xB2] = 1;
    b[0xB5] = 1;
    b[0x00] = 1; // language: USA
    b[0xAE] = 0;
    b[0xAF] = 0;
    b[0x02] = 0;
    b[0x03] = 2;
    b[0x04] = 1;
    b[0x05] = 0;
    b[0x06] = 2;
    b[0x08] = 1;
    InitGarage(s.garage);
    if (guestGarage) InitGarage(*guestGarage);
    for (uint16_t i = 0; i < d.courseCount; i++) { // the loop has no bound but the table's count
        if (i >= kCourseRecordSlots) throw std::runtime_error("new career: more courses than course record slots");
        InitTimeRecord(s.courses[i].best);
    }
    for (auto& licence : s.licences)
        for (LicenceTestRecord& test : licence) InitLicenceTestRecord(test);
    for (MachineTestRecord& m : s.machineTests) InitMachineTestRecord(m);
    InitCareerRecord(s.record);
}

CareerState NewCareer(const NewGameDefaults& defaults) {
    CareerState s;
    std::memset(&s, 0, sizeof(s)); // the block is BSS (0x8005E73C clears 0x15A5C bytes)
    InitNewCareer(s, nullptr, defaults);
    return s;
}

// ---------------------------------------------------------------- save game

CareerSave LoadCareerSaveFile(std::span<const uint8_t> file) {
    const GameSave g = ParseGameSave(file);
    CareerSave s;
    s.header = g.header;
    std::memcpy(&s.state, g.state.data(), sizeof(CareerState));
    s.storedCrc = g.storedCrc;
    s.computedCrc = g.computedCrc;
    return s;
}

CareerSave LoadCareerFromCard(std::span<const uint8_t> card) {
    for (const MemoryCardFile& f : ReadMemoryCardFiles(card))
        if (f.name == kSaveGameFileName) return LoadCareerSaveFile(f.bytes);
    throw std::runtime_error(std::string("no ") + kSaveGameFileName + " on the memory card");
}

CareerSave LoadCareer(const std::string& path) {
    const std::vector<uint8_t> bytes = ReadFileBytes(path);
    if (bytes.size() == kCardSize && bytes[0] == 'M' && bytes[1] == 'C') return LoadCareerFromCard(bytes);
    return LoadCareerSaveFile(bytes);
}

std::vector<uint8_t> BuildSaveHeader(const GuestImage& exe) {
    std::vector<uint8_t> h(kSaveHeaderSize, 0);
    h[0] = 'S';
    h[1] = 'C';
    h[2] = 0x13;
    h[3] = uint8_t((kSaveFileSize + kBlock - 1) / kBlock);
    // EUC-JP (JIS + 0x8080) -> Shift-JIS, two bytes per character, until the terminator.
    size_t out = 4;
    for (size_t i = 0; out + 2 <= 0x44; i += 2) {
        const uint8_t c1 = exe.At(exe.Sim(0x80091CA8u) + uint32_t(i), 2)[0], c2 = exe.At(exe.Sim(0x80091CA8u) + uint32_t(i), 2)[1];
        if (c1 == 0) break;
        if (c1 < 0x80) throw std::runtime_error("save header: unexpected single-byte title character");
        const uint32_t j1 = c1 - 0x80u, j2 = c2 - 0x80u;
        h[out++] = uint8_t(((j1 + 1) >> 1) + (j1 <= 0x5E ? 0x70 : 0xB0));
        h[out++] = uint8_t((j1 & 1) ? j2 + (j2 > 0x5F ? 0x20 : 0x1F) : j2 + 0x7E);
    }
    std::memcpy(h.data() + 0x60, exe.At(exe.Sim(0x80091CC4u), 32), 32);
    std::memcpy(h.data() + 0x80, exe.At(exe.Sim(0x80091CE4u), 384), 384);
    return h;
}

std::vector<uint8_t> BuildCareerSaveFile(const CareerSave& save) {
    if (save.header.size() != kSaveHeaderSize) throw std::runtime_error("career save: no 0x200-byte header");
    return BuildGameSaveFile(save.header, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&save.state), sizeof(CareerState)));
}

std::vector<uint8_t> FormatMemoryCard() {
    std::vector<uint8_t> card(kCardSize, 0);
    uint8_t* f0 = card.data();
    f0[0] = 'M';
    f0[1] = 'C';
    f0[kFrame - 1] = FrameChecksum(f0);
    for (size_t i = 1; i <= kDirFrames; i++) {
        uint8_t* f = card.data() + i * kFrame;
        PutLe32(f, 0xA0);
        PutLe16(f + 8, 0xFFFF);
        f[kFrame - 1] = FrameChecksum(f);
    }
    for (size_t i = 16; i < 36; i++) { // broken-sector list: unused
        uint8_t* f = card.data() + i * kFrame;
        PutLe32(f, 0xFFFFFFFFu);
        PutLe16(f + 8, 0xFFFF);
        f[kFrame - 1] = FrameChecksum(f);
    }
    std::memcpy(card.data() + 63 * kFrame, f0, kFrame); // write-test frame = copy of the header
    return card;
}

void StoreCareerOnCard(std::vector<uint8_t>& card, const CareerSave& save) {
    if (card.size() != kCardSize || card[0] != 'M' || card[1] != 'C') throw std::runtime_error("not a PS1 memory card image");
    const std::vector<uint8_t> file = BuildCareerSaveFile(save);
    const size_t blocks = (file.size() + kBlock - 1) / kBlock; // 4 (0x8006A000)
    std::vector<size_t> chain;
    for (size_t i = 1; i <= kDirFrames && chain.empty(); i++) {
        const uint8_t* f = card.data() + i * kFrame;
        if (Le32(f) != 0x51) continue;
        const char* name = reinterpret_cast<const char*>(f + 10);
        if (std::string(name, strnlen(name, 20)) != kSaveGameFileName) continue;
        size_t block = i;
        for (size_t guard = 0; guard < kDirFrames; guard++) {
            chain.push_back(block);
            const uint16_t next = uint16_t(card[block * kFrame + 8] | card[block * kFrame + 9] << 8);
            if (next == 0xFFFF) break;
            block = size_t(next) + 1;
            if (block < 1 || block > kDirFrames) throw std::runtime_error("memory card: broken block chain");
        }
        if (chain.size() != blocks) throw std::runtime_error("memory card: the existing save has an unexpected block count");
    }
    if (chain.empty()) { // allocate free blocks
        for (size_t i = 1; i <= kDirFrames && chain.size() < blocks; i++)
            if ((Le32(card.data() + i * kFrame) & 0xF0) == 0xA0) chain.push_back(i);
        if (chain.size() < blocks) throw std::runtime_error("memory card: not enough free blocks for the save");
        for (size_t k = 0; k < chain.size(); k++) {
            uint8_t* f = card.data() + chain[k] * kFrame;
            std::memset(f, 0, kFrame);
            PutLe32(f, k == 0 ? 0x51 : (k + 1 == chain.size() ? 0x53 : 0x52));
            PutLe32(f + 4, k == 0 ? uint32_t(blocks * kBlock) : 0);
            PutLe16(f + 8, k + 1 == chain.size() ? 0xFFFF : uint16_t(chain[k + 1] - 1));
            if (k == 0) std::memcpy(f + 10, kSaveGameFileName, std::strlen(kSaveGameFileName));
            f[kFrame - 1] = FrameChecksum(f);
        }
    }
    for (size_t k = 0; k < chain.size(); k++) {
        uint8_t* dst = card.data() + chain[k] * kBlock;
        std::memset(dst, 0, kBlock);
        const size_t at = k * kBlock;
        if (at < file.size()) std::memcpy(dst, file.data() + at, std::min(kBlock, file.size() - at));
    }
}

void SaveCareer(const std::string& path, const CareerSave& save, std::span<const uint8_t> templateCard) {
    const bool card = path.size() >= 4 && (path.compare(path.size() - 4, 4, ".mcd") == 0 || path.compare(path.size() - 4, 4, ".MCD") == 0);
    if (!card) {
        WriteFileBytes(path, BuildCareerSaveFile(save));
        return;
    }
    std::vector<uint8_t> image;
    if (std::FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fclose(f);
        image = ReadFileBytes(path);
    } else if (!templateCard.empty()) {
        image.assign(templateCard.begin(), templateCard.end());
    } else {
        image = FormatMemoryCard();
    }
    StoreCareerOnCard(image, save);
    WriteFileBytes(path, image);
}

std::vector<uint8_t> ReadFileBytes(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot open " + path);
    std::fseek(f, 0, SEEK_END);
    std::vector<uint8_t> bytes(size_t(std::ftell(f)));
    std::fseek(f, 0, SEEK_SET);
    const size_t n = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) throw std::runtime_error("cannot read " + path);
    return bytes;
}

void WriteFileBytes(const std::string& path, std::span<const uint8_t> bytes) {
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    const size_t n = std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (n != bytes.size()) throw std::runtime_error("cannot write " + path);
}

} // namespace gt2::career
