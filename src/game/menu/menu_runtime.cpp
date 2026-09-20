#include "game/menu/menu_runtime.h"
#include "game/pc_features.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "game/career/results.h"
#include "game/career/tuning.h"
#include "gt2formats/car_info.h"
#include "gt2formats/overlay_data.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2vfs/inflate.h"

namespace gt2::menu {

using namespace menu_item_flag;

namespace {

// GT-mode overlay (member 4) addresses.
constexpr uint32_t kUnqualifiedList = 0x80050B78u;  // 0x80018210: u32 pointers to 5-character car ids, 0-terminated
constexpr uint32_t kNoName = 0x80050B9Cu;           // 0x800182A8: UTF-16 "No Name"
constexpr uint32_t kRacingPrefix = 0x80050BB0u;     // 0x8001828C(1): UTF-16 prefix of racing-modified car names
constexpr uint32_t kMedalSprites = 0x80050978u;     // + medal * 12 (licence tests, 0x8001B9AC)
constexpr uint32_t kDriveSprites = 0x8005093Cu;     // + drive * 12 (type 0x97)
constexpr uint32_t kLicenceLevelSprites = 0x800509B4u; // + level * 12 (type 0xCC)
constexpr uint32_t kBadgeSprite = 0x80050A50u;      // type 0 bit 18
constexpr uint32_t kDriveNames = 0x80051260u;       // 5 pointers into the data-gt.txd block (0x801C30C0..)
constexpr uint32_t kFormatInt = 0x80023CFCu;        // "%d" (ovl4 rodata)
constexpr uint32_t kFormatNone = 0x80023D08u;       // the "no value" text
constexpr uint32_t kFormatYear = 0x80023D10u;       // the model year format
constexpr uint32_t kFormatFraction = 0x80023D00u;   // "%d.%02d" (types 0xBC / 0xC0 / 0xC1)
constexpr uint32_t kEquippedParts = 0x80050AB0u;    // 0x80017318: {s16 kind, unistrdb base, dx, dy}, ended by kind < 0
constexpr uint32_t kTxdTorque = 0x801C30FAu;        // "%d.%dlb-ft / " (type 0xBA)
// data-gt.txd block (RAM 0x801C30C0, MenuAssets::String): formats of the car data.
constexpr uint32_t kTxdNotAvailable = 0x801C30C0u, kTxdPurchased = 0x801C30D0u, kTxdPowerRpm = 0x801C30EDu, kTxdMm = 0x801C311Au;
constexpr uint32_t kTxdCc = 0x801C311Fu, kTxdHp = 0x801C313Bu;
// data-global.txd (gzip in GT2.OVL member 1, the title overlay; copied by the title to RAM): the menus' "%dlb" /
// "%dhp" formats at 0x801EF6C1 / 0x801EF6C6 = inflated file + 0x7BA / + 0x7BF (RAM image work/re/gtmode vs the
// file, 2026-09-19): the copy starts at 0x801EEF07.
constexpr uint32_t kGlobalTxdBase = 0x801EEF07u, kGlobalLb = 0x801EF6C1u, kGlobalHp = 0x801EF6C6u;
constexpr uint32_t kTextColour = 0x6E6E6E;
constexpr uint32_t kPowerColour = 0x145A78; // types 0x48 / 0x49 (0x8001B9AC)
// Licence test records of the medal item groups (0x8001B9AC): first type -> licence index (records 0x801CACF8 +
// licence * 0x668).
constexpr struct { uint16_t first; int licence; } kMedalGroups[] = {{0x53, 3}, {0x5E, 5}, {0x69, 4}, {0x74, 2}, {0x7F, 1}, {0x8A, 0}};

std::u16string ReadUtf16(const GuestImage& g, uint32_t a) {
    std::u16string s;
    for (;; a += 2) {
        const uint16_t c = g.Get<uint16_t>(a);
        if (c == 0 || s.size() > 256) break;
        s.push_back(char16_t(c));
    }
    return s;
}

std::string ReadAscii(const GuestImage& g, uint32_t a) {
    std::string s;
    for (;; a++) {
        const uint8_t c = g.Get<uint8_t>(a);
        if (c == 0 || s.size() > 256) break;
        s.push_back(char(c));
    }
    return s;
}

// The inflated data-global.txd of the title overlay (gzip member with that file name).
std::vector<uint8_t> GlobalTxd(const GuestImage& ovl1) {
    static const char kName[] = "data-global.txd";
    const std::vector<uint8_t>& b = ovl1.bytes;
    for (size_t i = 0; i + 10 + sizeof kName < b.size(); i++) {
        if (b[i] != 0x1F || b[i + 1] != 0x8B || b[i + 2] != 8 || (b[i + 3] & 8) == 0) continue;
        if (std::memcmp(&b[i + 10], kName, sizeof kName) != 0) continue;
        return Inflate(std::span<const uint8_t>(b).subspan(i + 10 + sizeof kName));
    }
    throw std::runtime_error("data-global.txd not found in GT2.OVL member 1");
}

std::string Format(const std::string& format, int a, int b = 0) {
    char buf[96];
    std::snprintf(buf, sizeof buf, format.c_str(), a, b);
    return buf;
}

// 0x8001FE0C(hundreds of millions, rest): "0" when both are 0; the rest with commas when `hi` is 0; else hi and the
// rest as 8 digits, with a comma every three digits from the right.
std::string MenuHundredMillions(int32_t hi, uint32_t lo) {
    if (hi == 0) return lo == 0 ? std::string("0") : MenuThousands(lo);
    char digits[32];
    std::snprintf(digits, sizeof digits, "%d%08u", hi, lo);
    const std::string d = digits;
    std::string out;
    for (size_t i = 0; i < d.size(); i++) {
        if (i > 0 && (d.size() - i) % 3 == 0) out.push_back(',');
        out.push_back(d[i]);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------- data

MenuData MenuData::Load(const DiscImage& disc, const GtfsVolume& vol) {
    MenuData d{career::CareerData::Load(disc, vol), {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}};
    d.career.strict = false; // the game takes the native approximations the career model names (garage.h)
    d.events = career::EventMenuData::Load(d.career.ovl4);
    d.eventInfos = career::BuildEventInfos(d.career, d.events);
    d.strings = ParseUniStrDb(vol.Read("carparam/usa_unistrdb.dat"));
    d.colours = std::make_unique<CarColorNames>(CarColorNames::Load(vol));
    const GuestImage& o4 = d.career.ovl4;
    for (uint32_t a = kUnqualifiedList;; a += 4) {
        const uint32_t p = o4.Get<uint32_t>(a);
        if (p == 0 || d.unqualifiedCars.size() > 64) break;
        d.unqualifiedCars.push_back(PackCarId(ReadAscii(o4, p).substr(0, 5))); // 0x80060924
    }
    d.noName = ReadUtf16(o4, kNoName);
    d.racingPrefix = ReadUtf16(o4, kRacingPrefix);
    d.vectors = NavVectors::Load(o4);
    d.usedCars = std::make_unique<UsedCarLists>(UsedCarLists::Load(vol));
    d.listNames = std::make_unique<MenuListNames>(MenuListNames::Load(vol, o4));
    const std::vector<uint8_t> global = GlobalTxd(LoadOverlayImage(disc, 1));
    for (uint32_t a : {kGlobalLb, kGlobalHp}) {
        std::string s;
        for (size_t i = a - kGlobalTxdBase; i < global.size() && global[i]; i++) s.push_back(char(global[i]));
        d.globalFormats[a] = s;
    }
    return d;
}

std::u16string MenuData::String(uint16_t index) const {
    if (index < strings.size()) return strings[index];
    return noName;
}

// ---------------------------------------------------------------- career actions

career::TuneSheet& CareerMenuActions::SheetOf(int index) {
    career::GarageBlock& g = state_.garage;
    if (!sheet_) sheet_ = std::make_unique<career::TuneSheet>();
    const int16_t cur = int16_t(index >= 0 && index < g.count ? index : -1);
    const uint32_t id = cur >= 0 ? g.cars[cur].carId : 0;
    if (cur != sheetCar_ || id != sheetCarId_ || (cur >= 0 && std::memcmp(&sheetConfig_, &g.cars[cur].config, sizeof(CarConfig)) != 0)) {
        if (cur >= 0) {
            career::LoadCarSheet(*sheet_, g.cars[cur], data_.career.tables); // 0x800173E8: clear 0xFF, rows, configuration
            sheetConfig_ = g.cars[cur].config;
        } else {
            career::ClearTuneSheet(*sheet_);
        }
        sheetCar_ = cur;
        sheetCarId_ = id;
    }
    return *sheet_;
}

const career::TuneSheet& CareerMenuActions::CurrentSheet() { return SheetOf(state_.garage.currentCar); }

PartFigures CareerMenuActions::PreviewPart(int32_t kind) { // 0x8001DB90 (the current car's sheet loaded)
    const career::GarageBlock& g = state_.garage;
    PartFigures f;
    if (g.currentCar < 0) return f; // the part pages are reached only with a current car (0x98 / 0xAD checks)
    const career::TuneSheet& sheet = SheetOf(g.currentCar);
    const career::PartPreview p = career::PreviewPart(g.cars[g.currentCar], sheet, kind, data_.career);
    f.price = p.price;
    f.owned = p.owned;
    f.powerBefore = p.powerBefore;
    f.powerAfter = p.powerAfter;
    return f;
}

uint32_t CareerMenuActions::RacingBody(bool first) { // 0x800174F4 / 0x80017530 on the current car's sheet
    const career::TuneSheet& sheet = CurrentSheet();
    return first ? career::FirstRacingBody(sheet, racingBody_) : career::NextRacingBody(sheet, racingBody_);
}

uint32_t CareerMenuActions::WheelId(const std::string& code) { return career::WheelIdOfCode(data_.career, code); } // 0x80013A28

bool CareerMenuActions::CarStages(int index, std::array<int16_t, 27>& stages) { // 0x80017288: clear, rows of the car, its configuration
    const career::GarageBlock& g = state_.garage;
    if (index < 0 || index >= g.count) return false;
    if (!listSheet_) listSheet_ = std::make_unique<career::TuneSheet>();
    if (index != listSheetCar_ || std::memcmp(&listSheetConfig_, &g.cars[index].config, sizeof(CarConfig)) != 0) {
        career::LoadCarSheet(*listSheet_, g.cars[index], data_.career.tables);
        listSheetCar_ = index;
        listSheetConfig_ = g.cars[index].config;
    }
    std::copy(std::begin(listSheet_->stage), std::end(listSheet_->stage), stages.begin());
    return true;
}

int CareerMenuActions::WheelColour(uint32_t wheelId) { // 0x80021BEC(wheel, slot car id != slot model id)
    const career::GarageBlock& g = state_.garage;
    const bool modified = g.currentCar >= 0 && g.cars[g.currentCar].carId != g.cars[g.currentCar].modelId;
    return career::WheelColour(data_.career, wheelId, modified);
}

uint32_t CareerMenuActions::CheckTransaction(Transaction& t) { // 0x8001DFEC
    career::GarageBlock& g = state_.garage;
    switch (t.state) {
    case Transaction::kBuyCar: {
        const int32_t r = career::CanBuy(g, t.price); // 0x80017914
        if (r == 1) return 0x80000000u;
        if (r == -2) return 0x8000000Eu;
        if (r == -1) return 0x80000001u;
        return 0;
    }
    case Transaction::kBuyPart: {
        if (t.partSlot < 0) return 0x80000002u;
        switch (career::PartPurchaseCheck(g, t.partSlot, t.partKind, data_.career.tables)) { // 0x80017B40
        case 1:
            switch (t.partKind) {
            case 7: case 9: case 0x12: case 0x13: case 0x14: case 0x20: case 0x22: return 0x8000000Bu;
            default: return 0x80000020u;
            }
        case -8: return 0x80000019u;
        case -7: return 0x80000011u;
        case -6: return 0x80000010u;
        case -5: return 0x8000000Fu;
        case -3: return 0x80000003u;
        case -1: return 0x80000001u;
        default: return 0;
        }
    }
    case Transaction::kWheels: // price 0 = no wheel chosen yet (0x8001DAF0); 0x800181D0: money >= price
        if (t.price == 0) return 0;
        if (t.wheelSlot < 0) return 0x80000002u;
        return t.price <= g.money ? 0x80000020u : 0x80000001u;
    case Transaction::kSell: return t.sellIndex >= 0 ? 0x8000000Du : 0x80000002u;
    case Transaction::kSelect:
        career::SelectCar(g, int16_t(t.selectIndex)); // 0x8001DFEC case 6 selects already, then 0x80017480
        CurrentSheet();
        return 0x80000005u;
    default: return 0;
    }
}

uint32_t CareerMenuActions::RunTransaction(Transaction& t) { // 0x8001DDAC
    career::GarageBlock& g = state_.garage;
    uint16_t next = 0;
    uint32_t message = 0;
    auto makeLastCurrent = [&] { // LAB_8001DE90
        next = Transaction::kBuyCar;
        career::SelectCar(g, int16_t(g.count - 1));
        CurrentSheet();
    };
    switch (t.state) {
    case Transaction::kBuyCar: {
        if (!purchaseSheet_) purchaseSheet_ = std::make_unique<career::TuneSheet>();
        std::vector<uint8_t> scratch(0x400, 0);
        career::BuyCar(g, t.carId, t.paint, t.price, data_.career, *purchaseSheet_, career::BuildScratch{scratch.data()}); // 0x8001796C
        next = Transaction::kCarBought;
        if (g.currentCar >= 0) {
            message = 0x80000008u;
        } else {
            message = 0x80000004u;
            makeLastCurrent();
        }
        break;
    }
    case Transaction::kCarBought:
        message = 0x80000005u;
        makeLastCurrent();
        break;
    case Transaction::kBuyPart: {
        career::BuyPart(g, t.partSlot, t.partKind, data_.career.tables); // 0x80017C98
        auto fit = [&] { // 0x80017D6C(slot, kind, player 0, paint) on the sheet of that car (0x800173E8)
            career::FitPart(g, t.partSlot, t.partKind, t.paint, SheetOf(t.partSlot), racingBody_, data_.career, career::BuildScratch{scratch_.data()});
        };
        switch (t.partKind) {
        case 7: case 9: case 0x20: // fitted at once: "installed"
            next = Transaction::kBuyPart;
            message = 0x80000006u;
            fit();
            break;
        case 0x12: case 0x13: case 0x14: case 0x22:
            next = Transaction::kBuyPart;
            message = 0x8000001Fu;
            fit();
            break;
        default: // bought; the message page offers to fit it (state 4)
            next = Transaction::kFitPart;
            message = 0x80000007u;
            break;
        }
        break;
    }
    case Transaction::kFitPart:
        career::FitPart(g, t.partSlot, t.partKind, t.paint, SheetOf(t.partSlot), racingBody_, data_.career, career::BuildScratch{scratch_.data()});
        next = Transaction::kBuyPart;
        switch (t.partKind) {
        case 6: case 0x10: case 0x11: case 0x1A: case 0x1B: case 0x1C: case 0x1D: case 0x1E: case 0x1F: case 0x2E: case 0x2F: case 0x30: case 0x31:
            message = 0x80000009u;
            break;
        default: message = 0; break;
        }
        break;
    case Transaction::kWheels: // 0x80018100(slot, wheel, price): state 0, no message (back)
        if (t.wheelSlot >= 0)
            career::BuyWheels(g, t.wheelSlot, t.wheel, t.price, SheetOf(t.wheelSlot), data_.career, career::BuildScratch{scratch_.data()});
        next = Transaction::kNone;
        break;
    case Transaction::kSell:
        next = Transaction::kSell;
        message = 0x8000001Cu;
        career::SellCar(g, t.sellIndex, data_.career.tables); // 0x80017A70
        CurrentSheet();
        break;
    default: break;
    }
    t.state = next;
    return message;
}

int32_t CareerMenuActions::EntryCheck(const std::string& name, uint32_t& message) {
    if (!name.empty() && name[0] == 'L') // 0x80018608 -> 0x80019B88: tests of licence L < 5 need licence L + 1
        return career::LicenceEntryCheck(state_, data_.events, name, message);
    const career::EntryCarState car = career::EntryCarStateOf(CurrentSheet());
    return career::EntryCheck(state_, state_.garage, data_.events, data_.eventInfos, name, car, message); // 0x8001973C
}

// ---------------------------------------------------------------- popup lists

namespace {
class CareerPopupList final : public PopupList {
public:
    CareerPopupList(MenuListKind kind, const MenuAssets& assets, const MenuData& data, MenuActions& actions)
        : list_(kind, assets, *data.listNames), data_(data), actions_(actions) {
        list_.sound = [this](int id) { actions_.Sound(id); };
        list_.moveToTop = [this](int index) { career::MoveCar(actions_.Career().garage, index, 0); }; // 0x8001EF10(garage, i, 0)
    }
    void Reset() override { list_.Reset(); }
    int Count() const override { return list_.count; }
    void Load(const MenuPage& page, const MenuItem& item, bool keepSelection) override {
        const career::CareerState& s = actions_.Career();
        if (list_.kind == MenuListKind::kGarage) { // 0x800204EC: the garage slots
            std::vector<MenuGarageRow> rows;
            for (int i = 0; i < s.garage.count && i < career::kGarageCapacity; i++) {
                const career::GarageCar& c = s.garage.cars[i];
                MenuGarageRow r;
                r.carId = c.carId;
                r.paint = c.paint;
                r.modelId = c.modelId;
                r.word98 = c.powerFlags;
                rows.push_back(r);
            }
            list_.LoadGarage(&item, std::move(rows), s.garage.currentCar);
        } else { // 0x800209B0 on a load with argument 0, then 0x80020A0C with the page's maker
            if (!keepSelection) list_.Reset();
            list_.LoadUsedCars(&item, MenuUsedCarRows(*data_.usedCars, data_.career.cars, s.record.days, page.Maker(), s.language));
        }
    }
    int Update(const PadState* pad, bool active) override {
        MenuListPad p;
        if (pad) {
            p.held = pad->held;
            p.pressed = pad->pressed;
            p.repeat = pad->repeat;
        }
        return list_.Update(pad ? &p : nullptr, active);
    }
    int Selection() const override { return list_.chosen; }
    uint32_t CarId() const override { return list_.ChosenUsedCar().carId; }
    int PaintId() const override { return list_.ChosenUsedCar().paint; }
    int32_t Price() const override { return int32_t(list_.ChosenUsedCar().price); }
    void Draw(const MenuItem& /*item*/, std::vector<MenuPrim>& drawOrder) override { list_.Draw(drawOrder); }

private:
    MenuPopupList list_;
    const MenuData& data_;
    MenuActions& actions_;
};
} // namespace

std::unique_ptr<PopupList> MakeCareerPopupList(MenuListKind kind, const MenuAssets& assets, const MenuData& data, MenuActions& actions) {
    return std::make_unique<CareerPopupList>(kind, assets, data, actions);
}

// ---------------------------------------------------------------- cursor

void Cursor::Reset() { // 0x8001E22C
    x = toX = fromX = 0x100;
    y = toY = fromY = 0xFC;
    countdown = 0;
    fx = fy = vx = vy = 0;
    blink = 0;
    item = -1;
}

void Cursor::Bounce(int dir) { // 0x8001E26C
    x = toX;
    y = toY;
    fy = int32_t(y) << 8;
    fx = int32_t(x) << 8;
    constexpr int32_t k = 10000000;
    int32_t ax = 0, ay = 0;
    switch (dir) {
    case 2: ay = k; break;
    case 3: ay = -k; break;
    case 4: ax = k; break;
    case 5: ax = -k; break;
    case 6: ax = -k, ay = k; break;
    case 7: ax = k, ay = k; break;
    case 8: ax = -k, ay = -k; break;
    case 9: ax = k, ay = -k; break;
    default: break;
    }
    vx = ax;
    vy = ay;
}

// ---------------------------------------------------------------- runtime

MenuRuntime::MenuRuntime(const MenuPages& pages, const MenuData& data, MenuActions& actions) : pages_(pages), data_(data), actions_(actions) {}

void MenuRuntime::AttachLists(std::unique_ptr<PopupList> garage, std::unique_ptr<PopupList> usedCars) {
    garageList_ = std::move(garage);
    usedList_ = std::move(usedCars);
}

SelectContext MenuRuntime::Context() const {
    SelectContext c;
    const career::CareerState& s = actions_.Career();
    for (int l = 0; l < 6; l++) c.licenceHeld[l] = career::LicenceHeld(s, l);
    c.racingBodies = racingBodies_;
    return c;
}

void MenuRuntime::Start(uint32_t page) {
    cursor_.Reset();
    popup_ = 0;
    transaction_ = Transaction{};
    backAllowed_ = 1;
    holdOff_ = 0;
    pageCountdown_ = 0;
    if (garageList_) garageList_->Reset(); // 0x80013CF8: the GT-mode entry resets both lists
    if (usedList_) usedList_->Reset();
    const int saved = music_; // 0x801EF5FC: the track the menus were left with (-1 at the first entry)
    music_ = -1;
    LoadPage(page, 0);
    if (saved >= 0 && saved != music_) { // 0x8001D118: requested again after the page load
        music_ = saved;
        actions_.Music(saved);
    }
}

void MenuRuntime::NewPageRequest(uint32_t page, uint32_t argument) {
    nextPage_ = page;
    nextArgument_ = argument;
    pageCountdown_ = 3;
    holdOff_ = 8;
}

void MenuRuntime::RequestPage(uint32_t page, uint32_t argument) { NewPageRequest(page, argument); }

void MenuRuntime::LoadPage(uint32_t id, uint32_t argument) { // 0x8001D2CC
    const uint32_t old = pageId_, previousFlags = loaded_ ? page_.flags : 0;
    page_ = pages_.Page(id);
    pageId_ = id;
    loaded_ = true;
    bool setBack = true;
    if (!page_.KeepsHistory()) {
        if (int32_t(id) >= 0) {
            backBefore_ = back_;
            back_ = page_.back;
            setBack = false;
        } else if (old & 0x80000000u) {
            setBack = false;
        }
    } else if ((previousFlags >> 8) & 1) {
        setBack = false;
    }
    if (setBack) {
        backBefore_ = back_;
        back_ = old;
    }
    char text[160];
    std::snprintf(text, sizeof text, "page %u%s (back %d)", page_.id, (id & 0x80000000u) ? " (message)" : "", int32_t(back_));
    log.push_back(text);
    const career::CareerState& s = actions_.Career();
    for (const MenuItem& it : page_.items) {
        uint16_t type = it.Type();
        if (it.Has(kWheel)) { // wheel shop: 0x8001AB6C (the current car shown unless it is) + 0x8001DAF0 (state 5, price 0)
            const int cur = s.garage.currentCar;
            if (cur >= 0 && !(car_.shown && car_.current)) {
                const career::GarageCar& c = s.garage.cars[cur];
                DoShowCar(PendingShow{cur, c.carId, c.modelId, PaintIndexOf(c.modelId, c.paint), 1}, false); // 0x8001AC20 directly
            }
            transaction_.state = Transaction::kWheels;
            transaction_.price = 0;
            transaction_.wheelSlot = cur;
        }
        if (type >= 0x14 && type <= 0x46) { // 0x8001DB90 (preview) + 0x8001DB24 (state 3, kind, slot, price)
            const PartFigures f = actions_.PreviewPart(type - 0x14);
            transaction_.owned = f.owned;
            transaction_.powerBefore = f.powerBefore;
            transaction_.powerAfter = f.powerAfter;
            transaction_.state = Transaction::kBuyPart;
            transaction_.partKind = type - 0x14;
            transaction_.partSlot = s.garage.currentCar;
            transaction_.price = s.garage.currentCar >= 0
                                     ? career::PartPrice(data_.career.tables, s.garage.cars[s.garage.currentCar].carId, type - 0x14)
                                     : 0;
            type = uint16_t(type - 0x14); // the original compares the part kind with 9 / 0x50 below (kind 9 loads the garage list)
        }
        if (type == 9 && garageList_) garageList_->Load(page_, it, true);                    // 0x800204EC
        if (type == 0x50 && usedList_) usedList_->Load(page_, it, argument != 0);            // 0x800209B0 + 0x80020A0C
    }
    if (page_.Music() != 0xFF && page_.Music() != music_) { // 0x80018FA4
        music_ = page_.Music();
        actions_.Music(music_);
    }
    // 0x8001B3AC after the load: reopen a list the page was left from, else the default item.
    int garageItem = -1, usedItem = -1;
    for (size_t i = 0; i < page_.items.size(); i++) {
        if (garageItem < 0 && page_.items[i].Type() == 9) garageItem = int(i);  // 0x8001D4E8
        if (usedItem < 0 && page_.items[i].Type() == 0x50) usedItem = int(i);   // 0x8001D554
    }
    if (garageItem >= 0 && (argument & 1) && garageList_ && garageList_->Count() > 0) {
        popup_ = 1;
        CursorToItem(garageItem);
    } else if (usedItem >= 0 && (argument & 1) && usedList_ && usedList_->Count() > 0) {
        popup_ = 2;
        CursorToItem(usedItem);
    } else {
        CursorToDefault();
    }
    pageCountdown_ = 0;
}

void MenuRuntime::CursorToDefault() { // 0x8001E924
    const int d = DefaultItem(page_.items, loaded_);
    cursor_.item = d;
    if (d < 0) {
        cursor_.item = ItemUnder(page_.items, cursor_.toX, cursor_.toY, Context(), loaded_);
    } else {
        cursor_.fromX = cursor_.toX;
        cursor_.fromY = cursor_.toY;
        cursor_.toX = int16_t(page_.items[size_t(d)].CenterX());
        cursor_.countdown = 8;
        cursor_.toY = int16_t(page_.items[size_t(d)].CenterY());
    }
    cursor_.blink = 0;
}

void MenuRuntime::CursorToItem(int index) { // 0x8001E9D0
    cursor_.item = index;
    cursor_.fromX = cursor_.toX;
    cursor_.fromY = cursor_.toY;
    cursor_.toX = int16_t(page_.items[size_t(index)].CenterX());
    cursor_.countdown = 8;
    cursor_.blink = 0;
    cursor_.toY = int16_t(page_.items[size_t(index)].CenterY());
}

void MenuRuntime::CursorUpdate(const PadState* pad) { // 0x8001E328
    Cursor& c = cursor_;
    if (++c.blink > 64) c.blink = 0;
    if (c.countdown == 0) {
        c.x = c.toX;
        c.vx = c.vy = 0;
        c.y = c.toY;
        c.fx = int32_t(c.x) << 8;
        c.fy = int32_t(c.y) << 8;
    }
    const SelectContext ctx = Context();
    auto move = [&](int dir, int fromX, int fromY, int current) { return NearestItem(page_.items, fromX, fromY, dir, current, ctx, data_.vectors, loaded_); };
    if (c.countdown < 2) {
        const uint32_t held = pad ? pad->held : 0, press = pad ? (pad->pressed | pad->repeat) : 0;
        int dir = 0;
        if (press & 0x10) dir = 7;
        if (press & 0x20) dir = 9;
        if (press & 0x1000) dir = 6;
        if (press & 0x2000) dir = 8;
        if (press & pad::kUp) {
            dir = 2;
            if (held & pad::kLeft) dir = 7;
            if (held & pad::kRight) dir = 6;
        }
        if (press & pad::kDown) {
            dir = 3;
            if (held & pad::kLeft) dir = 9;
            if (held & pad::kRight) dir = 8;
        }
        if (press & pad::kLeft) {
            dir = 4;
            if (held & pad::kUp) dir = 7;
            if (held & pad::kDown) dir = 9;
        }
        if (press & pad::kRight) {
            dir = 5;
            if (held & pad::kUp) dir = 6;
            if (held & pad::kDown) dir = 8;
        }
        if (dir != 0) {
            const int found = move(dir, c.toX, c.toY, c.item);
            c.direction = int8_t(dir);
            if (found < 0) {
                actions_.Sound(kSoundBuzzer);
                c.Bounce(dir);
                dir = 1;
                c.fromX = c.toX;
                c.fromY = c.toY;
            } else {
                actions_.Sound(kSoundMove);
                c.fromX = c.toX;
                c.fromY = c.toY;
                c.toX = int16_t(page_.items[size_t(found)].CenterX());
                c.item = found;
                c.blink = 0;
                c.toY = int16_t(page_.items[size_t(found)].CenterY());
            }
            c.lastResult = int8_t(dir);
            c.countdown = 8;
        }
    }
    if (c.countdown != 0 && --c.countdown >= 6) { // two frames after a move: a second key held makes it diagonal
        const uint32_t held = pad ? pad->held : 0;
        int d = c.direction;
        if (d == 3) {
            if (held & pad::kLeft) d = 9;
            if (held & pad::kRight) d = 8;
        } else if (d == 2) {
            if (held & pad::kLeft) d = 7;
            if (held & pad::kRight) d = 6;
        } else if (d == 4) {
            if (held & pad::kUp) d = 7;
            if (held & pad::kDown) d = 9;
        } else if (d == 5) {
            if (held & pad::kUp) d = 6;
            if (held & pad::kDown) d = 8;
        }
        if (d != c.direction) {
            const int found = move(d, c.fromX, c.fromY, -1);
            if (found < 0) {
                if (c.lastResult == 1) {
                    c.Bounce(d);
                    c.fromX = c.toX;
                    c.fromY = c.toY;
                }
            } else {
                c.lastResult = int8_t(d);
                c.direction = int8_t(d);
                c.toX = int16_t(page_.items[size_t(found)].CenterX());
                c.item = found;
                c.blink = 0;
                c.toY = int16_t(page_.items[size_t(found)].CenterY());
            }
        }
    }
    // Spring towards the target (x 256 fixed point, per field).
    const int32_t nvx = c.vx + ((((int32_t(c.toX) * 256 - c.fx) * 2500 + c.vx * -110) / 60) * 100) / 180;
    const int32_t nvy = c.vy + ((((int32_t(c.toY) * 256 - c.fy) * 2500 + c.vy * -110) / 60) * 100) / 180;
    c.vx = nvx;
    c.vy = nvy;
    c.fx += nvx / 60;
    c.fy += nvy / 60;
    c.x = int16_t(uint32_t(c.fx < 0 ? c.fx + 255 : c.fx) >> 8);
    c.y = int16_t(uint32_t(c.fy < 0 ? c.fy + 255 : c.fy) >> 8);
    // Choose (0xA00): the item's action, its third argument = the cross bit; back (0x500) unless an action ran.
    const uint32_t pressed = pad ? pad->pressed : 0;
    const bool acted = (pressed & pad::kChoose) != 0 && c.item >= 0 && Action(c.item, (pressed & pad::kCross) != 0);
    if (!acted && (pressed & pad::kBack)) Back();
}

void MenuRuntime::Back() { // 0x800142CC
    if (int32_t(back_) >= 0 && (backAllowed_ & 1)) {
        NewPageRequest(back_, 1);
        actions_.Sound(kSoundBack);
    } else {
        actions_.Sound(kSoundBuzzer);
    }
}

int MenuRuntime::PaintIndexOf(uint32_t carId, uint32_t paintId) const { // 0x80018350
    const CarInfoRecord* r = data_.career.cars.Find(carId);
    if (!r) return 0;
    const int i = r->PaintIndex(uint8_t(paintId));
    return i < 0 ? 0 : i;
}

std::optional<CarCatalogueRow> MenuRuntime::Catalogue(uint32_t carId) const {
    const std::optional<size_t> row = FindCatalogueRow(data_.career.tables, carId);
    if (!row) return std::nullopt;
    return CarCatalogueAt(data_.career.tables, *row);
}

void MenuRuntime::ShowCarOf(int garageIndex, uint32_t carId, uint32_t modelId, int paintIndex, int kind) { // 0x80014348
    pendingShow_ = PendingShow{garageIndex, carId, modelId, paintIndex, kind};
    showPending_ = kind;
}

void MenuRuntime::DoShowCar(const PendingShow& show, bool resetCamera) { // 0x8001D5C8 -> 0x8001D090 + 0x8001AC20
    if (resetCamera) camera_.Reset();
    const int garageIndex = show.garageIndex, paintIndex = show.paintIndex, kind = show.kind;
    const uint32_t carId = show.carId, modelId = show.modelId;
    const career::CareerState& s = actions_.Career();
    const CarParamTables& t = data_.career.tables;
    car_ = CarView{};
    car_.shown = true;
    car_.carId = carId;
    car_.modelId = modelId;
    car_.paintIndex = paintIndex;
    const CarInfoRecord* info = data_.career.cars.Find(carId);
    car_.paintCount = info ? int(info->PaintCount()) : 0;
    car_.garageIndex = garageIndex;
    car_.current = garageIndex >= 0 && garageIndex == s.garage.currentCar;
    const std::optional<CarCatalogueRow> cat = Catalogue(carId);
    car_.year = cat ? cat->year : 0;
    car_.price = cat ? int32_t(cat->price) : 0;
    try {
        CarConfig config{};
        sim::CarParams record{};
        uint8_t figures[0x6C] = {};
        if (garageIndex < 0) { // 0x8001AFA8: the catalogue configuration
            const std::optional<CarConfig> c = CatalogueCarConfig(t, carId);
            if (!c) throw std::runtime_error("not in the catalogue");
            config = *c;
        } else {                // 0x8001B10C: the garage car's configuration and its built record
            config = s.garage.cars[garageIndex].config;
            if (config.word00 & 0xFFFFE0FFu) car_.wheelId = config.word00; // 0x8001AC20: bought wheels (0x800615E8)
        }
        const auto& chassis = t.RowAs<ChassisRow>(3, config.chassis);
        const auto& body = t.RowAs<RacingModifyRow>(5, config.racingModify);
        const std::span<const uint8_t> engine = t.Row(6, config.engine);
        const auto& drive = t.RowAs<DrivetrainRow>(13, config.drivetrain);
        auto u16 = [](std::span<const uint8_t> r, size_t o) { return int(r[o] | (r[o + 1] << 8)); };
        const std::span<const uint8_t> chassisBytes = t.Row(3, config.chassis);
        car_.length = u16(chassisBytes, 8);
        car_.width = u16(chassisBytes, 0xA);
        car_.height = body.width;
        car_.displacement = u16(engine, 0x2C);
        car_.drive = drive.driveType;
        car_.engineText[0] = u16(engine, 4);
        car_.engineText[1] = u16(engine, 8);
        car_.engineText[2] = u16(engine, 6);
        if (garageIndex < 0) {
            car_.weight = chassis.weightKg;
            car_.power = u16(engine, 0x2E);
            car_.powerRpm = u16(engine, 0x30) * 10;
            car_.torque = u16(engine, 0x32);
            car_.torqueRpm = u16(engine, 0x34);
        } else {
            CarConfig built = config;
            career::BuildMenuRecord(t, built, record); // 0x800771AC
            career::CarPowerFigures(record, figures);  // 0x80075930
            int16_t weight; // record +0x5A (0x8001B10C)
            std::memcpy(&weight, reinterpret_cast<const uint8_t*>(&record) + 0x5A, 2);
            car_.weight = weight;
            auto f16 = [&](size_t o) { return int(figures[o] | (figures[o + 1] << 8)); };
            car_.power = f16(0);
            car_.powerRpm = f16(2);
            car_.torque = f16(4);
            car_.torqueRpm = -1;
            car_.torqueRpmText = Format("%drpm", f16(6)); // 0x8001B10C: "%drpm" of the data-gt block (0x801C3114)
        }
    } catch (const std::exception& e) {
        log.push_back(std::string("  car data unavailable: ") + e.what());
    }
    ShowCarRequest r;
    r.kind = kind;
    r.carId = carId;
    r.modelId = modelId;
    r.paintIndex = paintIndex;
    r.garageIndex = garageIndex;
    actions_.ShowCar(r);
}

bool MenuRuntime::Action(int index, bool cross) { // 0x80014380
    const MenuItem it = page_.items[size_t(index)];
    career::CareerState& s = actions_.Career();
    career::GarageBlock& g = s.garage;
    const int cur = g.currentCar;
    const uint16_t type = it.Type();
    bool result = true, fallthrough = true;
    char text[160];
    std::snprintf(text, sizeof text, "action item %d type 0x%02X flags %08X", index, type, it.flags);
    log.push_back(text);
    auto request = [&](uint32_t page) { // LAB_80014808
        actions_.Sound(kSoundAccept);
        fallthrough = false;
        NewPageRequest(page, 0);
    };
    auto showGarageCar = [&](int i) {
        const career::GarageCar& car = g.cars[i];
        ShowCarOf(i, car.carId, car.modelId, PaintIndexOf(car.modelId, car.paint));
    };
    bool afterBlock = false; // LAB_8001482C reached without a request
    if (it.Has(kAction)) {
        if (type == 0x9C) {
            uint32_t page;
            if (cur < 0) page = 0x80000002u;
            else if (career::PartOwned(g.cars[cur], 0x22)) page = 0x8000001Au;
            else if (racingBodies_ <= 0) page = 0x80000019u; // 0x800174AC
            else {
                page = it.Target();
                const uint32_t body = actions_.RacingBody(true); // 0x800174F4: body 1, shown (0x80014348 kind 1, no garage slot)
                ShowCarOf(-1, body, body, 0);
            }
            request(page);
        } else if (type == 0xAE) {
            if (cur < 0) {
                transaction_.state = Transaction::kNone;
                request(0x80000002u);
            } else if (std::find(data_.unqualifiedCars.begin(), data_.unqualifiedCars.end(), g.cars[cur].carId) != data_.unqualifiedCars.end()) { // 0x80018210
                transaction_.state = Transaction::kNone;
                request(0x8000001Eu);
            } else {
                request(it.Target());
            }
        } else if (type == 0xAD) {
            if (cur < 0) {
                transaction_.state = Transaction::kNone;
                request(0x80000002u);
            } else {
                showGarageCar(cur);
                request(it.Target());
            }
        } else if (type == 0xBB) {
            afterBlock = true;
        } else if (type == 9) {
            if (popup_ != 0) {
                if (popup_ == 1 && garageList_) {
                    popup_ = 0;
                    const int slot = garageList_->Selection();
                    showGarageCar(slot);
                    transaction_.selectIndex = slot;                 // the selection 0x800A8D62 the types 0xAA / 0xAB act on
                    transaction_.sellIndex = slot;
                    garageSelection_ = slot;
                    request(it.Target());                            // 0x80017288: the sheet of that car (the page's data)
                } else {
                    request(it.Target());
                }
            } else {
                if (!garageList_ || garageList_->Count() < 1) actions_.Sound(kSoundBuzzer);
                else {
                    popup_ = 1;
                    actions_.Sound(kSoundAccept);
                }
                afterBlock = true;
            }
        } else if (type == 0x50) {
            if (popup_ != 0) {
                if (popup_ == 2 && usedList_) {
                    popup_ = 0;
                    const uint32_t car = usedList_->CarId();
                    ShowCarOf(-1, car, car, PaintIndexOf(car, uint32_t(usedList_->PaintId())));
                    transaction_ = Transaction{}; // 0x8001DAA8(trans, car, lot price)
                    transaction_.state = Transaction::kBuyCar;
                    transaction_.carId = car;
                    transaction_.price = usedList_->Price() ? usedList_->Price() : career::CataloguePrice(data_.career.tables, car);
                }
                request(it.Target());
            } else {
                if (!usedList_ || usedList_->Count() < 1) actions_.Sound(kSoundBuzzer);
                else {
                    popup_ = 2;
                    actions_.Sound(kSoundAccept);
                }
                afterBlock = true;
            }
        } else if (type == 0x98) {
            if (cur < 0) {
                transaction_.state = Transaction::kNone;
                request(0x80000002u);
            } else if (g.cars[cur].config.byte79 != page_.Maker()) { // slot +0x81 = config +0x79 (the maker)
                transaction_.state = Transaction::kNone;
                request(0x8000001Du);
            } else {
                request(it.Target());
            }
        } else {
            request(it.Target());
        }
    } else {
        afterBlock = true;
    }
    (void)afterBlock;
    // LAB_8001482C: event items (bit 27 without the display bits 19..23) and wheel items (bit 28).
    const uint32_t f = it.flags;
    if ((f & kEvent) && !(f & (kEventTrophy | kEventPrize | kEventPower | kEventLicence))) {
        const std::string name = it.Name();
        if (!(f & kRacePath3)) {
            uint32_t message = 0;
            const int32_t r = actions_.EntryCheck(name, message);
            if (r == 1) {
                actions_.Sound(kSoundAccept);
                actions_.StartRace(name, 2);
                log.push_back("  race " + name + " (0x801EF5F5 = 2)");
            } else if (r == MenuActions::kNotAvailable) {
                actions_.Sound(kSoundBuzzer);
            } else {
                actions_.Sound(kSoundBuzzer);
                std::snprintf(text, sizeof text, "  entry refused: %d, message 0x%08X", r, message);
                log.push_back(text);
                NewPageRequest(message, 0);
            }
        } else {
            actions_.Sound(kSoundAccept);
            actions_.StartRace(name, 3);
            log.push_back("  race " + name + " (0x801EF5F5 = 3, no entry check)");
        }
        fallthrough = false;
    }
    if (f & kWheel) { // the wheel id of the code, its colour for the current car, transaction 5 at 2000 (0x8001DB0C)
        fallthrough = false;
        const uint32_t wheel = actions_.WheelId(it.Name());
        const int colour = cur >= 0 ? actions_.WheelColour(wheel) : 0;
        actions_.Sound(kSoundAccept);
        transaction_.state = Transaction::kWheels;
        transaction_.wheel = wheel;
        transaction_.price = 2000;
        car_.wheelId = wheel; // view +0x1CC = 3: 0x8001AEF8 puts the wheels on the displayed car at the next draw
        car_.wheelColour = colour;
        car_.wheelPreview = true;
        holdOff_ = 8;
        std::snprintf(text, sizeof text, "  wheel %s -> id 0x%08X colour %d", it.Name().c_str(), wheel, colour);
        log.push_back(text);
    }
    int sound = 1;
    switch (type) {
    case 2: {
        // 0x8001A454: the paint of the displayed car ('-' = 0x2D when none).
        transaction_.paint = 0x2D;
        if (car_.shown)
            if (const CarInfoRecord* r = data_.career.cars.Find(car_.carId); r && car_.paintIndex < int(r->paintIds.size()))
                transaction_.paint = r->paintIds[size_t(car_.paintIndex)];
        uint32_t page = actions_.RunTransaction(transaction_);
        if (page == 0) {
            backAllowed_ = 1;
            page = back_;
        }
        NewPageRequest(page, 0);
        break;
    }
    case 3: // 0x8001DD3C
        switch (transaction_.state) {
        case 1: case 2: transaction_.state = 1; break;
        case 3: case 4: transaction_.state = 3; break;
        case 5: transaction_.state = 5; break;
        case 6: transaction_.state = 6; break;
        default: break;
        }
        backAllowed_ = 1;
        NewPageRequest(back_, 0);
        break;
    case 4: {
        const uint32_t page = actions_.CheckTransaction(transaction_);
        if (page == 0) {
            sound = 0;
            result = false;
        } else {
            backAllowed_ = 0;
            NewPageRequest(page, 0);
        }
        break;
    }
    case 5: { // 0x8001D698: the car page of the displayed car (solodata)
        uint32_t page = 0;
        if (car_.shown)
            for (const auto& [carId, value] : pages_.Solo().cars)
                if (carId == car_.carId) page = value;
        if (page == 0) {
            sound = 0;
            result = false;
        } else {
            NewPageRequest(page, 0);
        }
        break;
    }
    case 6: // 0x8001DAA8 at the catalogue price, the car shown
        transaction_ = Transaction{};
        transaction_.state = Transaction::kBuyCar;
        transaction_.carId = it.CarId();
        transaction_.price = career::CataloguePrice(data_.career.tables, it.CarId());
        ShowCarOf(-1, it.CarId(), it.CarId(), 0);
        return result;
    case 9:
    case 0x50: return result;
    case 0x0B:
        actions_.Exit();
        log.push_back("  exit GT mode (0x801EF5F5 = 0)");
        return result;
    case 0x0D: // car wash: 50 credits when the car is dirty (slot +0xA2)
        if (g.money > 0x31 && cur >= 0 && g.cars[cur].wordA2 != 0) {
            g.money -= 50;
            popup_ = 3;
            washTimer_ = 0x40; // 0x8001D104
        } else {
            sound = 0;
            result = false;
        }
        break;
    case 0x10: { // 0x8001DCD0
        backAllowed_ = 1;
        bool previous = false;
        switch (transaction_.state) {
        case 1: case 2: transaction_.state = 1; break;
        case 3: case 4: transaction_.state = 3; break;
        case 5: transaction_.state = 5; break;
        case 6: previous = true; break;
        case 7: transaction_.state = 7; break;
        default: break;
        }
        if (previous) NewPageRequest(backBefore_, 1);
        else NewPageRequest(back_, 0);
        break;
    }
    case 0x12: NewPageRequest(back_, 1); break;
    case 0x13: NewPageRequest(0x8000002Eu, 0); break;
    case 0x95:
    case 0x96: // 0x8001A530: the displayed paint +1 (cross) / -1 (circle)
        if (car_.shown && car_.paintCount > 0) {
            car_.paintIndex = ((car_.paintIndex + (cross ? 1 : -1)) % car_.paintCount + car_.paintCount) % car_.paintCount;
            ShowCarRequest r;
            r.carId = car_.carId;
            r.modelId = car_.modelId;
            r.paintIndex = car_.paintIndex;
            r.garageIndex = car_.garageIndex;
            actions_.ShowCar(r);
        }
        break;
    case 0x9D: { // 0x80017530: the next racing-modification body, shown after the delay of kind 3
        const uint32_t body = actions_.RacingBody(false);
        ShowCarOf(-1, body, body, 0, 3);
        holdOff_ = 4;
        break;
    }
    case 0xAA: // sell the car chosen in the garage list
        transaction_ = Transaction{};
        transaction_.state = Transaction::kSell;
        transaction_.sellIndex = garageSelection_;
        transaction_.price = 0; // 0x80017A68 returns 0
        NewPageRequest(actions_.CheckTransaction(transaction_), 0);
        backAllowed_ = 0;
        break;
    case 0xAB: // make it the current car
        transaction_ = Transaction{};
        transaction_.state = Transaction::kSelect;
        transaction_.selectIndex = garageSelection_;
        NewPageRequest(actions_.CheckTransaction(transaction_), 0);
        backAllowed_ = 0;
        break;
    default:
        if (type >= 0x14 && type <= 0x46) { // buy part kind (type - 0x14) of the current car
            transaction_.state = Transaction::kBuyPart;
            transaction_.partKind = type - 0x14;
            transaction_.partSlot = cur;
            transaction_.price = cur >= 0 ? career::PartPrice(data_.career.tables, g.cars[cur].carId, type - 0x14) : 0;
            NewPageRequest(actions_.CheckTransaction(transaction_), 0);
            backAllowed_ = 0;
            break;
        }
        result = false;
        if (!fallthrough) return false;
        actions_.Sound(kSoundBuzzer);
        return false;
    }
    actions_.Sound(sound);
    return result;
}

void MenuRuntime::Update(const PadState& pad) {
    // 0x80013EEC
    if (pageCountdown_ > 0) pageCountdown_--;
    if (pageCountdown_ == 0 && showPending_ == 0) { // 0x8001D258: the car view turns (the wash spins it faster)
        const bool washing = washTimer_ > 0;
        if (washing && --washTimer_ < 1) washTimer_ = 0;
        camera_.Update(washing);
    }
    if (pageCountdown_ == 0 && showPending_ == 0) { // the rest of 0x80013EEC is skipped as well while a car is pending
        const bool input = holdOff_ < 1;
        switch (popup_) {
        case 0:
            CursorUpdate(input ? &pad : nullptr);
            if (garageList_) garageList_->Update(nullptr, false);
            if (usedList_) usedList_->Update(nullptr, false);
            break;
        case 1:
        case 2: {
            CursorUpdate(nullptr);
            PopupList* list = popup_ == 1 ? garageList_.get() : usedList_.get();
            const int r = list ? list->Update(input ? &pad : nullptr, true) : -2;
            if (r == -2) {
                popup_ = 0;
                actions_.Sound(kSoundBack);
            } else if (r != -1) {
                for (size_t i = 0; i < page_.items.size(); i++)
                    if (page_.items[i].Type() == (popup_ == 1 ? 9 : 0x50)) {
                        Action(int(i), false);
                        break;
                    }
            }
            break;
        }
        case 3:
            CursorUpdate(nullptr);
            if (washTimer_ == 0) {
                career::GarageBlock& g = actions_.Career().garage;
                if (g.currentCar >= 0) g.cars[g.currentCar].wordA2 = 0;
                popup_ = 0;
                actions_.Sound(kSoundAccept);
            }
            break;
        default: break;
        }
        if (holdOff_ > 0) holdOff_--;
    }
    // 0x8001B3AC: the requested page is loaded when the countdown reaches 1 (a pending car is shown with it); else
    // a pending car is shown when its delay (view +0x1B0) runs out.
    if (pageCountdown_ == 1) {
        LoadPage(nextPage_, nextArgument_);
        if (showPending_ > 0) DoShowCar(pendingShow_, true);
        showPending_ = 0;
    } else if (pageCountdown_ == 0 && showPending_ != 0) {
        if (showPending_ == 1) {
            DoShowCar(pendingShow_, true);
            showPending_ = 0;
        } else {
            showPending_--;
        }
    }
    // 0x800174D0 of the current car's sheet (type 0x9D / 0x9C).
    if (CareerMenuActions* c = dynamic_cast<CareerMenuActions*>(&actions_)) racingBodies_ = RacingBodyCount(c->CurrentSheet().racingModifyRow);
}

// ---------------------------------------------------------------- dynamic texts (0x8001B9AC)

bool MenuRuntime::DrawDynamic(const MenuAssets& assets, const MenuItem& it, std::vector<MenuPrim>& out) const {
    const career::CareerState& s = actions_.Career();
    const career::GarageBlock& g = s.garage;
    const GuestImage& o4 = assets.ovl4;
    MenuTextWriter text(assets.fonts, out);
    const bool second = it.Has(kSecondFont);
    const uint16_t type = it.Type();
    auto W = MenuTextWriter::Widen;
    auto sprite = [&](uint32_t address) { out.push_back(MenuCentredSprite(ReadMenuTableSprite(o4, address), it.CenterX(), it.CenterY())); };
    auto left8 = [&](const std::string& t, uint32_t colour = kTextColour) { text.Left(W(t), true, second, it.x0, it.y1, colour); };      // 0x8001FC64
    auto right8 = [&](const std::string& t) { text.Right(W(t), true, second, it.x1, it.y1, kTextColour); };                            // 0x8001FCA0
    auto rightWide = [&](const std::string& t) { text.Right(W(t), false, second, it.x1, it.y1, kTextColour); };                        // 0x8001FC28
    auto global = [&](uint32_t a) { const auto f = data_.globalFormats.find(a); return f == data_.globalFormats.end() ? std::string() : f->second; };
    if (type >= 0x53 && type <= 0x93) { // licence test medals
        for (const auto& grp : kMedalGroups)
            if (type >= grp.first && type < grp.first + 10) {
                const int medal = int(int8_t(s.licences[grp.licence][type - grp.first].passed)) - 1;
                if (medal >= 0) sprite(kMedalSprites + uint32_t(medal) * 12);
                return true;
            }
        return false;
    }
    switch (type) {
    case 0x00:
        if (it.Has(kLicenceGated)) { // the badge when the gate's licence is held
            const int l = it.Byte4A() == 0 ? 1 : it.Byte4A() == 1 ? 0 : -1;
            if (l >= 0 && career::LicenceHeld(s, l)) sprite(kBadgeSprite);
        }
        return true;
    case 0x07: // 0x8001A4AC: the colour name of the displayed paint (right-aligned, wide)
        if (car_.shown && data_.colours) {
            const int32_t ci = data_.career.cars.IndexOf(car_.carId);
            const CarInfoRecord* r = data_.career.cars.Find(car_.carId);
            if (ci >= 0 && r) {
                const std::vector<uint16_t> names = data_.colours->NameIndices(size_t(ci), r->PaintCount());
                if (car_.paintIndex < int(names.size())) rightWide(data_.colours->Name(names[size_t(car_.paintIndex)]));
            }
        }
        return true;
    case 0x47: { // 0x8001B818(carId, x0, y1, left, prefix = slot +0x98 bit 15, no grade)
        if (g.currentCar < 0) return true;
        const career::GarageCar& car = g.cars[g.currentCar];
        int x = it.x0;
        if (car.powerFlags & 0x8000) x += text.Left(data_.racingPrefix, false, second, x, it.y1, kTextColour);
        const std::optional<CarCatalogueRow> cat = Catalogue(car.carId);
        text.Left(cat ? data_.String(cat->modelName) : data_.noName, false, second, x, it.y1, kTextColour);
        return true;
    }
    case 0x48: // power before the part (0x801C309C), "%dhp" (0x8001FFCC) right-aligned, colour 0x145A78
        text.Right(W(Format(global(kGlobalHp), (transaction_.powerBefore * 1000) / 0x3F6)), false, second, it.x1, it.y1, kPowerColour);
        return true;
    case 0x49: // power after (0x801C30A0): "----" left when there is no such part
        if (transaction_.powerAfter < 0) text.Left(W(ReadAscii(o4, kFormatNone)), true, second, it.x0, it.y1, kPowerColour);
        else text.Right(W(Format(global(kGlobalHp), (transaction_.powerAfter * 1000) / 0x3F6)), false, second, it.x1, it.y1, kPowerColour);
        return true;
    case 0x4A: left8(Format(global(kGlobalHp), (car_.power * 1000) / 0x3F6)); return true;
    case 0x4C: left8(Format(global(kGlobalLb), (car_.weight * 0x561E) / 10000)); return true;
    case 0x4E: {
        const Transaction& t = transaction_;
        if (t.state == Transaction::kBuyCar) rightWide(MenuThousands(uint32_t(t.price)));
        else if (t.state == Transaction::kWheels) {
            if (t.price >= 1) rightWide(MenuThousands(uint32_t(t.price)));
        } else if (t.owned != 0) left8(assets.String(kTxdPurchased), 0xF05028);
        else if (t.price >= 0) rightWide(MenuThousands(uint32_t(t.price)));
        else left8(assets.String(kTxdNotAvailable), 0x2850F0);
        return true;
    }
    case 0x94: { // 0x8001A708(view, ot, x1, y0): one chip per paint of the displayed car, right to left from x1
        if (!car_.shown) return true;
        const CarInfoRecord* r = data_.career.cars.Find(car_.carId); // 0x80060BEC: the chip colours
        if (!r) return true;
        int x = it.x1;
        const int y = it.y0;
        for (size_t i = 0; i < r->chipColors.size(); i++) {
            const uint32_t c = r->chipColors[i];
            const uint32_t colour = ((c & 0x1F) << 3) | ((c & 0x3E0) << 6) | ((c & 0xF800) << 9); // as the original converts it
            const int left = x - 12;
            MenuPrim chip; // 0x8006B6E4: gouraud quad black -> chip colour, semi-transparent by the E1 0x220 below
            chip.kind = MenuPrim::kPolyG4;
            const int xs[4] = {left, left + 10, left, left + 10}, ys[4] = {y, y, y + 8, y + 8};
            for (int k = 0; k < 4; k++) chip.x[k] = int16_t(xs[k]), chip.y[k] = int16_t(ys[k]);
            chip.colour[0] = chip.colour[2] = 0;
            chip.colour[1] = chip.colour[3] = colour & 0xFFFFFF;
            chip.semi = true;
            chip.dither = true;
            chip.tpage = 0x220; // 0x8007DA44(ot, 0x220): additive, dither
            out.push_back(chip);
            MenuPrim back; // 0x8007D024: black TILE under the chip
            back.kind = MenuPrim::kTile;
            back.x[0] = int16_t(left), back.y[0] = int16_t(y), back.w = 10, back.h = 8;
            back.colour[0] = 0;
            out.push_back(back);
            // 0x8007E738: 5-point polyline around it, bright for the displayed paint
            const uint32_t grey = int(i) == car_.paintIndex ? 0xB4u : 0x46u;
            const int px[5] = {x - 13, x - 2, x - 2, x - 13, x - 13}, py[5] = {y - 1, y - 1, y + 8, y + 8, y - 1};
            for (int k = 0; k < 4; k++) {
                MenuPrim line;
                line.kind = MenuPrim::kLine;
                line.x[0] = int16_t(px[k]), line.y[0] = int16_t(py[k]), line.x[1] = int16_t(px[k + 1]), line.y[1] = int16_t(py[k + 1]);
                line.colour[0] = line.colour[1] = grey | (grey << 8) | (grey << 16);
                line.tpage = 0x220;
                out.push_back(line);
            }
            x = left;
        }
        return true;
    }
    case 0x97: sprite(kDriveSprites + uint32_t(car_.drive) * 12); return true;
    case 0x9A:
        left8(car_.year < 1 ? Format(ReadAscii(o4, kFormatNone), car_.year) : Format(ReadAscii(o4, kFormatYear), car_.year));
        return true;
    case 0xB2: left8(Format(assets.String(kTxdMm), car_.length)); return true;
    case 0xB3: left8(Format(assets.String(kTxdMm), car_.width)); return true;
    case 0xB4: left8(Format(assets.String(kTxdMm), car_.height)); return true;
    case 0xB5: left8(Format(global(kGlobalLb), (car_.weight * 0x561E) / 10000)); return true;
    case 0xB6:
        if (car_.displacement < 1) left8(Format(ReadAscii(o4, kFormatNone), car_.displacement));
        else if (((car_.displacement & 0xE000) >> 13) == 0) left8(Format(assets.String(kTxdCc), car_.displacement));
        else return false; // the rotary "%dx%dcc" form: its second argument is not traced
        return true;
    case 0xB7: {
        const uint32_t p = o4.Get<uint32_t>(kDriveNames + uint32_t(car_.drive) * 4);
        left8(assets.String(p));
        return true;
    }
    case 0xB9:
        if (car_.power < 1) left8(ReadAscii(o4, kFormatNone));
        else if (car_.powerRpm < 1) left8(Format(assets.String(kTxdHp), (car_.power * 1000) / 0x3F6, car_.powerRpm));
        else left8(Format(assets.String(kTxdPowerRpm), (car_.power * 1000) / 0x3F6, car_.powerRpm));
        return true;
    case 0xB8: { // engine texts (0x8001B2B8 / 0x8001B338): view +0x1C, then +0x24 12 px after it; "" without a car
        const std::u16string a = car_.shown ? data_.String(uint16_t(car_.engineText[0])) : std::u16string();
        const std::u16string b = car_.shown ? data_.String(uint16_t(car_.engineText[2])) : std::u16string();
        const int w = text.Left(a, false, second, it.x0, it.y1, kTextColour);
        text.Left(b, false, second, it.x0 + w + 12, it.y1, kTextColour);
        return true;
    }
    case 0xBA: { // torque: "%d.%dlb-ft / " of kgm x 10 * 72329 / 10000, then the rpm text (0x8001B378)
        if (car_.torque < 1) {
            left8(ReadAscii(o4, kFormatNone));
            return true;
        }
        const int v = (car_.torque * 72329) / 10000;
        const int w = text.Left(W(Format(assets.String(kTxdTorque), v / 10, v % 10)), true, second, it.x0, it.y1, kTextColour);
        const std::u16string rpm = car_.torqueRpm >= 0 ? data_.String(uint16_t(car_.torqueRpm)) : W(car_.torqueRpmText);
        text.Left(rpm, false, second, it.x0 + w, it.y1, kTextColour);
        return true;
    }
    case 0xBC: { // 0x80019D38: events won (result nibble 1) of the menu's 248, per cent of 219, "%d.%02d"
        const int32_t events = int32_t(data_.events.events.size());
        int won = 0;
        for (int32_t i = 0; i < events; i++) won += career::ResultAt(s.record, i) == 1;
        const int v = (won * 10000) / (events - 29);
        right8(Format(ReadAscii(o4, kFormatFraction), v / 100, v % 100));
        return true;
    }
    case 0xBD: rightWide(MenuHundredMillions(s.record.prizeCarry, s.record.prizeTotal)); return true; // 0x80019DF8 + 0x8001FE0C
    case 0xBE: right8(Format(ReadAscii(o4, kFormatInt), int(s.record.word48))); return true;
    case 0xBF: right8(Format(ReadAscii(o4, kFormatInt), s.record.wins)); return true;
    case 0xC0: { // 0x80019C80: wins (at most 0x68DB8) per race total (+0x48), "%d.%02d"
        const uint32_t w = std::min<uint32_t>(uint32_t(s.record.wins), 0x68DB8u);
        const uint32_t v = s.record.word48 ? (w * 10000u) / s.record.word48 : 0;
        right8(Format(ReadAscii(o4, kFormatFraction), int(v / 100), int(v % 100)));
        return true;
    }
    case 0xC1: { // 0x80019CF4: mean position x 100
        const uint32_t v = s.record.races ? (uint32_t(s.record.positionSum) * 100u) / uint32_t(s.record.races) : 0;
        right8(Format(ReadAscii(o4, kFormatFraction), int(v / 100), int(v % 100)));
        return true;
    }
    case 0xC2: right8(Format(ReadAscii(o4, kFormatInt), g.count)); return true;
    case 0xC3: { // 0x80019E2C: the garage's value (slot +0x90), hundreds of millions carried once
        int32_t hi = 0, sum = 0;
        for (int i = 0; i < g.count && i < career::kGarageCapacity; i++) sum += g.cars[i].value;
        if (sum > 100000000) {
            hi++;
            sum -= 100000000;
        }
        rightWide(MenuHundredMillions(hi, uint32_t(sum)));
        return true;
    }
    case 0xCD: { // 0x80017318(ot, 0x20, 0xA0): the fitted parts of the chosen garage car, entries 0x80050AB0
        std::array<int16_t, 27> stages{};
        if (!actions_.CarStages(garageSelection_, stages)) return true;
        for (uint32_t e = kEquippedParts;; e += 8) {
            const int16_t key = o4.Get<int16_t>(e), base = o4.Get<int16_t>(e + 2), dx = o4.Get<int16_t>(e + 4), dy = o4.Get<int16_t>(e + 6);
            if (key < 0 || key >= int16_t(stages.size())) break;
            if (stages[size_t(key)] > 0) text.Left(data_.String(uint16_t(base + stages[size_t(key)] - 1)), false, false, 0x20 + dx, 0xA0 + dy, 0x606060);
        }
        return true;
    }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: { // 0x80019EF8(n): tests with result n (C8 -> 4, C9 -> 3, CA -> 2, CB -> 1)
        const int n = type == 0xC8 ? 4 : type == 0xC9 ? 3 : type == 0xCA ? 2 : 1;
        int count = 0;
        for (size_t l = 0; l < career::kLicenceCount; l++)
            for (size_t k = 0; k < career::kLicenceTests; k++) count += int8_t(s.licences[l][k].passed) == n;
        right8(Format(ReadAscii(o4, kFormatInt), count));
        return true;
    }
    case 0xCC: {
        const int level = career::LicenceLevel(s);
        if (level < 6) sprite(kLicenceLevelSprites + uint32_t(level) * 12);
        return true;
    }
    case 0xD0: rightWide(MenuThousands(uint32_t(car_.price) >> 2)); return true;
    default: return false;
    }
}

bool MenuRuntime::DrawLate(const MenuAssets& assets, const MenuItem& it, std::vector<MenuPrim>& out) const {
    switch (it.Type()) {
    case 0x07: // 0x8001A4AC's text and 0x8001A708's chips go to view +0x88 (param_4 of 0x8001B9AC)
    case 0x94: return DrawDynamic(assets, it, out);
    case 0x11: // 0x8001A654: the name logo at the item's centre (loaded by 0x8001AC20 with the car)
        if (car_.shown && logo_) out.push_back(MenuCarLogoSprite(*logo_, it.CenterX(), it.CenterY()));
        return true;
    default: return false;
    }
}

MenuRenderState MenuRuntime::RenderState(const MenuAssets& assets) const {
    MenuRenderState st;
    const career::CareerState& s = actions_.Career();
    st.money = s.garage.money;
    st.day = s.record.days;
    st.carPrice = [this](uint32_t id) -> std::optional<uint32_t> { return uint32_t(career::CataloguePrice(data_.career.tables, id)); };
    st.eventInfo = [this](const std::string& name) -> std::optional<MenuRenderState::EventInfo> {
        const int32_t i = career::MenuEventIndex(data_.events, name); // 0x800188B0 (0 when absent, like the original)
        if (i < 0 || size_t(i) >= data_.eventInfos.infos.size()) return std::nullopt;
        const career::EventInfo& e = data_.eventInfos.infos[size_t(i)];
        MenuRenderState::EventInfo m;
        m.bonus = e.bonus;
        for (size_t k = 0; k < 6; k++) m.prize[k] = e.prize[k];
        m.powerLimit = e.powerLimit;
        const int need = (e.rules & 0xE) >> 1; // 0x80019634
        m.licence = (need >= 1 && need <= 6) ? 6 - need : -1;
        return m;
    };
    st.eventResult = [this](const std::string& name) { if (gt2::pc::unlockSimulationEvents) return 1; return int(career::ResultAt(actions_.Career().record, career::MenuEventIndex(data_.events, name))); };
    st.dynamicItem = [this, &assets](const MenuItem& item, std::vector<MenuPrim>& out) { return DrawDynamic(assets, item, out); };
    st.lateItem = [this, &assets](const MenuItem& item, std::vector<MenuPrim>& out) { return DrawLate(assets, item, out); };
    st.customItem = [this](const MenuItem& item, std::vector<MenuPrim>& out) {
        if (item.Type() == 9 && garageList_) {
            garageList_->Draw(item, out);
            return true;
        }
        if (item.Type() == 0x50 && usedList_) {
            usedList_->Draw(item, out);
            return true;
        }
        return false;
    };
    st.cursor = true;
    st.cursorAt = std::array<int, 2>{cursor_.x, cursor_.y};
    st.cursorOnItem = cursor_.item >= 0;
    return st;
}

} // namespace gt2::menu
