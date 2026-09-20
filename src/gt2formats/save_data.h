#pragma once
// GT-mode save game ("BASCUS-94455GAME" on a PS1 memory card) and the career state it is an image of.
// Derived from US Simulation v1.2 (EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): our disassembly of the
// save file class (EXE 0x80069FF8..0x8006A314), the new-game initialiser of the boot block (EXE 0x800106A0..
// 0x800107E0, runs once before the title overlay overwrites it), the garage code of the GT-mode overlay (GT2.OVL
// member 4: 0x8001EC0C add, 0x8001EDAC remove, 0x8001EF10 move, 0x80017914 / 0x8001796C buy) and a real save
// written by the game in our interpreter (work/re/menu_nav/y8.mcd: payload == RAM 0x801C98E0.. byte for byte,
// CRC matches). Format and evidence: docs/research/menus_gtmode.md sections 3-4.
//
// File (0x7EA0 bytes = 4 card blocks, 0x80069FF8 / 0x8006A000):
//   +0x0000  standard PS1 save header "SC" (0x8006A038): +2 icon flags 0x13 (3 frames), +3 block count 4,
//            +4 Shift-JIS title (EXE 0x80091CA8), +0x60 CLUT (0x80091CC4, 32 bytes), +0x80 3 icon frames
//            (0x80091CE4, 3 x 128 bytes)
//   +0x0200  the career state: a copy of RAM 0x801C98E0..0x801D157C (0x7C9C bytes; 0x8006A214 packs,
//            0x8006A278 unpacks)
//   +0x7E9C  u32 CRC-32 (reflected 0xEDB88320, init ~0, final ~; table EXE 0x800A6ACC, routine 0x80083178) of
//            file bytes 0x0000..0x7E9B; 0x8006A31C checks it on load
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/car_params.h"

