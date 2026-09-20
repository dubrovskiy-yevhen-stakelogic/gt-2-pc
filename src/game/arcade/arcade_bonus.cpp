#include "game/arcade/arcade_bonus.h"

#include <algorithm>
#include <cstdlib>

#include "game/arcade/arcade_setup.h"
#include "game/shell/title_screens.h"

namespace gt2::arcade {
namespace {

namespace mp = menu_list_pad;

// ---- member 2 (ARCADE v1.1)
constexpr uint32_t kBonusTable = 0x80051790u;     // 16-byte rows, ended by a null name
constexpr uint32_t kBonusList = 0x80052524u;      // the list widget object
constexpr uint32_t kBonusDialog = 0x80052558u;    // the "Next" dialog definition
constexpr uint32_t kBonusMessage = 0x800F8490u;   // the message under the legend
constexpr uint32_t kLegend = 0x80052484u;         // two 12-byte sprites: the level marks, the course kinds
constexpr uint32_t kMarks = 0x800524A8u;          // three 12-byte sprites: won on Easy / Normal / Difficult
constexpr uint32_t kRoadMark = 0x800524CCu;       // the sprite of a road course's second mark
constexpr uint32_t kCarMark = 0x800524D8u;        // the car sprite at the row's right end
constexpr uint32_t kBanners = 0x80052454u;        // four 12-byte row banners
constexpr uint32_t kBlack = 0x800524E4u, kZero = 0x800524E8u, kNameColour = 0x800524ECu, kBanner = 0x800524F4u, kDirtBanner = 0x800524F8u;
constexpr uint32_t kMessageColour = 0x800524FCu, kCarColour = 0x80052500u, kCarWonColour = 0x80052504u;
constexpr uint32_t kTierColours = 0x80052508u;    // u32 [6]: the row box colour per tier; [6] (0x80052520) with no tier
constexpr uint32_t kSmallFont = 0x801234D0u;
constexpr uint32_t kCreditsList = 0x800525A8u, kCreditsRows = 0x80052580u, kCreditsLogo = 0x80052430u;
constexpr size_t kCareerFlags = 0xB8, kSimulationDone = 0x215;
// ---- EXE (US Arcade v1.1; Simulation 0x80091EA8 / 0x80091EC4)
constexpr uint32_t kDialogText = 0x80091BA0u, kDialogZero = 0x80091BBCu;

uint32_t Grey(int v) { return uint32_t(v) | uint32_t(v) << 8 | uint32_t(v) << 16; }

// A 12-byte sprite definition {u, v, clut, w, h, tpage} centred at (cx, cy) (0x80081388 + 0x8007D954).
void SpriteAt(const GuestImage& o, MenuOtSlot& ot, uint32_t sprite, int cx, int cy, uint32_t colour) {
    const int w = o.Get<int16_t>(sprite + 4), h = o.Get<int16_t>(sprite + 6);
    AddSprite(ot, cx - (w >> 1), cy - (h >> 1), o.Get<uint8_t>(sprite), o.Get<uint8_t>(sprite + 1), o.Get<uint16_t>(sprite + 2), w, h,
              o.Get<uint16_t>(sprite + 8), colour);
}

std::string CString(const GuestImage& o, uint32_t address) {
    std::string s;
    for (uint32_t a = address; o.Contains(a, 1) && o.Get<uint8_t>(a) != 0; a++) s.push_back(char(o.Get<uint8_t>(a)));
    return s;
}

} // namespace

// ---------------------------------------------------------------- dialog

void ArcDialog::Init(const ArcadeMenuAssets& a, uint32_t t) { // EXE 0x8006DBC8
    const GuestImage& o = a.data.ovl2;
    x = o.Get<int16_t>(t), y = o.Get<int16_t>(t + 2);
    flags = o.Get<uint8_t>(t + 0x14);
    const int size = flags & 6;
    w = int16_t(size == 6 ? 0x80 : (size == 2 || size == 4) ? 0x60 : 0x50);
    h = int16_t((size == 4 || size == 6) ? 0x18 : 0x0C);
    // The template gets the font (+0x18), the height (+0x16) and c0 (+8) first.
    text = screens::TextObject::FromTemplate(a.exe, kDialogText, a.data.Text(o.Get<uint32_t>(t + 4)));
    text.font = o.Get<uint32_t>(t + 0x18);
    text.height = o.Get<uint8_t>(t + 0x16);
    text.c0 = o.Get<uint32_t>(t + 8);
    text.extra = o.Get<int8_t>(t + 0x15);
    fill = o.Get<uint32_t>(t + 0xC);
    gradient = o.Get<uint32_t>(t + 0x10);
    zero = a.exe.Get<uint32_t>(kDialogZero);
    anim = -1;
}

void ArcDialog::Open() { // EXE 0x8006DCE0
    text.Open(-1);
    anim = 0;
    flags |= 0x80;
}

void ArcDialog::Close() { // EXE 0x8006DD2C
    anim = -13;
    text.Close();
}

int ArcDialog::Update(const MenuListPad* pad) { // EXE 0x8006DD54
    if (anim < 0) {
        if (anim < -1) anim++;
        return -2;
    }
    if (++anim >= 72) {
        anim = 12;
        flags &= 0xFF7F;
    }
    int r = -2;
    if (pad) {
        if (pad->pressed & mp::kChoose) r = 0;
        if (pad->pressed & mp::kBack) r = -1;
    }
    text.Tick();
    return r;
}

void ArcDialog::Draw(const ArcadeMenuAssets& a, ViewOt& view) const { // EXE 0x8006DDFC
    screens::MenuOt ot;
    const int left = x - (w >> 1);
    const int textDy = (flags & 4) ? -4 : -2;
    if (anim < 0) {
        if (anim < -1) {
            const int k = anim + 13;
            const int wd = w - (w * k) / 12;
            const uint32_t c = LerpColour(fill, zero, k, 12);
            shell::AddGradientQuad(ot[0], x - (wd >> 1), y, wd, h, c, c);
            ot[0].DrawMode(0x20);
        }
    } else {
        const int g = anim < 12 ? (anim * 127) / 12 : 127;
        MenuListFrame(ot[0], Grey(g), left, y, w, h);
        shell::AddGradientQuad(ot[2], left, y, w, h, gradient, zero);
        ot[0].DrawMode(0x200);
        screens::TextObject t = text;
        t.alpha = uint8_t(g);
        t.Draw(ot, 0, left + (w >> 1), y + h + textDy, a.FontAt(t.font));
        int k = 0;
        if (anim >= 12) k = std::max(0, 60 - (anim - 12) * 4);
        const uint32_t white = (flags & 0x80) ? 0xFFu : 0x80u;
        uint32_t c = LerpColour(fill, Grey(int(white)), k, 60);
        c = LerpColour(c, zero, std::max(0, 12 - int(anim)), 12);
        ot[1].Add(Tile(left, y, w, h, c));
        ot[1].DrawMode(0x220);
    }
    for (int s = 0; s < 3; s++) view.slot[size_t(s)].Append(ot[s], 1); // screens::MenuOt's slots start with its marker packet
}

bool AllBonusCoursesWon(const ArcadeMenuAssets& a, std::span<const uint8_t> career) { // 0x800235F4
    const GuestImage& o = a.data.ovl2;
    for (uint32_t row = kBonusTable; o.Get<uint32_t>(row) != 0; row += 0x10) {
        const int32_t flag = o.Get<int32_t>(row + 0xC);
        if (flag < 0) continue;
        const uint8_t f = career[kCareerFlags + size_t(flag)];
        if (((f >> 2) & 1) == 0 && ((f >> 1) & 1) == 0) return false;
    }
    return true;
}

// ---------------------------------------------------------------- BONUS ITEMS

ArcadeBonusPage::ArcadeBonusPage(const ArcadeMenuAssets& a) : a_(a) {}

void ArcadeBonusPage::Enter(std::span<const uint8_t> career) { // 0x80023CA8
    const GuestImage& o = a_.data.ovl2;
    career_.assign(career.begin(), career.end());
    timer_ = 24;
    dialog_.Init(a_, kBonusDialog);
    rows_.clear();
    for (uint32_t row = kBonusTable; o.Get<uint32_t>(row) != 0; row += 0x10) {
        Row r;
        r.tier = o.Get<int32_t>(row + 8);
        if (!TierOpen(career_, r.tier)) break; // the rows are listed up to the first closed tier
        r.name = CString(o, o.Get<uint32_t>(row));
        r.road = o.Get<int8_t>(row + 4);
        r.dirt = o.Get<int8_t>(row + 5);
        r.carMark = o.Get<int8_t>(row + 6);
        r.flag = o.Get<int32_t>(row + 0xC);
        rows_.push_back(r);
    }
    list_ = MenuListWidget::Read(o, kBonusList);
    list_.count = int16_t(rows_.size());
    MenuListReset(list_, [this](int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) { return RowCallback(command, w, row, d); });
}

ArcadeBonusPage::Result ArcadeBonusPage::Update(const MenuListPad* pad, std::vector<int>& sounds) { // 0x80023D58
    if (timer_ > 0 && --timer_ == 0) {
        MenuListOpen(list_);
        dialog_.Open();
    }
    const int32_t r = MenuListUpdate(list_, pad);
    MenuListClamp(list_, 5, int(rows_.size()) - 5);
    if (r == -3) sounds.push_back(6);
    const int d = dialog_.Update(pad);
    if (d == -1) {
        sounds.push_back(4);
        MenuListClose(list_);
        dialog_.Close();
        return kBack;
    }
    if (d != 0) return kStay;
    sounds.push_back(3);
    MenuListClose(list_);
    dialog_.Close();
    return kNext;
}

int32_t ArcadeBonusPage::RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* d) const { // 0x80023BC8
    if (command == kMenuListEnabled) return 1;
    if (command != kMenuListDraw || !d) return 0;
    const int alpha = (((int(w.fade) << 7) / w.fadeMax) * d->alpha) >> 7;
    if (alpha <= 0 || w.state == -1) return 0;
    DrawRow(rows_.at(size_t(row)), d->ot + 1, d->x, d->y, alpha);
    return 0;
}

