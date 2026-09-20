// The race overlay's pause menu and race-end display (see race_overlay_screens.h, docs/formats/race_screens.md).
#include "gt2view/race_overlay_screens.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>

#include "gt2formats/exe_profile.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

namespace gt2::raceui {

namespace {

// ---------------------------------------------------------------- ordering-table slot
// Every packet is inserted at the head of the slot (0x8007D024 / 0x8007DD3C / 0x8007DA44 ...), so the GPU draws the
// packets in the reverse order of generation and the commands of one packet in order.
class Slot {
public:
    void Packet(std::vector<Gp0Prim> commands) { packets_.push_back(std::move(commands)); }
    void Mode(uint16_t e1) { // 0x8007DA44(ot, mode)
        Gp0Prim p;
        p.kind = Gp0Prim::kMode;
        p.e1 = e1;
        Packet({p});
    }
    void AppendGpuOrder(std::vector<Gp0Prim>& out) const {
        for (auto it = packets_.rbegin(); it != packets_.rend(); ++it) out.insert(out.end(), it->begin(), it->end());
    }

private:
    std::vector<std::vector<Gp0Prim>> packets_;
};

// The text context of the EXE text engine (0x8006AC68(ctx, -1), font 0x8007DA80, colour ctx + 0x14, mode = flags
// bits 21..22): every glyph is one packet {E1 page | mode << 5, SPRT 0x64 / 0x66 (colour bit 25)} (0x8007DD3C).
struct TextContext {
    const HudFont* font = nullptr;
    uint32_t colour = 0x808080;
    int mode = 1;
    void Glyphs(Slot& ot, const std::vector<HudFontSprite>& glyphs) const {
        for (const HudFontSprite& g : glyphs) {
            Gp0Prim e;
            e.kind = Gp0Prim::kMode;
            e.e1 = uint16_t(g.tpage | (mode & 3) << 5);
            Gp0Prim s;
            s.kind = Gp0Prim::kSprite;
            s.x[0] = int16_t(g.x), s.y[0] = int16_t(g.y), s.w = int16_t(g.w), s.h = int16_t(g.h);
            s.u[0] = uint8_t(g.u), s.v[0] = uint8_t(g.v);
            s.clut = g.clut;
            s.colour = colour & 0xFFFFFF;
            s.semi = (colour & 0x2000000u) != 0;
            ot.Packet({e, s});
        }
    }
    void Text(Slot& ot, const std::string& text, int x, int y, int spacing) const { // 0x8006AC90
        std::vector<HudFontSprite> g;
        font->Text(text, x, y, spacing, g);
        Glyphs(ot, g);
    }
    void Centred(Slot& ot, const std::string& text, int x, int y, int spacing) const { // 0x8006ADB4
        Text(ot, text, x - (font->TextWidth(text, spacing) >> 1), y, spacing);
    }
    void Right(Slot& ot, const std::string& text, int x, int y, int spacing) const { // 0x8006AE28
        std::vector<HudFontSprite> g;
        font->TextRight(text, x, y, spacing, g);
        Glyphs(ot, g);
    }
    // 0x8006B49C: a time in fixed cells centred on x; 0x8006B3F4: right-aligned.
    void TimeCentred(Slot& ot, const std::string& text, int x, int y, int advance, int narrow, int signFlag, int dotShift) const {
        std::vector<HudFontSprite> g;
        font->Time(text, x - (font->TimeWidth(text, advance, narrow, signFlag) >> 1), y, advance, narrow, signFlag, dotShift, g);
        Glyphs(ot, g);
    }
    void TimeRight(Slot& ot, const std::string& text, int x, int y, int advance, int narrow, int signFlag, int dotShift) const {
        std::vector<HudFontSprite> g;
        font->TimeRight(text, x, y, advance, narrow, signFlag, dotShift, g);
        Glyphs(ot, g);
    }
};

uint8_t Byte(uint32_t word, int i) { return uint8_t(word >> (8 * i)); }

// 0x8006B548(from, to, t, n): per channel from + (to - from) * t / n (t clamped to 0..n), the top byte of `from`.
uint32_t Lerp(uint32_t from, uint32_t to, int t, int n) {
    t = std::clamp(t, 0, n);
    uint32_t c = 0;
    for (int k = 0; k < 3; k++) c |= uint32_t(int(Byte(from, k)) + (int(Byte(to, k)) - int(Byte(from, k))) * t / n) << (8 * k);
    return (c & 0xFFFFFF) | (from & 0xFF000000u);
}

Gp0Prim Tile(int x, int y, int w, int h, uint32_t colourWord) { // 0x8007D024
    Gp0Prim p;
    p.kind = Gp0Prim::kTile;
    p.x[0] = int16_t(x), p.y[0] = int16_t(y), p.w = int16_t(w), p.h = int16_t(h);
    p.colour = colourWord & 0xFFFFFF;
    p.semi = (colourWord & 0x2000000u) != 0;
    return p;
}

// 0x800683FC(ot, {colour, x, y, w, h}, 32): a TILE (x - w/2, y - h/2, w, h) with half-round ends of 6 POLY_F4 each
// (EXE table 0x80091A78: 13 {x, y} points of a half circle, x scaled by h * 32 >> 17, y by h >> 12).
void RoundedBar(Slot& ot, const RaceOverlayAssets& a, uint32_t colourWord, int x, int y, int w, int h) {
    const int half = int16_t(w) >> 1;
    const int right = x + half, left = x - half;
    ot.Packet({Tile(left, y - (int16_t(h) >> 1), w, h, colourWord)});
    const int sx = h * 32;
    auto px = [&](int i) { return (a.capTable[size_t(2 * i)] * sx) >> 17; };
    auto py = [&](int i) { return y + ((a.capTable[size_t(2 * i + 1)] * h) >> 12); };
    for (int i = 0; i < 12; i += 2) {
        Gp0Prim r;
        r.kind = Gp0Prim::kPolyF4;
        r.colour = colourWord & 0xFFFFFF;
        r.semi = (colourWord & 0x2000000u) != 0;
        r.x[0] = int16_t(right + px(i)), r.y[0] = int16_t(py(i));
        r.x[1] = int16_t(right + px(i + 1)), r.y[1] = int16_t(py(i + 1));
        r.x[2] = int16_t(right), r.y[2] = int16_t(y);
        r.x[3] = int16_t(right + px(i + 2)), r.y[3] = int16_t(py(i + 2));
        ot.Packet({r});
        Gp0Prim l = r;
        l.x[3] = int16_t(left - px(i)), l.y[3] = int16_t(py(i));
        l.x[1] = int16_t(left - px(i + 1)), l.y[1] = int16_t(py(i + 1));
        l.x[0] = int16_t(left - px(i + 2)), l.y[0] = int16_t(py(i + 2));
        l.x[2] = int16_t(left), l.y[2] = int16_t(y);
        ot.Packet({l});
    }
}

// A POLY_FT4 of a 12-byte sprite descriptor centred at (cx, cy): half extents (hx, hy), the top edge narrowed and the
// bottom edge widened by `taper` on both sides (a tilt), as 0x8002B170 (the licence prize picture) and 0x8002A3E0
// (the badges) build it.
Gp0Prim SpriteQuad(const HudSpriteDesc& d, int cx, int cy, int hx, int hy, int taper, uint32_t colourWord) {
    Gp0Prim p;
    p.kind = Gp0Prim::kPolyFT4;
    p.colour = colourWord & 0xFFFFFF;
    p.semi = (colourWord & 0x2000000u) != 0;
    p.x[0] = int16_t(cx - hx + taper), p.y[0] = int16_t(cy - hy);
    p.x[1] = int16_t(cx + hx - taper), p.y[1] = int16_t(cy - hy);
    p.x[2] = int16_t(cx - hx - taper), p.y[2] = int16_t(cy + hy);
    p.x[3] = int16_t(cx + hx + taper), p.y[3] = int16_t(cy + hy);
    p.u[0] = p.u[2] = uint8_t(d.u), p.u[1] = p.u[3] = uint8_t(d.u + d.w - 1);
    p.v[0] = p.v[1] = uint8_t(d.v), p.v[2] = p.v[3] = uint8_t(d.v + d.h - 1);
    p.clut = d.clut;
    p.e1 = uint16_t(d.tpage | 0x20);
    return p;
}

HudSpriteDesc ReadDesc(const GuestImage& img, uint32_t address) {
    HudSpriteDesc d;
    d.u = img.Get<uint8_t>(address), d.v = img.Get<uint8_t>(address + 1);
    d.clut = img.Get<uint16_t>(address + 2);
    d.w = img.Get<int16_t>(address + 4), d.h = img.Get<int16_t>(address + 6);
    d.tpage = img.Get<uint16_t>(address + 8);
    return d;
}

void UploadWords(std::vector<uint16_t>& vram, int x, int y, int w, int h, const uint8_t* bytes) {
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) vram[size_t((y + r) & 511) * 1024 + size_t((x + c) & 1023)] = uint16_t(bytes[(size_t(r) * w + c) * 2] | bytes[(size_t(r) * w + c) * 2 + 1] << 8);
}

// "Finish" (0x8002AB60(ot, timer)): shown while the timer is below 0x96; from 0x50 it brightens (0x8002F608 ->
// 0x8002F60C over 12 frames), from 0x5C two more copies move apart by up to 8 pixels while all three fade out
// (grey 0x90 - 12 k, semi-transparent additive).
void FinishText(Slot& ot, const RaceOverlayAssets& a, int timer) {
    if (timer >= 0x96) return;
    TextContext t{&a.bigFont, 0xA376E, 1};
    const int i = timer - 0x50;
    if (i >= 0) {
        if (i < 12) {
            t.colour = Lerp(a.Colour(0x8002F608), a.Colour(0x8002F60C), i, 12);
        } else {
            const int k = std::clamp(timer - 0x5C, 0, 12);
            const uint32_t g = uint32_t(0x90 - k * 12);
            t.colour = g | g << 8 | g << 16 | 0x2000000u;
            const int d = (k << 3) / 12;
            const std::string finish = a.Text(0x801C6C99);
            t.Centred(ot, finish, 0xA0 - d, 0x82, 0);
            t.Centred(ot, finish, 0xA0 + d, 0x82, 0);
        }
    }
    t.Centred(ot, a.Text(0x801C6C99), 0xA0, 0x82, 0);
}

// 0x8002A3E0(ot, x, y, sprite, t): a badge that grows in over 16 frames (height by 0x8002F5E8[16 - t], taper by
// 0x8002F5E8[t], colour 0x8002F604 -> 0x8002F5FC), stays (colour 0x006680) until 64 and shrinks away over 16 more.
void Badge(Slot& ot, const RaceOverlayAssets& a, int x, int y, const HudSpriteDesc& d, int t) {
    if (t < 0) return;
    const int hy = (int16_t(d.h) - (int16_t(d.h) >> 15)) >> 1; // h / 2 rounded toward zero
    int sy = 0, taper = 0;
    uint32_t colour = 0;
    if (t < 16) {
        sy = (a.ease[size_t(16 - t)] * hy) >> 7;
        taper = (a.ease[size_t(t)] * hy) >> 8;
        colour = Lerp(a.Colour(0x8002F604), a.Colour(0x8002F5FC), t, 16);
    } else if (t < 64) {
        sy = hy;
        colour = 0x6680;
    } else if (t - 64 < 16) {
        const int k = t - 64;
        sy = (a.ease[size_t(k)] * hy) >> 7;
        taper = -((a.ease[size_t(16 - k)] * hy) >> 8);
        colour = Lerp(a.Colour(0x8002F5FC), a.Colour(0x8002F604), k, 16);
    } else {
        return;
    }
    ot.Packet({SpriteQuad(d, x, y, int16_t(d.w) >> 1, sy, taper, colour)});
}

} // namespace

