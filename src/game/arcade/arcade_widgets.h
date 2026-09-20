#pragma once
// Widgets of the arcade menus (GT2.OVL member 2 of US Arcade v1.1, SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95,
// member 2 SHA-1 304ee2b35f0b3dc430394cbfbe14f670a3353cf3; ARCADE v1.1 addresses). Evidence: our disassembly and Ghidra pseudo-C
// of the arcade menu RAM (work/re/arcade_menu, project gt2_arcade_menu) and gt2play --prims captures of the car / course
// selections (work/play/arcade_menu/cap). docs/research/arcade_disc.md section 18.
//
// Drawing model: a view draws into its ordering table (the menu's OT, 8 slots; 0x80013828 puts the header into slots 4 / 6);
// the GPU walks it from slot 7 down to slot 0, and every packet added to a slot is drawn BEFORE the packets added earlier to
// that slot (MenuOtSlot). Text goes through the EXE text context (the menus' object at view + 0x1C4): the font descriptor,
// the texture page word with the semi-transparency mode in bits 21..22, the target slot and the colour (bit 25 = semi).
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gt2formats/gt_menu_images.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/hud_assets.h"
#include "gt2formats/overlay_data.h"

namespace gt2::arcade {

struct ArcadeMenuAssets;

constexpr uint32_t kExeSmallFont = 0x80092E1Cu;          // the EXE font a text object without template uses (Simulation 0x80093124)
constexpr uint32_t kButtonCaptionTemplate = 0x8004F4FCu; // 0x8001AA98: the button bar caption's text template (font word overwritten)

// ---------------------------------------------------------------- ordering table

struct ViewOt {
    static constexpr int kSlots = 8;
    std::array<MenuOtSlot, kSlots> slot;
    // The slots in GPU order (7 .. 0); `mode` = the draw mode (E1) in effect before the table; returns the mode after it.
    uint16_t Emit(std::vector<MenuPrim>& out, uint16_t mode) const;
};

// ---------------------------------------------------------------- the text context and the EXE text routines

struct TextCtx {
    const HudFont* font = nullptr; // 0x8007D990 copies the descriptor
    int mode = 0;                  // +0x0C bits 21..22: the glyphs' E1 semi-transparency mode (kept across calls, as the object)
    uint32_t colour = 0;           // +0x14 (bit 25 = semi-transparent glyphs)
    MenuOtSlot* ot = nullptr;      // +0x10
    bool thickUnderline = false;   // +0x1C: the text object's underline is 2 pixels high
};

// 0x8007DC4C(ctx, code, x, y): the glyph packet(s) of character `code & 0xFF` (flags 0x100 / 0x200 as HudFont::Glyph) at
// pen x, baseline y into ctx.ot: E1 (page + the glyph's page offset, the ctx mode) + SPRT 0x64 (0x66 semi) per sprite.
void DrawGlyph(TextCtx& c, uint32_t code, int x, int y);
// 0x8006ABA0 / 0x8006AC4C / 0x8006AE50 / 0x8006AF54 (Simulation 0x8006AC90 / 0x8006AD3C / 0x8006AF40 / 0x8006B044).
void DrawText(TextCtx& c, const std::string& s, int x, int y, int spacing);
int TextWidth(const TextCtx& c, const std::string& s, int spacing);
void DrawNumber(TextCtx& c, const std::string& s, int x, int y, int spacing, int digitShift, int digitExtra);
int NumberWidth(const TextCtx& c, const std::string& s, int spacing, int digitExtra);
// 0x8006B304 (Simulation 0x8006B3F4 HudFont::TimeRight): a race time right-aligned at `right`.
void DrawTimeRight(TextCtx& c, const std::string& s, int right, int y, int advance, int narrow, int signFlag, int dotShift);
// The glyph packets of already placed font sprites (as DrawGlyph emits them).
void EmitGlyphSprites(TextCtx& c, const std::vector<HudFontSprite>& sprites);
// 0x80011B94: the decimal digits of a non-negative number (powers of ten 0x800272E0..0x800272FC).
std::string Decimal(int value);

// ---------------------------------------------------------------- primitive helpers

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour);              // 0x8007CF34: TILE 0x60 (bit 25: 0x62)
MenuPrim Line(int x0, int y0, int x1, int y1, uint32_t colour);         // 0x8007F704: LINE 0x40 (0x42)
MenuPrim FlatQuad(const std::array<int, 4>& xs, const std::array<int, 4>& ys, uint32_t colour); // 0x8007CF70: POLY_F4 0x28 (0x2A)
MenuPrim FlatTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t colour);        // 0x8007DFF0: POLY_F3 0x20 (0x22)
// 0x8007DFC0 filled as 0x8006B5F4 / 0x80011AFC: POLY_G4 0x3A over {x, y, w, h}, c0 on the left edge, c1 on the right.
MenuPrim Gradient(int x, int y, int w, int h, uint32_t c0, uint32_t c1);
// 0x80081388 + E1: a sprite (u, v, clut, w, h) of texture page `tpage` with a grey `brightness` (0x80 = as is).
void AddSprite(MenuOtSlot& ot, int x, int y, int u, int v, uint16_t clut, int w, int h, uint16_t tpage, uint32_t colour);
// 0x80011C2C / 0x80011D58: a sprite scaled by `scale` / 256 per half (POLY_FT4 0x2C, 0x2E with colour bit 25) centred at (x, y):
// corners x -+ (w * scale >> 8), y -+ (h * scale >> 8), texels u .. u + w - 1, v .. v + h - 1; `tpage` = the quad's page word.
void AddScaledSprite(MenuOtSlot& ot, int x, int y, int u, int v, uint16_t clut, int w, int h, uint16_t tpage, uint32_t colour, int scale);
// 0x8006B458(c0, c1, t, n): c0 + (c1 - c0) * t / n per channel (t clamped to 0..n), c0's top byte kept.
uint32_t LerpColour(uint32_t c0, uint32_t c1, int t, int n);