void ArcadeBonusPage::DrawRow(const Row& r, MenuOtSlot* ot, int x, int y, int alpha) const { // 0x80023674
    const GuestImage& o = a_.data.ovl2;
    TextCtx& c = *rowCtx_;
    c.font = &a_.FontAt(kSmallFont);
    c.mode = 1;
    c.ot = ot;
    c.colour = LerpColour(o.Get<uint32_t>(kBlack), o.Get<uint32_t>(kNameColour), alpha, 128);
    DrawText(c, r.name, x - 72, y + 8, 1);
    // 0x8006B68C: the tier box {x - 154, y - 12, 26, 24}, POLY_G4 from a quarter of the tier colour to the full one.
    const uint32_t box = o.Get<uint32_t>(r.tier >= 0 ? kTierColours + uint32_t(r.tier) * 4 : kTierColours + 0x18);
    const uint32_t c0 = LerpColour(o.Get<uint32_t>(kBlack), box, alpha, 512), c1 = LerpColour(o.Get<uint32_t>(kBlack), box, alpha, 128);
    MenuPrim p;
    p.kind = MenuPrim::kPolyG4;
    p.gouraud = true;
    p.semi = (c0 & 0x2000000u) != 0;
    const int xs[4] = {x - 154, x - 128, x - 154, x - 128}, ys[4] = {y - 12, y - 12, y + 12, y + 12};
    const uint32_t cs[4] = {c0, c1, c0, c1};
    for (int k = 0; k < 4; k++) p.x[k] = int16_t(xs[k]), p.y[k] = int16_t(ys[k]), p.colour[k] = cs[k] & 0xFFFFFF;
    ot->Add(p);
    ot->DrawMode(0x220);
    if (r.flag >= 0 && r.carMark != 0) {
        const uint8_t f = career_[kCareerFlags + size_t(r.flag)];
        const bool won = ((f >> 1) & 1) || ((f >> 2) & 1);
        const uint32_t colour = LerpColour(o.Get<uint32_t>(kZero), o.Get<uint32_t>(won ? kCarWonColour : kCarColour), alpha, 128);
        SpriteAt(o, *ot, kCarMark, x + 144, y, colour);
    }
    uint32_t banner = kBanners;
    if (r.flag >= 0) {
        const uint8_t f = career_[kCareerFlags + size_t(r.flag)];
        const int level = (f & 4) ? 2 : (f & 2) ? 1 : (f & 1) ? 0 : -1;
        if (level >= 0) SpriteAt(o, *ot, kMarks + uint32_t(level) * 12, x - 108, y, Grey(alpha));
        if (r.road != 0) {
            if (level == 2) SpriteAt(o, *ot, kRoadMark, x - 93, y, Grey(alpha));
            banner = kBanners + 12;
        }
    } else if (r.road == 0) {
        banner = kBanners + 24;
    } else {
        SpriteAt(o, *ot, kRoadMark, x - 93, y, Grey(alpha));
        banner = kBanners + 36;
    }
    const uint32_t colour = LerpColour(o.Get<uint32_t>(kZero), o.Get<uint32_t>(r.dirt != 0 ? kDirtBanner : kBanner), alpha, 128);
    SpriteAt(o, *ot, banner, x, y, colour);
    ot->DrawMode(0x20);
}

