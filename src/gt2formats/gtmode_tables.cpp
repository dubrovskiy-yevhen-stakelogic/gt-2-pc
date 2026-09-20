#include "gt2formats/gtmode_tables.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "gt2vfs/gtfs.h"
#include "gt2vfs/inflate.h"

namespace gt2 {

namespace {

uint16_t U16(std::span<const uint8_t> b, size_t o) {
    if (o + 2 > b.size()) throw std::out_of_range("gtmode tables: read past the end");
    return uint16_t(b[o] | (b[o + 1] << 8));
}
uint32_t U32(std::span<const uint8_t> b, size_t o) {
    if (o + 4 > b.size()) throw std::out_of_range("gtmode tables: read past the end");
    return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24);
}

// EXE 0x80091620: the 6-bit character set of the packed car ids, indexed by an event slot's paint code.
constexpr char kCharset6[] = "-0123456789abcdefghijklmnopqrstuvwxyz";

// Part rows of CarConfig, byte offsets (car_params.h).
constexpr size_t kCfgGearbox = 0x10, kCfgSuspension = 0x12, kCfgLsd = 0x14, kCfgDrivetrain = 0x0E, kCfgBrakeController = 0x06;
constexpr size_t kCfgRacingModify = 0x1C, kCfgTurbo = 0x28, kCfgAsm = 0x34, kCfgTcs = 0x36, kCfgProfile = 0x38;

// One copy of a mapping table: destination (config) offset, source (row) offset.
struct ByteMap {
    uint8_t dst;
    uint8_t src;
};

} // namespace

// ---------------------------------------------------------------- .carinfoa

bool CarInfoRecord::AvailableIn(uint8_t language) const { // 0x80060B70
    const uint32_t mask = RegionMask();
    uint32_t bit;
    if (language == 0) bit = 0;
    else if (language == 1) bit = 1;
    else if (language == 2) bit = 3;
    else bit = 2;
    return ((mask >> bit) & 1) == 0;
}

int CarInfoRecord::PaintIndex(uint8_t paintId) const {
    for (size_t i = 0; i < paintIds.size(); i++)
        if (paintIds[i] == paintId) return int(i);
    return -1;
}

CarInfoDirectory::CarInfoDirectory(std::vector<uint8_t> data) {
    const std::span<const uint8_t> d(data);
    if (d.size() < 8 || std::memcmp(d.data(), "CAR\0", 4) != 0) throw std::runtime_error("carinfo: bad magic");
    const size_t count = U16(d, 4);
    for (size_t i = 0; i < count; i++) {
        CarInfoRecord r;
        r.carId = U32(d, 8 + i * 8);
        r.word = U32(d, 12 + i * 8);
        const size_t at = r.word & 0x3FFFF, n = r.PaintCount();
        if (at + 3 * n + 1 > d.size()) throw std::runtime_error("carinfo: entry out of bounds");
        for (size_t k = 0; k < n; k++) r.chipColors.push_back(U16(d, at + 2 * k));
        r.paintIds.assign(d.begin() + std::ptrdiff_t(at + 2 * n), d.begin() + std::ptrdiff_t(at + 3 * n));
        size_t p = at + 3 * n + 1; // the code byte, then 0x7F padding
        for (size_t q = p; q < d.size() && d[q] != 0; q++) r.rawName.push_back(char(d[q]));
        while (p < d.size() && d[p] == 0x7F) p++;
        size_t e = p;
        while (e < d.size() && d[e] != 0) e++;
        r.name.assign(reinterpret_cast<const char*>(d.data() + p), e - p);
        records_.push_back(std::move(r));
    }
}

CarInfoDirectory CarInfoDirectory::Load(const GtfsVolume& vol, const std::string& path) { return CarInfoDirectory(vol.Read(path)); }

int32_t CarInfoDirectory::IndexOf(uint32_t carId) const { // 0x80060A24 (binary search, ids sorted)
    const auto it = std::lower_bound(records_.begin(), records_.end(), carId, [](const CarInfoRecord& r, uint32_t id) { return r.carId < id; });
    if (it == records_.end() || it->carId != carId) return -1;
    return int32_t(it - records_.begin());
}

