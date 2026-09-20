#include "game/shell/title_menu.h"

namespace gt2::shell {

TitleSprite TitleSprite::Read(const GuestImage& image, uint32_t address) {
    TitleSprite s;
    s.u = image.Get<uint8_t>(address);
    s.v = image.Get<uint8_t>(address + 1);
    s.clut = image.Get<uint16_t>(address + 2);
    s.w = image.Get<uint16_t>(address + 4);
    s.h = image.Get<uint16_t>(address + 6);
    s.tpage = image.Get<uint16_t>(address + 8);
    return s;
}

TitleMenu::TitleMenu(const GuestImage& ovl1, uint8_t language) {
    // The tables by their Simulation v1.2 addresses through the image's build profile (identity on the Simulation disc; on
    // US Arcade v1.1 member 1 they sit 0xB2C lower, exe_profiles.inc); the pointers stored in them are the image's own.
    for (int r = 0; r < kRows; r++) {
        results_[size_t(r)] = ovl1.Get<int16_t>(ovl1.Sim(kResults) + uint32_t(r) * 2);
        spriteIndex_[size_t(r)] = ovl1.Get<int16_t>(ovl1.Sim(kSpriteIndex) + uint32_t(r) * 2);
    }
    templateFlags_ = ovl1.Get<uint8_t>(ovl1.Sim(kRowTemplate));
    templateBrightness_ = ovl1.Get<uint8_t>(ovl1.Sim(kRowTemplate) + 1);
    templateReveal_ = ovl1.Get<int16_t>(ovl1.Sim(kRowTemplate) + 2);
    const uint32_t table = ovl1.Get<uint32_t>(ovl1.Sim(kSpriteTables) + uint32_t(language) * 4);
    for (int r = 0; r < kRows; r++) sprites_[size_t(r)] = TitleSprite::Read(ovl1, table + uint32_t(spriteIndex_[size_t(r)]) * 12);
    widget_ = MenuListWidget::Read(ovl1, ovl1.Sim(kWidget));
    Reset();
}

void TitleMenu::Reset() {
    fade_ = 0;
    leave_ = 0x10;
    state_ = 0;
    openDelay_ = 0x10;
    MenuListReset(widget_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) { return RowCallback(command, w, row, draw); });
    idle_ = 0;
    result = kTitleNone;
    startHeld = false;
    sounds.clear();
}

// 0x8001779C (rows with a negative result do nothing and are disabled).
int32_t TitleMenu::RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) {
    if (row < 0 || row >= kRows || results_[size_t(row)] < 0) return 0;
    TitleRowState& r = rows_[size_t(row)];
    switch (command) {
    case kMenuListReset: // 0x80016394 from the template 0x8004BC24, then the sprite of the row
        r.flags = templateFlags_;
        r.revealWidth = templateReveal_;
        r.anim = -1;
        r.alpha = 0x80;
        r.brightness = templateBrightness_;
        r.sprite = sprites_[size_t(row)];
        return 0;
    case kMenuListReveal: r.anim = 0; return 0;
    case kMenuListTick: // 0x800163C8
        if (r.anim < 0) {
            if (r.anim < -1) r.anim++;
        } else if (++r.anim > 11) {
            r.anim = 12;
        }
        return 0;
    case kMenuListDraw: {
        if (!draw || !draw->ot) return 0;
        const int32_t scaled = int32_t(draw->alpha) * ((int32_t(w.fade) << 7) / int32_t(w.fadeMax));
        const bool selected = w.selection == row;
        r.alpha = int16_t(selected ? scaled >> 7 : scaled >> 9);
        r.x = draw->x;
        r.y = draw->y;
        DrawTitleRow(r, *draw->ot, selected);
        return 0;
    }
    case kMenuListEnter: r.anim = 12; return 0;
    case kMenuListEnabled: return 1;
    default: return 0;
    }
}

int TitleMenu::Update(const MenuListPad* pad, uint32_t held) {
    sounds.clear();
    int ret = 0;
    bool fadeIn = false;
    if (state_ == 0) {
        const bool open = openDelay_ == 1;
        openDelay_--;
        if (open) {
            state_ = 1;
            MenuListOpen(widget_);
            widget_.revealPeriod = -1; // 0x8004BC44 = 0xFFFF: every row revealed in the first update
        }
        fadeIn = true;
    } else if (state_ == 1) {
        fadeIn = true;
    } else if (state_ == 2) {
        if (--fade_ < 0) fade_ = 0;
        if (--leave_ < 0) ret = 4;
    }
    if (fadeIn && ++fade_ > 12) fade_ = 12;
    if (held == 0) {
        if (++idle_ > kIdleFields) {
            MenuListClose(widget_);
            state_ = 2;
            result = kAttractDemo;
        }
    } else {
        idle_ = 0;
    }
    const int16_t before = widget_.selection;
    const int32_t r = MenuListUpdate(widget_, pad);
    MenuListClamp(widget_, 1, 6);
    if (r == -3) {
        if (before != widget_.selection) sounds.push_back(6);
        return ret;
    }
    if (r == -4 || r == -2 || r == -1) return ret;
    sounds.push_back(3);
    MenuListClose(widget_);
    state_ = 2;
    startHeld = pad && (pad->held & menu_list_pad::kStart) != 0;
    result = r >= 0 && r < kRows ? results_[size_t(r)] : kTitleNone;
    return ret;
}

