// GT4 Tex1 + GS local memory. See docs/formats/tex1.md for the layout and the CORE.GT4 addresses
// (FUN_00498b28 relocation, FUN_00498828 TBP/CBP rebasing, FUN_00499508 upload, FUN_0049cc48 bind,
// FUN_00498ca0 CLUT patch sets).
#include "gt4formats/tex1.h"

#include <algorithm>
#include <bit>

namespace gt4fmt {
namespace {

constexpr std::size_t kGsBytes = 4u * 1024u * 1024u;

// --- GS swizzle (GS User's Manual): pixel -> element inside a block, block inside a page.
std::uint32_t col32(std::uint32_t x, std::uint32_t y) {
    return (x & 1) | ((y & 1) << 1) | (((x >> 1) & 1) << 2) | ((x >> 2) << 3);
}
std::uint32_t block32(std::uint32_t bx, std::uint32_t by) {
    return (bx & 1) | ((by & 1) << 1) | (((bx >> 1) & 1) << 2) | (((by >> 1) & 1) << 3) | ((bx >> 2) << 4);
}
std::uint32_t block16(std::uint32_t bx, std::uint32_t by) {
    return (by & 1) | ((bx & 1) << 1) | (((by >> 1) & 1) << 2) | (((bx >> 1) & 1) << 3) | ((by >> 2) << 4);
}
std::uint32_t elemInBlock(std::uint32_t psm, std::uint32_t x, std::uint32_t y) {
    if (psm == kPsmCt32 || psm == kPsmCt24) return 16 * (y >> 1) + col32(x, y);
    if (psm == kPsmCt16) return 32 * (y >> 1) + 2 * col32(x & 7, y) + (x >> 3);
    std::uint32_t c = y >> 2, r = y & 3;
    std::uint32_t rot = ((r >> 1) ^ (c & 1)) << 2;
    std::uint32_t word = col32((x & 7) ^ rot, r & 1);
    std::uint32_t sub = (x >> 3) * 2 + (r >> 1);
    return psm == kPsmT8 ? c * 64 + word * 4 + sub : c * 128 + word * 8 + sub;
}

struct Geom {
    std::uint32_t pw, ph, bw, bh, epb;
    bool ct;  // CT formats: pages per row = BW; T8/T4: BW/2
    std::vector<std::uint32_t> table;  // ph x pw element offsets inside a page
};

const Geom& geom(std::uint32_t psm) {
    auto make = [](std::uint32_t p, std::uint32_t pw, std::uint32_t ph, std::uint32_t bw, std::uint32_t bh,
                   std::uint32_t epb, bool sixteen, bool ct) {
        Geom g{pw, ph, bw, bh, epb, ct, std::vector<std::uint32_t>(pw * ph)};
        for (std::uint32_t y = 0; y < ph; ++y)
            for (std::uint32_t x = 0; x < pw; ++x) {
                std::uint32_t blk = sixteen ? block16(x / bw, y / bh) : block32(x / bw, y / bh);
                g.table[y * pw + x] = blk * epb + elemInBlock(p, x % bw, y % bh);
            }
        return g;
    };
    static const Geom ct32 = make(kPsmCt32, 64, 32, 8, 8, 64, false, true);
    static const Geom ct16 = make(kPsmCt16, 64, 64, 16, 8, 128, true, true);
    static const Geom t8 = make(kPsmT8, 128, 64, 16, 16, 256, false, false);
    static const Geom t4 = make(kPsmT4, 128, 128, 32, 16, 512, true, false);
    switch (psm) {
        case kPsmCt32:
        case kPsmCt24: return ct32;
        case kPsmCt16: return ct16;
        case kPsmT8: return t8;
        case kPsmT4: return t4;
        default: throw FormatError("unsupported PSM");
    }
}

// Element addresses (relative to BP*elements_per_block) of a w x h rectangle at (0,0), row-major.
std::vector<std::uint32_t> addresses(std::uint32_t psm, std::uint32_t bw, std::uint32_t w, std::uint32_t h) {
    const Geom& g = geom(psm);
    std::uint32_t pageElems = g.epb * 32;
    std::uint32_t ppr = g.ct ? bw : (bw >> 1);
    if (ppr == 0) ppr = 1;
    std::vector<std::uint32_t> out(static_cast<std::size_t>(w) * h);
    std::size_t i = 0;
    for (std::uint32_t y = 0; y < h; ++y) {
        const std::uint32_t* trow = &g.table[(y % g.ph) * g.pw];
        std::uint32_t prow = (y / g.ph) * ppr;
        for (std::uint32_t x = 0; x < w; ++x) out[i++] = (prow + x / g.pw) * pageElems + trow[x % g.pw];
    }
    return out;
}

std::uint32_t ct16ToRgba(std::uint32_t c) {
    std::uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
    std::uint32_t a = (c & 0x8000) ? 0x80u : 0u;
    return r | (g << 8) | (b << 16) | (a << 24);
}

std::uint32_t transferBytes(std::uint32_t psm, std::uint32_t w, std::uint32_t h) {
    std::uint32_t bits;
    switch (psm) {
        case kPsmCt32: bits = 32; break;
        case kPsmCt24: bits = 24; break;
        case kPsmCt16:
        case 0x0A: bits = 16; break;
        case kPsmT8: bits = 8; break;
        case kPsmT4: bits = 4; break;
        default: throw FormatError("unsupported transfer PSM");
    }
    return static_cast<std::uint32_t>((static_cast<std::uint64_t>(w) * h * bits + 7) / 8);
}

}  // namespace

Tex0Fields tex0Fields(std::uint64_t t) {
    auto f = [t](int s, int n) { return static_cast<std::uint32_t>((t >> s) & ((1ull << n) - 1)); };
    return {f(0, 14), f(14, 6), f(20, 6), f(26, 4), f(30, 4), f(34, 1),
            f(35, 2), f(37, 14), f(51, 4), f(55, 1), f(56, 5), f(61, 3)};
}

std::uint64_t Tex1::variantTex0(std::size_t index, std::size_t variant) const {
    std::uint64_t t0 = textures.at(index).tex0;
    if (variant < clutPatches.size() && clutPatches[variant] && !clutPatches[variant]->empty()) {
        for (const ClutPatch& p : *clutPatches[variant]) {
            if (p.tex != index) continue;
            t0 &= ~((0xFull << 51) | (0x3FFFull << 37) | (0x1Full << 56));
            std::uint64_t cbp = static_cast<std::uint64_t>((p.cbp + baseTbp) & 0x3FFF);
            t0 |= (static_cast<std::uint64_t>(p.cpsm) << 51) | (cbp << 37) | (static_cast<std::uint64_t>(p.csa) << 56);
        }
    }
    return t0;
}

Tex1 parseTex1(Bytes b, std::size_t o) {
    need(b, o, 0x30, "Tex1 header");
    if (std::memcmp(&b[o], "Tex1", 4) != 0) throw FormatError("not a Tex1");
    Tex1 t;
    if (rd32(b, o + 4) != 0) throw FormatError("Tex1 stored relocation base != 0");
    t.size = rd32(b, o + 12);
    t.baseTbp = static_cast<std::int16_t>(rd16(b, o + 0x10));
    t.totalBlocks = rd16(b, o + 0x12);
    std::uint32_t ntex = rd16(b, o + 0x14), ntr = rd16(b, o + 0x16);
    std::uint32_t pTex = rd32(b, o + 0x18), pTr = rd32(b, o + 0x1c), pClut = rd32(b, o + 0x20);
    t.animOffset = rd32(b, o + 0x24);
    if (t.size > b.size() - o) throw FormatError("Tex1 size out of range");
    if (t.size < 0x30 && (ntex || ntr || pClut)) throw FormatError("Tex1 size too small for its tables");
    std::size_t end = o + t.size;
    for (std::uint32_t i = 0; i < ntex; ++i) {
        std::size_t p = o + pTex + i * 0x28ull;
        if (p + 0x28 > end) throw FormatError("pglu texture table outside set");
        t.textures.push_back({rd64(b, p), rd64(b, p + 8), rd64(b, p + 16), rd64(b, p + 24), rd64(b, p + 32)});
    }
    for (std::uint32_t i = 0; i < ntr; ++i) {
        std::size_t p = o + pTr + i * 12ull;
        if (p + 12 > end) throw FormatError("transfer table outside set");
        GsTransfer tr{rd32(b, p), rd16(b, p + 4), rd8(b, p + 6), rd8(b, p + 7), rd16(b, p + 8), rd16(b, p + 10), {}};
        std::uint32_t n = transferBytes(tr.psm, tr.w, tr.h);
        if (static_cast<std::uint64_t>(tr.offset) + n > t.size) throw FormatError("transfer data outside set");
        tr.data = b.subspan(o + tr.offset, n);
        t.transfers.push_back(tr);
    }
    if (pClut) {
        if (o + pClut + 4 > end) throw FormatError("clut patch table outside set");
        std::uint32_t n = rd32(b, o + pClut);
        if (n > 256 || o + pClut + 4 + 4ull * n > end) throw FormatError("bad clut patch count");
        for (std::uint32_t s = 0; s < n; ++s) {
            std::uint32_t q = rd32(b, o + pClut + 4 + 4ull * s);
            std::optional<std::vector<ClutPatch>> set;
            if (q != 0 && o + q + 4 <= end) {
                std::uint32_t k = rd32(b, o + q) & 0xFFFF;
                if (o + q + 4 + 4ull * k <= end) {
                    std::vector<ClutPatch> v;
                    bool ok = true;
                    for (std::uint32_t e = 0; e < k; ++e) {
                        std::uint32_t w = rd32(b, o + q + 4 + 4ull * e);
                        if ((w >> 23) >= ntex) {
                            ok = false;
                            break;
                        }
                        v.push_back({static_cast<std::uint16_t>(w >> 23), static_cast<std::uint8_t>(w & 31),
                                     static_cast<std::uint8_t>((w >> 19) & 15),
                                     static_cast<std::uint16_t>((w >> 5) & 0x3FFF)});
                    }
                    if (ok) set = std::move(v);
                }
            }
            t.clutPatches.push_back(std::move(set));
        }
    }
    return t;
}

GsMemory::GsMemory() : pages_(kGsBytes / kPageBytes) {}

std::uint8_t* GsMemory::Write(std::size_t at) {
    auto& page = pages_[at / kPageBytes];
    if (!page) page = std::make_unique<std::uint8_t[]>(kPageBytes);  // value-initialised: zero
    return page.get() + at % kPageBytes;
}

const std::uint8_t* GsMemory::Read(std::size_t at) const {
    const auto& page = pages_[at / kPageBytes];
    return page ? page.get() + at % kPageBytes : nullptr;
}

void GsMemory::upload(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t w, std::uint32_t h,
                      Bytes data) {
    const Geom& g = geom(psm);
    auto addrs = addresses(psm, bw, w, h);
    std::uint64_t base = static_cast<std::uint64_t>(bp) * g.epb;
    std::uint64_t maxa = addrs.empty() ? 0 : *std::max_element(addrs.begin(), addrs.end());
    std::uint64_t limit = (psm == kPsmCt32 || psm == kPsmCt24) ? kGsBytes / 4
                          : psm == kPsmCt16                     ? kGsBytes / 2
                          : psm == kPsmT8                       ? kGsBytes
                                                                : kGsBytes * 2;
    if (!addrs.empty() && base + maxa >= limit) throw FormatError("transfer outside GS memory");
    std::size_t n = addrs.size();
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t a = base + addrs[i];
        switch (psm) {
            case kPsmCt32: std::memcpy(Write(a * 4), &data[i * 4], 4); break;
            case kPsmCt24: std::memcpy(Write(a * 4), &data[i * 3], 3); break;
            case kPsmCt16: std::memcpy(Write(a * 2), &data[i * 2], 2); break;
            case kPsmT8: *Write(a) = data[i]; break;
            default: {
                std::uint8_t v = (data[i >> 1] >> ((i & 1) * 4)) & 15;
                std::uint8_t& m = *Write(a >> 1);
                m = (a & 1) ? static_cast<std::uint8_t>((m & 0x0F) | (v << 4)) : static_cast<std::uint8_t>((m & 0xF0) | v);
            }
        }
    }
}

