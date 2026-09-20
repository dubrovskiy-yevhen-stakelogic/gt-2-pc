#pragma once
// Licence tests: carparam/usa_license_data.dat (GTDT) - the tests' database records, their name pool and the licence
// cars, plus the car parameter tables the licence cars are built from. Derived from the bytes of US Simulation v1.2
// (EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a) and the RAM dump of licence test B-1 (work/re/license_race),
// where the file sits relocated at *(0x80092E6C) = 0x80169894. Format and provenance: docs/formats/license.md.
//
// Lookups of the original (race overlay / EXE):
//   0x8007830C(object, name) -> record pointer (0 = none): 0x800781E0 then 0x80078038.
//   0x800781E0(object, name): the name's index in the name pool of table 30 (0x80076C74), then a binary search of
//                             table 30 for the row whose u16 +0 equals it.
//   0x80076C74(pool, name):   { u16 count; count x { u8 length; char name[length]; u8 0 } } -> index of the first
//                             entry equal to `name` (strcmp 0x8008CF00), -1 when absent.
//   0x80078038(table, i):     table.pointer + table.rowSize * i.
//   0x8003D7B8(settings, m):  medal time m of the settings block (race_shell.h LicenseTargetTime).
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/car_params.h"

namespace gt2 {

class GtfsVolume;

constexpr size_t kLicenseTestTable = 30;       // 0x9C-byte rows: the tests
constexpr size_t kLicenseCarTable = 31;        // 0x60-byte rows: the licence cars
constexpr size_t kLicenseTestRowSize = 0x9C;
constexpr size_t kLicenseCarRowSize = 0x60;
constexpr size_t kLicenseSettingsOffset = 0x44; // the race settings block 0x801C98A0 inside a test row
constexpr size_t kLicenseSettingsSize = 0x40;
constexpr size_t kLicenseCarSlots = 16;         // row + 4: 16 x 4 bytes (slots 0 / 1 used)

// One car slot of a test row (row + 4 + slot * 4): u32 = (paint code << 26) | licence car number, the layout of the
// event slots of usa_gtmode_race.dat (0x80010078 / ovl0 0x8004C7A0 mask 0x3FFFFFF and shift 26).
struct LicenseCarSlot {
    uint8_t carNumber = 0;   // +0  low byte of the number: the licence car = table 31 row number - 1 (0 = no car)
    uint8_t byte1 = 0, byte2 = 0;
    uint8_t byte3 = 0;       // +3  paint code << 2 (0x14 for B-1: code 5 = '4')
    uint32_t word = 0;       // the whole u32

    uint32_t Number() const { return word & 0x3FFFFFFu; }
    uint32_t PaintCode() const { return word >> 26; }
};

// The race car of a test as the licence race builders set up the shell's car slot 0 (GT-mode ovl4 0x80010078 and its
// copy in the race overlay 0x8004C7A0, identical code): the table 31 row of slot 0's number (0x800768C0 -> 0x80077D5C
// (object, 31, number - 1)), the configuration from it (0x80076FC0 with the licence database selected: global
// 0x80092878 = 3, so no flag 0x40 and byte79 = 255), the car id = row + 0, replaced by the racing-modification body
// (table 5 row + 8, 0x80076F2C(5, config +0x1C)) when config +0x1C != 0; the paint character = EXE 0x80091620[code].
// Test slot 1 (six tests) is never read by any code of the US v1.2 build: those tests run with slot 0's car alone
// (checked on the original: B-7 = LJB06 race, work/re/lic2_b7, one car in 0x800AF231, slot 1 of the race block empty).
struct LicenseRaceCar {
    uint32_t carId = 0;      // 0x801D58B8
    uint32_t paintCode = 0;  // slot >> 26
    char paint = 0;          // 0x801D58BC: the character of the code
    CarConfig config{};      // 0x801D58C0 (before the builder 0x800771AC writes back into it)
};

// One licence test (a table 30 row).
struct LicenseTest {
    size_t row = 0;                         // index in table 30
    uint16_t nameIndex = 0;                 // +0x00  name pool index of the test name ("LJB00")
    uint16_t courseIndex = 0;               // +0x02  name pool index of the course file name ("TC_lisence")
    std::string name, course;
    std::array<LicenseCarSlot, kLicenseCarSlots> cars{}; // +0x04
    // +0x44: the shell's race settings block 0x801C98A0.. (0x40 bytes; the dump's block equals it byte for byte).
    std::array<uint8_t, kLicenseSettingsSize> settings{};
    uint32_t word84 = 0;                    // +0x84  unknown (non-zero only in the first test of each licence)
    // +0x94: u16 name pool index of the sponsor category (ovl4 0x80010078 / ovl0 0x8004C7A0: strcpy to race block + 0x44 =
    // 0x801D58A0, with + 9 = 1; the race load 0x800275E8 places no boards when no .crstims.tsd category has that name, e.g. "0").
    uint16_t tagNameIndex = 0;
    std::string tag;
    std::vector<uint8_t> bytes;             // the whole row

