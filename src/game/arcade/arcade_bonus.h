#pragma once
// BONUS ITEMS and ENDING CREDITS of the arcade menus (GT2.OVL member 2 of US Arcade v1.1, SCUS_944.55 SHA-1
// 231f9dba7191b9ef915621662afdc40a7c66df95; ARCADE v1.1 addresses). docs/research/arcade_disc.md section 18.
//
// BONUS ITEMS (view 0x800525C8: init 0x80023CA8, update 0x80023D58, draw 0x80023EB8; title 0x800F8469, colour 0x0A16C0): the
// record courses of the bonus table 0x80051790 (16-byte rows {+0 name, +4 s8 road, +5 s8 dirt banner, +6 s8 car mark, +8 s32
// unlock tier (0x8002357C), +0xC s32 the course's flag byte career + 0xB8 + n (-1 none)}, listed while their tier is open) in the
// list widget 0x80052524 (EXE 0x8006CCDC .., rows 0x80023BC8 -> 0x80023674), the legend sprites 0x80052484 / 0x80052490, the
// message 0x800F8490 and the one-button dialog 0x80052558 ("Next", EXE 0x8006DBC8 ..). The dialog's cross opens ENDING CREDITS.
// ENDING CREDITS (view 0x8005261C: init 0x800240BC, update 0x8002416C, draw 0x800242FC): the panel list 0x800525A8 (Arcade Mode
// row open when every course of the bonus table with a flag byte has bit 1 or 2 set (0x800235F4), Simulation Mode row open when
// career + 0x215 != 0 (0x80023664)) under the logo sprite 0x80052430. A chosen row goes to view 0x80052670, which after 24 fields
// returns 5 / 6 (0x800F36AE: the row) and the top level plays the credits movie of STREAM.DAT (MDEC; not ported).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/arcade/arcade_menus.h"
#include "game/arcade/arcade_widgets.h"
#include "gt2view/race_menus.h"

namespace gt2::arcade {

// The executable's one-button dialog (US Arcade v1.1 EXE: init 0x8006DBC8, open 0x8006DCE0, close 0x8006DD2C, update 0x8006DD54,
// draw 0x8006DDFC, text template 0x80091BA0, zero colour 0x80091BBC - the code of Simulation 0x8006DCB8 .. instruction for
// instruction, screens::ResultDialog). Definition (member 2): +0 x, +2 y, +4 text, +8 text c0, +0xC fill, +0x10 gradient, +0x14
// flags (bits 1..2 size), +0x15 s8 spacing, +0x16 height, +0x18 font.
struct ArcDialog {
    int16_t x = 0, y = 0;
    screens::TextObject text;
    uint16_t flags = 0;
    int16_t w = 80, h = 12;
    int16_t anim = -1;
    uint32_t fill = 0, gradient = 0, zero = 0;
    void Init(const ArcadeMenuAssets& a, uint32_t definition);
    void Open();
    void Close();
    int Update(const MenuListPad* pad); // -2 nothing, 0 chosen (cross / circle), -1 back (triangle / square)
    // Into slots 0..2 of `ot` (the text glyphs slot 1).
    void Draw(const ArcadeMenuAssets& a, ViewOt& ot) const;
};

// 0x800235F4: every course of the bonus table with a flag byte won (bit 1 or 2 of career + 0xB8 + n).
bool AllBonusCoursesWon(const ArcadeMenuAssets& a, std::span<const uint8_t> career);

class ArcadeBonusPage {
public:
    explicit ArcadeBonusPage(const ArcadeMenuAssets& a);
    void Enter(std::span<const uint8_t> career); // 0x80023CA8
    enum Result { kStay = 0, kNext = 1, kBack = 2 };
    Result Update(const MenuListPad* pad, std::vector<int>& sounds); // 0x80023D58
    void Draw(ViewOt& ot, TextCtx& c) const;                         // 0x80023EB8

private:
    struct Row {
        std::string name;
        int8_t road = 0, dirt = 0, carMark = 0;
        int32_t tier = -1, flag = -1;
    };
    int32_t RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) const; // 0x80023BC8
    void DrawRow(const Row& r, MenuOtSlot* ot, int x, int y, int alpha) const;                         // 0x80023674

    const ArcadeMenuAssets& a_;
    std::vector<Row> rows_;
    std::vector<uint8_t> career_;
    MenuListWidget list_;
    ArcDialog dialog_;
    int timer_ = 0;
    mutable TextCtx* rowCtx_ = nullptr;
};

class ArcadeCreditsPage {
public:
    explicit ArcadeCreditsPage(const ArcadeMenuAssets& a);
    void Enter(std::span<const uint8_t> career); // 0x800240BC
    // 0x8002416C: -1 stay, -2 back, else the row chosen (0 Arcade, 1 Simulation).
    int Update(const MenuListPad* pad, std::vector<int>& sounds);
    void Draw(MenuOtSlot& ot) const; // 0x800242FC

private:
    const ArcadeMenuAssets& a_;
    PanelList list_;
    int timer_ = 0, logo_ = 0; // view + 0x14 / + 0x18
};

} // namespace gt2::arcade
