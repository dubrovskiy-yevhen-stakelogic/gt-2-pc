#include "game/shell/title_screens.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <string>
#include <vector>

#include "game/shell/title_draw.h"

namespace gt2::shell {

namespace {

MenuPrim Tile(int x, int y, int w, int h, uint32_t colour) {
    MenuPrim p;
    p.kind = MenuPrim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour[0] = colour & 0xFFFFFF;
    p.semi = (colour & 0x2000000u) != 0;
    return p;
}

uint32_t Grey(uint32_t v) { return v | v << 8 | v << 16; }

// ---- the PC SETTINGS page (ours): its rows as choice lists over PcSettings (speed units, graphics).
constexpr int kPcRowCount = 11;
constexpr int kPcCaps[] = {0, 30, 60, 120, 144, 165, 240};
constexpr int kPcScales[] = {50, 75, 100, 125, 150, 200};
constexpr int kPcMsaa[] = {1, 2, 4, 8};
constexpr int kPcDistances[] = {0, 500, 1000, 2000, -1};

struct PcRow {
    const char* label = "";
    std::vector<std::string> values;
    int index = 0;
};

template <size_t N>
int IndexOf(const int (&table)[N], int value, std::vector<std::string>& values, const char* customFormat) {
    for (size_t i = 0; i < N; i++)
        if (table[i] == value) return int(i);
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), customFormat, value); // a value given on the command line / in the file
    values.push_back(buffer);
    return int(values.size()) - 1;
}

PcRow PcRowOf(const PcSettings& pc, int row) {
    const GraphicsSettings& g = pc.graphics;
    PcRow r;
    switch (row) {
    case 0: r = {"Speed Units", {"mph", "km/h"}, pc.metric ? 1 : 0}; break;
    case 1: r = {"Frame Rate", {"Original 30", "Display"}, g.frameRate == GraphicsSettings::kFrameRateOriginal ? 0 : 1}; break;
    case 2:
        r = {"Frame Cap", {"Off", "30", "60", "120", "144", "165", "240"}, 0};
        r.index = IndexOf(kPcCaps, g.frameCap, r.values, "%d");
        break;
    case 3: r = {"VSync", {"Off", "On"}, g.vsync ? 1 : 0}; break;
    case 4:
        r = {"Render Scale", {"50%", "75%", "100%", "125%", "150%", "200%"}, 0};
        r.index = IndexOf(kPcScales, g.renderScale, r.values, "%d%%");
        break;
    case 5:
        r = {"Anti-Aliasing", {"Off", "MSAA 2x", "MSAA 4x", "MSAA 8x"}, 0};
        r.index = IndexOf(kPcMsaa, g.msaa, r.values, "MSAA %dx");
        break;
    case 6: r = {"Textures", {"PS1 Nearest", "Smooth"}, g.smoothTextures ? 1 : 0}; break;
    case 7: r = {"Texture Mapping", {"Perspective", "Affine PS1"}, g.affine ? 1 : 0}; break;
    case 8: r = {"Scenery Detail", {"Original", "Max"}, g.maxDetail ? 1 : 0}; break;
    case 9:
        r = {"Draw Distance", {"Original", "500 m", "1000 m", "2000 m", "All"}, 0};
        r.index = IndexOf(kPcDistances, g.drawDistance, r.values, "%d m");
        break;
    default: {
        const bool vanilla = g == GraphicsSettings::Vanilla(), modern = g == GraphicsSettings::Modern();
        r = {"Preset", {"Vanilla", "Custom", "Modern"}, vanilla ? 0 : modern ? 2 : 1};
        break;
    }
    }
    return r;
}

// Steps row `row` by `step` (-1 / +1, no wrap); true when a setting changed.
bool StepPcRow(PcSettings& pc, int row, int step) {
    const PcRow r = PcRowOf(pc, row);
    const int count = std::min(int(r.values.size()), row == 2 ? int(std::size(kPcCaps)) : row == 4 ? int(std::size(kPcScales))
                                                     : row == 5 ? int(std::size(kPcMsaa)) : row == 9 ? int(std::size(kPcDistances)) : int(r.values.size()));
    int index = r.index;
    if (index >= count) index = step > 0 ? count - 1 : 0; // a custom value: the first step lands on the table
    else index = std::clamp(index + step, 0, count - 1);
    if (row == 10) index = step < 0 ? 0 : 2; // Preset: left = Vanilla, right = Modern
    GraphicsSettings& g = pc.graphics;
    const PcSettings before = pc;
    switch (row) {
    case 0: pc.metric = index == 1; break;
    case 1: g.frameRate = index == 0 ? GraphicsSettings::kFrameRateOriginal : GraphicsSettings::kFrameRateDisplay; break;
    case 2: g.frameCap = kPcCaps[index]; break;
    case 3: g.vsync = index == 1; break;
    case 4: g.renderScale = kPcScales[index]; break;
    case 5: g.msaa = kPcMsaa[index]; break;
    case 6: g.smoothTextures = index == 1; break;
    case 7: g.affine = index == 1; break;
    case 8: g.maxDetail = index == 1; break;
    case 9: g.drawDistance = kPcDistances[index]; break;
    default: g = index == 0 ? GraphicsSettings::Vanilla() : GraphicsSettings::Modern(); break;
    }
    return !(before.metric == pc.metric && before.graphics == pc.graphics);
}

// 0x8006C5DC / 0x8006CD04 in the settled state: the text object of the button bars. Per character: a space or a digit
// takes the font's cell (digits drawn right-aligned in the middle of the cell + extra), others the text engine's
// advance (0x8007DC78); every character adds the object's extra spacing (template 0x80091EC8 +0x0A = 2 for the bar's
// title, the bar template's byte +0x22 = 1 for the labels); centred on x by the total width >> 1.
int TextObjectWidth(const HudFont& font, const std::string& s, int extra) {
    int w = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const uint8_t c = uint8_t(s[i]);
        w += ((c >= '0' && c <= '9') || c == ' ') ? font.cell : font.Advance(c, i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0));
        w += extra;
    }
    return w;
}

void AddTextObject(MenuOtSlot& ot, const HudFont& font, const std::string& s, int x, int y, int extra, uint32_t colour, int mode) {
    x -= TextObjectWidth(font, s, extra) >> 1;
    std::vector<HudFontSprite> glyphs;
    for (size_t i = 0; i < s.size(); i++) {
        const uint8_t c = uint8_t(s[i]);
        int advance = font.cell;
        if (c == ' ') {
        } else if (c >= '0' && c <= '9') {
            font.Glyph(uint32_t(c) | HudFont::kRightAlign, x + ((font.cell + extra) >> 1), y, glyphs);
        } else {
            font.Glyph(c, x, y, glyphs);
            advance = font.Advance(c, i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0));
        }
        x += advance + extra;
    }
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

// ---------------------------------------------------------------- shared pieces

Band Band::Read(const GuestImage& image, uint32_t address) {
    Band b;
    b.w = image.Get<int16_t>(address);
    b.h = image.Get<int16_t>(address + 2);
    b.steps = image.Get<int16_t>(address + 4);
    b.flags = image.Get<int16_t>(address + 6);
    b.c0 = image.Get<uint32_t>(address + 8);
    b.c1 = image.Get<uint32_t>(address + 0xC);
    b.target = image.Get<uint32_t>(address + 0x10);
    b.targetOut = image.Get<uint32_t>(address + 0x14);
    b.anim = image.Get<int16_t>(address + 0x18);
    return b;
}

void Band::Tick() {
    if (anim < 0) {
        if (anim < -1) anim++;
    } else if (++anim > steps) {
        anim = steps;
    }
}

void Band::Draw(MenuOtSlot& ot, int x, int y) const {
    if (anim == -1 || steps == 0) return;
    uint32_t tc = target;
    int t = steps - anim;
    if (anim < 0) {
        tc = targetOut;
        t = steps + 1 + anim;
    }
    int left = x - w, right = x;
    if ((flags & 1) == 0) left = x, right = x + w;
    const int grow = (w * t) / steps;
    left -= grow;
    right += grow;
    const uint32_t a = MenuListLerp(c0, tc, t, steps), b = MenuListLerp(c1, tc, t, steps);
    MenuPrim p;
    p.kind = MenuPrim::kPolyG4;
    p.gouraud = true;
    p.semi = (a & 0x2000000u) != 0;
    p.x[0] = int16_t(left), p.y[0] = int16_t(y), p.x[1] = int16_t(right), p.y[1] = int16_t(y);
    p.x[2] = int16_t(left), p.y[2] = int16_t(y + h), p.x[3] = int16_t(right), p.y[3] = int16_t(y + h);
    p.colour[0] = p.colour[2] = a & 0xFFFFFF;
    p.colour[1] = p.colour[3] = b & 0xFFFFFF;
    ot.Add(p);
}

void AddGradientQuad(MenuOtSlot& ot, int x, int y, int w, int h, uint32_t c0, uint32_t c1) {
    MenuPrim p;
    p.kind = MenuPrim::kPolyG4;
    p.gouraud = true;
    p.semi = true; // 0x3A
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.x[1] = int16_t(x + w), p.y[1] = int16_t(y);
    p.x[2] = int16_t(x), p.y[2] = int16_t(y + h), p.x[3] = int16_t(x + w), p.y[3] = int16_t(y + h);
    p.colour[0] = p.colour[2] = c0 & 0xFFFFFF;
    p.colour[1] = p.colour[3] = c1 & 0xFFFFFF;
    ot.Add(p);
}

// ---------------------------------------------------------------- options

OptionsScreen::OptionsScreen(const TitleAssets& assets, career::CareerState& state, PcSettings* pc) : assets_(assets), state_(state), pc_(pc) {
    const GuestImage& o = assets.ovl1;
    rows_[0] = ReadOptionRows(o, kGlobalOptionRows, kGlobalOptionCount);
    rows_[1] = ReadOptionRows(o, kRaceOptionRows, kRaceOptionCount);
    lists_[0] = MenuListWidget::Read(o, kGlobalWidget);
    lists_[1] = MenuListWidget::Read(o, kRaceWidget);
    for (int l = 0; l < 2; l++)
        for (int k = 0; k < 8; k++) rowOffsets_[size_t(l)][size_t(k)] = o.Get<int16_t>((l == 0 ? kGlobalArg : kRaceArg) + 4 + uint32_t(k) * 2);
    for (size_t b = 0; b < bands_.size(); b++) bands_[b] = Band::Read(o, kBands[b]);
    for (size_t p = 0; p < pageTitles_.size(); p++) pageTitles_[p] = o.Get<uint32_t>(kPageTitles + uint32_t(p) * 4);
    for (size_t c = 0; c < colours_.size(); c++) colours_[c] = o.Get<uint32_t>(kColours + uint32_t(c) * 4);
    x_ = o.Get<int16_t>(kSwitcher), y_ = o.Get<int16_t>(kSwitcher + 2), w_ = o.Get<int16_t>(kSwitcher + 4), h_ = o.Get<int16_t>(kSwitcher + 6);
    count_ = o.Get<int8_t>(kSwitcher + 8);
    if (pc_) count_++; // ours: the PC SETTINGS page
    keyData_ = KeyConfigData::Read(o, assets.exe);
    analogData_ = AnalogConfigData::Read(o, assets.exe);
    Start();
}

namespace {
const MenuListPad* g_optionPad = nullptr; // the pad the list update hands its callbacks (block + 8)
} // namespace

void OptionsScreen::Start() {
    // 0x8001D4B0 + 0x8001D4D4
    page_ = 0, previous_ = -1, anim_ = 0, slide_ = 0, arrows_ = 0, editing_ = 0, skip_ = 0, leaving_ = 0;
    for (int l = 0; l < 2; l++) {
        MenuListReset(lists_[size_t(l)], [this, l](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) {
            return RowCallback(l, command, w, row, draw);
        });
        MenuListOpen(lists_[size_t(l)]);
        lists_[size_t(l)].selection = -1;
    }
    bands_[0].anim = bands_[1].anim = bands_[2].anim = 0;
    bands_[3].anim = bands_[4].anim = -1;
    keyPage_.Init(); // 0x8001A7E0
    for (int port = 0; port < 2; port++) AnalogReset(analogPages_[size_t(port)], port, PadBlock(port), analogData_); // 0x8001C48C
}

