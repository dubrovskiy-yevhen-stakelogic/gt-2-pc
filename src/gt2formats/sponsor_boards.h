#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/overlay_data.h"

// Sponsor boards of the race: the logos the original puts over the course's own billboard textures at race load
// (docs/formats/sponsor_boards.md). Facts of US Simulation v1.2 (SCUS_944.88 SHA-1 3030aa27...; race overlay =
// GT2.OVL member 0), established from the loader 0x800275E8 and checked against the VRAM of the captured Seattle
// attract race (tools/gt2play --prims: every one of the 19 slots, image and CLUT, equals this placement).
namespace gt2 {

// `.crstims.tsd` (GT2.VOL root, gzip): u16 recordsPerCategory (90), u16 categoryCount (10), u16 groupCount[5]
// (47, 15, 14, 8, 6: logos per size group), u16 pad; then `categoryCount` categories of { char name[16];
// { u16 chance (0..4096), u16 logoId } record[recordsPerCategory] } - record i belongs to logo TIM i; then the
// TIMs: 5 blank templates (one per size group), followed by the logos of group 0, 1, ... (4-bit TIMs with a
// 16-colour CLUT; their own destinations are not used).
struct SponsorLogoTim {
    int group = 0;                  // size group 0..4
    uint16_t width = 0, height = 0; // image in 16-bit VRAM words (4-bit texels: 4 per word)
    std::vector<uint16_t> image;    // width * height words
    std::array<uint16_t, 16> clut{};
};

struct SponsorCategory {
    std::string name;                               // "General01", "General02", "One-Make", "JP", "US", "UK", "DE", "FR", "IT", "TUNE"
    std::vector<std::pair<uint16_t, uint16_t>> records; // per logo: {chance of 4096, logo id}
};

struct SponsorTable {
    std::vector<SponsorCategory> categories;
    std::array<SponsorLogoTim, 5> templates;        // the blank board of each size group
    std::vector<SponsorLogoTim> logos;              // all groups in order (record i = logos[i])
    std::array<uint16_t, 5> groupCounts{};
};

// Parses `.crstims.tsd` (inflated). Throws std::runtime_error.
SponsorTable ParseSponsorTable(std::span<const uint8_t> tsd);

// One board slot of the race overlay's table at 0x8002F5A4 (5 groups of 12 bytes: u32 slot list pointer,
// u16 clutX, u16 clutY, u16 TIM size, u8 slotCount; the list holds {u16 x, u16 y} per slot). Slot s of a group
// gets its image at (x, y) and its CLUT at (clutX, clutY + s).
struct SponsorSlotGroup {
    uint16_t clutX = 0, clutY = 0;
    std::vector<std::array<uint16_t, 2>> slots;
};
constexpr uint32_t kSponsorSlotTableAddress = 0x8002F5A4u;
std::array<SponsorSlotGroup, 5> LoadSponsorSlots(const GuestImage& raceOverlay);

// One upload of the placement: a logo (or a group's blank template) into a slot.
struct SponsorUpload {
    int group = 0, slot = 0;
    int logo = -1;                  // index into SponsorTable::logos, -1 = the group's template
    uint16_t logoId = 0;            // the record's id (0 for templates)
    uint16_t imageX = 0, imageY = 0, clutX = 0, clutY = 0;
    const SponsorLogoTim* tim = nullptr;
};

// The random generator of 0x80083AE0: state = state * 17 + 17; returns state ^ (state rotated by 16).
uint32_t SponsorRandom(uint32_t& state);

// 0x800275E8: no boards when bit 6 (0x40) of the course's .crsinfo flags is set; otherwise, with the category named
// `category` (the original: "General01" when the byte 0x801D5865 is 0, else the string at 0x801D58A0 that the menus
// set - "General02" in the attract race) and the generator seeded with `seed` (0x801D58B0: VSync(-1) at the load of a
// player's race, kept for the attract race / replays), for each size group g: a random permutation of its slots
// (draw until an unused slot), then for each logo of the group while slots remain one draw decides it - placed in the
// next slot of the permutation when (draw & 0xFFF) <= chance and its id is not 0x13; the remaining slots get the
// group's template. Returns the uploads in the original's order; empty when the category does not exist.
std::vector<SponsorUpload> PlaceSponsorBoards(const SponsorTable& table, const std::array<SponsorSlotGroup, 5>& slots, const std::string& category,
                                              uint32_t seed, uint16_t courseFlags);

// Writes the uploads into a 1024 x 512 VRAM image.
void ApplySponsorBoards(const std::vector<SponsorUpload>& uploads, std::vector<uint16_t>& vramWords);

} // namespace gt2
