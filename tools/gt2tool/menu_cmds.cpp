// gt2tool menu-page / menu-dump: GM pages of the GT-mode menus (gt2formats/gt_menu.h), rendered with our own
// software renderer (gt2formats/gt_menu_images.h). Output only under work\ (game art). docs/formats/gt_menu.md.
#include "menu_cmds.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>

#include "game/career/career_state.h"
#include "game/career/events.h"
#include "game/career/garage.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_params.h"
#include "gt2formats/gt_menu.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/gtmode_tables.h"
#include "gt2vfs/inflate.h"

namespace gt2 {
namespace {

// ---------------------------------------------------------------- compressed PNG (fixed-Huffman deflate + LZ77)

class BitWriter {
public:
    void Bits(uint32_t v, int n) { // LSB first
        for (int i = 0; i < n; i++) {
            if (bit_ == 0) out.push_back(0);
            if (v >> i & 1) out.back() |= uint8_t(1u << bit_);
            bit_ = (bit_ + 1) & 7;
        }
    }
    void Huffman(uint32_t code, int n) { // MSB first
        for (int i = n - 1; i >= 0; i--) Bits(code >> i & 1, 1);
    }
    std::vector<uint8_t> out;
private:
    int bit_ = 0;
};

void FixedLiteral(BitWriter& w, int sym) {
    if (sym < 144) w.Huffman(uint32_t(0x30 + sym), 8);
    else if (sym < 256) w.Huffman(uint32_t(0x190 + sym - 144), 9);
    else if (sym < 280) w.Huffman(uint32_t(sym - 256), 7);
    else w.Huffman(uint32_t(0xC0 + sym - 280), 8);
}

std::vector<uint8_t> Deflate(const std::vector<uint8_t>& in) {
    static constexpr int kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr int kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr int kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static constexpr int kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    BitWriter w;
    w.Bits(1, 1); // final
    w.Bits(1, 2); // fixed Huffman
    constexpr size_t kHash = 1 << 15, kWindow = 32768;
    std::vector<int64_t> head(kHash, -1), prev(in.size(), -1);
    auto hash = [&](size_t i) { return uint32_t(((uint32_t(in[i]) << 10) ^ (uint32_t(in[i + 1]) << 5) ^ in[i + 2]) & (kHash - 1)); };
    size_t i = 0;
    while (i < in.size()) {
        int bestLen = 0;
        size_t bestDist = 0;
        if (i + 2 < in.size()) {
            const uint32_t h = hash(i);
            int64_t cand = head[h];
            for (int chain = 0; cand >= 0 && chain < 32 && i - size_t(cand) <= kWindow; chain++, cand = prev[size_t(cand)]) {
                int len = 0;
                while (len < 258 && i + size_t(len) < in.size() && in[size_t(cand) + size_t(len)] == in[i + size_t(len)]) len++;
                if (len > bestLen) {
                    bestLen = len;
                    bestDist = i - size_t(cand);
                }
            }
            prev[i] = head[h];
            head[h] = int64_t(i);
        }
        if (bestLen >= 3) {
            int l = 28;
            while (kLenBase[l] > bestLen) l--;
            FixedLiteral(w, 257 + l);
            w.Bits(uint32_t(bestLen - kLenBase[l]), kLenExtra[l]);
            int d = 29;
            while (size_t(kDistBase[d]) > bestDist) d--;
            w.Huffman(uint32_t(d), 5);
            w.Bits(uint32_t(bestDist - size_t(kDistBase[d])), kDistExtra[d]);
            for (size_t k = 1; k < size_t(bestLen); k++) { // index the skipped positions
                const size_t p = i + k;
                if (p + 2 < in.size()) {
                    const uint32_t h = hash(p);
                    prev[p] = head[h];
                    head[h] = int64_t(p);
                }
            }
            i += size_t(bestLen);
        } else {
            FixedLiteral(w, in[i]);
            i++;
        }
    }
    FixedLiteral(w, 256);
    return w.out;
}

void WritePngCompressed(const std::string& path, int width, int height, const std::vector<uint8_t>& rgba) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(height) * (size_t(width) * 3 + 1));
    for (int y = 0; y < height; y++) {
        raw.push_back(0);
        for (int x = 0; x < width; x++)
            for (int c = 0; c < 3; c++) raw.push_back(rgba[(size_t(y) * width + x) * 4 + c]);
    }
    std::vector<uint8_t> z = {0x78, 0x01};
    const std::vector<uint8_t> d = Deflate(raw);
    z.insert(z.end(), d.begin(), d.end());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; k--) z.push_back(uint8_t(adler >> (8 * k)));
    std::vector<uint8_t> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        for (int k = 3; k >= 0; k--) file.push_back(uint8_t(data.size() >> (8 * k)));
        std::vector<uint8_t> body(type, type + 4);
        body.insert(body.end(), data.begin(), data.end());
        file.insert(file.end(), body.begin(), body.end());
        const uint32_t crc = Crc32(std::span<const uint8_t>(body), 0u);
        for (int k = 3; k >= 0; k--) file.push_back(uint8_t(crc >> (8 * k)));
    };
    std::vector<uint8_t> ihdr;
    for (int v : {width, height})
        for (int k = 3; k >= 0; k--) ihdr.push_back(uint8_t(uint32_t(v) >> (8 * k)));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0}); // 8-bit RGB
    chunk("IHDR", ihdr);
    chunk("IDAT", z);
    chunk("IEND", {});
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("cannot write " + path);
    std::fwrite(file.data(), 1, file.size(), f);
    std::fclose(f);
}