namespace gt2 {

constexpr const char* kSaveGameFileName = "BASCUS-94455GAME"; // EXE 0x80091C94 (replays: "BASCUS-94455REPLAY", 0x80091AB8)
constexpr size_t kSaveFileSize = 0x7EA0;
constexpr size_t kSaveHeaderSize = 0x200;
constexpr size_t kSaveStateSize = 0x7C9C;        // RAM 0x801C98E0 .. 0x801D157B
constexpr size_t kSaveCrcOffset = 0x7E9C;
constexpr uint32_t kCareerStateAddress = 0x801C98E0;

// Sections of the career state (offsets from 0x801C98E0; the save class exposes the same sections of a loaded
// file through 0x8006A2E4..0x8006A314 = file + 0x200 + offset).
constexpr size_t kStateCareerRecord = 0x00B8;    // 0x160 bytes, 0x800107B4: cleared, days = 1
constexpr size_t kStateCourseRecords = 0x0218;   // .crsinfo count x 0x24, 0x8005DD68 each
constexpr size_t kStateLicenseRecords = 0x1418;  // 6 licences x 10 tests x 0xA4, 0x8005DE1C each
constexpr size_t kStateMachineTests = 0x3A88;    // 3 x 0xA4 at 0x3A88 / 0x3B2C / 0x3BD0, 0x8005E07C each
constexpr size_t kStateGarage = 0x3C74;          // 0x4028 bytes; a second garage block (the guest garage of the
                                                 // two-player battle, 0x8006A2A8) follows at 0x7C9C and is not saved
constexpr size_t kGarageBlockSize = 0x4028;
constexpr size_t kGarageSlots = 100;
constexpr size_t kGarageSlotSize = 0xA4;

// PS1 memory card image (.mcd, 128 KB): directory frames 1..15 of block 0, 8 KB data blocks linked by the
// directory's next-block field.
struct MemoryCardFile {
    std::string name;
    uint32_t size = 0;
    int firstBlock = 0;
    std::vector<uint8_t> bytes; // size bytes, following the block chain
};
std::vector<MemoryCardFile> ReadMemoryCardFiles(std::span<const uint8_t> image);

uint32_t Crc32(std::span<const uint8_t> bytes); // standard CRC-32 (zlib), as 0x80083178

// One car of the garage (0xA4 bytes; written by 0x8001EC0C).
struct GarageCar {
    uint32_t carId = 0;          // +00  packed car id (car_info.h)
    uint32_t colour = 0;         // +04  paint id: a character of the car's paint list ('b' = 0x62; gtmode_tables.md),
                                 //      0x80018350(carId, colour) -> colour name through .carcolor / .cclatain
    CarConfig config{};          // +08  parts and settings (car_params.h), incl. the builder's write-backs
    uint32_t modelId = 0;        // +8C  car id shown / raced (equals carId on purchase)
    uint32_t price = 0;          // +90  price paid (+ parts: 0x8005E8B0 adds part prices)
    uint16_t weightKg = 0;       // +94  bits 0..12: CarParams weightKg of the built record (0x1F80005A)
    uint8_t driveType = 0;       // +94  bits 13..15: CarParams driveType (0x1F80008A)
    uint16_t word96 = 0;         // +96  0x80075930 result +4 (torque figure; 243 for a 24.x kgm car)
    uint16_t power = 0;          // +98  bits 0..13: 0x80075930 result +0 (power figure)
    bool flag14 = false;         // +98  bit 14: gearbox row +9 < 3 (0x800178E4)
    bool flag15 = false;         // +98  bit 15: 0x80017750 (car analysis, *0x801DBC8E > 0)
    std::array<uint8_t, 7> partsOwned{}; // +9A  bit k = part kind k bought (0x8005E874 test, 0x8005E8B0 set)
    uint16_t wordA2 = 0;         // +A2  cleared on purchase; 0x80013EEC clears it for the current car
};

struct Garage {
    uint16_t count = 0;          // +0000 (max 100)
    std::vector<GarageCar> cars; // +0004 count x 0xA4
    uint32_t money = 0;          // +4014 credits, clamped to 0..99,999,999 by 0x8005E7B0; new game 10,000
    int16_t currentCar = -1;     // +4018 index of the car in use, -1 = none (0x801D156C)
    uint8_t byte401B = 0;        // +401B cleared by the new-game init
};

// Career record (state + 0xB8, 0x160 bytes; base 0x801C9998 in RAM).
struct CareerRecord {
    uint32_t days = 0;           // +40 (0x801C99D8) day counter, 1 on a new game, +1 per race; used-car lot = (days / 10) % 60
    uint32_t word48 = 0;         // +48
    uint32_t wins = 0;           // +4C (0x8005DC64: +1 when the position is 1)
    uint32_t positionSum = 0;    // +50 (0x8005DC64: + position)
    uint32_t races = 0;          // +54 (0x8005DC64: +1)
    uint32_t prizeHundredMillions = 0; // +58 (0x8005DC9C: carry of +5C)
    uint32_t prize = 0;          // +5C prize money total modulo 100,000,000
    // +60..: 4-bit results, entry n = byte (n / 2), low nibble for even n (0x8005DB90 get, 0x8005DBC0 set only
    // when better): 1 gold / 1st .. 3 bronze / 3rd, 0 none.
    std::vector<uint8_t> resultNibbles;
    uint8_t Result(size_t n) const { return n / 2 < resultNibbles.size() ? uint8_t((resultNibbles[n / 2] >> ((n & 1) * 4)) & 15) : 0; }
};

struct GameSave {
    std::vector<uint8_t> header;   // 0x200 bytes
    std::vector<uint8_t> state;    // 0x7C9C bytes = RAM 0x801C98E0..
    uint32_t storedCrc = 0, computedCrc = 0;
    bool CrcOk() const { return storedCrc == computedCrc; }

    uint8_t Language() const { return state.at(0); } // 1 = USA (selects the carparam file set, EXE 0x800925A4)
    CareerRecord Career() const;
    Garage GarageBlock() const;
};

// Parses a save file image (the 0x7EA0 bytes of "BASCUS-94455GAME"; trailing card padding is ignored).
GameSave ParseGameSave(std::span<const uint8_t> file);
// Parses the career state as it sits in RAM (0x7C9C bytes from 0x801C98E0), e.g. from a RAM dump.
GameSave GameSaveFromState(std::span<const uint8_t> state);
// Builds a file image around a state (header copied, CRC recomputed): the inverse of ParseGameSave.
std::vector<uint8_t> BuildGameSaveFile(std::span<const uint8_t> header, std::span<const uint8_t> state);

GarageCar ParseGarageCar(std::span<const uint8_t> slot);

} // namespace gt2
