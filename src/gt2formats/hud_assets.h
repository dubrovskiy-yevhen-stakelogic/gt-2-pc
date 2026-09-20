#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/overlay_data.h"
#include "gt2vfs/gtfs.h"

// The graphics and strings of the in-race HUD (docs/formats/hud.md). Facts of US Simulation v1.2 (SCUS_944.88,
// SHA-1 3030aa27...), established from the disc's bytes and the original's GP0 traffic in the Seattle attract
// race (gt2play --prims 3600): the race font, the gauge sheet, the tachometer dial faces and the caption strings.
namespace gt2 {

// One glyph of the race font as the executable's table at 0x80096260 describes it (4 bytes per character code:
// u8 u, u8 v, u16 packed = width / 2 (bits 0-4) | height (bits 5-8) | 0x1000 (present) | 0x2000 (u + 256)).
struct RaceFontGlyph {
    uint16_t x = 0;    // texel column in the 512-texel-wide font image (0..511)
    uint8_t y = 0;     // texel row
    uint8_t width = 0, height = 0;
    bool present = false;
};

// font/racefont.dat: a raw 4-bit image of 128 words x 256 rows (256 x 256 texels) that the game uploads to VRAM
// (384, 0) unchanged (the capture's VRAM equals the file word for word). Row 0 holds eight 16-entry CLUTs; the
// text of the HUD uses CLUT 2 (VRAM (416, 0), i.e. the GPU CLUT word 0x001A): a 16-step grey ramp with entry 1 =
// 0x8000 (the glyphs' dark outline) and entry 0 transparent.
struct RaceFont {
    static constexpr int kWords = 128, kRows = 256, kVramX = 384, kVramY = 0;
    static constexpr int kClutIndex = 2;              // the HUD's CLUT of row 0
    static constexpr int kLineHeight = 9;             // the tallest glyph; glyphs sit on the line's bottom
    static constexpr uint32_t kGlyphTableAddress = 0x80096260u;
    static constexpr uint32_t kGlyphFirst = 0x1A, kGlyphCount = 0xC6; // codes 0x1A..0xC5 are glyphs; the words around the table are code
    std::vector<uint16_t> words;                      // 128 x 256
    std::array<RaceFontGlyph, 256> glyphs{};

