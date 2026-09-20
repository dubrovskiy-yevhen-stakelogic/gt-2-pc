#pragma once
#include "game/audio/mixer.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstddef>
namespace gt2::audio {
template<class Source, class BufferState> void ResampleAudio(Source& mixer, BufferState& b, int16_t* out, int32_t frames) {
    const double ratio = double(kSampleRate) / double(b.streamRate); // source frames per output frame
    // How many source frames this block needs (plus one for the interpolation's right-hand sample).
    if (frames <= 0) return;
    const size_t needed = size_t(std::floor(b.position + ratio * double(frames - 1))) + 2;
    const size_t buffered = b.mixed.size() / 2;
    if (buffered < needed) {
        b.mixed.resize(needed * 2, 0.0f);
        mixer.Mix(b.mixed.data() + buffered * 2, needed - buffered);
    }

    for (int32_t i = 0; i < frames; i++) {
        const double at = b.position + ratio * double(i);
        const size_t index = size_t(at);
        const float t = float(at - double(index));
        for (int c = 0; c < 2; c++) {
            const float a = b.mixed[index * 2 + size_t(c)];
            const float s = a + (b.mixed[(index + 1) * 2 + size_t(c)] - a) * t;
            out[i * 2 + c] = int16_t(std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f));
        }
    }
    // Carry the position (and the frame it sits in) into the next block: no sample is produced twice or skipped.
    const double end = b.position + ratio * double(frames);
    const size_t consumed = size_t(end);
    b.mixed.erase(b.mixed.begin(), b.mixed.begin() + std::ptrdiff_t(consumed * 2));
    b.position = end - double(consumed);
}

}
