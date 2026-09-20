#pragma once
// GT-mode menu data: the used-car lots (.usedcar_usa, "UCAR"), the car catalogue (tables 29 / 30 of
// carparam/usa_gtmode_data.dat), the event tables (carparam/usa_gtmode_race.dat), the string database
// (carparam/usa_unistrdb.dat, "WSDB"), the paint colour names (.carcolor + .cclatain / .ccjapanese), the car directory
// words of .carinfoa that the menus use, and the containers of the menu pictures (gtmenu/*).
//
// Derived from the bytes of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a) and
// our disassembly of the EXE and of GT2.OVL member 4 (the GT-mode menus, loaded at 0x80010000). Layouts, code
// addresses and evidence: docs/formats/gtmode_tables.md. Offsets in comments are hexadecimal.
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "gt2formats/car_params.h"

namespace gt2 {

class GtfsVolume;

// The shell's language byte 0x801C98E0 (column index of the EXE file table 0x800925A4: 0 = jpn / unprefixed, 1 =
// usa, 2 = eng, 3 = fra, 4 = ger, 5 = ita, 6 = spa). The US disc runs with 1.
constexpr uint8_t kLanguageUsa = 1;

// ---------------------------------------------------------------- .carinfoa directory words

// One car of the .carinfo directory ({ u32 packedId; u32 word } at +8, sorted by id; binary search 0x80060A24).
// The menus read the word through 0x80060A88 / 0x80060B30: bits 0..17 = offset of the car's entry, bits 18..22 =
// paint count - 1, bits 23..26 = region exclusion mask (0x80060B70).
struct CarInfoRecord {
    uint32_t carId = 0;
    uint32_t word = 0;
    std::string name;               // NUL-terminated name at the end of the entry
    std::string rawName;            // the bytes 0x80060AE8 returns (entry + 3 x paints + 1): the 0x7F padding + the name
    std::vector<uint16_t> chipColors;
    std::vector<uint8_t> paintIds;  // one character per paint (the used-car / event paint codes use these)

