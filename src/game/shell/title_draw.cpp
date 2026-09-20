#include "game/shell/title_draw.h"

#include "game/shell/title_menu.h"

namespace gt2::shell {

std::vector<MenuPrim> TitleFrameStart() {
    std::vector<MenuPrim> out;
    MenuPrim clear;
    clear.kind = MenuPrim::kTile;
    clear.x[0] = 0, clear.y[0] = 0, clear.w = TitleAssets::kScreenWidth, clear.h = TitleAssets::kScreenHeight;
    clear.colour[0] = 0;
    clear.tpage = 0x200;
    out.push_back(clear);
    return out;
}

namespace {

void AddGlyphs(MenuOtSlot& ot, const std::vector<HudFontSprite>& glyphs, uint32_t colour, int mode) {
    const bool semi = (colour & 0x2000000u) != 0;
    for (const HudFontSprite& g : glyphs) {
        const uint16_t e1 = uint16_t(g.tpage | (semi ? (mode & 3) << 5 : 0));
        MenuPrim p;
        p.kind = MenuPrim::kSprite;
        p.x[0] = int16_t(g.x), p.y[0] = int16_t(g.y), p.w = int16_t(g.w), p.h = int16_t(g.h);
        p.u = uint8_t(g.u), p.v = uint8_t(g.v);
        p.clut = g.clut;
        p.tpage = e1;
        p.colour[0] = colour & 0xFFFFFF;
        p.semi = semi;
        ot.Add(p);
        ot.DrawMode(e1);
    }
}

} // namespace

int AddText(MenuOtSlot& ot, const HudFont& font, const std::string& text, int x, int y, int spacing, uint32_t colour, int mode, TextAlign align) {
    const int width = font.TextWidth(text, spacing);
    if (align == TextAlign::kCentre) x -= width >> 1;       // 0x8006ADB4
    else if (align == TextAlign::kRight) x -= width;        // 0x8006AE28
    std::vector<HudFontSprite> glyphs;
    font.Text(text, x, y, spacing, glyphs);
    AddGlyphs(ot, glyphs, colour, mode);
    return width;
}

int AddNumberText(MenuOtSlot& ot, const HudFont& font, const std::string& text, int x, int y, int spacing, int digitShift, int digitExtra, uint32_t colour,
                  int mode, bool right) {
    std::vector<HudFontSprite> glyphs;
    const int w = right ? font.NumberRight(text, x, y, spacing, digitShift, digitExtra, glyphs) : font.Number(text, x, y, spacing, digitShift, digitExtra, glyphs);
    AddGlyphs(ot, glyphs, colour, mode);
    return w;
}

void AddViewHeader(MenuOtSlot& ot, const TitleAssets& assets, const std::string& title, uint32_t colour, int alpha) {
    const HudFont& font = assets.fonts[TitleAssets::kHeaderFont];
    const int spacing = 0x22 - (alpha >> 2);
    const int width = font.TextWidth(title, spacing);
    const int x = (0x160 - width) >> 1;
    MenuPrim line; // 0x8007D024: TILE
    line.kind = MenuPrim::kTile;
    line.x[0] = int16_t(x + 1), line.y[0] = 0x5C, line.w = int16_t(width), line.h = 2;
    line.colour[0] = uint32_t((alpha * 0xF2) >> 7);
    ot.Add(line);
    const uint32_t grey = uint32_t((alpha * 0x66) >> 7);
    std::vector<HudFontSprite> glyphs;
    font.Text(title, x, 0x5A, spacing, glyphs);
    AddGlyphs(ot, glyphs, grey | grey << 8 | grey << 16, 0);
    glyphs.clear();
    font.Text(title, x + 3, 0x5D, spacing, glyphs);
    AddGlyphs(ot, glyphs, 0, 0);
    const uint32_t c = uint32_t(((colour & 0xFF) * uint32_t(alpha)) >> 7) | uint32_t((((colour >> 8) & 0xFF) * uint32_t(alpha)) >> 7) << 8 |
                       uint32_t((((colour >> 16) & 0xFF) * uint32_t(alpha)) >> 7) << 16;
    const int16_t half = int16_t((alpha * 0x30) >> 7);
    MenuPrim g; // 0x8007E0B0: POLY_G4 0x3A
    g.kind = MenuPrim::kPolyG4;
    g.semi = true;
    g.gouraud = true;
    g.x[0] = 0, g.y[0] = int16_t(0x30 - half);
    g.x[1] = 0x160, g.y[1] = int16_t(0x30 - half);
    g.x[2] = 0, g.y[2] = int16_t(0x30 + half);
    g.x[3] = 0x160, g.y[3] = int16_t(0x30 + half);
    g.colour[0] = g.colour[1] = c;
    g.colour[2] = g.colour[3] = 0;
    ot.Add(g);
    ot.DrawMode(0x220);
}

std::vector<MenuPrim> BuildTitleFrame(const TitleMenu& menu) {
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot ot;
    menu.Draw(ot);
    ot.Emit(prims, 0x200);
    return prims;
}

MenuCanvas RenderTitleFrame(const TitleAssets& assets, const std::vector<MenuPrim>& prims, MenuCanvas::Rules rules) {
    MenuCanvas canvas;
    canvas.rules = rules;
    for (const MenuPrim& p : prims) canvas.Draw(assets.vram, p);
    return canvas;
}

} // namespace gt2::shell
