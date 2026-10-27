// GT4 CAR4 + Pat0. Layout and CORE.GT4 addresses: docs/formats/car4.md
// (FUN_004583d0/FUN_00458400 relocation, FUN_00458238 Pat0 relocation, FUN_00458290 Pat0 apply,
// FUN_00452fa0 colour selection, FUN_001553c0 '.pat' sibling file).
#include "gt4formats/car4.h"

namespace gt4fmt {

const char* car4SlotName(std::uint32_t slot) {
    switch (slot) {
        case kSlotInfo: return "info";
        case kSlotBlock14: return "block14";
        case kSlotBody: return "body";
        case kSlotBodyPat: return "body_pat";
        case kSlotWheel: return "wheel";
        case kSlotWheelPat: return "wheel_pat";
        case kSlotExtra28: return "extra28";
        case kSlotTyreFront: return "tyre_front";
        case kSlotTyreRear: return "tyre_rear";
        case kSlotExtra34: return "extra34";
        case kSlotTex38: return "tex38";
        default: return "?";
    }
}

Pat0 parsePat0(Bytes b, std::size_t o) {
    need(b, o, 0x20, "Pat0 header");
    if (std::memcmp(&b[o], "Pat0", 4) != 0) throw FormatError("not a Pat0");
    if (rd32(b, o + 4) != 0) throw FormatError("Pat0 stored base != 0");
    Pat0 p;
    p.colors = rd16(b, o + 0x10);
    p.entries = rd16(b, o + 0x12);
    std::uint64_t n = static_cast<std::uint64_t>(p.colors) * p.entries;
    if (o + 0x20 + 4 * n > b.size()) throw FormatError("Pat0 table past end");
    for (std::uint32_t c = 0; c < p.colors; ++c) {
        std::vector<std::optional<PatEntry>> rowv;
        for (std::uint32_t e = 0; e < p.entries; ++e) {
            std::uint32_t q = rd32(b, o + 0x20 + 4ull * (c * p.entries + e));
            if (q == 0) {
                rowv.emplace_back(std::nullopt);
                continue;
            }
            if (o + q + 8 > b.size()) throw FormatError("Pat0 entry past end");
            std::uint32_t dst = rd32(b, o + q), size = rd32(b, o + q + 4);
            if (o + q + 8 + static_cast<std::uint64_t>(size) > b.size()) throw FormatError("Pat0 entry data past end");
            rowv.emplace_back(PatEntry{dst, b.subspan(o + q + 8, size)});
        }
        p.table.push_back(std::move(rowv));
    }
    return p;
}

void Pat0::apply(std::vector<std::uint8_t>& image, std::size_t targetOffset, std::size_t targetSize, int color) const {
    if (color < 0 || color >= colors) throw FormatError("colour out of range");
    for (const auto& e : table[static_cast<std::size_t>(color)]) {
        if (!e) continue;
        if (e->dst + e->data.size() > targetSize) throw FormatError("Pat0 patch outside target");
        std::memcpy(&image[targetOffset + e->dst], e->data.data(), e->data.size());
    }
}

Car4 parseCar4(Bytes b) {
    need(b, 0, 0x40, "CAR4 header");
    if (std::memcmp(b.data(), "CAR4", 4) != 0) throw FormatError("not a CAR4");
    Car4 c;
    for (int i = 0; i < 16; ++i) c.header[i] = rd32(b, 4ull * i);
    if (c.header[1] != 0) throw FormatError("CAR4 stored base != 0");
    if (c.header[2] != b.size()) throw FormatError("CAR4 size field != file size");
    for (std::uint32_t s = kSlotInfo; s <= kSlotTex38; s += 4)
        if (c.slot(s) >= b.size()) throw FormatError("slot outside file");
    for (std::uint32_t s : {kSlotBody, kSlotWheel, kSlotExtra28, kSlotTyreFront, kSlotTyreRear, kSlotExtra34}) {
        std::uint32_t off = c.slot(s);
        if (!off) continue;
        need(b, off, 4, "slot");
        if (std::memcmp(&b[off], "MDLS", 4) != 0) throw FormatError("MDLS slot is not MDLS");
        parseModelSet(b, off, false);
        c.mdlsSlots.push_back(s);
    }
    for (std::uint32_t s : {kSlotBodyPat, kSlotWheelPat})
        if (c.slot(s)) c.pats.emplace(s, parsePat0(b, c.slot(s)));
    if (c.slot(kSlotTex38)) c.tex38 = parseTex1(b, c.slot(kSlotTex38));
    return c;
}

namespace {
void patStream(const Pat0& p, ByteWriter& out) {
    out.str("P0");
    out.u16(p.colors);
    out.u16(p.entries);
    for (const auto& rowv : p.table)
        for (const auto& e : rowv) {
            if (!e) {
                out.u32(0xFFFFFFFFu);
                out.u32(0);
            } else {
                out.u32(e->dst);
                out.u32(static_cast<std::uint32_t>(e->data.size()));
                out.bytes(e->data.data(), e->data.size());
            }
        }
}
}  // namespace

std::optional<FileDigest> digestFile(Bytes b) {
    if (b.size() < 4) return std::nullopt;
    std::string magic(reinterpret_cast<const char*>(b.data()), 4);
    if (magic != "CAR4" && magic != "MDLS" && magic != "Tex1" && magic != "Pat0") return std::nullopt;
    FileDigest fd;
    fd.kind = magic;
    ByteWriter out;
    try {
        if (magic == "CAR4") {
            Car4 car = parseCar4(b);
            out.str("C4");
            for (std::uint32_t w : car.header) out.u32(w);
            for (std::uint32_t s = kSlotInfo; s <= kSlotTex38; s += 4) {
                bool isMdls = std::find(car.mdlsSlots.begin(), car.mdlsSlots.end(), s) != car.mdlsSlots.end();
                if (isMdls) {
                    out.u8('M');
                    mdlsStream(b, parseModelSet(b, car.slot(s)), out, fd.stats);
                } else if (car.pats.count(s)) {
                    out.u8('P');
                    patStream(car.pats.at(s), out);
                    ++fd.stats.pats;
                } else if (s == kSlotTex38 && car.tex38) {
                    out.u8('T');
                    tex1Stream(*car.tex38, out, fd.stats);
                    ++fd.stats.texsets;
                }
            }
        } else if (magic == "MDLS") {
            mdlsStream(b, parseModelSet(b, 0), out, fd.stats);
        } else if (magic == "Tex1") {
            Tex1 t = parseTex1(b, 0);
            ++fd.stats.texsets;
            tex1Stream(t, out, fd.stats);
        } else {
            ++fd.stats.pats;
            patStream(parsePat0(b, 0), out);
        }
        fd.digest = Sha256::hex(out.buf);
    } catch (const FormatError& e) {
        fd.digest = "ERROR";
        fd.error = e.what();
        fd.stats = DigestStats{};
    }
    return fd;
}

}  // namespace gt4fmt
