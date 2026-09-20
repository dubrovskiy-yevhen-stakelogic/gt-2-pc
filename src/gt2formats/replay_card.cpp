#include "gt2formats/replay_card.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "gt2formats/overlay_data.h"
#include "gt2formats/save_data.h"

namespace gt2 {

namespace {

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void PutLe32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; i++) p[i] = uint8_t(v >> (8 * i));
}
size_t Round4(size_t n) { return (n + 3) & ~size_t(3); }
size_t EntryOffset(int index) { return kReplayDirEntries + size_t(index) * kReplayEntrySize; }

} // namespace

// ---------------------------------------------------------------- entries

std::string ReplayCardEntry::Title() const {
    const char* p = reinterpret_cast<const char*>(desc.data());
    return std::string(p, strnlen(p, 0x20));
}

std::string ReplayCardEntry::CarName() const { // 0x80069048
    const char* p = reinterpret_cast<const char*>(desc.data() + 0x20);
    if (desc[0x3F] == 0) return std::string(p, strnlen(p, 0x20));
    // strncpy(buf, +0x20, 31), then the 3 bytes of EXE 0x8008FA70 ("..", NUL) at buf + 31: visible only after a 31-character name
    std::string s(p, strnlen(p, 31));
    if (s.size() == 31) s += "..";
    return s;
}

uint32_t ReplayCardEntry::CourseId() const { return Le32(desc.data() + 0x44); }
uint32_t ReplayCardEntry::CarId() const { return Le32(desc.data() + 0x48); }

void ReplayCardEntry::SetTitle(const std::string& title) { // 0x80069028 (strcpy)
    const size_t n = std::min<size_t>(title.size(), 0x1F);
    std::memcpy(desc.data(), title.data(), n);
    desc[n] = 0;
}

void ReplayCardEntry::SetCarName(const std::string& name) { // 0x800690B8
    if (name.size() < 32) {
        std::memcpy(desc.data() + 0x20, name.data(), name.size());
        desc[0x20 + name.size()] = 0;
    } else { // strncpy(+0x20, name, 31); +0x3F = '?' (0x3F)
        std::memcpy(desc.data() + 0x20, name.data(), 31);
        desc[0x3F] = 0x3F;
    }
}

// ---------------------------------------------------------------- the file

ReplayCardFile ReplayCardFile::Create(int blocks, std::span<const uint8_t> header) {
    if (blocks < 2 || blocks > 15) throw std::runtime_error("replay file: block count " + std::to_string(blocks));
    ReplayCardFile f;
    f.bytes_.assign(size_t(blocks) * kReplayCardBlock, 0);
    if (header.size() >= 0x200) std::memcpy(f.bytes_.data(), header.data(), 0x200);
    f.bytes_[0] = 'S', f.bytes_[1] = 'C';
    f.bytes_[2] = 0x13;
    f.bytes_[3] = uint8_t(blocks);
    f.bytes_[kReplayDirCount] = 0;
    const int16_t total = int16_t(blocks * 64 - 43); // 0x8006911C
    f.Put16(kReplayDirTotal, total);
    for (size_t i = 0; i < kReplayTableSlots; i++) f.Put16(kReplayDirTable + 2 + i * 2, kReplaySectorFree); // 0x80068E2C
    f.Put16(kReplayDirTable, total);
    f.UpdateCrc();
    return f;
}

ReplayCardFile ReplayCardFile::FromBytes(std::span<const uint8_t> bytes) {
    if (bytes.size() < kReplayDataStart) throw std::runtime_error("replay file: shorter than its directory");
    ReplayCardFile f;
    f.bytes_.assign(bytes.begin(), bytes.end());
    return f;
}

int ReplayCardFile::FreeSectors() const {
    const int n = I16(kReplayDirTable);
    int free = 0;
    for (int i = 0; i < n && i < int(kReplayTableSlots); i++) free += Next(i) == kReplaySectorFree ? 1 : 0;
    return free;
}