// ---------------------------------------------------------------- assets

std::string RaceOverlayAssets::Text(uint32_t address) const {
    // The build's address of the string (the profile's reference runs of the lui-built string references); none = "".
    const std::optional<uint32_t> at = exe.profile ? exe.profile->TryData(address, exe.module) : std::optional<uint32_t>(address);
    return at ? strings.At(*at) : std::string();
}

std::string RaceOverlayAssets::Text(uint32_t address, uint32_t offset) const {
    const std::optional<uint32_t> at = exe.profile ? exe.profile->TryData(address, exe.module) : std::optional<uint32_t>(address);
    return at ? strings.At(*at + offset) : std::string();
}

RaceOverlayAssets RaceOverlayAssets::Load(const DiscImage& disc, const GtfsVolume& vol, uint8_t language) {
    RaceOverlayAssets a;
    a.exe = LoadExeImage(disc);
    a.ovl0 = LoadOverlayImage(disc, kRaceOverlayIndex);
    a.largeFont = LoadHudFont(a.exe, a.exe.Sim(HudFont::kLargeFont));
    a.bigFont = LoadHudFont(a.exe, a.exe.Sim(0x8009313Cu));
    const bool arcade = a.exe.profile && a.exe.profile->build == ExeBuild::kArcadeUs11;
    a.strings = arcade ? LoadHudStrings(vol, language, kArcadeRaceTextBlockSize) : LoadHudStrings(vol, language);
    a.strings.base = a.exe.Sim(HudStrings::kBase);
    const RaceFont font = LoadRaceFont(vol, a.exe);
    for (int y = 0; y < RaceFont::kRows; y++)
        for (int x = 0; x < RaceFont::kWords; x++) a.vram[size_t(RaceFont::kVramY + y) * 1024 + size_t(RaceFont::kVramX + x)] = font.words[size_t(y) * RaceFont::kWords + size_t(x)];
    const HudSheet sheet = LoadHudSheet(vol);
    for (int y = 0; y < HudSheet::kSheetRows; y++)
        for (int x = 0; x < HudSheet::kSheetWords; x++) a.vram[size_t(y) * 1024 + size_t(HudSheet::kSheetVramX + x)] = sheet.sheet[size_t(y) * HudSheet::kSheetWords + size_t(x)];
    const std::vector<uint8_t> tim = vol.Read("arcade/license_tim.tim"); // 4-bit, no CLUT block: image block at +8
    if (tim.size() < 20 || tim[0] != 0x10 || (tim[4] & 8) != 0) throw std::runtime_error("arcade/license_tim.tim: unexpected TIM layout");
    const int w = tim[16] | tim[17] << 8, h = tim[18] | tim[19] << 8;
    if (tim.size() < 20 + size_t(w) * size_t(h) * 2) throw std::runtime_error("arcade/license_tim.tim: short image block");
    UploadWords(a.vram, 384, 256, w, h, tim.data() + 20);
    const uint32_t medals = a.ovl0.Sim(0x8005B18Cu); // the tables at the build's addresses (GuestImage::Sim)
    for (int i = 0; i < 4; i++) a.medals[size_t(i)] = ReadDesc(a.ovl0, medals + uint32_t(i) * 12);
    a.badgeA = ReadDesc(a.ovl0, a.ovl0.Sim(0x8002F7F8u));
    a.badgeB = ReadDesc(a.ovl0, a.ovl0.Sim(0x8002F804u));
    const uint32_t caps = a.exe.Sim(0x80091A78u), ease = a.ovl0.Sim(0x8002F5E8u);
    for (size_t i = 0; i < a.capTable.size(); i++) a.capTable[i] = a.exe.Get<int16_t>(caps + uint32_t(i) * 2);
    for (size_t i = 0; i < a.ease.size(); i++) a.ease[i] = a.ovl0.Get<uint8_t>(ease + uint32_t(i));
    return a;
}