int OptionsScreen::SwitcherAlpha() const {
    int a = 0;
    if (anim_ < -1) {
        const int v = ~int(anim_) * 0x80;
        a = v < 0 ? (v + 0xF) >> 4 : v >> 4;
    }
    if (anim_ > 0) a = anim_ < 12 ? (int(anim_) << 7) / 12 : 0x80;
    return a;
}

// 0x80018574 (the row callback of both option lists).
int32_t OptionsScreen::RowCallback(int list, int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) {
    if (row < 0 || row >= int(rows_[size_t(list)].size())) return 1;
    const OptionRow& r = rows_[size_t(list)][size_t(row)];
    if (command == kMenuListDraw && draw && draw->ot) {
        const int32_t v = int32_t(draw->alpha) * listAlpha_[size_t(list)];
        const bool dim = w.selection >= 0 && w.selection != row;
        DrawRow(*draw->ot, r, draw->x, draw->y + rowOffsets_[size_t(list)][size_t(row)], dim ? v >> 8 : v >> 7, w.selection == row);
    } else if (command == kMenuListTick && row == w.selection && g_optionPad) {
        const OptionInput in = OptionRowInput(state_, r, *g_optionPad);
        if (in.sound) sounds.push_back(5);
    }
    return 1;
}

// 0x800186E4 commands 0 (update), 3 (enter), 4 (page entered).
int32_t OptionsScreen::PageCallback(int command, int page, const MenuListPad* pad, MenuOtSlot*, int, int, int) {
    if (command == 0) {
        const MenuListPad* p = (editing_ && page == page_) ? pad : nullptr;
        if (page == 0 || page == 1) {
            MenuListWidget& w = lists_[size_t(page)];
            g_optionPad = p;
            const int32_t r = MenuListUpdate(w, p);
            g_optionPad = nullptr;
            if (r == -3) {
                sounds.push_back(6);
            } else if (r != -2) {
                sounds.push_back(1);
                w.selection = -1;
                editing_ = 0;
                skip_ = 1;
            }
        }
        if (page == 2) { // 0x8001A860 on the career's pad blocks; 0 = a port chose EXIT
            uint8_t* career = reinterpret_cast<uint8_t*>(&state_);
            uint8_t* blocks[2] = {career + 0x0A, career + 0x5C};
            const MenuListPad* pads[2] = {p, nullptr};
            if (keyPage_.Update(blocks, padTypes_, pads, keyData_, sounds) == 0) {
                sounds.push_back(1);
                keyPage_.Leave();
                editing_ = 0;
                skip_ = 1;
            }
        }
        if (page == 3 || page == 4) { // 0x8001C7B8 (the pads of both ports, the port's live bytes)
            AnalogPageObject& a = analogPages_[size_t(page - 3)];
            const std::array<MenuListPad, 2> pads = {pad ? *pad : MenuListPad{}, MenuListPad{}};
            const int r = AnalogUpdate(a, analogGlobals_, padTypes_, pads, analogRaw_[size_t(a.port & 1)], PadBlock(a.port), sounds);
            if (r == 0 || r == -2) {
                if (r == 0) sounds.push_back(1);
                a.state = 3; // 0x8001C684
                editing_ = 0;
                skip_ = 1;
            }
        }
        if (page == 5 && p && pc_) { // ours: up / down choose the row, left / right its value, cross / circle / triangle / square leave
            const uint32_t edges = p->pressed | p->repeat;
            if (p->pressed & (menu_list_pad::kBack | menu_list_pad::kChoose)) {
                sounds.push_back(1);
                pcRow_ = -1;
                editing_ = 0;
                skip_ = 1;
            } else if (edges & (menu_list_pad::kUp | menu_list_pad::kDown)) {
                const int row = std::clamp(int(pcRow_) + ((edges & menu_list_pad::kDown) ? 1 : -1), 0, kPcRowCount - 1);
                if (row != pcRow_) sounds.push_back(6);
                pcRow_ = int8_t(row);
            } else if (edges & (menu_list_pad::kLeft | menu_list_pad::kRight)) {
                if (StepPcRow(*pc_, pcRow_, (edges & menu_list_pad::kRight) ? 1 : -1)) sounds.push_back(5);
            }
        }
        for (Band& b : bands_) b.Tick();
        return 0;
    }
    if (command == 3) {
        editing_ = 1, skip_ = 1;
        if (page == 5) {
            pcRow_ = 0;
            sounds.push_back(1);
        } else if (page == 0 || page == 1) {
            lists_[size_t(page)].selection = 0;
            sounds.push_back(1);
        } else if (page == 2) {
            keyPage_.Enter(); // 0x8001A834
            sounds.push_back(1);
        } else if (page == 3 || page == 4) { // 0x8001C690: a neGcon-type controller in the page's port, else sound 0
            AnalogPageObject& a = analogPages_[size_t(page - 3)];
            if (AnalogEnter(a, analogGlobals_, padTypes_[size_t(a.port & 1)], PadBlock(a.port)) != 1) {
                notes.push_back("analog settings: no neGcon-type controller in the port");
                sounds.push_back(0);
                editing_ = 0;
            } else {
                sounds.push_back(1);
            }
        }
        return 0;
    }
    if (command == 4) { // the page left (+0x0E) fades its bands out, the new page's bands come in
        if (previous_ == 0) {
            for (int b = 0; b < 3; b++) bands_[size_t(b)].anim = int16_t(~bands_[size_t(b)].steps);
        } else if (previous_ == 1) {
            for (int b = 3; b < 5; b++) bands_[size_t(b)].anim = int16_t(~bands_[size_t(b)].steps);
        } else if (previous_ == 3 || previous_ == 4) {
            AnalogPageLeft(analogPages_[size_t(previous_ - 3)]); // 0x8001C648
        }
        if (page == 3 || page == 4) AnalogPageEntered(analogPages_[size_t(page - 3)], PadBlock(page - 3), analogData_); // 0x8001C610
        if (page == 0) bands_[0].anim = bands_[1].anim = bands_[2].anim = 0;
        else if (page == 1) bands_[3].anim = bands_[4].anim = 0;
        return 0;
    }
    return 0;
}

bool OptionsScreen::Update(const MenuListPad* pad, const MenuListPad* pad2) {
    sounds.clear();
    if (leaving_) { // 0x8001D4E8: anim -17 .. -1
        if (anim_ < -1) anim_++;
        return anim_ < -1;
    }
    int r = -2;
    if (anim_ < 0) {
        if (anim_ < -1) anim_++;
    } else {
        if (++anim_ > 0x38) anim_ = 12;
        if (slide_ > 0) slide_--;
        if (slide_ < 0) slide_++;
        if (slide_ == 0) previous_ = -1;
        if (previous_ >= 0) PageCallback(0, previous_, pad, nullptr, 0, 0, 0);
        PageCallback(0, page_, pad, nullptr, 0, 0, 0);
        if (!skip_) {
            arrows_ = 0;
            if (pad && !editing_) {
                if (pad->pressed & menu_list_pad::kBack) {
                    r = -1;
                } else {
                    const uint32_t choose = (page_ != 4 || !pad2) ? pad->pressed : pad2->pressed;
                    if (choose & menu_list_pad::kChoose) {
                        PageCallback(3, page_, pad, nullptr, 0, 0, 0);
                        r = page_;
                    } else if (count_ >= 2) {
                        const uint32_t dirs = pad->pressed | pad->repeat;
                        arrows_ = 1;
                        if (dirs & menu_list_pad::kLeft) {
                            previous_ = page_;
                            page_ = int8_t(page_ - 1 < 0 ? count_ - 1 : page_ - 1);
                            slide_ = -12;
                            PageCallback(4, page_, pad, nullptr, 0, 0, 0);
                            r = -3;
                        }
                        if (dirs & menu_list_pad::kRight) {
                            previous_ = page_;
                            page_ = int8_t(page_ + 1 >= count_ ? 0 : page_ + 1);
                            slide_ = 12;
                            PageCallback(4, page_, pad, nullptr, 0, 0, 0);
                            r = -3;
                        }
                    }
                }
            }
        } else {
            skip_ = 0;
        }
    }
    // 0x80019038
    if (r == -3) sounds.push_back(7);
    if (r == -1) {
        anim_ = -17;
        leaving_ = 1;
        sounds.push_back(4);
    }
    return true;
}

void OptionsScreen::DrawRow(MenuOtSlot& ot, const OptionRow& row, int x, int y, int alpha, bool selected) const {
    const HudFont& font = assets_.fonts[TitleAssets::kSmallFont];
    const uint32_t base = colours_[0];
    const int value = OptionValue(state_, row.id);
    const uint32_t labelColour = MenuListLerp(base, selected ? colours_[2] : colours_[1], alpha, 0x80);
    AddNumberText(ot, font, assets_.Text(row.label), x + row.labelX, y, 1, -2, 0, labelColour, 1);
    const int vx = x + row.valueX;
    if (row.kind == kVolume) {
        for (int k = 0; k < 16; k++) {
            const uint32_t c = MenuListLerp(base, value <= (k << 4) ? colours_[6] : colours_[7], alpha, 0x80);
            ot.Add(Tile(vx + k * 6, y - 0x14, 5, 0x14, c));
        }
    } else if (row.kind == kChoice) {
        for (int k = 0; k < row.max && size_t(k) < row.valueLabels.size(); k++) {
            const uint32_t c = MenuListLerp(base, k == value ? colours_[4] : colours_[3], alpha, 0x80);
            AddNumberText(ot, font, assets_.Text(row.valueLabels[size_t(k)]), vx + 0x3C * k, y, 1, -2, 0, c, 1);
        }
    } else if (row.kind == kSlider) {
        ot.Add(Tile(vx, y - 10, 0x8C, 4, MenuListLerp(base, 0x2785030u, alpha, 0x80)));
        const int span = int(row.max) - int(row.min);
        ot.Add(Tile(vx + ((value - row.min) * 0x8C) / (span ? span : 1) - 1, y - 0xD, 2, 10, MenuListLerp(base, 0x2909090u, alpha, 0x80)));
        const uint32_t textColour = MenuListLerp(base, colours_[1], alpha, 0x80);
        const int ty = y + 0x12;
        char buffer[32];
        AddText(ot, font, assets_.Text(0x801B9CADu), vx + 0x10, ty, 1, textColour, 1, TextAlign::kRight); // "1P"
        std::snprintf(buffer, sizeof(buffer), assets_.Text(0x801B9CB7u).c_str(), value >= 0 ? value * 10 : 0); // "-%dm"
        AddNumberText(ot, font, buffer, vx + 0x38, ty, 1, -2, 0, textColour, 1, true);
        AddText(ot, font, assets_.Text(0x801B9CB2u), vx + 0x60, ty, 1, textColour, 1, TextAlign::kRight); // "2P"
        std::snprintf(buffer, sizeof(buffer), assets_.Text(0x801B9CB7u).c_str(), value < 0 ? -value * 10 : 0);
        AddNumberText(ot, font, buffer, vx + 0x88, ty, 1, -2, 0, textColour, 1, true);
    } else if (row.kind == kLaps) {
        std::string text = assets_.Text(0x801B9B29u); // "1Lap"
        if (value > 1) {
            char buffer[32];
            std::snprintf(buffer, sizeof(buffer), assets_.Text(0x801B9B32u).c_str(), value); // "%dLap"
            text = buffer;
        }
        AddNumberText(ot, font, text, vx, y, 1, -2, 0, MenuListLerp(base, colours_[7], alpha, 0x80), 1);
    }
}