int ReplayCardFile::UsedSectors() const {
    const int n = I16(kReplayDirTable);
    int used = 0;
    for (int i = 0; i < n && i < int(kReplayTableSlots); i++) used += Next(i) != kReplaySectorFree ? 1 : 0;
    return used;
}

ReplayCardEntry ReplayCardFile::Entry(int index) const {
    if (index < 0 || index >= int(kReplayMaxEntries)) throw std::runtime_error("replay file: entry " + std::to_string(index));
    const uint8_t* e = bytes_.data() + EntryOffset(index);
    ReplayCardEntry r;
    std::memcpy(r.desc.data(), e, 0x50);
    r.first = int16_t(e[0x50] | e[0x51] << 8);
    r.sectors = int16_t(e[0x52] | e[0x53] << 8);
    r.size = int32_t(Le32(e + 0x54));
    r.crc = Le32(e + 0x58);
    return r;
}

bool ReplayCardFile::Valid() const { // 0x800691DC
    const uint8_t blocks = bytes_[3];
    const int8_t count = int8_t(bytes_[kReplayDirCount]);
    if (uint32_t(int32_t(count)) >= 33) return false;
    const int16_t total = Total();
    int sum = 0;
    for (int i = 0; i < count; i++) {
        const ReplayCardEntry e = Entry(i);
        if (total <= e.first) return false;
        sum += e.sectors;
    }
    const bool ok = UsedSectors() == sum && total == I16(kReplayDirTable) && blocks >= 2 && blocks < 16;
    return ok && Le32(bytes_.data() + kReplayDirCrc) == Crc32(std::span<const uint8_t>(bytes_.data(), kReplayDirCrc));
}

std::vector<int16_t> ReplayCardFile::Chain(int16_t first) const { // 0x80068F50
    std::vector<int16_t> chain;
    int16_t s = first;
    for (size_t guard = 0; guard <= kReplayTableSlots; guard++) {
        chain.push_back(s);
        if (s < 0 || size_t(s) >= kReplayTableSlots) return {};
        s = Next(s);
        if (s == kReplaySectorFree) return {};
        if (s == kReplaySectorLast) return chain;
    }
    return {};
}

std::vector<uint8_t> ReplayCardFile::EntryData(int index) const {
    const ReplayCardEntry e = Entry(index);
    std::vector<uint8_t> out;
    int16_t s = e.first;
    for (int k = 0; k < e.sectors; k++) { // 0x80020E14: sectors copies following the table
        const size_t at = kReplayDataStart + size_t(uint16_t(s)) * kReplaySectorSize;
        if (at + kReplaySectorSize > bytes_.size()) throw std::runtime_error("replay file: sector outside the file");
        out.insert(out.end(), bytes_.begin() + std::ptrdiff_t(at), bytes_.begin() + std::ptrdiff_t(at + kReplaySectorSize));
        s = Next(s);
    }
    return out;
}

bool ReplayCardFile::EntryCrcOk(int index) const { // 0x800692DC
    const ReplayCardEntry e = Entry(index);
    const std::vector<uint8_t> data = EntryData(index);
    if (e.size < 0 || size_t(e.size) > data.size()) return false;
    return Crc32(std::span<const uint8_t>(data.data(), size_t(e.size))) == e.crc;
}

int ReplayCardFile::Fits(int index, int32_t size) const { // 0x80069358
    int free = FreeSectors();
    if (index < 0) {
        if (Count() == int(kReplayMaxEntries)) return 1;
    } else {
        free += Entry(index).sectors;
    }
    return free < int((uint32_t(size) + 0x7F) >> 7) ? 2 : 0;
}