const CarInfoRecord* CarInfoDirectory::Find(uint32_t carId) const {
    const int32_t i = IndexOf(carId);
    return i < 0 ? nullptr : &records_[size_t(i)];
}

// ---------------------------------------------------------------- .usedcar_usa

UsedCarLists::UsedCarLists(std::vector<uint8_t> data) : data_(std::move(data)) {
    const std::span<const uint8_t> d(data_);
    if (d.size() < 8 + 4 * periods_.size() || std::memcmp(d.data(), "UCAR", 4) != 0) throw std::runtime_error("usedcar: bad magic");
    for (size_t i = 0; i < periods_.size(); i++) periods_[i] = U32(d, 8 + 4 * i);
    if (periods_[0] != 8 + 4 * periods_.size() || periods_.back() != d.size()) throw std::runtime_error("usedcar: period table does not span the file");
    for (size_t p = 0; p < kUsedCarPeriodCount; p++) {
        if (periods_[p + 1] < periods_[p] + 4 * kUsedCarMakerCount) throw std::runtime_error("usedcar: period too small");
        for (size_t m = 0; m < kUsedCarMakerCount; m++) {
            const size_t off = U16(d, periods_[p] + 4 * m), count = U16(d, periods_[p] + 4 * m + 2);
            if (periods_[p] + off + 8 * count > periods_[p + 1]) throw std::runtime_error("usedcar: list out of its period");
        }
    }
}

UsedCarLists UsedCarLists::Load(const GtfsVolume& vol, const std::string& path) { return UsedCarLists(vol.Read(path)); }

std::vector<UsedCarEntry> UsedCarLists::List(size_t period, size_t maker) const {
    if (period >= kUsedCarPeriodCount || maker >= kUsedCarMakerCount) throw std::out_of_range("usedcar: period / maker");
    const std::span<const uint8_t> d(data_);
    const size_t base = periods_[period];
    const size_t off = U16(d, base + 4 * maker), count = U16(d, base + 4 * maker + 2);
    std::vector<UsedCarEntry> out;
    for (size_t i = 0; i < count; i++) {
        const size_t e = base + off + 8 * i;
        out.push_back({U32(d, e), U32(d, e + 4) & 0xFFFFFFu, d[e + 7]});
    }
    return out;
}

std::vector<UsedCarEntry> UsedCarLists::Lot(size_t period, size_t maker, const CarInfoDirectory& cars, uint8_t language) const {
    std::vector<UsedCarEntry> out;
    for (const UsedCarEntry& e : List(period, maker)) { // 0x800225B8: keep when 0x80060B70(id) != 0
        const CarInfoRecord* r = cars.Find(e.carId);
        if (r && r->AvailableIn(language)) out.push_back(e);
    }
    return out;
}

// ---------------------------------------------------------------- catalogue

size_t CarCatalogueCount(const CarParamTables& tables) { return tables.RowCount(kCarCatalogueTable); }

CarCatalogueRow CarCatalogueAt(const CarParamTables& tables, size_t row) {
    const std::span<const uint8_t> r = tables.Row(kCarCatalogueTable, row);
    if (r.size() != sizeof(CarCatalogueRow)) throw std::runtime_error("catalogue: row size");
    CarCatalogueRow c;
    std::memcpy(&c, r.data(), sizeof(c));
    return c;
}

std::optional<size_t> FindCatalogueRow(const CarParamTables& tables, uint32_t carId) { // 0x80077F54
    size_t lo = 0, hi = CarCatalogueCount(tables);
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint32_t id = U32(tables.Row(kCarCatalogueTable, mid), 0);
        if (id == carId) return mid;
        if (id < carId) lo = mid + 1;
        else hi = mid;
    }
    return std::nullopt;
}