std::vector<std::uint32_t> GsMemory::read(std::uint32_t psm, std::uint32_t bp, std::uint32_t bw, std::uint32_t w,
                                          std::uint32_t h) const {
    const Geom& g = geom(psm);
    auto addrs = addresses(psm, bw, w, h);
    std::uint64_t base = static_cast<std::uint64_t>(bp) * g.epb;
    std::uint64_t maxa = addrs.empty() ? 0 : *std::max_element(addrs.begin(), addrs.end());
    std::uint64_t limit = (psm == kPsmCt32 || psm == kPsmCt24) ? kGsBytes / 4
                          : psm == kPsmCt16                     ? kGsBytes / 2
                          : psm == kPsmT8                       ? kGsBytes
                                                                : kGsBytes * 2;
    if (!addrs.empty() && base + maxa >= limit) throw FormatError("texture outside GS memory");
    std::vector<std::uint32_t> out(addrs.size());
    for (std::size_t i = 0; i < addrs.size(); ++i) {
        std::uint64_t a = base + addrs[i];
        switch (psm) {
            case kPsmCt32:
            case kPsmCt24: {
                std::uint32_t v = 0;
                if (const std::uint8_t* p = Read(a * 4)) std::memcpy(&v, p, 4);
                out[i] = v;
                break;
            }
            case kPsmCt16: {
                const std::uint8_t* p = Read(a * 2);
                out[i] = p ? static_cast<std::uint32_t>(p[0] | (p[1] << 8)) : 0u;
                break;
            }
            case kPsmT8: {
                const std::uint8_t* p = Read(a);
                out[i] = p ? *p : 0u;
                break;
            }
            default: {
                const std::uint8_t* p = Read(a >> 1);
                out[i] = p ? (*p >> ((a & 1) * 4)) & 15 : 0u;
            }
        }
    }
    return out;
}

