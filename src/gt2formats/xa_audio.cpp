#include "gt2formats/xa_audio.h"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace gt2 {
namespace {

constexpr int32_t kFilterPos[4] = {0, 60, 115, 98};
constexpr int32_t kFilterNeg[4] = {0, 0, -52, -55};
constexpr size_t kDataOffset = 24; // sync 12 + header 4 + subheader 8
constexpr int kGroups = 18, kGroupSize = 128, kRows = 28;

int16_t DecodeSample(int32_t t, int shift, int filter, int32_t& old, int32_t& older) {
    int32_t s = t >> shift;
    // psx-spx's "(... + 32) / 64", taken as a C division (truncating toward zero) like the runtime SPU (src/machine/spu.cpp):
    // gt2verify row XaDecode matches it sample for sample. An arithmetic shift instead differs by up to 80 units on
    // the music (the filter feeds the rounding back); which one the hardware does is not established here.
    s += (old * kFilterPos[filter] + older * kFilterNeg[filter] + 32) / 64;
    s = std::clamp(s, -0x8000, 0x7FFF);
    older = old;
    old = s;
    return int16_t(s);
}

} // namespace

XaFormat XaFormatOf(uint8_t coding) {
    XaFormat f;
    f.stereo = (coding & 3) == 1;
    f.sampleRate = ((coding >> 2) & 3) == 1 ? 18900 : 37800;
    f.bitsPerSample = ((coding >> 4) & 3) == 1 ? 8 : 4;
    return f;
}

size_t DecodeXaSector(const uint8_t* raw, XaDecoderState& state, std::vector<int16_t>& out) {
    const XaFormat format = XaFormatOf(raw[19]);
    const int units = format.bitsPerSample == 4 ? 8 : 4;
    const int channels = format.stereo ? 2 : 1;
    size_t frames = 0;
    for (int group = 0; group < kGroups; group++) {
        const uint8_t* g = raw + kDataOffset + size_t(group) * kGroupSize;
        for (int unit = 0; unit < units; unit += channels) {
            int16_t decoded[2][kRows];
            for (int ch = 0; ch < channels; ch++) {
                const int u = unit + ch;
                const uint8_t param = g[4 + u];
                int shift = param & 0xF;
                if (shift > 12) shift = 9;
                const int filter = (param >> 4) & 3;
                for (int row = 0; row < kRows; row++) {
                    int32_t t;
                    if (format.bitsPerSample == 4) {
                        const int nibble = (g[16 + row * 4 + u / 2] >> ((u & 1) * 4)) & 0xF;
                        t = int32_t(int16_t(uint16_t(nibble << 12)));
                    } else {
                        t = int32_t(int16_t(uint16_t(g[16 + row * 4 + u] << 8)));
                    }
                    decoded[ch][row] = DecodeSample(t, shift, filter, state.old[ch], state.older[ch]);
                }
            }
            for (int row = 0; row < kRows; row++)
                for (int ch = 0; ch < channels; ch++) out.push_back(decoded[ch][row]);
            frames += kRows;
        }
    }
    return frames;
}

std::vector<MusicTrack> ReadMusicTable(const GuestImage& exe) {
    std::vector<MusicTrack> tracks(kMusicTrackCount);
    for (uint32_t i = 0; i < kMusicTrackCount; i++) {
        const uint32_t entry = exe.Sim(kMusicTableAddress) + i * 12; // the table of the executable's build
        tracks[i].first = exe.Get<uint32_t>(entry + 4);
        tracks[i].end = exe.Get<uint32_t>(entry + 8);
        if (tracks[i].end <= tracks[i].first) throw std::runtime_error("music table: entry " + std::to_string(i) + " has no sectors");
    }
    return tracks;
}

} // namespace gt2
