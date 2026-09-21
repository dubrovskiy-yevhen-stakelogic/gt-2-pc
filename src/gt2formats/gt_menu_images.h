#pragma once
// Pictures of the GT-mode menus and a small software renderer of GM pages (tools / inspection; the product UI is
// native). Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), from the disc's
// bytes and our disassembly of GT2.OVL member 4 (GT-mode menus at 0x80010000); verified against the VRAM and the
// GP0 primitives of the original (gt2play --prims). Formats and evidence: docs/formats/gt_menu.md.
//
// VRAM layout while a GM page is shown (1024 x 512 words):
//   (0, 504)   512 x 8   commonpic CLUTs: 16 CLUTs of 256 colours (8002136C)
//   (768, 0)   256 x N   commonpic pixels, 8-bit, rows of 512 texels (streamed in 4 KB chunks, 0x80021608)
//   (576, 0)   64 x 156  arcade/gt_cursor.tim  (tpage 9: cursors; CLUT rows inside, 0x80013B60)
//   (640, 0)   64 x 247  arcade/gtmode_font.tim (tpage 10: both menu fonts, CLUT rows inside)
//   (704, 0)   64 x 256  gtmenu/<lang>/iconimg.dat (tpage 11: group sprites, CLUT rows inside; 0x80020ECC)
//   (512, 256) 64 x 157  arcade/gt_items.tim   (tpage 0x18)
//   (576, 248) 64 x 8    the page's CLUTs: 32 CLUTs of 16 colours (0x80021284)
//   (640, 256) 128 x N   the page's pixels, 4-bit, rows of 512 texels (0x80021284)
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "gt2formats/gt_menu.h"
#include "gt2formats/overlay_data.h"