bool ReplayCardFile::Store(int index, const std::array<uint8_t, 0x50>& desc, std::span<const uint8_t> payload) { // 0x80069418
    const int32_t size = int32_t(payload.size());
    if (Fits(index, size) != 0) return false;
    const bool added = index < 0;
    if (added) {
        index = Count();
    } else { // 0x80068EBC: free the old chain
        int16_t s = Entry(index).first;
        for (size_t guard = 0; guard <= kReplayTableSlots && s >= 0 && size_t(s) < kReplayTableSlots; guard++) {
            const int16_t next = Next(s);
            Put16(kReplayDirTable + 2 + size_t(s) * 2, kReplaySectorFree);
            if (next == kReplaySectorLast) break;
            s = next;
        }
    }
    const int needed = int((uint32_t(size) + 0x7F) >> 7);
    std::vector<int16_t> list; // 0x80068EE4: the first free slots in table order
    const int n = I16(kReplayDirTable);
    for (int i = 0; i < n && int(list.size()) < needed; i++)
        if (Next(i) == kReplaySectorFree) list.push_back(int16_t(i));
    if (int(list.size()) < needed) return false;
    for (size_t k = 0; k < list.size(); k++) // 0x80068E50
        Put16(kReplayDirTable + 2 + size_t(list[k]) * 2, k + 1 == list.size() ? kReplaySectorLast : list[k + 1]);
    for (size_t k = 0; k < list.size(); k++) { // 0x80069758 -> 0x8007D658(7): the data, sector by sector (a partial sector's rest zero)
        const size_t at = kReplayDataStart + size_t(list[k]) * kReplaySectorSize;
        if (at + kReplaySectorSize > bytes_.size()) throw std::runtime_error("replay file: sector outside the file");
        std::memset(bytes_.data() + at, 0, kReplaySectorSize);
        const size_t from = k * kReplaySectorSize;
        std::memcpy(bytes_.data() + at, payload.data() + from, std::min(kReplaySectorSize, payload.size() - from));
    }
    uint8_t* e = bytes_.data() + EntryOffset(index);
    std::memcpy(e, desc.data(), 0x50);
    e[0x50] = uint8_t(list.empty() ? 0 : list[0]), e[0x51] = uint8_t(uint16_t(list.empty() ? 0 : list[0]) >> 8);
    e[0x52] = uint8_t(list.size()), e[0x53] = uint8_t(list.size() >> 8);
    PutLe32(e + 0x54, uint32_t(size));
    PutLe32(e + 0x58, Crc32(payload));
    if (added) bytes_[kReplayDirCount] = uint8_t(bytes_[kReplayDirCount] + 1);
    UpdateCrc();
    return true;
}

void ReplayCardFile::Remove(int index) { // 0x800695DC
    int16_t s = Entry(index).first;
    for (size_t guard = 0; guard <= kReplayTableSlots && s >= 0 && size_t(s) < kReplayTableSlots; guard++) {
        const int16_t next = Next(s);
        Put16(kReplayDirTable + 2 + size_t(s) * 2, kReplaySectorFree);
        if (next == kReplaySectorLast) break;
        s = next;
    }
    for (int i = index; i < Count() - 1; i++) std::memmove(bytes_.data() + EntryOffset(i), bytes_.data() + EntryOffset(i + 1), kReplayEntrySize);
    bytes_[kReplayDirCount] = uint8_t(bytes_[kReplayDirCount] - 1);
}

void ReplayCardFile::SetEntryTitle(int index, const std::string& title) { // 0x80069028
    if (index < 0 || index >= int(kReplayMaxEntries)) throw std::runtime_error("replay file: entry " + std::to_string(index));
    ReplayCardEntry e = Entry(index);
    e.SetTitle(title);
    std::memcpy(bytes_.data() + EntryOffset(index), e.desc.data(), 0x20);
}

void ReplayCardFile::UpdateCrc() { PutLe32(bytes_.data() + kReplayDirCrc, Crc32(std::span<const uint8_t>(bytes_.data(), kReplayDirCrc))); }

// ---------------------------------------------------------------- payload