void ArcadeBonusPage::Draw(ViewOt& ot, TextCtx& c) const { // 0x80023EB8
    const GuestImage& o = a_.data.ovl2;
    c.thickUnderline = false; // 0x8006AB78(ctx, 30)
    c.ot = &ot.slot[0];
    c.font = &a_.FontAt(kSmallFont);
    c.mode = 1;
    dialog_.Draw(a_, ot);
    rowCtx_ = &c;
    MenuListDraw(list_, ot.slot[2]);
    rowCtx_ = nullptr;
    if (list_.state == -1) return;
    const int alpha = (int(list_.fade) << 7) / list_.fadeMax;
    SpriteAt(o, ot.slot[0], kLegend, 96, 102, Grey(alpha));
    SpriteAt(o, ot.slot[0], kLegend + 12, 250, 102, Grey(alpha));
    c.ot = &ot.slot[0];
    c.colour = LerpColour(o.Get<uint32_t>(kBlack), o.Get<uint32_t>(kMessageColour), alpha, 128);
    const std::string message = a_.data.Text(kBonusMessage);
    DrawText(c, message, 176 - (TextWidth(c, message, 1) >> 1), 138, 1); // 0x8006ACC4
}

// ---------------------------------------------------------------- ENDING CREDITS

ArcadeCreditsPage::ArcadeCreditsPage(const ArcadeMenuAssets& a) : a_(a) {}