// ---------------------------------------------------------------- pause

int PauseMenu::Update(bool up, bool down, bool choose) {
    if (counter < 0) return -1;
    counter = int8_t(counter + 1);
    if (counter >= 31) counter = 0;
    if (up) {
        selection = int8_t(std::max(0, selection - 1));
        counter = 0;
    }
    if (down) {
        selection = int8_t(std::min(1, selection + 1));
        counter = 0;
    }
    if (!choose) return -1;
    const int chosen = selection;
    selection = 0;
    counter = -1;
    return chosen;
}

std::vector<Gp0Prim> BuildPauseFrame(const RaceOverlayAssets& a, const PauseMenu& menu) {
    std::vector<Gp0Prim> out;
    if (menu.counter < 0) return out;
    Slot ot;
    TextContext text{&a.largeFont, 0x02475B5B, 1};
    // The selected button flashes from 0xF2F2F2 to 0x6EA0DC over 14 fields (channel = 0xF2 - (22, 82, 132) * t / 14).
    const int t = std::min<int>(menu.counter, 14);
    const uint32_t selected = uint32_t(0xF2 - 22 * t / 14) | uint32_t(0xF2 - 82 * t / 14) << 8 | uint32_t(0xF2 - 132 * t / 14) << 16 | 0x2000000u;
    const uint32_t labels[2] = {0x801C6DE8u, 0x801C6DF2u}; // ovl0 0x8002F5E0: "Continue", "Exit"
    for (int i = 0; i < 2; i++) {
        const int x = 0x9C + 4 * i, y = 0x6D + 22 * i;
        text.Centred(ot, a.Text(labels[i]), x, y + 7, 0);
        RoundedBar(ot, a, i == menu.selection ? selected : 0x02244290u, x, y, 100, 20);
        RoundedBar(ot, a, 0x02181818u, x, y, 100, 20);
        ot.Mode(0);
    }
    ot.AppendGpuOrder(out);
    return out;
}