GsMemory buildGs(const Tex1& t) {
    GsMemory gs;
    for (const GsTransfer& tr : t.transfers) gs.upload(tr.psm, tr.bp, tr.bw, tr.w, tr.h, tr.data);
    return gs;
}

Image decodeTexture(const GsMemory& gs, std::uint64_t tex0) {
    Tex0Fields f = tex0Fields(tex0);
    if (f.tw > 10 || f.th > 10) throw FormatError("texture larger than 1024");
    Image img;
    img.w = 1u << f.tw;
    img.h = 1u << f.th;
    switch (f.psm) {
        case kPsmCt32: img.rgba = gs.read(kPsmCt32, f.tbp0, f.tbw, img.w, img.h); return img;
        case kPsmCt24:
            img.rgba = gs.read(kPsmCt24, f.tbp0, f.tbw, img.w, img.h);
            for (auto& v : img.rgba) v = (v & 0xFFFFFF) | 0x80000000u;
            return img;
        case kPsmCt16:
            img.rgba = gs.read(kPsmCt16, f.tbp0, f.tbw, img.w, img.h);
            for (auto& v : img.rgba) v = ct16ToRgba(v);
            return img;
        case kPsmT8:
        case kPsmT4: break;
        default: throw FormatError("unsupported texture PSM");
    }
    if (f.csm != 0) throw FormatError("CSM2 CLUT not supported");
    // CSM1 256-entry CLUT (FUN_0049cc48 loads a T8 CLUT when CLD != 0 and CSA != 0; CSA picks 16 entries).
    std::vector<std::uint32_t> img16;
    if (f.cpsm == kPsmCt32) {
        img16 = gs.read(kPsmCt32, f.cbp, 1, 16, 16);
    } else if (f.cpsm == kPsmCt16) {
        img16 = gs.read(kPsmCt16, f.cbp, 1, 16, 16);
        for (auto& v : img16) v = ct16ToRgba(v);
    } else {
        throw FormatError("unsupported CLUT format");
    }
    std::uint32_t clut[256];
    for (std::uint32_t e = 0; e < 256; ++e) clut[e] = img16[(e & 0xE7) | ((e & 0x08) << 1) | ((e & 0x10) >> 1)];
    std::uint32_t start = 0;
    if (f.psm == kPsmT4) {
        start = f.csa * 16;
        if (start + 16 > 256) throw FormatError("CSA outside 256-entry CLUT");
    }
    img.rgba = gs.read(f.psm, f.tbp0, f.tbw, img.w, img.h);
    for (auto& v : img.rgba) v = clut[start + v];
    return img;
}

