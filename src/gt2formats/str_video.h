#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/overlay_data.h"
#include "gt2formats/xa_audio.h"
#include "gt2vfs/disc_image.h"

// Full-motion video of GT2 (docs/formats/str_video.md): the Arcade disc's STREAM.DAT, its sector / frame layout, the
// frame bitstream and the MDEC. Addresses: US Arcade v1.1 (SCUS_944.55 SHA-1 231f9dba..., GT2.OVL member 5 = the
// movie overlay, member 2 = the arcade menus); the Simulation disc has no movie file and never enters member 5.
//
// Pipeline of the original (member 5; member 2 holds a copy of the same player for the course previews):
//   CD stream (file 1: data sectors of any channel = video chunks, channel 7 = XA audio) -> sector callback
//   0x800106EC (32-byte header, 2016 data bytes per chunk, chunks in order, a frame complete at its last chunk) ->
//   frame decoder 0x80010AC0 (the EXE's GT-ZIP 0x80083C1C = gt2formats/course_map.h InflateGtZip of the per-block DC
//   words + an MPEG-1 style AC bitstream with the table member 5 0x800114CC) -> MDEC run-length codes -> libpress
//   DecDCTin / DecDCTout (DMA0 / DMA1) -> 16-pixel wide columns of macroblocks LoadImage'd to VRAM (0x800104C4).
namespace gt2 {

// ---------------------------------------------------------------- STREAM.DAT sectors

constexpr uint32_t kGtVideoMagic = 0x53490160u; // u16 0x0160, u16 0x5349 ("IS") at the start of the Form 1 data
constexpr size_t kGtVideoHeaderBytes = 32;
constexpr size_t kGtVideoChunkBytes = 2016; // 504 words read by 0x800106EC after the header

struct GtVideoSector {
    uint16_t chunk = 0, chunkCount = 0;
    uint32_t frame = 0;      // 1-based
    uint32_t usedBytes = 0;  // +0x0C (not read by the player)
    uint16_t frameCount = 0; // +0x10 (the player masks the u32 with 0x3FFFFFFF: the flags' top bits drop out)
    uint16_t flags = 0;      // +0x12: 0x8000 first chunk, 0x4000 last chunk
};
// A data sector (raw 2352 bytes) of the movie stream: nullopt for XA audio sectors and for data sectors without the
// magic (the padding sectors of the unused channels).
std::optional<GtVideoSector> ParseGtVideoSector(const uint8_t* raw2352);

// The movie table of the Arcade executable, 0x80092088: 28 u32 sector offsets into STREAM.DAT; movie i is read from
// offset[i] for offset[i + 1] - offset[i] - 25 sectors (0x800100A4 -> 0x800100F8: end LBA = start + count - 1).
constexpr uint32_t kArcadeMovieTableAddress = 0x80092088u;
constexpr int kArcadeMovieCount = 27;
// Movie numbers used by the code: 0..23 course previews (member 2 table 0x80053334[course]), 24 the intro (member 5
// 0x80011328 at boot), 25 / 26 the endings (member 5 0x800114E0(1 / 0), from the ENDING CREDITS view).
constexpr int kMovieIntro = 24, kMovieEndingA = 25, kMovieEndingB = 26;
struct StreamMovie {
    uint32_t first = 0, sectors = 0; // relative to STREAM.DAT
};
// Throws when the executable is not the Arcade build (the Simulation disc has no STREAM.DAT).
std::vector<StreamMovie> ReadStreamMovieTable(const GuestImage& exe);
// The course -> movie table of member 2 (0x80053334, u16 per .crsinfo course record, 29 entries on the disc).
constexpr uint32_t kCourseMovieTableAddress = 0x80053334u;
std::vector<uint16_t> ReadCourseMovieTable(const GuestImage& arcadeMenus);

// ---------------------------------------------------------------- frame bitstream

// The AC code table of member 5 (0x800114CC; member 2 keeps an identical copy at 0x80052734): u32 entries, index =
// leading zeros * 64 + the 6 bits after the first one; entry = code length << 26 | MDEC code. Zero counts 1..11 are
// used (0x800115CC..0x800120CC).
struct GtVlcTable {
    static constexpr int kZeroCounts = 12;
    std::array<uint32_t, kZeroCounts * 64> entries{};
};
constexpr uint32_t kMovieVlcTableAddress = 0x800114CCu;      // member 5
constexpr uint32_t kCourseMovieVlcTableAddress = 0x80052734u; // member 2
GtVlcTable ReadGtVlcTable(const GuestImage& overlay, uint32_t address);


// One frame (the bytes of its chunks) -> the MDEC input of 0x80010AC0: word 0 = u16 parameter words | u16 0x3800
// (the decode command's top half; DecDCTin sets the depth), then the run-length codes, padded with 0xFE00 to
// 4 + 4 * words bytes. Frame header: u16 words, u16 0x3800, u16 width, u16 height, u16 GT-ZIP bytes, GT-ZIP data (for
// each of the mb * 6 blocks the high byte, then the low byte of its DC / scale code), then the AC bitstream as
// 16-bit words, most significant bit first.
struct MdecFrameCodes {
    int width = 0, height = 0;
    std::vector<uint16_t> codes; // codes[0] = parameter words, codes[1] = 0x3800, then 2 * words halfwords
};
MdecFrameCodes GtFrameToMdecCodes(const uint8_t* frame, size_t size, const GtVlcTable& table);

// ---------------------------------------------------------------- MDEC (hardware model)

// The MDEC's decode as modelled here (docs/formats/str_video.md section 5; psx-spx "MDEC"): run-length codes ->
// dequantisation (DC * q[0]; AC (v * q[k] * scale + 4) >> 3; scale 0: v * 2, stored unzigzagged) clamped to
// -0x400..0x3FF -> two 1-D IDCT passes with the scale table (s16 >> 3), each (sum + 0xFFF) >> 13 -> YCbCr -> RGB
// (R = Y + (359 Cr + 128) >> 8, G = Y + (-88 Cb - 183 Cr + 128) >> 8, B = Y + (454 Cb + 128) >> 8, clamped to
// -128..127, unsigned output + 128; 15-bit = 8-bit >> 3). The same code runs in the interpreter's MDEC device
// (src/machine/mdec.*), so the native decode and the original's frames compare pixel for pixel.
class MdecCore {
public:
    enum Depth { k4Bit = 0, k8Bit = 1, k24Bit = 2, k15Bit = 3 };
    struct Output {
        Depth depth = k15Bit;
        bool isSigned = false, bit15 = false;
    };