void OptionsScreen::DrawPage(MenuOtSlot& ot, int page, int x, int y, int alpha) const {
    const HudFont& header = assets_.fonts[TitleAssets::kHeaderFont];
    const HudFont& medium = assets_.fonts[TitleAssets::kMediumFont];
    const uint32_t titleColour = MenuListLerp(colours_[0], colours_[5], alpha, editing_ ? 0x200 : 0x80);
    AddText(ot, header, page < 5 ? assets_.Text(pageTitles_[size_t(page)]) : std::string("PC SETTINGS"), x, y + 0x14, 0, titleColour, 1, TextAlign::kCentre);
    if (page == 0 || page == 1) {
        MenuListWidget& w = const_cast<MenuListWidget&>(lists_[size_t(page)]);
        w.x = int16_t(x), w.y = int16_t(y + 0x2E);
        const_cast<OptionsScreen*>(this)->listAlpha_[size_t(page)] = alpha;
        MenuListDraw(w, ot);
        const uint32_t section = MenuListLerp(colours_[0], colours_[8], alpha, 0x80);
        struct Section { uint32_t text; int textY, band, bandY; };
        static constexpr Section kGlobal[3] = {{0x801B9ABFu, 0x3A, 0, 0x21}, {0x801B9AD1u, 0xD2, 1, 0xB9}, {0x801B9AD8u, 0x122, 2, 0x109}};
        static constexpr Section kRace[2] = {{0x801B9B06u, 0x3A, 3, 0x21}, {0x801B9B15u, 0x8A, 4, 0x71}};
        const Section* s = page == 0 ? kGlobal : kRace;
        const int n = page == 0 ? 3 : 2;
        for (int k = 0; k < n; k++) {
            AddText(ot, medium, assets_.Text(s[k].text), x - 0x8E, y + s[k].textY, 1, section, 1);
            bands_[size_t(s[k].band)].Draw(ot, x - 0x94, y + s[k].bandY);
            ot.DrawMode(0x220);
        }
    } else if (page == 2) { // 0x8001AA84: the key configuration at (x, y + 0x40)
        keyPage_.Draw(ot, assets_, keyData_, x, y + 0x40, alpha);
    } else if (page == 3 || page == 4) { // 0x8001CE28 at (x, y + 0x2E)
        const AnalogPageObject& a = analogPages_[size_t(page - 3)];
        AnalogDraw(ot, assets_, analogData_, a, analogGlobals_, padTypes_[size_t(a.port & 1)], const_cast<OptionsScreen*>(this)->PadBlock(a.port), x, y + 0x2E, alpha);
    } else if (page == 5 && pc_) { // ours: PC SETTINGS in the style of the GLOBAL OPTIONS rows (0x8001805C)
        const HudFont& small = assets_.fonts[TitleAssets::kSmallFont];
        const uint32_t section = MenuListLerp(colours_[0], colours_[8], alpha, 0x80);
        AddText(ot, medium, "DISPLAY", x - 0x8E, y + 0x3A, 1, section, 1);
        AddText(ot, medium, "GRAPHICS", x - 0x8E, y + 0x6E, 1, section, 1);
        for (int row = 0; row < kPcRowCount; row++) {
            // row 0 under DISPLAY (as before), the graphics rows under GRAPHICS, 18 lines apart
            const int rowY = row == 0 ? y + 0x2E + 10 + 24 : y + 0x6E + 22 + (row - 1) * 18;
            const bool selected = pcRow_ == row;
            const int a = pcRow_ >= 0 && !selected ? alpha >> 1 : alpha;
            const PcRow r = PcRowOf(*pc_, row);
            AddNumberText(ot, small, r.label, x - 124, rowY, 1, -2, 0, MenuListLerp(colours_[0], selected ? colours_[2] : colours_[1], a, 0x80), 1);
            if (row == 0) { // both units side by side, the chosen one highlighted (the GLOBAL OPTIONS style)
                for (int k = 0; k < 2; k++)
                    AddNumberText(ot, small, r.values[size_t(k)], x - 16 + 0x3C * k, rowY, 1, -2, 0,
                                  MenuListLerp(colours_[0], k == r.index ? colours_[4] : colours_[3], a, 0x80), 1);
            } else {
                AddNumberText(ot, small, r.values[size_t(r.index)], x + 8, rowY, 1, -2, 0, MenuListLerp(colours_[0], colours_[4], a, 0x80), 1);
            }
        }
    } else {
        const uint32_t c = MenuListLerp(colours_[0], colours_[8], alpha, 0x80);
        AddText(ot, medium, "Not available yet", x, y + 0x80, 1, c, 1, TextAlign::kCentre); // ours: the page is not ported
    }
}

std::vector<MenuPrim> OptionsScreen::Frame() const {
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot header, content;
    AddViewHeader(header, assets_, assets_.Text(kViewTitle), kViewColour, 0x80);
    // 0x8001D7C0
    if (anim_ < 0) {
        if (anim_ < -1) {
            const int v = ~int(anim_) * 0x80;
            DrawPage(content, page_, x_, y_, v < 0 ? (v + 0xF) >> 4 : v >> 4);
        }
    } else {
        const int arrowPhase = anim_ - 12;
        if (arrowPhase >= 0 && (arrows_ & 1)) {
            const int dx = (int(int16_t(w_)) >> 1) + 8;
            auto arrow = [&](int ax, int dir) { // 0x8006BA48: POLY_F3 0x22 semi, r = 255 * k / 10, g = r / 2
                int k = 0x28 - arrowPhase;
                k = std::clamp(k, 0, 10);
                const uint32_t r = uint32_t(k * 0xFF) / 10;
                MenuPrim p;
                p.kind = MenuPrim::kPolyF4;
                p.semi = true;
                p.x[0] = int16_t(ax + dir), p.y[0] = y_;
                p.x[1] = int16_t(ax), p.y[1] = int16_t(y_ + 10);
                p.x[2] = int16_t(ax), p.y[2] = int16_t(y_ - 10);
                p.x[3] = p.x[2], p.y[3] = p.y[2];
                p.colour[0] = r | (r >> 1) << 8;
                content.Add(p);
            };
            arrow(x_ + dx + 6, 6);
            arrow(x_ - dx - 6, -6);
            content.DrawMode(0x20);
        }
        const int fade = anim_ < 12 ? (int(anim_) << 7) / 12 : 0x80;
        const int slide = slide_, magnitude = slide < 0 ? -slide : slide;
        const int offset = (int(w_) * slide) / 12;
        const int a = (fade * (12 - magnitude)) / 12;
        if (previous_ >= 0) DrawPage(content, previous_, x_ + offset + (slide < 0 ? w_ : -w_), y_, 0x80 - a);
        DrawPage(content, page_, x_ + offset / 2, y_, a);
    }
    header.Emit(prims, 0x200);
    content.Emit(prims, 0x200);
    return prims;
}

// ---------------------------------------------------------------- memory cards

namespace {
constexpr size_t kFrame = 128, kCardSize = 128 * 1024;
constexpr char kSaveName[] = "BASCUS-94455GAME"; // EXE 0x80091C94
// Strings of data-global.txd (0x801EF6B0 block).
constexpr uint32_t kSelectSlot = 0x801EF709u, kChecking = 0x801EF726u, kDoNotRemove = 0x801EF744u, kNoCard = 0x801EF76Fu, kReadFailed = 0x801EF78Cu,
                   kNotFormatted = 0x801EF7B5u, kWantFormat = 0x801EF7D6u, kFormatting = 0x801EF7EEu, kFormatFailed = 0x801EF807u,
                   kStartLoading = 0x801EF880u, kLoadingNow = 0x801EF895u, kLoadingComplete = 0x801EF8A8u, kLoadDataFailed = 0x801EF8BFu,
                   kStartSaving = 0x801EF8DCu, kOverwriteFile = 0x801EF8F1u, kSavingNow = 0x801EF908u, kSavingComplete = 0x801EF91Bu,
                   kSaveDataFailed = 0x801EF951u, kFileExists = 0x801EF96Au, kNoGameFile = 0x801EF997u, kLoadingFailed = 0x801EF9BFu,
                   kCreateFailed = 0x801EF9D6u, kNotEnoughBlocks = 0x801EF6EAu, kMemoryCard1 = 0x801EFB39u, kMemoryCard2 = 0x801EFB4Au;
constexpr uint32_t kCheckingReplay = 0x801EF824u, kNoReplayFiles = 0x801EFA00u, kNoReplayData = 0x801EFA22u, kReplayLoadFailed = 0x801EFA7Cu;
constexpr uint32_t kReplayList = 0x80091FE4u, kReplayRowStyle = 0x800921BCu; // the list widget of modes 0..3 (0x8007263C) and the row style
// Mode 2 (0x8006F6E8 / 0x8006FF8C / 0x8006F5DC / 0x800705D0) and the save modes' hints.
constexpr uint32_t kSelectBlocks = 0x801EF6CBu, kCreateReplayFailed = 0x801EFA4Du, kNoMatchingData = 0x801EFA93u, kNeedSectors = 0x801EFB8Bu;
constexpr uint32_t kChangesNotSaved = 0x801EFD7Eu, kCancelQuestion = 0x801EFDA2u, kEditReplayFile = 0x801EFBC2u, kDeleteModeHint = 0x801EFBA2u,
                   kTotalReplays = 0x801EFB5Bu, kSectorsFree = 0x801EFB74u, kTrySavingAgain = 0x801EF933u;
// The sector bar 0x8006AA68: colours EXE 0x80091E98 (used), 0x80091E9C (the selection), 0x80091EA0, 0x80091EA4 (frame); all fade
// toward 0x80091E7C.
constexpr uint32_t kSectorUsed = 0x80091E98u, kSectorSelected = 0x80091E9Cu, kSectorEnd = 0x80091EA0u, kSectorFrame = 0x80091EA4u, kFadeTarget = 0x80091E7Cu;
// Button bars (templates in the EXE; the manager object's bar at + 0x50 + k * 0x98, in the order 0x8007263C sets them up).
enum BarIndex {
    kBarError = 0,     // + 0x050 0x80091F04
    kBarSlot = 1,      // + 0x0E8 0x80091F34
    kBarNoCard = 2,    // + 0x180 0x80091F64
    kBarFormat = 3,    // + 0x218 0x80091F94
    kBarRetry = 4,     // + 0x2B0 0x80092018 (state 0xF "Try Saving Again?")
    kBarSaveError = 5, // + 0x348 0x80092048
    kBarOk = 6,        // + 0x3E0 0x80092078
    kBarOverwrite = 7, // + 0x478 0x800920A8
    kBarSave = 8,      // + 0x510 0x800920D8
    kBarLoad = 9,      // + 0x5A8 0x80092108
    kBarCancel = 10,   // + 0x640 0x80092138 (state 0x15 "Cancel?")
};
constexpr uint32_t kBarTemplates[11] = {0x80091F04u, 0x80091F34u, 0x80091F64u, 0x80091F94u, 0x80092018u, 0x80092048u,
                                        0x80092078u, 0x800920A8u, 0x800920D8u, 0x80092108u, 0x80092138u};
std::string FormatInt(const std::string& format, int a, int b = 0) { // the "%d" of 0x8008CF34 (one or two values)
    std::string out;
    int n = 0;
    for (size_t i = 0; i < format.size(); i++) {
        if (format[i] == '%' && i + 1 < format.size() && format[i + 1] == 'd') {
            out += std::to_string(n++ == 0 ? a : b);
            i++;
        } else {
            out.push_back(format[i]);
        }
    }
    return out;
}
} // namespace

