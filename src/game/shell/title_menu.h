#pragma once
// The title screen of GT2.OVL member 1 ("ovl1"): the view 0x8004BC78 {init 0x8001792C, update 0x80017984, draw
// 0x80017C00} and its list rows (callback 0x8001779C, row state 0x800B1230 + row * 0x14, row draw 0x80016410).
// Ported from US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); evidence: our
// disassembly / Ghidra pseudo-C of work/re/title/ram.bin and the original's GP0 primitives of the title
// (gt2play --prims 1150 / 1370; docs/research/menus_gtmode.md section 10).
//
// The list is the resident EXE list widget (gt2formats/gt_menu_list.h) with the widget object of member 1 at
// 0x8004BC28 (8 rows, 3 visible, wrap, centre (176, 274), row pitch 28, fade 16). Rows 0 and 7 are blank (result
// table 0x8004BC04 = -1: the callback does nothing for them and reports them disabled), rows 1..6 show the item
// sprites of arcade/title_item.tim (tpage 0x0C) for results 0 Start Game, 1 Replay Theater, 2 Options, 3 Save Game,
// 4 Load Game, 5 Data Transfer (-> 0x801EF5F3; the entry 0x80011384 dispatches on it; 6 = the attract demo after 900
// idle fields).
#include <array>
#include <cstdint>
#include <vector>

#include "gt2formats/gt_menu_list.h"
#include "gt2formats/overlay_data.h"

namespace gt2::shell {

// Title menu results (0x801EF5F3).
enum TitleResult : int {
    kTitleNone = -1, kStartGame = 0, kReplayTheater = 1, kOptions = 2, kSaveGame = 3, kLoadGame = 4, kDataTransfer = 5, kAttractDemo = 6,
};

// A 12-byte sprite entry of member 1 {u8 u, u8 v, u16 clut, u16 w, u16 h, u16 tpage, pad}.
struct TitleSprite {
    uint8_t u = 0, v = 0;
    uint16_t clut = 0, w = 0, h = 0, tpage = 0;
    static TitleSprite Read(const GuestImage& image, uint32_t address);
};

// One row of the title list (0x14 bytes at 0x800B1230 + row * 0x14).
struct TitleRowState {
    uint8_t flags = 0;        // +00 bit 0 second pass (selected: black + light; else a subtractive shadow), bit 1 semi-
                              //     transparent, bit 2 reveal ghosts, bits 3-4 blend mode (<< 2 -> E1 bits 5-6),
                              //     bits 5-6 alignment (0x20 left edge at x, 0x40 right edge at x)
    int16_t revealWidth = 0;  // +02 ghost spread at the start of the reveal
    int16_t x = 0, y = 0;     // +04 / +06 the row centre (from the widget's draw block)
    uint8_t brightness = 0;   // +08
    TitleSprite sprite;       // +0C (pointer into the sprite table of the language)
    int16_t anim = -1;        // +10 -1 hidden, 0..12 reveal (12 = shown)
    int16_t alpha = 0x80;     // +12
};

class TitleMenu {
public:
    static constexpr uint32_t kWidget = 0x8004BC28u, kResults = 0x8004BC04u, kSpriteIndex = 0x8004BC14u, kRowTemplate = 0x8004BC24u;
    static constexpr uint32_t kSpriteTables = 0x8004BC5Cu; // u32 [language] -> 12-byte sprites
    static constexpr int kRows = 8, kIdleFields = 900;

    TitleMenu(const GuestImage& ovl1, uint8_t language);
    TitleMenu(const TitleMenu&) = delete; // the widget's callback points at this object
    TitleMenu& operator=(const TitleMenu&) = delete;

    // 0x8001792C (the view's init).
    void Reset();
    // 0x80017984: one field. `pad` = null while the view takes no input; `held` = the held bits of the view's pad block
    // (the idle counter reads them even then). Returns 4 once the view has faded out after a choice (the view loop
    // 0x800833E8 then ends), else 0. `result` holds the choice (0x801EF5F3).
    int Update(const MenuListPad* pad, uint32_t held);
    // 0x80017C00: the list (0x8006D50C through the row callback), then the background (4 sprites of the 8-bit picture at
    // (384, 0), CLUT (384, 511), brightness fade * 128 / 12), then E1 0x280; all into one ordering-table entry.
    void Draw(MenuOtSlot& ot) const;

    int result = kTitleNone;       // 0x801EF5F3
    bool startHeld = false;        // 0x801EF601: start was held when the item was chosen
    std::vector<int> sounds;       // 0x80060840 menu sound requests of the last Update (6 move, 3 choose)

    const MenuListWidget& Widget() const { return widget_; }
    int Selection() const { return widget_.selection; }
    int16_t Fade() const { return fade_; }
    int State() const { return state_; }
    const TitleRowState& Row(int r) const { return rows_[size_t(r)]; }

private:
    int32_t RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw);

    std::array<int16_t, kRows> results_{}, spriteIndex_{};
    uint8_t templateFlags_ = 0, templateBrightness_ = 0;
    int16_t templateReveal_ = 0;
    std::array<TitleSprite, kRows> sprites_{};
    MenuListWidget widget_;
    std::array<TitleRowState, kRows> rows_{};
    int16_t fade_ = 0;        // 0x8004BC00: background brightness 0..12
    int16_t leave_ = 0x10;    // 0x8004BC02: fields of the fade-out after a choice
    int16_t state_ = 0;       // 0x800B122C: 0 opening delay, 1 running, 2 leaving
    int16_t openDelay_ = 16;  // 0x800B122E
    int32_t idle_ = 0;        // 0x800B1228
};

// 0x80016410: the sprites of one row (generation order into `ot`); `selected` = the widget's selection is this row.
void DrawTitleRow(const TitleRowState& row, MenuOtSlot& ot, bool selected);

} // namespace gt2::shell
