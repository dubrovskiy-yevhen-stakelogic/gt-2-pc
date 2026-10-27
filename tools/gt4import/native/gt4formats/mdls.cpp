// GT4 MDLS (ModelSet2). Layout and CORE.GT4 addresses: docs/formats/mdls.md
// (FUN_00454410 init, FUN_00455708 command interpreter, FUN_0049d308 shape call, FUN_0049e548 shape scale,
// FUN_0049cea0 texture/material binding).
#include "gt4formats/mdls.h"
#include "game/math/vu0_matrix.h"

#include <cstring>
#include "game/math/motion_skin.h"

#include <algorithm>
#include <functional>
#include <set>

namespace gt4fmt {
namespace {

// Operand byte counts of the render opcodes; -1 = variable length.
int operandBytes(std::uint8_t op) {
    static const int table[0x5B] = {
        0,  0,  1,  2,  1,  2,  -1, -1, 1,  2,  -1, 0,  0,  0,  1,  64,  // 0x00
        64, 12, 12, 16, 4,  4,  4,  0,  0,  1,  0,  0,  2,  0,  0,  1,   // 0x10
        2,  4,  0,  0,  4,  0,  0,  4,  16, 1,  1,  1,  2,  0,  0,  1,   // 0x20
        0,  12, 4,  16, 4,  4,  4,  16, 0,  0,  0,  0,  0,  0,  0,  0,   // 0x30
        0,  3,  3,  0,  2,  2,  2,  0,  2,  2,  2,  2,  2,  -1, 0,  0,   // 0x40
        1,  2,  16, 16, 8,  8,  0,  0,  0,  1,  2};                       // 0x50
    return op < 0x5B ? table[op] : 0;  // the interpreter's default case skips unknown opcodes
}

std::int32_t s8at(Bytes b, std::size_t p) { return static_cast<std::int8_t>(rd8(b, p)); }
std::int32_t s16at(Bytes b, std::size_t p) { return static_cast<std::int16_t>(rd16(b, p)); }

// Branch targets and fall-through of the command at p.
void commandTargets(Bytes b, std::size_t p, std::vector<std::int64_t>& targets, bool& fall) {
    std::uint8_t op = rd8(b, p);
    targets.clear();
    fall = true;
    std::int64_t ip = static_cast<std::int64_t>(p);
    switch (op) {
        case 0x00: fall = false; return;
        case 0x08: targets.push_back(ip + 1 + s8at(b, p + 1)); fall = false; return;
        case 0x09: targets.push_back(ip + 1 + s16at(b, p + 1)); fall = false; return;
        case 0x06:
        case 0x4D: {
            std::uint32_t n = rd8(b, p + 3);
            for (std::uint32_t i = 0; i < n; ++i) targets.push_back(ip + 4 + 2 * i + s16at(b, p + 4 + 2 * i));
            return;
        }
        case 0x07: {
            std::uint32_t n = rd8(b, p + 0x11);
            for (std::uint32_t i = 0; i < n; ++i)
                targets.push_back(ip + 0x12 + 2 * i + s16at(b, p + 0x12 + 2 * i));
            fall = false;
            return;
        }
        case 0x0A: {
            std::size_t q = p + 2 + 12ull * rd8(b, p + 1);
            targets.push_back(static_cast<std::int64_t>(q) + s16at(b, q));
            return;
        }
        default: return;
    }
}

std::vector<Command> decodeCommands(Bytes b, std::size_t start, std::size_t limit) {
    std::map<std::size_t, Command> seen;
    std::vector<std::int64_t> work{static_cast<std::int64_t>(start)}, targets;
    while (!work.empty()) {
        std::int64_t p = work.back();
        work.pop_back();
        while (true) {
            if (p < 0 || static_cast<std::uint64_t>(p) >= limit) throw FormatError("command offset outside model set");
            std::size_t up = static_cast<std::size_t>(p);
            if (seen.count(up)) break;
            std::size_t ln = commandLength(b, up);
            if (up + ln > limit) throw FormatError("command runs past model set");
            seen[up] = {static_cast<std::uint32_t>(up), rd8(b, up), static_cast<std::uint16_t>(ln)};
            bool fall;
            commandTargets(b, up, targets, fall);
            work.insert(work.end(), targets.begin(), targets.end());
            if (!fall) break;
            p += static_cast<std::int64_t>(ln);
        }
    }
    std::vector<Command> out;
    for (auto& kv : seen) out.push_back(kv.second);
    return out;
}

struct Slot {
    std::array<std::uint32_t, 4> v{};
    std::uint8_t mask = 0;  // written components
    std::uint8_t vn = 0, bits = 0, usn = 0;
    bool exists = false;
};

void put16(std::vector<std::uint8_t>& c, std::uint32_t v) {
    c.push_back(static_cast<std::uint8_t>(v));
    c.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& c, std::uint32_t v) {
    put16(c, v);
    put16(c, v >> 16);
}

VifPacket decodePacket(Bytes b, std::size_t p, std::uint32_t qwc, std::uint32_t initMask) {
    static thread_local std::vector<Slot> mem(1280);
    static thread_local std::vector<std::uint32_t> touched;
    for (std::uint32_t a : touched) mem[a] = Slot{};
    touched.clear();
    VifPacket pk;
    std::vector<std::uint8_t>& canon = pk.canon;
    std::size_t end = p + qwc * 16ull;
    std::uint32_t mode = 0, mask = initMask;
    std::array<std::uint32_t, 4> row{};
    bool rowSet = false;
    while (p < end) {
        std::uint32_t code = rd32(b, p);
        p += 4;
        std::uint32_t imm = code & 0xFFFF, num = (code >> 16) & 0xFF, cmd = (code >> 24) & 0x7F;
        if (cmd >= 0x60) {
            std::uint32_t vn = ((cmd >> 2) & 3) + 1, vl = cmd & 3, masked = (cmd >> 4) & 1;
            static const std::uint32_t kBits[4] = {32, 16, 8, 5};
            std::uint32_t bits = kBits[vl];
            if (bits == 5) throw FormatError("V4-5 unpack");
            std::uint32_t n = num ? num : 256;
            std::size_t size = (vn * bits * n + 31) / 32 * 4;
            if (p + size > end) throw FormatError("unpack data past packet end");
            std::uint32_t usn = (imm >> 14) & 1, addr = imm & 0x3FF;
            if (mode != 0 && !rowSet) throw FormatError("STMOD without STROW in packet");
            put16(canon, addr);
            canon.push_back(static_cast<std::uint8_t>(vn));
            canon.push_back(static_cast<std::uint8_t>(bits));
            canon.push_back(static_cast<std::uint8_t>(usn));
            canon.push_back(static_cast<std::uint8_t>(mode));
            put16(canon, n);
            canon.push_back(static_cast<std::uint8_t>(masked));
            canon.push_back(0);
            need(b, p, size, "unpack data");
            for (std::uint32_t i = 0; i < n; ++i) {
                std::uint32_t a = addr + i;
                Slot& slot = mem[a];
                if (!slot.exists) {
                    slot.exists = true;
                    touched.push_back(a);
                }
                std::uint32_t comps[4];
                for (std::uint32_t e = 0; e < vn; ++e) {
                    std::size_t q = p + (static_cast<std::size_t>(i) * vn + e) * (bits / 8);
                    std::uint32_t v;
                    if (bits == 32) v = rd32(b, q);
                    else if (bits == 16) v = usn ? rd16(b, q) : static_cast<std::uint32_t>(static_cast<std::int16_t>(rd16(b, q)));
                    else v = usn ? rd8(b, q) : static_cast<std::uint32_t>(static_cast<std::int8_t>(rd8(b, q)));
                    comps[e] = v;
                }
                std::uint32_t ntargets = vn;
                if (vn == 1) {
                    comps[1] = comps[2] = comps[3] = comps[0];
                    ntargets = 4;
                }
                bool firstIsX = false, any = false;
                for (std::uint32_t e = 0; e < ntargets; ++e) {
                    if (masked) {
                        std::uint32_t m = (mask >> (e * 2)) & 3;
                        if (m == 3) continue;
                        if (m != 0) throw FormatError("unsupported STMASK mode");
                    }
                    std::uint32_t v = comps[e];
                    if (mode == 1) {
                        v += row[e];
                    } else if (mode == 2) {
                        v += row[e];
                        row[e] = v;
                    }
                    slot.v[e] = v;
                    slot.mask |= static_cast<std::uint8_t>(1u << e);
                    if (!any) firstIsX = e == 0;
                    any = true;
                    canon.push_back(static_cast<std::uint8_t>(e));
                    put32(canon, v);
                }
                if (!masked || (any && firstIsX)) {
                    slot.vn = static_cast<std::uint8_t>(vn);
                    slot.bits = static_cast<std::uint8_t>(bits);
                    slot.usn = static_cast<std::uint8_t>(usn);
                }
            }
            p += size;
        } else if (cmd == 0x00) {
        } else if (cmd == 0x05) {
            mode = imm & 3;
            if (mode == 3) throw FormatError("STMOD 3");
        } else if (cmd == 0x20) {
            mask = rd32(b, p);
            p += 4;
            if (mask != 0x3F3F3F3F) throw FormatError("unexpected STMASK");
        } else if (cmd == 0x30) {
            for (int e = 0; e < 4; ++e) row[e] = rd32(b, p + 4 * e);
            rowSet = true;
            p += 16;
        } else if (cmd == 0x31) {
            p += 16;
        } else {
            throw FormatError("unexpected VIF command");
        }
    }
    const Slot& g = mem[0xC0];
    if (!g.exists || (g.mask & 7) != 7) throw FormatError("packet without GIF tag at 0xC0");
    pk.gif = {g.v[0], g.v[1], g.v[2]};
    pk.nloop = g.v[0] & 0x7FFF;
    if (pk.nloop == 0 || pk.nloop > 64) throw FormatError("bad NLOOP");
    const Slot& cs = mem[0x40];
    if (!cs.exists || !(cs.mask & 8)) throw FormatError("packet without strip table");
    std::uint32_t ns = cs.v[3] & 0xFF, sum = 0;
    for (std::uint32_t i = 0; i < ns; ++i) {
        const Slot& s = mem[0x41 + i];
        if (!s.exists || !(s.mask & 8) || (s.v[3] & 0xFF) % 3) throw FormatError("bad strip table");
        std::uint32_t len = (s.v[3] & 0xFF) / 3 + 2;
        pk.strips.push_back(static_cast<std::uint8_t>(len));
        sum += len;
    }
    if (sum != pk.nloop) throw FormatError("strip lengths do not add up to NLOOP");
    for (std::uint32_t w : pk.gif) put32(canon, w);
    put16(canon, ns);
    canon.insert(canon.end(), pk.strips.begin(), pk.strips.end());
    for (const auto& t : stripTriangles(pk.strips)) {
        put16(canon, t[0]);
        put16(canon, t[1]);
        put16(canon, t[2]);
    }
    auto copy = [](const Slot& s) {
        VuValue v;
        if (s.exists) {
            v.v = s.v;
            v.mask = s.mask;
            v.vn = s.vn;
            v.bits = s.bits;
            v.usn = s.usn;
        }
        return v;
    };
    pk.verts.resize(pk.nloop);
    for (std::uint32_t i = 0; i < pk.nloop; ++i)
        pk.verts[i] = {copy(mem[i]), copy(mem[0x40 + i]), copy(mem[0x80 + i])};
    return pk;
}

Shape parseShape(Bytes b, std::size_t so, std::size_t limit) {
    if (so + 0x10 > limit) throw FormatError("shape header outside set");
    Shape s;
    s.offset = static_cast<std::uint32_t>(so);
    if (rd32(b, so) != 0) throw FormatError("shape stored base != 0");
    s.scaleBits = rd32(b, so + 4);
    s.flags8 = rd8(b, so + 8);
    s.flags9 = rd8(b, so + 9);
    std::uint32_t nd = rd16(b, so + 10);
    s.totalVerts = rd16(b, so + 12);
    s.totalTris = rd16(b, so + 14);
    std::uint32_t initMask = (s.flags9 & 0x40) ? 0x3F3F3F3Fu : 0xC0C0C0C0u;  // FUN_0049d308
    std::uint32_t tv = 0, tt = 0;
    for (std::uint32_t k = 0; k < nd; ++k) {
        std::uint32_t off = rd32(b, so + 16 + 8ull * k);
        std::uint32_t qwc = rd16(b, so + 20 + 8ull * k), fl = rd16(b, so + 22 + 8ull * k);
        if (so + off + qwc * 16ull > limit) throw FormatError("VIF packet outside set");
        VifPacket pk = decodePacket(b, so + off, qwc, initMask);
        pk.offset = static_cast<std::uint32_t>(so + off);
        pk.qwc = static_cast<std::uint16_t>(qwc);
        pk.tex = static_cast<std::uint16_t>(fl & 0x1FF);
        pk.mat = static_cast<std::uint16_t>(fl >> 9);
        if (s.flags9 & 0x80) {
            size_t cursor = so+off+qwc*16ull;
            auto require = [&](size_t size) {
                if (cursor > limit || size > limit-cursor) throw FormatError("skin stream outside model set");
            };
            pk.skinPacked = (s.flags9 & 0x20) != 0;
            require(pk.skinPacked ? 6 : 4);
            const uint32_t count = pk.skinPacked ? rd8(b,cursor+4) : rd16(b,cursor);
            pk.skinNormal = pk.skinPacked ? rd8(b,cursor+5) != 0 : rd16(b,cursor+2) != 0;
            pk.skinScale = pk.skinPacked ? rd32(b,cursor) : 0x3f800000;
            cursor += pk.skinPacked ? 6 : 4;
            bool more;
            do {
                require(pk.skinPacked ? 6 : 8);
                const uint32_t group = pk.skinPacked ? rd8(b,cursor++) : rd16(b,cursor);
                if (!pk.skinPacked) cursor += 2;
                more = (group & (pk.skinPacked ? 0x80 : 0x8000)) != 0;
                const uint32_t vertices = group & (pk.skinPacked ? 0x7f : 0x7fff);
                cursor += pk.skinPacked ? 1 : 2; // original consumes an unused bone-count field
                std::array<uint8_t,4> bones;
                for (size_t i = 0; i < 4; ++i) bones[i] = rd8(b,cursor++);
                const size_t stride = pk.skinPacked ? (pk.skinNormal ? 13 : 10) : (pk.skinNormal ? 40 : 28);
                if (pk.skin.size()+vertices > count) throw FormatError("skin group exceeds vertex count");
                require(size_t(vertices)*stride);
                for (uint32_t i = 0; i < vertices; ++i) {
                    SkinVertex vertex; vertex.bones = bones;
                    for (uint32_t lane = 0; lane < 3; ++lane) {
                        vertex.position[lane] = pk.skinPacked ? static_cast<uint32_t>(static_cast<int32_t>(
                            static_cast<int16_t>(rd16(b,cursor+2*lane)))) : rd32(b,cursor+4*lane);
                        if (pk.skinNormal) vertex.normal[lane] = pk.skinPacked ? static_cast<uint32_t>(static_cast<int32_t>(
                            static_cast<int8_t>(rd8(b,cursor+6+lane)))) : rd32(b,cursor+12+4*lane);
                    }
                    const size_t weights = cursor+(pk.skinPacked ? (pk.skinNormal ? 9 : 6) : (pk.skinNormal ? 24 : 12));
                    for (uint32_t lane = 0; lane < 4; ++lane) vertex.weights[lane] = pk.skinPacked ?
                        static_cast<uint32_t>(static_cast<int32_t>(static_cast<int8_t>(rd8(b,weights+lane)))) : rd32(b,weights+4*lane);
                    pk.skin.push_back(vertex); cursor += stride;
                }
            } while (more);
            if (pk.skin.size() != count || count != pk.nloop) throw FormatError("skin vertex count mismatch: decoded "+
                std::to_string(pk.skin.size())+" header "+std::to_string(count)+" VIF "+std::to_string(pk.nloop));
        }
        tv += pk.nloop;
        for (auto l : pk.strips) tt += l - 2u;
        s.packets.push_back(std::move(pk));
    }
    if (tv != s.totalVerts) throw FormatError("shape vertex total mismatch");
    if (tt != s.totalTris) throw FormatError("shape triangle total mismatch");
    return s;
}

Material readMaterial(Bytes b, std::size_t p) {
    Material m{};
    for (int i = 0; i < 16; ++i) m.colors[i] = rdf32(b, p + 4ull * i);
    m.f40 = rdf32(b, p + 0x40);
    m.flags = rd32(b, p + 0x44);
    m.f48 = rdf32(b, p + 0x48);
    m.f4c = rdf32(b, p + 0x4c);
    return m;
}

}  // namespace

VifPacket decodeVifPacket(Bytes data, std::size_t offset, std::uint32_t qwords, std::uint32_t initialMask) {
    need(data, offset, static_cast<std::size_t>(qwords) * 16, "standalone VIF packet");
    return decodePacket(data, offset, qwords, initialMask);
}
std::size_t commandLength(Bytes b, std::size_t p) {
    std::uint8_t op = rd8(b, p);
    int n = operandBytes(op);
    if (n >= 0) return 1 + static_cast<std::size_t>(n);
    if (op == 0x06 || op == 0x4D) return 4 + 2ull * rd8(b, p + 3);
    if (op == 0x07) return 0x12 + 2ull * rd8(b, p + 0x11);
    return 2 + 12ull * rd8(b, p + 1) + 2;  // 0x0A
}

std::vector<std::array<std::uint16_t, 3>> stripTriangles(const std::vector<std::uint8_t>& strips) {
    std::vector<std::array<std::uint16_t, 3>> tris;
    std::uint32_t s = 0;
    for (std::uint8_t ln : strips) {
        for (std::uint32_t k = 0; k + 2 < ln; ++k) {
            auto a = static_cast<std::uint16_t>(s + k), c = static_cast<std::uint16_t>(s + k + 2);
            if (k % 2 == 0) tris.push_back({a, static_cast<std::uint16_t>(a + 1), c});
            else tris.push_back({static_cast<std::uint16_t>(a + 1), a, c});
        }
        s += ln;
    }
    return tris;
}

ModelSet parseModelSet(Bytes b, std::size_t o, bool decodeShapes) {
    if (b.size() < o || b.size() - o < 0x80 || std::memcmp(&b[o], "MDLS", 4) != 0) throw FormatError("not an MDLS");
    ModelSet ms;
    ms.base = o;
    if (rd32(b, o + 0x0c) != 0) throw FormatError("MDLS stored relocation base != 0");
    ms.size = rd32(b, o + 0x10);
    ms.unk14 = rd8(b, o + 0x14);
    ms.instanceFlags = rd8(b, o + 0x15);
    std::uint32_t nModels = rd16(b, o + 0x16), nShapes = rd16(b, o + 0x18), nMats = rd16(b, o + 0x1a);
    ms.nTexsets = rd16(b, o + 0x1c);
    ms.nTexlists = rd16(b, o + 0x1e);
    ms.nVariations = rd8(b, o + 0x2c);
    ms.curVariation = rd8(b, o + 0x2d);
    ms.nBindMatrices = rd16(b, o + 0x2e);
    for (int i = 0; i < 18; ++i) ms.ptrs[i] = rd32(b, o + 0x38 + 4ull * i);
    if (ms.size > b.size() - o) throw FormatError("MDLS size past end of data");
    std::size_t limit = o + ms.size;
    auto chk = [&](std::uint32_t p, std::uint64_t n, const char* what) {
        if (p && o + p + n > limit) throw FormatError(std::string(what) + " outside MDLS");
    };
    const std::uint32_t pModels = ms.ptrs[0], pShapes = ms.ptrs[1], pMats = ms.ptrs[2], pTex = ms.ptrs[3],
                        pVar = ms.ptrs[10], pParams = ms.ptrs[12];
    chk(pModels, 0x28ull * nModels, "models");
    chk(pShapes, 4ull * nShapes, "shape table");
    chk(pMats, 0x50ull * nMats, "materials");
    chk(pTex, 4ull * ms.nTexlists, "texture set table");
    const uint32_t pBindings = ms.ptrs[4], pSymbols = ms.ptrs[8];
    const uint32_t symbolCount = rd16(b,o+0x2a);
    chk(pBindings,0x50ull*ms.nBindMatrices,"bind matrices");
    chk(pSymbols,8ull*symbolCount,"symbols");
    auto name = [&](uint32_t offset) {
        if (!offset || o+offset >= limit) throw FormatError("MDLS name outside model set");
        size_t end = o+offset;
        while (end < limit && b[end]) ++end;
        if (end == limit) throw FormatError("unterminated MDLS name");
        return std::string(reinterpret_cast<const char*>(b.data()+o+offset),end-(o+offset));
    };
    if (pBindings) for (uint32_t i = 0; i < ms.nBindMatrices; ++i) {
        const size_t at = o+pBindings+0x50ull*i;
        ModelBinding binding;
        for (uint32_t j = 0; j < 16; ++j) binding.matrix[j] = rd32(b,at+4*j);
        binding.paletteIndex = rd32(b,at+0x40); binding.name = name(rd32(b,at+0x44));
        ms.bindings.push_back(std::move(binding));
    }
    if (pSymbols) for (uint32_t i = 0; i < symbolCount; ++i) {
        const size_t at = o+pSymbols+8ull*i;
        ms.symbols.push_back({name(rd32(b,at)),rd32(b,at+4)});
    }
    if (pMats)
        for (std::uint32_t i = 0; i < nMats; ++i) ms.materials.push_back(readMaterial(b, o + pMats + 0x50ull * i));
    if (pVar) {
        chk(pVar, 4ull * ms.nVariations, "variation table");
        for (std::uint32_t i = 0; i < ms.nVariations; ++i) {
            std::uint32_t q = rd32(b, o + pVar + 4ull * i);
            chk(q, 0x50ull * nMats, "variation materials");
            std::vector<Material> v;
            for (std::uint32_t k = 0; k < nMats; ++k) v.push_back(readMaterial(b, o + q + 0x50ull * k));
            ms.variations.push_back(std::move(v));
        }
    }
    for (std::uint32_t i = 0; i < nModels; ++i) {
        std::size_t mo = o + pModels + 0x28ull * i;
        Model m;
        m.unk0 = rd8(b, mo);
        m.nBounds = rd8(b, mo + 1);
        m.unk2 = rd8(b, mo + 2);
        m.unk3 = rd8(b, mo + 3);
        std::uint32_t pBounds = rd32(b, mo + 4);
        m.radius = rdf32(b, mo + 8);
        m.origin = {rdf32(b, mo + 12), rdf32(b, mo + 16), rdf32(b, mo + 20)};
        m.cmdOffset = rd32(b, mo + 24);
        m.vm = {static_cast<std::int32_t>(rd32(b, mo + 28)), static_cast<std::int32_t>(rd32(b, mo + 32)),
                static_cast<std::int32_t>(rd32(b, mo + 36))};
        if (pBounds)
            for (std::uint32_t k = 0; k < m.nBounds; ++k) {
                std::size_t q = o + pBounds + 12ull * k;
                m.bounds.push_back({rdf32(b, q), rdf32(b, q + 4), rdf32(b, q + 8)});
            }
        if (m.cmdOffset) m.commands = decodeCommands(b, o + m.cmdOffset, limit);
        ms.models.push_back(std::move(m));
    }
    for (std::uint32_t i = 0; i < nShapes; ++i) ms.shapeOffsets.push_back(rd32(b, o + pShapes + 4ull * i));
    if (decodeShapes)
        for (std::uint32_t so : ms.shapeOffsets) ms.shapes.push_back(parseShape(b, o + so, limit));
    for (std::uint32_t i = 0; i < ms.nTexlists; ++i) {
        std::uint32_t lo = rd32(b, o + pTex + 4ull * i);
        chk(lo, 4ull * ms.nTexsets, "texture list");
        std::vector<std::optional<Tex1>> rowv;
        for (std::uint32_t j = 0; j < ms.nTexsets; ++j) {
            std::uint32_t to = rd32(b, o + lo + 4ull * j);
            if (to) rowv.emplace_back(parseTex1(b, o + to));
            else rowv.emplace_back(std::nullopt);
        }
        ms.texsets.push_back(std::move(rowv));
    }
    if (pParams) {
        std::size_t p = o + pParams;
        while (p + 4 <= limit) {
            std::uint32_t w = rd32(b, p);
            if ((w & 0xFFFFFF00u) == 0) break;
            std::uint32_t n = w & 0xFF, cnt = n == 0xFF ? 1 : n;
            std::vector<std::uint32_t> vals;
            for (std::uint32_t k = 0; k < cnt; ++k) vals.push_back(rd32(b, p + 4 + 4ull * k));
            ms.params[w >> 8] = std::move(vals);
            p += n == 0xFF ? 8 : 4 + 4ull * n;
        }
    }
    return ms;
}

namespace {
using Mat4r = std::array<float, 16>;  // row-major, row-vector convention (VU0 vf28..vf31)

struct MatState {
    Mat4r m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::vector<Mat4r> stack;
    bool touched = false;
    const int* callbacks = nullptr;  // callback_branch values by id (0x30 entries), nullptr = all 0
};

template<class Operation> void applyMatrix(MatState& mat, Operation operation) {
    std::array<uint8_t, 16384> scratch{};
    gt4::game::Vu0Regs vu;
    vu.vf[0].f[3] = 0x3f800000;
    std::array<uint32_t, 16> words;
    std::memcpy(words.data(), mat.m.data(), 64);
    gt4::game::Vu0MatLoad(vu, scratch.data(), words.data());
    operation(vu, scratch.data());
    gt4::game::Vu0MatStore(vu, words.data());
    std::memcpy(mat.m.data(), words.data(), 64);
    mat.touched = true;
}

std::vector<Draw> drawListImpl(Bytes b, const ModelSet& ms, int model, int lod, int texlist, int depth, bool wheel,
                               MatState& mat);
}  // namespace

std::vector<Draw> drawList(Bytes b, const ModelSet& ms, int model, int lod, int texlist, int depth, bool wheel) {
    MatState mat;
    return drawListImpl(b, ms, model, lod, texlist, depth, wheel, mat);
}

std::vector<Draw> drawList(Bytes b, const ModelSet& ms, int model, int lod, int texlist, int depth, bool wheel,
                           const int* callbackValues) {
    MatState mat;
    mat.callbacks = callbackValues;
    return drawListImpl(b, ms, model, lod, texlist, depth, wheel, mat);
}

namespace {
std::vector<Draw> drawListImpl(Bytes b, const ModelSet& ms, int model, int lod, int texlist, int depth, bool wheel,
                               MatState& mat) {
    std::vector<Draw> draws;
    const Model& m = ms.models.at(static_cast<std::size_t>(model));
    if (!m.cmdOffset) return draws;
    std::size_t p = ms.base + m.cmdOffset;
    Draw state{0, -1, model};
    int steps = 0;
    int wheelSet = -1;
    while (true) {
        if (++steps > 100000) throw FormatError("command walk does not terminate");
        std::uint8_t op = rd8(b, p);
        std::size_t nxt = p + commandLength(b, p);
        if (op == 0x00) break;
        switch (op) {
            case 0x02:
            case 0x03: {
                std::uint32_t idx = op == 0x02 ? rd8(b, p + 1) : rd16(b, p + 1);
                if (depth < 8 && idx < ms.models.size()) {
                    auto sub = drawListImpl(b, ms, static_cast<int>(idx), lod, texlist, depth + 1, wheel, mat);
                    draws.insert(draws.end(), sub.begin(), sub.end());
                }
                break;
            }
            case 0x04:
            case 0x05:
            case 0x35: {
                std::uint32_t idx = op == 0x04 ? rd8(b, p + 1) : rd16(b, p + 1);
                if (idx >= ms.shapeOffsets.size()) throw FormatError("shape index out of range");
                Draw d = state;
                d.shape = idx;
                d.hasMatrix = mat.touched;
                d.matrix = mat.m;
                draws.push_back(d);
                break;
            }
            case 0x06:
            case 0x4D: {
                std::uint32_t n = rd8(b, p + 3);
                std::uint32_t id = rd16(b, p + 1);
                if (op == 0x06 && wheel) wheelSet = static_cast<int>(id);
                // branch on the callback value v (GT4Model::CarModel::Callback vf02 0x00453698): v < n jumps through
                // rel[v], larger values fall through; value 0 unless given (wheel ids: the texture set above)
                int v = (op == 0x06 && mat.callbacks && id < 0x30 && !wheel) ? mat.callbacks[id] : 0;
                if (v >= 0 && static_cast<std::uint32_t>(v) < n) {
                    std::size_t q = p + 4 + 2ull * static_cast<std::uint32_t>(v);
                    nxt = static_cast<std::size_t>(static_cast<std::int64_t>(q) + s16at(b, q));
                }
                break;
            }
            case 0x07: {
                std::uint32_t n = rd8(b, p + 0x11);
                std::size_t q = p + 0x12 + 2ull * static_cast<std::uint32_t>(std::min<int>(lod, static_cast<int>(n) - 1));
                nxt = static_cast<std::size_t>(static_cast<std::int64_t>(q) + s16at(b, q));
                break;
            }
            case 0x08: nxt = static_cast<std::size_t>(static_cast<std::int64_t>(p) + 1 + s8at(b, p + 1)); break;
            case 0x09: nxt = static_cast<std::size_t>(static_cast<std::int64_t>(p) + 1 + s16at(b, p + 1)); break;
            case 0x2B:
            case 0x2C: {
                std::uint32_t idx = op == 0x2B ? rd8(b, p + 1) : rd16(b, p + 1);
                int tl = std::min<int>(texlist, static_cast<int>(ms.texsets.size()) - 1);
                state.texset = (!ms.texsets.empty() && idx < ms.texsets[static_cast<std::size_t>(tl)].size())
                                   ? static_cast<int>(idx) : -1;
                break;
            }
            case 0x50:
            case 0x51: {
                std::uint32_t idx = op == 0x50 ? rd8(b, p + 1) : rd16(b, p + 1);
                state.texset = (wheel && idx == 2 && wheelSet >= 0) ? wheelSet : -2;
                break;
            }
            case 0x20: state.blend0 = rd8(b, p + 1); state.blend1 = rd8(b, p + 2); break;
            case 0x56: state.environmentDefault=true; break;
            case 0x57: state.environmentDefault=false; break;
            case 0x1C: state.alphaFunc = rd8(b, p + 1); state.alphaRef = rd8(b, p + 2); break;
            case 0x2F: state.alphaFail = rd8(b, p + 1); break;
            case 0x24: state.fbmsk = rd32(b, p + 1); break;
            case 0x0C: mat.stack.push_back(mat.m); break;  // FUN_004a53f8: VCALLMS 0x900 (save matrix)
            case 0x0D:                                       // FUN_004a5400: VCALLMS 0x930 (restore)
                if (!mat.stack.empty()) {
                    mat.m = mat.stack.back();
                    mat.stack.pop_back();
                }
                break;
            case 0x0F:  // FUN_004a74b4: load
            case 0x10: {  // FUN_004a7698: multiply (local)
                std::array<uint32_t, 16> g;
                for (int i = 0; i < 16; ++i) g[static_cast<std::size_t>(i)] = rd32(b, p + 1 + 4ull * static_cast<std::size_t>(i));
                if (op == 0x0F) std::memcpy(mat.m.data(), g.data(), 64);
                else applyMatrix(mat, [&](auto& vu, auto* scratch) { gt4::game::Vu0MatMultiply(vu, scratch, g.data()); });
                mat.touched = true;
                break;
            }
            case 0x11: {  // FUN_004a7844: row3 += x row0 + y row1 + z row2
                applyMatrix(mat, [&](auto& vu, auto* scratch) {
                    gt4::game::Vu0MatTranslate(vu, scratch, rd32(b, p + 1), rd32(b, p + 5), rd32(b, p + 9));
                });
                break;
            }
            case 0x12: {  // FUN_004a7890: rows 0..2 scaled by x, y, z
                applyMatrix(mat, [&](auto& vu, auto* scratch) {
                    gt4::game::Vu0MatScale(vu, scratch, rd32(b, p + 1), rd32(b, p + 5), rd32(b, p + 9));
                });
                break;
            }
            case 0x14:
            case 0x15:
            case 0x16: {  // FUN_004a7988 / 79b0 / 79d8: rotate about X / Y / Z (VU0 micro 0x78 / 0xb8 / 0xf8)
                // Operands are degrees. Use the recovered EE/VU trigonometry and matrix operations.
                applyMatrix(mat, [&](auto& vu, auto* scratch) {
                    const uint32_t angle = rd32(b, p + 1);
                    if (op == 0x14) gt4::game::Vu0MatRotateX(vu, scratch, angle);
                    else if (op == 0x15) gt4::game::Vu0MatRotateY(vu, scratch, angle);
                    else gt4::game::Vu0MatRotateZ(vu, scratch, angle);
                });
                break;
            }
            case 0x2D: state.cull = 1; break;
            case 0x2E: state.cull = 0; break;
            case 0x1A: state.alphaTest = true; break;
            case 0x1B: state.alphaTest = false; break;
            case 0x25: state.depthMask = false; break;
            case 0x26: state.depthMask = true; break;
            default: break;
        }
        p = nxt;
    }
    return draws;
}
}  // namespace

Vertex interpretVertex(const VertexRaw& r, float scale) {
    Vertex out;
    auto conv = [](const VuValue& s, int comps, int kind, float scale, float* dst) {
        if (s.vn == 0) return false;
        for (int e = 0; e < comps; ++e)
            if (!(s.mask & (1 << e))) return false;
        for (int e = 0; e < comps; ++e) {
            std::uint32_t v = s.v[static_cast<std::size_t>(e)];
            if (s.bits == 32 && s.usn) {
                dst[e] = bitsToFloat(v);
            } else {
                double iv = static_cast<double>(static_cast<std::int32_t>(v));
                if (kind == 0) dst[e] = static_cast<float>(scale != 0.0f ? iv * scale : iv);
                else if (kind == 1) dst[e] = static_cast<float>(iv / 4096.0);
                else dst[e] = static_cast<float>(iv / 127.0);
            }
        }
        return true;
    };
    out.hasPos = conv(r.pos, 3, 0, scale, out.pos);
    if (r.bank1.vn == 2) out.hasUv = conv(r.bank1, 2, 1, scale, out.uv);
    else if (r.bank1.vn == 3) out.hasNrm = conv(r.bank1, 3, 2, scale, out.nrm);
    if (r.bank2.vn == 3) {
        out.hasNrm = conv(r.bank2, 3, 2, scale, out.nrm);
    } else if (r.bank2.vn == 4 && (r.bank2.mask & 15) == 15) {
        out.hasCol = true;
        for (int e = 0; e < 4; ++e) out.col[e] = static_cast<std::uint8_t>(r.bank2.v[static_cast<std::size_t>(e)] & 0xFF);
    }
    return out;
}

void mdlsStream(Bytes b, const ModelSet& ms, ByteWriter& out, DigestStats& stats) {
    out.str("MS");
    out.u16(static_cast<std::uint32_t>(ms.models.size()));
    out.u16(static_cast<std::uint32_t>(ms.shapeOffsets.size()));
    out.u16(static_cast<std::uint32_t>(ms.materials.size()));
    out.u16(ms.texsets.empty() ? 0u : static_cast<std::uint32_t>(ms.texsets[0].size()));
    out.u16(static_cast<std::uint32_t>(ms.texsets.size()));
    for (const Model& m : ms.models) {
        out.u32(m.cmdOffset);
        out.u32(static_cast<std::uint32_t>(m.commands.size()));
        for (const Command& c : m.commands) {
            out.u32(static_cast<std::uint32_t>(c.offset - ms.base));
            out.u8(c.op);
            out.u16(c.length);
        }
    }
    for (const Shape& s : ms.shapes) {
        out.u32(s.scaleBits);
        out.u8(s.flags8);
        out.u8(s.flags9);
        out.u16(s.totalVerts);
        out.u16(s.totalTris);
        for (const VifPacket& pk : s.packets) {
            out.u16(pk.tex);
            out.u16(pk.mat);
            out.u32(static_cast<std::uint32_t>(pk.canon.size()));
            out.bytes(pk.canon.data(), pk.canon.size());
        }
        ++stats.shapes;
        stats.packets += s.packets.size();
        stats.vertices += s.totalVerts;
        stats.triangles += s.totalTris;
    }
    for (const auto& rowv : ms.texsets)
        for (const auto& t : rowv) {
            if (!t) {
                out.u8(0);
            } else {
                out.u8(1);
                tex1Stream(*t, out, stats);
                ++stats.texsets;
            }
        }
    stats.models += ms.models.size();
    (void)b;
}

std::vector<Vertex> skinPacket(const VifPacket& packet,std::span<const std::array<uint32_t,16>> palette) {
    if (packet.skin.size() != packet.verts.size()) throw FormatError("skin/VIF vertex count differs");
    std::vector<Vertex> result;
    for (size_t i = 0; i < packet.skin.size(); ++i) {
        const auto& input = packet.skin[i];
        gt4::game::Vu0Regs vu;
        vu.vf[0].f[3] = 0x3f800000; vu.vf[4].f[0] = packet.skinScale;
        std::copy(input.weights.begin(),input.weights.end(),vu.vf[1].f);
        std::copy(input.position.begin(),input.position.end(),vu.vf[2].f);
        std::copy(input.normal.begin(),input.normal.end(),vu.vf[3].f);
        for (size_t bone = 0; bone < 4; ++bone) {
            if (input.bones[bone] >= palette.size()) throw FormatError("skin bone outside palette");
            const auto& matrix = palette[input.bones[bone]];
            for (size_t row = 0; row < 4; ++row)
                std::copy_n(matrix.data()+row*4,4,vu.vf[8+4*bone+row].f);
        }
        gt4::game::MotionSkinVertex(vu,packet.skinPacked,packet.skinNormal);
        Vertex vertex = interpretVertex(packet.verts[i],0);
        vertex.hasPos = true; vertex.hasNrm = packet.skinNormal;
        for (size_t lane = 0; lane < 3; ++lane) {
            vertex.pos[lane] = bitsToFloat(vu.vf[2].f[lane]);
            if (packet.skinNormal) vertex.nrm[lane] = bitsToFloat(vu.vf[3].f[lane]);
        }
        result.push_back(vertex);
    }
    return result;
}
}  // namespace gt4fmt
