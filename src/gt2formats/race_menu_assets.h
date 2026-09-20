#pragma once
// The pictures, fonts and strings of the race overlay's full-screen menus (GT2.OVL member 0, "ovl0", loaded at
// 0x80010000): the licence test menu (view 0x8005B470, draw 0x8004F474), the event pre-race menu (draw 0x800585C0),
// the settings screens (parts pages 0x80052D84, machine settings 0x80056194). Facts of US Simulation v1.2
// (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), from our disassembly / Ghidra pseudo-C of RAM dumps
// taken with member 0 loaded (work/re/rs_licmenu, work/re/rs_menus) and the original's GP0 traffic (gt2play --prims).
//
// The screens are 352 x 480 frames (E3 0,0 / E4 351,479) drawn with the executable's text engine and widgets, like
// the title overlay (title_assets.h).
//
// VRAM (file ids index the EXE table 0x801E2EF0 -> VOL TOC record; checked word for word against the captures'
// VRAM work/play/racescreens/cap/licmenu_8100.txt.vram.bin and event_4900.txt.vram.bin):
//   0x80047B40: file 0x0D arcade/arc_font.tim -> RAM 0x8018EC00; 0x80047CAC uploads its image block (after the CLUT
//               block, flag bit 3) to (0x180, 0) = texture page 6 with the block's own size (128 x 256 words; the
//               glyph CLUTs are inside the image rows, CLUT base 0x18 = (384, 0)).
//   0x80047EA0 state 2: file 0x38 arcade/license_tim.tim when the race mode 0x801D5866 is 3 (0x80047C88: the
//               licence tests), file 0x39 arcade/setting.tim when it is 1 or 6..10 (0x80047C44: GT-mode event
//               races), loaded to 0x8018EC00; 0x80047CAC -> 0x80046FB0(0x8018EC00, 0x16): texture page 0x16 =
//               (384, 256) (license_tim 128 x 170 words: medals / licence sprites; setting 64 x 235: parts icons,
//               slider bars).
//   0x80047EA0 state 0: file 0x0E arcade/arc_fontinfo -> RAM 0x801A8C00 (u32 8, then eight offsets = four fonts
//               {glyph table, kerning table} in the format of the EXE text engine, hud_assets.h HudFont); state 1
//               sets the descriptors {u32 glyph, u32 kerning, u8 cell, u8 gap 0}: 0x801C9130 = +0x04 / +0x08 cell 12
//               (headers, "B-1"), 0x801C9150 = +0x0C / +0x10 cell 7 (the event rows), 0x801C9120 = +0x14 / +0x18
//               cell 5 (texts of the licence menu, list rows), 0x801C9110 = +0x1C / +0x20 cell 4; and the work
//               block pointer 0x801C90A0 = align4(0x801A8C00 + size of file 0x0E). Texture page 6 (0x8006AC68(ctx,
//               6)), CLUT base 0x18.
// Strings:
//   .text/data-race.txd: blocks of 0x1915 bytes, one per language; the race copies the US block (index 1, file
//   offset 0x1915) to RAM 0x801C6C50 (the dump's 0x1915 bytes equal the file's). The code addresses the strings by
//   those RAM addresses (0x801C7028 "License Test", 0x801C7097 "Start", 0x801C78A9 "Start Race", ...).
//   data-global.txd (gzip in ovl1 at 0x80022D80, block language * 0x7A9) stays at 0x801EF6B0 from the title
//   overlay (the race dumps' block equals it): the licence labels "B-%d" etc. (0x801EFE38.., table ovl0 0x8005B220).
//   arcade/license_info_us: the licence tests' titles and descriptions, loaded to 0x80173894 (0x8004CCF8):
//   u16 6 licences, u16 10 tests, u32 0x4B4 (header size), then 60 records of 20 bytes {u16 title offset, u16 line
//   count, u16 line offsets[8]} (offsets from the file start), record = licence * 10 + test; the licences in the
//   order of 0x801D5867 (0 S, 1 IA, 2 IB, 3 IC, 4 A, 5 B).
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

struct RaceMenuAssets {
    static constexpr int kScreenWidth = 352, kScreenHeight = 480;
    static constexpr uint32_t kRaceTextBase = 0x801C6C50u, kRaceTextStride = 0x1915;
    static constexpr uint32_t kGlobalTextBase = 0x801EF6B0u, kGlobalTextStride = 0x7A9;
    static constexpr uint32_t kGlobalTxdGzip = 0x80022D80u; // in ovl1 (the title overlay)
    static constexpr uint32_t kFontInfoAddress = 0x801A8C00u;
    static constexpr uint32_t kLicenceInfoAddress = 0x80173894u;
    static constexpr uint16_t kFontPage = 6, kPicturePage = 0x16;

    // The font descriptors of the race overlay (0x80047EA0 state 1).
    enum Font : int { kHeaderFont = 0, kMediumFont = 1, kSmallFont = 2, kTinyFont = 3 };
    static constexpr uint32_t kFontDescriptors[4] = {0x801C9130u, 0x801C9150u, 0x801C9120u, 0x801C9110u};
    static constexpr uint8_t kFontCells[4] = {12, 7, 5, 4};

    // Which picture file is on texture page 0x16 (0x80047EA0 state 2).
    enum class Pictures : uint8_t { kLicence, kSettings };

    GuestImage ovl0;                       // member 0 (tables, templates, widgets of the menus)
    GuestImage exe;                        // SCUS_944.88
    MenuVram vram;
    Pictures pictures = Pictures::kLicence;
    std::vector<uint8_t> raceText, globalText; // language blocks (RAM 0x801C6C50 / 0x801EF6B0)
    std::vector<uint8_t> licenceInfo;          // arcade/license_info_us (RAM 0x80173894)
    std::array<HudFont, 4> fonts;
    uint8_t language = 1;                  // 1 = USA

    static RaceMenuAssets Load(const DiscImage& disc, const GtfsVolume& vol, Pictures pictures, uint8_t language = 1);

    // The NUL-terminated string at a guest address of the race or global text block, or of ovl0's image (its own
    // strings such as 0x8005A8D8 "LIS%02d"); "" elsewhere.
    std::string Text(uint32_t address) const;
    bool HasText(uint32_t address) const;
    const HudFont& FontAt(uint32_t descriptor) const; // one of kFontDescriptors

    // 0x8004CCF8: the title and description lines of licence `licence` (0 S .. 5 B), test `test` (0..9).
    struct LicenceInfo {
        std::string title;
        std::vector<std::string> lines;
    };
    LicenceInfo Licence(int licence, int test) const;
};

// Uploads the image block of a TIM (after its CLUT block when flag bit 3) at (x, y) with its own size, as
// 0x80047CAC / 0x80046FB0 do (the TIM's destination is not used).
void UploadTimImage(MenuVram& vram, const std::vector<uint8_t>& tim, int x, int y);

} // namespace gt2