// ---------------------------------------------------------------- race end

int LicencePrizeOf(const RaceEndState& s) {
    if (s.licenceResult != 1) return 0;
    int prize = 0;
    if (s.licenceTime < s.medalTimes[3] && s.fourthPrizeCounts) prize = 1;
    for (int k = 3; k >= 1; k--)
        if (s.licenceTime < s.medalTimes[size_t(k - 1)]) prize = 5 - k;
    return prize;
}

std::vector<Gp0Prim> BuildRaceEndFrame(const RaceOverlayAssets& a, const RaceEndState& s) {
    std::vector<Gp0Prim> out;
    if (s.timer < 0) return out;
    Slot ot;
    const int timer = s.timer;
    TextContext big{&a.bigFont, 0x808080, 1};
    auto darken = [&](int t) { // TILE (0, 0, 320, 240) subtractive, 4 per frame up to 0x40 (0x8002BB68)
        if (t < 1) return;
        const int c = std::min(t, 16);
        ot.Packet({Tile(0, 0, 0x140, 0xF0, uint32_t(c * 4) | uint32_t(c) << 10 | uint32_t(c) << 18 | 0x2000000u)});
        ot.Mode(0x40);
    };
    if (s.subMode == 3) { // licence test
        if (s.licenceResult != 1) {
            big.colour = 0x6E;
            big.Centred(ot, a.Text(0x801C708B), 0xA0, 0xA0, 0); // "FAIL"
            ot.AppendGpuOrder(out);
            return out;
        }
        FinishText(ot, a, timer);
        int t = timer - 0x78;
        if (t > 0) {
            const int prize = LicencePrizeOf(s);
            static const uint32_t kText[5] = {0x801C708Bu, 0x801C8488u, 0x801C703Cu, 0x801C7051u, 0x801C7065u}; // FAIL, KIDS PRIZE ACQUIRED!, BRONZE / SILVER / GOLD PRIZE!
            static const uint32_t kColour[5] = {0x8002F62Cu, 0x8002F628u, 0x8002F628u, 0x8002F624u, 0x8002F620u};
            static const int kMedal[5] = {-1, 3, 2, 1, 0};
            t = std::min(t, 12);
            big.colour = Lerp(a.Colour(0x8002F61C), a.Colour(kColour[prize]), t, 12);
            big.Centred(ot, a.Text(kText[prize]), 0xA0, 0xCE, -1);
            TextContext large = big;
            large.font = &a.largeFont;
            large.TimeCentred(ot, FormatRaceTime(s.licenceTime), 0xA0, 0xB2, 8, 7, 1, 0);
            if (kMedal[prize] >= 0) {
                const HudSpriteDesc& d = a.medals[size_t(kMedal[prize])];
                const int k = (t << 4) / 12;
                const int hy = (int16_t(d.h) - (int16_t(d.h) >> 15)) >> 1;
                const uint32_t colour = k == 16 ? a.Colour(0x8002F600) : Lerp(a.Colour(0x8002F604), a.Colour(0x8002F5FC), k, 16);
                ot.Packet({SpriteQuad(d, 0xA0, 0x6E, int16_t(d.w) >> 1, (a.ease[size_t(16 - k)] * hy) >> 7, (a.ease[size_t(k)] * hy) >> 8, colour)});
            }
        }
        darken(timer - 0x5A);
        ot.AppendGpuOrder(out);
        return out;
    }

    FinishText(ot, a, timer);
    Badge(ot, a, 0xA0, 0xD8, a.badgeB, s.auxTimer); // 0x8002A630: 0x8002F804 with the aux timer,
    Badge(ot, a, 0xA0, 0xD8, a.badgeA, s.auxTimer < 0 ? -1 : std::min(s.auxTimer - 0x50, 0x10)); // then 0x8002F7F8 from 0x50
    uint32_t header = 0x801C6CA0u; // "Results"
    const int cars = int(s.rows.size());
    TextContext rowText{&a.largeFont, 0x2004670, 1};
    // A row of the table (0x8002ACE0 results / 0x8002AF8C points): position digit, name, value, rounded bar. The 2 player Battle
    // (sub-mode 0) passes 0x8002ACE0's last argument 1: no row is highlighted as the player's.
    auto row = [&](int i, int t, const RaceEndRow& r, const std::string& value, bool timeValue, uint32_t barColour) {
        t = std::min(t, 8);
        rowText.colour = 0x2004670;
        rowText.Text(ot, std::string(1, char('1' + i)), i * 4 + 0x12, i * 24 + 0x51, 1);
        rowText.colour = r.player && s.subMode != 0 ? 0x70543A : 0x606060;
        rowText.Text(ot, r.name, i * 4 + 0x1F, i * 24 + 0x51, -1);
        if (timeValue) rowText.TimeRight(ot, value, i * 4 + 0x118, i * 24 + 0x51, 7, 6, 1, 0);
        else rowText.Right(ot, value, i * 4 + 0x118, i * 24 + 0x51, -1);
        RoundedBar(ot, a, Lerp(barColour, a.Colour(0x8002F618), 8 - t, 8), i * 4 + 0xAC, i * 24 + 0x4A, 0x130, 0x16);
        ot.Mode(0);
    };
    auto resultRows = [&] { // default case: the results with times from 0xBE, one row per 16 frames
        const int32_t leader = cars > 0 ? s.rows[0].finishTime : -1;
        // 0x8002B170: the leader's laps, at most the lap count + 1 (else lap count + 1).
        const int leaderLaps = cars > 0 && s.rows[0].laps <= s.lapCount + 1 ? s.rows[0].laps : s.lapCount + 1;
        for (int i = 0; i < cars; i++) {
            const int t = timer - (0xBE + 16 * i);
            if (t < 0) break;
            const RaceEndRow& r = s.rows[size_t(i)];
            std::string value;
            if (r.finishTime == leader) {
                value = FormatRaceTime(uint32_t(leader));
            } else {
                value = a.Text(0x801C6CAA); // "Running"
                if (r.finished) { // 0x80068B04: the difference to the winner
                    const int32_t d = r.finishTime - leader;
                    char text[32];
                    std::snprintf(text, sizeof text, "+%d.%03d", d / 1000, d % 1000);
                    value = text;
                }
                const int behind = leaderLaps - r.laps;
                if (behind > 1) {
                    char text[32];
                    std::snprintf(text, sizeof text, a.Text(0x801C6CBD).c_str(), behind - 1); // "%dLaps"
                    value = text;
                    if (behind == 2) value = a.Text(0x801C6CB4); // "1Lap"
                }
            }
            row(i, t, r, value, true, a.Colour(0x8002F614));
        }
    };
    if (s.subMode == 2 && s.seriesRaces > 1) {
        if (timer < 0x13C) {
            resultRows();
        } else if (timer < 0x1BA) {
            header = 0x801C78D7u; // "Points"
            for (int i = 0; i < cars; i++) {
                const int t = timer - (0x13C + 16 * i);
                if (t < 0) break;
                char pts[32];
                std::snprintf(pts, sizeof pts, a.Text(0x801C78BD).c_str(), s.rows[size_t(i)].racePoints); // "%dpts"
                row(i, t, s.rows[size_t(i)], pts, false, a.Colour(0x8002F614));
            }
        } else {
            header = 0x801C78C6u; // "Total Points"
            for (int i = 0; i < int(s.standings.size()); i++) {
                const int t = timer - (0x1BA + 16 * i);
                if (t < 0) break;
                char pts[32];
                std::snprintf(pts, sizeof pts, a.Text(0x801C78BD).c_str(), s.standings[size_t(i)].totalPoints);
                row(i, t, s.standings[size_t(i)], pts, false, a.Colour(0x8002F610));
            }
        }
    } else if (s.subMode == 0) { // 2 player Battle: the rows, then from 0xEE "PLAYER 1 WINS!!!" / "PLAYER 2 WINS!!!" (the leader's car)
        resultRows();
        int t = timer - 0xEE;
        if (t >= 0) {
            const bool secondWins = cars > 0 && s.rows[0].car != 0;
            t = std::min(t, 8);
            big.colour = Lerp(a.Colour(0x8002F604), 0x144080u, t, 8);           // 0x8006B548(0x8002F604, 0x144080, t, 8)
            big.Centred(ot, a.Text(0x801C7814u, secondWins ? 24u : 0u), 0xA0, 0xB4, 0); // Arcade 0x801C74F4 + 24 * (row 0 car != 0); 0x8006ADB4 at (160, 180)
        }
    } else if (s.subMode != 1 && (s.subMode < 6 || s.subMode > 10)) {
        resultRows();
    }
    if (s.subMode == 1 || (s.subMode >= 6 && s.subMode < 12)) { // the whole frame darkens by 6 per frame up to 0x60
        const int t = timer - 0x9E;
        if (t > 0) {
            const int c = std::min(t, 16) * 3;
            ot.Packet({Tile(0, 0, 0x140, 0xF0, uint32_t(c * 2) | uint32_t(c * 2) << 8 | uint32_t(c * 2) << 16 | 0x2000000u)});
            ot.Mode(0x40);
        }
    } else {
        const int t = timer - 0x9E;
        if (t > 0) { // the table's band darkens
            const int c = std::min(t, 16);
            ot.Packet({Tile(0, 0x32, 0x140, 0xBE, uint32_t(c * 4) | uint32_t(c) << 10 | uint32_t(c) << 18 | 0x2000000u)});
            ot.Mode(0x40);
        }
        if (timer > 0x95) { // header: a red line, the title, a black band
            ot.Packet({Tile(0, 0x30, 0x140, 2, 0x80)});
            big.colour = 0xA376F;
            big.Centred(ot, a.Text(header), 0xA0, 0x2E, 0);
            ot.Packet({Tile(0, 0, 0x140, 0x30, 0)});
        }
    }
    ot.AppendGpuOrder(out);
    return out;
}