CarConfig ConfigFromCarSpec(const CarParamTables& tables, std::span<const uint8_t> src, bool gtMode) {
    if (src.size() < sizeof(CarSpec) + 1) throw std::runtime_error("car spec: row too short");
    std::array<uint8_t, sizeof(CarConfig)> c{}; // 0x80076A20: memset(config, 0, 0x84); byte79 = 255
    c[0x79] = 255;
    auto put16 = [&](size_t o, uint16_t v) { c[o] = uint8_t(v); c[o + 1] = uint8_t(v >> 8); };
    auto get16 = [&](size_t o) { return uint16_t(c[o] | (c[o + 1] << 8)); };
    if (gtMode) {
        c[0x7A] |= 0x40;
        c[0x79] = src[0x3A];
    }
    // Mapping 0x80092BB4: { config offset, spec offset } (u16 copies).
    static constexpr std::array<ByteMap, 27> kSpecMap = {{
        {0x04, 0x04}, {0x06, 0x06}, {0x08, 0x08}, {0x0A, 0x0A}, {0x0C, 0x10}, {0x0E, 0x1E}, {0x10, 0x28}, {0x12, 0x2A}, {0x14, 0x26},
        {0x16, 0x30}, {0x18, 0x32}, {0x1A, 0x0C}, {0x1C, 0x0E}, {0x1E, 0x12}, {0x20, 0x14}, {0x22, 0x16}, {0x24, 0x18}, {0x26, 0x1A},
        {0x28, 0x1C}, {0x2A, 0x20}, {0x2C, 0x22}, {0x2E, 0x24}, {0x30, 0x2E}, {0x32, 0x2C}, {0x34, 0x34}, {0x36, 0x36}, {0x38, 0x38},
    }};
    for (const auto& [dst, s] : kSpecMap) put16(dst, U16(src, s));

    const auto row = [&](size_t table, size_t cfgOffset) { return tables.Row(table, get16(cfgOffset)); }; // 0x80076F2C
    { // gearbox (table 17), mapping 0x800928A0: ratios + final -> +3C..+4D, +21 -> +4E
        const std::span<const uint8_t> r = row(kTableGearbox, kCfgGearbox);
        for (size_t i = 0; i < 9; i++) put16(0x3C + 2 * i, U16(r, 0x0A + 2 * i));
        c[0x4E] = r[0x21];
    }
    { // racing modification (table 5), 0x800928D8: downforce
        const std::span<const uint8_t> r = row(kTableRacingModify, kCfgRacingModify);
        c[0x52] = r[0x12];
        c[0x53] = r[0x15];
    }
    { // turbo kit (table 12), 0x800928E4
        const std::span<const uint8_t> r = row(kTableTurboKit, kCfgTurbo);
        for (size_t i = 0; i < 6; i++) c[0x54 + i] = r[0x0A + i];
    }
    { // suspension (table 18), 0x80092900
        const std::span<const uint8_t> r = row(kTableSuspension, kCfgSuspension);
        static constexpr std::array<ByteMap, 18> kSusp = {{
            {0x5A, 0x0B}, {0x5B, 0x0E}, {0x5C, 0x15}, {0x5D, 0x18}, {0x60, 0x1F}, {0x61, 0x22}, {0x62, 0x23}, {0x63, 0x24}, {0x64, 0x2A},
            {0x65, 0x2D}, {0x66, 0x31}, {0x67, 0x34}, {0x68, 0x38}, {0x69, 0x3B}, {0x6A, 0x3F}, {0x6B, 0x42}, {0x6C, 0x46}, {0x6D, 0x4A},
        }};
        for (const auto& [dst, s] : kSusp) c[dst] = r[s];
    }
    c[0x5E] = 128; // toe, set before the brake controller mapping
    c[0x5F] = 128;
    { // brake controller (table 1), 0x800928CC
        const std::span<const uint8_t> r = row(kTableBrakeController, kCfgBrakeController);
        c[0x50] = r[0x0C];
        c[0x51] = r[0x0C];
    }
    { // LSD (table 21), 0x8009294C
        const std::span<const uint8_t> r = row(kTableLsd, kCfgLsd);
        c[0x6E] = r[0x0D];
        c[0x6F] = r[0x17];
        c[0x70] = r[0x10];
        c[0x71] = r[0x1A];
        c[0x72] = r[0x13];
        c[0x73] = r[0x1D];
    }
    c[0x4F] = row(kTableDrivetrain, kCfgDrivetrain)[0x07]; // 0x80092888
    c[0x74] = row(kTableAsm, kCfgAsm)[0x0B];               // 0x80092890
    c[0x75] = row(kTableTcs, kCfgTcs)[0x0C];               // 0x80092898
    { // table 29 row (config +38): word00 = u32 +0 | (byte +7 & 0x1F) << 8
        const std::span<const uint8_t> r = row(kCarProfileTable, kCfgProfile);
        const uint32_t w = U32(r, 0) | (uint32_t(r[7] & 0x1F) << 8);
        std::memcpy(c.data(), &w, 4);
    }
    CarConfig out;
    std::memcpy(&out, c.data(), sizeof(out));
    return out;
}