std::vector<uint8_t> PackReplayPayload(const ReplayPayload& p) { // 0x80069948
    std::vector<uint8_t> out;
    auto put = [&](std::span<const uint8_t> bytes, size_t size) {
        const size_t at = out.size();
        out.resize(at + Round4(size), 0);
        std::memcpy(out.data() + at, bytes.data(), std::min(size, bytes.size()));
    };
    put(p.race, kReplayPayloadRace);
    if (p.GameMode() == 6) {
        put(p.results6, kReplayPayloadResults);
        const uint8_t count = uint8_t(p.ghosts.size());
        put(std::span<const uint8_t>(&count, 1), 1); // 0x80069918: one byte, 4 bytes on the file
        for (const auto& g : p.ghosts) {
            put(g.first, kReplayPayloadGhostHead);
            const size_t used = g.second.size() >= 0x12 ? size_t(g.second[0x10] | g.second[0x11] << 8) : 0;
            put(g.second, used + ReplayStream::kHeaderSize);
        }
        return out;
    }
    for (const ReplayPayload::Player& pl : p.players) {
        if (p.GameMode() != 0) put(pl.params, kReplayPayloadParams);
        put(pl.results, kReplayPayloadResults);
        const size_t used = pl.stream.size() >= 0x12 ? size_t(pl.stream[0x10] | pl.stream[0x11] << 8) : 0;
        put(pl.stream, used + ReplayStream::kHeaderSize);
    }
    return out;
}

ReplayPayload UnpackReplayPayload(std::span<const uint8_t> bytes) { // 0x80069AC4
    ReplayPayload p;
    size_t at = 0;
    auto take = [&](size_t size) {
        if (at + size > bytes.size()) throw std::runtime_error("replay payload: too short");
        std::vector<uint8_t> v(bytes.begin() + std::ptrdiff_t(at), bytes.begin() + std::ptrdiff_t(at + size));
        at += Round4(size);
        return v;
    };
    auto takeStream = [&] { // the length is the source's own "used" field (+0x10) + 0x19
        if (at + 0x12 > bytes.size()) throw std::runtime_error("replay payload: too short");
        const size_t used = size_t(bytes[at + 0x10] | bytes[at + 0x11] << 8);
        return take(used + ReplayStream::kHeaderSize);
    };
    const std::vector<uint8_t> race = take(kReplayPayloadRace);
    std::copy(race.begin(), race.end(), p.race.begin());
    if (p.GameMode() == 6) {
        p.results6 = take(kReplayPayloadResults);
        const int count = int(int8_t(take(1)[0])); // 0x8006992C: s8 -> 0x801D5F84 (s16)
        for (int k = 0; k < count; k++) {
            std::vector<uint8_t> head = take(kReplayPayloadGhostHead);
            p.ghosts.emplace_back(std::move(head), takeStream());
        }
        return p;
    }
    const int players = p.GameMode() == 0 ? 2 : 1;
    for (int i = 0; i < players; i++) {
        ReplayPayload::Player pl;
        if (p.GameMode() != 0) pl.params = take(kReplayPayloadParams);
        pl.results = take(kReplayPayloadResults);
        pl.stream = takeStream();
        p.players.push_back(std::move(pl));
    }
    return p;
}

ReplayFile ReplayPayload::ToReplayFile() const {
    if (GameMode() == 6 || players.empty()) throw std::runtime_error("replay: a game mode 6 record (ghosts) is not a replay");
    ReplayFile r;
    std::memcpy(r.raceBlock.data(), race.data(), kReplayRaceBlockSize);
    if (r.CarCount() == 0 || r.CarCount() > kReplayCars) throw std::runtime_error("replay: car count " + std::to_string(r.CarCount()));
    for (size_t i = 0; i < r.CarCount(); i++) {
        ReplayEntry e;
        std::memcpy(e.slot.data(), race.data() + kReplayRaceBlockSize + i * kReplayCarStride, kReplayCarStride);
        std::memcpy(&e.carId, e.slot.data(), 4);
        r.cars.push_back(e);
    }
    r.stream = players[0].stream;
    if (GameMode() == 0 && players.size() > 1) r.stream2 = players[1].stream; // 0x80069AC4: the second player of mode 0
    return r;
}