// ---------------------------------------------------------------- captures

std::vector<std::string> ListGp0(const std::vector<Gp0Prim>& prims) {
    std::vector<std::string> out;
    char line[400];
    for (const Gp0Prim& p : prims) {
        switch (p.kind) {
        case Gp0Prim::kMode:
            std::snprintf(line, sizeof line, "E1 tpage=%03X blend=%u depth=%u dither=%u", p.e1 & 0x7FFu, (p.e1 >> 5) & 3u, (p.e1 >> 7) & 3u, (p.e1 >> 9) & 1u);
            break;
        case Gp0Prim::kSprite:
            std::snprintf(line, sizeof line, "RECT %02X rgb=%06X xy=(%d,%d) uv=(%u,%u) clut=%04X size=(%d,%d)%s", p.semi ? 0x66 : 0x64, p.colour, p.x[0], p.y[0], p.u[0], p.v[0], p.clut,
                          p.w, p.h, p.semi ? " semi" : "");
            break;
        case Gp0Prim::kTile:
            std::snprintf(line, sizeof line, "RECT %02X rgb=%06X xy=(%d,%d) size=(%d,%d)%s", p.semi ? 0x62 : 0x60, p.colour, p.x[0], p.y[0], p.w, p.h, p.semi ? " semi" : "");
            break;
        case Gp0Prim::kPolyF4:
            std::snprintf(line, sizeof line, "POLY %02X quad%s rgb=%06X v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) v3=(%d,%d)", p.semi ? 0x2A : 0x28, p.semi ? " semi" : "", p.colour, p.x[0], p.y[0],
                          p.x[1], p.y[1], p.x[2], p.y[2], p.x[3], p.y[3]);
            break;
        case Gp0Prim::kPolyFT4:
            std::snprintf(line, sizeof line,
                          "POLY %02X quad tex%s rgb=%06X v0=(%d,%d) uv0=(%u,%u) clut=%04X v1=(%d,%d) uv1=(%u,%u) tpage=%04X v2=(%d,%d) uv2=(%u,%u) v3=(%d,%d) uv3=(%u,%u)",
                          p.semi ? 0x2E : 0x2C, p.semi ? " semi" : "", p.colour, p.x[0], p.y[0], p.u[0], p.v[0], p.clut, p.x[1], p.y[1], p.u[1], p.v[1], p.e1, p.x[2], p.y[2],
                          p.u[2], p.v[2], p.x[3], p.y[3], p.u[3], p.v[3]);
            break;
        }
        out.push_back(line);
    }
    return out;
}