// ---------------------------------------------------------------- state from the game tables

struct MenuContext {
    MenuAssets assets;
    MenuPages pages;
    std::map<uint32_t, uint32_t> prices;   // catalogue price by car id (0x800177D4)
    std::map<std::string, MenuRenderState::EventInfo> events;
    std::string eventNote;
};

MenuContext LoadContext(const DiscImage& disc, const GtfsVolume& vol) {
    MenuContext c{MenuAssets::Load(disc, vol), MenuPages::Load(vol), {}, {}, {}};
    const CarParamTables tables = CarParamTables::Load(vol);
    for (size_t i = 0; i < CarCatalogueCount(tables); i++) {
        const CarCatalogueRow r = CarCatalogueAt(tables, i);
        c.prices[r.spec.carId] = r.price;
    }
    try { // event records of the menus (0x80019474), for the event items' texts and icons
        const career::CareerData data = career::CareerData::Load(disc, vol);
        const career::EventMenuData menu = career::EventMenuData::Load(data.ovl4);
        const career::EventInfoTable infos = career::BuildEventInfos(data, menu);
        for (size_t i = 0; i < menu.events.size() && i < infos.infos.size(); i++) {
            const career::EventInfo& e = infos.infos[i];
            MenuRenderState::EventInfo m;
            m.bonus = e.bonus;
            for (size_t k = 0; k < 6; k++) m.prize[k] = e.prize[k];
            m.powerLimit = e.powerLimit;
            const int need = (e.rules & 0xE) >> 1; // 0x80019634: 1..6 -> licence 5..0, else -1
            m.licence = (need >= 1 && need <= 6) ? 6 - need : -1;
            c.events[menu.events[i]] = m;
        }
    } catch (const std::exception& e) {
        c.eventNote = std::string("event records unavailable: ") + e.what();
    }
    return c;
}

MenuRenderState StateFor(const MenuContext& c, int32_t money, uint32_t day, bool cursor) {
    MenuRenderState s;
    s.money = money;
    s.day = day;
    s.cursor = cursor;
    s.carPrice = [&c](uint32_t id) -> std::optional<uint32_t> {
        const auto it = c.prices.find(id);
        if (it == c.prices.end()) return std::nullopt;
        return it->second;
    };
    s.eventInfo = [&c](const std::string& name) -> std::optional<MenuRenderState::EventInfo> {
        const auto it = c.events.find(name);
        if (it == c.events.end()) return std::nullopt;
        return it->second;
    };
    return s;
}

uint32_t ParseId(const std::string& s) { return uint32_t(std::strtoul(s.c_str(), nullptr, 0)); }

