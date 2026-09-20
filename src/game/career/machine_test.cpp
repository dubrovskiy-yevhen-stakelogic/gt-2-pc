// The GT-mode machine test: setup and records (see machine_test.h).
#include "game/career/machine_test.h"

#include <cstring>
#include <stdexcept>

#include "game/career/championship.h"
#include "game/career/events.h"
#include "game/career/garage.h"
#include "game/career/tuning.h"
#include "gt2formats/car_info.h"
#include "gt2formats/gtmode_tables.h"

namespace gt2::career {

namespace {

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void PutLe32(uint8_t* p, uint32_t v) {
    for (int k = 0; k < 4; k++) p[k] = uint8_t(v >> (8 * k));
}
void PutLe16(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
}

// strcpy into a byte buffer (the bytes of the string and its NUL).
void CopyString(uint8_t* at, const std::string& s) {
    std::memcpy(at, s.c_str(), s.size() + 1);
}

// 0x80060AE8(car id): the .carinfoa record's name (with its 0x7F padding) of the binary search 0x80060A24 (record 0 when absent).
const std::string& CarName(const CareerData& d, uint32_t carId) {
    const int32_t index = d.cars.IndexOf(carId);
    return d.cars.At(index < 0 ? 0 : size_t(index)).rawName;
}

} // namespace

int MachineTestIndex(int subMode) { return subMode >= kMachineTest400 && subMode <= kMachineTestMaxSpeed ? subMode - kMachineTest400 : -1; }

MachineTestRecord* MachineTestRecordOf(CareerState& s, int subMode) {
    const int i = MachineTestIndex(subMode);
    return i < 0 ? nullptr : &s.machineTests[i];
}
const MachineTestRecord* MachineTestRecordOf(const CareerState& s, int subMode) {
    const int i = MachineTestIndex(subMode);
    return i < 0 ? nullptr : &s.machineTests[i];
}

uint32_t MachineTestEntryCar(const MachineTestRecord& r, int k) { return Le32(MachineTestEntry(r, k)); }
uint32_t MachineTestEntryValue(const MachineTestRecord& r, int k) { return Le32(MachineTestEntry(r, k) + 4); }
std::string MachineTestEntryName(const MachineTestRecord& r, int k) {
    const uint8_t* e = MachineTestEntry(r, k) + 8;
    std::string name;
    for (size_t i = 0; i < 12 && e[i] != 0; i++) name.push_back(char(e[i]));
    return name;
}

int32_t InsertMachineTestRecord(MachineTestRecord& record, const uint8_t* entry, bool higher) { // EXE 0x8005E0D0
    uint8_t* r = record.bytes;
    auto at = [&](int k) { return r + 4 + k * int(kMachineTestEntrySize); };
    auto better = [&](uint32_t value, uint32_t than) { return higher ? than < value : value < than; };
    if (r[0] > 8) r[0] = 0;
    const uint32_t car = Le32(entry), value = Le32(entry + 4);
    for (int k = 0; k < int(int8_t(r[0])); k++) { // an entry of the same car: replaced only by a better value
        if (Le32(at(k)) != car) continue;
        if (!better(value, Le32(at(k) + 4))) return -1;
        for (int j = k + 1; j < int(int8_t(r[0])); j++) std::memmove(at(j - 1), at(j), kMachineTestEntrySize);
        PutLe32(at(int(int8_t(r[0])) - 1) + 4, 0xFFFFFFFFu);
        r[0] = uint8_t(r[0] - 1);
        break;
    }
    int k = 0;
    const int count = int(int8_t(r[0]));
    while (k < count && !better(value, Le32(at(k) + 4))) k++;
    if (k == count && k >= kMachineTestEntries) return -1;
    for (int j = kMachineTestEntries - 2; j >= k; j--) std::memmove(at(j + 1), at(j), kMachineTestEntrySize);
    std::memcpy(at(k), entry, kMachineTestEntrySize);
    if (int8_t(r[0]) < kMachineTestEntries) r[0] = uint8_t(r[0] + 1);
    return k;
}

MachineTestOutcome WriteMachineTestRecord(CareerState& s, int subMode, uint32_t carId, uint32_t time, uint16_t maxSpeed) { // ovl0 0x80050D78
    MachineTestOutcome o;
    o.time = time;
    o.maxSpeed = maxSpeed;
    MachineTestRecord* record = MachineTestRecordOf(s, subMode);
    if (!record) return o; // W + 2 = -1 (the switch falls through)
    uint8_t entry[kMachineTestEntrySize] = {};
    PutLe32(entry, carId);
    const bool speed = subMode == kMachineTestMaxSpeed;
    PutLe32(entry + 4, speed ? uint32_t(maxSpeed) : time);
    // + 8: the name strcpy'd from ovl0 0x8005A980 (""): its NUL, the rest the stack's (the entry's other 11 bytes are copied by
    // the insert as they are). The original's stack there holds what earlier calls left; the entry the insert keeps carries it.
    o.rank = int16_t(InsertMachineTestRecord(*record, entry, speed));
    return o;
}

void StoreMachineTestName(MachineTestRecord& record, int32_t index, const std::string& name) { // EXE 0x8005E03C
    if (uint32_t(index) >= uint32_t(kMachineTestEntries)) return;
    CopyString(record.bytes + 0x0C + index * int(kMachineTestEntrySize), name);
}

std::string MachineTestCarNames::NameOf(uint32_t carId) const { // EXE 0x80074AE4
    if (bytes.size() < 2) return {};
    const int16_t count = int16_t(uint16_t(bytes[0] | bytes[1] << 8));
    for (int k = 0; k < count; k++) {
        const size_t e = 4 + size_t(k) * 0x48;
        if (e + 8 > bytes.size()) break;
        if (Le32(bytes.data() + e) != carId) continue;
        std::string name;
        for (size_t i = e + 4; i < bytes.size() && bytes[i] != 0; i++) name.push_back(char(bytes[i]));
        return name;
    }
    return {};
}

MachineTestRace PrepareMachineTest(CareerState& s, const CareerData& d, const EventMenuData& menu, const EventInfoTable& infos,
                                   const CourseInfoTable& courseInfo, const std::string& name, TuneSheet& sheet, uint8_t* scratch,
                                   TuneSheet* raceSheet) { // ovl4 0x80012C6C
    MachineTestRace m;
    m.name = name;
    m.subMode = MachineTestMode(menu, name);
    if (m.subMode < 0) throw std::runtime_error(name + " is not a machine test (0x8001861C)");
    const int32_t row = d.race.FindEvent(name); // 0x8007830C
    if (row < 0) throw std::runtime_error("no event " + name + " in carparam/usa_gtmode_race.dat");
    const RaceEvent event = d.race.EventAt(size_t(row));
    GarageBlock& g = s.garage;
    const int16_t index = g.currentCar; // garage + 0x4018
    if (index < 0 || index >= g.count) throw std::runtime_error("the career has no current car (buy one first)");

    std::array<uint8_t, 0x58C>& b = m.raceBlock; // 0x8008CE30(block, 0, 0x58C)
    b.fill(0);
    const uint8_t* career = reinterpret_cast<const uint8_t*>(&s);
    for (size_t k = 0; k < 8; k++) b[k] = career[1 + k]; // lwl / lwr of career + 1 and + 5
    b[0x08] = 2;
    b[0x09] = 1;
    b[0x0A] = uint8_t(m.subMode);
    b[0x0D] = 1;
    b[0x0E] = 0;
    SetRaceEventName(b, event.name);                                   // 0x8005E548(block, string of row + 0)
    SetRaceCourse(b, CourseFileId(event.course), courseInfo);          // 0x8005E5F0 -> 0x80083004 -> 0x8005E590
    CopyString(b.data() + 0x44, event.tag);                            // strcpy(block + 0x44, string of row + 0x94)
    b[0x0B] = 5;
    PutLe16(b.data() + 0x57C, 0xFFFF);
    PutLe16(b.data() + 0x582, 0xFFFF);
    PutLe16(b.data() + 0x584, 0xFFFF);
    b[0x0C] = 0;
    b[0x0F] = 1;
    b[0x5A] = 1;
    PutLe16(b.data() + 0x586, 0);
    b[0x580] = 0;
    PutLe32(b.data() + 0x588, (Le32(b.data() + 0x588) | 1u) & ~6u);
    std::copy(event.settings.begin(), event.settings.end(), m.settings.begin()); // row + 0x44 .. + 0x84 -> 0x801C98A0
    m.course = event.course;
    m.tag = event.tag;

    // 0x80019538(name): the event info's rules bit 0 (0x800188B0: the menu index, 0 when the name is not a menu event).
    const int32_t info = MenuEventIndex(menu, name);
    m.dirt = info >= 0 && size_t(info) < infos.infos.size() && (infos.infos[size_t(info)].rules & 1) != 0;
    PrepareRaceTyres(g, index, m.dirt, sheet, d, BuildScratch{scratch}); // 0x80018004(index, 0, dirt)

    // 0x80011000(block, 0, -1, 3, 0, garage car, 0, index): slot 0 (not cleared: grid -1).
    const GarageCar& car = g.cars[index];
    uint8_t* slot = b.data() + 0x5C;
    slot[0x8E] = 3;
    slot[0x8F] = 0;
    PutLe32(slot, car.modelId);
    PutLe32(slot + 4, car.paint);
    CarConfig config = car.config;
    uint8_t* const cfg = reinterpret_cast<uint8_t*>(&config);
    cfg[0x7A] = uint8_t(cfg[0x7A] | 0x40);
    CopyString(slot + 0x90, CarName(d, Le32(slot)));
    PutLe16(b.data() + 0x582, 0);
    PutLe16(b.data() + 0x584, uint32_t(uint16_t(index)));
    BuildMenuRecord(d.tables, config, m.params); // 0x800771AC(slot + 8, 0x801C98E0 + 0x14FDA)
    std::memcpy(slot + 8, &config, sizeof(config));

    // 0x8001F124(career + 0xC6C0, record of the sub-mode, slot 0 + 0x8C of the garage car)
    const MachineTestRecord& record = *MachineTestRecordOf(s, m.subMode);
    const int count = int(int8_t(record.bytes[0]));
    const size_t size = 4 + size_t(count + 1) * 0x48;
    m.carNames.bytes.assign(size, 0);
    m.carNames.written.assign(size, 0);
    auto put = [&](size_t at, const uint8_t* p, size_t n) {
        std::memcpy(m.carNames.bytes.data() + at, p, n);
        std::memset(m.carNames.written.data() + at, 1, n);
    };
    auto entry = [&](int k, uint32_t carId) {
        uint8_t id[4];
        PutLe32(id, carId);
        put(4 + size_t(k) * 0x48, id, 4);
        const std::string& n = CarName(d, carId);
        put(8 + size_t(k) * 0x48, reinterpret_cast<const uint8_t*>(n.c_str()), n.size() + 1);
    };
    for (int k = 0; k < count; k++) entry(k, MachineTestEntryCar(record, k));
    entry(count, car.modelId);
    uint8_t n16[2];
    PutLe16(n16, uint32_t(count + 1));
    put(0, n16, 2);

    if (raceSheet) { // 0x80011160 / 0x80011184(sheet, car + 0) / 0x800129B8(sheet, car + 8)
        ClearTuneSheet(*raceSheet);
        LoadTuneSheet(*raceSheet, car.carId, d.tables);
        SetTuneConfig(*raceSheet, car.config, d.tables);
    }
    return m;
}

uint32_t MachineTestCourseTitle(int subMode) { // ovl0 0x800587BC
    return subMode == kMachineTest400 ? 0x801EFE0Eu : subMode == kMachineTest1000 ? 0x801EFE18u : 0x801EFE23u;
}

} // namespace gt2::career