std::vector<Gp0Prim> ParseGp0Lines(const std::vector<std::string>& lines) {
    std::vector<Gp0Prim> out;
    auto field = [](const std::string& l, const char* key) -> const char* {
        const size_t at = l.find(key);
        return at == std::string::npos ? nullptr : l.c_str() + at + std::string(key).size();
    };
    auto pair = [&](const std::string& l, const char* key, int& a, int& b) {
        const char* f = field(l, key);
        if (!f) return false;
        return std::sscanf(f, "(%d,%d)", &a, &b) == 2;
    };
    for (const std::string& l : lines) {
        Gp0Prim p;
        int a = 0, b = 0;
        if (l.find(" E1 ") != std::string::npos || l.rfind("E1 ", 0) == 0) {
            p.kind = Gp0Prim::kMode;
            p.e1 = uint16_t(std::strtoul(field(l, "tpage="), nullptr, 16));
        } else if (const char* r = field(l, "RECT ")) {
            const unsigned cmd = unsigned(std::strtoul(r, nullptr, 16));
            p.semi = (cmd & 2) != 0;
            p.colour = uint32_t(std::strtoul(field(l, "rgb="), nullptr, 16));
            pair(l, "xy=", a, b), p.x[0] = int16_t(a), p.y[0] = int16_t(b);
            if (pair(l, "size=", a, b)) p.w = int16_t(a), p.h = int16_t(b);
            if (cmd & 4) {
                p.kind = Gp0Prim::kSprite;
                pair(l, "uv=", a, b), p.u[0] = uint8_t(a), p.v[0] = uint8_t(b);
                p.clut = uint16_t(std::strtoul(field(l, "clut="), nullptr, 16));
            } else {
                p.kind = Gp0Prim::kTile;
            }
        } else if (const char* q = field(l, "POLY ")) {
            const unsigned cmd = unsigned(std::strtoul(q, nullptr, 16));
            if ((cmd & 0x08) == 0 || (cmd & 0x10) != 0) continue; // only flat quads here
            p.kind = (cmd & 4) ? Gp0Prim::kPolyFT4 : Gp0Prim::kPolyF4;
            p.semi = (cmd & 2) != 0;
            p.colour = uint32_t(std::strtoul(field(l, "rgb="), nullptr, 16));
            for (int k = 0; k < 4; k++) {
                const std::string vk = " v" + std::to_string(k) + "=", uvk = " uv" + std::to_string(k) + "=";
                pair(l, vk.c_str(), a, b), p.x[k] = int16_t(a), p.y[k] = int16_t(b);
                if (p.kind == Gp0Prim::kPolyFT4) pair(l, uvk.c_str(), a, b), p.u[k] = uint8_t(a), p.v[k] = uint8_t(b);
            }
            if (p.kind == Gp0Prim::kPolyFT4) {
                p.clut = uint16_t(std::strtoul(field(l, "clut="), nullptr, 16));
                p.e1 = uint16_t(std::strtoul(field(l, "tpage="), nullptr, 16));
            }
        } else {
            continue;
        }
        out.push_back(p);
    }
    return out;
}