std::string FlagNames(uint32_t f) {
    using namespace menu_item_flag;
    static const std::pair<uint32_t, const char*> kNames[] = {
        {kLicenceGated, "licence-gated"}, {kEventResult, "result"}, {kEventLicence, "licence-icon"}, {kEventPower, "power"},
        {kEventPrize, "prize"}, {kEventTrophy, "trophy"}, {kAction, "action"}, {kDefault, "default"}, {kCarPrice, "car"},
        {kEvent, "event"}, {kWheel, "wheel"}, {kHidePrice, "no-price"}, {kRacePath3, "race-path-3"}, {kSecondFont, "font-b"}};
    std::string s;
    for (const auto& [bit, name] : kNames)
        if (f & bit) s += std::string(s.empty() ? "" : ",") + name;
    const uint32_t other = f & 0x00030000u; // bits 16..17: not tested by the overlay
    if (other) {
        char b[16];
        std::snprintf(b, sizeof b, "%s0x%X", s.empty() ? "" : ",", other);
        s += b;
    }
    return s;
}

void PrintPage(std::FILE* out, const MenuPage& p, uint32_t requested) {
    size_t sprites = 0;
    for (const MenuGroup& g : p.groups) sprites += g.sprites.size();
    std::fprintf(out, "page %u (requested 0x%X): flags %08X (maker %u, music %u%s%s) back %s picture %u; %zu groups (%zu sprites), %zu items, %zu page tiles, %u texture tiles (%d rows)\n",
                 p.id, requested, p.flags, p.Maker(), p.Music(), p.KeepsHistory() ? ", keeps history" : "", p.ClearsBehind() ? ", clears behind" : "",
                 p.back == 0xFFFFFFFFu ? "-1" : std::to_string(p.back).c_str(), p.picture, p.groups.size(), sprites, p.items.size(), p.own.tiles.size(),
                 p.own.textureTiles, p.own.Rows());
    for (size_t g = 0; g < p.groups.size(); g++)
        for (const MenuSprite& s : p.groups[g].sprites)
            std::fprintf(out, "  group %zu sprite (%d,%d) %ux%u uv (%u,%u) tpage %X clut %04X\n", g, s.x, s.y, s.w, s.h, s.u, s.v, s.tpage, s.clut);
    for (size_t i = 0; i < p.items.size(); i++) {
        const MenuItem& it = p.items[i];
        std::string data;
        if (it.Has(menu_item_flag::kEvent) || it.Has(menu_item_flag::kWheel)) data = "name \"" + it.Name() + "\"";
        else if (it.Type() == 0x06) {
            char b[64];
            std::snprintf(b, sizeof b, "target %u car %s", it.Target(), UnpackCarId(it.CarId()).c_str());
            data = b;
        } else if (it.Type() == 0x01) {
            const MenuSprite s = it.AltSprite();
            char b[96];
            std::snprintf(b, sizeof b, "target %u alt sprite (%d,%d) %ux%u uv (%u,%u) clut %04X", it.Target(), s.x, s.y, s.w, s.h, s.u, s.v, s.clut);
            data = b;
        } else if (it.Target() != 0) {
            char b[32];
            std::snprintf(b, sizeof b, "target %s0x%X", it.Target() & 0x80000000u ? "message " : "", it.Target());
            data = b;
        }
        if (it.Byte4A() || it.Byte4B()) data += " +4A " + std::to_string(it.Byte4A()) + " +4B " + std::to_string(it.Byte4B());
        std::fprintf(out, "  %2zu%s (%3d,%3d)-(%3d,%3d) flags %08X type %02X %-26s %s [%s]\n", i, it.group >= 0 ? "g" : " ", it.x0, it.y0, it.x1, it.y1, it.flags,
                     it.Type(), MenuItemTypeName(it.Type()).c_str(), data.c_str(), FlagNames(it.flags).c_str());
    }
}

