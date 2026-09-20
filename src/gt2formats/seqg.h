#pragma once
// SEQG music sequences (sound/spu_02.seq .. spu_10.seq: the GT-mode menu music) and the program table of INST banks
// (sound/gtmseq.ins), as the US Simulation v1.2 executable (SCUS_944.88, SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a)
// reads them: relocation 0x8007A1C4, player 0x8007A300 / 0x80079A38 / 0x80079744 / 0x80079460 (game/audio/sequencer.h).
// Format and evidence: docs/formats/sound.md section 8.
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace gt2 {

// One sequence descriptor (0x48 bytes at +0x0C + i * 0x48): volume (0x4000 = 1.0), a word the player does not read,
// the tempo (accumulator units per tick: 480,000,000 / 60 are added per 60 Hz field) and 16 track offsets from the
// start of the file (0x8007A1C4 adds the file's address to the non-zero ones).
struct SeqgSequence {
    uint16_t volume = 0x4000;
    uint16_t word02 = 0;
    uint32_t tempo = 0;
    std::array<uint32_t, 16> tracks{};
};

// "SEQG", u32 self pointer (0 on the disc), u32 sequence count, then the descriptors; the track event streams follow.
struct SeqgFile {
    std::vector<uint8_t> bytes;
    std::vector<SeqgSequence> sequences;
};
// Throws std::runtime_error on a bad magic or offsets outside the file.
SeqgFile ParseSeqg(std::span<const uint8_t> file);

// INST table 1 (header +0x20 count, +0x24 offset): u32 offsets (from the start of the file) of the programs
// {u8 sample count, u8 pan, s16 volume (0x4000 = 1.0), u8 sample index[count]} (0x80079460).
struct InstProgram {
    uint8_t pan = 0x40;
    int16_t volume = 0x4000;
    std::vector<uint8_t> samples;
};
std::vector<InstProgram> ParseInstPrograms(std::span<const uint8_t> file);

} // namespace gt2
