// GT-mode menu navigation (src/game/menu/menu_nav.*) against the GT-mode overlay (GT2.OVL member 4) of a RAM image
// taken in the menus: the selectable tests 0x8001B6F0 / 0x8001B680, the nearest-item search 0x8001D754, the
// item-under-point search 0x8001D954, the default item 0x8001D6CC and the racing-body count 0x8005F858. The rows
// run only when the dump holds member 4 (checked by a hash of the search routine's code); elsewhere they skip.
//
// Inputs are randomised on the guest image: the page's item array (0x801C3150 + i * 0x4C, count 0x800A8D74), the
// licence records the gate reads (career state 0x801C98E0 + 0x1418, byte +1 of each test) and the current car's
// tune sheet words 0x800B4490 + 0x15E0.. (0x800174D0); a quarter of the cases keep the dump's own page items. The
// port reads the same image through the career structs (career::LicenceHeld, career::TuneSheet).
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "game/career/career_state.h"
#include "game/career/garage.h"
#include "game/career/results.h"
#include "game/menu/menu_nav.h"
#include "guest.h"

namespace gt2::verify {
namespace {

constexpr uint32_t kItems = 0x801C3150u, kItemCount = 0x800A8D74u, kItemSize = 0x4C, kMaxItems = 64;
constexpr uint32_t kNearest = 0x8001D754u, kUnder = 0x8001D954u, kDefault = 0x8001D6CCu, kSelectable = 0x8001B6F0u, kGate = 0x8001B680u;
constexpr uint32_t kBodyCount = 0x8005F858u, kMenuSheet = 0x800B4490u;
constexpr uint32_t kPageObject = kStack + 0x80; // a stand-in page object: only its first byte (loaded flag) is read
constexpr uint32_t kNavCodeHash = 0x09D9DF76u;   // FNV-1a of 0x8001D754..0x8001D954 of member 4 (the overlay is loaded)

uint32_t Fnv(const uint8_t* p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

uint8_t* At(uint8_t* ram, uint32_t address) { return ram + (address & 0x1FFFFF); }
uint32_t Word(const uint8_t* ram, uint32_t address) {
    uint32_t v;
    std::memcpy(&v, ram + (address & 0x1FFFFF), 4);
    return v;
}
void SetWord(uint8_t* ram, uint32_t address, uint32_t v) { std::memcpy(ram + (address & 0x1FFFFF), &v, 4); }

MenuItem ItemAt(const uint8_t* ram, uint32_t address) {
    const uint8_t* p = ram + (address & 0x1FFFFF);
    MenuItem it;
    std::memcpy(&it.x0, p, 2);
    std::memcpy(&it.y0, p + 2, 2);
    std::memcpy(&it.x1, p + 4, 2);
    std::memcpy(&it.y1, p + 6, 2);
    std::memcpy(&it.flags, p + 8, 4);
    std::memcpy(it.data.data(), p + 0x0C, it.data.size());
    return it;
}

std::vector<MenuItem> ItemsOf(const uint8_t* ram) {
    std::vector<MenuItem> items;
    const uint32_t n = Word(ram, kItemCount);
    for (uint32_t i = 0; i < n && i < kMaxItems; i++) items.push_back(ItemAt(ram, kItems + i * kItemSize));
    return items;
}

menu::SelectContext ContextOf(const uint8_t* ram) {
    const auto& state = *reinterpret_cast<const career::CareerState*>(ram + (career::kStateAddress & 0x1FFFFF));
    const auto& sheet = *reinterpret_cast<const career::TuneSheet*>(ram + (kMenuSheet & 0x1FFFFF));
    menu::SelectContext c;
    for (int l = 0; l < 6; l++) c.licenceHeld[l] = career::LicenceHeld(state, l);
    c.racingBodies = menu::RacingBodyCount(sheet.racingModifyRow);
    return c;
}

// Types worth covering: every case of 0x8001B6F0 and some drawn-only types.
constexpr uint16_t kTypes[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x10, 0x11, 0x12, 0x13, 0x14,
                               0x2A, 0x46, 0x47, 0x4D, 0x4F, 0x50, 0x94, 0x95, 0x96, 0x98, 0x9C, 0x9D, 0xAA, 0xAB, 0xAD, 0xAE, 0xBB, 0xCC, 0xCF, 0xFFFF};

// Randomises the item array (count 0..64 items; rectangles on and off the 512 x 480 page, degenerate ones), the
// licence records and the tune sheet words.
void RandomisePage(uint8_t* ram, std::mt19937& rng, bool keepDumpItems) {
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    for (int l = 0; l < 6; l++) { // licence L held when all ten tests have byte +1 != 0: mostly all or none, some mixed
        const int mode = r(0, 3);
        for (int t = 0; t < 10; t++)
            At(ram, career::kStateAddress + 0x1418 + uint32_t(l) * 0x668 + uint32_t(t) * 0xA4)[1] = uint8_t(mode == 0 ? 0 : mode == 1 ? r(1, 255) : r(0, 1) * r(1, 3));
    }
    for (uint32_t k = 0; k < 5; k++) SetWord(ram, kMenuSheet + 0x15DC + k * 4, r(0, 2) ? uint32_t(r(0, 0x300)) : 0xFFFFFFFFu);
    if (keepDumpItems) return;
    const uint32_t count = uint32_t(r(0, 3) ? r(1, 40) : r(0, int(kMaxItems)));
    SetWord(ram, kItemCount, count);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* p = At(ram, kItems + i * kItemSize);
        for (uint32_t k = 0; k < kItemSize; k++) p[k] = uint8_t(r(0, 255));
        int16_t x0, y0, x1, y1;
        if (r(0, 9) == 0) { // anything, including inverted and huge rectangles
            x0 = int16_t(r(-0x8000, 0x7FFF)), y0 = int16_t(r(-0x8000, 0x7FFF)), x1 = int16_t(r(-0x8000, 0x7FFF)), y1 = int16_t(r(-0x8000, 0x7FFF));
        } else {
            x0 = int16_t(r(-40, 520)), y0 = int16_t(r(-40, 500));
            x1 = int16_t(x0 + r(0, 200)), y1 = int16_t(y0 + r(0, 120));
        }
        std::memcpy(p, &x0, 2), std::memcpy(p + 2, &y0, 2), std::memcpy(p + 4, &x1, 2), std::memcpy(p + 6, &y1, 2);
        uint32_t flags = r(0, 5) ? kTypes[size_t(r(0, int(std::size(kTypes)) - 1))] : uint32_t(r(0, 0xFFFF));
        for (int bit = 16; bit < 32; bit++)
            if (r(0, 5) == 0) flags |= 1u << bit;
        std::memcpy(p + 8, &flags, 4);
        p[0x4A] = uint8_t(r(0, 3) ? r(0, 2) : r(0, 255));
    }
}

} // namespace

int VerifyMenu(Guest& guest, const std::vector<uint8_t>& pristine, std::mt19937& rng) {
    if (Fnv(pristine.data() + (kNearest & 0x1FFFFF), 0x200) != kNavCodeHash) {
        std::printf("%-10s skipped (the GT-mode overlay, GT2.OVL member 4, is not loaded in this dump)\n", "MenuNav");
        return 0;
    }
    int failures = 0;
    const menu::NavVectors vectors = menu::NavVectors::FromBytes(pristine.data() + (menu::NavVectors::kAddress & 0x1FFFFF));
    auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    auto reset = [&](bool keepDumpItems) {
        std::memcpy(guest.Ram(), pristine.data(), pristine.size());
        RandomisePage(guest.Ram(), rng, keepDumpItems);
        At(guest.Ram(), kPageObject)[0] = 1;
    };
    auto itemAddress = [](int index) { return index < 0 ? 0u : kItems + uint32_t(index) * kItemSize; };

    { // 0x8005F858 on the menus' tune sheet
        size_t cases = 0, bad = 0;
        for (int n = 0; n < 400; n++) {
            reset(true);
            const auto& sheet = *reinterpret_cast<const career::TuneSheet*>(guest.Ram() + (kMenuSheet & 0x1FFFFF));
            const int ours = menu::RacingBodyCount(sheet.racingModifyRow);
            const int original = int32_t(guest.Call(kBodyCount, kMenuSheet));
            cases++;
            if (ours != original && bad++ < 3) std::printf("    MISMATCH case %d: original %d ours %d\n", n, original, ours);
        }
        Report("MenuBodies", kBodyCount, cases, bad, failures);
    }
    { // 0x8001B6F0 / 0x8001B680 on every item of random pages
        size_t cases = 0, badSel = 0, badGate = 0;
        for (int n = 0; n < 300; n++) {
            reset(n % 4 == 0);
            const std::vector<MenuItem> items = ItemsOf(guest.Ram());
            const menu::SelectContext ctx = ContextOf(guest.Ram());
            for (size_t i = 0; i < items.size(); i++) {
                const uint32_t a = itemAddress(int(i));
                const bool sel = guest.Call(kSelectable, a) != 0, gate = guest.Call(kGate, a) != 0;
                cases++;
                if (sel != menu::ItemSelectable(items[i], ctx) && badSel++ < 3)
                    std::printf("    MISMATCH 0x8001B6F0 item flags %08X +4A %u: original %d\n", items[i].flags, items[i].Byte4A(), int(sel));
                if (gate != menu::ItemLicenceGate(items[i], ctx) && badGate++ < 3)
                    std::printf("    MISMATCH 0x8001B680 item flags %08X +4A %u: original %d\n", items[i].flags, items[i].Byte4A(), int(gate));
            }
        }
        Report("MenuSelect", kSelectable, cases, badSel, failures);
        Report("MenuGate", kGate, cases, badGate, failures);
    }
    { // 0x8001D754: random pages, cursor positions (item centres, random, extreme), directions, current items
        size_t cases = 0, bad = 0, found = 0;
        for (int n = 0; n < 20000; n++) {
            reset(n % 4 == 0);
            const std::vector<MenuItem> items = ItemsOf(guest.Ram());
            const menu::SelectContext ctx = ContextOf(guest.Ram());
            int x, y;
            const int where = r(0, 9);
            if (where < 6 && !items.empty()) {
                const MenuItem& it = items[size_t(r(0, int(items.size()) - 1))];
                x = it.CenterX(), y = it.CenterY();
            } else if (where < 9) {
                x = r(-64, 600), y = r(-64, 560);
            } else {
                x = r(-0x10000, 0x10000), y = r(-0x10000, 0x10000);
            }
            const int direction = r(0, 19) == 0 ? r(0, 1) : r(2, 9);
            int current = items.empty() || r(0, 4) == 0 ? -1 : r(0, int(items.size()) - 1);
            const bool loaded = r(0, 49) != 0;
            At(guest.Ram(), kPageObject)[0] = loaded ? uint8_t(r(1, 255)) : 0;
            SetWord(guest.Ram(), kStack + 0x10, itemAddress(current)); // 5th argument: the current item
            SetWord(guest.Ram(), kStack + 0x14, 1);                    // 6th argument (not read)
            const int ours = menu::NearestItem(items, x, y, direction, current, ctx, vectors, loaded);
            const uint32_t original = guest.Call(kNearest, kPageObject, uint32_t(x), uint32_t(y), uint32_t(direction));
            cases++;
            found += original != 0;
            if (original != itemAddress(ours) && bad++ < 3)
                std::printf("    MISMATCH case %d: (%d,%d) dir %d current %d, %zu items: original %08X ours %08X\n", n, x, y, direction, current, items.size(), original,
                            itemAddress(ours));
        }
        Report("MenuNav", kNearest, cases, bad, failures);
        std::printf("           (%zu of %zu searches found an item)\n", found, cases);
    }
    { // 0x8001D954 and 0x8001D6CC
        size_t cases = 0, badUnder = 0, badDefault = 0;
        for (int n = 0; n < 5000; n++) {
            reset(n % 4 == 0);
            const std::vector<MenuItem> items = ItemsOf(guest.Ram());
            const menu::SelectContext ctx = ContextOf(guest.Ram());
            const bool loaded = r(0, 49) != 0;
            At(guest.Ram(), kPageObject)[0] = loaded ? 1 : 0;
            int x = r(-40, 560), y = r(-40, 520);
            if (!items.empty() && r(0, 1)) {
                const MenuItem& it = items[size_t(r(0, int(items.size()) - 1))];
                x = it.x0 + r(-1, 2), y = it.CenterY();
            }
            const uint32_t under = guest.Call(kUnder, kPageObject, uint32_t(x), uint32_t(y));
            const uint32_t dflt = guest.Call(kDefault, kPageObject);
            cases++;
            if (under != itemAddress(menu::ItemUnder(items, x, y, ctx, loaded)) && badUnder++ < 3)
                std::printf("    MISMATCH 0x8001D954 case %d (%d,%d): original %08X\n", n, x, y, under);
            if (dflt != itemAddress(menu::DefaultItem(items, loaded)) && badDefault++ < 3) std::printf("    MISMATCH 0x8001D6CC case %d: original %08X\n", n, dflt);
        }
        Report("MenuUnder", kUnder, cases, badUnder, failures);
        Report("MenuDflt", kDefault, cases, badDefault, failures);
    }
    return failures;
}

} // namespace gt2::verify