std::optional<CarConfig> CatalogueCarConfig(const CarParamTables& tables, uint32_t carId) {
    const std::optional<size_t> row = FindCatalogueRow(tables, carId);
    if (!row) return std::nullopt;
    return ConfigFromCarSpec(tables, tables.Row(kCarCatalogueTable, *row), true);
}

CarConfig OpponentCarConfig(const CarParamTables& tables, std::span<const uint8_t> src, bool gtMode) {
    if (src.size() != kOpponentRowSize) throw std::runtime_error("opponent: row size");
    CarConfig cfg = ConfigFromCarSpec(tables, src, gtMode);
    std::array<uint8_t, sizeof(CarConfig)> c{};
    std::memcpy(c.data(), &cfg, sizeof(cfg));
    // Mapping 0x80092C24.
    c[0x4C] = src[0x3A];
    c[0x4D] = src[0x3B];
    static constexpr std::array<ByteMap, 29> kOpp = {{
        {0x4E, 0x3C}, {0x6E, 0x3D}, {0x70, 0x3E}, {0x72, 0x3F}, {0x6F, 0x40}, {0x71, 0x41}, {0x73, 0x42}, {0x52, 0x43}, {0x53, 0x44}, {0x5A, 0x45},
        {0x5B, 0x46}, {0x5E, 0x47}, {0x5F, 0x48}, {0x5C, 0x49}, {0x5D, 0x4A}, {0x60, 0x4B}, {0x61, 0x4C}, {0x64, 0x4D}, {0x65, 0x4E}, {0x66, 0x4F},
        {0x67, 0x50}, {0x68, 0x51}, {0x69, 0x52}, {0x6A, 0x53}, {0x6B, 0x54}, {0x6C, 0x55}, {0x6D, 0x56}, {0x74, 0x57}, {0x75, 0x58},
    }};
    for (const auto& [dst, s] : kOpp) c[dst] = src[s];
    c[0x3A] = src[0x5C];
    c[0x3B] = src[0x5D];
    c[0x7A] |= 1;
    std::memcpy(&cfg, c.data(), sizeof(cfg));
    return cfg;
}

// ---------------------------------------------------------------- race data

size_t RaceEvent::OpponentCount() const {
    size_t n = 0;
    while (n < slots.size() && slots[n] != 0) n++;
    return n;
}

char RaceEvent::SlotPaint(uint32_t slot) { return kCharset6[(slot >> 26) % (sizeof(kCharset6) - 1)]; }