    // Texel (0..15) of the font image.
    uint8_t Texel(int x, int y) const { return uint8_t((words[size_t(y) * kWords + size_t(x) / 4] >> ((x & 3) * 4)) & 15); }
    // Ink rule of the text layout (kerning): the glyph body, entries >= 2 (entry 1 is the outline).
    bool Ink(const RaceFontGlyph& g, int column, int row) const { return Texel(g.x + column, g.y + row) >= 2; }
    // Vertical offset of a glyph from the top of a text line (bottom-aligned on the line, descenders one lower).
    int GlyphTop(char c) const;
    // The x advance from glyph `a` to glyph `b` in proportional text: the glyph bodies are packed with a one
    // texel gap (reproduces the captured captions "Lap", "Total Time", "Lap Time", "Replay"; see hud.md).
    int Advance(char a, char b) const;
    int TextWidth(const std::string& text) const;
};

// Loads font/racefont.dat and the glyph table of the resident executable. Throws on inconsistent data.
RaceFont LoadRaceFont(const GtfsVolume& vol, const GuestImage& exe);

// arcade/game_status_files(.gz): u32 count = 11, u32 offset[count], then the members. Member 0 is a TIM without
// a CLUT (4-bit, 64 x 234 words = 256 x 234 texels, file destination (0, 0)) that the game uploads to VRAM
// (512, 0): the gauge sheet - six tachometer ring variants, the speed digits, the "0123456789R/" 16 x 16 strip,
// the position badges, km/h / mph, SEIKO, the light. Its rows 224..233 are raw CLUT words: the HUD's CLUTs at
// (512, 229) (grey ramp, GPU word 0x3960) and (544, 230) (inverted ramp, 0x39A2). Members 1..10 are TIMs with a
// 16-entry CLUT (file destination (0, 480)) and a 20 x 80 word image (80 x 80 texels, file destination (768, 0)):
// the dial faces for rev limits 6, 7, 8, 9, 10, 11, 12, 14, 16 and 18 thousand rpm.
struct HudSheet {
    static constexpr int kSheetWords = 64, kSheetRows = 234, kSheetVramX = 512, kSheetVramY = 0;
    static constexpr int kDialWords = 20, kDialRows = 80, kDialCount = 10;
    std::vector<uint16_t> sheet;                       // 64 x 234 words
    struct Dial {
        int limitRpm = 0;                              // 6000, 7000, ... 18000
        std::array<uint16_t, 16> clut{};
        std::vector<uint16_t> words;                   // 20 x 80
    };
    std::vector<Dial> dials;                           // 10 faces in file order
    // The face whose scale is the smallest one at or above `revLimitRpm`.
    const Dial& DialFor(int revLimitRpm) const;
};

HudSheet LoadHudSheet(const GtfsVolume& vol);

// .text/data-race.txd: NUL-terminated strings, one block per language; the executable copies the first block to
// 0x801C6C50 at race load (with the unit strings patched for the region), and the race shell's caption "tokens"
// (car record + 0xA98, race_shell.h kLapTimeLabel, ShellGlobals::splitLabels) are pointers into that copy.
struct HudStrings {
    static constexpr uint32_t kBase = 0x801C6C50u;
    uint32_t base = kBase; // the copy's address in the disc's build (US Arcade v1.1: 0x801C6940; set by the caller from the profile)
    std::vector<uint8_t> bytes;
    // The string at `token` (a guest pointer into the copy); empty when outside the file.
    std::string At(uint32_t token) const;
    // The first string equal to `text` (its token), 0 when absent.
    uint32_t Find(const std::string& text) const;
};

HudStrings LoadHudStrings(const GtfsVolume& vol);
// The block the race overlay really copies (0x80028CC0: file id 8, offset language * 0x1915, 0x1915 bytes to
// 0x801C6C50; language = career + 0, 1 = USA). Block 0 (above) differs from the US block in texts the race screens use
// ("FAIL" / "FAILED", ...); the HUD captions it serves are equal in both.
constexpr uint32_t kRaceTextBlockSize = 0x1915;
HudStrings LoadHudStrings(const GtfsVolume& vol, uint8_t language);
// US Arcade v1.1 (SCUS_944.55, EXE SHA-1 231f9dba...): its race overlay's loader (member 0 0x80028C4C: slot 8 = file 0x0C,
// offset language * 5759 computed as ((3 l * 16 - 3 l) << 7) - l) copies blocks of 0x167F bytes (7 in the file) to 0x801C6940.
constexpr uint32_t kArcadeRaceTextBlockSize = 0x167F;
// The block of `language` in blocks of `blockSize` bytes (the build's; base = HudStrings::kBase, set by the caller).
HudStrings LoadHudStrings(const GtfsVolume& vol, uint8_t language, uint32_t blockSize);

// ---------------------------------------------------------------- the race's text engine
// Ported from the race overlay / executable (US v1.2): 0x8007DD3C draws one character, 0x8007DC78 is the advance
// from one character to the next (per-glyph width minus a class-pair kerning value), and the layout helpers
// 0x8006AC90 (text), 0x8006AD3C (its width), 0x8006AE28 (right-aligned), 0x8006B360 / 0x8006B218 / 0x8006B3F4
// (times: fixed cells, narrow around ':' and '.'), 0x8006B044 / 0x8006AF40 / 0x8006B184 (numbers with a fixed
// digit cell). A font descriptor (0x80093124 = the HUD's font, 0x80093130 = the large digits) is {u32 glyph table,
// u32 kerning table, u8 cell, u8 gap}; the glyph table holds per character code two words:
//   w0: bits 0-5 advance, 6-13 sprite index (0 = no sprite), 14-16 accent index, 17-22 centring offset (code
//       flag 0x100), 23-30 left kerning class (bit 31 set: bits 23-28 are a fixed kerning value);
//   w1: bits 0-5 x offset, 6-11 height above the baseline, 12-17 accent x, 18-22 accent y, 23-30 right kerning class
//       (bit 31: fixed value as for w0);
// then (at +0x7FC) the accent sprites 1..7 and (at +0x818) the sprites: u8 u, u8 v, bits 16-20 width / 2, 21-26
// height, 27-28 CLUT offset, 29-31 texture page offset. The kerning table: u32 {u8 row stride, u8 pad, u8 bits per
// entry} + rows of packed entries; kern(a, b) = row class(a), entry class(b).
struct HudFontSprite {
    int x = 0, y = 0, w = 0, h = 0, u = 0, v = 0;
    uint16_t tpage = 0, clut = 0; // GPU words: texture page (the draw mode the glyph packet sets) and CLUT
};

class HudFont {
public:
    // 0x8009313C: the START / countdown font of 0x8002A19C (large race-font glyphs, tpage 6, CLUT 0x001A).
    static constexpr uint32_t kSmallFont = 0x80093124u, kLargeFont = 0x80093130u, kStartFont = 0x8009313Cu;
    static constexpr uint32_t kCentre = 0x100, kRightAlign = 0x200;

    uint8_t cell = 8, gap = 1;
    uint16_t tpage = 6, clutBase = 0x18; // 0x8007DC40(-1): page = *0x80093148 (6 in the race), CLUT = (page & 15) * 4 + (page & 0x10) * 0x400

    // 0x8007DD3C: the sprites of character `code & 0xFF` with the flags of `code`, pen at x, baseline y.
    void Glyph(uint32_t code, int x, int y, std::vector<HudFontSprite>& out) const;
    int Advance(uint8_t a, uint8_t b) const; // 0x8007DC78

