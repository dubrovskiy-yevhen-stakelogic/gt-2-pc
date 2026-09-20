#pragma once
// GT-mode menu pages ("GM" pages of gtmenu/<lang>/gtmenudat.dat): header, sprite groups, items (rectangle, flags,
// type code, target / name / car id), the page picture block, and the meaning of the item types as the GT-mode
// overlay (GT2.OVL member 4, loaded at 0x80010000) draws them (0x8001B9AC) and acts on them (0x80014380).
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), established from the
// bytes of the disc and our disassembly of member 4; the full description with evidence is docs/formats/gt_menu.md.
// Pictures (commonpic GTMP, the page's own 4-bit tiles, fonts) and the software renderer: gt_menu_images.h.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/gtmode_tables.h"

namespace gt2 {

class GtfsVolume;

// One 12-byte sprite record (group sprites, and the alternative sprite of type-1 items at item +0x10): drawn by
// 0x800220C8 as a SPRT (0x64, colour 0x808080) after a texpage command `tpage & 0x1F` (4-bit texels):
// s16 x, s16 y, u8 u, u8 v, u8 w, u8 h, u16 tpage, u16 clut.
struct MenuSprite {
    int16_t x = 0, y = 0;
    uint8_t u = 0, v = 0, w = 0, h = 0;
    uint16_t tpage = 0, clut = 0;
};

// Flag bits of an item (+0x08; bits 0..15 are the type code).
namespace menu_item_flag {
constexpr uint32_t kTypeMask = 0xFFFFu;
constexpr uint32_t kLicenceGated = 1u << 18;  // type 0: shown (badge 0x80050A50) and selectable only when licence +0x4A is held (0x8001915C)
constexpr uint32_t kEventResult = 1u << 19;   // event item: result position / medal (0x8005DB90)
constexpr uint32_t kEventLicence = 1u << 20;  // event item: required licence icon (0x80019634)
constexpr uint32_t kEventPower = 1u << 21;    // event item: power limit "~%dhp" / "free" (0x800196C8)
constexpr uint32_t kEventPrize = 1u << 22;    // event item: prize of position +0x4B (0x800196EC)
constexpr uint32_t kEventTrophy = 1u << 23;   // event item: championship result (trophy when first)
constexpr uint32_t kAction = 1u << 24;        // generic "go to target page" (plus the checks of types 0x98 / 0x9C / 0xAD / 0xAE)
constexpr uint32_t kDefault = 1u << 25;       // the cursor starts on this item (0x8001D6CC)
constexpr uint32_t kCarPrice = 1u << 26;      // set on all type-6 items (not tested by the overlay)
constexpr uint32_t kEvent = 1u << 27;         // +0x0C is an event / licence-test name: cross starts it (0x8001973C / 0x80019B88)
constexpr uint32_t kWheel = 1u << 28;         // +0x0C is a wheel code ("bb001--s"): buy wheels (0x80013A28, price 2000)
constexpr uint32_t kHidePrice = 1u << 29;     // type 6: no price text
constexpr uint32_t kRacePath3 = 1u << 30;     // event item: the race starts with 0x801EF5F5 = 3 (no entry check)
constexpr uint32_t kSecondFont = 1u << 31;    // text in the second font (0x8005129C / 0x80052040: 28-px digits)
} // namespace menu_item_flag

// One 76-byte item (the interpreter's copy at 0x801C3150 + i * 0x4C, at most 64 per page; 0x800215C8(i)).
struct MenuItem {
    static constexpr size_t kSize = 0x4C;
    int16_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;   // +00 rectangle (screen pixels, 512 x 448 page)
    uint32_t flags = 0;                        // +08
    std::array<uint8_t, 0x40> data{};          // +0C..+4B: the item's data (union, see Target / Name / CarId)
    int group = -1;                            // the sprite group the item came with, -1 = the page's own list