GtModeRaceData::GtModeRaceData(std::vector<uint8_t> gtdt) : file_(std::move(gtdt)) {
    if (file_.EntryCount() < 2 * (kCarListTable + 1)) throw std::runtime_error("race data: too few GTDT entries");
    if (file_.Entry(kRaceEventTable).size % kRaceEventRowSize != 0 || file_.Entry(kOpponentTable).size % kOpponentRowSize != 0 ||
        file_.Entry(kCarListTable).size % kCarListRowSize != 0)
        throw std::runtime_error("race data: tables are not whole rows");
    const std::span<const uint8_t> pool = file_.Bytes(file_.EntryCount() / 2 + kRaceEventTable);
    const size_t count = U16(pool, 0);
    size_t at = 2;
    for (size_t i = 0; i < count; i++) { // 0x80076C74's walk: { u8 length; char name[length]; u8 0 }
        if (at >= pool.size() || at + 1 + pool[at] + 1 > pool.size()) throw std::runtime_error("race data: name pool out of bounds");
        const size_t length = pool[at];
        names_.emplace_back(reinterpret_cast<const char*>(pool.data() + at + 1), length);
        at += length + 2;
    }
    for (size_t row = 0; row < EventCount(); row++) {
        const std::span<const uint8_t> r = file_.Bytes(kRaceEventTable).subspan(row * kRaceEventRowSize, kRaceEventRowSize);
        if (U16(r, 0) >= names_.size() || U16(r, 2) >= names_.size() || U16(r, 0x94) >= names_.size())
            throw std::runtime_error("race data: event row " + std::to_string(row) + " names outside the pool");
        for (size_t s = 0; s < kEventSlotCount; s++) {
            const uint32_t n = RaceEvent::SlotOpponent(U32(r, 4 + 4 * s));
            if (n > OpponentCount()) throw std::runtime_error("race data: event row " + std::to_string(row) + " slot beyond the opponent table");
        }
        if (r[0x76] > CarListCount()) throw std::runtime_error("race data: event row " + std::to_string(row) + " car list beyond table 2");
    }
}

GtModeRaceData GtModeRaceData::Load(const GtfsVolume& vol, const std::string& path) { return GtModeRaceData(vol.Read(path)); }

size_t GtModeRaceData::EventCount() const { return file_.Entry(kRaceEventTable).size / kRaceEventRowSize; }
size_t GtModeRaceData::OpponentCount() const { return file_.Entry(kOpponentTable).size / kOpponentRowSize; }
size_t GtModeRaceData::CarListCount() const { return file_.Entry(kCarListTable).size / kCarListRowSize; }

RaceEvent GtModeRaceData::EventAt(size_t row) const {
    if (row >= EventCount()) throw std::out_of_range("race data: event row");
    return ParseRaceEvent(file_.Bytes(kRaceEventTable).subspan(row * kRaceEventRowSize, kRaceEventRowSize), names_, row);
}

RaceEvent ParseRaceEvent(std::span<const uint8_t> r, const std::vector<std::string>& names_, size_t row) {
    if (r.size() != kRaceEventRowSize) throw std::runtime_error("race event: row size");
    if (U16(r, 0) >= names_.size() || U16(r, 2) >= names_.size() || U16(r, 0x94) >= names_.size())
        throw std::runtime_error("race event: row " + std::to_string(row) + " names outside the pool");
    RaceEvent e;
    e.row = row;
    e.nameIndex = U16(r, 0);
    e.courseIndex = U16(r, 2);
    e.name = names_[e.nameIndex];
    e.course = names_[e.courseIndex];
    for (size_t s = 0; s < kEventSlotCount; s++) e.slots[s] = U32(r, 4 + 4 * s);
    std::memcpy(e.settings.data(), r.data() + 0x44, e.settings.size());
    for (size_t i = 0; i < e.prize.size(); i++) e.prize[i] = uint32_t(U16(r, 0x78 + 2 * i)) * 100;
    for (size_t i = 0; i < e.prizeCars.size(); i++) e.prizeCars[i] = U32(r, 0x84 + 4 * i);
    e.tagNameIndex = U16(r, 0x94);
    e.tag = names_[e.tagNameIndex];
    e.powerLimit = U16(r, 0x96);
    e.bonus = uint32_t(U16(r, 0x98)) * 100;
    e.aspiration = r[0x9A];
    e.byte9B = r[0x9B];
    e.bytes.assign(r.begin(), r.end());
    return e;
}

