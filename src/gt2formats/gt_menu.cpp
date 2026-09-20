#include "gt2formats/gt_menu.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

#include "gt2vfs/gtfs.h"

namespace gt2 {
namespace {

uint16_t U16(std::span<const uint8_t> b, size_t o) {
    if (o + 2 > b.size()) throw std::runtime_error("GM page: read beyond the end");
    return uint16_t(b[o] | (b[o + 1] << 8));
}
uint32_t U32(std::span<const uint8_t> b, size_t o) { return uint32_t(U16(b, o)) | (uint32_t(U16(b, o + 2)) << 16); }

MenuSprite ReadSprite(std::span<const uint8_t> b, size_t o) {
    MenuSprite s;
    s.x = int16_t(U16(b, o));
    s.y = int16_t(U16(b, o + 2));
    s.u = b[o + 4];
    s.v = b[o + 5];
    s.w = b[o + 6];
    s.h = b[o + 7];
    s.tpage = U16(b, o + 8);
    s.clut = U16(b, o + 10);
    return s;
}

MenuItem ReadItem(std::span<const uint8_t> b, size_t o, int group) {
    if (o + MenuItem::kSize > b.size()) throw std::runtime_error("GM page: item beyond the end");
    MenuItem it;
    it.x0 = int16_t(U16(b, o));
    it.y0 = int16_t(U16(b, o + 2));
    it.x1 = int16_t(U16(b, o + 4));
    it.y1 = int16_t(U16(b, o + 6));
    it.flags = U32(b, o + 8);
    std::memcpy(it.data.data(), b.data() + o + 0x0C, it.data.size());
    it.group = group;
    return it;
}

std::string Hex(uint32_t v, int digits = 2) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%0*X", digits, v);
    return buf;
}

} // namespace

