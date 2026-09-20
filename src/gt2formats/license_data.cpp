#include "gt2formats/license_data.h"

#include <cctype>
#include <cstring>
#include <stdexcept>

#include "gt2formats/gtmode_tables.h"
#include "gt2vfs/gtfs.h"

namespace gt2 {

namespace {

uint16_t U16(std::span<const uint8_t> b, size_t o) { return uint16_t(b[o] | (b[o + 1] << 8)); }
uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(b[o]) | (uint32_t(b[o + 1]) << 8) | (uint32_t(b[o + 2]) << 16) | (uint32_t(b[o + 3]) << 24); }

} // namespace

uint32_t LicenseTest::MedalTime(uint32_t medal) const { // 0x8003D7B8
    const size_t at = 0x26 + size_t(medal) * 2;
    if (at + 1 >= settings.size()) throw std::out_of_range("licence: medal index");
    const uint32_t secondsField = settings[at]; // minutes * 100 + seconds
    return (secondsField / 100) * 60000 + (secondsField % 100) * 1000 + uint32_t(settings[at + 1]) * 10;
}

LicenseData::LicenseData(std::vector<uint8_t> gtdt) : file_(gtdt), tables_(gtdt) {
    // 0x40 directory entries: tables 0..31 and their extras 32..63; the name pool is the extra of table 30.
    if (file_.EntryCount() < 2 * (kLicenseCarTable + 1)) throw std::runtime_error("licence data: too few GTDT entries");
    if (file_.Entry(kLicenseTestTable).size % kLicenseTestRowSize != 0 || file_.Entry(kLicenseCarTable).size % kLicenseCarRowSize != 0)
        throw std::runtime_error("licence data: table 30 / 31 sizes are not whole rows");
    const std::span<const uint8_t> pool = file_.Bytes(file_.EntryCount() / 2 + kLicenseTestTable);
    if (pool.size() < 2) throw std::runtime_error("licence data: no name pool");
    const size_t count = U16(pool, 0);
    size_t at = 2;
    for (size_t i = 0; i < count; i++) { // 0x80076C74's walk: { u8 length; char name[length]; u8 0 }
        if (at >= pool.size() || at + 1 + pool[at] + 1 > pool.size()) throw std::runtime_error("licence data: name pool out of bounds");
        const size_t length = pool[at];
        names_.emplace_back(reinterpret_cast<const char*>(pool.data() + at + 1), length);
        at += length + 2;
    }
    for (size_t row = 0; row < TestCount(); row++) {
        const std::span<const uint8_t> r = file_.Bytes(kLicenseTestTable).subspan(row * kLicenseTestRowSize, kLicenseTestRowSize);
        if (U16(r, 0) >= names_.size() || U16(r, 2) >= names_.size()) throw std::runtime_error("licence data: test row " + std::to_string(row) + " names outside the pool");
    }
}

LicenseData LicenseData::Load(const GtfsVolume& vol, const std::string& path) { return LicenseData(vol.Read(path)); }

int32_t LicenseData::NameIndex(const std::string& name) const { // 0x80076C74
    for (size_t i = 0; i < names_.size(); i++)
        if (names_[i] == name) return int32_t(i);
    return -1;
}

size_t LicenseData::TestCount() const { return file_.Entry(kLicenseTestTable).size / kLicenseTestRowSize; }

int32_t LicenseData::FindTestRow(const std::string& name) const { // 0x800781E0
    const int32_t index = NameIndex(name);
    if (index < 0) return -1;
    const std::span<const uint8_t> table = file_.Bytes(kLicenseTestTable);
    int32_t low = 0, high = int32_t(TestCount());
    // The original's loop: mid = (low + high) >> 1; equal -> mid; low == high -> -1; row < index -> low = mid, else
    // high = mid. For a name missing from a table it never reaches low == high (low = mid repeats); the name pool and
    // the table of the file hold the same tests, so that does not happen with the disc's data - here it is an error.
    for (int guard = 0; guard < 64; guard++) {
        const int32_t mid = (low + high) >> 1;
        const int32_t id = U16(table, size_t(mid) * kLicenseTestRowSize);
        if (id == index) return mid;
        if (low == high) return -1;
        if (id < index) low = mid;
        else high = mid;
    }
    throw std::runtime_error("licence data: " + name + " is in the name pool but not in the test table");
}