    int Text(const std::string& s, int x, int y, int spacing, std::vector<HudFontSprite>& out) const;          // 0x8006AC90
    int TextWidth(const std::string& s, int spacing) const;                                                     // 0x8006AD3C
    int TextRight(const std::string& s, int right, int y, int spacing, std::vector<HudFontSprite>& out) const;   // 0x8006AE28 (returns the width)
    int TimeWidth(const std::string& s, int advance, int narrow, int signFlag) const;                          // 0x8006B360
    int Time(const std::string& s, int x, int y, int advance, int narrow, int signFlag, int dotShift, std::vector<HudFontSprite>& out) const; // 0x8006B218
    int TimeRight(const std::string& s, int right, int y, int advance, int narrow, int signFlag, int dotShift, std::vector<HudFontSprite>& out) const; // 0x8006B3F4
    int NumberWidth(const std::string& s, int spacing, int digitExtra) const;                                   // 0x8006B044
    int Number(const std::string& s, int x, int y, int spacing, int digitShift, int digitExtra, std::vector<HudFontSprite>& out) const; // 0x8006AF40
    int NumberRight(const std::string& s, int right, int y, int spacing, int digitShift, int digitExtra, std::vector<HudFontSprite>& out) const; // 0x8006B184

    friend HudFont LoadHudFont(const GuestImage& exe, uint32_t descriptor);

private:
    uint32_t GlyphWord(uint8_t c, int word) const;
    uint32_t Word(uint32_t address) const;
    uint32_t glyphTable_ = 0, kerningTable_ = 0, base_ = 0;
    std::vector<uint8_t> bytes_; // a copy of the executable's font tables
};

// Reads a font descriptor (HudFont::kSmallFont / kLargeFont) and its tables from the executable.
HudFont LoadHudFont(const GuestImage& exe, uint32_t descriptor);

// 0x80068734: a time in 1/1000 s as the HUD prints it: "M:SS.mmm" (MM when >= 10 min), "H:MM:SS.d" from one hour,
// "--:--:---" for -1 (and beyond 100 hours).
std::string FormatRaceTime(uint32_t ms);
// 0x80068CA0: a speed readout (1/100 units, clamped to 99999) as "%d.%d" of (v / 100, v % 100) - e.g. 5607 -> "56.7".
std::string FormatRaceSpeed(uint32_t readout);

// A sprite descriptor of the race overlay's HUD tables: {u8 u, u8 v, u16 clut, u16 w, u16 h, u16 tpage, u16 pad}.
struct HudSpriteDesc {
    int u = 0, v = 0, w = 0, h = 0;
    uint16_t clut = 0, tpage = 0;
};

// The HUD's data tables of the race overlay (GT2.OVL member 0, loaded at 0x80010000).
struct HudTables {
    std::array<HudSpriteDesc, 6> faces;      // 0x8002F630: tachometer face slots in the sheet (80 x 80)
    std::array<HudSpriteDesc, 10> speedDigits; // 0x8002F678
    HudSpriteDesc unitKmh, unitMph;           // 0x8002F6F0 / 0x8002F6FC (the US game uses mph)
    std::array<HudSpriteDesc, 12> strip;      // 0x8002F708: "0123456789R/" (gear and lap counter)
    std::array<HudSpriteDesc, 6> badges;      // 0x8002F798: position 1st..6th
    std::array<HudSpriteDesc, 2> turbo;       // 0x8002F7E0 face, 0x8002F7EC back plate
    std::array<int8_t, 10> dialLimits{};      // 0x8002F868: thousands of rpm of the ten dial faces (6 .. 18)
    std::array<int8_t, 10> gearGlyph{};       // 0x8002F874: strip glyph per gear (0 = reverse -> 'R')
    struct Needle { uint32_t color0 = 0, color1 = 0; int32_t radius0 = 0, radius1 = 0, half0 = 0, half1 = 0; };
    std::array<Needle, 4> needles;            // 0x8002F880 / 0x8002F898 tachometer, 0x8002F8B0 / 0x8002F8C8 turbo
    uint32_t barColor0 = 0, barColor1 = 0;    // 0x8002F8E8 / 0x8002F8EC: the caption underline's gouraud colours
    int16_t barHeight = 0;                    // 0x8002F8E6
};
HudTables LoadHudTables(const GuestImage& raceOverlay);

// crsmap/<course>.tim(.gz): the course map of the HUD, a 4-bit TIM (96 x 96 texels, CLUT 16 x 1) that the game
// uploads to VRAM (576, 144) with its CLUT at (368, 508) whatever the file's own destinations (checked: the attract
// race's and the license test's captured VRAM equal the file words).
struct CourseMap {
    static constexpr int kWords = 24, kRows = 96, kVramX = 576, kVramY = 144, kClutX = 368, kClutY = 508;
    std::array<uint16_t, 16> clut{};
    std::vector<uint16_t> words; // 24 x 96
};
// Throws when the course has no map file.
CourseMap LoadCourseMap(const GtfsVolume& vol, const std::string& courseName);

} // namespace gt2