    MdecCore();
    // Command 2: 64 luma bytes, then (colour) 64 chroma bytes, both in zigzag order.
    void SetQuantTable(const uint8_t* bytes, bool colour);
    // Command 3: 64 s16 of the IDCT matrix (row = frequency, column = position).
    void SetScaleTable(const int16_t* table);

    // Decodes one 8x8 block from codes[pos..count): leading 0xFE00 words are skipped. Returns the index after the
    // block's last code, or `count` + 1 when the codes end first (incomplete). `quant` 0 = luma, 1 = chroma.
    size_t DecodeBlock(const uint16_t* codes, size_t count, size_t pos, int quant, int16_t out[64]) const;
    // A colour macroblock (Cr, Cb, Y0..Y3 = top left, top right, bottom left, bottom right) -> the output words the
    // MDEC hands out for it (15-bit: 128 words, 24-bit: 192 words; 16 rows of 16 pixels).
    void ColourMacroblock(const int16_t cr[64], const int16_t cb[64], const int16_t y[4][64], const Output& out,
                          std::vector<uint32_t>& words) const;
    // A monochrome block (depth 4 / 8 bit): 8 rows of 8 samples -> 4-bit: 8 words, 8-bit: 16 words.
    void MonoBlock(const int16_t y[64], const Output& out, std::vector<uint32_t>& words) const;

    const std::array<uint8_t, 64>& Quant(int i) const { return quant_[size_t(i)]; }

private:
    void Idct(int16_t block[64]) const;
    std::array<std::array<uint8_t, 64>, 2> quant_{};
    std::array<int16_t, 64> scale_{};
};

// The libpress tables DecDCTReset sends (Arcade executable: 0x800A721C = command 0x40000001 + luma / chroma bytes,
// 0x800A72A0 = command 0x60000000 + 64 s16); the movies use them unchanged.
constexpr uint32_t kLibpressQuantAddress = 0x800A721Cu, kLibpressScaleAddress = 0x800A72A0u;
MdecCore MdecCoreFromExe(const GuestImage& exe);

// A whole frame through the MDEC (24-bit output, as the movie player's DecDCTin(buf, 1)) and arranged like the
// player's LoadImage of 16-pixel columns: `width` x `height` RGB bytes (the macroblocks run top to bottom, then left
// to right; the rows below `height` of the last macroblock row are not loaded).
struct MovieImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgb; // width * height * 3
};
MovieImage DecodeMovieFrame(const MdecCore& mdec, const MdecFrameCodes& codes, MdecCore::Depth depth = MdecCore::k24Bit);
// The same frame as 15-bit VRAM words (the course previews' DecDCTin(buf, 0)).
std::vector<uint16_t> DecodeMovieFrame15(const MdecCore& mdec, const MdecFrameCodes& codes);

// ---------------------------------------------------------------- reading a movie

// Sequential reader of one movie of STREAM.DAT: frames as the sector callback 0x800106EC assembles them (chunks in
// order from 0; a frame whose chunks break the order is dropped; the stream ends with the frame numbered >= the
// frame count) and the XA audio of channel 7 (file 1) decoded in sector order.
class GtMovieReader {
public:
    GtMovieReader(const DiscImage& disc, uint32_t streamLba, const StreamMovie& movie);

    struct Frame {
        uint32_t number = 0;       // 1-based
        uint32_t frameCount = 0;
        uint32_t lastSector = 0;   // relative to the movie's first sector: where the frame completed
        std::vector<uint8_t> data; // chunkCount * 2016 bytes
    };
    // The next complete frame; false at the end of the movie. Audio sectors met on the way are decoded into Audio().
    bool NextFrame(Frame& frame);
    // One sector (the stream's pace: 150 per second at the drive's double speed): true when it completed a frame.
    bool ReadSector(Frame& frame);
    bool AtEnd() const { return ended_ || next_ >= count_; }
    // Decoded XA samples so far (stereo interleaved at `AudioRate()` Hz), taken by the caller.
    std::vector<int16_t>& Audio() { return audio_; }
    int AudioRate() const { return audioRate_; }
    bool AudioStereo() const { return audioStereo_; }
    uint32_t SectorsRead() const { return next_; }
    uint32_t AudioSectors() const { return audioSectors_; }
    bool Ended() const { return ended_; }

private:
    const DiscImage& disc_;
    uint32_t lba_ = 0, count_ = 0, next_ = 0;
    std::vector<uint8_t> building_;
    uint32_t buildingFrame_ = 0, received_ = 0;
    bool dropping_ = false, ended_ = false;
    std::vector<int16_t> audio_;
    XaDecoderState xa_;
    int audioRate_ = 37800;
    bool audioStereo_ = true;
    uint32_t audioSectors_ = 0;
};

// The movie file of the disc: STREAM.DAT's LBA (the executable keeps it in 0x801D8E2C at run time).
uint32_t StreamFileLba(const DiscImage& disc);

} // namespace gt2