    uint16_t Type() const { return uint16_t(flags & menu_item_flag::kTypeMask); }
    bool Has(uint32_t flag) const { return (flags & flag) != 0; }
    uint32_t Word(size_t itemOffset) const;    // u32 at item + itemOffset (0x0C..0x48)
    uint32_t Target() const { return Word(0x0C); }   // target page id (bit 31 = message page)
    std::string Name() const;                        // NUL-terminated text at +0x0C (event / licence / wheel code)
    uint32_t CarId() const { return Word(0x10); }    // type 6: packed car id
    MenuSprite AltSprite() const;                    // type 1: sprite record at +0x10
    uint8_t Byte4A() const { return data[0x4A - 0x0C]; } // licence index of kLicenceGated (0 -> licence 1, 1 -> licence 0)
    uint8_t Byte4B() const { return data[0x4B - 0x0C]; } // prize position of kEventPrize (1..6)
    int CenterX() const { return (x0 + x1) >> 1; }
    int CenterY() const { return (y0 + y1) >> 1; }
};

// A sprite group of the page: sprites drawn by 0x800220C8 plus the items that come with them.
struct MenuGroup {
    std::vector<MenuSprite> sprites;
    uint16_t itemCount = 0;
};

// The picture block that ends a page (0x80021284): "GMLL" (not read), u32 k, k tile words, a 64 x 8 CLUT block
// (VRAM 576, 248: 32 CLUTs of 16 colours), u32 n = number of 16 x 8 4-bit tiles, then (n / 32 + 1) * 8 rows of
// 256 bytes (128 VRAM words); the game uploads ceil(n / 32) * 8 of them to VRAM (640, 256) (0x80021284; the files
// hold one unused strip more when n is a multiple of 32: 93 pages).
struct MenuPagePicture {
    uint32_t magic = 0;
    std::vector<uint32_t> tiles;                // 0x8002202C: bit 7 set = flat 16 x 8 tile, else a 4-bit 16 x 8 texture tile
    std::array<uint16_t, 64 * 8> clut{};
    uint32_t textureTiles = 0;
    std::vector<uint8_t> pixels;                // stored rows x 256 bytes
    int Rows() const { return int((textureTiles + 31) / 32) * 8; } // rows uploaded
};

// One GM page (parsed by 0x800213C4).
struct MenuPage {
    uint32_t id = 0;
    std::vector<MenuGroup> groups;
    std::vector<MenuItem> items;                // 0x801C3150 order: the items of group 0, 1, ..., then the page's own
    uint32_t flags = 0;                         // -> 0x800A8D78
    uint32_t back = 0xFFFFFFFFu;                // -> 0x800A8D70: page of the back button (circle); -1 none
    uint32_t picture = 0;                       // commonpic entry of the background (reloaded only when it changes)
    MenuPagePicture own;

    uint8_t Maker() const { return uint8_t(flags & 0xFF); }        // used-car list maker (0x80020A0C), type-0x98 check
    bool KeepsHistory() const { return (flags & 0x100) != 0; }     // 0x8001D2CC: back returns to the previous page
    bool ClearsBehind() const { return (flags & 0x200) != 0; }     // black flat tiles skipped, the frame is cleared (0x8001B9AC): 3D car shows through
    uint8_t Music() const { return uint8_t(flags >> 16); }          // menu music track (0x80018FA4 -> 0x80052A50[track]); 0xFF keep
    const MenuItem* DefaultItem() const;                            // first item with kDefault (0x8001D6CC)
};

// Throws std::runtime_error when the bytes are not a well-formed page (every field is consumed to the last byte).
MenuPage ParseMenuPage(std::span<const uint8_t> bytes, uint32_t id = 0);

// Short name of an item type code (e.g. "money", "buy part 0x05"), "" when the overlay does nothing with it.
std::string MenuItemTypeName(uint16_t type);
// Handler of the type in 0x80014380 / 0x8001B9AC as text ("action 0x80014380 case 2 -> 0x8001DDAC ..."); "" if none.
std::string MenuItemTypeHandler(uint16_t type);

// The page cursor's nearest-item search in one direction (0x8001D754): direction 2 up, 3 down, 4 left, 5 right,
// 6 up-right, 7 up-left, 8 down-right, 9 down-left (vectors 0x80051274); scores items by cos^2 / distance from
// (x, y) to their centre, ignoring items behind. `selectable(item)` = 0x8001B6F0 && 0x8001B680.
const MenuItem* MenuNearestItem(const MenuPage& page, int x, int y, int direction, const MenuItem* current,
                                bool (*selectable)(const MenuItem&) = nullptr);
// 0x8001B6F0 without career state: the item types the cursor can rest on (licence-gated items count as selectable).
bool MenuItemSelectable(const MenuItem& item);

// gtmenu/<lang>/gtmenudat.{idx,dat} + solodata.dat of one language: pages by id; ids with bit 31 set are message
// pages (0x800211FC: solodata page list entry id & 0x7FFFFFFF).
class MenuPages {
public:
    static MenuPages Load(const GtfsVolume& vol, const std::string& language = "usa");
    size_t Count() const { return index_.Count(); }
    uint32_t Resolve(uint32_t id) const;         // message ids -> gtmenudat entry
    std::vector<uint8_t> Bytes(uint32_t id) const; // inflated page (id resolved)
    MenuPage Page(uint32_t id) const;
    const SoloData& Solo() const { return solo_; }
private:
    MenuPackIndex index_;
    std::vector<uint8_t> dat_;
    SoloData solo_;
};

} // namespace gt2