// ---------------------------------------------------------------- the text object (0x2C bytes)

// Member 2's text object: init 0x80019FC8 (template: +0 u8 revealDivisor, +1 u8 waveDivisor, +2 s16 steps, +4 s16 glowSpread,
// +6 s16 period, +8 s16 fadeSteps, +0xA s8 extra spacing, +0xC u16 flags, +0xE u8 height, +0x10 font, +0x14 c0, +0x18 c1; no
// template = 0x80, 2, 0x1E, 0x10, 0x1E, 0x10, 1, 0, 0x10, EXE font 0x80092E1C, white), open 0x8001A0D8, restart 0x8001A13C, close
// 0x8001A170, tick 0x8001A1A8, width 0x8001A9D0, draw 0x8001A204. Flags: bits 0..1 the glyphs' semi-transparency mode, 2 an
// underline TILE per character, 3 a brightness wave once settled, 4 fixed cells, 5 semi-transparent glyphs, 6 glow copies while
// revealing, 7..8 alignment (0x80 centre, 0x100 right).
struct ArcText {
    uint8_t revealDivisor = 0x80, waveDivisor = 2;
    int16_t steps = 0x1E, glowSpread = 0x10, period = 0x1E, fadeSteps = 0x10;
    int8_t extra = 1;
    uint8_t height = 0x10;
    uint16_t flags = 0;
    uint8_t alpha = 0x80;           // +0x0E
    int16_t x = 0, y = 0;           // +0x10 / +0x12
    uint32_t c0 = 0xFFFFFF, c1 = 0xFFFFFF;
    const HudFont* font = nullptr;  // +0x1C
    std::string text;               // +0x20
    int16_t anim = -1, settle = 0, length = 0; // +0x24 / +0x26 / +0x28