void tex1Stream(const Tex1& t, ByteWriter& out, DigestStats& stats) {
    out.str("T1");
    out.u16(static_cast<std::uint32_t>(t.textures.size()));
    out.u16(static_cast<std::uint32_t>(t.transfers.size()));
    out.u16(static_cast<std::uint32_t>(t.clutPatches.size()));
    for (const auto& r : t.textures) {
        out.u64(r.tex0);
        out.u64(r.tex1);
        out.u64(r.miptbp1);
        out.u64(r.miptbp2);
        out.u64(r.clamp);
    }
    if (t.textures.empty()) return;
    GsMemory gs = buildGs(t);
    for (std::size_t v = 0; v < t.numVariants(); ++v) {
        if (v < t.clutPatches.size() && !t.clutPatches[v]) {
            out.u16(static_cast<std::uint32_t>(v));
            out.u16(0xFFFF);
            ++stats.badClutSets;
            continue;
        }
        for (std::size_t i = 0; i < t.textures.size(); ++i) {
            if (v > 0) {
                bool patched = false;
                if (v < t.clutPatches.size())
                    for (const auto& p : *t.clutPatches[v]) patched |= p.tex == i;
                if (!patched) continue;
            }
            try {
                Image img = decodeTexture(gs, t.variantTex0(i, v));
                out.u16(static_cast<std::uint32_t>(v));
                out.u16(static_cast<std::uint32_t>(i));
                out.u16(img.w);
                out.u16(img.h);
                static_assert(std::endian::native == std::endian::little, "texel stream assumes little endian");
                out.bytes(img.rgba.data(), img.rgba.size() * 4);
                ++stats.textures;
                stats.texels += static_cast<std::uint64_t>(img.w) * img.h;
            } catch (const FormatError&) {
                out.u16(static_cast<std::uint32_t>(v));
                out.u16(static_cast<std::uint32_t>(i));
                out.u16(0);
                out.u16(0);
                ++stats.texFail;
            }
        }
    }
}

}  // namespace gt4fmt