    size_t PaintCount() const { return ((word >> 18) & 0x1F) + 1; }
    uint8_t RegionMask() const { return uint8_t((word >> 23) & 0xF); }
    // 0x80060B70: language 0 tests bit 0, 1 bit 1, 2 bit 3, 3..6 bit 2; a set bit hides the car.
    bool AvailableIn(uint8_t language) const;
    // Index of `paintId` in paintIds, -1 when the car has no such paint.
    int PaintIndex(uint8_t paintId) const;
};

class CarInfoDirectory {
public:
    // `data` = .carinfoa (the US EXE loads file 1 = /.carinfoa: 0x800609F8).
    explicit CarInfoDirectory(std::vector<uint8_t> data);
    static CarInfoDirectory Load(const GtfsVolume& vol, const std::string& path = ".carinfoa");
    size_t Count() const { return records_.size(); }
    const CarInfoRecord& At(size_t index) const { return records_.at(index); }
    int32_t IndexOf(uint32_t carId) const; // -1 when absent
    const CarInfoRecord* Find(uint32_t carId) const;

private:
    std::vector<CarInfoRecord> records_;
};

// ---------------------------------------------------------------- .usedcar_usa ("UCAR")

// "UCAR", u32 0, u32 periodOffset[61] (the last = file size), then per period 39 x { u16 listOffset; u16 count } (from
// the period's start) followed by the lists of 8-byte entries. 0x800224E0(counter, buffer) copies period
// (counter % 60) to 0x800B9544 and drops the entries whose car is hidden in the language (0x80060B70); the caller
// 0x800136B0 passes u32[0x801C99D8] / 10.
constexpr size_t kUsedCarPeriodCount = 60;
constexpr size_t kUsedCarMakerCount = 39; // one list per manufacturer index (catalogue row + 3A)

struct UsedCarEntry {
    uint32_t carId = 0;   // +0 packed car id
    uint32_t price = 0;   // +4 u32 & 0xFFFFFF (0x800207F8), credits
    uint8_t paintId = 0;  // +7 paint character (one of the car's CarInfoRecord::paintIds)
};

class UsedCarLists {
public:
    explicit UsedCarLists(std::vector<uint8_t> data);
    static UsedCarLists Load(const GtfsVolume& vol, const std::string& path = ".usedcar_usa");
    // The list as stored (sorted by price).
    std::vector<UsedCarEntry> List(size_t period, size_t maker) const;
    // The list as the lot shows it: List() without the cars hidden in `language` (0x800224E0's filter).
    std::vector<UsedCarEntry> Lot(size_t period, size_t maker, const CarInfoDirectory& cars, uint8_t language = kLanguageUsa) const;
    // 0x800136B0 + 0x800224E0: the period shown for the value of the counter u32[0x801C99D8].
    static size_t PeriodOfCounter(uint32_t counter) { return (counter / 10) % kUsedCarPeriodCount; }

private:
    std::vector<uint8_t> data_;
    std::array<uint32_t, kUsedCarPeriodCount + 1> periods_{};
};

// ---------------------------------------------------------------- car catalogue (carparam gtmode_data tables 29 / 30)

constexpr size_t kCarProfileTable = 29;   // 8-byte rows, indexed by CarSpec::profileRow
constexpr size_t kCarCatalogueTable = 30; // 72-byte rows, one per car, same order / count as the chassis table 3

#pragma pack(push, 1)
// Head shared by the catalogue rows (table 30) and the opponent rows (race table 1): the car id and one row index
// per part table. 0x80076FC0 copies it into a CarConfig with the mapping table 0x80092BB4 (note the order of
// lsd / gearbox / suspension and of intercooler / muffler, which differs from CarConfig).
struct CarSpec {
    uint32_t carId;          // +00
    uint16_t brakes;         // +04 table 0
    uint16_t brakeController; // +06 table 1
    uint16_t steering;       // +08 table 2
    uint16_t chassis;        // +0A table 3
    uint16_t lightweight;    // +0C table 4
    uint16_t racingModify;   // +0E table 5
    uint16_t engine;         // +10 table 6
    uint16_t portPolish;     // +12 table 7
    uint16_t engineBalance;  // +14 table 8
    uint16_t displacement;   // +16 table 9
    uint16_t computer;       // +18 table 10
    uint16_t naTune;         // +1A table 11
    uint16_t turboKit;       // +1C table 12
    uint16_t drivetrain;     // +1E table 13
    uint16_t flywheel;       // +20 table 14
    uint16_t clutch;         // +22 table 15
    uint16_t propellerShaft; // +24 table 16
    uint16_t lsd;            // +26 table 21
    uint16_t gearbox;        // +28 table 17
    uint16_t suspension;     // +2A table 18
    uint16_t intercooler;    // +2C table 19
    uint16_t muffler;        // +2E table 20
    uint16_t tyresFront;     // +30 table 22
    uint16_t tyresRear;      // +32 table 23
    uint16_t activeStability; // +34 table 27
    uint16_t tractionControl; // +36 table 28
    uint16_t profileRow;     // +38 row of table 29 -> CarConfig::word38
};
struct CarCatalogueRow { // table 30
    CarSpec spec;
    uint8_t maker;           // +3A manufacturer index 0..38 (the used-car list index); -> CarConfig::byte79 in GT mode
                             //     (the attract player's byte79 = 29 is its maker)
    uint8_t byte3B;          // 0
    uint16_t modelName;      // +3C string index of carparam/usa_unistrdb.dat (model)
    uint16_t gradeName;      // +3E string index (grade / variant)
    uint8_t raceCar;         // +40 1 in 88 rows (all 80 ids ending in 'r' + 8); their prices are 500000 / 1000000 / 2000000
    uint8_t year;            // +41 two-digit model year, 0 when not given
    uint16_t pad42;          // 0
    uint32_t price;          // +44 new-car price, credits
};
struct OpponentCarRow { // race table 1
    CarSpec spec;
    uint16_t finalDrive;     // +3A -> CarConfig::finalDrive (0x80092C24)
    uint8_t gearAutoFinal;   // +3C -> gearAutoFinal
    uint8_t diffInitialFront; // +3D -> diffInitial[0]
    uint8_t diffAccelFront;  // +3E -> diffAccel[0]
    uint8_t diffDecelFront;  // +3F -> diffDecel[0]
    uint8_t diffInitialRear; // +40 -> diffInitial[1]
    uint8_t diffAccelRear;   // +41 -> diffAccel[1]
    uint8_t diffDecelRear;   // +42 -> diffDecel[1]
    uint8_t downforce[2];    // +43 -> downforce
    uint8_t camber10[2];     // +45 -> camber10
    uint8_t toeCode[2];      // +47 -> toeCode
    uint8_t rideHeightMm[2]; // +49 -> rideHeightMm
    uint8_t springCode[2];   // +4B -> springCode
    uint8_t damperLevel[8];  // +4D -> damperLevel
    uint8_t antiRollLevel[2]; // +55 -> antiRollLevel
    uint8_t asmLevel;        // +57 -> asmLevel
    uint8_t tcsLevel;        // +58 -> tcsLevel
    uint8_t raw59[3];        // 0
    uint16_t torqueMultiplier100; // +5C -> torqueMultiplier100 (flags |= 1)
    uint16_t number;         // +5E row + 1: the value the event slots refer to
};
#pragma pack(pop)
static_assert(sizeof(CarSpec) == 0x3A && sizeof(CarCatalogueRow) == 72 && sizeof(OpponentCarRow) == 96);
static_assert(offsetof(CarCatalogueRow, price) == 0x44 && offsetof(OpponentCarRow, torqueMultiplier100) == 0x5C);

// Catalogue access over the parameter tables (the file the menus load with 0x80076D74).
size_t CarCatalogueCount(const CarParamTables& tables);
CarCatalogueRow CarCatalogueAt(const CarParamTables& tables, size_t row);
// 0x80077F54 on table 30: binary search by car id.
std::optional<size_t> FindCatalogueRow(const CarParamTables& tables, uint32_t carId);

// 0x80076FC0(spec, config): a car's configuration from a catalogue / opponent row - the part rows of the spec and
// the settings at the part rows' defaults (gearbox, racing modification, turbo, suspension, brake controller, LSD,
// drivetrain, ASM, TCS rows), toe 128, word00 from the table 29 row. `gtMode` = the global 0x80092878 == 2 (the
// GT-mode parameter file is loaded): flags |= 0x40 and byte79 = spec byte + 3A.
CarConfig ConfigFromCarSpec(const CarParamTables& tables, std::span<const uint8_t> specRow, bool gtMode = true);
// 0x80076954(carId, config): ConfigFromCarSpec of the car's catalogue row; nullopt when the car is not in table 30
// (the original then uses row 0 and returns 0).
std::optional<CarConfig> CatalogueCarConfig(const CarParamTables& tables, uint32_t carId);
// 0x80076F5C(opponentRow, config): ConfigFromCarSpec, then the opponent's settings (mapping 0x80092C24) and flags |= 1.
// `gtMode` as for ConfigFromCarSpec (false for the arcade races: no flag 0x40, byte79 = 255).
CarConfig OpponentCarConfig(const CarParamTables& tables, std::span<const uint8_t> opponentRow, bool gtMode = true);

// ---------------------------------------------------------------- carparam/usa_gtmode_race.dat

// GTDT with 6 entries: tables 0..2 (row sizes 156, 96, 128 from EXE 0x80092490) and their extras; extra 0 (entry 3)
// is the name pool of the event names and course names, entries 4 / 5 are empty.
constexpr size_t kRaceEventTable = 0;    // 0x9C-byte rows: the events (same layout as the licence tests)
constexpr size_t kOpponentTable = 1;     // 0x60-byte rows: OpponentCarRow
constexpr size_t kCarListTable = 2;      // 0x80-byte rows: 32 x u32 packed car id, 0-terminated
constexpr size_t kRaceEventRowSize = 0x9C;
constexpr size_t kOpponentRowSize = 0x60;
constexpr size_t kCarListRowSize = 0x80;
constexpr size_t kEventSlotCount = 16;

struct RaceEvent {
    size_t row = 0;
    uint16_t nameIndex = 0, courseIndex = 0; // +00 / +02 name pool indices (course "none": chosen elsewhere)
    std::string name, course;
    // +04: 16 slots, u32 = (paint code << 26) | opponent number (race table 1 row + 1; 0x800768C0 masks 0x3FFFFFF and
    // subtracts 1); the paint code indexes the character set "-0-9a-z" at EXE 0x80091620. Used slots are leading.
    std::array<uint32_t, kEventSlotCount> slots{};
    // +44: the race settings block copied to 0x801C98A0 (licence.md section 4).
    std::array<uint8_t, 0x40> settings{};
    std::array<uint32_t, 6> prize{};         // +78: u16 x 6 times 100 (0x8001928C), credits by finishing position
    std::array<uint32_t, 4> prizeCars{};     // +84: packed car ids (0 = none)
    uint16_t tagNameIndex = 0;               // +94: name pool index of a tag string (0x80010078 strcpy's it to 0x801D58A0)
    std::string tag;
    uint16_t powerLimit = 0;                 // +96: 0 = none; the check fails when the car's power exceeds it
    uint32_t bonus = 0;                      // +98: u16 times 100
    uint8_t aspiration = 0;                  // +9A: 0 none, 1 / 2 (see the doc)
    uint8_t byte9B = 0;                      // +9B: 0 none, 1 / 2 (see the doc)
    std::vector<uint8_t> bytes;              // the whole row