void TitleMenu::Draw(MenuOtSlot& ot) const {
    MenuListDraw(widget_, ot);
    const uint32_t level = uint32_t((int32_t(fade_) << 7) / 12);
    const uint32_t colour = level | level << 8 | level << 16;
    struct Part { int16_t x, y, w, h; uint16_t tpage; };
    static constexpr Part kParts[4] = {{0, 0, 256, 256, 0x86}, {0, 256, 256, 224, 0x96}, {256, 0, 96, 256, 0x88}, {256, 256, 96, 224, 0x98}};
    for (const Part& p : kParts) { // 0x80080450: a DR_TPAGE + SPRT packet each (uv 0, CLUT 0x7FD8 = (384, 511))
        MenuPrim s;
        s.kind = MenuPrim::kSprite;
        s.x[0] = p.x, s.y[0] = p.y, s.w = p.w, s.h = p.h;
        s.u = 0, s.v = 0;
        s.tpage = p.tpage;
        s.clut = 0x7FD8;
        s.colour[0] = colour;
        ot.Add(s);
        ot.DrawMode(p.tpage);
    }
    ot.DrawMode(0x280);
}

void DrawTitleRow(const TitleRowState& row, MenuOtSlot& ot, bool selected) {
    if (row.anim == -1) return;
    const TitleSprite& s = row.sprite;
    const int half = int(int16_t(s.w)) >> 1, halfH = int(int16_t(s.h)) >> 1;
    int x = row.x;
    if ((row.flags & 0x60) == 0x20) x += half;
    else if ((row.flags & 0x60) == 0x40) x -= half;
    // anim < -1 (a collapse drawn with 0x8006B61C) is never set by the title's callback (commands 0 / 1 / 3 / 6 only).
    if (row.anim < 0) return;
    const int y = row.y;
    auto sprite = [&](int left, uint32_t rgb, bool semi, uint16_t mode) {
        MenuPrim p;
        p.kind = MenuPrim::kSprite;
        p.x[0] = int16_t(left), p.y[0] = int16_t(y - halfH);
        p.w = int16_t(s.w), p.h = int16_t(s.h);
        p.u = s.u, p.v = s.v, p.clut = s.clut;
        p.tpage = mode; // the E1 the GPU meets right before this sprite (the one generated after it)
        p.colour[0] = rgb & 0xFFFFFF;
        p.semi = semi;
        ot.Add(p);
    };
    const uint32_t brightness = row.brightness;
    uint32_t level = brightness, ghost = 0;
    int spread = 1;
    if (row.anim < 12) {
        level = brightness * uint32_t(row.anim) / 12;
        ghost = brightness - level;
        spread = (int(row.revealWidth) * (12 - row.anim)) / 12;
    }
    const int32_t scaled = int32_t(level) * int32_t(row.alpha);
    const uint16_t mode = uint16_t((row.flags & 0x18) << 2);
    uint32_t c = uint32_t(scaled >> 7);
    auto grey = [](uint32_t v) { return v | v << 8 | v << 16; };
    if ((row.flags & 4) && row.anim < 12) {
        const uint16_t e1 = uint16_t(s.tpage | mode);
        sprite(x - spread - half, grey(ghost), true, e1);
        sprite(x + spread - half, grey(ghost), true, e1);
        ot.DrawMode(e1);
    }
    if ((row.flags & 2) == 0) {
        sprite(x - half, grey(c), false, uint16_t(s.tpage | mode));
    } else {
        sprite(x - half, grey(c), true, uint16_t(s.tpage | mode));
        ot.DrawMode(uint16_t(s.tpage | mode));
        if (row.flags & 1) {
            if (!selected) {
                c = uint32_t(scaled >> 8);
            } else {
                sprite(x - half, 0, true, s.tpage);
                ot.DrawMode(s.tpage);
            }
            sprite(x - half, grey(c), true, uint16_t(s.tpage | 0x40));
            ot.DrawMode(uint16_t(s.tpage | 0x40));
        }
    }
    ot.DrawMode(uint16_t(s.tpage | mode));
}

} // namespace gt2::shell
