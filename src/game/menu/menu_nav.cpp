#include "game/menu/menu_nav.h"

#include <cstring>

#include "game/sim/tyres.h"
#include "gt2formats/overlay_data.h"

namespace gt2::menu {

using namespace menu_item_flag;

NavVectors NavVectors::FromBytes(const uint8_t* p) {
    NavVectors n;
    for (size_t d = 0; d < n.v.size(); d++)
        for (size_t k = 0; k < 2; k++) std::memcpy(&n.v[d][k], p + d * 4 + k * 2, 2);
    return n;
}

NavVectors NavVectors::Load(const GuestImage& ovl4) { return FromBytes(ovl4.At(kAddress, 40)); }

int RacingBodyCount(const uint32_t racingModifyRow[5]) {
    int count = 0; // 0x8005F858: words at sheet + 0x15E0 .. + 0x15EC while >= 0
    for (int k = 1; k <= 4; k++) {
        if (int32_t(racingModifyRow[k]) < 0) break;
        count++;
    }
    return count;
}

namespace {
// 0x8001B6F0 / 0x8001B680 case 0 with bit 18: +0x4A 0 -> licence 1 (IA), 1 -> licence 0 (S), else never.
bool GatedLicence(const MenuItem& it, const SelectContext& ctx) {
    if (it.Byte4A() == 0) return ctx.licenceHeld[1];
    if (it.Byte4A() == 1) return ctx.licenceHeld[0];
    return false;
}
} // namespace

bool ItemSelectable(const MenuItem& it, const SelectContext& ctx) {
    const uint32_t f = it.flags;
    if (f & kEvent) return (f & (kEventResult | kEventTrophy | kEventPrize | kEventPower | kEventLicence)) == 0;
    const uint16_t t = it.Type();
    if (t >= 0x14 && t <= 0x46) return true;
    switch (t) {
    case 0x00: return (f & kLicenceGated) == 0 || GatedLicence(it, ctx);
    case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06: case 0x08: case 0x09: case 0x0B: case 0x0C: case 0x0D:
    case 0x10: case 0x12: case 0x13: case 0x50: case 0x95: case 0x96: case 0xAA: case 0xAB: case 0xBB:
        return true;
    case 0x9D: return ctx.racingBodies >= 2;
    default: return false;
    }
}

bool ItemLicenceGate(const MenuItem& it, const SelectContext& ctx) {
    if (it.Type() == 0 && it.Has(kLicenceGated)) return GatedLicence(it, ctx);
    return true;
}

bool ItemReachable(const MenuItem& it, const SelectContext& ctx) {
    if (!it.Has(kAction) && !ItemSelectable(it, ctx)) return false; // byte +0xB bit 0 = flag bit 24
    return ItemLicenceGate(it, ctx);
}

int NearestItem(std::span<const MenuItem> items, int x, int y, int direction, int current, const SelectContext& ctx, const NavVectors& vectors,
                bool pageLoaded) {
    if (!pageLoaded) return -1;
    const int32_t vx = vectors.v[size_t(direction)][0], vy = vectors.v[size_t(direction)][1];
    int best = -1;
    int32_t bestScore = 0;
    for (size_t i = 0; i < items.size(); i++) {
        if (int(i) == current) continue;
        const MenuItem& it = items[i];
        if (!ItemReachable(it, ctx)) continue;
        const int32_t dx = ((int32_t(it.x0) + int32_t(it.x1)) >> 1) - int32_t(x);
        const int32_t dy = ((int32_t(it.y0) + int32_t(it.y1)) >> 1) - int32_t(y);
        const uint32_t square = uint32_t(dx) * uint32_t(dx) + uint32_t(dy) * uint32_t(dy); // mult / mflo, 32-bit sum
        const int32_t d = sim::SquareRoot(int32_t(square), 0);                                // 0x80081288(sq, 0)
        if (d == 0) continue;
        const int32_t qx = int32_t(uint32_t(dx) << 12) / d, qy = int32_t(uint32_t(dy) << 12) / d;
        const int32_t dot = (int32_t(uint32_t(vx) * uint32_t(qx)) >> 12) + (int32_t(uint32_t(vy) * uint32_t(qy)) >> 12);
        const int32_t inverse = 0x1000000 / d;
        if (dot < 0) continue;
        const int32_t cos2 = int32_t(uint32_t(dot) * uint32_t(dot)) >> 12;
        const int64_t product = int64_t(inverse) * int64_t(cos2); // mult (signed), mfhi / mflo
        const uint32_t lo = uint32_t(uint64_t(product)), hi = uint32_t(uint64_t(product) >> 32);
        const int32_t score = int32_t((lo >> 12) | (hi << 20));
        if (bestScore < score) {
            bestScore = score;
            best = int(i);
        }
    }
    return best;
}

int ItemUnder(std::span<const MenuItem> items, int x, int y, const SelectContext& ctx, bool pageLoaded) {
    if (!pageLoaded) return -1;
    for (size_t i = 0; i < items.size(); i++) {
        const MenuItem& it = items[i];
        if (ItemSelectable(it, ctx) && it.x0 < x && x < it.x1 && it.y0 < y && y < it.y1) return int(i);
    }
    return -1;
}

int DefaultItem(std::span<const MenuItem> items, bool pageLoaded) {
    if (!pageLoaded) return -1;
    for (size_t i = 0; i < items.size(); i++)
        if (items[i].Has(kDefault)) return int(i);
    return -1;
}

} // namespace gt2::menu
