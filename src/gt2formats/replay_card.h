#pragma once
// The replay file of GT2 (US Simulation v1.2, SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): the
// memory-card file "BASCUS-94455REPLAY" (EXE 0x80091AB8) that the race overlay's "Save Replay" writes and the title's
// Replay Theater reads, and the demo files on the disc (arcade/demofile*.gmr) - the same format: a small file system of
// up to 32 replays in 128-byte sectors. Evidence: our disassembly / Ghidra pseudo-C of the EXE's replay-file class
// (0x80068E2C..0x80069FF8) and card manager (0x8006F298.., modes 0..3), a replay the original saved in our
// interpreter (work/play/theater/card_b1_replay.mcd: licence B-1, every CRC matching) and the four demo files.
// docs/formats/replay.md section 9.
//
// File (blocks x 0x2000 bytes; created by 0x8006911C with 3..15 blocks, "Select Number of Blocks"):
//   +0x0000  "SC" save header: +2 icon flags 0x13, +3 block count, +4 Shift-JIS title (EXE 0x80091ACC), +0x60 CLUT
//            (0x80091AEC, 32 bytes), +0x80 three icon frames (0x80091B0C.., 3 x 128)
//   +0x0200  s8 replay count (<= 32)          +0x0202 s16 data sectors = blocks * 64 - 43
//   +0x0204  the sector table: s16 sector count (= +0x202), s16 next[960] (0x3C0 free, -1 = the last sector of a chain)
//   +0x0988  32 entries x 0x5C (below)
//   +0x1508  u32 CRC-32 (0x80083178) of bytes 0..0x1507
//   +0x1580  the data sectors (sector s at 0x1580 + s * 0x80)
// Entry (0x5C): +0x00 char title[32] (the name typed on "Save Replay"; "No Name" before), +0x20 char car[32] (race slot
// 0's name; +0x3F = 0x3F when it was cut to 31 characters: drawn with ".."; the demo files hold other bytes after the name), +0x40 u8 race block + 9 (0x801D5865),
// +0x41 u8 game mode (0x801D5866), +0x42 u8 ghost (1: a mode-6 ghost, "Cannot Play Ghost"), +0x44 u32 race block + 0x40
// (the course file id), +0x48 u32 slot 0's car id, +0x4C u32 (not written by 0x800724F8), +0x50 s16 first sector,
// +0x52 s16 sectors, +0x54 s32 payload bytes, +0x58 u32 CRC-32 of the payload.
// Payload (0x80069948 packs / 0x80069AC4 unpacks; every piece padded to 4 bytes): RAM 0x801D585C (0x58C bytes: the race
// block, the six car slots, ...), then per player (two in game mode 0, else one): the car's parameter record
// 0x801DE8BA + i * 0x1C0 (not in game mode 0), the results record 0x801D5E88 + i * 0x4518 (0xFC) and the
// input stream object 0x801D5F84 + i * 0x4518 (header 0x19 + used bytes). Game mode 6: the results record, the ghost
// count (1 byte in 4), per ghost 0xE0 bytes of 0x801D5F88 + k * 0x10FC and its stream object right after them.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/replay.h"

namespace gt2 {
struct GuestImage;
}

namespace gt2 {

constexpr const char* kReplayCardFileName = "BASCUS-94455REPLAY"; // EXE 0x80091AB8
constexpr size_t kReplayCardBlock = 0x2000;
constexpr size_t kReplayDirCount = 0x200, kReplayDirTotal = 0x202, kReplayDirTable = 0x204, kReplayDirEntries = 0x988, kReplayDirCrc = 0x1508;
constexpr size_t kReplayDataStart = 0x1580, kReplaySectorSize = 0x80, kReplayTableSlots = 960, kReplayEntrySize = 0x5C, kReplayMaxEntries = 32;
constexpr int16_t kReplaySectorFree = 0x3C0, kReplaySectorLast = -1;
constexpr size_t kReplayPayloadRace = 0x58C, kReplayPayloadParams = 0x1C0, kReplayPayloadResults = 0xFC, kReplayPayloadGhostHead = 0xE0;

// One entry of the directory (0x5C bytes at +0x988 + i * 0x5C).
struct ReplayCardEntry {
    std::array<uint8_t, 0x50> desc{}; // +0x00..+0x4F (see above)
    int16_t first = 0, sectors = 0;
    int32_t size = 0;
    uint32_t crc = 0;