int CardStatus(const CardSlot& slot, std::vector<uint8_t>* image) {
    if (!slot.Present()) return 2;
    std::error_code ec;
    if (!std::filesystem::exists(slot.path, ec)) return 3; // ours: a card file that does not exist yet = an unformatted card
    std::vector<uint8_t> bytes;
    try {
        bytes = career::ReadFileBytes(slot.path);
    } catch (const std::exception&) {
        return 4;
    }
    if (bytes.size() != kCardSize) return 4;
    if (bytes[0] != 'M' || bytes[1] != 'C') return 3; // 0x8007EDC4
    if (image) *image = std::move(bytes);
    return 0;
}

int CardFreeBlocks(const std::vector<uint8_t>& image) {
    int used = 0;
    for (int e = 1; e <= 15; e++) {
        if (image[size_t(e) * kFrame] != 0x51) continue;
        int b = e, guard = 0;
        while (b >= 1 && b <= 15 && guard++ < 15) {
            used++;
            const uint16_t next = uint16_t(image[size_t(b) * kFrame + 8] | image[size_t(b) * kFrame + 9] << 8);
            if (next == 0xFFFF) break;
            b = int(next) + 1;
        }
    }
    return 15 - used;
}

int CardFindFile(const std::vector<uint8_t>& image, const std::string& name) {
    for (int e = 1; e <= 15; e++) {
        const uint8_t* f = &image[size_t(e) * kFrame];
        if (f[0] != 0x51) continue;
        if (std::strncmp(reinterpret_cast<const char*>(f + 10), name.c_str(), 20) == 0) return e;
    }
    return -1;
}

CardManager::Bar CardManager::ReadBar(const GuestImage& exe, uint32_t t) { // a bar template (0x30 bytes)
    Bar b;
    b.x = exe.Get<int16_t>(t), b.y = exe.Get<int16_t>(t + 2);
    b.title = exe.Get<uint32_t>(t + 4), b.label0 = exe.Get<uint32_t>(t + 8), b.label1 = exe.Get<uint32_t>(t + 12);
    b.titleColour = exe.Get<uint32_t>(t + 16), b.labelColour = exe.Get<uint32_t>(t + 20), b.fill = exe.Get<uint32_t>(t + 24);
    b.gradient = exe.Get<uint32_t>(t + 28);
    b.flags = exe.Get<uint16_t>(t + 32);
    b.labelExtra = exe.Get<int8_t>(t + 0x22);
    const uint16_t size = b.flags & 6; // 0x8006E1CC
    b.w = size == 6 ? 0x80 : (size == 2 || size == 4) ? 0x60 : 0x50;
    b.h = (size == 4 || size == 6) ? 0x18 : 0x0C;
    return b;
}

CardManager::CardManager(const TitleAssets& assets, Mode mode, career::CareerState& state, std::array<CardSlot, 2> slots)
    : assets_(assets), mode_(mode), career_(state), slots_(std::move(slots)) {
    for (size_t k = 0; k < bars_.size(); k++) bars_[k] = ReadBar(assets.exe, kBarTemplates[k]);
    // mgr + 0x34: the "Memory Card N" band {w 156, h 16, steps 14, c0 0x02783618, c1 0x02000000, target 0x02DEDEDE,
    // targetOut 0x02000000} as the manager object holds it (read from the RAM dump work/re/title/s_save/ram_001990.bin;
    // where 0x8007263C takes it from is not traced).
    headerBand_.w = 0x9C, headerBand_.h = 0x10, headerBand_.steps = 0x0E, headerBand_.flags = 0;
    headerBand_.c0 = 0x02783618u, headerBand_.c1 = 0x02000000u, headerBand_.target = 0x02DEDEDEu, headerBand_.targetOut = 0x02000000u;
    headerBand_.anim = -1;
    if (mode_ == kSaveGame) { // 0x80072F9C: the file image of the career block as it is now
        image_.state = career_;
        image_.header = career::BuildSaveHeader(assets.exe);
    }
    { // 0x8007263C: the block selector 0x80091FC4 (0x8006D9C8: hidden); its font word names one of the manager's fonts
        constexpr uint32_t kBlocks = 0x80091FC4u;
        const uint32_t font = assets.exe.Get<uint32_t>(kBlocks + 0x10);
        blocks_ = ReadBlockSelector(assets.exe, kBlocks,
                                    font == 0x801C94D4u ? TitleAssets::kTinyFont : font == 0x801C94B4u ? TitleAssets::kSmallFont : TitleAssets::kMediumFont);
    }
    if (mode_ <= kLoadGhost) { // 0x8007263C: the list 0x80091FE4 reset with the row callback 0x8006F060
        replayList_ = MenuListWidget::Read(assets.exe, kReplayList);
        replayStyle_.titleSpacing = assets.exe.Get<int16_t>(kReplayRowStyle + 6);
        replayStyle_.detailSpacing = assets.exe.Get<int16_t>(kReplayRowStyle + 0xE);
        replayStyle_.rowColour = 0x5A4A3Eu; // 0x8006F060 writes the style's +0x10 (DAT_800921CC) before each row: 0x5A4A3E (0x324052 in delete mode)
        MenuListReset(replayList_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { return ReplayRowCallback(command, w, row, draw); });
    }
    Switch(0);
}

void CardManager::OpenBar(Bar& b) { // 0x8006E388
    b.anim = 0;
    b.slide = 0;
    b.flags |= 0x80;
    b.open = true;
}

void CardManager::CloseBar(Bar& b) { // 0x8006E3FC
    b.anim = -17;
    b.open = false;
}

int CardManager::UpdateBar(Bar& b, const MenuListPad* pad, std::vector<int>& sounds) { // 0x8006E43C
    if (b.anim < 0) {
        if (b.anim < -1) b.anim++;
        return -2;
    }
    if (++b.anim > 0x47) {
        b.anim = 12;
        b.flags &= 0xFF7F;
    }
    if (b.slide > 0) b.slide--;
    if (!pad) return -2;
    const uint32_t pressed = pad->pressed;
    int r = -2;
    if (pressed & menu_list_pad::kBack) {
        r = -1;
    } else if (pressed & menu_list_pad::kChoose) {
        r = b.cursor;
    } else {
        int c = b.cursor;
        if (pressed & menu_list_pad::kLeft) c = 0;
        if (pressed & menu_list_pad::kRight) c = 1;
        if (c != b.cursor) {
            if (b.anim > 11) b.anim = 12;
            b.slide = 6;
            b.flags |= 0x80;
            sounds.push_back(6); // template byte +0x2C
            r = -3;
        }
        b.cursor = int8_t(c != 0);
        return r;
    }
    if (b.flags & 8) CloseBar(b);
    return r;
}

int CardManager::Status() const { return CardStatus(slots_[size_t(slot_)]); }

void CardManager::ProgressReset(uint32_t total) { // 0x8006C04C
    progressTotal_ = total;
    progressVisible_ = 0;
    progress_.fill(-1);
}

void CardManager::ProgressUpdate(uint32_t remaining) { // 0x8006C074
    if (progressVisible_ < 0) return;
    const uint32_t total = progressTotal_;
    int32_t done = int32_t(total - remaining);
    if (done < 0) done = 0;
    if (int32_t(total) < done) done = int32_t(total);
    uint32_t p;
    if (total < 40000) p = total ? (uint32_t(done) * uint32_t(done)) / total : 0;
    else p = (uint32_t(done) * uint32_t(done >> 8)) / (total >> 8);
    for (int i = 0; i < 32; i++) {
        if (progress_[size_t(i)] < 0) {
            if (int32_t((uint32_t(i) * total) >> 5) <= int32_t(p)) progress_[size_t(i)] = 0;
        } else if (++progress_[size_t(i)] > 0x18) {
            progress_[size_t(i)] = 0x18;
        }
    }
}

// 0x8006F060: command 4 draws the row with the row printer (into the list's OT slot + 1), command 8 = enabled (mode 1: the
// entry is playable, +0x42 != 1).
int32_t CardManager::ReplayRowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) {
    if (row < 0 || row >= int(replayRows_.size())) return 0;
    const ReplayRow& r = replayRows_[size_t(row)];
    if (command == kMenuListEnabled) { // modes 0 / 2 in delete mode: only the entries
        if (deleteMode_) return r.kind == kReplayRowEntry ? 1 : 0;
        return r.enabled ? 1 : 0; // mode 0: the payload fits (+ 0x892)
    }
    if (command != kMenuListDraw || !draw || !draw->ot || !replayText_ || !replayFile_) return 0;
    int x = 0, alpha = 0;
    if (!ReplayRowPlacement(w, *draw, x, alpha)) return 0;
    const ReplayCardEntry e = r.kind == kReplayRowEntry ? replayFile_->Entry(r.entry) : ReplayCardEntry{};
    // DAT_800921CC: the row colour 0x5A4A3E (0x324052 in the delete mode of modes 0 / 2)
    ReplayRowStyle style = replayStyle_;
    style.rowColour = deleteMode_ ? 0x324052u : 0x5A4A3Eu;
    DrawReplayRow(rowSlot_ ? *rowSlot_ : *draw->ot, assets_, *replayText_, style, r.kind == kReplayRowEntry ? &e : nullptr, r.kind, x, draw->y, alpha);
    return 0;
}