    uint8_t TargetLap() const { return settings[1]; }   // 0x801C98A1
    uint8_t Type() const { return settings[2]; }        // 0x801C98A2: 2 stop in the box, 3 reach the goal, 5 N laps (1: control class 1)
    uint8_t BoxStart() const { return settings[0x24]; } // 0x801C98C4: x 10 m of course distance
    uint8_t BoxLength() const { return settings[0x25]; } // 0x801C98C5: m
    // 0x8003D7B8 on the settings block: medal `medal` (1 gold, 2 silver, 3 bronze, 4 the fourth prize of the results
    // screen 0x8002B170) in 1/1000 s; the pair at + 0x26 + 2 * medal is {minutes * 100 + seconds, 1/100 s}.
    uint32_t MedalTime(uint32_t medal) const;
};

class LicenseData {
public:
    // `gtdt` = the decompressed carparam/usa_license_data.dat.
    explicit LicenseData(std::vector<uint8_t> gtdt);
    static LicenseData Load(const GtfsVolume& vol, const std::string& path = "carparam/usa_license_data.dat");

    // 0x80076C74 on the name pool of table 30: the index of `name`, -1 when absent.
    int32_t NameIndex(const std::string& name) const;
    const std::vector<std::string>& Names() const { return names_; }
    // 0x800781E0: the row of table 30 of the test `name` (binary search on the u16 name index at + 0), -1 when absent.
    int32_t FindTestRow(const std::string& name) const;
    size_t TestCount() const;
    LicenseTest TestAt(size_t row) const;
    // The test `name` ("LJB00"); throws when absent.
    LicenseTest Test(const std::string& name) const;
    // The licence car of `carNumber` (table 31 row with u16 + 0x5E == carNumber): its packed id (+0); throws when absent.
    uint32_t CarId(uint8_t carNumber) const;
    std::span<const uint8_t> CarRow(uint8_t carNumber) const;
    // 0x800768C0(number): the table 31 row number - 1 (what the race builders use; equals CarRow for the disc's data).
    std::span<const uint8_t> CarRowAt(uint32_t number) const;
    size_t CarCount() const;
    // The shell's car slot 0 of `test` (see LicenseRaceCar).
    LicenseRaceCar RaceCar(const LicenseTest& test) const;
    // Tables 0..29 have the car parameter layout (car_params.h): the licence cars are built from them.
    const CarParamTables& Tables() const { return tables_; }

private:
    GtdtFile file_;
    CarParamTables tables_;
    std::vector<std::string> names_;
};

// The test name of a licence label: "B-1" -> "LJB00" ... "B-10" -> "LJB09". Letters: B -> LJB, A -> LJA, IC -> LIC,
// IB -> LIB, IA -> LIA, S -> LIS (B-1 = LJB00 is the dump's; the rest follows the pool's naming). A name that already
// looks like a test name ("LJB00") is returned as is. Empty when the label is not understood.
std::string LicenseTestName(const std::string& label);

} // namespace gt2
