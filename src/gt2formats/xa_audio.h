#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "gt2formats/overlay_data.h"

// CD-XA ADPCM audio (the public Mode 2 Form 2 "XA audio" sector format, written from psx-spx) and the race music
// table of GT2's executable (docs/formats/sound.md section 6).
//
// Sector (2352 bytes raw): sync 12, header 4, subheader 8 (file, channel, submode, coding info, repeated), then 18
// sound groups of 128 bytes (16 header bytes + 112 data bytes). Coding info: bits 0-1 stereo, bits 2-3 sample rate
// (0 = 37800 Hz, 1 = 18900 Hz), bits 4-5 bits per sample (0 = 4, 1 = 8). A 4-bit group has 8 sound units of 28
// samples (stereo: even units left, odd units right), an 8-bit group 4 units; unit u's filter / shift byte is group
// byte 4 + u. Decoding: s = (t << 12 (4-bit) or << 8 (8-bit)) >> shift + (old * f0 + older * f1 + 32) / 64, clamped
// to 16 bits, with (f0, f1) = (0, 0), (60, 0), (115, -52), (98, -55); shifts above 12 act as 9.
namespace gt2 {

struct XaSubheader {
    uint8_t file = 0, channel = 0, submode = 0, coding = 0;
};

inline XaSubheader XaSubheaderOf(const uint8_t* raw2352) { return {raw2352[16], raw2352[17], raw2352[18], raw2352[19]}; }
// An XA audio sector (submode: audio + real-time, as the CD-ROM controller routes them to the SPU).
inline bool IsXaAudio(const XaSubheader& h) { return (h.submode & 0x44) == 0x44; }

struct XaFormat {
    bool stereo = false;
    int sampleRate = 37800;
    int bitsPerSample = 4;
};
XaFormat XaFormatOf(uint8_t coding);

// The ADPCM filter history, per channel (left / mono = 0, right = 1); it carries over from sector to sector.
struct XaDecoderState {
    int32_t old[2] = {0, 0}, older[2] = {0, 0};
};

// Decodes the 18 sound groups of one XA audio sector and appends the samples to `out` (stereo interleaved L, R;
// mono one sample per frame). Returns the number of frames appended (4-bit stereo: 2016, 4-bit mono: 4032,
// 8-bit stereo: 1008, 8-bit mono: 2016).
size_t DecodeXaSector(const uint8_t* raw2352, XaDecoderState& state, std::vector<int16_t>& out);

// ---- the race music table (US Simulation v1.2 executable)
//
// 0x800959C0: 21 entries of 12 bytes {u32 name pointer, u32 first sector, u32 end sector}, sectors relative to the
// start of the root file MUSIC.DAT (the executable keeps its LBA in 0x80095AC0 at run time, the table pointer in
// 0x80095ABC). Track `id` plays XA file 1, channel id + 1 (0x80080F24), from base + first to base + end.
constexpr uint32_t kMusicTableAddress = 0x800959C0u;
constexpr uint32_t kMusicTrackCount = 21;
constexpr uint8_t kMusicXaFile = 1;

struct MusicTrack {
    uint32_t first = 0, end = 0; // sectors relative to MUSIC.DAT
};

std::vector<MusicTrack> ReadMusicTable(const GuestImage& exe);
inline uint8_t MusicXaChannel(int id) { return uint8_t(id + 1); }
// 0x8008103C: the track's length in whole seconds, rounded up ((end - first + 74) / 75).
inline uint32_t MusicTrackSeconds(const MusicTrack& t) { return (t.end - t.first + 0x4Au) / 0x4Bu; }

} // namespace gt2