// Side by side: ours | original | differences (red), and the pixel counts. `capture` = gt2play --prims *.vram.bin.
int Compare(const MenuRender& r, const std::string& capturePath, const std::string& sidePath) {
    std::FILE* f = std::fopen(capturePath.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + capturePath);
    std::vector<uint16_t> cap(size_t(MenuVram::kWidth) * MenuVram::kHeight);
    const size_t got = std::fread(cap.data(), 2, cap.size(), f);
    std::fclose(f);
    if (got != cap.size()) throw std::runtime_error(capturePath + ": not a 1024 x 512 VRAM dump");
    const int W = MenuCanvas::kWidth, H = MenuCanvas::kHeight;
    size_t diff = 0;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    const std::vector<uint8_t> ours = r.canvas.Rgba();
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint16_t a = r.canvas.At(x, y) & 0x7FFF, b = cap[size_t(y) * MenuVram::kWidth + size_t(x)] & 0x7FFF;
            const bool d = a != b;
            if (d) {
                diff++;
                minX = std::min(minX, x), minY = std::min(minY, y), maxX = std::max(maxX, x), maxY = std::max(maxY, y);
            }
            uint8_t* o = &side[(size_t(y) * W * 3 + size_t(x)) * 4];
            std::memcpy(o, &ours[(size_t(y) * W + size_t(x)) * 4], 4);
            uint8_t* c = &side[(size_t(y) * W * 3 + size_t(W + x)) * 4];
            c[0] = uint8_t((b & 31) << 3), c[1] = uint8_t(((b >> 5) & 31) << 3), c[2] = uint8_t(((b >> 10) & 31) << 3), c[3] = 255;
            uint8_t* e = &side[(size_t(y) * W * 3 + size_t(2 * W + x)) * 4];
            const uint8_t grey = uint8_t(c[0] / 4 + c[1] / 4 + c[2] / 4);
            e[0] = d ? 255 : grey, e[1] = d ? 0 : grey, e[2] = d ? 0 : grey, e[3] = 255;
        }
    std::printf("frame 512x480 vs %s: %zu differing pixels", capturePath.c_str(), diff);
    if (diff) std::printf(" (box %d,%d - %d,%d)", minX, minY, maxX, maxY);
    std::printf("\n");
    struct Region { const char* name; int x, y, w, h; };
    for (const Region g : {Region{"commonpic CLUT (0,504)", 0, 504, 512, 8}, Region{"commonpic pixels (768,0)", 768, 0, 256, r.backgroundRows},
                           Region{"gt_cursor.tim (576,0)", 576, 0, 64, 156}, Region{"gtmode_font.tim (640,0)", 640, 0, 64, 247},
                           Region{"iconimg (704,0)", 704, 0, 64, 256}, Region{"gt_items.tim (512,256)", 512, 256, 64, 157},
                           Region{"page CLUT (576,248)", 576, 248, 64, 8}, Region{"page pixels (640,256)", 640, 256, 128, r.pageRows}}) {
        size_t n = 0;
        for (int y = g.y; y < g.y + g.h; y++)
            for (int x = g.x; x < g.x + g.w; x++) {
                const uint16_t ours16 = r.vram.Word(x, y);
                n += ours16 != cap[size_t(y) * MenuVram::kWidth + size_t(x)];
            }
        std::printf("  VRAM %-26s %zu of %d words differ\n", g.name, n, g.w * g.h);
    }
    if (!sidePath.empty()) {
        WritePngCompressed(sidePath, W * 3, H, side);
        std::printf("wrote %s (ours | original | differences)\n", sidePath.c_str());
    }
    return diff == 0 ? 0 : 1;
}

} // namespace

