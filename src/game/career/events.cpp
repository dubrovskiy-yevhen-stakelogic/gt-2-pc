#include "game/career/events.h"
#include "game/pc_features.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "gt2formats/car_info.h"

namespace gt2::career {

namespace {

std::string GuestString(const GuestImage& image, uint32_t address) {
    std::string s;
    for (uint32_t a = address;; a++) {
        const char c = char(image.Get<uint8_t>(a));
        if (c == 0) break;
        s.push_back(c);
        if (s.size() > 64) throw std::runtime_error("event menu data: unterminated string");
    }
    return s;
}

std::vector<std::string> GuestStringList(const GuestImage& image, uint32_t address, size_t maxCount) {
    std::vector<std::string> out;
    for (uint32_t a = address; out.size() < maxCount; a += 4) {
        const uint32_t p = image.Get<uint32_t>(a);
        if (p == 0) break;
        out.push_back(GuestString(image, p));
    }
    return out;
}

// The .carinfoa record the original's binary search 0x80060A24 lands on (record 0 when the id is absent).
const CarInfoRecord& CarInfoOf(const CarInfoDirectory& cars, uint32_t carId) {
    const int32_t index = cars.IndexOf(carId);
    return cars.At(index < 0 ? 0 : size_t(index));
}

// 0x80060B70(carId): the car is not hidden in the language.
bool Available(const CarInfoDirectory& cars, uint32_t carId, uint8_t language) { return CarInfoOf(cars, carId).AvailableIn(language); }

// 0x80060C90(char, paints, count): the paint's index (letters compared case-insensitively), 0 when absent.
size_t PaintIndexOf(const CarInfoRecord& r, int32_t paint) {
    auto lower = [](int32_t c) { return uint32_t(c - 0x41) < 0x1A ? c + 0x20 : c; };
    const int32_t wanted = lower(paint);
    const size_t count = r.PaintCount();
    for (size_t i = 0; i < count; i++)
        if (lower(int32_t(int8_t(r.paintIds.at(i)))) == wanted) return i;
    return 0;
}

// 0x800105C8: the saturation of a 15-bit colour, ((max - min) * 32) / max over its three 5-bit channels.
int32_t Saturation(uint16_t colour) {
    const int32_t r = colour & 0x1F, g = (colour >> 5) & 0x1F, b = (colour >> 10) & 0x1F;
    const int32_t hi = std::max(r, std::max(g, b)), lo = std::min(r, std::min(g, b));
    return hi == 0 ? 0 : ((hi - lo) * 0x20) / hi;
}

constexpr char kPaintCharset[] = "-0123456789abcdefghijklmnopqrstuvwxyz"; // EXE 0x80091620 (index = 6-bit code)

char PaintOfCode(uint32_t code) {
    if (code >= sizeof(kPaintCharset) - 1) throw std::logic_error("paint code beyond the character set (EXE 0x80091620 continues; not ported)");
    return kPaintCharset[code];
}

// Working entry of 0x80010A30 (12 bytes on its stack): car id, a byte, paint character, colour, opponent number.
struct PickEntry {
    uint32_t carId = 0;
    uint8_t byte4 = 0;
    char paint = 0;
    uint16_t colour = 0;
    uint16_t opponent = 0;
};

uint32_t OpponentCarId(const GtModeRaceData& race, uint32_t number) { return race.Opponent(number).spec.carId; }

// 0x80010714(entry, file, event, &seed, entries, count): returns the slot's paint code.
uint32_t PickOpponent(PickEntry& e, const CareerData& d, const RaceEvent& event, uint8_t language, uint32_t& seed, const std::vector<PickEntry>& earlier) {
    const uint32_t used = uint32_t(event.OpponentCount()); // 0x80078138
    if (used < 1) return 0;
    int32_t budget = 0x40;
    uint32_t slot = 0, number = 0;
    bool anyAvailable = false; // the original loops forever when no slot's car exists in the language
    for (uint32_t k = 0; k < used; k++) anyAvailable = anyAvailable || Available(d.cars, OpponentCarId(d.race, event.slots[k] & 0xFFFF), language);
    if (!anyAvailable) throw std::logic_error("event " + event.name + ": no opponent car of the language (the original never returns)");
    for (;;) {
        do {
            slot = event.slots[NextRandom(seed) % used];
            number = slot & 0xFFFF;
        } while (!Available(d.cars, OpponentCarId(d.race, number), language));
        bool reject = false;
        for (const PickEntry& prior : earlier) {
            if ((prior.opponent == number || prior.carId == OpponentCarId(d.race, number)) && (NextRandom(seed) & 0x1F) < 0x1D && budget > 0) {
                reject = true;
                break;
            }
        }
        if (!reject) break;
        budget--;
    }
    const uint32_t code = slot >> 26;
    const uint32_t carId = OpponentCarId(d.race, number);
    const CarInfoRecord& info = CarInfoOf(d.cars, carId);
    char paint;
    uint16_t colour;
    if (code != 0) {
        paint = PaintOfCode(code);
        colour = info.chipColors.at(PaintIndexOf(info, paint)); // 0x80060D28
    } else {
        const uint32_t count = uint32_t(info.PaintCount());
        uint32_t r = 0;
        for (int32_t tries = 0; tries < 2; tries++) { // at most two draws; a saturated colour (> 5) ends it
            r = NextRandom(seed);
            if (Saturation(info.chipColors.at(r % count)) > 5) break;
        }
        paint = char(info.paintIds.at(r % count));
        colour = info.chipColors.at(r % count);
    }
    e.carId = carId;
    e.paint = paint;
    e.colour = colour;
    e.opponent = uint16_t(slot);
    return code;
}

// 0x80010984(entry, model, code, &seed): the paint of the racing-modification body.
void RepaintForModel(PickEntry& e, uint32_t model, uint32_t code, const CareerData& d, uint32_t& seed) {
    const CarInfoRecord& info = CarInfoOf(d.cars, model);
    if (code == 0) {
        const uint32_t count = uint32_t(info.PaintCount());
        const uint32_t r = NextRandom(seed);
        e.colour = info.chipColors.at(r % count);
        e.paint = char(info.paintIds.at(r % count));
    } else {
        const char paint = PaintOfCode(code & 0x3F);
        e.colour = info.chipColors.at(PaintIndexOf(info, paint));
        e.paint = paint;
    }
}

} // namespace

EventMenuData EventMenuData::Load(const GuestImage& ovl4) {
    EventMenuData m;
    m.events = GuestStringList(ovl4, 0x80050D1Cu, 248);
    if (m.events.size() != 248) throw std::runtime_error("event menu data: expected 248 events at ovl4 0x80050D1C");
    m.goldListA = GuestStringList(ovl4, 0x80050BB4u, 64);
    m.goldListB = GuestStringList(ovl4, 0x80050BF8u, 64);
    m.prefixes = {GuestString(ovl4, 0x80022E64u), GuestString(ovl4, 0x80022E68u), GuestString(ovl4, 0x80022F1Cu)};
    m.randomCourses = GuestStringList(ovl4, 0x80050C4Cu, 64);
    m.machineTests = {GuestString(ovl4, 0x80022F20u), GuestString(ovl4, 0x80022F28u), GuestString(ovl4, 0x80022F30u)};
    for (uint32_t k = 0; k < 6; k++) {
        m.licencePrefixes.push_back(GuestString(ovl4, ovl4.Get<uint32_t>(0x80050D04u + k * 4)));
        m.licenceMessages[k] = ovl4.Get<uint32_t>(0x80051164u + k * 4);
    }
    return m;
}

int32_t LicenceOfTest(const EventMenuData& menu, const std::string& name) { // 0x800190E4
    const std::string prefix = name.substr(0, std::min<size_t>(3, name.size()));
    int32_t licence = 0;
    for (int32_t k = 0; k < int32_t(menu.licencePrefixes.size()); k++)
        if (menu.licencePrefixes[size_t(k)] == prefix) licence = k;
    return licence;
}

int32_t LicenceEntryCheck(const CareerState& s, const EventMenuData& menu, const std::string& name, uint32_t& message) { // 0x80019B88
    const int32_t licence = LicenceOfTest(menu, name);
    message = menu.licenceMessages[size_t(licence)];
    if (licence < 5 && !LicenceHeld(s, licence + 1)) return -3;
    return 1;
}

int32_t MachineTestMode(const EventMenuData& menu, const std::string& name) { // 0x8001861C
    for (int32_t k = 0; k < 3; k++)
        if (name == menu.machineTests[size_t(k)]) return 7 + k;
    return -1;
}

int32_t MenuEventIndex(const EventMenuData& menu, const std::string& name) { // 0x800188B0
    for (size_t i = 0; i < menu.events.size(); i++)
        if (menu.events[i] == name) return int32_t(i);
    return 0;
}

EventInfoTable BuildEventInfos(const CareerData& d, const EventMenuData& menu) { // 0x80019474 / 0x8001928C
    EventInfoTable t;
    for (const std::string& name : menu.events) {
        const int32_t row = d.race.FindEvent(name);
        if (row < 0) throw std::runtime_error("event " + name + " is not in the race file (the original reads through a null row)");
        const RaceEvent e = d.race.EventAt(size_t(row));
        const uint8_t* b = e.bytes.data();
        EventInfo info{};
        info.bonus = int32_t(uint32_t(uint16_t(b[0x98] | b[0x99] << 8)) * 100u);
        for (size_t k = 0; k < 6; k++) info.prize[k] = int32_t(uint32_t(uint16_t(b[0x78 + 2 * k] | b[0x79 + 2 * k] << 8)) * 100u);
        info.carListStart = int16_t(t.carListPool.size());
        info.carListCount = 0;
        info.powerLimit = int16_t(uint16_t(b[0x96] | b[0x97] << 8));
        uint16_t rules = 0;
        rules = uint16_t((rules & 0xFFFE) | b[0x75]);
        rules = uint16_t((rules & 0xFFF1) | (b[0x47] << 1));
        rules = uint16_t((rules & 0xFF8F) | (b[0x77] << 4));
        rules = uint16_t((rules & 0xFE7F) | (b[0x9A] << 7));
        rules = uint16_t((rules & 0xF9FF) | (b[0x9B] << 9));
        info.rules = rules;
        const std::vector<uint32_t> list = d.race.CarList(b[0x76]);
        for (uint32_t id : list) t.carListPool.push_back(id);
        info.carListCount = uint8_t(list.size());
        t.infos.push_back(info);
    }
    return t;
}

EntryCarState EntryCarStateOf(const TuneSheet& sheet) {
    EntryCarState c;
    c.naturallyAspirated = reinterpret_cast<const uint8_t*>(&sheet.config)[0x54] == 0;
    c.racingModified = sheet.stage[kTuneRacingModify] > 0;
    c.stock = true;
    static constexpr int kKinds[] = {0, 1, 2, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    for (int k : kKinds) c.stock = c.stock && sheet.stage[k] < 1;
    return c;
}

int32_t EntryCheck(const CareerState& s, const GarageBlock& g, const EventMenuData& menu, const EventInfoTable& infos, const std::string& name,
                   const EntryCarState& car, uint32_t& message) { // 0x8001973C
    const int32_t current = g.currentCar;
    message = 0x80000002u;
    if (current < 0) return -1;
    const std::string prefix = name.substr(0, std::min<size_t>(3, name.size()));
    auto allWon = [&](const std::vector<std::string>& list) {
        if (gt2::pc::unlockSimulationEvents) return true;
        bool won = true;
        for (const std::string& n : list)
            if (ResultAt(s.record, MenuEventIndex(menu, n)) != 1) won = false;
        return won;
    };
    if (prefix == menu.prefixes[0] || prefix == menu.prefixes[1]) { // 0x800183EC
        message = 0x8000002Fu;
        if (!allWon(menu.goldListA)) return -9; // 0x8001847C
    }
    if (prefix == menu.prefixes[2]) { // 0x8001859C
        message = 0x80000030u;
        if (!allWon(menu.goldListB)) return -10; // 0x8001850C
    }
    const EventInfo& info = infos.infos.at(size_t(MenuEventIndex(menu, name)));
    const GarageCar& c = g.cars[current];
    const uint16_t power = c.powerFlags;
    const uint32_t carId = c.carId;
    const uint32_t drive = uint32_t(c.weightDrive >> 13);
    const int32_t licences = 6 - LicenceLevel(s);
    const uint32_t rules = info.rules;
    if ((rules & 1) == 1 && !PartOwned(c, 0x2D)) {
        message = 0x80000025u;
        return -2;
    }
    static constexpr uint32_t kLicenceMessage[7] = {0, 0x13, 0x12, 0x14, 0x16, 0x15, 0x17};
    const uint32_t licence = (rules & 0xE) >> 1;
    if (licence >= 1 && licences < int32_t(licence)) {
        message = 0x80000000u | kLicenceMessage[licence];
        return -3;
    }
    message = 0x80000023u;
    const uint32_t driveRule = (rules & 0x70) >> 4;
    if (driveRule == 2) {
        if (drive != 0) return -4;
    } else if (driveRule == 1 || driveRule == 3 || driveRule == 4 || driveRule == 5) {
        static constexpr uint32_t kDrive[6] = {0, 1, 0, 3, 4, 2};
        if (drive != kDrive[driveRule]) return -4;
    }
    const uint32_t aspiration = (rules & 0x180) >> 7;
    if (aspiration == 1) {
        message = 0x80000026u;
        if (!car.naturallyAspirated) return -5;
    } else if (aspiration == 2) {
        message = 0x80000027u;
        if (car.naturallyAspirated) return -5;
    }
    message = 0x8000001Eu;
    const uint32_t modification = (rules & 0x600) >> 9;
    if (modification == 1) {
        message = 0x80000029u;
        if (car.racingModified) return -6;
    } else if (modification == 2) {
        message = 0x80000028u;
        if (!car.racingModified) return -6;
    } else if (modification == 3) {
        message = 0x8000002Cu;
        if (!car.stock) return -6;
    }
    message = 0x8000002Bu;
    if (info.powerLimit >= 1 && int32_t(power & 0x3FFF) > info.powerLimit) return -7;
    message = 0x8000001Eu;
    if (info.carListCount == 0) return 1;
    for (int32_t k = 0; k < info.carListCount; k++)
        if (infos.carListPool.at(size_t(uint16_t(info.carListStart)) + size_t(k)) == carId) return 1;
    return -8;
}

void PreparePrizes(PrizeBlock& p, const RaceEvent& event, uint32_t vsyncCounter, const CareerData& d, TuneSheet& sheet, BuildScratch scratch) { // 0x80018C8C
    uint32_t seed = vsyncCounter;
    for (int32_t& v : p.prize) v = 0; // 0x80018BF4
    p.prizeCarCount = 0;
    const uint8_t* b = event.bytes.data();
    p.bonus = int32_t(uint32_t(uint16_t(b[0x98] | b[0x99] << 8)) * 100u);
    for (size_t k = 0; k < 6; k++) p.prize[k] = int32_t(uint32_t(uint16_t(b[0x78 + 2 * k] | b[0x79 + 2 * k] << 8)) * 100u);
    for (size_t k = 0; k < 4; k++) {
        const uint32_t id = event.prizeCars[k];
        if (id == 0) break;
        const CarInfoRecord& info = CarInfoOf(d.cars, id);
        const uint32_t r = NextRandom(seed);
        const char paint = char(info.paintIds.at(r % uint32_t(info.PaintCount())));
        CarConfig config = *CatalogueCarConfig(d.tables, id);
        const int32_t value = CataloguePrice(d.tables, id);
        const bool flag15 = AnalysePurchase(sheet, id, config, d);
        const bool flag14 = GearboxFlag(d.tables, id);
        std::array<uint8_t, 7> fitted{};
        const bool haveParts = FittedParts(d, id, fitted);
        BuildGarageCar(p.prizeCars[k], id, id, uint32_t(int32_t(int8_t(paint))), config, value, flag15, flag14, haveParts ? fitted.data() : nullptr, d.tables, scratch);
        p.prizeCarCount = int8_t(p.prizeCarCount + 1);
    }
}

std::string EventCourse(const EventMenuData& menu, const RaceEvent& event, uint32_t vsyncCounter) { // 0x80013108 / 0x8001907C
    if (event.course != "none") return event.course;
    if (menu.randomCourses.empty()) throw std::runtime_error("event course list (ovl4 0x80050C4C) is empty");
    uint32_t seed = vsyncCounter;
    return menu.randomCourses[NextRandom(seed) % uint32_t(menu.randomCourses.size())];
}

std::vector<GridCar> PickEventOpponents(const CareerData& d, const RaceEvent& event, uint8_t language, uint32_t& seed, size_t count) { // 0x80010A30
    std::vector<GridCar> grid;
    std::vector<PickEntry> entries;
    for (size_t i = 0; i < count; i++) {
        PickEntry e;
        const uint32_t code = PickOpponent(e, d, event, language, seed, entries);
        e.byte4 = 0;
        GridCar g;
        g.carId = e.carId;
        g.paintCode = uint8_t(code);
        g.config = OpponentCarConfig(d.tables, d.race.OpponentBytes(e.opponent)); // 0x800768C0 + 0x80076F5C
        const std::span<const uint8_t> rm = d.tables.Row(kTableRacingModify, g.config.racingModify);
        if (rm[0x0E] != 0) { // the opponent races the racing-modification body (row +8)
            g.carId = uint32_t(rm[8]) | uint32_t(rm[9]) << 8 | uint32_t(rm[10]) << 16 | uint32_t(rm[11]) << 24;
            RepaintForModel(e, g.carId, code, d, seed);
        }
        g.config.flags = uint8_t(g.config.flags | 0x40);
        g.paint = e.paint;
        g.colour = e.colour;
        g.opponent = e.opponent;
        g.name = CarInfoOf(d.cars, g.carId).rawName; // 0x80060AE8 (strcpy: the 0x7F padding + the name, up to the NUL)
        entries.push_back(e);
        grid.push_back(g);
    }
    return grid;
}

} // namespace gt2::career
