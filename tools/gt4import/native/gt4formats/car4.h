// GT4 'CAR4' car containers and 'Pat0' paint patch tables. Format: docs/formats/car4.md.
// Independent re-implementation of scripts/gt4_car4.py (agreement checked by `gt4tool digest`).
#pragma once

#include <optional>
#include <string>

#include "gt4formats/mdls.h"

namespace gt4fmt {

struct PatEntry {
    std::uint32_t dst;
    Bytes data;
};

struct Pat0 {
    std::uint16_t colors = 0, entries = 0;
    std::vector<std::vector<std::optional<PatEntry>>> table;  // [colour][entry]
    // FUN_00458290: copy every entry of row `color` to target + dst (target = the MDLS image).
    void apply(std::vector<std::uint8_t>& image, std::size_t targetOffset, std::size_t targetSize, int color) const;
};
Pat0 parsePat0(Bytes data, std::size_t offset);

// Header slots (offsets from the file start), see car4.md.
enum Car4Slot : std::uint32_t {
    kSlotInfo = 0x10,
    kSlotBlock14 = 0x14,
    kSlotBody = 0x18,
    kSlotBodyPat = 0x1c,
    kSlotWheel = 0x20,
    kSlotWheelPat = 0x24,
    kSlotExtra28 = 0x28,
    kSlotTyreFront = 0x2c,
    kSlotTyreRear = 0x30,
    kSlotExtra34 = 0x34,
    kSlotTex38 = 0x38,
};
const char* car4SlotName(std::uint32_t slot);

struct Car4 {
    std::array<std::uint32_t, 16> header{};
    std::uint32_t slot(std::uint32_t s) const { return header[s / 4]; }
    std::map<std::uint32_t, Pat0> pats;  // keyed by slot
    std::optional<Tex1> tex38;
    std::vector<std::uint32_t> mdlsSlots;  // slots holding an MDLS
};
Car4 parseCar4(Bytes data);

struct FileDigest {
    std::string kind;    // CAR4 / MDLS / Tex1 / Pat0
    std::string digest;  // SHA-256 of the canonical stream, or ERROR
    DigestStats stats;
    std::string error;
};
// nullopt = not one of the four formats (by magic).
std::optional<FileDigest> digestFile(Bytes data);

}  // namespace gt4fmt