namespace {

// The popup lists of `menu-page` (gt2formats/gt_menu_list.h): what to draw and the widget state to show.
struct ListOptions {
    bool usedCars = false, garage = false;
    std::string card;                    // garage rows from this card / save file (empty = a new game: no cars)
    int frames = 0;                      // updates to run after the page load
    bool active = false;                 // ... as the active popup (pad given, flash counting) or in page mode
    std::string pad;                     // "frame:button,..." presses during those updates
    int sel = -1, scroll = 0x7FFF, flash = 0x7FFF, state = 0x7FFF, activeByte = -1; // overrides (applied last)
};

uint32_t ListButton(const std::string& name) {
    using namespace menu_list_pad;
    static const std::map<std::string, uint32_t> kBits = {{"up", kUp}, {"down", kDown}, {"left", kLeft}, {"right", kRight}, {"triangle", kTriangle},
                                                           {"cross", kCross}, {"square", kSquare}, {"circle", kCircle}, {"start", kStart}, {"select", kSelect},
                                                           {"l1", kL1}, {"r1", kR1}, {"l2", kL2}, {"r2", kR2}};
    const auto it = kBits.find(name);
    if (it == kBits.end()) throw std::runtime_error("--list-pad: unknown button " + name);
    return it->second;
}

// Page load (0x8001D2CC: GT-mode entry reset, then the load of each list item), `frames` updates, overrides.
void PrepareList(MenuPopupList& list, const MenuPage& page, const ListOptions& o, const GtfsVolume& vol, uint32_t day) {
    list.Reset();
    for (const MenuItem& it : page.items) {
        if (list.kind == MenuListKind::kUsedCars && it.Type() == 0x50) {
            const UsedCarLists lots = UsedCarLists::Load(vol);
            const CarInfoDirectory cars = CarInfoDirectory::Load(vol);
            list.Reset(); // a page load with argument 0
            list.LoadUsedCars(&it, MenuUsedCarRows(lots, cars, day, page.Maker()));
        }
        if (list.kind == MenuListKind::kGarage && it.Type() == 0x09) {
            std::vector<MenuGarageRow> rows;
            int16_t current = -1;
            if (!o.card.empty()) {
                const career::CareerSave save = career::LoadCareer(o.card);
                const career::GarageBlock& g = save.state.garage;
                for (int i = 0; i < g.count && i < int(career::kGarageCapacity); i++) {
                    const career::GarageCar& s = g.cars[i];
                    rows.push_back({s.carId, s.paint, s.modelId, s.powerFlags});
                }
                current = g.currentCar;
            }
            list.LoadGarage(&it, std::move(rows), current);
        }
    }
    std::map<int, uint32_t> presses;
    for (size_t pos = 0; pos < o.pad.size();) {
        size_t comma = o.pad.find(',', pos);
        if (comma == std::string::npos) comma = o.pad.size();
        const std::string e = o.pad.substr(pos, comma - pos);
        const size_t colon = e.find(':');
        if (colon == std::string::npos) throw std::runtime_error("--list-pad: expected frame:button");
        presses[std::atoi(e.c_str())] |= ListButton(e.substr(colon + 1));
        pos = comma + 1;
    }
    for (int f = 0; f < o.frames; f++) {
        MenuListPad pad;
        const auto it = presses.find(f);
        if (it != presses.end()) pad.pressed = it->second;
        const int32_t r = list.Update(o.active ? &pad : nullptr, o.active);
        if (r != -1) std::printf("  list frame %d: update returned %d\n", f, r);
    }
    if (o.sel >= 0) list.widget.selection = int16_t(o.sel);
    if (o.scroll != 0x7FFF) list.widget.scroll = int16_t(o.scroll);
    if (o.flash != 0x7FFF) list.flash = int16_t(o.flash);
    if (o.state != 0x7FFF) list.widget.state = int16_t(o.state);
    if (o.activeByte >= 0) list.widget.active = uint8_t(o.activeByte);
    std::printf("  %s list: %d rows, selection %d, scroll %d, blink %d, state %d, active %d, flash %d\n", list.kind == MenuListKind::kGarage ? "garage" : "used-car",
                list.count, list.widget.selection, list.widget.scroll, list.widget.blink, list.widget.state, list.widget.active, list.flash);
}

} // namespace