// Handlers of the table 0x800921D4 (entry h(0, 0)); returns the next state or -1.
int CardManager::Enter(int id) {
    std::vector<uint8_t> card;
    switch (id) {
    case 0: delay_ = 0x18; line1_ = line2_ = 0; return -1;
    case 2:
        line1_ = kSelectSlot, line2_ = 0;
        headerBand_.anim = int16_t(~headerBand_.steps); // +0x4C = ~+0x38
        OpenBar(bars_[kBarSlot]);
        return -1;
    case 3:
        line1_ = kChecking, line2_ = 0;
        headerBand_.anim = 0;
        return Step();
    case 4: line1_ = kNoCard, line2_ = 0; OpenBar(bars_[kBarNoCard]); return -1;
    case 5:
        line1_ = kNotFormatted, line2_ = kWantFormat;
        bars_[kBarFormat].cursor = 1;
        OpenBar(bars_[kBarFormat]);
        return -1;
    case 6: {
        line1_ = kFormatting, line2_ = kDoNotRemove;
        try { // 0x8007F5A8: ours writes a formatted card image
            const std::filesystem::path path = slots_[size_t(slot_)].path;
            if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
            career::WriteFileBytes(path.string(), career::FormatMemoryCard());
            log.push_back("formatted " + path.string());
            delay_ = 0;
        } catch (const std::exception& e) {
            log.push_back(std::string("format failed: ") + e.what());
            delay_ = -1;
        }
        return -1;
    }
    case 9:
        sounds.push_back(0);
        line1_ = kNotEnoughBlocks, line2_ = 0;
        bars_[kBarError].cursor = 0;
        OpenBar(bars_[kBarError]);
        return -1;
    case 0x1D:
        line1_ = kFileExists, line2_ = kOverwriteFile;
        bars_[kBarOverwrite].cursor = 1;
        OpenBar(bars_[kBarOverwrite]);
        return -1;
    case 0x20:
        line1_ = kStartSaving, line2_ = 0;
        bars_[kBarSave].cursor = 1;
        OpenBar(bars_[kBarSave]);
        return -1;
    case 0x1F: { // 0x8006A0FC -> 0x8007D428: create "BASCUS-94455GAME" with 4 blocks
        line1_ = kSavingNow, line2_ = kDoNotRemove;
        const int status = CardStatus(slots_[size_t(slot_)], &card);
        if (status != 0 || CardFindFile(card, kSaveName) >= 0 || CardFreeBlocks(card) < kSaveBlocks) {
            error_ = kCreateFailed;
            return 0x21;
        }
        delay_ = 1; // the create op completes on the next poll
        return -1;
    }
    case 0x1E:
        line1_ = kSavingNow, line2_ = kDoNotRemove;
        progressColour_ = 0x000C5090u;
        ProgressReset(0x7F00 + 0x80);
        transferLeft_ = 0x7F00 + 0x80;
        return -1;
    case 0x10:
        line1_ = kSavingComplete, line2_ = 0;
        bars_[kBarOk].cursor = 1;
        OpenBar(bars_[kBarOk]);
        return -1;
    case 0x21:
        sounds.push_back(0);
        line1_ = kSaveDataFailed, line2_ = 0;
        bars_[kBarSaveError].cursor = 0;
        OpenBar(bars_[kBarSaveError]);
        return -1;
    case 1:
        sounds.push_back(0);
        line1_ = line2_ = 0;
        bars_[kBarError].cursor = 0;
        OpenBar(bars_[kBarError]);
        return -1;
    case 0x22:
        line1_ = kStartLoading, line2_ = 0;
        bars_[kBarLoad].cursor = 1;
        OpenBar(bars_[kBarLoad]);
        return -1;
    case 0x23:
        line1_ = kLoadingNow, line2_ = kDoNotRemove;
        progressColour_ = 0x0090500Cu;
        ProgressReset(0x7F00 + 0x80);
        transferLeft_ = 0x7F00 + 0x80;
        return -1;
    case 0x25:
        line1_ = kLoadingComplete, line2_ = 0;
        bars_[kBarOk].cursor = 1;
        OpenBar(bars_[kBarOk]);
        return -1;
    case 0x24:
        sounds.push_back(0);
        line1_ = kNoGameFile, line2_ = 0;
        bars_[kBarError].cursor = 0;
        OpenBar(bars_[kBarError]);
        return -1;
    case 7: // 0x8006F3E0: the replay file's directory (0x1580 bytes, 0x800697AC -> 0x8007D658(6)) with a progress bar
        line1_ = kCheckingReplay, line2_ = 0;
        progressColour_ = 0x0090500Cu;
        ProgressReset(0x150C);
        transferLeft_ = 0x150C;
        return -1;
    case 0x11: // 0x80072114 (modes 1..3; 0x16 in mode 2): "No Replay Files Found", ERROR! Change Slot / Exit
    case 0x16:
        sounds.push_back(0);
        line1_ = kNoReplayFiles, line2_ = 0;
        bars_[kBarError].cursor = 0;
        OpenBar(bars_[kBarError]);
        return -1;
    case 0x14: // 0x800722A4 (0x18 in mode 2): the lines of state 7 stay ("No Replay Data in File")
    case 0x18:
        sounds.push_back(0);
        bars_[kBarError].cursor = 0;
        OpenBar(bars_[kBarError]);
        return -1;
    case 0x12: { // 0x80070868: the list of the file's replays (count rows, selection 0), sound 7
        line1_ = line2_ = 0;
        replayRows_.clear();
        for (int i = 0; i < replayFile_->Count(); i++) {
            const bool playable = !replayFile_->Entry(i).Ghost(); // 0x8006932C
            replayRows_.push_back({int8_t(i), int8_t(kReplayRowEntry), playable, playable});
        }
        replayList_.count = int16_t(replayRows_.size());
        replayList_.selection = 0;
        MenuListOpen(replayList_);
        sounds.push_back(7);
        return -1;
    }
    case 0x17: // 0x8006F6E8 h(0) (mode 2; also state 10): the lines cleared, the file's sectors, the rows (the list opens later)
    case 0x0A:
        line1_ = line2_ = 0;
        sectorsTotal_ = replayFile_->Total();                  // + 0x934 = file + 0x202
        sectorsUsed_ = int16_t(replayFile_->UsedSectors());    // + 0x936 = 0x80068FE8
        BuildReplayRows();
        return -1;
    case 0x15: // 0x8006F5DC: "Changes are not Saved" / "Cancel?", the bar + 0x640 with No (+ 0x6BD = 1)
        bars_[kBarCancel].cursor = 1;
        OpenBar(bars_[kBarCancel]);
        line1_ = kChangesNotSaved, line2_ = kCancelQuestion;
        return -1;
    case 0x0B: return -1; // 0x8006FF8C h(0): nothing (the keyboard opens after + 0x12 fields)
    case 8: { // 0x8006F298 h(0) (mode 0, no replay file on the card): "Select Number of Blocks" 3 .. the card's free blocks
        const int free = CardStatus(slots_[size_t(slot_)], &card) == 0 ? CardFreeBlocks(card) : 0; // 0x801F097E + slot * 0x264
        if (free < 3) return 9;
        blocks_.min = 3, blocks_.max = int16_t(free), blocks_.value = 3, blocks_.anim = 0;
        line1_ = kSelectBlocks, line2_ = 0;
        return -1;
    }
    case 0x0C: { // 0x80070254 h(0): 0x800696EC = 0x8007D428(slot, "BASCUS-94455REPLAY", blocks << 13)
        if (CardStatus(slots_[size_t(slot_)], &card) != 0 || CardFindFile(card, kReplayCardFileName) >= 0 || CardFreeBlocks(card) < replayFile_->Blocks()) {
            error_ = kCreateReplayFailed;
            return 1;
        }
        line1_ = kSavingNow, line2_ = kDoNotRemove;
        delay_ = 1; // the create op completes on the next poll
        return -1;
    }
    case 0x1A: { // 0x80070C14 h(0) (mode 3): the ghost entries of the race's course
        line1_ = line2_ = 0;
        replayRows_.clear();
        for (int i = 0; i < replayFile_->Count(); i++) {
            const ReplayCardEntry e = replayFile_->Entry(i);
            if (e.desc[0x42] == 1 && e.CourseId() == ghostCourse_) replayRows_.push_back({int8_t(i), int8_t(kReplayRowEntry), true, true});
        }
        if (replayRows_.empty()) {
            line1_ = kNoMatchingData, line2_ = 0;
            return 0x14;
        }
        replayList_.count = int16_t(replayRows_.size());
        replayList_.selection = 0;
        MenuListOpen(replayList_);
        sounds.push_back(7);
        return -1;
    }
    case 0x1B: { // 0x80070E38 h(0): the entry's sectors (0x800697E8) with a progress bar (colour 0x50782C)
        const ReplayCardEntry e = replayFile_->Entry(renameIndex_);
        progressColour_ = 0x0050782Cu;
        const uint32_t total = (uint32_t(e.size) + 0x7F) & ~0x7Fu;
        ProgressReset(total);
        transferLeft_ = int(total);
        line1_ = kLoadingNow, line2_ = kDoNotRemove;
        return -1;
    }
    case 0x1C: // 0x800717B8 (the same as 0x25): "Loading Complete", OK Change Slot / Exit [Exit]
        line1_ = kLoadingComplete, line2_ = 0;
        bars_[kBarOk].cursor = 1;
        OpenBar(bars_[kBarOk]);
        return -1;
    case 0x19: // 0x8007031C (mode 2; also state 0xD, the retry): the directory CRC (0x800693EC), the progress over 0x150C bytes
    case 0x0D:
        if (mode_ == kSaveReplay) { // mode 0: the entry's sectors (0x80069758), the progress over its size + 0x150C
            progressColour_ = 0x000C5090u;
            ProgressReset(uint32_t(savePayload_.size()) + 0x150C);
            transferLeft_ = int((savePayload_.size() + 0x7F) & ~size_t(0x7F));
            line1_ = kSavingNow, line2_ = kDoNotRemove;
            return -1;
        }
        replayFile_->UpdateCrc();
        progressColour_ = 0x000C5090u;
        ProgressReset(0x150C);
        line1_ = kSavingNow, line2_ = kDoNotRemove;
        return 0x0E;
    case 0x0E: // 0x800704C4 h(0): 0x8006971C = 0x8007D658(7): the file's first 0x1580 bytes (header and directory) to the card
        line1_ = kSavingNow, line2_ = kDoNotRemove;
        transferLeft_ = 0x1580;
        return -1;
    case 0x0F: // 0x800705D0: "Saving Data Failed" / "Try Saving Again?", the bar + 0x2B0 on Yes (no card: 0x21)
        if (Status() == 2) return 0x21;
        sounds.push_back(0);
        bars_[kBarRetry].cursor = 0;
        OpenBar(bars_[kBarRetry]);
        line1_ = kSaveDataFailed, line2_ = kTrySavingAgain;
        return -1;
    case 0x13: { // 0x80070A6C: the entry's sectors (0x800697E8) with a progress bar (colour 0x50782C, the payload rounded to sectors)
        const ReplayCardEntry e = replayFile_->Entry(replayRows_[size_t(replayChosen_)].entry);
        progressColour_ = 0x0050782Cu;
        const uint32_t total = (uint32_t(e.size) + 0x7F) & ~0x7Fu;
        ProgressReset(total);
        transferLeft_ = int(total);
        line1_ = kLoadingNow, line2_ = kDoNotRemove;
        return -1;
    }
    default: return -1;
    }
}