ReplayPayload PayloadOfReplay(const ReplayFile& replay, std::span<const uint8_t> params, std::span<const uint8_t> results, std::span<const uint8_t> results2) {
    ReplayPayload p;
    std::memcpy(p.race.data(), replay.raceBlock.data(), kReplayRaceBlockSize);
    for (size_t i = 0; i < replay.cars.size() && i < kReplayCars; i++)
        std::memcpy(p.race.data() + kReplayRaceBlockSize + i * kReplayCarStride, replay.cars[i].slot.data(), kReplayCarStride);
    ReplayPayload::Player pl;
    if (p.GameMode() != 0) {
        pl.params.assign(kReplayPayloadParams, 0);
        std::memcpy(pl.params.data(), params.data(), std::min(params.size(), kReplayPayloadParams));
    }
    pl.results.assign(kReplayPayloadResults, 0);
    std::memcpy(pl.results.data(), results.data(), std::min(results.size(), kReplayPayloadResults));
    const size_t used = replay.stream.size() >= 0x12 ? size_t(replay.stream[0x10] | replay.stream[0x11] << 8) : 0;
    pl.stream.assign(replay.stream.begin(), replay.stream.begin() + std::ptrdiff_t(std::min(replay.stream.size(), used + ReplayStream::kHeaderSize)));
    p.players.push_back(std::move(pl));
    if (p.GameMode() == 0) { // 0x80069948 packs both players of mode 0 (0x801D5E88 / 0x801DA3A0 + 0x4518: results, stream)
        ReplayPayload::Player second;
        second.results.assign(kReplayPayloadResults, 0);
        std::memcpy(second.results.data(), results2.data(), std::min(results2.size(), kReplayPayloadResults));
        const std::vector<uint8_t>& s2 = replay.stream2.empty() ? replay.stream : replay.stream2; // no second stream: player 1's again
        const size_t used2 = s2.size() >= 0x12 ? size_t(s2[0x10] | s2[0x11] << 8) : 0;
        second.stream.assign(s2.begin(), s2.begin() + std::ptrdiff_t(std::min(s2.size(), used2 + ReplayStream::kHeaderSize)));
        p.players.push_back(std::move(second));
    }
    return p;
}

std::vector<uint8_t> PackGhostRecord(const ReplayGhostRecord& g) { // 0x80069CC0 (0x80069890: memcpy, the pointer + the size rounded to 4)
    std::vector<uint8_t> out;
    auto put = [&](const uint8_t* p, size_t n) {
        const size_t at = out.size();
        out.resize(at + Round4(n), 0);
        std::memcpy(out.data() + at, p, n);
    };
    put(g.entry.data(), g.entry.size());
    put(g.params.data(), g.params.size());
    put(g.head.data(), g.head.size());
    const size_t used = g.stream.size() >= 0x12 ? size_t(g.stream[0x10] | g.stream[0x11] << 8) : 0;
    const size_t n = std::min(g.stream.size(), used + 0x19);
    put(g.stream.data(), n);
    return out;
}

ReplayGhostRecord UnpackGhostRecord(std::span<const uint8_t> bytes) { // 0x80069D58
    ReplayGhostRecord g;
    size_t at = 0;
    auto take = [&](uint8_t* p, size_t n) {
        if (at + n > bytes.size()) throw std::runtime_error("ghost record: too short");
        std::memcpy(p, bytes.data() + at, n);
        at += Round4(n);
    };
    take(g.entry.data(), g.entry.size());
    take(g.params.data(), g.params.size());
    take(g.head.data(), g.head.size());
    if (at + 0x12 > bytes.size()) throw std::runtime_error("ghost record: too short");
    const size_t used = size_t(bytes[at + 0x10] | bytes[at + 0x11] << 8); // the stream object's own +0x10 (+ 0x19)
    g.stream.resize(used + 0x19);
    take(g.stream.data(), g.stream.size());
    return g;
}

