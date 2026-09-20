#pragma once
// carparam/usa_arcade_data.dat: the arcade shell's car tables and race tables (US Arcade v1.1, SCUS_944.55 SHA-1
// 231f9dba7191b9ef915621662afdc40a7c66df95; docs/research/arcade_disc.md section 11). A GTDT with 34 tables (+ 34
// extras):
//   0..29  the car part tables of car_params.h (the layout of usa_gtmode_data.dat);
//   30     the arcade race events, 0x9C-byte rows in the RaceEvent layout of gtmode_tables.h, names in extra 30:
//          "A<level><class>" (level 0 / 1 / 2 = the menu's Easy / Normal / Difficult, class A / B / C / S = the car class
//          of the car selection), "A2P" (no opponents), "ADT" / "ATT" (80 km/h rolling start, no opponents);
//   31     the opponent cars, OpponentCarRow (0x60 bytes); an event slot's opponent number is row + 1;
//   32, 33 the player's cars: one 60-byte row per car (a CarSpec + u16 + u16), the same 63 cars in the same order;
//          the two tables differ only in the tyre rows (+0x30 / +0x32) of 37 cars.
// The race launcher (GT2.OVL member 3, 0x800121DC) fills the race block's entries from them: the player's car =
// 0x80076ED0 (Simulation 0x80076FC0 = ConfigFromCarSpec) of its row, the opponents = 0x80076E6C (Simulation 0x80076F5C
// = OpponentCarConfig) of the event's randomly drawn opponent rows (0x80011EC0), both without the GT-mode flag (byte79
// 255, flags bit 6 clear); the AI's engine multiplier comes from the opponent row (+0x5C: 100 in every row, flags bit 0).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/car_params.h"
#include "gt2formats/gtmode_tables.h"

namespace gt2 {

class GtfsVolume;

constexpr size_t kArcadeEventTable = 30;
constexpr size_t kArcadeOpponentTable = 31;
constexpr size_t kArcadePlayerCarTable = 32;    // first tyre choice
constexpr size_t kArcadePlayerCarTableAlt = 33; // second tyre choice
constexpr size_t kArcadePlayerCarRowSize = 60;

class ArcadeData {
public:
    explicit ArcadeData(std::vector<uint8_t> gtdt);
    static ArcadeData Load(const GtfsVolume& vol, const std::string& path = "carparam/usa_arcade_data.dat");

    const CarParamTables& Tables() const { return tables_; }
    const std::vector<std::string>& Names() const { return names_; }

    size_t EventCount() const;
    RaceEvent EventAt(size_t row) const;
    int32_t FindEvent(const std::string& name) const; // row, -1 when absent
    // "A" + level digit + class letter: the event of a level (0 Easy, 1 Normal, 2 Difficult) and car class.
    static std::string EventName(int level, char carClass);

    size_t OpponentCount() const;
    std::span<const uint8_t> OpponentBytes(uint32_t number) const; // number = row + 1
    OpponentCarRow Opponent(uint32_t number) const;
    CarConfig OpponentConfig(uint32_t number) const;                // OpponentCarConfig(row, arcade)

    size_t PlayerCarCount() const;
    std::span<const uint8_t> PlayerCarBytes(size_t table, size_t row) const;
    uint32_t PlayerCarId(size_t row) const;
    std::optional<size_t> FindPlayerCar(uint32_t carId) const;
    CarConfig PlayerCarConfig(size_t row, size_t table = kArcadePlayerCarTable) const; // ConfigFromCarSpec(row, arcade)

private:
    CarParamTables tables_;
    std::vector<std::string> names_;
    std::vector<uint8_t> bytes_;
    GtdtFile file_;
};

} // namespace gt2