int CardManager::Step() {
    auto choice = [&](int bar) { return barResult_[size_t(bar)]; };
    switch (state_) {
    case 0: return --delay_ < 0 ? 2 : -1;
    case 2: {
        const int r = choice(kBarSlot);
        if (r == 0 || r == 1) {
            sounds.push_back(1);
            slot_ = r;
            return 3;
        }
        return r == -1 ? kExit : -1;
    }
    case 3: {
        std::vector<uint8_t> card;
        const int status = CardStatus(slots_[size_t(slot_)], &card);
        const bool found = status == 0 && CardFindFile(card, kSaveName) >= 0;
        if (mode_ == kSaveGame) {
            switch (status) {
            case 1: return -1;
            case 2: return 4;
            case 3: return 5;
            case 4: error_ = kReadFailed; return 1;
            default: return found ? 0x1D : CardFreeBlocks(card) < kSaveBlocks ? 9 : 0x20;
            }
        }
        if (mode_ == kSaveReplay) { // 0x80071B30 mode 0: a file -> 7 (its directory), none -> 8 (a new file's blocks)
            switch (status) {
            case 1: return -1;
            case 2: return 4;
            case 3: return 5;
            case 4: error_ = kReadFailed; return 1;
            default: fileOnCard_ = CardFindFile(card, kReplayCardFileName) >= 0; return fileOnCard_ ? 7 : 8;
            }
        }
        if (mode_ == kLoadReplay || mode_ == kRenameReplay || mode_ == kLoadGhost) { // 0x80071B30 modes 1 / 3 (no file: 0x11) and 2 (0x16)
            const int noFile = mode_ == kRenameReplay ? 0x16 : 0x11;
            switch (status) {
            case 1: return -1;
            case 2: return 4;
            case 4: error_ = kReadFailed; return 1;
            case 0: return CardFindFile(card, kReplayCardFileName) >= 0 ? 7 : noFile;
            default: return noFile;
            }
        }
        switch (status) {
        case 1: return -1;
        case 2: return 4;
        case 4: error_ = kReadFailed; return 1;
        default: return found ? 0x22 : 0x24;
        }
    }
    case 4:
        if (Status() == 2) {
            const int r = choice(kBarNoCard);
            if (r == 0) { sounds.push_back(1); return 2; }
            if (r == 1 || r == -1) return kExit;
            return -1;
        }
        CloseBar(bars_[kBarNoCard]);
        return 3;
    case 5:
        if (Status() == 3) {
            const int r = choice(kBarFormat);
            if (r == 0) { sounds.push_back(1); return 6; }
            if (r == 1 || r == -1) { sounds.push_back(1); return 2; }
            return -1;
        }
        CloseBar(bars_[kBarFormat]);
        return 3;
    case 6: // 0x80072044: 0x8007D7CC poll: 0 done -> file creation (mode 4) or the slot check again, else failure
        if (delay_ < 0) { error_ = kFormatFailed; return 1; }
        return mode_ == kSaveGame ? 0x1F : 3;
    case 9:
        if (Status() == 0) {
            const int r = choice(kBarError);
            if (r == 0) { sounds.push_back(1); return 2; }
            if (r == 1 || r == -1) return kExit;
            return -1;
        }
        CloseBar(bars_[kBarError]);
        return 3;
    case 0x1D:
    case 0x20:
    case 0x22: {
        const int bar = state_ == 0x1D ? kBarOverwrite : state_ == 0x20 ? kBarSave : kBarLoad;
        if (Status() == 0) {
            const int r = choice(bar);
            if (r == 0) { sounds.push_back(1); return state_ == 0x1D ? 0x1E : state_ == 0x20 ? 0x1F : 0x23; }
            if (r == 1) { sounds.push_back(1); return 2; }
            if (r == -1) { sounds.push_back(2); return 2; }
            return -1;
        }
        CloseBar(bars_[size_t(bar)]);
        return 3;
    }
    case 0x1F: return --delay_ < 0 ? 0x1E : -1;
    case 0x1E:
    case 0x23: {
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        ProgressUpdate(uint32_t(transferLeft_));
        if (transferLeft_ > 0) return -1;
        progressVisible_ = -1;
        const std::string path = slots_[size_t(slot_)].path;
        if (state_ == 0x1E) { // 0x8007D658(7): the file image (header, block, CRC) into the card's blocks
            try {
                std::vector<uint8_t> card = career::ReadFileBytes(path);
                career::StoreCareerOnCard(card, image_);
                career::WriteFileBytes(path, card);
                saved_ = true;
                log.push_back("saved " + path);
                return 0x10;
            } catch (const std::exception& e) {
                log.push_back(std::string("save failed: ") + e.what());
                error_ = kSaveDataFailed;
                return 0x21;
            }
        }
        try { // 0x8007D658(6) + 0x8006A314 CRC check + 0x8006A278
            const career::CareerSave loaded = career::LoadCareerFromCard(career::ReadFileBytes(path));
            if (!loaded.CrcOk()) {
                error_ = kLoadingFailed;
                return 1;
            }
            career_ = loaded.state;
            loaded_ = true;
            log.push_back("loaded " + path);
            return 0x25;
        } catch (const std::exception& e) {
            log.push_back(std::string("load failed: ") + e.what());
            error_ = kLoadDataFailed;
            return 1;
        }
    }
    case 0x10:
    case 0x25:
    case 1: {
        const int bar = state_ == 1 ? kBarError : kBarOk;
        const int r = choice(bar);
        if (r == 0) { sounds.push_back(1); return 2; }
        if (r == 1 || r == -1) return kExit;
        return -1;
    }
    case 0x21: {
        const int r = choice(kBarSaveError);
        if (r == 0) { CloseBar(bars_[kBarSaveError]); sounds.push_back(1); return 2; }
        if (r == 1) { CloseBar(bars_[kBarSaveError]); sounds.push_back(1); return kExit; }
        if (r == -1) sounds.push_back(0);
        return -1;
    }
    case 0x24: {
        const int status = Status();
        if (status == 2 || status == 4) { CloseBar(bars_[kBarError]); return 3; }
        const int r = choice(kBarError);
        if (r == 0) { sounds.push_back(1); return 2; }
        if (r == 1 || r == -1) return kExit;
        return -1;
    }
    case 7: { // 0x8006F3E0 step: the read completes -> the directory's count and 0x800691DC
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        ProgressUpdate(uint32_t(transferLeft_));
        if (transferLeft_ > 0) return -1;
        progressVisible_ = -1;
        try {
            const std::vector<uint8_t> file = ReadReplayCardFile(career::ReadFileBytes(slots_[size_t(slot_)].path));
            if (file.size() < kReplayDataStart) throw std::runtime_error("no replay file");
            replayFile_ = ReplayCardFile::FromBytes(file);
        } catch (const std::exception& e) {
            log.push_back(std::string("replay file: ") + e.what());
            error_ = kLoadDataFailed;
            return 1;
        }
        int next = 0x12;
        if (mode_ == kRenameReplay) { // 0x8006F3E0 mode 2: the list after one field, selection 0
            next = 0x17;
            listDelay_ = 1;
            keptSelection_ = 0;
        }
        if (mode_ == kSaveReplay) { // mode 0: the list after one field (no "no data" case: "- New File -")
            next = 0x0A;
            listDelay_ = 1;
        }
        if (mode_ == kLoadGhost) next = 0x1A;
        if (replayFile_->Count() == 0 && mode_ != kSaveReplay) {
            line1_ = kNoReplayData, line2_ = 0;
            next = mode_ == kRenameReplay ? 0x18 : 0x14;
        }
        if (!replayFile_->Valid()) {
            error_ = kReplayLoadFailed;
            return 1;
        }
        log.push_back("replay file: " + std::to_string(replayFile_->Count()) + " replay(s), " + std::to_string(replayFile_->FreeSectors()) + " of " +
                      std::to_string(replayFile_->Total()) + " sectors free");
        return next;
    }
    case 0x11: // 0x80072114, modes 1..3: the bar while the slot's card is ready or unformatted
    case 0x16: {
        const int status = Status();
        if (status == 0 || status == 3) {
            const int r = choice(kBarError);
            if (r == 0) { sounds.push_back(1); return 2; }
            if (r == 1 || r == -1) return kExit;
            return -1;
        }
        CloseBar(bars_[kBarError]);
        return 3;
    }
    case 0x14: // 0x800722A4
    case 0x18: {
        if (Status() == 0) {
            const int r = choice(kBarError);
            if (r == 0) { sounds.push_back(1); return 2; }
            if (r == 1 || r == -1) return kExit;
            return -1;
        }
        CloseBar(bars_[kBarError]);
        return 3;
    }
    case 0x12: { // 0x80070868 step
        if (Status() != 0) {
            MenuListClose(replayList_);
            return 3;
        }
        const int32_t r = listResult_;
        if (r == -3) { sounds.push_back(6); return -1; }
        if (r == -4) { sounds.push_back(0); return -1; }
        if (r == -2) return -1;
        if (r == -1) {
            MenuListClose(replayList_);
            sounds.push_back(2);
            return 2;
        }
        sounds.push_back(1);
        replayChosen_ = r;
        MenuListClose(replayList_);
        return 0x13;
    }
    case 0x17: // 0x8006F6E8 h(1) (mode 2)
    case 0x0A: {
        if (listDelay_ > 0 && --listDelay_ == 0) {
            sounds.push_back(7);
            MenuListOpen(replayList_);
        }
        if (Status() != 0) {
            MenuListClose(replayList_);
            return 3;
        }
        const int32_t r = listResult_;
        if (r == -3) { sounds.push_back(6); return -1; }
        if (r == -4) { sounds.push_back(0); return -1; }
        if (r == -2 || r < -4) return -1;
        if (r == -1) {
            MenuListClose(replayList_);
            sounds.push_back(2);
            return mode_ == kSaveReplay ? 2 : 0x15;
        }
        sounds.push_back(1);
        if (mode_ == kRenameReplay) keptSelection_ = int16_t(r); // + 0x932
        const ReplayRow row = replayRows_[size_t(r)];
        if (mode_ == kRenameReplay && row.kind == kReplayRowOk) {
            MenuListClose(replayList_);
            return 0x19;
        }
        const int entry = row.kind == kReplayRowEntry ? r : -1;
        MenuListClose(replayList_);
        if (deleteMode_) { // 0x800695DC: the entry removed from the file in memory (written by "OK")
            replayFile_->Remove(entry);
            log.push_back("replay " + std::to_string(entry) + " deleted (not saved yet)");
            listDelay_ = 1;
            return 0x0A;
        }
        renameIndex_ = int16_t(entry); // + 0x930
        listDelay_ = 0x0C;
        return 0x0B;
    }
    case 0x15: { // 0x8006F5DC h(1)
        if (Status() != 0) {
            CloseBar(bars_[kBarCancel]);
            return 3;
        }
        const int r = choice(kBarCancel);
        if (r == 0) { sounds.push_back(1); return 2; }
        if (r == 1 || r == -1) {
            sounds.push_back(1);
            listDelay_ = 1;
            return 0x0A;
        }
        return -1;
    }
    case 0x0B: { // 0x8006FF8C h(1): the keyboard opens after 12 fields on the entry's title
        if (listDelay_ > 0 && --listDelay_ == 0) {
            const std::string title = renameIndex_ >= 0 ? replayFile_->Entry(renameIndex_).Title() : std::string();
            if (keyboard_) keyboard_->Open(title);
        }
        if (Status() != 0) {
            if (keyboard_) keyboard_->Close();
            return 3;
        }
        if (!keyboard_) { // our build without the widget: back to the list (nothing renamed)
            log.push_back("rename: no keyboard widget");
            listDelay_ = 0x0C;
            return 0x0A;
        }
        const int r = keyboardResult_;
        if (r == -1) {
            sounds.push_back(2);
            keyboard_->Close();
        } else if (r == 0) {
            sounds.push_back(1);
            keyboard_->Close();
            if (mode_ == kRenameReplay && renameIndex_ >= 0) { // 0x80069028
                replayFile_->SetEntryTitle(renameIndex_, keyboard_->Name());
                log.push_back("replay " + std::to_string(renameIndex_) + " renamed '" + keyboard_->Name() + "' (not saved yet)");
            }
            if (mode_ == kSaveReplay) { // 0x80069028 on the description, 0x80069418 into the file in memory, 0x800691DC
                ReplayCardEntry desc;
                desc.desc = saveDesc_;
                desc.SetTitle(keyboard_->Name());
                const bool added = renameIndex_ < 0;
                if (!replayFile_->Store(renameIndex_, desc.desc, savePayload_) || !replayFile_->Valid()) {
                    error_ = kReplayLoadFailed;
                    return 1;
                }
                savedEntry_ = added ? replayFile_->Count() - 1 : renameIndex_;
                log.push_back("replay stored as entry " + std::to_string(savedEntry_) + " '" + keyboard_->Name() + "' (" + std::to_string(savePayload_.size()) + " bytes)");
                return fileOnCard_ ? 0x0D : 0x0C; // 0x800696C4: the file on the card
            }
        } else {
            return -1;
        }
        listDelay_ = 0x0C;
        return 0x0A;
    }
    case 8: { // 0x8006F298 h(1)
        if (Status() != 0) {
            blocks_.anim = -1;
            return 3;
        }
        const int r = blockResult_;
        if (r == -1) {
            blocks_.anim = -1;
            sounds.push_back(2);
            return 2;
        }
        if (r < 0) return -1;
        blocks_.anim = -1; // 0x8006911C: the file of `r` blocks in memory (every sector free)
        replayFile_ = ReplayCardFile::Create(r, BuildReplayCardHeader(assets_.exe, r));
        fileOnCard_ = false;
        listDelay_ = 0x0C;
        return 0x0A;
    }
    case 0x0C: // 0x80070254 h(1): the create op's poll
        return --delay_ < 0 ? 0x0D : -1;
    case 0x0D: { // 0x8007031C h(1), mode 0: the data write's poll (the progress counts the directory's 0x150C bytes still to come)
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        ProgressUpdate(uint32_t(transferLeft_) + 0x150C);
        if (transferLeft_ > 0) return -1;
        return 0x0E;
    }
    case 0x1A: { // 0x80070C14 h(1)
        if (Status() != 0) {
            MenuListClose(replayList_);
            return 3;
        }
        const int32_t r = listResult_;
        if (r == -3) { sounds.push_back(6); return -1; }
        if (r == -4) { sounds.push_back(0); return -1; }
        if (r == -2 || r < -4) return -1;
        if (r == -1) {
            MenuListClose(replayList_);
            sounds.push_back(2);
            return 2;
        }
        sounds.push_back(1);
        renameIndex_ = replayRows_[size_t(r)].entry; // + 0x930
        MenuListClose(replayList_);
        return 0x1B;
    }
    case 0x1B: { // 0x80070E38 h(1): the read completes -> 0x800692DC (CRC) -> 0x80069D58, the flag 0x801C94E0
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        ProgressUpdate(uint32_t(transferLeft_));
        if (transferLeft_ > 0) return -1;
        progressVisible_ = -1;
        headerBand_.anim = int16_t(~headerBand_.steps);
        if (!replayFile_->EntryCrcOk(renameIndex_)) {
            error_ = kReplayLoadFailed;
            return 1;
        }
        loadedData_ = replayFile_->EntryData(renameIndex_);
        loadedData_.resize(size_t(replayFile_->Entry(renameIndex_).size));
        loadedTitle_ = replayFile_->Entry(renameIndex_).Title();
        loaded_ = true;
        log.push_back("loaded ghost " + std::to_string(renameIndex_) + " '" + loadedTitle_ + "' (" + std::to_string(loadedData_.size()) + " bytes) of " +
                      slots_[size_t(slot_)].path);
        return 0x1C;
    }
    case 0x1C: { // 0x800717B8 h(1)
        const int r = choice(kBarOk);
        if (r == 0) { sounds.push_back(1); return 2; }
        if (r == 1 || r == -1) return kExit;
        return -1;
    }
    case 0x0E: { // 0x800704C4 h(1)
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        ProgressUpdate(uint32_t(transferLeft_));
        if (transferLeft_ > 0) return -1;
        progressVisible_ = -1;
        try {
            const std::string path = slots_[size_t(slot_)].path;
            std::vector<uint8_t> card = career::ReadFileBytes(path);
            std::vector<uint8_t> file = ReadReplayCardFile(card);
            if (mode_ == kSaveReplay) { // the entry's sectors (0xD) and the directory (0xE); a new file: its blocks (0xC)
                file = replayFile_->Bytes();
            } else {
                if (file.size() < kReplayDataStart || replayFile_->Bytes().size() < kReplayDataStart) throw std::runtime_error("no replay file on the card");
                std::copy(replayFile_->Bytes().begin(), replayFile_->Bytes().begin() + std::ptrdiff_t(kReplayDataStart), file.begin());
            }
            StoreReplayCardFile(card, file);
            career::WriteFileBytes(path, card);
            saved_ = true;
            log.push_back("replay file directory saved to " + path + " (" + std::to_string(replayFile_->Count()) + " replay(s))");
            return 0x10;
        } catch (const std::exception& e) {
            log.push_back(std::string("replay file save failed: ") + e.what());
            error_ = kSaveDataFailed;
            return 0x0F;
        }
    }
    case 0x0F: { // 0x800705D0 h(1)
        const int status = Status();
        if (status == 2) { sounds.push_back(0); CloseBar(bars_[kBarRetry]); return 4; }
        if (status == 3) { sounds.push_back(0); CloseBar(bars_[kBarRetry]); return 5; }
        if (status == 4) {
            sounds.push_back(0);
            CloseBar(bars_[kBarRetry]);
            error_ = kReadFailed;
            return 1;
        }
        if (status != 0) return -1;
        const int r = choice(kBarRetry);
        if (r == 0) { CloseBar(bars_[kBarRetry]); sounds.push_back(1); return 0x0D; }
        if (r == -1) { sounds.push_back(0); return -1; }
        if (r == 1) { CloseBar(bars_[kBarRetry]); sounds.push_back(2); return 2; }
        return -1;
    }
    case 0x13: { // 0x80070A6C step: the read completes -> 0x800692DC (CRC) -> 0x80069AC4, exit 0x28
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        ProgressUpdate(uint32_t(transferLeft_));
        if (transferLeft_ > 0) return -1;
        progressVisible_ = -1;
        headerBand_.anim = int16_t(~headerBand_.steps);
        const int index = replayRows_[size_t(replayChosen_)].entry;
        if (!replayFile_->EntryCrcOk(index)) {
            error_ = kReplayLoadFailed;
            return 1;
        }
        try {
            std::vector<uint8_t> data = replayFile_->EntryData(index);
            data.resize(size_t(replayFile_->Entry(index).size));
            loadedReplay_ = UnpackReplayPayload(data);
            loadedTitle_ = replayFile_->Entry(index).Title();
        } catch (const std::exception& e) {
            log.push_back(std::string("replay: ") + e.what());
            error_ = kReplayLoadFailed;
            return 1;
        }
        loaded_ = true;
        log.push_back("loaded replay " + std::to_string(index) + " '" + loadedTitle_ + "' of " + slots_[size_t(slot_)].path);
        return 0x28;
    }
    default: return -1;
    }
}