LicenseTest LicenseData::TestAt(size_t row) const {
    if (row >= TestCount()) throw std::out_of_range("licence data: test row");
    const std::span<const uint8_t> r = file_.Bytes(kLicenseTestTable).subspan(row * kLicenseTestRowSize, kLicenseTestRowSize);
    LicenseTest t;
    t.row = row;
    t.nameIndex = U16(r, 0);
    t.courseIndex = U16(r, 2);
    t.name = names_[t.nameIndex];
    t.course = names_[t.courseIndex];
    for (size_t s = 0; s < kLicenseCarSlots; s++) t.cars[s] = {r[4 + s * 4], r[5 + s * 4], r[6 + s * 4], r[7 + s * 4], U32(r, 4 + s * 4)};
    std::memcpy(t.settings.data(), r.data() + kLicenseSettingsOffset, kLicenseSettingsSize);
    t.word84 = U32(r, 0x84);
    t.tagNameIndex = U16(r, 0x94);
    t.tag = t.tagNameIndex < names_.size() ? names_[t.tagNameIndex] : std::string();
    t.bytes.assign(r.begin(), r.end());
    return t;
}

LicenseTest LicenseData::Test(const std::string& name) const {
    const int32_t row = FindTestRow(name);
    if (row < 0) throw std::runtime_error("licence data: no test " + name);
    return TestAt(size_t(row));
}

std::span<const uint8_t> LicenseData::CarRow(uint8_t carNumber) const {
    const std::span<const uint8_t> table = file_.Bytes(kLicenseCarTable);
    for (size_t row = 0; row < table.size() / kLicenseCarRowSize; row++) {
        const std::span<const uint8_t> r = table.subspan(row * kLicenseCarRowSize, kLicenseCarRowSize);
        if (U16(r, 0x5E) == carNumber) return r;
    }
    throw std::runtime_error("licence data: no licence car " + std::to_string(carNumber));
}

uint32_t LicenseData::CarId(uint8_t carNumber) const { return U32(CarRow(carNumber), 0); }


size_t LicenseData::CarCount() const { return file_.Entry(kLicenseCarTable).size / kLicenseCarRowSize; }

std::span<const uint8_t> LicenseData::CarRowAt(uint32_t number) const { // 0x800768C0 -> 0x80077D5C(object, 31, number - 1)
    if (number == 0 || number > CarCount()) throw std::runtime_error("licence data: licence car number " + std::to_string(number) + " outside table 31");
    return file_.Bytes(kLicenseCarTable).subspan(size_t(number - 1) * kLicenseCarRowSize, kLicenseCarRowSize);
}

LicenseRaceCar LicenseData::RaceCar(const LicenseTest& test) const { // 0x80010078 / 0x8004C7A0, car slot 0 part
    static constexpr char kPaintCharset[] = "-0123456789abcdefghijklmnopqrstuvwxyz"; // EXE 0x80091620 (the codes of the file)
    const LicenseCarSlot& slot = test.cars[0];
    const std::span<const uint8_t> row = CarRowAt(slot.Number());
    LicenseRaceCar car;
    car.carId = U32(row, 0);
    car.paintCode = slot.PaintCode();
    if (car.paintCode >= sizeof(kPaintCharset) - 1) throw std::runtime_error("licence data: paint code beyond the character set");
    car.paint = kPaintCharset[car.paintCode];
    car.config = ConfigFromCarSpec(tables_, row, false); // 0x80076FC0 with 0x80092878 = 3 (the licence database)
    if (car.config.racingModify != 0) { // lhu + blez: any non-zero row -> the body of the modification
        const std::span<const uint8_t> rm = tables_.Row(kTableRacingModify, car.config.racingModify);
        car.carId = U32(rm, 8);
    }
    return car;
}
std::string LicenseTestName(const std::string& label) {
    if (label.size() == 5 && label[0] == 'L') return label;
    const size_t dash = label.find('-');
    if (dash == std::string::npos || dash + 1 >= label.size()) return {};
    std::string letters = label.substr(0, dash);
    for (char& c : letters) c = char(std::toupper(static_cast<unsigned char>(c)));
    std::string prefix;
    if (letters == "B") prefix = "LJB";
    else if (letters == "A") prefix = "LJA";
    else if (letters == "IC") prefix = "LIC";
    else if (letters == "IB") prefix = "LIB";
    else if (letters == "IA") prefix = "LIA";
    else if (letters == "S") prefix = "LIS";
    else return {};
    int number = 0;
    for (size_t i = dash + 1; i < label.size(); i++) {
        if (!std::isdigit(static_cast<unsigned char>(label[i]))) return {};
        number = number * 10 + (label[i] - '0');
    }
    if (number < 1 || number > 10) return {};
    const char digits[3] = {char('0' + (number - 1) / 10), char('0' + (number - 1) % 10), 0};
    return prefix + digits;
}

} // namespace gt2
