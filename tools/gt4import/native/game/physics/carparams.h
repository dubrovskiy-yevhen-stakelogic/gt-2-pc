#pragma once
// Car parameter setup from SpecDB (CORE.GT4 US 1.01): the car-parameter builder 0x004413c8 with every per-part copy
// routine it calls, and the race-time completion 0x003f3b68. Byte images keep the original offsets.
// Table column meanings: docs/formats/specdb.md ("Car parameter builder"). Verified by the gt4verify rows
// "carparams_build", "carparams_race" and "carparams_state".
//
// Objects:
//   car  - the garage car object (input, 0x170 bytes): +0x00 GENERIC_CAR key, +0x10 VARIATION key, +0x18 s8 colour,
//          +0x20.. part keys (u64 table << 32 | row id, -1 = none; slot -> table in kPartSlots), +0x110.. settings.
//   cprm - the car parameter block (output, 0x600 bytes); 0x003f3b68 clears it, 0x004413c8 fills it.
#include <cstdint>

namespace gt4::game::carparams {

constexpr uint32_t kCarSize = 0x170;
constexpr uint32_t kCprmSize = 0x600;
constexpr uint32_t kVariationTable = 42;   // DAT_00623784 = SpecDB table slot 42 (VARIATION<locale>)

// Garage car part slots (0x004468f8 installs a key by its table id into these offsets).
struct PartSlot {
    uint16_t offset;
    uint8_t table;
};
extern const PartSlot kPartSlots[30];

// Row access of the SpecDB storage object 0x006235a8 (implementations: carparams_specdb.h; the gt4verify rows
// hook the original entry points with the same implementation).
class SpecSource {
public:
    virtual ~SpecSource() = default;
    // 0x00443ed0(db, key, out): decodes the row of table key >> 32 with id (u32)key into out; returns the table's
    // row size, or 0 when the row (or table) is absent - out is then left untouched.
    virtual uint32_t Fetch(uint64_t key, uint8_t* out) const = 0;
    // 0x00443fb8(db, key) -> 0x00449770: number of consecutive rows with id (u32)key, -1 when absent.
    virtual int32_t Count(uint64_t key) const = 0;
    // 0x00449210(table, id): index of the first row with that id, -1 when absent.
    virtual int32_t FirstIndex(uint32_t table, uint32_t id) const = 0;
    // 0x004492d8(table, index, out): row by index; returns the row size, 0 (out untouched) when out of range.
    virtual uint32_t RowAt(uint32_t table, uint32_t index, uint8_t* out) const = 0;
    // 0x00443f40(db, key): the row exists (table id < 43, table loaded, id found).
    virtual bool Exists(uint64_t key) const = 0;
};

// Stack state the builder reads when rows are absent (the original reads whatever its frame held).
struct BuildFrame {
    uint8_t frame[0xd0] = {};   // 0x004413c8's row buffers sp+0x00..0xcf (sp, sp+0x10, sp+0x20, sp+0x50)
    uint8_t sub[0x120] = {};    // the area below it used by the callees' row buffers (0x00442218 / 0x00442358 /
                                // 0x004461f8 / 0x004462e0 / 0x00446320 / 0x00446820)
};

struct BuildGlobals {
    uint8_t flag006224a8 = 0;   // byte 0x006224a8, read by 0x003f16d0 (TCS/ASCC mask choice)
};

// 0x004413c8(car, cprm). `car` is modified like the original (colour -1 -> 0 at +0x18).
void BuildCarParams(uint8_t* car, uint8_t* cprm, const SpecSource& db, BuildFrame& frame, const BuildGlobals& g);

// Globals read by 0x003f3b68.
struct RaceGlobals {
    BuildGlobals build;
    uint8_t d620e22 = 0, d620e23 = 0, d620e15 = 0, d620e16 = 0;
    uint8_t d6224a0 = 0, d6224a1 = 0, d6224a2 = 0, d6224a3 = 0, d6224a4 = 0, d6224a5 = 0;
    uint8_t d6a4028[14] = {};   // 0x006a4028..0x006a4035 (default STEER curve bytes)
    uint8_t d622420[6] = {}, d622428[6] = {};   // default tyre curve bytes
};
// Reads the RaceGlobals bytes from a guest memory image (EE RAM, physical address = index).
RaceGlobals ReadRaceGlobals(const uint8_t* ram);

// 0x003f3b68(cprm, car, -, mode): clears the 0x600-byte block, runs the builder, applies race defaults.
void BuildRaceCarParams(uint8_t* cprm, uint8_t* car, int32_t mode, const SpecSource& db, BuildFrame& frame,
                        const RaceGlobals& g);

} // namespace gt4::game::carparams