    // 0x80019FC8 with the template at `address` of the member-2 image (0 = the defaults); `font` != 0 replaces the template's
    // font word (a template whose font the caller writes before the copy).
    void Init(const ArcadeMenuAssets& a, uint32_t address, uint32_t font = 0);
    void Open(int settleAt = -1);   // 0x8001A0D8
    void Restart();                 // 0x8001A13C
    void Close();                   // 0x8001A170
    void Tick();                    // 0x8001A1A8
    int Width(TextCtx& c) const;    // 0x8001A9D0 (sets the ctx font as the original)
    // 0x8001A204: glyphs into ot + 1, underline / glow / closing quads into ot (`ot` = a slot of a ViewOt).
    void Draw(MenuOtSlot* ot, TextCtx& c) const;
};

// ---------------------------------------------------------------- the button bar (0x8001AA98, 0x98 bytes)

// Two buttons with a caption (TRANSMISSION AT / MT, SETTINGS Racing / Drift): definition {+0 caption, +4 / +8 labels, +0xC s8
// selection, +0xE u16 flags (bit 0 red instead of blue, 1 slide from the other side, 3 caption 4 higher, 2..3 size, 7 flash),
// +0x10 s8 spacing, +0x14 font}; open 0x8001ABD8, close 0x8001AC40, update 0x8001AC68, draw 0x8001ADA8.
struct ArcButtonBar {
    int16_t x = 0, y = 0;
    ArcText caption, label[2];
    int8_t selected = 0;
    uint8_t halfHeight = 0, halfWidth = 0; // +0x8A (height) / +0x8B
    int16_t anim = -1, flash = 0;          // +0x8C / +0x8E
    uint16_t flags = 0;                    // +0x90

    void Init(const ArcadeMenuAssets& a, uint32_t definition);
    void Open();
    void Close();
    // -2 nothing, -3 moved, -1 back, else the button chosen; `pad` null = no input.
    int Update(const MenuListPad* pad);
    // Lines / caption / labels into `ot`, the highlight into ot + 1.
    void Draw(MenuOtSlot* ot, TextCtx& c) const;
};

// ---------------------------------------------------------------- the carousel (0x8001C790, 0x2C bytes)

// A horizontal (or vertical) list of pages drawn by a callback: init 0x8001C790 {x, y, w, h, callback, context} + 0x8001C7F0
// (counts per group), open 0x8001C820, close 0x8001C864, update 0x8001C870 (left / right: the item, skipping the ones the
// availability flags close; up / down: the group), draw 0x8001CBF4 (blinking arrows, the old page sliding out, the new one in).
struct ArcCarousel {
    int16_t x = 0, y = 0, w = 0, h = 0;
    int16_t group = 0, index = 0;          // +8 / +0xA
    std::array<int16_t, 4> counts{};       // +0xC per group
    int16_t groups = 1;                    // +0x10
    int16_t prevGroup = -1, prevIndex = -1; // +0x12 / +0x14
    int16_t anim = -1;                     // +0x16
    int16_t slide = 0, vertical = 0;       // +0x18 / +0x1A
    bool arrows = false;                   // +0x1C
    const std::vector<uint8_t>* available = nullptr; // +0x28 (null = every item)

    struct Item {
        int x = 0, y = 0, brightness = 0x80;
        int group = 0, index = 0;
        int current = 0; // the list's current index (-1 while closing)
    };
    void Open();
    void Close();
    int Update(const MenuListPad* pad);
    void Draw(MenuOtSlot& ot, const std::function<void(const Item&)>& page) const;
};

// ---------------------------------------------------------------- the growing line (EXE 0x8006BBC8 / 0x8006BC18)

struct ArcGrowLine {
    uint32_t c0 = 0, c1 = 0;
    int16_t x = 0, y = 0, w = 0, h = 0, steps = 0, anim = -1;
    void Read(const GuestImage& ovl2, uint32_t address);
    void Open() { anim = 0; }
    void Close() { anim = int16_t(~steps); }
    void Tick();
    void Draw(MenuOtSlot& ot) const;
};

} // namespace gt2::arcade
