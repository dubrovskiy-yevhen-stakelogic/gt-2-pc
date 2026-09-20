#pragma once
// Frames of the title overlay's screens as GPU primitives in draw order (gt2formats MenuPrim: the same list the
// software canvas rasterises for comparisons and gt2view/title_view draws natively). The screen is 352 x 480
// (TitleAssets::kScreenWidth); every frame starts with the view manager's clear (0x800121A8: E1 0x200 + a black TILE
// over the drawing area).
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/gt_menu_list.h"
#include "gt2formats/hud_assets.h"
#include "gt2formats/title_assets.h"

namespace gt2::shell {

class TitleMenu;

// The frame start (clear) in GPU order.
std::vector<MenuPrim> TitleFrameStart();

// The EXE text engine (0x8006AC90 and friends through gt2formats HudFont) into an ordering-table entry: every glyph
// is a SPRT followed by its draw mode (glyph page | semi-transparency mode << 5), as 0x8007DD3C adds them. `colour`
// bit 25 = semi-transparent glyphs (0x8006B548 keeps it), `mode` = the context's mode (bits 21..22 of its flags).
// Returns the width 0x8006AC90 returns.
enum class TextAlign { kLeft, kCentre, kRight };
int AddText(MenuOtSlot& ot, const HudFont& font, const std::string& text, int x, int y, int spacing, uint32_t colour, int mode,
            TextAlign align = TextAlign::kLeft);
// 0x8006AF40 / 0x8006B184 (fixed digit cells: spacing, digit shift -2, digit extra 0 as the options call them).
int AddNumberText(MenuOtSlot& ot, const HudFont& font, const std::string& text, int x, int y, int spacing, int digitShift, int digitExtra, uint32_t colour,
                  int mode, bool right = false);

// 0x8001191C: the header of a view with a title (the view table's +0x10 string, +0x0C colour): the title in the
// header font (cell 12) centred on the 352-pixel screen with letter spacing 0x22 - alpha / 4, a shadow (colour 0 at
// +3, +3 over grey 0x66 * alpha / 128 at (x, 90)), a red underline TILE (x + 1, 92, width, 2) of 0xF2 * alpha / 128,
// and the gradient POLY_G4 (0, 48 - 48 * alpha / 128) .. (352, 48 + 48 * alpha / 128) from the colour * alpha / 128
// to black, E1 0x220.
void AddViewHeader(MenuOtSlot& ot, const TitleAssets& assets, const std::string& title, uint32_t colour, int alpha);

// 0x80017C00 over 0x800121A8: the title screen.
std::vector<MenuPrim> BuildTitleFrame(const TitleMenu& menu);

// Software rendering of a frame (the VRAM of the title assets, MenuCanvas rules) for comparisons with captures.
MenuCanvas RenderTitleFrame(const TitleAssets& assets, const std::vector<MenuPrim>& prims, MenuCanvas::Rules rules);

} // namespace gt2::shell