void CardManager::Switch(int id) { // 0x80072494 + the chaining of 0x80072A88
    for (int guard = 0; guard < 16 && id >= 0; guard++) {
        if (id == kExit || id == 0x28) { // hide the header, clear the lines, state -1
            headerBand_.anim = -1;
            line1_ = line2_ = 0;
            exited_ = true;
            state_ = id;
            return;
        }
        state_ = id;
        id = Enter(id);
    }
}

bool CardManager::Update(const MenuListPad* pad) {
    sounds.clear();
    if (exited_) return false;
    // 0x800728F0: + 0x20 = L1 + R1 held in modes 0 / 2 (the delete mode); the band, the bars, the lists, the keyboard
    deleteMode_ = (mode_ == kRenameReplay || mode_ == kSaveReplay) && pad &&
                  (pad->held & (menu_list_pad::kL1 | menu_list_pad::kR1)) == (menu_list_pad::kL1 | menu_list_pad::kR1);
    headerBand_.Tick();
    for (size_t k = 0; k < 4; k++) barResult_[k] = UpdateBar(bars_[k], pad, sounds);
    blockResult_ = UpdateBlocks(pad); // 0x80091FC4
    if (mode_ <= kLoadGhost) listResult_ = MenuListUpdate(replayList_, pad); // the list 0x80091FE4
    for (size_t k = 4; k < 7; k++) barResult_[k] = UpdateBar(bars_[k], pad, sounds);
    keyboardResult_ = keyboard_ ? keyboard_->Update(pad, sounds) : -2; // + 0x6D8 (0x80073720)
    for (size_t k = 7; k < bars_.size(); k++) barResult_[k] = UpdateBar(bars_[k], pad, sounds);
    Switch(Step());
    return !exited_;
}

void CardManager::DrawBar(const TitleAssets& assets_, MenuOtSlot& gradients, MenuOtSlot& fills, MenuOtSlot& base, const Bar& b) { // 0x8006E5B8
    if (b.anim == -1) return;
    const HudFont& small = assets_.fonts[TitleAssets::kSmallFont];
    const int W = b.w, H = b.h;
    const int textDy = (b.flags & 4) ? -4 : -2;
    auto scaled = [](uint32_t c, uint32_t a) { // the bar's text objects: colour * alpha >> 8 (title alpha 0x80, labels the bar level)
        return (c & 0xFF000000u) | ((c & 0xFF) * a >> 8) | ((((c >> 8) & 0xFF) * a >> 8) << 8) | ((((c >> 16) & 0xFF) * a >> 8) << 16);
    };
    if (b.anim < -1) { // closing: the chosen button's fill shrinks
        int cx = b.x + (W >> 1);
        if (b.cursor == 0) cx -= W;
        int s = W * (b.anim + 0x11);
        if (s < 0) s += 0xF;
        const int width = W - (s >> 4);
        const uint32_t c = MenuListLerp(b.fill, 0, b.anim + 0x11, 0x10);
        AddGradientQuad(base, cx - (width >> 1), b.y, width, H, c, c);
        base.DrawMode(0x20);
        AddTextObject(base, small, assets_.Text(b.title), b.x, b.y + textDy, 2, scaled(b.titleColour, 0x80), 1);
        return;
    }
    uint32_t level = 0x7F;
    int offset = 0;
    if (b.anim < 12) {
        level = uint32_t(b.anim * 0x7F) / 12;
        offset = (W * 2 * (12 - b.anim)) / 12;
    }
    if (b.flags & 1) offset = -offset;
    const int cx = b.x + offset, left = cx - W, right = cx;
    MenuListFrame(base, Grey(level), left, b.y, W, H);
    MenuListFrame(base, Grey(level), right, b.y, W, H);
    AddGradientQuad(gradients, left, b.y, W, H, b.gradient, 0);
    AddGradientQuad(gradients, right, b.y, W, H, b.gradient, 0);
    base.DrawMode(0x200);
    AddTextObject(base, small, assets_.Text(b.title), b.x, b.y + textDy, 2, scaled(b.titleColour, 0x80), 1);
    AddTextObject(base, small, assets_.Text(b.label0), left + (W >> 1), b.y + H + textDy, b.labelExtra, scaled(b.labelColour, level), 1);
    AddTextObject(base, small, assets_.Text(b.label1), cx + (W >> 1), b.y + H + textDy, b.labelExtra, scaled(b.labelColour, level), 1);
    int t = 0;
    if (b.anim >= 12) t = std::max(0, 0x3C - (b.anim - 12) * 4);
    const uint32_t white = (b.flags & 0x80) ? 0xFFu : 0x80u;
    uint32_t fill = MenuListLerp(b.fill, Grey(white), t, 0x3C);
    fill = MenuListLerp(fill, 0, std::max(0, 12 - int(b.anim)), 12);
    fills.Add(Tile(b.cursor ? right : left, b.y, W, H, fill));
    if (b.slide > 0) {
        const int sw = (W * b.slide) / 6;
        if (b.cursor) AddGradientQuad(fills, right - sw, b.y, sw, H, 0, fill);
        else AddGradientQuad(fills, right, b.y, sw, H, fill, 0);
    }
    fills.DrawMode(0x220);
}

void CardManager::DrawSlotBand(const TitleAssets& assets, MenuOtSlot& base, const Band& band, int slot) {
    const int a = band.anim >= 0 ? band.anim : band.anim < -1 ? ~band.anim : 0; // 0x8006BEB4
    const int alpha = band.steps ? (a << 7) / band.steps : 0;
    if (alpha == 0) return;
    AddText(base, assets.fonts[TitleAssets::kMediumFont], assets.Text(slot == 0 ? kMemoryCard1 : kMemoryCard2), 0x1C, 0x7E, 1,
            MenuListLerp(0x02000000u, 0x0242362Au, alpha, 0x80), 1);
    band.Draw(base, 0x18, 0x6E);
    base.DrawMode(0x220);
}

void CardManager::DrawProgress(MenuOtSlot& base, const std::array<int8_t, 32>& segments, uint32_t colour, int y) { // 0x8006C174
    for (int i = 0; i < 32; i++) {
        const int k = segments[size_t(i)];
        const uint32_t c = k < 0 ? MenuListLerp(colour, 0, 0x60, 0x80) : MenuListLerp(colour, 0xD4D4D4u, 0x18 - k, 0x18);
        base.Add(Tile(0x60 + 5 * i, y, 4, 0x18, c));
    }
}

void CardManager::DrawSectorBar(const TitleAssets& assets, MenuOtSlot& base, int total, int used, int x, int y, int fade, int less, int more) { // 0x8006AA68
    int t = 0x80 - fade;
    if (t < 0) t = 0;
    const GuestImage& exe = assets.exe;
    auto colour = [&](uint32_t a) { return MenuListLerp(exe.Get<uint32_t>(a), exe.Get<uint32_t>(kFadeTarget), t, 0x80); };
    if (total == 0) total = 1;
    const int w1 = ((used - less) * 0x100) / total;
    AddGradientQuad(base, x - 0x80, y - 4, w1, 8, colour(kSectorUsed), colour(kSectorEnd));
    const int w2 = (((used - less) + more) * 0x100) / total - w1;
    AddGradientQuad(base, x - 0x80 + w1, y - 4, w2, 8, colour(kSectorSelected), colour(kSectorUsed));
    base.DrawMode(0x220);
    base.Add(Tile(x - 0x80, y - 4, 0x100, 8, 0));
    base.Add(Tile(x - 0x81, y - 6, 0x102, 0x0C, colour(kSectorFrame)));
}

