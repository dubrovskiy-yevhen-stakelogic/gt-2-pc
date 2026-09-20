#include "game/arcade/arcade_card.h"

#include <algorithm>
#include <cstring>

#include "game/career/career_state.h"

namespace gt2::arcade {
namespace {

namespace mp = menu_list_pad;

// ---- member 2 (ARCADE v1.1)
constexpr uint32_t kSetup = 0x80051FD0u;          // {+0xC / +0x10 / +0x14 font descriptors copied to 0x800F3708 / 36E8 / 36F8, +0x18 page}
constexpr uint32_t kFontCopies[3] = {0x800F3708u, 0x800F36E8u, 0x800F36F8u};
constexpr uint32_t kHeaderBand = 0x800526C4u;     // 0x1C bytes -> +0x34
constexpr uint32_t kBarTemplates[5] = {0x800526E0u, 0x80052710u, 0x80052740u, 0x80052770u, 0x800527A0u};
constexpr uint32_t kProgress = 0x800527D0u;       // -> +0x348
constexpr uint32_t kCardNames = 0x80052808u;      // "Memory Card 1" / "2"
constexpr uint32_t kEmpty = 0x80027298u;          // ""
constexpr uint32_t kNoCars = 0x800F8475u;         // "No Cars Found"
// ---- the global text block (data-global.txd at 0x801EF0E0)
constexpr uint32_t kSelectSlot = 0x801EF140u, kChecking = 0x801EF15Du, kDoNotRemove = 0x801EF17Bu, kNoCard = 0x801EF1A6u;
constexpr uint32_t kCardError = 0x801EF1C3u, kStartLoading = 0x801EF2B7u, kLoadingNow = 0x801EF2CCu, kLoadingComplete = 0x801EF2DFu;
constexpr uint32_t kLoadFailed = 0x801EF2F6u, kNoGameFile = 0x801EF3CFu, kCorrupt = 0x801EF3F4u;
// ---- EXE (US Arcade v1.1; Simulation 0x80091EC8 / 0x80091EE4)
constexpr uint32_t kBarText = 0x80091BC0u, kBarZero = 0x80091BDCu;
constexpr char kSaveName[] = "BASCUS-94455GAME"; // EXE 0x8009198C
constexpr uint32_t kFileSize = 0x7EA0;            // 0x80069F08
constexpr size_t kFileGarage = 0x200 + 0x3C74, kGarageSize = 0x4028; // 0x8006A1C4
constexpr int kExit = 10, kLoaded = 8;

uint32_t Grey(int v) { return uint32_t(v) | uint32_t(v) << 8 | uint32_t(v) << 16; }

} // namespace

// ---------------------------------------------------------------- bar

void ArcExeBar::Init(const ArcadeMenuAssets& a, uint32_t t, int c) { // EXE 0x8006E0DC
    const GuestImage& o = a.data.ovl2;
    x = o.Get<int16_t>(t), y = o.Get<int16_t>(t + 2);
    flags = o.Get<uint16_t>(t + 0x20);
    const int size = flags & 6;
    w = int16_t(size == 6 ? 0x80 : (size == 2 || size == 4) ? 0x60 : 0x50);
    h = int16_t((size == 4 || size == 6) ? 0x18 : 0x0C);
    // The template gets the font (+0x28), the height (+0x24), the flags and c0 of each text.
    screens::TextObject base = screens::TextObject::FromTemplate(a.exe, kBarText, "");
    base.font = o.Get<uint32_t>(t + 0x28);
    base.height = o.Get<uint8_t>(t + 0x24);
    const uint32_t titleColour = o.Get<uint32_t>(t + 0x10), labelColour = o.Get<uint32_t>(t + 0x14);
    title = base;
    title.flags = (titleColour & 0x2000000u) ? 0xE9 : 0xC8;
    title.c0 = titleColour;
    title.text = a.AnyText(o.Get<uint32_t>(t + 4));
    label0 = base;
    label0.flags = (labelColour & 0x2000000u) ? 0xE1 : 0xC0;
    label0.c0 = labelColour;
    label1 = label0;
    label0.text = a.AnyText(o.Get<uint32_t>(t + 8));
    label1.text = a.AnyText(o.Get<uint32_t>(t + 0xC));
    label0.extra = label1.extra = o.Get<int8_t>(t + 0x22);
    cursor = int8_t(c != 0);
    sound = o.Get<int8_t>(t + 0x2C);
    fill = o.Get<uint32_t>(t + 0x18);
    gradient = o.Get<uint32_t>(t + 0x1C);
    zero = a.exe.Get<uint32_t>(kBarZero);
    anim = -1;
    slide = 0;
}

void ArcExeBar::Open() { // EXE 0x8006E298
    title.Open(60);
    label0.Open(-1);
    label1.Open(-1);
    anim = 0;
    slide = 0;
    flags |= 0x80;
}

void ArcExeBar::Close() { // EXE 0x8006E30C
    anim = -17;
    title.Close();
    label0.Close();
    label1.Close();
}

int ArcExeBar::Update(const MenuListPad* pad, std::vector<int>& sounds) { // EXE 0x8006E34C
    if (anim < 0) {
        if (anim < -1) anim++;
        title.Tick();
        return -2;
    }
    if (++anim >= 72) {
        anim = 12;
        flags &= 0xFF7F;
    }
    if (slide > 0) slide--;
    int r = -2;
    if (pad) {
        const uint32_t pressed = pad->pressed;
        if (pressed & mp::kBack) {
            r = -1;
            if (flags & 8) Close();
        } else if (pressed & mp::kChoose) {
            r = cursor;
            if (flags & 8) Close();
        } else {
            int c = cursor;
            if (pressed & mp::kLeft) c = 0;
            if (pressed & mp::kRight) c = 1;
            if (c != cursor) {
                r = -3;
                if (anim >= 12) anim = 12;
                slide = 6;
                flags |= 0x80;
                if (sound >= 0) sounds.push_back(sound);
            }
            cursor = int8_t(c != 0);
        }
    }
    title.Tick();
    label0.Tick();
    label1.Tick();
    return r;
}

void ArcExeBar::Draw(const ArcadeMenuAssets& a, ViewOt& view, int slot) const { // EXE 0x8006E4C8
    if (anim == -1) return;
    screens::MenuOt ot;
    const int textDy = (flags & 4) ? -4 : -2;
    if (anim < -1) { // closing: the chosen button shrinks
        const int k = anim + 17;
        int cx = x + (w >> 1);
        if (cursor == 0) cx -= w;
        int s = w * k;
        if (s < 0) s += 15;
        const int wd = w - (s >> 4);
        const uint32_t c = LerpColour(fill, zero, k, 16);
        shell::AddGradientQuad(ot[0], cx - (wd >> 1), y, wd, h, c, c);
        ot[0].DrawMode(0x20);
        title.Draw(ot, 0, x, y + textDy, a.FontAt(title.font));
    } else {
        int g = 127, off = 0;
        if (anim < 12) {
            off = (w * 2 * (12 - anim)) / 12;
            g = (anim * 127) / 12;
        }
        if (flags & 1) off = -off;
        const int cx = x + off, left = cx - w, right = cx;
        MenuListFrame(ot[0], Grey(g), left, y, w, h);
        MenuListFrame(ot[0], Grey(g), right, y, w, h);
        shell::AddGradientQuad(ot[2], left, y, w, h, gradient, zero);
        shell::AddGradientQuad(ot[2], right, y, w, h, gradient, zero);
        ot[0].DrawMode(0x200);
        title.Draw(ot, 0, x, y + textDy, a.FontAt(title.font));
        screens::TextObject l0 = label0, l1 = label1;
        l0.alpha = l1.alpha = uint8_t(g);
        l0.Draw(ot, 0, left + (w >> 1), y + h + textDy, a.FontAt(l0.font));
        l1.Draw(ot, 0, right + (w >> 1), y + h + textDy, a.FontAt(l1.font));
        int k = 0;
        if (anim >= 12) k = std::max(0, 60 - (anim - 12) * 4);
        const uint32_t white = (flags & 0x80) ? 0xFFu : 0x80u;
        uint32_t c = LerpColour(fill, Grey(int(white)), k, 60);
        c = LerpColour(c, zero, std::max(0, 12 - int(anim)), 12);
        ot[1].Add(Tile(cursor ? right : left, y, w, h, c));
        if (slide > 0) {
            const int sw = (w * slide) / 6;
            if (cursor) shell::AddGradientQuad(ot[1], right - sw, y, sw, h, zero, c);
            else shell::AddGradientQuad(ot[1], right, y, sw, h, c, zero);
        }
        ot[1].DrawMode(0x220);
    }
    for (int s = 0; s < 3; s++) view.slot[size_t(slot + s)].Append(ot[s], 1); // screens::MenuOt's slots start with its marker packet
}

// ---------------------------------------------------------------- the manager

ArcadeGuestLoad::ArcadeGuestLoad(const ArcadeMenuAssets& a, std::array<shell::CardSlot, 2> slots) : a_(a), slots_(std::move(slots)) {}

const HudFont& ArcadeGuestLoad::Font(uint32_t descriptor) const {
    for (uint32_t k = 0; k < 3; k++)
        if (descriptor == kFontCopies[k]) return a_.FontAt(a_.data.ovl2.Get<uint32_t>(kSetup + 0xC + k * 4));
    return a_.FontAt(descriptor);
}

std::string ArcadeGuestLoad::Text(uint32_t address) const { return a_.AnyText(address); }

int ArcadeGuestLoad::Status() const { return shell::CardStatus(slots_[size_t(slot_)]); }

void ArcadeGuestLoad::Enter() { // 0x80023478 -> 0x80025178 + 0x8002521C (-> 0x80025030, state 0)
    const GuestImage& o = a_.data.ovl2;
    band_.Read(o, kHeaderBand);
    band_.anim = -1;
    for (size_t k = 0; k < bars_.size(); k++) {
        bars_[k].Init(a_, kBarTemplates[k], 0);
        // the bar templates name the manager's font copies
        for (screens::TextObject* t : {&bars_[k].title, &bars_[k].label0, &bars_[k].label1})
            for (uint32_t c = 0; c < 3; c++)
                if (t->font == kFontCopies[c]) t->font = o.Get<uint32_t>(kSetup + 0xC + c * 4);
    }
    bars_[1].cursor = 1;
    results_.fill(-2);
    progX_ = o.Get<int16_t>(kProgress), progY_ = o.Get<int16_t>(kProgress + 2);
    progW_ = o.Get<int16_t>(kProgress + 4), progH_ = o.Get<int16_t>(kProgress + 6), progPitch_ = o.Get<int16_t>(kProgress + 8);
    progVisible_ = -1;
    progress_.fill(-1);
    line1_ = line2_ = kEmpty;
    error_ = 0;
    slot_ = 0;
    garage_.clear();
    Switch(0);
}

int ArcadeGuestLoad::EnterState(int id) {
    switch (id) {
    case 0: delay_ = 24; return -1; // 0x80024A24
    case 1: // 0x80024A7C
        sounds_->push_back(0);
        bars_[0].Open();
        bars_[0].cursor = 0;
        line1_ = line2_ = kEmpty;
        return -1;
    case 2: // 0x80024BF0
        if (band_.anim >= 0) band_.anim = int16_t(~band_.steps);
        bars_[1].Open();
        line1_ = kSelectSlot, line2_ = kEmpty;
        return -1;
    case 3: // 0x80024CCC: then its step at once
        line1_ = kChecking, line2_ = kEmpty;
        band_.anim = 0;
        return Step();
    case 4: // 0x80024DCC
        bars_[2].Open();
        line1_ = kNoCard, line2_ = kEmpty;
        return -1;
    case 5: // 0x80024EB8
        sounds_->push_back(0);
        bars_[0].Open();
        bars_[0].cursor = 0;
        line1_ = kNoGameFile, line2_ = kEmpty;
        return -1;
    case 6: // 0x800246B8
        bars_[4].Open();
        bars_[4].cursor = 1;
        line1_ = kStartLoading, line2_ = kEmpty;
        return -1;
    case 7: { // 0x800247D0: 0x8006A0C4 starts the read of the file (0x7EA0 bytes, rounded to 0x7F00)
        std::vector<uint8_t> card;
        if (shell::CardStatus(slots_[size_t(slot_)], &card) != 0 || shell::CardFindFile(card, kSaveName) < 0) {
            error_ = kLoadFailed;
            return 1;
        }
        progColour_ = 0x0090500Cu;
        progTotal_ = (kFileSize + 0x7F) & ~0x7Fu;
        progVisible_ = 0; // 0x8006BF5C
        progress_.fill(-1);
        transferLeft_ = int(progTotal_);
        line1_ = kLoadingNow, line2_ = kDoNotRemove;
        return -1;
    }
    case 8: { // 0x80024930
        bars_[3].Open();
        bars_[3].cursor = 1;
        int16_t count = 0;
        std::memcpy(&count, garage_.data(), 2);
        line1_ = count > 0 ? kLoadingComplete : kNoCars, line2_ = kEmpty;
        return -1;
    }
    default: return -1;
    }
}

int ArcadeGuestLoad::Step() {
    auto choice = [&](int bar, int onChoose) { // the common rule: 0 -> sound 1 and `onChoose`, 1 / -1 -> exit
        const int r = results_[size_t(bar)];
        if (r == 0) {
            sounds_->push_back(1);
            return onChoose;
        }
        return (r == 1 || r == -1) ? kExit : -1;
    };
    switch (state_) {
    case 0: return --delay_ < 0 ? 2 : -1;
    case 1: return choice(0, 2);
    case 2: {
        const int r = results_[1];
        if (r == -1) return kExit;
        if (r == 0 || r == 1) {
            sounds_->push_back(1);
            slot_ = r;
            return 3;
        }
        return -1;
    }
    case 3: {
        std::vector<uint8_t> card;
        const int status = shell::CardStatus(slots_[size_t(slot_)], &card);
        if (status == 1) return -1;
        if (status == 0) return shell::CardFindFile(card, kSaveName) >= 0 ? 6 : 5; // 0x80069FE4
        if (status == 2) return 4;
        if (status == 4) {
            error_ = kCardError;
            return 1;
        }
        return 5;
    }
    case 4:
        if (Status() != 2) {
            bars_[2].Close();
            return 3;
        }
        return choice(2, 2);
    case 5: {
        const int status = Status();
        if (status == 2 || status == 4) {
            bars_[0].Close();
            return 3;
        }
        return choice(0, 2);
    }
    case 6: {
        if (Status() != 0) {
            bars_[4].Close();
            return 3;
        }
        const int r = results_[4];
        if (r == 0) {
            sounds_->push_back(1);
            return 7;
        }
        if (r == 1) {
            sounds_->push_back(1);
            return 2;
        }
        if (r == -1) {
            sounds_->push_back(2);
            return 2;
        }
        return -1;
    }
    case 7: {
        transferLeft_ = std::max(0, transferLeft_ - 0x80 * sectorsPerField);
        { // 0x8006BF84: the segments of the done part
            int32_t done = int32_t(progTotal_) - transferLeft_;
            done = std::clamp(done, 0, int32_t(progTotal_));
            uint32_t p;
            if (progTotal_ <= 0x9C3F) p = progTotal_ ? (uint32_t(done) * uint32_t(done)) / progTotal_ : 0;
            else p = (uint32_t(done) * uint32_t(done >> 8)) / (progTotal_ >> 8);
            for (int i = 0; i < 32; i++) {
                int8_t& k = progress_[size_t(i)];
                if (k < 0) {
                    if (!(int32_t(p) < int32_t((uint32_t(i) * progTotal_) >> 5))) k = 0;
                } else if (++k > 24) {
                    k = 24;
                }
            }
        }
        if (transferLeft_ > 0) return -1;
        progVisible_ = -1;
        try { // 0x8006A224 (CRC) + 0x8006A1C4: the file's home garage into the guest block
            const std::vector<uint8_t> card = career::ReadFileBytes(slots_[size_t(slot_)].path);
            const career::CareerSave save = career::LoadCareerFromCard(card);
            if (!save.CrcOk()) {
                error_ = kCorrupt;
                return 1;
            }
            const uint8_t* state = reinterpret_cast<const uint8_t*>(&save.state);
            garage_.assign(state + (kFileGarage - 0x200), state + (kFileGarage - 0x200) + kGarageSize);
            log.push_back("guest garage loaded from " + slots_[size_t(slot_)].path);
            return kLoaded;
        } catch (const std::exception& e) {
            log.push_back(std::string("guest garage: ") + e.what());
            error_ = kLoadFailed;
            return 1;
        }
    }
    case 8: return choice(3, 2);
    default: return -1;
    }
}

void ArcadeGuestLoad::Switch(int id) { // 0x80024FCC: the handler's enter, chained while it returns a state
    for (int guard = 0; guard < 16 && id >= 0 && id != kExit && id != kLoaded; guard++) {
        state_ = id;
        id = EnterState(id);
    }
}

int ArcadeGuestLoad::Update(const MenuListPad* pad, std::vector<int>& sounds) { // 0x800234C8 -> 0x80025258
    sounds_ = &sounds;
    if (state_ < 0) return 0;
    band_.Tick();
    for (size_t k = 0; k < bars_.size(); k++) results_[k] = bars_[k].Update(pad, sounds);
    int r = Step();
    for (int guard = 0; guard < 16; guard++) {
        if (r == kExit) { // the header band closes, the lines clear, no state
            if (band_.anim >= 0) band_.anim = int16_t(~band_.steps);
            line1_ = line2_ = kEmpty;
            state_ = -1;
            sounds.push_back(4); // 0x800234C8: the view goes back
            return 2;
        }
        if (r == kLoaded) {
            state_ = kLoaded;
            EnterState(kLoaded);
            return 3;
        }
        if (r < 0) return 0;
        state_ = r;
        r = EnterState(r);
    }
    return 0;
}

void ArcadeGuestLoad::Draw(ViewOt& ot) const { // 0x80025390 (into slot 4)
    MenuOtSlot& base = ot.slot[4];
    TextCtx c;
    c.font = &Font(kFontCopies[2]);
    c.thickUnderline = true;
    c.ot = &base;
    c.colour = 0x025C5248u;
    c.mode = 1;
    const std::string l1 = Text(line1_), l2 = Text(line2_);
    DrawText(c, l1, 176 - (TextWidth(c, l1, 1) >> 1), 214, 1); // 0x8006ACC4
    DrawText(c, l2, 176 - (TextWidth(c, l2, 1) >> 1), 250, 1);
    if (const int alpha = band_.Fade(); alpha != 0) {
        c.colour = LerpColour(0x02000000u, 0x0242362Au, alpha, 128);
        DrawText(c, Text(a_.data.ovl2.Get<uint32_t>(kCardNames + uint32_t(slot_) * 4)), 28, 126, 1);
        band_.Draw(base, 24, 110);
        base.DrawMode(0x220);
    }
    for (const ArcExeBar& b : bars_) b.Draw(a_, ot, 4);
    if (state_ == 1) { // h(2): the error text (a context of its own: page 0x1E, cell 7, mode 0)
        TextCtx e;
        e.font = &Font(kFontCopies[2]);
        e.thickUnderline = true;
        e.ot = &base;
        e.colour = 0x6F6F6Fu;
        e.mode = 0;
        const std::string s = Text(error_);
        DrawText(e, s, 176 - (TextWidth(e, s, 1) >> 1), 250, 1);
    }
    if (state_ == 7 && progVisible_ >= 0) // 0x8006C084: 32 segments
        for (int i = 0; i < 32; i++) {
            const int k = progress_[size_t(i)];
            uint32_t col = LerpColour(progColour_, 0, 0x60, 0x80);
            if (k >= 0) col = LerpColour(progColour_, 0xD4D4D4u, 24 - k, 24);
            base.Add(Tile(progX_ + progPitch_ * i, progY_, progW_, progH_, col));
        }
}

} // namespace gt2::arcade
