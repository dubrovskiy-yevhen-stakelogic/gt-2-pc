#include "game/audio/sequencer.h"

#include <algorithm>

namespace gt2::audio {

uint32_t Sequencer::Vlv(uint32_t& at) const { // MIDI-style variable length: 7 bits per byte, bit 7 = more
    const std::vector<uint8_t>& d = file_->bytes;
    auto byte = [&]() -> uint32_t { return at < d.size() ? d[at++] : 0u; };
    uint32_t v = byte();
    if (v & 0x80) {
        v &= 0x7F;
        for (;;) {
            const uint32_t b = byte();
            v = v * 128 + (b & 0x7F);
            if (!(b & 0x80)) break;
        }
    }
    return v;
}

void Sequencer::Start(const SeqgFile& file, size_t index, const InstBank& bank, const std::vector<InstProgram>& programs, int32_t master) { // 0x8007A300
    Stop();
    if (index >= file.sequences.size()) return;
    file_ = &file;
    bank_ = &bank;
    programs_ = &programs;
    const SeqgSequence& s = file.sequences[index];
    master_ = master;
    volume_ = s.volume;
    effective_ = (master_ * volume_) >> 14;
    tempo_ = s.tempo;
    accumulator_ = int32_t(kIncrementPerField);
    for (int i = 0; i < kTracks; i++) {
        Track t;
        t.at = s.tracks[size_t(i)];
        t.countdown = int32_t(Vlv(t.at));
        t.volume = 0x4000;
        t.effective = (effective_ * t.volume) >> 14;
        tracks_[size_t(i)] = t;
    }
    playing_ = true;
}

void Sequencer::Stop() { // 0x8007A4D8: the slots lose their player; the next Tick releases them
    playing_ = false;
    for (Slot& s : slots_) s.owned = false;
}

void Sequencer::SlotVolume(Slot& s, uint16_t out[2]) const { // 0x80079460 / 0x80079B64: the squared volume at the pan
    const Track& t = tracks_[size_t(s.track)];
    const int32_t k = (((t.effective * s.programVolume) >> 14) * int32_t(tables_.velocity[s.velocity & 0x7F])) >> 14;
    const int32_t v = (k * s.sample->volume) >> 14;
    const int32_t pan = std::clamp(int32_t(t.pan) + int32_t(s.programPan) + int32_t(s.sample->byte09) - 0x80, 0, 127);
    int16_t lr[2];
    PanVolume(tables_.pan, lr, (v * v) >> 14, pan);
    out[0] = uint16_t(lr[0]);
    out[1] = uint16_t(lr[1]);
    s.effective = t.effective;
    s.pan = t.pan;
}

uint16_t Sequencer::SlotPitch(const Slot& s, int16_t bend) const { // the bend range in semitones: +0xE down, +0xF up
    const int32_t range = bend < 0 ? s.sample->byte0E : s.sample->byte0F;
    return uint16_t(NotePitch(tables_.pitch, int32_t(s.note) * 256 + ((int32_t(bend) * range) >> 5), s.sample->baseNote));
}

void Sequencer::NoteOn(Track& t, int trackIndex, uint8_t note, uint8_t velocity, int32_t length, int ticks) { // 0x80079460
    if (onNote) onNote(trackIndex, note, velocity, length, t.program);
    if (!programs_ || t.program >= programs_->size()) return;
    const InstProgram& p = (*programs_)[t.program];
    for (uint8_t index : p.samples) {
        if (index >= bank_->samples.size()) continue;
        const InstSample& smp = bank_->samples[index];
        if (note < smp.note || note > smp.byte0D) continue; // the sample's key range (+0xC .. +0xD)
        int found = -1;
        for (int k = 0; k < kSlots; k++) { // circular search from the cursor for a slot without a voice
            const int i = (cursor_ + k) % kSlots;
            if (slots_[size_t(i)].handle < 0) {
                found = i;
                break;
            }
        }
        if (found < 0) continue; // no slot: the note is dropped
        cursor_ = (found + 1) % kSlots;
        Slot& s = slots_[size_t(found)];
        s = Slot{};
        s.owned = true;
        s.track = trackIndex;
        s.sample = &smp;
        s.note = note;
        s.velocity = velocity;
        s.programPan = p.pan;
        s.programVolume = p.volume;
        s.countdown = length + ticks + t.countdown;
        s.bend = t.bend;
        uint16_t lr[2];
        SlotVolume(s, lr);
        VoiceRequest r;
        r.volumeLeft = lr[0];
        r.volumeRight = lr[1];
        r.pitch = SlotPitch(s, t.bend);
        r.address = smp.address;
        r.adsr1 = smp.adsr1;
        r.adsr2 = smp.adsr2;
        r.duration = smp.duration;
        r.priority = smp.priority;
        r.flags = smp.flags;
        if (onVoice) onVoice(r);
        driver_.Play(&s.handle, r);
    }
}

void Sequencer::Event(Track& t, int ticks) { // 0x80079744: one event, then the delta to the next
    const std::vector<uint8_t>& d = file_->bytes;
    auto byte = [&]() -> uint8_t { return t.at < d.size() ? d[t.at++] : uint8_t(0); };
    const uint8_t b = byte();
    const int trackIndex = int(&t - tracks_.data());
    if (b & 0x80) { // note, velocity, length
        const uint8_t velocity = byte();
        const int32_t length = int32_t(Vlv(t.at));
        NoteOn(t, trackIndex, uint8_t(b & 0x7F), velocity, length, ticks);
    } else if (b & 0x40) { // pitch bend: 14 bits, centre 0x2000
        const uint8_t lo = byte();
        t.bend = int16_t(uint16_t(((((uint32_t(b) << 8) | lo) & 0x3FFF) + 0xE000) & 0xFFFF));
    } else if (b & 0x20) { // loop start, count (b & 0x1F) + 1; the loop returns to the delta after this byte
        if (t.depth < kLoopDepth - 1) {
            t.depth++;
            t.loopCount[size_t(t.depth)] = uint8_t((b & 0x1F) + 1);
            t.loopAt[size_t(t.depth)] = t.at;
        }
    } else {
        switch (b) {
        case 0x00: t.alive = false; return; // end of the track, no delta
        case 0x01: {                          // loop start with a count (0xFF = forever)
            const uint8_t count = byte();
            if (t.depth < kLoopDepth - 1) {
                t.depth++;
                t.loopCount[size_t(t.depth)] = count;
                t.loopAt[size_t(t.depth)] = t.at;
            }
            break;
        }
        case 0x02: // loop end
            if (t.depth >= 0) {
                const uint8_t c = t.loopCount[size_t(t.depth)];
                if (c == 0) {
                    t.depth--;
                } else {
                    if (c != 0xFF) t.loopCount[size_t(t.depth)] = uint8_t(c - 1);
                    t.at = t.loopAt[size_t(t.depth)];
                }
            }
            break;
        case 0x03: t.program = byte(); break;
        case 0x04: { // volume: the velocity curve's value
            const uint8_t v = byte();
            t.volume = tables_.velocity[v & 0x7F];
            t.effective = (effective_ * t.volume) >> 14;
            break;
        }
        case 0x05: t.pan = byte(); break;
        case 0x06: { // tempo, 24 bits big-endian
            const uint32_t b1 = byte(), b2 = byte(), b3 = byte();
            tempo_ = (b1 << 16) | (b2 << 8) | b3;
            break;
        }
        default: break;
        }
    }
    t.countdown += int32_t(Vlv(t.at));
}

void Sequencer::Tick() {
    int ticks = 0;
    if (playing_) { // 0x80079A38
        if (accumulator_ > 0) {
            do {
                ticks++;
                accumulator_ -= int32_t(tempo_);
            } while (accumulator_ > 0 && tempo_ != 0);
        }
        accumulator_ += int32_t(kIncrementPerField);
        for (Track& t : tracks_) {
            if (!t.alive) continue;
            t.countdown -= ticks;
            while (t.alive && t.countdown < 1) Event(t, ticks);
        }
    }
    for (Slot& s : slots_) { // 0x80079B64
        if (s.handle < 0) continue;
        if (!s.owned || !playing_) {
            driver_.Release(&s.handle);
            driver_.Detach(&s.handle);
            s.owned = false;
            continue;
        }
        s.countdown -= ticks;
        if (s.countdown < 1 && !s.released) {
            driver_.Release(&s.handle);
            s.released = true;
        }
        const Track& t = tracks_[size_t(s.track)];
        if (t.bend != s.bend) {
            s.bend = t.bend;
            driver_.SetPitch(&s.handle, SlotPitch(s, t.bend));
        }
        if (t.effective != s.effective || t.pan != s.pan) {
            uint16_t lr[2];
            SlotVolume(s, lr);
            driver_.SetVolume(&s.handle, lr[0], lr[1]);
        }
    }
}

} // namespace gt2::audio