uint32_t MenuItem::Word(size_t itemOffset) const {
    if (itemOffset < 0x0C || itemOffset + 4 > kSize) throw std::out_of_range("MenuItem::Word");
    const uint8_t* p = data.data() + (itemOffset - 0x0C);
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

std::string MenuItem::Name() const {
    size_t n = 0;
    while (n < data.size() && data[n] != 0) n++;
    return std::string(reinterpret_cast<const char*>(data.data()), n);
}

MenuSprite MenuItem::AltSprite() const {
    return ReadSprite(std::span<const uint8_t>(data.data(), data.size()), 0x10 - 0x0C);
}

const MenuItem* MenuPage::DefaultItem() const {
    for (const MenuItem& it : items)
        if (it.Has(menu_item_flag::kDefault)) return &it;
    return nullptr;
}

MenuPage ParseMenuPage(std::span<const uint8_t> b, uint32_t id) {
    if (b.size() < 8 || std::memcmp(b.data(), "GM\x03\0", 4) != 0) throw std::runtime_error("GM page: bad magic");
    MenuPage p;
    p.id = id;
    const uint32_t groups = U32(b, 4);
    if (groups > 64) throw std::runtime_error("GM page: implausible group count");
    size_t o = 8;
    for (uint32_t g = 0; g < groups; g++) {
        MenuGroup group;
        const uint16_t sprites = U16(b, o), items = U16(b, o + 2);
        o += 4;
        for (uint16_t s = 0; s < sprites; s++, o += 12) group.sprites.push_back(ReadSprite(b, o));
        for (uint16_t i = 0; i < items; i++, o += MenuItem::kSize) p.items.push_back(ReadItem(b, o, int(g)));
        group.itemCount = items;
        p.groups.push_back(std::move(group));
    }
    const uint32_t count = U32(b, o);
    o += 4;
    if (count > 64) throw std::runtime_error("GM page: implausible item count");
    for (uint32_t i = 0; i < count; i++, o += MenuItem::kSize) p.items.push_back(ReadItem(b, o, -1));
    if (p.items.size() > 64) p.items.resize(64); // 0x8002150C copies at most 64 records
    p.flags = U32(b, o);
    p.back = U32(b, o + 4);
    p.picture = U32(b, o + 8);
    o += 12;
    MenuPagePicture& pic = p.own;
    pic.magic = U32(b, o);
    const uint32_t k = U32(b, o + 4);
    o += 8;
    if (k > 0x9FF) throw std::runtime_error("GM page: too many picture tiles"); // the copy at 0x80048430 holds 0x2800 bytes
    for (uint32_t i = 0; i < k; i++, o += 4) pic.tiles.push_back(U32(b, o));
    for (size_t i = 0; i < pic.clut.size(); i++) pic.clut[i] = U16(b, o + 2 * i);
    o += pic.clut.size() * 2;
    pic.textureTiles = U32(b, o);
    o += 4;
    const size_t rows = size_t(pic.textureTiles / 32 + 1) * 8; // stored: one strip more when n is a multiple of 32
    if (o + rows * 256 != b.size()) throw std::runtime_error("GM page: picture size does not match the page size");
    pic.pixels.assign(b.begin() + ptrdiff_t(o), b.end());
    return p;
}

std::string MenuItemTypeName(uint16_t t) {
    if (t >= 0x14 && t <= 0x46) return "buy part kind " + Hex(t - 0x14u);
    struct Licence { uint16_t first; int licence; };
    for (const Licence l : {Licence{0x53, 3}, Licence{0x5E, 5}, Licence{0x69, 4}, Licence{0x74, 2}, Licence{0x7F, 1}, Licence{0x8A, 0}})
        if (t >= l.first && t < l.first + 10) return "licence " + std::to_string(l.licence) + " test " + std::to_string(t - l.first + 1) + " medal";
    switch (t) {
    case 0x00: return "button";
    case 0x01: return "tab button";
    case 0x02: return "yes (run transaction)";
    case 0x03: return "no (cancel transaction)";
    case 0x04: return "check transaction";
    case 0x05: return "car page (solodata)";
    case 0x06: return "new car (dealer) + price";
    case 0x07: return "setting value";
    case 0x09: return "garage car list popup";
    case 0x0A: return "3D car viewport";
    case 0x0B: return "exit GT mode";
    case 0x0D: return "car wash";
    case 0x10: return "cancel + back";
    case 0x11: return "car name logo";
    case 0x12: return "back";
    case 0x13: return "message 0x2E";
    case 0x47: return "current car name";
    case 0x48: return "power before (hp)";
    case 0x49: return "power after (hp)";
    case 0x4A: return "power (hp)";
    case 0x4C: return "weight (lb)";
    case 0x4D: return "money";
    case 0x4E: return "transaction price";
    case 0x4F: return "days";
    case 0x50: return "used-car list popup";
    case 0x94: return "paint chips";
    case 0x95: return "paint +1";
    case 0x96: return "paint -1";
    case 0x97: return "drive-train icon";
    case 0x98: return "shop of the car's maker";
    case 0x9A: return "model year";
    case 0x9B: return "paint name";
    case 0x9C: return "racing modification";
    case 0x9D: return "show car (paint list)";
    case 0xAA: return "sell car";
    case 0xAB: return "select car";
    case 0xAD: return "page with current car";
    case 0xAE: return "page if car qualifies";
    case 0xB2: return "length (mm)";
    case 0xB3: return "width (mm)";
    case 0xB4: return "height (mm)";
    case 0xB5: return "weight (lb)";
    case 0xB6: return "displacement (cc)";
    case 0xB7: return "drive train";
    case 0xB8: return "engine text";
    case 0xB9: return "max power (hp / rpm)";
    case 0xBA: return "max torque";
    case 0xBB: return "no-op";
    case 0xBC: return "status percentage";
    case 0xBD: return "status pair";
    case 0xBE: return "status count A";
    case 0xBF: return "status count B";
    case 0xC0: return "status ratio A";
    case 0xC1: return "status ratio B";
    case 0xC2: return "status count C";
    case 0xC3: return "status pair B";
    case 0xC4: return "status best car A";
    case 0xC5: return "status most powerful car";
    case 0xC6: return "status best car B";
    case 0xC8: return "licence count 4";
    case 0xC9: return "licence count 3";
    case 0xCA: return "licence count 2";
    case 0xCB: return "licence count 1";
    case 0xCC: return "licence level icon";
    case 0xCD: return "licence list";
    case 0xCF: return "event bonus";
    case 0xD0: return "value / 4";
    default: return "";
    }
}

std::string MenuItemTypeHandler(uint16_t t) {
    if (t >= 0x14 && t <= 0x46) return "action default: 0x8001DB24 (transaction 3) + check 0x8001DFEC; load: 0x8001DB90";
    switch (t) {
    case 0x02: return "action case 2: 0x8001DDAC";
    case 0x03: return "action case 3: 0x8001DD3C";
    case 0x04: return "action case 4: 0x8001DFEC";
    case 0x05: return "action case 5: 0x8001D698 -> 0x80020F54";
    case 0x06: return "action case 6: 0x8001DAA8 (transaction 1); draw 0x800177D4";
    case 0x07: return "draw 0x8001A4AC";
    case 0x09: return "action 0x80014380 (popup 1); load 0x800204EC; draw 0x8002068C";
    case 0x0A: return "draw 0x8001A8A4";
    case 0x0B: return "action case 0xB (0x801EF5F5 = 0)";
    case 0x0D: return "action case 0xD: 0x80018274, money -= 50";
    case 0x10: return "action case 0x10: 0x8001DCD0";
    case 0x11: return "draw 0x8001A654";
    case 0x12: return "action case 0x12";
    case 0x13: return "action case 0x13";
    case 0x47: return "draw 0x8001B818";
    case 0x4D: return "draw 0x8001FCDC";
    case 0x4F: return "draw 0x80020110";
    case 0x50: return "action 0x80014380 (popup 2); load 0x80020A0C; draw 0x80020B70";
    case 0x94: return "draw 0x8001A708";
    case 0x95: case 0x96: return "action case 0x95/0x96: 0x8001A530";
    case 0x98: return "action: slot +0x81 == page maker, else message 0x1D";
    case 0x9B: return "draw 0x800183B8";
    case 0x9C: return "action: 0x8005E874(kind 0x22), 0x800174AC";
    case 0x9D: return "action case 0x9D: 0x80017530; draw fill if 0x800174D0 < 2";
    case 0xAA: return "action case 0xAA: 0x8001DA38 (transaction 6)";
    case 0xAB: return "action case 0xAB: 0x8001DA98 (transaction 7)";
    case 0xAD: return "action: show car 0x80014348";
    case 0xAE: return "action: 0x80018210";
    case 0xB7: return "draw table 0x80051260";
    case 0xB8: return "draw 0x8001B2B8 / 0x8001B338";
    case 0xBA: return "draw 0x8001B378";
    case 0xBC: return "draw 0x80019D38";
    case 0xBD: return "draw 0x80019DF8";
    case 0xBE: return "draw 0x80019C60";
    case 0xBF: return "draw 0x80019C70";
    case 0xC0: return "draw 0x80019C80";
    case 0xC1: return "draw 0x80019CF4";
    case 0xC2: return "draw 0x80019E1C";
    case 0xC3: return "draw 0x80019E2C";
    case 0xC5: return "draw 0x80019E98";
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: return "draw 0x80019EF8";
    case 0xCC: return "draw 0x800191C4, sprites 0x800509B4";
    case 0xCD: return "draw 0x80017318";
    case 0xCF: return "draw 0x80019718";
    default: return "";
    }
}

bool MenuItemSelectable(const MenuItem& it) {
    using namespace menu_item_flag;
    const uint16_t t = it.Type();
    if (t == 0 && it.Has(kLicenceGated) && it.Byte4A() > 1) return false; // 0x8001B680 (licence held: career state)
    if (it.Has(kAction)) return true;                                      // 0x8001D754 skips 0x8001B6F0 for these
    if (it.Has(kEvent)) return !it.Has(kEventResult | kEventTrophy | kEventPrize | kEventPower | kEventLicence);
    if (t == 0) return true;
    if (t >= 0x14 && t <= 0x46) return true;
    switch (t) {
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0B: case 0x0C:
    case 0x0D: case 0x10: case 0x12: case 0x13: case 0x50: case 0x95: case 0x96: case 0x9D: case 0xAA: case 0xAB:
    case 0xBB:
        return true;
    default: return false;
    }
}

const MenuItem* MenuNearestItem(const MenuPage& page, int x, int y, int direction, const MenuItem* current,
                                bool (*selectable)(const MenuItem&)) {
    static constexpr int16_t kDir[10][2] = {{0, 0}, {0, 0}, {0, -4096}, {0, 4096}, {-4096, 0}, {4096, 0},
                                            {2896, -2896}, {-2896, -2896}, {2896, 2896}, {-2896, 2896}}; // 0x80051274
    if (direction < 2 || direction > 9) return nullptr;
    if (!selectable) selectable = MenuItemSelectable;
    const MenuItem* best = nullptr;
    uint32_t bestScore = 0;
    for (const MenuItem& it : page.items) {
        if (&it == current || !selectable(it)) continue;
        const int32_t dx = it.CenterX() - x, dy = it.CenterY() - y;
        const uint32_t sq = uint32_t(dx * dx + dy * dy);
        uint32_t d = 0; // 0x80081288: integer square root
        while (uint64_t(d + 1) * (d + 1) <= sq) d++;
        if (d == 0) continue;
        const int32_t dot = ((kDir[direction][0] * ((dx * 0x1000) / int32_t(d))) >> 12) + ((kDir[direction][1] * ((dy * 0x1000) / int32_t(d))) >> 12);
        if (dot < 0) continue;
        const int64_t prod = int64_t(0x1000000 / int32_t(d)) * int64_t((dot * dot) >> 12);
        const uint32_t score = uint32_t(uint64_t(prod) >> 12); // lo >> 12 | hi << 20
        if (int32_t(bestScore) < int32_t(score)) {
            bestScore = score;
            best = &it;
        }
    }
    return best;
}

MenuPages MenuPages::Load(const GtfsVolume& vol, const std::string& language) {
    MenuPages m;
    const std::string dir = "gtmenu/" + language + "/";
    m.index_ = ParseMenuPackIndex(vol.Read(dir + "gtmenudat.idx"));
    const GtfsEntry* dat = vol.Find(dir + "gtmenudat.dat");
    if (!dat) throw std::runtime_error("no " + dir + "gtmenudat.dat");
    m.dat_ = vol.ReadStored(*dat);
    m.solo_ = ParseSoloData(vol.Read(dir + "solodata.dat"));
    return m;
}

uint32_t MenuPages::Resolve(uint32_t id) const {
    if (id & 0x80000000u) {
        const uint32_t i = id & 0x7FFFFFFFu;
        if (i >= solo_.pages.size()) throw std::out_of_range("message page " + Hex(id, 8) + " not in solodata");
        return solo_.pages[i];
    }
    if (id >= Count()) throw std::out_of_range("page " + std::to_string(id) + " beyond gtmenudat");
    return id;
}

std::vector<uint8_t> MenuPages::Bytes(uint32_t id) const { return MenuPackEntry(dat_, index_, Resolve(id), true); }

MenuPage MenuPages::Page(uint32_t id) const { return ParseMenuPage(Bytes(id), Resolve(id)); }

} // namespace gt2