int CmdMenuPage(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv) {
    if (argc < 4)
        throw std::runtime_error("menu-page <disc> <page> [--png out.png] [--money N] [--day N] [--no-cursor] [--compare capture.vram.bin [--side out.png]]\n"
                                 "  [--rules ps1|interp] [--used-cars] [--garage [--card save.mcd]] [--list-frames N [--list-active] [--list-pad f:button,...]]\n"
                                 "  [--list-sel N] [--list-scroll N] [--list-flash N] [--list-state N] [--list-active-byte 0|1]");
    const uint32_t id = ParseId(argv[3]);
    std::string png, compare, side, rules;
    int32_t money = 10000;
    uint32_t day = 1;
    bool cursor = true;
    ListOptions lo;
    for (int i = 4; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--png" && i + 1 < argc) png = argv[++i];
        else if (a == "--money" && i + 1 < argc) money = int32_t(std::strtol(argv[++i], nullptr, 10));
        else if (a == "--day" && i + 1 < argc) day = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (a == "--no-cursor") cursor = false;
        else if (a == "--compare" && i + 1 < argc) compare = argv[++i];
        else if (a == "--side" && i + 1 < argc) side = argv[++i];
        else if (a == "--rules" && i + 1 < argc) rules = argv[++i];
        else if (a == "--used-cars") lo.usedCars = true;
        else if (a == "--garage") lo.garage = true;
        else if (a == "--card" && i + 1 < argc) lo.card = argv[++i];
        else if (a == "--list-frames" && i + 1 < argc) lo.frames = std::atoi(argv[++i]);
        else if (a == "--list-active") lo.active = true;
        else if (a == "--list-pad" && i + 1 < argc) lo.pad = argv[++i];
        else if (a == "--list-sel" && i + 1 < argc) lo.sel = std::atoi(argv[++i]);
        else if (a == "--list-scroll" && i + 1 < argc) lo.scroll = std::atoi(argv[++i]);
        else if (a == "--list-flash" && i + 1 < argc) lo.flash = std::atoi(argv[++i]);
        else if (a == "--list-state" && i + 1 < argc) lo.state = std::atoi(argv[++i]);
        else if (a == "--list-active-byte" && i + 1 < argc) lo.activeByte = std::atoi(argv[++i]);
        else throw std::runtime_error("menu-page: unknown option " + a);
    }
    const MenuContext c = LoadContext(disc, vol);
    if (!c.eventNote.empty()) std::printf("%s\n", c.eventNote.c_str());
    const MenuPage page = c.pages.Page(id);
    PrintPage(stdout, page, id);
    for (const MenuItem& it : page.items) {
        const std::string h = MenuItemTypeHandler(it.Type());
        if (!h.empty()) std::printf("  type %02X: %s\n", it.Type(), h.c_str());
    }
    MenuRenderState state = StateFor(c, money, day, cursor);
    std::unique_ptr<MenuListNames> names;
    std::unique_ptr<MenuPopupList> usedCars, garage;
    if (lo.usedCars || lo.garage) {
        names = std::make_unique<MenuListNames>(MenuListNames::Load(vol, c.assets.ovl4));
        if (lo.usedCars) {
            usedCars = std::make_unique<MenuPopupList>(MenuListKind::kUsedCars, c.assets, *names);
            PrepareList(*usedCars, page, lo, vol, day);
        }
        if (lo.garage) {
            garage = std::make_unique<MenuPopupList>(MenuListKind::kGarage, c.assets, *names);
            PrepareList(*garage, page, lo, vol, day);
        }
        if (garage && garage->currentCar >= 0 && size_t(garage->currentCar) < garage->garageRows.size()) {
            // Type 0x47 with the card's current car: 0x8001B818(ot, slot +0, x0, y1, font, 0, slot +98 bit 15, 0, 0x6E6E6E)
            // = [the racing prefix] + the model name, UTF-16, left at (x0, y1) (0x8001FBEC).
            const MenuGarageRow car = garage->garageRows[size_t(garage->currentCar)];
            state.dynamicItem = [&c, &names, car](const MenuItem& it, std::vector<MenuPrim>& generation) {
                if (it.Type() != 0x47) return false;
                MenuTextWriter text(c.assets.fonts, generation);
                const bool second = it.Has(menu_item_flag::kSecondFont);
                int x = it.x0;
                if (car.word98 >> 15) x += text.Left(names->racingPrefix, false, second, x, it.y1, 0x6E6E6E);
                text.Left(names->Model(car.carId), false, second, x, it.y1, 0x6E6E6E);
                return true;
            };
        }
        state.customItem = [&](const MenuItem& it, std::vector<MenuPrim>& out) {
            if (it.Type() == 0x50 && usedCars) usedCars->Draw(out);
            else if (it.Type() == 0x09 && garage) garage->Draw(out);
            else return false;
            return true;
        };
    }
    // RenderMenuPage with the canvas rules: the interpreter's for comparisons with its captures, else the PS1's.
    MenuRender r;
    ComposeMenuVram(c.assets, page, r.vram, &r.backgroundRows, &r.pageRows);
    const MenuFrame frame = BuildMenuFrame(c.assets, page, state);
    if (rules.empty()) rules = compare.empty() ? "ps1" : "interp";
    if (rules != "ps1" && rules != "interp") throw std::runtime_error("--rules ps1|interp");
    r.canvas.rules = rules == "ps1" ? MenuCanvas::Rules::kPs1 : MenuCanvas::Rules::kInterpreter;
    for (const MenuPrim& p : frame.prims) r.canvas.Draw(r.vram, p);
    r.notes = frame.notes;
    std::printf("  canvas rules: %s\n", rules == "ps1" ? "PS1 GPU" : "interpreter GPU (src/machine/gpu.cpp)");
    for (const std::string& n : r.notes) std::printf("  drawn: %s\n", n.c_str());
    if (!png.empty()) {
        WritePngCompressed(png, MenuCanvas::kWidth, MenuCanvas::kHeight, r.canvas.Rgba());
        std::printf("wrote %s\n", png.c_str());
    }
    if (!compare.empty()) return Compare(r, compare, side);
    return 0;
}

