#pragma once
#include <cstdint>
#include <span>
#include <stdexcept>
namespace gt2view {
// Standard ASTC file, restricted to the block-aligned generated mip atlas.
inline std::span<const uint8_t> GeneratedAstcPayload(std::span<const uint8_t> bytes,uint32_t side) {
    auto u24=[&](size_t at){return uint32_t(bytes[at])|(uint32_t(bytes[at+1])<<8)|(uint32_t(bytes[at+2])<<16);};
    if(bytes.size()<16 || bytes[0]!=0x13 || bytes[1]!=0xab || bytes[2]!=0xa1 || bytes[3]!=0x5c ||
       bytes[4]!=4 || bytes[5]!=4 || bytes[6]!=1 || u24(7)!=side || u24(10)!=side || u24(13)!=1 ||
       bytes.size()!=16+size_t((side+3)/4)*((side+3)/4)*16)
        throw std::runtime_error("invalid generated ASTC atlas");
    return bytes.subspan(16);
}
}