// ---------------------------------------------------------------- canvas (interpreter GPU rules)

void OverlayCanvas::Clear(int x, int y, int w, int h) {
    for (int r = 0; r < h; r++)
        for (int c = 0; c < w; c++) vram_[size_t((y + r) & 511) * 1024 + size_t((x + c) & 1023)] = 0;
}

uint16_t OverlayCanvas::Texel(int u, int v) const {
    u &= 0xFF, v &= 0xFF;
    const int pageX = (mode_ & 0xF) * 64, pageY = ((mode_ >> 4) & 1) * 256;
    const int clutX = (clut_ & 0x3F) * 16, clutY = (clut_ >> 6) & 0x1FF;
    switch ((mode_ >> 7) & 3) {
    case 0: return At(clutX + ((At(pageX + u / 4, pageY + v) >> ((u & 3) * 4)) & 0xF), clutY);
    case 1: return At(clutX + ((At(pageX + u / 2, pageY + v) >> ((u & 1) * 8)) & 0xFF), clutY);
    default: return At(pageX + u, pageY + v);
    }
}

void OverlayCanvas::Plot(int x, int y, int r, int g, int b, bool semi, bool mask) {
    if (x < left_ || x > right_ || y < top_ || y > bottom_ || x < 0 || y < 0 || x >= 1024 || y >= 512) return;
    uint16_t& dst = vram_[size_t(y) * 1024 + size_t(x)];
    if (semi) {
        const int br = (dst & 31) << 3, bg = ((dst >> 5) & 31) << 3, bb = ((dst >> 10) & 31) << 3;
        switch ((mode_ >> 5) & 3) {
        case 0: r = (br + r) / 2, g = (bg + g) / 2, b = (bb + b) / 2; break;
        case 1: r = br + r, g = bg + g, b = bb + b; break;
        case 2: r = br - r, g = bg - g, b = bb - b; break;
        default: r = br + r / 4, g = bg + g / 4, b = bb + b / 4; break;
        }
    }
    r = std::clamp(r, 0, 255), g = std::clamp(g, 0, 255), b = std::clamp(b, 0, 255);
    dst = uint16_t((r >> 3) | (g >> 3) << 5 | (b >> 3) << 10 | (mask ? 0x8000 : 0));
}

