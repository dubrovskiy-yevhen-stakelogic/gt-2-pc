#include "machine/mdec.h"

#include <cstring>

namespace gt2 {

void Mdec::WriteControl(uint32_t word) {
    if (word & 0x80000000u) { // reset: abort the command, clear the FIFOs (status 0x80040000)
        command_ = 0;
        remaining_ = 0;
        codes_.clear();
        codePos_ = 0;
        block_ = 0;
        table_.clear();
        out_.clear();
    }
    enableIn_ = (word & 0x40000000u) != 0;
    enableOut_ = (word & 0x20000000u) != 0;
}

void Mdec::WriteCommand(uint32_t word) {
    if (remaining_ > 0) {
        Parameter(word);
        return;
    }
    commands++;
    command_ = word;
    codes_.clear();
    codePos_ = 0;
    block_ = 0;
    table_.clear();
    switch (word >> 29) {
    case 1: remaining_ = word & 0xFFFF; break;         // decode macroblocks
    case 2: remaining_ = (word & 1) ? 32 : 16; break;  // quant table(s)
    case 3: remaining_ = 32; break;                    // scale table
    default: remaining_ = 0; break;                    // no function
    }
}

void Mdec::Parameter(uint32_t word) {
    remaining_--;
    const uint32_t op = command_ >> 29;
    if (op == 1) {
        codes_.push_back(uint16_t(word));
        codes_.push_back(uint16_t(word >> 16));
        if (remaining_ == 0 || codes_.size() - codePos_ >= 512) DecodeAvailable();
        if (remaining_ == 0) { // the command ends: an unfinished block is dropped
            codes_.clear();
            codePos_ = 0;
            block_ = 0;
        }
        return;
    }
    table_.push_back(word);
    if (remaining_ != 0) return;
    uint8_t bytes[128];
    std::memcpy(bytes, table_.data(), table_.size() * 4);
    if (op == 2) core_.SetQuantTable(bytes, (command_ & 1) != 0);
    else core_.SetScaleTable(reinterpret_cast<const int16_t*>(bytes));
    table_.clear();
}

void Mdec::DecodeAvailable() {
    const MdecCore::Depth depth = MdecCore::Depth((command_ >> 27) & 3);
    MdecCore::Output o;
    o.depth = depth;
    o.isSigned = (command_ >> 26) & 1;
    o.bit15 = (command_ >> 25) & 1;
    const bool colour = depth == MdecCore::k24Bit || depth == MdecCore::k15Bit;
    std::vector<uint32_t> words;
    for (;;) {
        const int quant = colour && block_ < 2 ? 1 : 0;
        const size_t next = core_.DecodeBlock(codes_.data(), codes_.size(), codePos_, quant, blocks_[colour ? block_ : 0]);
        if (next > codes_.size()) break; // needs more codes
        codePos_ = next;
        if (!colour) {
            core_.MonoBlock(blocks_[0], o, words);
            macroblocks++;
            continue;
        }
        if (++block_ < 6) continue;
        block_ = 0;
        const int16_t(*y)[64] = &blocks_[2];
        core_.ColourMacroblock(blocks_[0], blocks_[1], y, o, words);
        macroblocks++;
    }
    out_.insert(out_.end(), words.begin(), words.end());
    if (codePos_ > 4096) { // keep the pending codes small
        codes_.erase(codes_.begin(), codes_.begin() + std::ptrdiff_t(codePos_));
        codePos_ = 0;
    }
}

uint32_t Mdec::ReadData() {
    if (out_.empty()) {
        underflows++;
        return 0;
    }
    const uint32_t w = out_.front();
    out_.pop_front();
    return w;
}

uint32_t Mdec::ReadStatus() const {
    uint32_t s = 0;
    if (out_.empty()) s |= 1u << 31;
    const bool busy = remaining_ > 0 || !out_.empty();
    if (busy) s |= 1u << 29;
    if (enableIn_ && remaining_ > 0) s |= 1u << 28;
    if (enableOut_ && !out_.empty()) s |= 1u << 27;
    s |= ((command_ >> 25) & 0xF) << 23; // depth, signed, bit 15 of the current command
    const int current = block_ < 2 ? 4 + block_ : block_ - 2;
    s |= uint32_t(current) << 16;
    s |= (remaining_ - 1) & 0xFFFF;
    return s;
}

void Mdec::DmaIn(const uint32_t* words, size_t count) {
    for (size_t i = 0; i < count; i++) WriteCommand(words[i]);
    if ((command_ >> 29) == 1 && remaining_ > 0) DecodeAvailable();
}

void Mdec::DmaOut(uint32_t* words, size_t count) {
    for (size_t i = 0; i < count; i++) words[i] = ReadData();
}

} // namespace gt2
