#pragma once
// Cursor navigation of the GT-mode menu pages, ported from the GT-mode overlay (GT2.OVL member 4 at 0x80010000) of
// US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): the selectable tests
// 0x8001B6F0 / 0x8001B680, the geometric nearest-item search 0x8001D754 (direction vectors 0x80051274, integer
// square root EXE 0x80081288 = sim::SquareRoot), and the item-under-point search 0x8001D954. Verified against the
// original on a GT-mode RAM image (tools/gt2verify/verify_menu.cpp). docs/formats/gt_menu.md section 3.2.
#include <array>
#include <cstdint>
#include <span>

#include "gt2formats/gt_menu.h"

namespace gt2 {
struct GuestImage;
}

namespace gt2::menu {

// Pad directions of 0x8001E328: 2 up, 3 down, 4 left, 5 right, 6 up-right, 7 up-left, 8 down-right, 9 down-left.
enum Direction : int { kDirUp = 2, kDirDown = 3, kDirLeft = 4, kDirRight = 5, kDirUpRight = 6, kDirUpLeft = 7, kDirDownRight = 8, kDirDownLeft = 9 };

// The unit vectors of the directions (ovl4 0x80051274: s16 x, s16 y per direction 0..9, 4096 = 1.0).
struct NavVectors {
    static constexpr uint32_t kAddress = 0x80051274u;
    std::array<std::array<int16_t, 2>, 10> v{};
    static NavVectors Load(const GuestImage& ovl4);
    static NavVectors FromBytes(const uint8_t* bytesAtAddress); // 40 bytes at 0x80051274 (e.g. of a RAM image)
};

// The career facts the selectable tests read.
struct SelectContext {
    // 0x8001915C(licence): all ten tests of licence L (0 S .. 5 B) passed.
    bool licenceHeld[6] = {};
    // 0x800174D0 = 0x8005F858(0x800B4490): the number of racing-modification bodies of the current car's tune sheet
    // (consecutive rows >= 0 at sheet +0x15E0.., at most 4); type 0x9D is selectable only when it is >= 2.
    int racingBodies = 0;
};

// 0x8005F858(sheet): consecutive non-negative words at sheet + 0x15E0 (racingModifyRow[1..4] of career::TuneSheet).
int RacingBodyCount(const uint32_t racingModifyRow[5]);

// 0x8001B6F0(item): the item types the cursor can rest on (without the action bit).
bool ItemSelectable(const MenuItem& item, const SelectContext& ctx);
// 0x8001B680(item): 1 unless a licence-gated type-0 item's licence is not held.
bool ItemLicenceGate(const MenuItem& item, const SelectContext& ctx);
// The test of 0x8001D754 / 0x8001D954: (action bit or 0x8001B6F0) and 0x8001B680.
bool ItemReachable(const MenuItem& item, const SelectContext& ctx);

// 0x8001D754(page, x, y, direction, current, 1): the item the cursor at (x, y) moves to in `direction`, -1 when
// none. Every other reachable item is scored by cos^2 / distance of its centre (items behind or at distance 0 are
// skipped; the first of equal best scores wins; a score of 0 is never chosen). `current` = the index to skip (-1
// none). `pageLoaded` = the page object's first byte (0 = no page: -1).
int NearestItem(std::span<const MenuItem> items, int x, int y, int direction, int current, const SelectContext& ctx, const NavVectors& vectors,
                bool pageLoaded = true);

// 0x8001D954(page, x, y): the first item selectable by 0x8001B6F0 whose rectangle strictly contains (x, y), -1 none.
int ItemUnder(std::span<const MenuItem> items, int x, int y, const SelectContext& ctx, bool pageLoaded = true);

// 0x8001D6CC(page): the first item with flag bit 25 (the default item), -1 none.
int DefaultItem(std::span<const MenuItem> items, bool pageLoaded = true);

} // namespace gt2::menu