namespace gt2 {

class GtfsVolume;

// gtmenu/commonpic.dat entry (stored, 4 KB aligned, 0x8002117C): a full-screen background.
//   +0x0000 "GTMP"
//   +0x0004 2016 tile words (63 rows x 32, 0x80021F88): bit 7 set = flat 16 x 8 TILE (0x80021D30), else a 16 x 8 8-bit
//           texture tile (0x80021EB0)
//   +0x1F84 512 x 8 CLUT words -> VRAM (0, 504)
//   +0x3F84 u32 used texture tiles (twice), then bytes the game does not read
//   +0x4000 pixels: rows of 512 bytes (8-bit indices) -> VRAM (768, 0..), N = (size - 0x4000) / 512 rows
struct GtmpPicture {
    static constexpr size_t kHeaderSize = 0x4000, kTileWords = 63 * 32;
    std::array<uint32_t, kTileWords> tiles{};
    std::array<uint16_t, 512 * 8> clut{};
    uint32_t textureTiles = 0;
    std::vector<uint8_t> pixels;
    int Rows() const { return int(pixels.size() / 512); }
};
GtmpPicture ParseGtmp(std::span<const uint8_t> entry);

// Tile words (commonpic map and page list). Common: bit 7 = flat tile; x = bits 0..6 (flat: 0..5) * 16,
// y = bits 8..15 * 8. Flat: colour = 15-bit BGR in bits 16..30. Texture: u = bits 16..19 * 16, v = bits 21..25 * 8,
// bit 20 = second texture page; commonpic: bit 26 = page row 256, bit 28 + bits 29..31 = CLUT (256 * bit28, 504 + k);
// page list: bits 27..28 + 29..31 = CLUT (576 + 16 * b, 248 + k).
struct MenuTile {
    bool flat = false;
    int x = 0, y = 0;
    uint8_t r = 0, g = 0, b = 0;   // flat
    uint8_t u = 0, v = 0;          // texture
    uint16_t tpage = 0, clut = 0;  // GPU words
};
MenuTile DecodeBackgroundTile(uint32_t word); // 0x80021EB0 / 0x80021D30
MenuTile DecodePageTile(uint32_t word);       // 0x80021DE8 / 0x80021D30

class MenuVram {
public:
    static constexpr int kWidth = 1024, kHeight = 512;
    MenuVram() : words_(size_t(kWidth) * kHeight, 0) {}
    void Upload(int x, int y, int w, int h, std::span<const uint8_t> littleEndianWords);
    // A TIM as 0x80013B60 uploads it: the image block (after the CLUT block when flag bit 3) to the texture page
    // `tpage` (x = (tpage & 15) * 64, y = (tpage & 16) * 16), its own width / height.
    void UploadTimToPage(std::span<const uint8_t> tim, uint16_t tpage);
    uint16_t Word(int x, int y) const { return words_[size_t(y & (kHeight - 1)) * kWidth + size_t(x & (kWidth - 1))]; }
    uint16_t Sample(uint16_t tpage, uint16_t clut, uint8_t u, uint8_t v) const;
    const std::vector<uint16_t>& Words() const { return words_; }
private:
    std::vector<uint16_t> words_;
};

// The two menu fonts of member 4 (both in arcade/gtmode_font.tim at tpage 10). Glyph table: 12 bytes {u8 u, u8 v,
// u16 clut, u16 w, u16 h, u16 tpage, pad}; metrics: 8 bytes {u16 code, s16 dx, s16 dy, s16 advance}; the 8-bit
// maps are built by 0x8001F210 (last glyph of a code < 0x100 wins).
struct MenuFont {
    struct Glyph {
        uint16_t code = 0;
        int16_t dx = 0, dy = 0, advance = 0;
        uint8_t u = 0, v = 0;
        uint16_t clut = 0, w = 0, h = 0, tpage = 0;
    };
    std::vector<Glyph> glyphs;
    std::array<int16_t, 256> map{};
    bool searchWide = false; // font A: codes >= 0x100 by the binary search of 0x8001F2C8
    int Lookup(uint32_t code) const;
};
struct MenuFonts {
    MenuFont a;  // 0x800513BC / 0x80052100, 0x10B glyphs (0x8001F38C..)
    MenuFont b;  // 0x8005129C / 0x80052040, 0x18 glyphs: digits etc. of 28 px (0x8001F7CC..; item flag bit 31)
};
MenuFonts LoadMenuFonts(const GuestImage& ovl4);

// A 12-byte sprite entry of the overlay's tables {u32 uv | clut << 16, u16 w, u16 h, u16 tpage, pad} (medals
// 0x80050978, licence icons 0x800509B4 / 0x800509FC, trophy 0x80050A5C, badge 0x80050A50, cursors 0x80050924 /
// 0x80050930).
struct MenuTableSprite {
    uint8_t u = 0, v = 0;
    uint16_t clut = 0, w = 0, h = 0, tpage = 0;
};
MenuTableSprite ReadMenuTableSprite(const GuestImage& ovl4, uint32_t address);

// One GPU primitive of a menu frame, in the order the GPU draws it (the ordering table already walked). Shared by
// the software canvas below (tools, pixel comparisons with the original) and the native Vulkan view
// (gt2view/menu_view.h), so both draw exactly the same list.
//   kSprite: SPRT at (x[0], y[0]) w x h, texels (u, v) of texture page `tpage` (E1 bits: page, depth 7..8,
//            semi-transparency mode 5..6) with `clut`; colour[0] modulates ((texel * c) >> 7), texel 0 transparent,
//            `semi` = STP texels blend by the page's mode.
//   kTile:   flat rectangle (x[0], y[0]) w x h of colour[0]; `semi` blends by tpage's mode.
//   kPolyF4 / kPolyG4: quad v0..v3 in GPU order (triangles v0 v1 v2 and v1 v2 v3), flat colour[0] or gouraud
//            colour[0..3]; `semi` blends by tpage's mode; `dither` = the draw mode's dither bit (E1 bit 9).
//   kLine:   line (x[0], y[0]) - (x[1], y[1]), colour[0] (flat) or colour[0..1] (gouraud = `gouraud`).
//   kPolyFT4: textured quad v0..v3 (GPU order) with texels (tu[k], tv[k]) of `tpage` (the primitive's own texture page word,
//            which also becomes the draw mode's page bits) and `clut`; colour[0] modulates as a sprite (the scaled sprites of
//            the arcade menus, 0x80011C2C / 0x8001713C with a scale).
struct MenuPrim {
    enum Kind : uint8_t { kSprite, kTile, kPolyF4, kPolyG4, kLine, kPolyFT4 };
    Kind kind = kSprite;
    int16_t x[4] = {}, y[4] = {};
    int16_t w = 0, h = 0;
    uint8_t u = 0, v = 0;
    uint8_t tu[4] = {}, tv[4] = {}; // kPolyFT4
    uint16_t tpage = 0, clut = 0;
    uint32_t colour[4] = {0x808080, 0x808080, 0x808080, 0x808080};
    bool semi = false, dither = false, gouraud = false;
    // Drawing area of a separate draw environment (E3 / E4, inclusive; clipX1 < clipX0 = the whole frame): polygons,
    // lines and tiles are clipped to it (the menus' car view: 0x8008034C with the viewport).
    int16_t clipX0 = 0, clipY0 = 0, clipX1 = -1, clipY1 = -1;
    bool Clipped(int px, int py) const { return clipX1 >= clipX0 && (px < clipX0 || px > clipX1 || py < clipY0 || py > clipY1); }
};

// What a page shows that is not in the page: the career state and game tables. Missing values = the item is drawn
// as the original draws it for a new game (nothing, or "----").
struct MenuRenderState {
    int32_t money = 10000;       // 0x801D1568 (new game)
    uint32_t day = 1;            // 0x801C99D8
    std::string currentCar;      // type 0x47 (0x8001B818); empty = no current car (nothing drawn)
    std::function<std::optional<uint32_t>(uint32_t carId)> carPrice;             // type 6 (0x800177D4)
    struct EventInfo { int32_t bonus = 0; std::array<int32_t, 6> prize{}; int16_t powerLimit = 0; int licence = -1; };
    std::function<std::optional<EventInfo>(const std::string& name)> eventInfo;  // event items (0x800188B0 + 0x800B5E88 records)
    std::function<int(const std::string& name)> eventResult;                     // 0x8005DB90 (0 = none)
    // Items the caller draws itself (e.g. the popup lists of types 9 / 0x50, the car data of 0xB2..0xBA): called in
    // the item pass of 0x8001B9AC for every item; returns true when it handled the item. Its primitives are
    // appended in the order the GPU draws them and are drawn after the item texts, before the cursor.
    std::function<bool(const MenuItem& item, std::vector<MenuPrim>& drawOrder)> customItem;
    // Dynamic items of 0x8001B9AC that need career state (current car, licences, transaction, car view...): called
    // for every item before the built-in cases; appends primitives in the original's GENERATION order (they are
    // reversed with the other items, like the OT does) and returns true when it handled the item's type.
    std::function<bool(const MenuItem& item, std::vector<MenuPrim>& generationOrder)> dynamicItem;
    // Items 0x8001B9AC draws into the view's second ordering table (view +0x88, its argument param_4: the colour
    // name 0x07, the name logo 0x11, the paint chips 0x94), which the GPU draws after the 3D car layer and before the
    // cursor: appended in generation order (reversed like the OT does); true = handled (nothing else is drawn).
    std::function<bool(const MenuItem& item, std::vector<MenuPrim>& generationOrder)> lateItem;
    bool cursor = true;          // the arrow (0x8001EA24)
    // Cursor position (the arrow's tip, 0x8001E328's smoothed position) and whether it rests on an item (sprite
    // 0x80050930 vs 0x80050924); unset = at the page's default item like a freshly loaded page.
    std::optional<std::array<int, 2>> cursorAt;
    bool cursorOnItem = true;
};

// 0x8001FCDC: decimal with a comma every three digits.
std::string MenuThousands(uint32_t value);

// The menu text writer (0x8001F38C / 0x8001F7CC wide, 0x8001F520 / 0x8001F96C 8-bit; 0x8001FC28 / 0x8001FBEC /
// 0x8001FC64 / 0x8001FCA0 aligned): glyph sprites appended in generation order; returns the width drawn.
class MenuTextWriter {
public:
    MenuTextWriter(const MenuFonts& fonts, std::vector<MenuPrim>& out) : fonts_(fonts), out_(out) {}
    int Draw(const std::u16string& text, bool eightBit, bool secondFont, int x, int y, uint32_t colour);
    int Width(const std::u16string& text, bool eightBit, bool secondFont) const;
    int Right(const std::u16string& t, bool eightBit, bool second, int x, int y, uint32_t c) { return Draw(t, eightBit, second, x - Width(t, eightBit, second), y, c); }
    int Left(const std::u16string& t, bool eightBit, bool second, int x, int y, uint32_t c) { return Draw(t, eightBit, second, x, y, c); }
    static std::u16string Widen(const std::string& s);
private:
    static int Index(const MenuFont& f, char16_t c, bool eightBit, bool second);
    const MenuFonts& fonts_;
    std::vector<MenuPrim>& out_;
};

// A 12-byte table sprite centred at (cx, cy) (+ extra), as 0x8001B9AC places medals / icons.
MenuPrim MenuCentredSprite(const MenuTableSprite& s, int cx, int cy, int extraX = 0, int extraY = 0);

// The 512 x 480 drawing area (E3 0,0 / E4 511,479) with the PS1 sprite / tile rules the menus use: texel 0 is
// transparent, colour modulation (texel * c) >> 7, semi-transparency of STP texels by the texpage mode.
class MenuCanvas {
public:
    static constexpr int kWidth = 512, kHeight = 480;
    MenuCanvas() : pixels_(size_t(kWidth) * kHeight, 0) {}
    void Fill(int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b);
    void Sprite(const MenuVram& vram, int x, int y, int w, int h, uint8_t u, uint8_t v, uint16_t tpage, uint16_t clut,
                uint32_t colour = 0x808080, bool semi = false);
    // Rasterises one primitive (sprites and tiles as above; polygons and lines: see MenuPrim).
    void Draw(const MenuVram& vram, const MenuPrim& p);
    uint16_t At(int x, int y) const { return pixels_[size_t(y) * kWidth + size_t(x)]; }
    std::vector<uint8_t> Rgba() const;
    // Rules for polygons, lines and flat tiles (sprites are the same under both):
    //   kPs1:         the PS1 GPU: polygon edges walked in 32.32 fixed point from the top vertex, gouraud colours
    //                 stepped in 12.12 from the leftmost vertex, 4x4 ordered dither of shaded primitives when the
    //                 draw mode's bit 9 is set, lines stepped in 32.32 (both end points), blending in 5 bits.
    //   kInterpreter: the rules of our interpreter's GPU (src/machine/gpu.cpp), which produced the gt2play
    //                 captures: floating-point barycentrics with a top-left rule, no dither, rounded line steps,
    //                 blending in 8 bits.
    enum class Rules : uint8_t { kPs1, kInterpreter };
    Rules rules = Rules::kPs1;
private:
    void Blend(int x, int y, int r, int g, int b, bool semi, int mode, bool dither); // 8-bit channels
    std::vector<uint16_t> pixels_;
};

// Everything the renderer needs besides the page: loaded once.
struct MenuAssets {
    GuestImage ovl4;
    MenuFonts fonts;
    MenuPackIndex commonIndex;
    std::vector<uint8_t> commonDat;
    std::vector<uint8_t> cursorTim, itemsTim, fontTim, iconImage;
    std::vector<uint8_t> strings; // data-gt.txd block of the language (gzip at member 4 0x800242A8, 0x88 bytes each; RAM copy 0x801C30C0)
    std::string String(uint32_t address) const; // a string of that block by its RAM address (0x801C30C0..)
    static MenuAssets Load(const DiscImage& disc, const GtfsVolume& vol, const std::string& language = "usa");
    GtmpPicture Background(uint32_t entry) const;
};

// Result of a page render: the canvas, the composed VRAM and one line per drawn dynamic item.
struct MenuRender {
    MenuCanvas canvas;
    MenuVram vram;
    int backgroundRows = 0, pageRows = 0; // VRAM rows the pictures filled (768,0.. / 640,256..)
    std::vector<std::string> notes;
};

// A menu frame as GPU primitives in draw order: 0x80022278's list (background tiles, page tiles, group sprites), the
// item pass of 0x8001B9AC (drawn in reverse: the OT prepends), the custom items, the cursor.
struct MenuFrame {
    int backgroundId = -1;
    size_t backgroundPrims = 0;
    std::vector<MenuPrim> prims;
    bool clearBehind = false;   // page flag bit 9: the frame is cleared first (the 3D car shows through)
    // Where the 3D car view (type 0x0A: its own draw environment, drawn between the text OT and the OT of the
    // late items) goes: prims[0 .. layer3dAt) are drawn before it, the rest after.
    size_t layer3dAt = 0;
    std::vector<std::string> notes;
};
// The VRAM a page needs (section "VRAM layout" above); `backgroundRows` / `pageRows` get the rows filled.
void ComposeMenuVram(const MenuAssets& assets, const MenuPage& page, MenuVram& vram, int* backgroundRows = nullptr, int* pageRows = nullptr);
MenuFrame BuildMenuFrame(const MenuAssets& assets, const MenuPage& page, const MenuRenderState& state);

// Draws `page` like the GT-mode overlay: ComposeMenuVram + BuildMenuFrame rasterised on the canvas. Not drawn
// unless `state.customItem` draws them: the popup lists (types 9 / 0x50), the 3D car (0x0A / 0x11 / 0x94) and the
// items that need tune / garage state.
MenuRender RenderMenuPage(const MenuAssets& assets, const MenuPage& page, const MenuRenderState& state);

} // namespace gt2
