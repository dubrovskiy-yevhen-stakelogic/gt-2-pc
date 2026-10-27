// GT4 'Tex1' texture sets and PS2 GS local-memory emulation. Format: docs/formats/tex1.md.
// Independent re-implementation of scripts/gt4_tex1.py (agreement checked by `gt4tool digest`).
#pragma once

#include <optional>
#include <memory>
#include <vector>

#include "gt4formats/common.h"

namespace gt4fmt {

enum : std::uint32_t {
    kPsmCt32 = 0x00,
    kPsmCt24 = 0x01,
    kPsmCt16 = 0x02,
    kPsmT8 = 0x13,
    kPsmT4 = 0x14,
};

// One 0x28-byte "pglu texture": raw 64-bit GS registers.
struct GsTextureRegs {
    std::uint64_t tex0, tex1, miptbp1, miptbp2, clamp;
};

struct GsTransfer {
    std::uint32_t offset;  // data offset from the Tex1 start
    std::uint16_t bp;
    std::uint8_t bw, psm;
    std::uint16_t w, h;
    Bytes data;
};

struct ClutPatch {
    std::uint16_t tex;
    std::uint8_t csa, cpsm;
    std::uint16_t cbp;  // relative to Tex1::baseTbp
};

struct Tex0Fields {
    std::uint32_t tbp0, tbw, psm, tw, th, tcc, tfx, cbp, cpsm, csm, csa, cld;
};
Tex0Fields tex0Fields(std::uint64_t tex0);

struct Tex1 {
    std::uint32_t size = 0;
    std::int16_t baseTbp = 0;
    std::uint16_t totalBlocks = 0;
    std::uint32_t animOffset = 0;  // +0x24, one file in the volume uses it (not decoded)
    std::vector<GsTextureRegs> textures;
    std::vector<GsTransfer> transfers;
    // nullopt = a set that cannot be applied the way FUN_00498ca0 reads it (see tex1.md)
    std::vector<std::optional<std::vector<ClutPatch>>> clutPatches;

    std::uint64_t variantTex0(std::size_t index, std::size_t variant) const;
    std::size_t numVariants() const { return clutPatches.empty() ? 1 : clutPatches.size(); }
};

// Parses a Tex1 that starts at `offset` inside `data` (all internal offsets are relative to that start).
Tex1 parseTex1(Bytes data, std::size_t offset);

class GsMemory {
public:
    GsMemory();
    void upload(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t w, std::uint32_t h, Bytes data);
    // Raw elements of a w x h rectangle at (0,0): u32 (CT32/24), u16 (CT16), index (T8/T4).
    std::vector<std::uint32_t> read(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t w,
                                    std::uint32_t h) const;

private:
    // 4 MB of GS local memory in 8 KB pages allocated (zeroed) on first write; a page never written reads as zero,
    // exactly like the zero-filled 4 MB array this replaces (clearing it cost every decode about 4 MB of writes).
    // Element accesses are size-aligned, so none crosses a page.
    static constexpr std::size_t kPageBytes = 8192;
    std::uint8_t* Write(std::size_t at);
    const std::uint8_t* Read(std::size_t at) const;  // null: page never written
    std::vector<std::unique_ptr<std::uint8_t[]>> pages_;
};

GsMemory buildGs(const Tex1& t);

struct Image {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint32_t> rgba;  // R | G<<8 | B<<16 | A<<24, raw GS alpha (0x80 = 1.0)
};

// Decodes a texture exactly like the GS samples it (full 2^TW x 2^TH). Throws FormatError.
Image decodeTexture(const GsMemory& gs, std::uint64_t tex0);

struct DigestStats {
    std::uint64_t models = 0, shapes = 0, packets = 0, vertices = 0, triangles = 0, texsets = 0, textures = 0,
                  texels = 0, texFail = 0, pats = 0, badClutSets = 0;
};

// Canonical texel stream (docs/formats/tex1.md "Canonical stream").
void tex1Stream(const Tex1& t, ByteWriter& out, DigestStats& stats);

}  // namespace gt4fmt
