#include "gt2formats/sponsor_boards.h"

#include <stdexcept>

namespace gt2 {
namespace {

uint16_t U16(std::span<const uint8_t> d, size_t o) {
    if (o + 2 > d.size()) throw std::runtime_error("crstims.tsd: read out of bounds");
    return uint16_t(d[o] | (d[o + 1] << 8));
}
uint32_t U32(std::span<const uint8_t> d, size_t o) { return uint32_t(U16(d, o)) | (uint32_t(U16(d, o + 2)) << 16); }

// A 4-bit TIM with CLUT at `o`; returns the offset past it.
size_t ReadTim(std::span<const uint8_t> d, size_t o, SponsorLogoTim& tim) {
    if (U32(d, o) != 0x10 || U32(d, o + 4) != 8) throw std::runtime_error("crstims.tsd: expected a 4-bit TIM with CLUT");
    size_t p = o + 8;
    const uint32_t clutLength = U32(d, p);
    if (U16(d, p + 8) != 16 || U16(d, p + 10) != 1 || clutLength != 12 + 32) throw std::runtime_error("crstims.tsd: unexpected CLUT block");
    for (size_t i = 0; i < 16; i++) tim.clut[i] = U16(d, p + 12 + i * 2);
    p += clutLength;
    const uint32_t imageLength = U32(d, p);
    tim.width = U16(d, p + 8);
    tim.height = U16(d, p + 10);
    if (imageLength != 12 + size_t(tim.width) * tim.height * 2) throw std::runtime_error("crstims.tsd: unexpected image block");
    tim.image.resize(size_t(tim.width) * tim.height);
    for (size_t i = 0; i < tim.image.size(); i++) tim.image[i] = U16(d, p + 12 + i * 2);
    return p + imageLength;
}

} // namespace

SponsorTable ParseSponsorTable(std::span<const uint8_t> d) {
    SponsorTable t;
    const uint16_t perCategory = U16(d, 0), categoryCount = U16(d, 2);
    size_t logoTotal = 0;
    for (size_t g = 0; g < 5; g++) {
        t.groupCounts[g] = U16(d, 4 + g * 2);
        logoTotal += t.groupCounts[g];
    }
    if (logoTotal != perCategory) throw std::runtime_error("crstims.tsd: record count differs from the logo count");
    size_t o = 0x10;
    for (uint16_t c = 0; c < categoryCount; c++) {
        SponsorCategory cat;
        for (size_t i = 0; i < 16 && o + i < d.size() && d[o + i] != 0; i++) cat.name.push_back(char(d[o + i]));
        for (uint16_t r = 0; r < perCategory; r++) cat.records.push_back({U16(d, o + 16 + r * 4u), U16(d, o + 18 + r * 4u)});
        t.categories.push_back(std::move(cat));
        o += 16 + size_t(perCategory) * 4;
    }
    for (int g = 0; g < 5; g++) {
        t.templates[size_t(g)].group = g;
        o = ReadTim(d, o, t.templates[size_t(g)]);
    }
    for (int g = 0; g < 5; g++)
        for (uint16_t i = 0; i < t.groupCounts[size_t(g)]; i++) {
            SponsorLogoTim tim;
            tim.group = g;
            o = ReadTim(d, o, tim);
            if (tim.width != t.templates[size_t(g)].width || tim.height != t.templates[size_t(g)].height)
                throw std::runtime_error("crstims.tsd: logo size differs from its group's template");
            t.logos.push_back(std::move(tim));
        }
    if (o != d.size()) throw std::runtime_error("crstims.tsd: trailing bytes");
    return t;
}

std::array<SponsorSlotGroup, 5> LoadSponsorSlots(const GuestImage& overlay) {
    std::array<SponsorSlotGroup, 5> groups;
    for (uint32_t g = 0; g < 5; g++) {
        const uint32_t a = overlay.Sim(kSponsorSlotTableAddress) + g * 12; // the table of the overlay's build
        const uint32_t list = overlay.Get<uint32_t>(a);
        groups[g].clutX = overlay.Get<uint16_t>(a + 4);
        groups[g].clutY = overlay.Get<uint16_t>(a + 6);
        const uint8_t count = overlay.Get<uint8_t>(a + 10);
        if (count > 8) throw std::runtime_error("sponsor slot table: more than 8 slots in a group"); // the permutation buffer is 8 bytes
        for (uint32_t s = 0; s < count; s++) groups[g].slots.push_back({overlay.Get<uint16_t>(list + s * 4), overlay.Get<uint16_t>(list + s * 4 + 2)});
    }
    return groups;
}

uint32_t SponsorRandom(uint32_t& state) {
    state = state * 17u + 17u;
    return state ^ ((state << 16) | (state >> 16));
}

std::vector<SponsorUpload> PlaceSponsorBoards(const SponsorTable& table, const std::array<SponsorSlotGroup, 5>& slots, const std::string& category,
                                              uint32_t seed, uint16_t courseFlags) {
    std::vector<SponsorUpload> out;
    if (courseFlags & 0x40) return out;
    const SponsorCategory* cat = nullptr;
    for (const SponsorCategory& c : table.categories)
        if (c.name == category) cat = &c; // the loader keeps the last match
    if (!cat || category.empty()) return out;
    uint32_t state = seed;
    size_t record = 0;
    for (int g = 0; g < 5; g++) {
        const SponsorSlotGroup& group = slots[size_t(g)];
        const uint32_t count = uint32_t(group.slots.size());
        std::array<int, 8> order;
        order.fill(-1);
        for (uint32_t i = 0; i < count; i++) {
            uint32_t r;
            do r = SponsorRandom(state); while (order[r % count] != -1);
            order[r % count] = int(i);
        }
        uint32_t placed = 0;
        auto put = [&](int logo, uint16_t id, const SponsorLogoTim* tim) {
            const int s = order[placed++];
            SponsorUpload u;
            u.group = g;
            u.slot = s;
            u.logo = logo;
            u.logoId = id;
            u.imageX = group.slots[size_t(s)][0];
            u.imageY = group.slots[size_t(s)][1];
            u.clutX = group.clutX;
            u.clutY = uint16_t(group.clutY + s);
            u.tim = tim;
            out.push_back(u);
        };
        for (uint16_t i = 0; i < table.groupCounts[size_t(g)]; i++, record++) {
            if (placed >= count) continue; // no more draws once the slots are full
            const uint32_t r = SponsorRandom(state);
            const auto [chance, id] = cat->records[record];
            if (id != 0x13 && (r & 0xFFF) <= chance) put(int(record), id, &table.logos[record]);
        }
        while (placed < count) put(-1, 0, &table.templates[size_t(g)]);
    }
    return out;
}

void ApplySponsorBoards(const std::vector<SponsorUpload>& uploads, std::vector<uint16_t>& vram) {
    constexpr size_t kWidth = 1024, kHeight = 512;
    if (vram.size() < kWidth * kHeight) throw std::runtime_error("ApplySponsorBoards: VRAM image too small");
    for (const SponsorUpload& u : uploads) {
        const SponsorLogoTim& t = *u.tim;
        for (size_t y = 0; y < t.height; y++)
            for (size_t x = 0; x < t.width; x++) vram[((u.imageY + y) % kHeight) * kWidth + (u.imageX + x) % kWidth] = t.image[y * t.width + x];
        for (size_t i = 0; i < 16; i++) vram[(u.clutY % kHeight) * kWidth + (u.clutX + i) % kWidth] = t.clut[i];
    }
}

} // namespace gt2