int32_t GtModeRaceData::FindEvent(const std::string& name) const {
    int32_t index = -1;
    for (size_t i = 0; i < names_.size(); i++)
        if (names_[i] == name) {
            index = int32_t(i);
            break;
        }
    if (index < 0) return -1;
    // 0x800781E0's search (licence.md section 3): the rows are sorted by their name index.
    const std::span<const uint8_t> table = file_.Bytes(kRaceEventTable);
    size_t lo = 0, hi = EventCount();
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const int32_t id = U16(table, mid * kRaceEventRowSize);
        if (id == index) return int32_t(mid);
        if (id < index) lo = mid + 1;
        else hi = mid;
    }
    return -1;
}

std::span<const uint8_t> GtModeRaceData::OpponentBytes(uint32_t number) const {
    if (number == 0 || number > OpponentCount()) throw std::out_of_range("race data: opponent number");
    return file_.Bytes(kOpponentTable).subspan((number - 1) * kOpponentRowSize, kOpponentRowSize);
}

OpponentCarRow GtModeRaceData::Opponent(uint32_t number) const {
    OpponentCarRow o;
    std::memcpy(&o, OpponentBytes(number).data(), sizeof(o));
    return o;
}

std::vector<uint32_t> GtModeRaceData::CarList(uint8_t index) const {
    std::vector<uint32_t> out;
    if (index == 0) return out;
    if (index > CarListCount()) throw std::out_of_range("race data: car list index");
    const std::span<const uint8_t> r = file_.Bytes(kCarListTable).subspan((index - 1) * kCarListRowSize, kCarListRowSize);
    for (size_t i = 0; i < kCarListRowSize / 4; i++) { // 0x8001928C copies until the first 0 (at most 32)
        const uint32_t id = U32(r, 4 * i);
        if (id == 0) break;
        out.push_back(id);
    }
    return out;
}

// ---------------------------------------------------------------- unistrdb

std::vector<std::u16string> ParseUniStrDb(std::span<const uint8_t> d) {
    if (d.size() < 10 || std::memcmp(d.data() + 4, "WSDB", 4) != 0) throw std::runtime_error("unistrdb: bad magic");
    if (U32(d, 0) != d.size()) throw std::runtime_error("unistrdb: size word does not match the file");
    const size_t count = U16(d, 8);
    std::vector<std::u16string> out;
    size_t at = 10;
    for (size_t i = 0; i < count; i++) {
        const size_t length = U16(d, at);
        if (at + 2 + 2 * length + 2 > d.size() || U16(d, at + 2 + 2 * length) != 0) throw std::runtime_error("unistrdb: string out of bounds");
        std::u16string s;
        for (size_t k = 0; k < length; k++) s.push_back(char16_t(U16(d, at + 2 + 2 * k)));
        out.push_back(std::move(s));
        at += 2 + 2 * length + 2;
    }
    if (at != d.size()) throw std::runtime_error("unistrdb: trailing bytes");
    return out;
}