void CardManager::DrawErrorText(const TitleAssets& assets, MenuOtSlot& base, uint32_t text) { // state 1 h(2), 0x800718E0 (US branch)
    AddText(base, assets.fonts[TitleAssets::kMediumFont], assets.Text(text), 0xB0, 0xFA, 1, 0x6F6F6Fu, 1, TextAlign::kCentre);
}

void CardManager::BuildReplayRows() { // 0x8006F6E8 h(0), mode 2: the entries (kind 0, enabled) and "- OK -" (kind 2)
    replayRows_.clear();
    if (mode_ == kSaveReplay) { // mode 0: the entries (enabled when the payload fits in place, 0x80069358) and "- New File -" (kind 1)
        rowFit_.clear();
        const int32_t size = int32_t(savePayload_.size());
        for (int i = 0; i < replayFile_->Count(); i++) {
            const int fit = replayFile_->Fits(i, size);
            replayRows_.push_back({int8_t(i), int8_t(kReplayRowEntry), fit == 0, true});
            rowFit_.push_back(int8_t(fit));
        }
        const int fit = replayFile_->Fits(-1, size);
        replayRows_.push_back({int8_t(-1), int8_t(kReplayRowNewFile), fit == 0, true});
        rowFit_.push_back(int8_t(fit));
        replayList_.count = int16_t(replayRows_.size());
        replayList_.selection = int16_t(replayFile_->Count()); // 0x80091FEA = the count: "- New File -"
        return;
    }
    for (int i = 0; i < replayFile_->Count(); i++) replayRows_.push_back({int8_t(i), int8_t(kReplayRowEntry), true, true});
    replayRows_.push_back({int8_t(-1), int8_t(kReplayRowOk), true, true});
    replayList_.count = int16_t(replayRows_.size());      // 0x80091FE4 = count + 1
    replayList_.selection = keptSelection_;               // 0x80091FEA = + 0x932
}

void CardManager::DrawReplayHints(MenuOtSlot& base) const {
    if (mode_ != kRenameReplay && mode_ != kSaveReplay) return;
    const HudFont& small = assets_.fonts[TitleAssets::kSmallFont]; // 0x801C94B4
    const HudFont& tiny = assets_.fonts[TitleAssets::kTinyFont];   // 0x801C94D4
    auto rightText = [&](const HudFont& font, const std::string& text, int y, int spacing, uint32_t colour) { // 0x8006AD3C + 0x8006AC90 at 0x148 - width
        AddText(base, font, text, 0x148 - font.TextWidth(text, spacing), y, spacing, colour, 1);
    };
    if (state_ == 0x0B) { // 0x8006FF8C h(2)
        rightText(small, assets_.Text(kEditReplayFile), 0x7C, 0, 0x020C3060u);
        return;
    }
    if (state_ != 0x17 && state_ != 0x0A) return;
    // 0x8006F6E8 h(2), mode 2 (the US build: 0x8006EE08's action line is the Japanese path only)
    rightText(small, assets_.Text(kDeleteModeHint), 0x7C, 0, 0x020C3060u);
    const int selection = replayList_.selection;
    int entrySectors = 0;
    if (selection >= 0 && selection < int(replayRows_.size()) && replayRows_[size_t(selection)].kind == kReplayRowEntry)
        entrySectors = replayFile_->Entry(replayRows_[size_t(selection)].entry).sectors; // + 0x1312 = entry + 0x52
    // 0x8006AA68(+ 0x934, ot, 0xB0, 0x1C2, list fade, the entry's sectors, the entry's sectors (mode 0: the payload's sectors))
    const int needed = int((savePayload_.size() + 0x7F) >> 7);
    const int fade = replayList_.fadeMax ? (int(replayList_.fade) << 7) / int(replayList_.fadeMax) : 0;
    DrawSectorBar(assets_, base, sectorsTotal_, sectorsUsed_, 0xB0, 0x1C2, fade, entrySectors, mode_ == kSaveReplay ? needed : entrySectors);
    rightText(tiny, FormatInt(assets_.Text(kTotalReplays), replayFile_->Count()), 0x1A4, 1, 0x024A4136u);
    const std::string free = FormatInt(assets_.Text(kSectorsFree), replayFile_->FreeSectors(), replayFile_->Total());
    rightText(tiny, free, 0x1B4, 1, 0x024A4136u);
    if (mode_ == kSaveReplay) { // "need %d sector" left of it: x = 0x140 - its width - the free text's width
        const std::string need = FormatInt(assets_.Text(kNeedSectors), needed);
        AddText(base, tiny, need, 0x140 - tiny.TextWidth(free, 1) - tiny.TextWidth(need, 1), 0x1B4, 1, 0x024A4136u, 1);
    }
}

CardManager::BlockSelector CardManager::ReadBlockSelector(const GuestImage& image, uint32_t k, int fontIndex) { // + 0x8006D9C8 (hidden)
    BlockSelector b;
    b.x = image.Get<int16_t>(k), b.y = image.Get<int16_t>(k + 2);
    b.value = image.Get<int16_t>(k + 6); // 0x8006D9C8: value = min
    b.min = image.Get<int16_t>(k + 6), b.max = image.Get<int16_t>(k + 8);
    b.colour = image.Get<uint32_t>(k + 0xC);
    b.fontIndex = fontIndex;
    b.height = image.Get<int16_t>(k + 0x14), b.spacing = image.Get<int16_t>(k + 0x16);
    b.digitShift = image.Get<int16_t>(k + 0x18), b.sound = image.Get<int16_t>(k + 0x1A);
    b.anim = -1;
    return b;
}

int CardManager::UpdateBlockSelector(BlockSelector& b, const MenuListPad* pad, std::vector<int>& sounds) { // 0x8006D9DC
    if (b.anim < 0) return -2;
    if (++b.anim > 0x2D) b.anim = 0;
    if (!pad) return -2;
    if (pad->pressed & menu_list_pad::kBack) return -1;
    if (pad->pressed & menu_list_pad::kChoose) return b.value;
    const uint32_t bits = pad->pressed | pad->repeat;
    int v = b.value;
    if (bits & menu_list_pad::kLeft) v--;
    if (bits & menu_list_pad::kRight) v++;
    v = std::max<int>(v, b.min);
    v = std::min<int>(v, b.max);
    if (v != b.value && b.sound >= 0) sounds.push_back(b.sound);
    b.value = int16_t(v);
    return -2;
}

int CardManager::UpdateBlocks(const MenuListPad* pad) { return UpdateBlockSelector(blocks_, pad, sounds); }

void CardManager::DrawBlockSelector(const TitleAssets& assets, MenuOtSlot& base, const BlockSelector& b) { // 0x8006DAF8
    if (b.anim < 0) return;
    const HudFont& font = assets.fonts[size_t(b.fontIndex)];
    const std::string text = std::to_string(b.value); // "%d" 0x8008FAAC
    const int y = b.y + (int(int16_t(b.height)) >> 1);
    const int x = b.x - (font.NumberWidth(text, b.spacing, 0) >> 1); // 0x8006B0EC
    AddNumberText(base, font, text, x, y, b.spacing, b.digitShift, 0, b.colour, 1);
    int t = 0x28 - b.anim;
    t = std::clamp(t, 0, 10);
    const uint32_t level = uint32_t(t * 0xFF) / 10;
    const uint32_t colour = level | (level >> 1) << 8 | 0x2000000u;
    auto arrow = [&](int edge, int tip) { // 0x8007E0E0: POLY_F3 (edge, y - 10), (tip, y), (edge, y + 10)
        MenuPrim p;
        p.kind = MenuPrim::kPolyF4;
        p.semi = true;
        for (uint32_t& c : p.colour) c = colour & 0xFFFFFF;
        p.x[0] = int16_t(edge), p.y[0] = int16_t(b.y - 10);
        p.x[1] = int16_t(tip), p.y[1] = b.y;
        p.x[2] = int16_t(edge), p.y[2] = int16_t(b.y + 10);
        p.x[3] = p.x[2], p.y[3] = p.y[2]; // a triangle: the quad's second half is empty
        base.Add(p);
    };
    const int cell = font.cell * 2;
    if (b.value != b.max) arrow(b.x + cell, b.x + cell + 6);
    if (b.value != b.min) arrow(b.x - cell, b.x - cell - 6);
}

void CardManager::DrawBlocks(MenuOtSlot& base) const { DrawBlockSelector(assets_, base, blocks_); } // the context of 0x80072B78

void CardManager::Draw(MenuOtSlot& base, MenuOtSlot& fills, MenuOtSlot& gradients) const {
    const HudFont& medium = assets_.fonts[TitleAssets::kMediumFont];
    // 0x80072B78: the two lines (centred at x 0xB0, baselines 0xD6 / 0xFA, spacing 1, colour 0x025C5248)
    AddText(base, medium, assets_.Text(line1_), 0xB0, 0xD6, 1, 0x025C5248u, 1, TextAlign::kCentre);
    AddText(base, medium, assets_.Text(line2_), 0xB0, 0xFA, 1, 0x025C5248u, 1, TextAlign::kCentre);
    DrawSlotBand(assets_, base, headerBand_, slot_);
    for (size_t k = 0; k < bars_.size(); k++) {
        DrawBar(assets_, gradients, fills, base, bars_[k]);
        if (k == 3) { // 0x80072B78: after the bars at +0x50..+0x218 the block selector 0x80091FC4, E1 0x20, the list 0x80091FE4
            DrawBlocks(base);
            base.DrawMode(0x20);
            if (mode_ <= kLoadGhost) {
                rowSlot_ = &fills; // 0x8006F060 draws into the list's OT slot + 1
                MenuListDraw(replayList_, base);
                rowSlot_ = nullptr;
            }
        }
    }
    if (keyboard_) keyboard_->Draw(base); // + 0x6D8 (0x80073AFC)
    // h(2) of the state: the error text of state 1, the progress bar of the transfers, the hints of the replay list / name entry
    if (state_ == 1 && error_) DrawErrorText(assets_, base, error_);
    if (progressVisible_ >= 0) DrawProgress(base, progress_, progressColour_);
    DrawReplayHints(base);
}

std::vector<MenuPrim> CardManager::Frame() const {
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot gradients, fills, base, header;
    Draw(base, fills, gradients);
    if (mode_ == kLoadReplay) AddViewHeader(header, assets_, assets_.Text(0x801B99C7u), 0xD63B54u, 0x80); // view 0x8004B3C8 "LOAD REPLAY"
    else if (mode_ == kRenameReplay) AddViewHeader(header, assets_, assets_.Text(0x801B99F0u), 0x288DC0u, 0x80); // view 0x8004B41C "RENAME & DELETE"
    else if (mode_ == kSaveGame || mode_ == kLoadGame)
        AddViewHeader(header, assets_, assets_.Text(mode_ == kSaveGame ? 0x801BA44Du : 0x801BA45Du), mode_ == kSaveGame ? 0xF2u : 0xF20000u, 0x80);
    // (modes 0 / 3 run in the race overlay, whose view manager draws the header: Draw())
    gradients.Emit(prims, 0x200);
    fills.Emit(prims, 0x200);
    base.Emit(prims, 0x200);
    header.Emit(prims, 0x200);
    return prims;
}

std::vector<MenuPrim> NoticeFrame(const TitleAssets& assets, const std::string& title, uint32_t colour, const std::string& line) {
    std::vector<MenuPrim> prims = TitleFrameStart();
    MenuOtSlot content, header;
    AddViewHeader(header, assets, title, colour, 0x80);
    AddText(content, assets.fonts[TitleAssets::kMediumFont], line, 0xB0, 0xD6, 1, 0x025C5248u, 1, TextAlign::kCentre);
    header.Emit(prims, 0x200);
    content.Emit(prims, 0x200);
    return prims;
}

} // namespace gt2::shell
