#include "gt2formats/arcade_data.h"

#include <cstring>
#include <stdexcept>

#include "gt2vfs/gtfs.h"

namespace gt2 {
namespace {

uint16_t U16(std::span<const uint8_t> b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)) | (uint32_t(b[o + 3]) << 24); }

} // namespace

ArcadeData::ArcadeData(std::vector<uint8_t> gtdt) : tables_(gtdt), file_(std::move(gtdt)) {
    if (file_.EntryCount() < 2 * (kArcadePlayerCarTableAlt + 1)) throw std::runtime_error("arcade data: too few GTDT entries (not usa_arcade_data.dat)");
    if (file_.Entry(kArcadeEventTable).size % kRaceEventRowSize != 0 || file_.Entry(kArcadeOpponentTable).size % kOpponentRowSize != 0 ||
        file_.Entry(kArcadePlayerCarTable).size % kArcadePlayerCarRowSize != 0 || file_.Entry(kArcadePlayerCarTableAlt).size != file_.Entry(kArcadePlayerCarTable).size)
        throw std::runtime_error("arcade data: tables 30..33 are not whole rows");
    // The event names: extra 30, { u16 count; { u8 length; char name[length]; u8 0 } ... } (as usa_gtmode_race.dat's pool).
    const std::span<const uint8_t> pool = file_.Bytes(file_.EntryCount() / 2 + kArcadeEventTable);
    if (pool.size() < 2) throw std::runtime_error("arcade data: no event name pool");
    const size_t count = U16(pool, 0);
    size_t at = 2;
    for (size_t i = 0; i < count; i++) {
        if (at >= pool.size() || at + 1 + pool[at] + 1 > pool.size()) throw std::runtime_error("arcade data: name pool out of bounds");
        names_.emplace_back(reinterpret_cast<const char*>(pool.data() + at + 1), pool[at]);
        at += size_t(pool[at]) + 2;
    }
    for (size_t row = 0; row < EventCount(); row++) {
        const RaceEvent e = EventAt(row); // validates the names
        for (uint32_t slot : e.slots)
            if (RaceEvent::SlotOpponent(slot) > OpponentCount()) throw std::runtime_error("arcade data: event " + e.name + " names an opponent beyond table 31");
    }
    for (size_t row = 0; row < PlayerCarCount(); row++)
        if (U32(PlayerCarBytes(kArcadePlayerCarTable, row), 0) != U32(PlayerCarBytes(kArcadePlayerCarTableAlt, row), 0))
            throw std::runtime_error("arcade data: player car tables 32 / 33 list different cars");
}

ArcadeData ArcadeData::Load(const GtfsVolume& vol, const std::string& path) { return ArcadeData(vol.Read(path)); }

size_t ArcadeData::EventCount() const { return file_.Entry(kArcadeEventTable).size / kRaceEventRowSize; }

RaceEvent ArcadeData::EventAt(size_t row) const {
    if (row >= EventCount()) throw std::out_of_range("arcade data: event row");
    return ParseRaceEvent(file_.Bytes(kArcadeEventTable).subspan(row * kRaceEventRowSize, kRaceEventRowSize), names_, row);
}

int32_t ArcadeData::FindEvent(const std::string& name) const {
    for (size_t row = 0; row < EventCount(); row++)
        if (EventAt(row).name == name) return int32_t(row);
    return -1;
}

std::string ArcadeData::EventName(int level, char carClass) {
    if (level < 0 || level > 2) throw std::out_of_range("arcade level (0 Easy, 1 Normal, 2 Difficult)");
    return std::string("A") + char('0' + level) + carClass;
}

size_t ArcadeData::OpponentCount() const { return file_.Entry(kArcadeOpponentTable).size / kOpponentRowSize; }

std::span<const uint8_t> ArcadeData::OpponentBytes(uint32_t number) const {
    if (number == 0 || number > OpponentCount()) throw std::out_of_range("arcade data: opponent number");
    return file_.Bytes(kArcadeOpponentTable).subspan((number - 1) * kOpponentRowSize, kOpponentRowSize);
}

OpponentCarRow ArcadeData::Opponent(uint32_t number) const {
    OpponentCarRow o;
    std::memcpy(&o, OpponentBytes(number).data(), sizeof(o));
    return o;
}

CarConfig ArcadeData::OpponentConfig(uint32_t number) const { return OpponentCarConfig(tables_, OpponentBytes(number), false); }

size_t ArcadeData::PlayerCarCount() const { return file_.Entry(kArcadePlayerCarTable).size / kArcadePlayerCarRowSize; }

std::span<const uint8_t> ArcadeData::PlayerCarBytes(size_t table, size_t row) const {
    if (table != kArcadePlayerCarTable && table != kArcadePlayerCarTableAlt) throw std::out_of_range("arcade data: player car table");
    if (row >= PlayerCarCount()) throw std::out_of_range("arcade data: player car row");
    return file_.Bytes(table).subspan(row * kArcadePlayerCarRowSize, kArcadePlayerCarRowSize);
}

uint32_t ArcadeData::PlayerCarId(size_t row) const { return U32(PlayerCarBytes(kArcadePlayerCarTable, row), 0); }

std::optional<size_t> ArcadeData::FindPlayerCar(uint32_t carId) const {
    for (size_t row = 0; row < PlayerCarCount(); row++)
        if (PlayerCarId(row) == carId) return row;
    return std::nullopt;
}

CarConfig ArcadeData::PlayerCarConfig(size_t row, size_t table) const { return ConfigFromCarSpec(tables_, PlayerCarBytes(table, row), false); }

} // namespace gt2