std::string Utf16ToUtf8(const std::u16string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        uint32_t c = s[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t(s[i + 1]) - 0xDC00);
            i++;
        }
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) {
            out.push_back(char(0xC0 | (c >> 6)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else if (c < 0x10000) {
            out.push_back(char(0xE0 | (c >> 12)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        } else {
            out.push_back(char(0xF0 | (c >> 18)));
            out.push_back(char(0x80 | ((c >> 12) & 0x3F)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(char(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// ---------------------------------------------------------------- colours

CarColorNames::CarColorNames(std::vector<uint8_t> carcolor, std::vector<uint8_t> names) : carcolor_(std::move(carcolor)), names_(std::move(names)) {
    const std::span<const uint8_t> c(carcolor_), n(names_);
    if (c.size() < 10 || std::memcmp(c.data(), "CCOL00", 6) != 0) throw std::runtime_error("carcolor: bad magic");
    const size_t cars = (U16(c, 8) - 8) / 2;
    for (size_t i = 0; i < cars; i++) carOffsets_.push_back(U16(c, 8 + 2 * i));
    const size_t count = U16(n, 0) / 2;
    for (size_t i = 0; i < count; i++) {
        nameOffsets_.push_back(U16(n, 2 * i));
        if (nameOffsets_.back() >= n.size()) throw std::runtime_error("colour names: offset out of bounds");
    }
}

CarColorNames CarColorNames::Load(const GtfsVolume& vol, bool latin) {
    return CarColorNames(vol.Read(".carcolor"), vol.Read(latin ? ".cclatain" : ".ccjapanese"));
}

std::vector<uint16_t> CarColorNames::NameIndices(size_t carInfoIndex, size_t paintCount) const {
    const size_t at = carOffsets_.at(carInfoIndex);
    const size_t end = carInfoIndex + 1 < carOffsets_.size() ? carOffsets_[carInfoIndex + 1] : carcolor_.size();
    if (at + 2 * paintCount > end) throw std::runtime_error("carcolor: entry shorter than the paint count");
    std::vector<uint16_t> out;
    for (size_t i = 0; i < paintCount; i++) out.push_back(U16(carcolor_, at + 2 * i));
    return out;
}

std::string CarColorNames::Name(uint16_t index) const {
    const size_t at = nameOffsets_.at(index);
    size_t e = at;
    while (e < names_.size() && names_[e] != 0) e++;
    return std::string(reinterpret_cast<const char*>(names_.data() + at), e - at);
}

// ---------------------------------------------------------------- gtmenu containers

MenuPackIndex ParseMenuPackIndex(std::span<const uint8_t> idx) {
    const size_t count = U32(idx, 0);
    if (4 + 4 * (count + 1) != idx.size()) throw std::runtime_error("menu index: size is not 4 + 4 * (count + 1)");
    MenuPackIndex m;
    for (size_t i = 0; i <= count; i++) m.offsets.push_back(U32(idx, 4 + 4 * i));
    for (size_t i = 0; i < count; i++)
        if (m.offsets[i + 1] < m.offsets[i]) throw std::runtime_error("menu index: offsets not ascending");
    return m;
}

std::vector<uint8_t> MenuPackEntry(std::span<const uint8_t> dat, const MenuPackIndex& index, size_t i, bool gzip) {
    const size_t start = index.Start(i), length = index.ReadLength(i);
    if (start + length > dat.size()) throw std::runtime_error("menu pack: entry beyond the file");
    const std::span<const uint8_t> bytes = dat.subspan(start, length);
    if (gzip) return Gunzip(bytes);
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

SoloData ParseSoloData(std::span<const uint8_t> d) {
    SoloData s;
    const size_t n = U32(d, 0);
    for (size_t i = 0; i < n; i++) s.pages.push_back(U16(d, 4 + 2 * i));
    size_t at = 4 + 2 * ((n + 1) & ~size_t(1)); // 0x80020E4C: ((n + 1) & ~1) * 2 + 4
    const size_t m = U32(d, at);
    at += 4;
    if (at + 8 * m != d.size()) throw std::runtime_error("solodata: size mismatch");
    for (size_t i = 0; i < m; i++) s.cars.emplace_back(U32(d, at + 8 * i), U32(d, at + 8 * i + 4));
    for (size_t i = 1; i < m; i++)
        if (s.cars[i].first <= s.cars[i - 1].first) throw std::runtime_error("solodata: car ids not sorted");
    return s;
}

std::optional<uint32_t> SoloData::ValueOf(uint32_t carId) const { // 0x80020F54
    const auto it = std::lower_bound(cars.begin(), cars.end(), carId, [](const std::pair<uint32_t, uint32_t>& e, uint32_t id) { return e.first < id; });
    if (it == cars.end() || it->first != carId) return std::nullopt;
    return it->second;
}

} // namespace gt2