    size_t OpponentCount() const;            // 0x8007812C: leading non-zero slots
    static uint32_t SlotOpponent(uint32_t slot) { return slot & 0x3FFFFFFu; }
    static char SlotPaint(uint32_t slot);    // the character of the slot's paint code
    uint8_t StartSpeed() const { return settings[0x00]; }      // km/h, 0 = standing start
    uint8_t Laps() const { return settings[0x01]; }            // 0x80010078 -> race block + 0x0F
    uint8_t LicenceRequired() const { return settings[0x03]; } // 0 none, 1..6 (0x80019750)
    uint8_t DirtTyresRequired() const { return settings[0x31]; }
    uint8_t CarListIndex() const { return settings[0x32]; }    // race table 2 row + 1, 0 = no list
    uint8_t DriveRestriction() const { return settings[0x33]; } // 0 none; 1 FF, 2 FR, 3 MR, 4 RR, 5 4WD
};

// One 0x9C-byte event row (names = the file's name pool). Shared by usa_gtmode_race.dat (table 0) and the arcade
// events of usa_arcade_data.dat (table 30, gt2formats/arcade_data.h).
RaceEvent ParseRaceEvent(std::span<const uint8_t> row, const std::vector<std::string>& names, size_t rowIndex);

class GtModeRaceData {
public:
    explicit GtModeRaceData(std::vector<uint8_t> gtdt);
    static GtModeRaceData Load(const GtfsVolume& vol, const std::string& path = "carparam/usa_gtmode_race.dat");

