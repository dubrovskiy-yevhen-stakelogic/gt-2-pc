#pragma once
// GT-mode career state: the block the original keeps at RAM 0x801C98E0 ("GameState"; the first 0x7C9C bytes are the
// save game), laid out exactly like the original so that the same struct works on a RAM image (gt2verify) and on a
// save file. Derived from US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): our
// disassembly of the new-game initialiser (EXE 0x800104A0, runs once at boot before the title overlay overwrites it),
// the record initialisers 0x8005DD68 / 0x8005DE1C / 0x8005E07C / 0x800107B4 / 0x80010798, the save class
// 0x80069FF8..0x8006A31C and the GT-mode overlay's garage code. Evidence: docs/research/menus_gtmode.md sections 3-5.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/car_params.h"
#include "gt2formats/save_data.h"

namespace gt2 {
class DiscImage;
struct GuestImage;
} // namespace gt2

namespace gt2::career {

constexpr uint32_t kStateAddress = 0x801C98E0u;   // the career block in RAM
constexpr uint32_t kGarageAddress = 0x801CD554u;  // state + 0x3C74: garage of player 0; player 1 (guest garage) at + 0x4028
constexpr size_t kSavedStateSize = 0x7C9C;        // bytes of the block that the save file carries
constexpr size_t kCourseRecordSlots = 0x80;       // room for course records (+0x218 .. +0x1418, 0x24 each)
constexpr size_t kLicenceCount = 6, kLicenceTests = 10;
constexpr int kGarageCapacity = 100;
constexpr int32_t kMoneyLimit = 99999999;         // 0x8005E7B0 clamps the money to 0..99,999,999
constexpr int32_t kNewGameMoney = 10000;          // 0x80010798

#pragma pack(push, 1)
// One car of the garage (0xA4 bytes; written by 0x8001EC0C, copied as a whole by 0x8001EDAC / 0x8001EF10 / 0x8005E7F0).
struct GarageCar {
    uint32_t carId;              // +00 packed car id (car_info.h)
    uint32_t paint;              // +04 paint character of the car's paint list ('b' = 0x62)
    CarConfig config;            // +08 parts and settings (car_params.h)
    uint32_t modelId;            // +8C car id shown / raced (= carId on purchase)
    int32_t value;               // +90 purchase price (+ part prices, 0x8005E8B0)
    uint16_t weightDrive;        // +94 bits 0..12 weight kg of the built record (+0x5A), bits 13..15 drive type (+0x8A)
    uint16_t torqueFigure;       // +96 0x80075930 result +4 (max torque, kgm x 10)
    uint16_t powerFlags;         // +98 bits 0..13 0x80075930 result +0 (max power, PS), bit 14 = gearbox row +9 < 3 (0x800178E4),
                                 //     bit 15 = the racing-modification stage of the car's own row > 0 (0x80017750)
    uint8_t partsOwned[7];       // +9A bit k = part kind k (0x80076570 numbering) bought
    uint8_t byteA1;
    uint16_t wordA2;             // +A2 cleared on purchase
};
static_assert(sizeof(GarageCar) == 0xA4);
static_assert(offsetof(GarageCar, config) == 0x08 && offsetof(GarageCar, modelId) == 0x8C && offsetof(GarageCar, weightDrive) == 0x94);
static_assert(offsetof(GarageCar, partsOwned) == 0x9A && offsetof(GarageCar, wordA2) == 0xA2);

// The garage of one player (0x4028 bytes; RAM 0x801CD554 + player * 0x4028).
struct GarageBlock {
    int16_t count;               // +0000 cars in use (0..100)
    uint16_t pad0002;
    GarageCar cars[kGarageCapacity]; // +0004
    int32_t money;               // +4014 credits (RAM 0x801D1568)
    int16_t currentCar;          // +4018 index of the car in use, -1 = none (RAM 0x801D156C)
    uint8_t byte401A;
    uint8_t byte401B;            // +401B cleared on a new game
    uint8_t tail[0x0C];
};
static_assert(sizeof(GarageBlock) == 0x4028);
static_assert(offsetof(GarageBlock, money) == 0x4014 && offsetof(GarageBlock, currentCar) == 0x4018 && offsetof(GarageBlock, byte401B) == 0x401B);

// Career record (state + 0xB8, 0x160 bytes; RAM 0x801C9998).
struct CareerRecord {
    uint8_t head[0x40];
    uint32_t days;               // +40 day counter: 1 on a new game, +1 per event / licence / machine-test race
    uint16_t word44, word46;     // +44 / +46 cleared by the title entry (ovl1 0x80011384)
    uint32_t word48;
    int32_t wins;                // +4C (0x8005DC64)
    int32_t positionSum;         // +50
    int32_t races;               // +54
    int32_t prizeCarry;          // +58 hundreds of millions of the prize total (0x8005DC9C), saturates at 0x7FFFFFFF
    uint32_t prizeTotal;         // +5C prize total modulo 100,000,000
    uint8_t results[0x100];      // +60 4-bit results, entry n = byte n / 2, low nibble for even n (0x8005DB90)
};
static_assert(sizeof(CareerRecord) == 0x160 && offsetof(CareerRecord, days) == 0x40 && offsetof(CareerRecord, results) == 0x60);

// Best-time record (0x14; 0x8005DD68 sets five u32 -1 and then the u16 at +0x10 to 0: {-1, -1, -1, -1, 0x0000, 0xFFFF}).
struct TimeRecord {
    int32_t time[4];
    uint16_t word10;
    uint16_t word12;
};
static_assert(sizeof(TimeRecord) == 0x14);

// Course record (0x24; 0x800106E0 initialises the time record of the first .crsinfo-count entries).
struct CourseRecord {
    TimeRecord best;
    uint8_t rest[0x10];
};
static_assert(sizeof(CourseRecord) == 0x24);

// Licence test record (0xA4; 0x8005DE1C): +1 non-zero = passed (0x8001915C tests it).
struct LicenceTestRecord {
    uint8_t byte0;
    uint8_t passed;
    uint8_t byte2;
    uint8_t byte3;
    TimeRecord times[5];         // +04
    uint8_t entries[5][0x0C];    // +68 (byte 0 of each cleared by 0x8005DE1C)
};
static_assert(sizeof(LicenceTestRecord) == 0xA4);

// Machine-test record (0xA4; 0x8005E07C: cleared, u32 at +8 of each of the 8 entries of 0x14 = -1).
struct MachineTestRecord {
    uint8_t bytes[0xA4];
};

// The saved part of the career block (0x7C9C bytes = file + 0x200 of "BASCUS-94455GAME").
struct CareerState {
    uint8_t language;            // +0000 1 = USA (the carparam file column of EXE 0x800925A4)
    uint8_t options[0xB7];       // +0001 options, button tables, pad configurations (docs/research/menus_gtmode.md section 3)
    CareerRecord record;         // +00B8
    CourseRecord courses[kCourseRecordSlots]; // +0218
    LicenceTestRecord licences[kLicenceCount][kLicenceTests]; // +1418 licence L = 0 S, 1 IA, 2 IB, 3 IC, 4 A, 5 B
    MachineTestRecord machineTests[3]; // +3A88 (race sub-modes 7 / 8 / 9)
    GarageBlock garage;          // +3C74
};
#pragma pack(pop)
static_assert(sizeof(CareerState) == kSavedStateSize);
static_assert(offsetof(CareerState, record) == 0xB8 && offsetof(CareerState, courses) == 0x218 && offsetof(CareerState, licences) == 0x1418);
static_assert(offsetof(CareerState, machineTests) == 0x3A88 && offsetof(CareerState, garage) == 0x3C74);

// Bytes of the executable the new-game initialiser copies (read from the disc's SCUS_944.88, never stored here):
// four 11-byte button tables (EXE 0x80091570, 0x8009157C, 0x80091588, 0x80091594) and a 20-byte pad configuration
// (0x800A6ED8), each written for both pads.
struct NewGameDefaults {
    std::array<std::array<uint8_t, 11>, 4> buttonTables{};
    std::array<uint8_t, 20> padConfig{};
    uint16_t courseCount = 0; // .crsinfo count (u16 at 0x801E18E0 + 6): the course records the initialiser sets
};
NewGameDefaults ReadNewGameDefaults(const GuestImage& exe, uint16_t courseCount);
NewGameDefaults ReadNewGameDefaults(const DiscImage& disc); // + the course count of the disc's .crsinfo

// 0x800104A0: the new-game values on a cleared block (the original runs on the zeroed BSS; `state` is cleared first
// by NewCareer, the RAM variant only writes what the original writes). `guestGarage` = the second garage block that
// follows the saved part in RAM (0x801D157C), or null.
void InitNewCareer(CareerState& state, GarageBlock* guestGarage, const NewGameDefaults& defaults);
CareerState NewCareer(const NewGameDefaults& defaults);

// Record initialisers (EXE): 0x8005DD68 time record, 0x8005DE1C licence test record, 0x8005E07C machine-test record,
// 0x800107B4 career record, 0x80010798 garage.
void InitTimeRecord(TimeRecord& r);
void InitLicenceTestRecord(LicenceTestRecord& r);
void InitMachineTestRecord(MachineTestRecord& r);
void InitCareerRecord(CareerRecord& r);
void InitGarage(GarageBlock& g);

// ---------------------------------------------------------------- save game

// A career loaded from a card image or a save file, with the file's header kept for a byte-identical write-back.
struct CareerSave {
    CareerState state{};
    std::vector<uint8_t> header;     // the 0x200-byte "SC" header of the file
    uint32_t storedCrc = 0, computedCrc = 0;
    bool CrcOk() const { return storedCrc == computedCrc; }
};

// The save file ("BASCUS-94455GAME", 0x7EA0 bytes); throws on a malformed file (the CRC is reported, not enforced).
CareerSave LoadCareerSaveFile(std::span<const uint8_t> file);
// A PS1 memory card image (.mcd, 128 KB) holding "BASCUS-94455GAME".
CareerSave LoadCareerFromCard(std::span<const uint8_t> card);
// Loads either (by size / magic) from a path.
CareerSave LoadCareer(const std::string& path);

// 0x8006A038: the "SC" header of a new save file from the executable's data: +2 icon flags 0x13, +3 blocks (4),
// +4 the title (EUC-JP at EXE 0x80091CA8, converted to Shift-JIS by 0x8007D370), +0x60 the CLUT (0x80091CC4, 32
// bytes), +0x80 three icon frames (0x80091CE4, 3 x 128 bytes).
std::vector<uint8_t> BuildSaveHeader(const GuestImage& exe);

// The file image (header + state + CRC-32, 0x7EA0 bytes).
std::vector<uint8_t> BuildCareerSaveFile(const CareerSave& save);
// Writes the save file into a card image in place: the file's existing blocks when the card already holds it,
// else free blocks (directory frame 0x51 / 0x52 / 0x53 chain, XOR checksum). Throws when there is no room.
void StoreCareerOnCard(std::vector<uint8_t>& card, const CareerSave& save);
// Writes `save` to `path` in the format chosen by the extension: ".mcd" = card image (the existing card at `path`
// or `templateCard` is updated, else a formatted card is created), anything else = the bare save file.
void SaveCareer(const std::string& path, const CareerSave& save, std::span<const uint8_t> templateCard = {});

// A formatted empty 128 KB card image (header "MC", 15 free directory frames, checksums).
std::vector<uint8_t> FormatMemoryCard();

std::vector<uint8_t> ReadFileBytes(const std::string& path);
void WriteFileBytes(const std::string& path, std::span<const uint8_t> bytes);

} // namespace gt2::career
