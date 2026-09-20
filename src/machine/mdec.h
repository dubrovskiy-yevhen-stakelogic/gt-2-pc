#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

#include "gt2formats/str_video.h"

namespace gt2 {

// The MDEC (0x1F801820 / 0x1F801824, DMA0 in, DMA1 out) at the register level (psx-spx "MDEC"), decoding with the
// same model as the native movie player (gt2formats/str_video.h MdecCore). Transfers are instantaneous: a DMA0 block
// is decoded as it arrives and its macroblocks wait in the output FIFO for DMA1 / data reads.
class Mdec {
public:
    // 0x1F801820 write: a command word, or a parameter of the current command.
    void WriteCommand(uint32_t word);
    // 0x1F801824 write: bit 31 reset, bit 30 data-in request enable (DMA0), bit 29 data-out request enable (DMA1).
    void WriteControl(uint32_t word);
    uint32_t ReadData();   // 0x1F801820 read: the next output word (0 when empty)
    uint32_t ReadStatus() const; // 0x1F801824 read

    void DmaIn(const uint32_t* words, size_t count);
    void DmaOut(uint32_t* words, size_t count);

    // Counters for the reports.
    uint64_t macroblocks = 0, commands = 0, underflows = 0;

private:
    void Parameter(uint32_t word);
    void DecodeAvailable();

    MdecCore core_;
    uint32_t command_ = 0;     // the current command word
    uint32_t remaining_ = 0;   // parameter words still expected
    bool enableIn_ = false, enableOut_ = false;
    std::vector<uint16_t> codes_;   // decode command: received run-length codes not yet consumed
    size_t codePos_ = 0;
    int block_ = 0;                 // next block of the macroblock: 0 Cr, 1 Cb, 2..5 Y0..Y3 (monochrome: always Y)
    int16_t blocks_[6][64] = {};
    std::vector<uint32_t> table_;   // quant / scale command: collected parameter words
    std::deque<uint32_t> out_;
};

} // namespace gt2
