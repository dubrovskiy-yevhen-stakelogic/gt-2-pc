#pragma once
// The pictures, fonts and strings of the title overlay (GT2.OVL member 1, "ovl1", loaded at 0x80010000): the title
// screen, the options, the save / load screens of the memory-card manager. Facts of US Simulation v1.2 (SCUS_944.88,
// EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), from our disassembly / Ghidra pseudo-C of RAM dumps taken with
// member 1 loaded (work/re/title) and the original's GP0 traffic (gt2play --prims); docs/formats/title.md.
//
// VRAM while the title overlay runs (all uploads by member 1; file ids are indices of the EXE file table 0x801E2EF0,
// resolved to VOL paths on the US disc):
//   0x80011178: file 0x16 arcade/arc_key_config.tim -> tpage 0x1D, file 0x0D arcade/arc_font.tim -> tpage 0x1E
//               (0x800110FC: the image block, after the CLUT block when the TIM has one, at the page, its own size)
//   0x800111DC: file 0x80023E7C[language] (0x45 = arcade/topmenu_panels_us.tim) -> tpage 0x0E,
//               file 0x3E arcade/title_item.tim(.gz) -> tpage 0x0C (the item names of the title list),
//               file 0x80023EA8[language] (0x3D = arcade/title_gtmode_us.tim(.gz)): its CLUT block to (384, 511),
//               its image block (8-bit, 352 x 480) to (384, 0)
//   0x80011BC4: file 0x0E arcade/arc_fontinfo at 0x800E15C0: u32 8 + eight offsets = four fonts {glyph table,
//               kerning table} in the format of the EXE text engine (hud_assets.h HudFont); descriptors 0x801B95F0
//               (cell 12, the headers), 0x801B9620 (cell 7), 0x801B95C0 (cell 5), 0x801B95D0 (cell 3); texture page
//               0x1E (0x8006AC68(ctx, 0x1E) -> 0x8007DC40), CLUT base 0x4038.
// Strings: data-title.txd (gzip at member 1 0x80021104; 0x80016254 copies the language's 0xE47-byte block to
// 0x801B9630) and data-global.txd (gzip at 0x80022D80; 0x8001DA08 copies the language's 0x7A9-byte block to
// 0x801EF6B0). The code addresses its strings by those RAM addresses.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/gt_menu_images.h"
#include "gt2formats/hud_assets.h"
#include "gt2formats/overlay_data.h"

namespace gt2 {

class DiscImage;
class GtfsVolume;
struct ExeProfile;

struct TitleAssets {
    static constexpr uint32_t kTitleTextBase = 0x801B9630u, kTitleTextStride = 0xE47;   // 0x80016254
    static constexpr uint32_t kGlobalTextBase = 0x801EF6B0u, kGlobalTextStride = 0x7A9; // 0x8001DA08
    static constexpr uint32_t kTitleTxdGzip = 0x80021104u, kGlobalTxdGzip = 0x80022D80u;
    static constexpr uint32_t kFontInfoAddress = 0x800E15C0u;                            // 0x80011868(0x800E15C0, 0x0E)
    static constexpr uint16_t kFontPage = 0x1E;
    static constexpr int kScreenWidth = 352, kScreenHeight = 480;                        // E3 0,0 / E4 351,479

    enum Font : int { kHeaderFont = 0, kMediumFont = 1, kSmallFont = 2, kTinyFont = 3 }; // 0x801B95F0 / 9620 / 95C0 / 95D0

    GuestImage ovl1;                       // member 1 (tables: the option rows, the list widgets, sprites, colours)
    GuestImage exe;                        // SCUS_944.88 (the card manager's button bar templates 0x80091F04..)
    MenuVram vram;                         // the VRAM of the title overlay's screens (above)
    std::vector<uint8_t> titleText, globalText; // the language blocks of the two txd files
    std::array<HudFont, 4> fonts;
    uint8_t language = 1;                  // career +0 (1 = USA)

    // Where Text() finds the two blocks. On the Simulation disc they sit at the Simulation addresses above. On another build
    // (LoadArcade) they sit at that build's addresses and `textProfile` is the build's profile: a Simulation string address
    // (the port names its strings so) goes through the profile first (ExeProfile::TryData, scope -1: the reference runs of
    // the lui-built string references and the string facts of db/<build>_symbols.yaml), a build text token (below) is taken
    // as it is. docs/formats/title.md section 8.
    uint32_t titleTextAt = kTitleTextBase, globalTextAt = kGlobalTextBase;
    const ExeProfile* textProfile = nullptr;
    // A string pointer of a build table copied to the Simulation layout (SimLayoutScreens): the build's text address + this
    // tag (such a string has no Simulation address of its own when the build's texts differ).
    static constexpr uint32_t kBuildTextTag = 0x10000000u;

    static TitleAssets Load(const DiscImage& disc, const GtfsVolume& vol, uint8_t language = 1);
    // The title of the US Arcade v1.1 disc (member 1 of its GT2.OVL, background arcade/title_arcade_us.tim; `ovl1` / `exe`
    // are that build's images with its profile) and its own data-title.txd / data-global.txd blocks (other builds of the
    // texts: 0x800161B4 copies block language * 0xDAE of the gzip at member 1 0x80020D88 to 0x801B9330, 0x8001D674 block
    // language * 0x76F of the gzip at 0x8002247C to 0x801EF0E0; db/arcade_us11_symbols.yaml).
    static TitleAssets LoadArcade(const DiscImage& disc, const GtfsVolume& vol, uint8_t language = 1);
    // The assets as the screens that name member 1's / the executable's tables by their Simulation addresses read them
    // (shell::OptionsScreen, CardManager, the key / analog pages): a copy of these assets. On the Simulation disc nothing
    // changes. On another build `ovl1` / `exe` become images at the Simulation addresses: the build's bytes copied through
    // its profile (aligned runs, then reference runs, then facts: ExeProfile::TryData's order); the pointers the screens follow
    // translated back (member 1's pointers into member 1 by the inverse of the copy, both images' string pointers to build
    // text tokens; other words stay: the executable's data holds values that look like its addresses); they then carry the
    // Simulation profile. Bytes no range covers stay 0.
    TitleAssets SimLayoutScreens() const;
    // The four fonts of arcade/arc_fontinfo (shared by the title and the arcade menus of both discs).
    static void LoadTitleFonts(TitleAssets& a, const GtfsVolume& vol);
    // The NUL-terminated string at a guest address of either text block ("" outside them).
    std::string Text(uint32_t address) const;
    bool HasText(uint32_t address) const;
    // The block and offset of a string address (the rules of titleTextAt / textProfile above); null outside the blocks.
    const std::vector<uint8_t>* Locate(uint32_t address, size_t& at) const;
};

// Parses the gzip member at `address` of a module image (header per RFC 1952, then the raw deflate stream).
std::vector<uint8_t> InflateEmbeddedGzip(const GuestImage& image, uint32_t address);
std::vector<uint8_t> InflateEmbeddedGzipNamed(const GuestImage& image, const char* name);
std::vector<uint8_t> EuropeanArcadeText(const std::vector<uint8_t>& data, bool global);

} // namespace gt2