std::array<uint8_t, 0x50> ReplayDescription(const ReplayPayload& payload, const std::string& title) { // 0x800724F8
    ReplayCardEntry e;
    e.SetTitle(title);
    e.desc[0x40] = payload.race[9];
    e.desc[0x41] = payload.race[0xA];
    e.desc[0x42] = 0;
    std::memcpy(e.desc.data() + 0x44, payload.race.data() + 0x40, 4); // 0x801D589C
    std::memcpy(e.desc.data() + 0x48, payload.race.data() + 0x5C, 4); // 0x801D58B8
    const char* name = reinterpret_cast<const char*>(payload.race.data() + 0x5C + 0x90); // 0x801D5948
    e.SetCarName(std::string(name, strnlen(name, 0x40)));
    return e.desc;
}

std::vector<uint8_t> BuildReplayCardHeader(const GuestImage& exe, int blocks) {
    std::vector<uint8_t> h(0x200, 0);
    h[0] = 'S', h[1] = 'C', h[2] = 0x13, h[3] = uint8_t(blocks);
    // 0x8007D370: EUC-JP (JIS + 0x8080) pairs -> Shift-JIS (0x80085858) until a NUL byte, into +4 (64 bytes cleared)
    const uint32_t title = exe.Sim(0x80091ACCu);
    for (size_t i = 0, out = 4; out + 2 <= 0x44; i += 2) {
        const uint8_t* p = exe.At(title + uint32_t(i), 2);
        if (p[0] == 0) break;
        const uint32_t j1 = p[0] & 0x7Fu, j2 = p[1] & 0x7Fu;
        h[out++] = uint8_t(((j1 + 1) >> 1) + (j1 <= 0x5E ? 0x70 : 0xB0));
        h[out++] = uint8_t((j1 & 1) ? j2 + (j2 > 0x5F ? 0x20 : 0x1F) : j2 + 0x7E);
    }
    std::memcpy(h.data() + 0x60, exe.At(exe.Sim(0x80091AECu), 0x20), 0x20);
    std::memcpy(h.data() + 0x80, exe.At(exe.Sim(0x80091B0Cu), 0x180), 0x180);
    return h;
}

ReplayFile LoadReplayEntry(const ReplayCardFile& file, int index) {
    if (index < 0 || index >= file.Count()) throw std::runtime_error("replay file: no replay " + std::to_string(index) + " (" + std::to_string(file.Count()) + " in the file)");
    if (!file.EntryCrcOk(index)) throw std::runtime_error("replay file: replay " + std::to_string(index) + " fails its CRC");
    std::vector<uint8_t> data = file.EntryData(index);
    data.resize(size_t(file.Entry(index).size));
    return UnpackReplayPayload(data).ToReplayFile();
}

ReplayFile LoadReplay(std::span<const uint8_t> bytes, int index) {
    if (bytes.size() == 128 * 1024 && bytes[0] == 'M' && bytes[1] == 'C') {
        const std::vector<uint8_t> file = ReadReplayCardFile(bytes);
        if (file.empty()) throw std::runtime_error(std::string("no ") + kReplayCardFileName + " on the memory card");
        return LoadReplayEntry(ReplayCardFile::FromBytes(file), index);
    }
    if (bytes.size() >= kReplayDataStart && bytes[0] == 'S' && bytes[1] == 'C') {
        const ReplayCardFile file = ReplayCardFile::FromBytes(bytes);
        if (file.Valid()) return LoadReplayEntry(file, index);
    }
    if (index != 0) throw std::runtime_error("replay: not a replay file with a directory (only replay 0 can be read)");
    return ParseReplayFile(bytes);
}

// ---------------------------------------------------------------- card images