    const std::vector<std::string>& Names() const { return names_; }
    size_t EventCount() const;
    RaceEvent EventAt(size_t row) const;
    // 0x800781E0 with the race file: the name's pool index, then a binary search of table 0 on the u16 at + 0.
    int32_t FindEvent(const std::string& name) const;
    size_t OpponentCount() const;
    // The opponent with `number` (1-based, as in the event slots).
    OpponentCarRow Opponent(uint32_t number) const;
    std::span<const uint8_t> OpponentBytes(uint32_t number) const;
    size_t CarListCount() const;
    // Race table 2 row `index - 1` (0x8001924C); empty for index 0.
    std::vector<uint32_t> CarList(uint8_t index) const;

private:
    GtdtFile file_;
    std::vector<std::string> names_;
};

// ---------------------------------------------------------------- carparam/usa_unistrdb.dat ("WSDB")

// u32 file size, "WSDB", u16 count, count x { u16 length; u16 text[length]; u16 0 } (UTF-16LE).
std::vector<std::u16string> ParseUniStrDb(std::span<const uint8_t> data);
std::string Utf16ToUtf8(const std::u16string& s);

// ---------------------------------------------------------------- .carcolor + colour names

// .carcolor: "CCOL00\0\0", u16 offset[n] (n = (offset[0] - 8) / 2 = the .carinfo car count, in directory order),
// entry = u16 colourName[paintCount of the car]. .cclatain / .ccjapanese: u16 offset[count] (count = offset[0] / 2) of
// NUL-terminated names. 0x800222E4 loads .carcolor (file 0) and .cclatain (file 5; .ccjapanese = file 4).
class CarColorNames {
public:
    CarColorNames(std::vector<uint8_t> carcolor, std::vector<uint8_t> names);
    static CarColorNames Load(const GtfsVolume& vol, bool latin = true);
    size_t CarCount() const { return carOffsets_.size(); }
    // The colour-name indices of the car at `carInfoIndex` (CarInfoDirectory order).
    std::vector<uint16_t> NameIndices(size_t carInfoIndex, size_t paintCount) const;
    std::string Name(uint16_t index) const;
    size_t NameCount() const { return nameOffsets_.size(); }

private:
    std::vector<uint8_t> carcolor_, names_;
    std::vector<uint16_t> carOffsets_, nameOffsets_;
};

// ---------------------------------------------------------------- gtmenu containers

// gtmenu/<lang>/gtmenudat.idx and gtmenu/commonpic.idx: u32 count, u32 offset[count + 1] into the .dat. Entry i
// starts at offset[i] & ~3 and the loader (0x80021078 / 0x8002117C) reads offset[i + 1] - offset[i] bytes from there.
// gtmenudat entries are single gzip members (inflated: "GM\x03\0" pages, parsed by 0x800213C4); commonpic entries
// are stored ("GTMP" magic, 4-KB aligned). The language table at overlay 0x800529E4 (8 bytes per language) holds the
// file indices { gtmenudat.dat, iconimg.dat, solodata.dat, gtmenudat.idx }.
struct MenuPackIndex {
    std::vector<uint32_t> offsets; // count + 1
    size_t Count() const { return offsets.empty() ? 0 : offsets.size() - 1; }
    uint32_t Start(size_t i) const { return offsets.at(i) & ~3u; }
    uint32_t ReadLength(size_t i) const { return offsets.at(i + 1) - offsets.at(i); }
};
MenuPackIndex ParseMenuPackIndex(std::span<const uint8_t> idx);
// The entry's bytes as the loader reads them (`dat` = the stored .dat file); gunzipped when `gzip`.
std::vector<uint8_t> MenuPackEntry(std::span<const uint8_t> dat, const MenuPackIndex& index, size_t i, bool gzip);

// gtmenu/<lang>/solodata.dat (inflated): u32 n, u16 page[n] (padded to 4 bytes), u32 m, m x { u32 carId; u32 value }
// sorted by car id (0x80020E4C loads it; 0x80020F54 = binary search -> value, 0 when absent; 0x800211FC(i) with bit
// 31 set loads gtmenudat entry page[i & 0x7FFFFFFF]).
struct SoloData {
    std::vector<uint16_t> pages;
    std::vector<std::pair<uint32_t, uint32_t>> cars;
    std::optional<uint32_t> ValueOf(uint32_t carId) const;
};
SoloData ParseSoloData(std::span<const uint8_t> data);

// gtmenu/<lang>/iconimg.dat (inflated, 32768 bytes): raw VRAM image, uploaded by 0x80020ECC with LoadImage to
// x 704, y 0, 64 x 256 (16-bit units).
constexpr int kIconImageVramX = 704, kIconImageVramY = 0, kIconImageWidth = 64, kIconImageHeight = 256;

} // namespace gt2