    std::string Title() const;        // +0x00 (NUL-terminated, up to 32 bytes)
    std::string CarName() const;      // +0x20 as 0x80069048 prints it (+0x3F set: 31 characters at most, ".." after 31)
    uint8_t RaceFlag() const { return desc[0x40]; }
    uint8_t GameMode() const { return desc[0x41]; }
    bool Ghost() const { return desc[0x42] == 1; }       // 0x8006932C: playable = +0x42 != 1
    uint32_t CourseId() const;        // +0x44
    uint32_t CarId() const;           // +0x48
    void SetTitle(const std::string& title);            // 0x80069028: strcpy (the bytes after the terminator stay)
    void SetCarName(const std::string& name);           // 0x800690B8
};

// The whole file image.
class ReplayCardFile {
public:
    // 0x8006911C: a new file of `blocks` blocks (3..15) with the save header `header` (0x200 bytes: "SC", icon flags,
    // title, CLUT, icons; +3 is set to the block count), no replays, every sector free.
    static ReplayCardFile Create(int blocks, std::span<const uint8_t> header);
    // A file read from a card / the disc (the bytes as they are; Valid() checks them).
    static ReplayCardFile FromBytes(std::span<const uint8_t> bytes);

    const std::vector<uint8_t>& Bytes() const { return bytes_; }
    int Blocks() const { return bytes_.size() >= 4 ? bytes_[3] : 0; }
    int Count() const { return int(int8_t(bytes_[kReplayDirCount])); }
    int16_t Total() const { return I16(kReplayDirTotal); }
    int16_t Next(int sector) const { return I16(kReplayDirTable + 2 + size_t(sector) * 2); }
    int FreeSectors() const;          // 0x80068FA8: table slots holding 0x3C0 among the first +0x204 ones
    int UsedSectors() const;          // 0x80068FE8
    ReplayCardEntry Entry(int index) const;
    // 0x800691DC: the directory is consistent (blocks 2..15, count <= 32, every entry's first sector below the total,
    // the entries' sectors add up to the used ones, total = the table's count, the directory CRC).
    bool Valid() const;
    // 0x80068F50: the chain of sectors of an entry (empty when it runs into a free slot).
    std::vector<int16_t> Chain(int16_t first) const;
    // 0x80020E14 / 0x800697E8 + 0x8007D658(6): the entry's sectors in chain order (sectors x 0x80 bytes).
    std::vector<uint8_t> EntryData(int index) const;
    // 0x800692DC: the CRC of the entry's payload (its first +0x54 bytes) matches +0x58.
    bool EntryCrcOk(int index) const;
    // 0x80069358: 0 = a payload of `size` bytes fits (replacing entry `index`, or as a new one with index < 0),
    // 1 = no new entry (32 files), 2 = not enough free sectors.
    int Fits(int index, int32_t size) const;
    // 0x80069418: stores `payload` as entry `index` (< 0: a new entry at the end) with the description `desc`: frees the
    // replaced entry's chain, takes the first free sectors in table order, links them, writes the data, the entry, its
    // CRC, the count and the directory CRC. False (nothing changed) when it does not fit.
    bool Store(int index, const std::array<uint8_t, 0x50>& desc, std::span<const uint8_t> payload);
    // 0x800695DC: frees the entry's chain and moves the later entries down (the directory CRC is not updated, as the
    // original: 0x800693EC does that before the write of mode 2).
    void Remove(int index);
    // 0x800693EC: the directory CRC.
    void UpdateCrc();
    // 0x80069028 on entry `index` (the name entry of "Rename & Delete"): strcpy of the title into +0x00 (the bytes after its
    // terminator stay; the directory CRC is not updated, as the original).
    void SetEntryTitle(int index, const std::string& title);

private:
    int16_t I16(size_t o) const { return int16_t(bytes_[o] | bytes_[o + 1] << 8); }
    void Put16(size_t o, int16_t v) { bytes_[o] = uint8_t(v), bytes_[o + 1] = uint8_t(uint16_t(v) >> 8); }
    std::vector<uint8_t> bytes_;
};

// The payload of one replay (0x80069948 / 0x80069AC4).
struct ReplayPayload {
    std::array<uint8_t, kReplayPayloadRace> race{}; // RAM 0x801D585C..
    struct Player {
        std::vector<uint8_t> params;  // 0x1C0 (empty in game mode 0)
        std::vector<uint8_t> results; // 0xFC
        std::vector<uint8_t> stream;  // the stream object: header 0x19 + used bytes
    };
    std::vector<Player> players;      // game mode 0: 2, 6: none, else 1
    // Game mode 6 (the arcade Time Trial): the results record and the ghosts {0xE0 bytes, stream object}.
    std::vector<uint8_t> results6;
    std::vector<std::pair<std::vector<uint8_t>, std::vector<uint8_t>>> ghosts;