namespace {
constexpr size_t kFrame = 128, kCardBlocks = 15, kCardImageSize = 128 * 1024;
uint8_t FrameChecksum(const uint8_t* f) {
    uint8_t x = 0;
    for (size_t i = 0; i < kFrame - 1; i++) x ^= f[i];
    return x;
}
} // namespace

std::vector<uint8_t> ReadReplayCardFile(std::span<const uint8_t> card) {
    for (const MemoryCardFile& f : ReadMemoryCardFiles(card))
        if (f.name == kReplayCardFileName) return f.bytes;
    return {};
}

int CardImageFreeBlocks(std::span<const uint8_t> card) {
    int used = 0;
    for (size_t e = 1; e <= kCardBlocks; e++) used += (Le32(card.data() + e * kFrame) & 0xF0) == 0x50 ? 1 : 0;
    return int(kCardBlocks) - used;
}

void StoreReplayCardFile(std::vector<uint8_t>& card, std::span<const uint8_t> file) {
    if (card.size() != kCardImageSize || card[0] != 'M' || card[1] != 'C') throw std::runtime_error("not a PS1 memory card image");
    const size_t blocks = (file.size() + kReplayCardBlock - 1) / kReplayCardBlock;
    std::vector<size_t> chain;
    for (size_t i = 1; i <= kCardBlocks; i++) { // the existing file's chain
        const uint8_t* f = card.data() + i * kFrame;
        if (Le32(f) != 0x51 || std::strncmp(reinterpret_cast<const char*>(f + 10), kReplayCardFileName, 20) != 0) continue;
        size_t b = i;
        for (size_t guard = 0; guard < kCardBlocks; guard++) {
            chain.push_back(b);
            const uint16_t next = uint16_t(card[b * kFrame + 8] | card[b * kFrame + 9] << 8);
            if (next == 0xFFFF) break;
            b = size_t(next) + 1;
            if (b < 1 || b > kCardBlocks) throw std::runtime_error("memory card: broken block chain");
        }
        break;
    }
    if (chain.size() != blocks) {
        for (size_t b : chain) { // free the old file (0xA0 = free)
            uint8_t* f = card.data() + b * kFrame;
            std::memset(f, 0, kFrame);
            PutLe32(f, 0xA0);
            f[8] = f[9] = 0xFF;
            f[kFrame - 1] = FrameChecksum(f);
        }
        chain.clear();
        for (size_t i = 1; i <= kCardBlocks && chain.size() < blocks; i++)
            if ((Le32(card.data() + i * kFrame) & 0xF0) == 0xA0) chain.push_back(i);
        if (chain.size() < blocks) throw std::runtime_error("memory card: not enough free blocks for the replay file");
        for (size_t k = 0; k < chain.size(); k++) {
            uint8_t* f = card.data() + chain[k] * kFrame;
            std::memset(f, 0, kFrame);
            PutLe32(f, k == 0 ? 0x51 : (k + 1 == chain.size() ? 0x53 : 0x52));
            PutLe32(f + 4, k == 0 ? uint32_t(blocks * kReplayCardBlock) : 0);
            const uint16_t next = k + 1 == chain.size() ? 0xFFFF : uint16_t(chain[k + 1] - 1);
            f[8] = uint8_t(next), f[9] = uint8_t(next >> 8);
            if (k == 0) std::memcpy(f + 10, kReplayCardFileName, std::strlen(kReplayCardFileName));
            f[kFrame - 1] = FrameChecksum(f);
        }
    }
    for (size_t k = 0; k < chain.size(); k++) {
        uint8_t* dst = card.data() + chain[k] * kReplayCardBlock;
        std::memset(dst, 0, kReplayCardBlock);
        const size_t at = k * kReplayCardBlock;
        if (at < file.size()) std::memcpy(dst, file.data() + at, std::min(kReplayCardBlock, file.size() - at));
    }
}

} // namespace gt2