void OverlayCanvas::Triangle(const int* xs, const int* ys, const int* us, const int* vs, int i0, int i1, int i2, int r, int g, int b, bool textured, bool semi) {
    struct V { int x, y, u, v; };
    V v0{xs[i0], ys[i0], us[i0], vs[i0]}, v1{xs[i1], ys[i1], us[i1], vs[i1]}, v2{xs[i2], ys[i2], us[i2], vs[i2]};
    int64_t area = int64_t(v1.x - v0.x) * (v2.y - v0.y) - int64_t(v1.y - v0.y) * (v2.x - v0.x);
    if (area == 0) return;
    if (area < 0) std::swap(v1, v2), area = -area;
    const int minX = std::max(std::min({v0.x, v1.x, v2.x}), left_), maxX = std::min(std::max({v0.x, v1.x, v2.x}), right_);
    const int minY = std::max(std::min({v0.y, v1.y, v2.y}), top_), maxY = std::min(std::max({v0.y, v1.y, v2.y}), bottom_);
    auto edge = [](const V& a, const V& c, int x, int y) { return int64_t(c.x - a.x) * (y - a.y) - int64_t(c.y - a.y) * (x - a.x); };
    auto bias = [](const V& a, const V& c) { const int dx = c.x - a.x, dy = c.y - a.y; return (dy < 0 || (dy == 0 && dx > 0)) ? 0 : -1; };
    const int b0 = bias(v1, v2), b1 = bias(v2, v0), b2 = bias(v0, v1);
    const double inv = 1.0 / double(area);
    for (int y = minY; y <= maxY; y++)
        for (int x = minX; x <= maxX; x++) {
            const int64_t w0 = edge(v1, v2, x, y), w1 = edge(v2, v0, x, y), w2 = edge(v0, v1, x, y);
            if (w0 + b0 < 0 || w1 + b1 < 0 || w2 + b2 < 0) continue;
            if (!textured) { Plot(x, y, r, g, b, semi, false); continue; }
            const double l0 = double(w0) * inv, l1 = double(w1) * inv, l2 = double(w2) * inv;
            const uint16_t t = Texel(int(l0 * v0.u + l1 * v1.u + l2 * v2.u + 0.5), int(l0 * v0.v + l1 * v1.v + l2 * v2.v + 0.5));
            if (t == 0) continue;
            Plot(x, y, ((t & 31) << 3) * r / 128, (((t >> 5) & 31) << 3) * g / 128, (((t >> 10) & 31) << 3) * b / 128, semi && (t & 0x8000), (t & 0x8000) != 0);
        }
}

void OverlayCanvas::Draw(const std::vector<Gp0Prim>& prims, int originY) {
    top_ = originY, bottom_ = originY + 239;
    for (const Gp0Prim& p : prims) {
        const int r = int(p.colour & 0xFF), g = int((p.colour >> 8) & 0xFF), b = int((p.colour >> 16) & 0xFF);
        switch (p.kind) {
        case Gp0Prim::kMode: mode_ = uint16_t(p.e1 & 0x7FF); break;
        case Gp0Prim::kSprite:
            clut_ = p.clut;
            for (int y = 0; y < p.h; y++)
                for (int x = 0; x < p.w; x++) {
                    const uint16_t t = Texel(p.u[0] + x, p.v[0] + y);
                    if (t == 0) continue;
                    Plot(p.x[0] + x, originY + p.y[0] + y, ((t & 31) << 3) * r / 128, (((t >> 5) & 31) << 3) * g / 128, (((t >> 10) & 31) << 3) * b / 128, p.semi && (t & 0x8000),
                         (t & 0x8000) != 0);
                }
            break;
        case Gp0Prim::kTile:
            for (int y = 0; y < p.h; y++)
                for (int x = 0; x < p.w; x++) Plot(p.x[0] + x, originY + p.y[0] + y, r, g, b, p.semi, false);
            break;
        case Gp0Prim::kPolyF4:
        case Gp0Prim::kPolyFT4: {
            int xs[4], ys[4], us[4], vs[4];
            for (int k = 0; k < 4; k++) xs[k] = p.x[k], ys[k] = originY + p.y[k], us[k] = p.u[k], vs[k] = p.v[k];
            const bool textured = p.kind == Gp0Prim::kPolyFT4;
            if (textured) clut_ = p.clut, mode_ = uint16_t((mode_ & ~0x9FFu) | (p.e1 & 0x9FF));
            Triangle(xs, ys, us, vs, 0, 1, 2, r, g, b, textured, p.semi);
            Triangle(xs, ys, us, vs, 1, 2, 3, r, g, b, textured, p.semi);
            break;
        }
        }
    }
}

} // namespace gt2::raceui