    uint8_t RaceFlag() const { return race[9]; }
    uint8_t GameMode() const { return race[0xA]; }
    // The replay as gt2game plays it (race block, car slots, player 1's stream); throws for mode 6 (ghost records).
    ReplayFile ToReplayFile() const;
};
std::vector<uint8_t> PackReplayPayload(const ReplayPayload& payload);   // 0x80069948
ReplayPayload UnpackReplayPayload(std::span<const uint8_t> bytes);      // 0x80069AC4 (throws on a short payload)
// A payload of a gt2game replay: the race block and slots of `replay` (the rest of the 0x58C bytes zero), the given
// parameter record (0x1C0, or empty) and results record (0xFC, or zeros), player 1's stream; game mode 0: player 2's
// results record (`results2`, or zeros) and stream (ReplayFile::stream2).
ReplayPayload PayloadOfReplay(const ReplayFile& replay, std::span<const uint8_t> params, std::span<const uint8_t> results,
                              std::span<const uint8_t> results2 = {});
// The ghost file's payload (0x80069CC0 packs / 0x80069D58 unpacks; every piece padded to 4 bytes, the padding zero here, the
// buffer's residue in the original): race block entry 1 (0x801D5988, 0xD0), its parameter record (0x801DEA7A, 0x1C0), the
// reference lap's head (0x801DA4A0, 0xE0) and stream object (0x801DA580: 0x19 + its used bytes).
struct ReplayGhostRecord {
    std::array<uint8_t, 0xD0> entry{};
    std::array<uint8_t, 0x1C0> params{};
    std::array<uint8_t, 0xE0> head{};
    std::vector<uint8_t> stream;
};
std::vector<uint8_t> PackGhostRecord(const ReplayGhostRecord& g);   // 0x80069CC0
ReplayGhostRecord UnpackGhostRecord(std::span<const uint8_t> bytes); // 0x80069D58 (throws on a short payload)
// 0x800724F8 (a replay, not a ghost): the entry description of a payload: title "No Name" (0x8008FB14), + 0x40 race
// block + 9, + 0x41 game mode, + 0x42 = 0, + 0x44 race block + 0x40, + 0x48 slot 0's car id, + 0x20 slot 0's name.
std::array<uint8_t, 0x50> ReplayDescription(const ReplayPayload& payload, const std::string& title = "No Name");

// 0x8007D32C + 0x8006911C: the file's 0x200-byte save header from the EXE ("SC", icon flags 0x13, the block count,
// the title 0x80091ACC converted to Shift-JIS by 0x8007D370 / 0x80085858, the CLUT 0x80091AEC, the icons 0x80091B0C).
std::vector<uint8_t> BuildReplayCardHeader(const GuestImage& exe, int blocks);

// Entry `index` of a replay file as gt2game plays it (0x80020E14 gather + 0x80069AC4; throws when the entry is out of range,
// its CRC does not match or it is not a one-player replay).
ReplayFile LoadReplayEntry(const ReplayCardFile& file, int index);
// A replay from any of: a memory card image (.mcd with "BASCUS-94455REPLAY"), a replay file (demo files, gt2game's .gmr),
// the legacy .gmr layout (entry 0 only).
ReplayFile LoadReplay(std::span<const uint8_t> bytes, int index = 0);

// ---------------------------------------------------------------- memory card images (.mcd)

// The replay file of a card image (empty when the card has none).
std::vector<uint8_t> ReadReplayCardFile(std::span<const uint8_t> card);
// Writes `file` (whole blocks) as "BASCUS-94455REPLAY" into the card image: over the existing file's blocks when it has
// the same block count, else into free blocks (the old file's blocks freed first). Throws when the card has no room.
void StoreReplayCardFile(std::vector<uint8_t>& card, std::span<const uint8_t> file);
// Free blocks of a card image (15 minus the blocks of the files).
int CardImageFreeBlocks(std::span<const uint8_t> card);

} // namespace gt2