void ArcadeCreditsPage::Enter(std::span<const uint8_t> career) { // 0x800240BC
    timer_ = 24;
    list_ = PanelList::Read(a_.data.ovl2, kCreditsList, kCreditsRows);
    // Row +0 bit 7 (disabled) set, then cleared when the row's ending is open.
    list_.items.at(0).kind |= 0x80;
    if (AllBonusCoursesWon(a_, career)) list_.items.at(0).kind &= 0x7F;
    list_.items.at(1).kind |= 0x80;
    if (career[kSimulationDone] != 0) list_.items.at(1).kind &= 0x7F;
    list_.Init();
    list_.selected = 0;
    logo_ = 1;
}

int ArcadeCreditsPage::Update(const MenuListPad* pad, std::vector<int>& sounds) { // 0x8002416C
    if (timer_ > 0 && --timer_ == 0) list_.Open();
    if (logo_ > 0) {
        logo_++;
        if (logo_ >= 13) logo_ = 12;
    }
    if (logo_ < 0) logo_++;
    const int32_t r = list_.Update(pad);
    if (r == -3) {
        sounds.push_back(6);
        return -1;
    }
    if (r == -4) {
        sounds.push_back(0);
        return -1;
    }
    if (r == -2) return -1;
    if (r == -1) {
        sounds.push_back(4);
        logo_ = -12;
        list_.Close();
        return -2;
    }
    sounds.push_back(3);
    list_.Close(); // then 0x8002585C (the card manager's 0x80025790) and view 0x80052670
    return r;
}

void ArcadeCreditsPage::Draw(MenuOtSlot& ot) const { // 0x800242FC
    list_.Draw(ot);
    const int alpha = (std::abs(logo_) << 7) / 12;
    SpriteAt(a_.data.ovl2, ot, kCreditsLogo, 176, 150, Grey(alpha));
}

} // namespace gt2::arcade