int CmdMenuDump(const DiscImage& disc, const GtfsVolume& vol, int argc, char** argv) {
    if (argc < 4) throw std::runtime_error("menu-dump <disc> <outDir> [--no-png]");
    const std::filesystem::path dir = argv[3];
    bool pngs = true;
    for (int i = 4; i < argc; i++)
        if (std::string(argv[i]) == "--no-png") pngs = false;
    std::filesystem::create_directories(dir);
    const MenuContext c = LoadContext(disc, vol);
    if (!c.eventNote.empty()) std::printf("%s\n", c.eventNote.c_str());
    size_t pictures = 0;
    for (size_t i = 0; i < c.assets.commonIndex.Count(); i++) {
        const GtmpPicture g = c.assets.Background(uint32_t(i));
        for (uint32_t w : g.tiles) {
            const MenuTile t = DecodeBackgroundTile(w);
            if (!t.flat && ((t.tpage & 0x10) ? 256 : 0) + t.v + 8 > g.Rows()) throw std::runtime_error("commonpic " + std::to_string(i) + ": tile beyond the pixels");
        }
        pictures++;
    }
    std::printf("commonpic: %zu GTMP pictures parsed, every texture tile inside the picture's rows\n", pictures);
    std::FILE* txt = std::fopen((dir / "pages.txt").string().c_str(), "w");
    if (!txt) throw std::runtime_error("cannot write pages.txt");
    std::map<uint16_t, size_t> types;
    size_t parsed = 0, failed = 0, drawn = 0;
    for (uint32_t id = 0; id < c.pages.Count(); id++) {
        try {
            const MenuPage p = c.pages.Page(id);
            parsed++;
            PrintPage(txt, p, id);
            for (const MenuItem& it : p.items) types[it.Type()]++;
            if (pngs) {
                const MenuRender r = RenderMenuPage(c.assets, p, StateFor(c, 10000, 1, true));
                char name[32];
                std::snprintf(name, sizeof name, "page_%04u.png", id);
                WritePngCompressed((dir / name).string(), MenuCanvas::kWidth, MenuCanvas::kHeight, r.canvas.Rgba());
                drawn++;
            }
        } catch (const std::exception& e) {
            failed++;
            std::fprintf(txt, "page %u: ERROR %s\n", id, e.what());
            std::printf("page %u: %s\n", id, e.what());
        }
        if (id % 500 == 499) std::printf("  %u pages...\n", id + 1);
    }
    std::fprintf(txt, "\nitem types:\n");
    for (const auto& [t, n] : types)
        std::fprintf(txt, "  %02X %6zu %-28s %s\n", t, n, MenuItemTypeName(t).c_str(), MenuItemTypeHandler(t).c_str());
    std::fclose(txt);
    std::printf("gtmenudat: %zu pages parsed to the last byte, %zu failed, %zu rendered; %zu item types -> %s\n", parsed, failed, drawn, types.size(),
                (dir / "pages.txt").string().c_str());
    return failed == 0 ? 0 : 1;
}

} // namespace gt2
