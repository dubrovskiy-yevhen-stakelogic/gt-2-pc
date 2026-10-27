#include "game/physics/carparams_specdb.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace gt4::game::carparams {

const char* const kSpecTableNames[43] = {
    "GENERIC_CAR", "BRAKE", "BRAKECONTROLLER", "SUSPENSION", "ASCC", "TCSC", "CHASSIS", "RACINGMODIFY", "LIGHTWEIGHT",
    "STEER", "DRIVETRAIN", "GEAR", "ENGINE", "NATUNE", "TURBINEKIT", "PORTPOLISH", "ENGINEBALANCE", "DISPLACEMENT",
    "COMPUTER", "INTERCOOLER", "MUFFLER", "CLUTCH", "FLYWHEEL", "PROPELLERSHAFT", "LSD", "FRONTTIRE", "REARTIRE", "NOS",
    "SUPERCHARGER", "WHEEL", "WING", "TIRESIZE", "TIRECOMPOUND", "TIREFORCEVOL", "COURSE", "RACE", "DEFAULT_PARTS",
    "DEFAULT_PARAM", "ENEMY_CARS", "CAR_NAME_", "COURSE_NAME_", "CAR_VARIATION_", "VARIATION",
};

bool SpecDbSource::Load(const std::string& dir, const std::string& locale, std::string* error) {
    // Explicit load replaces the binding, even on failure. Never ignore the
    // requested locale behind a previously installed shared provider.
    localized_.reset();
    namespace fs = std::filesystem;
    int loaded = 0;
    for (uint32_t i = 0; i < 43; ++i) {
        std::string name = kSpecTableNames[i];
        if (i >= 39) name += locale;   // DAT_00623570 = {39, 40, 41, 42}
        const fs::path p = fs::path(dir) / (name + ".dbt");
        std::ifstream in(p, std::ios::binary);
        if (!in) continue;
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        auto t = std::make_unique<gt4::specdb::Table>();
        if (!t->Load(std::move(bytes), error)) {
            *error = p.string() + ": " + *error;
            return false;
        }
        tables_[i] = std::move(t);
        ++loaded;
    }
    if (!loaded) {
        *error = "no SpecDB tables in " + dir;
        return false;
    }
    return true;
}

int32_t SpecDbSource::Search(uint32_t table, uint32_t id) const {
    const gt4::specdb::Table* t = this->table(table);
    if (!t) return -1;
    const int32_t count = static_cast<int32_t>(t->rowCount());
    auto idAt = [&](int32_t i) { return (i >= 0 && i < count) ? t->rowId(static_cast<uint32_t>(i)) : 0xffffffffu; };
    int32_t lo = -1, hi = count;
    if (hi < 1) return -1;
    int32_t mid = hi - 1;
    for (;;) {
        mid >>= 1;
        const uint32_t v = idAt(mid);
        if (v == id) return mid;
        int32_t next = mid;
        if (id <= v) {
            next = lo;
            hi = mid;
        }
        lo = next;
        if (lo + 1 == hi) return -1;
        mid = lo + hi;
    }
}

int32_t SpecDbSource::FirstIndex(uint32_t table, uint32_t id) const {
    const gt4::specdb::Table* t = this->table(table);
    int32_t i1 = Search(table, id), i2 = i1;
    if (i1 != -1) {
        const int32_t count = static_cast<int32_t>(t->rowCount());
        uint32_t v;
        do {
            i1 = i2;
            i2 = i1 - 1;
            v = (i2 >= 0 && i2 < count) ? t->rowId(static_cast<uint32_t>(i2)) : 0xffffffffu;
        } while (id == v && i1 != 0);
    }
    return i1;
}

int32_t SpecDbSource::Count(uint64_t key) const {
    const uint32_t table = static_cast<uint32_t>(key >> 32), id = static_cast<uint32_t>(key);
    const gt4::specdb::Table* t = this->table(table);
    if (!t) return -1;
    int32_t i = FirstIndex(table, id);
    if (i == -1) return -1;
    const int32_t count = static_cast<int32_t>(t->rowCount());
    int32_t n = 1;
    for (;;) {
        const int32_t next = i + 1;
        const uint32_t v = (next >= 0 && next < count) ? t->rowId(static_cast<uint32_t>(next)) : 0xffffffffu;
        if (id != v || count <= i) return n;
        ++n;
        i = next;
    }
}

uint32_t SpecDbSource::RowAt(uint32_t table, uint32_t index, uint8_t* out) const {
    const gt4::specdb::Table* t = this->table(table);
    if (!t || index >= t->rowCount()) return 0;
    std::string error;
    if (!t->Row(index, &row_, &error)) return 0;
    std::memcpy(out, row_.data(), t->rowSize());
    return t->rowSize();
}

bool SpecDbSource::Exists(uint64_t key) const {
    const uint32_t table = static_cast<uint32_t>(key >> 32);
    return table < 43 && tables_[table] && Search(table, static_cast<uint32_t>(key)) != -1;
}

uint32_t SpecDbSource::Fetch(uint64_t key, uint8_t* out) const {
    const uint32_t table = static_cast<uint32_t>(key >> 32);
    const int32_t i = Search(table, static_cast<uint32_t>(key));
    if (i < 0) return 0;
    return RowAt(table, static_cast<uint32_t>(i), out);
}

} // namespace gt4::game::carparams
