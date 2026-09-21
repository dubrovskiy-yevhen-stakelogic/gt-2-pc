#include "gt2view/hud.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace gt2view {
namespace {

// Colour words of the HUD routines (GP0 order 0xBBGGRR; bit 25 = semi-transparent).
constexpr uint32_t kCaption = 0x284078;      // the orange text and sprites
constexpr uint32_t kGearDim = 0x1C2644;      // 0x8002C1CC: the gear glyph while the clutch is not engaged
constexpr uint32_t kGearBox = 0x21E1E1E;     // the subtractive box behind the gear
constexpr uint32_t kFace = 0x2606060;        // dial / turbo face (additive)
constexpr uint32_t kTurboPlate = 0x2282828;  // the turbo gauge's back plate (mode 0)
constexpr uint32_t kRingGrey = 0x2505050, kRingRed = 0x20000F4;
constexpr uint32_t kReplayText = 0x606060;   // captured "Replay" / car name (RECT 64, opaque)
constexpr uint32_t kSemi = 0x2000000;

// VRAM of the console's HUD data (words): the sheet's dial slot and CLUT, the course map.
constexpr int kFontX = 384, kSheetX = 512, kDialClutRow = 228;
constexpr uint32_t kSinTable = 0x80093150u; // 4096 + 1024 entries: sin, and cos at +0x400 (0x8002BCF0)

// 16-bit pair add of the original's packed screen words (x in the low half, y in the high half): a negative x
// borrows from y (0x8002BF14 / 0x8002C00C add the pivot to the packed vertex word).
void PackedAdd(int& x, int& y, int px, int py) {
    const uint32_t word = (uint32_t(uint16_t(int16_t(x)))) | (uint32_t(uint16_t(int16_t(y))) << 16);
    const uint32_t sum = word + ((uint32_t(uint16_t(int16_t(py))) << 16) | uint32_t(uint16_t(int16_t(px))));
    x = int(int16_t(uint16_t(sum & 0xFFFF)));
    y = int(int16_t(uint16_t(sum >> 16)));
}

std::array<uint8_t, 3> ColorOf(uint32_t w) { return {uint8_t(w & 0xFF), uint8_t((w >> 8) & 0xFF), uint8_t((w >> 16) & 0xFF)}; }

} // namespace

// ---------------------------------------------------------------- setup

Hud::Hud(VkSceneRenderer& renderer, const gt2::GtfsVolume& vol, const gt2::GuestImage& exe, const gt2::GuestImage& raceOverlay)
    : renderer_(renderer), vol_(vol), font_(gt2::LoadRaceFont(vol, exe)), text_(gt2::LoadHudFont(exe, exe.Sim(gt2::HudFont::kSmallFont))),
      large_(gt2::LoadHudFont(exe, exe.Sim(gt2::HudFont::kLargeFont))), start_(gt2::LoadHudFont(exe, exe.Sim(gt2::HudFont::kStartFont))),
      sheet_(gt2::LoadHudSheet(vol)), strings_(gt2::LoadHudStrings(vol)), tables_(gt2::LoadHudTables(raceOverlay)), block_(size_t(1024) * 512, 0) {
    strings_.base = exe.Sim(gt2::HudStrings::kBase); // the race text copy of the executable's build
    try { // 0x8002D20C: the 5 bytes at 0x8002F2E8 copied as the text of a record without a speed
        for (uint32_t a = raceOverlay.Sim(0x8002F2E8u), k = 0; k < 5 && raceOverlay.Get<uint8_t>(a + k) != 0; k++) noRecordSpeed_.push_back(char(raceOverlay.Get<uint8_t>(a + k)));
    } catch (const std::exception&) {
        noRecordSpeed_.clear(); // a build without the fact: the empty record's speed is drawn as nothing
    }
    const uint32_t sinTable = exe.Sim(kSinTable); // the table of the executable's build (gt2formats/exe_profile.h)
    for (size_t i = 0; i < sinTable_.size(); i++) sinTable_[i] = exe.Get<int16_t>(sinTable + uint32_t(i) * 2);
    // 0x8002E204's strings: the jump table 0x8002F320 (12 cases, code - 1) leads to `lui v0, hi` + `addiu s3, v0, lo`
    // (the addiu in the next two instructions): the token of the race text copy. Read from the overlay's own code, so
    // no string or address list is kept here; a build without the table (profile gap) draws no warnings.
    try {
        const uint32_t table = raceOverlay.Sim(0x8002F320u);
        for (uint32_t i = 0; i < warningTokens_.size(); i++) {
            const uint32_t target = raceOverlay.Get<uint32_t>(table + i * 4);
            const uint32_t lui = raceOverlay.Get<uint32_t>(target);
            if ((lui >> 26) != 0x0F) throw std::runtime_error("warning case without lui");
            for (uint32_t k = 1; k <= 2; k++) {
                const uint32_t w = raceOverlay.Get<uint32_t>(target + k * 4);
                if ((w >> 26) == 0x09 && ((w >> 16) & 31) == 19 && ((w >> 21) & 31) == ((lui >> 16) & 31)) {
                    warningTokens_[i] = ((lui & 0xFFFF) << 16) + uint32_t(int32_t(int16_t(uint16_t(w & 0xFFFF))));
                    break;
                }
            }
            if (warningTokens_[i] == 0) throw std::runtime_error("warning case without addiu s3");
        }
    } catch (const std::exception& e) {
        std::printf("hud: warnings not available (%s)\n", e.what());
        warningTokens_.fill(0);
    }
    // 0x8002DE8C's sprites and tint colours (race overlay tables, descriptor = {u8 u, u8 v, u16 clut, u16 w, u16 h, u16 tpage})
    try {
        for (uint32_t i = 0; i < tyreSprites_.size(); i++) {
            const uint32_t a = raceOverlay.Sim(0x8002F81Cu) + i * 12;
            gt2::HudSpriteDesc& d = tyreSprites_[i];
            d.u = raceOverlay.Get<uint8_t>(a);
            d.v = raceOverlay.Get<uint8_t>(a + 1);
            d.clut = raceOverlay.Get<uint16_t>(a + 2);
            d.w = raceOverlay.Get<uint16_t>(a + 4);
            d.h = raceOverlay.Get<uint16_t>(a + 6);
            d.tpage = raceOverlay.Get<uint16_t>(a + 8);
        }
        tyreTintFrom_ = raceOverlay.Get<uint32_t>(raceOverlay.Sim(0x8002F904u));
        tyreTintTo_ = raceOverlay.Get<uint32_t>(raceOverlay.Sim(0x8002F908u));
        haveTyreSprites_ = true;
    } catch (const std::exception& e) {
        std::printf("hud: tyre panel not available (%s)\n", e.what());
    }
    // 0x8002A19C's texts: `lui v0, hi` (0x8002A1B8), `addiu s1, v0, lo` (0x8002A1C0) = START; in a replay
    // `addiu s1, s1, d` (0x8002A1D0) moves the token to REPLAY.
    try {
        const uint32_t lui = raceOverlay.Get<uint32_t>(raceOverlay.Sim(0x8002A1B8u));
        const uint32_t add = raceOverlay.Get<uint32_t>(raceOverlay.Sim(0x8002A1C0u));
        const uint32_t replay = raceOverlay.Get<uint32_t>(raceOverlay.Sim(0x8002A1D0u));
        if ((lui >> 26) != 0x0F || (add >> 26) != 0x09 || (replay >> 26) != 0x09 || ((add >> 16) & 31) != 17 || ((replay >> 21) & 31) != 17)
            throw std::runtime_error("unexpected code at 0x8002A1B8");
        startToken_ = ((lui & 0xFFFF) << 16) + uint32_t(int32_t(int16_t(uint16_t(add & 0xFFFF))));
        replayToken_ = startToken_ + uint32_t(int32_t(int16_t(uint16_t(replay & 0xFFFF))));
    } catch (const std::exception& e) {
        std::printf("hud: START / REPLAY text not available (%s)\n", e.what());
    }
    UploadBlock();
}

void Hud::UseCourse(const std::string& courseName) {
    try {
        map_ = gt2::LoadCourseMap(vol_, courseName);
        haveMap_ = true;
    } catch (const std::exception& e) {
        std::printf("hud: no course map for %s (%s)\n", courseName.c_str(), e.what());
        haveMap_ = false;
    }
    uploadedDial_ = -2; // rebuild the block
}

int Hud::DialIndexFor(int revLimitRpm) const {
    // 0x8002BD84: thousands = ceil(limit / 1000) (s16 + 999, signed division); the face = the first table entry
    // (0x8002F868, ascending, -1 terminated) that is not below it.
    const int thousands = (int(int16_t(revLimitRpm)) + 999) / 1000;
    int index = 0;
    while (index < int(tables_.dialLimits.size()) && tables_.dialLimits[size_t(index)] >= 0 && tables_.dialLimits[size_t(index)] < thousands) index++;
    return std::min(index, int(sheet_.dials.size()) - 1);
}

void Hud::UploadBlock() {
    std::fill(block_.begin(), block_.end(), uint16_t(0));
    for (int y = 0; y < gt2::RaceFont::kRows; y++)
        for (int x = 0; x < gt2::RaceFont::kWords; x++) block_[size_t(y) * 1024 + size_t(kFontX + x)] = font_.words[size_t(y) * gt2::RaceFont::kWords + size_t(x)];
    for (int y = 0; y < gt2::HudSheet::kSheetRows; y++)
        for (int x = 0; x < gt2::HudSheet::kSheetWords; x++) block_[size_t(y) * 1024 + size_t(kSheetX + x)] = sheet_.sheet[size_t(y) * gt2::HudSheet::kSheetWords + size_t(x)];
    if (uploadedDial_ >= 0) { // the car's dial face over face slot 0 of the sheet, its CLUT at (512, 228) (captured license test)
        const gt2::HudSheet::Dial& dial = sheet_.dials[size_t(uploadedDial_)];
        for (int y = 0; y < gt2::HudSheet::kDialRows; y++)
            for (int x = 0; x < gt2::HudSheet::kDialWords; x++) block_[size_t(y) * 1024 + size_t(kSheetX + x)] = dial.words[size_t(y) * gt2::HudSheet::kDialWords + size_t(x)];
        for (size_t i = 0; i < 16; i++) block_[size_t(kDialClutRow) * 1024 + kSheetX + i] = dial.clut[i];
    }
    if (uploadedDial2_ >= 0) { // the 2 player Battle: car 1's face over face slot 1 (0x8002E390: slot = the entry, image only)
        const gt2::HudSheet::Dial& dial = sheet_.dials[size_t(uploadedDial2_)];
        const int x0 = kSheetX + int(tables_.faces[1].u) / 4, y0 = int(tables_.faces[1].v);
        for (int y = 0; y < gt2::HudSheet::kDialRows; y++)
            for (int x = 0; x < gt2::HudSheet::kDialWords; x++) block_[size_t(y0 + y) * 1024 + size_t(x0 + x)] = dial.words[size_t(y) * gt2::HudSheet::kDialWords + size_t(x)];
    }
    if (haveMap_) {
        for (int y = 0; y < gt2::CourseMap::kRows; y++)
            for (int x = 0; x < gt2::CourseMap::kWords; x++)
                block_[size_t(gt2::CourseMap::kVramY + y) * 1024 + size_t(gt2::CourseMap::kVramX + x)] = map_.words[size_t(y) * gt2::CourseMap::kWords + size_t(x)];
        for (size_t i = 0; i < 16; i++) block_[size_t(gt2::CourseMap::kClutY) * 1024 + gt2::CourseMap::kClutX + i] = map_.clut[i];
    }
    renderer_.UploadVram(kRowBase, 512, block_.data());
}

// ---------------------------------------------------------------- packets

void Hud::Packet(std::initializer_list<Cmd> cmds) {
    std::vector<Cmd> p(cmds);
    // Keep draw-mode commands from hidden sections: later PS1 packets inherit their state.
    if (!visible_) p.erase(std::remove_if(p.begin(), p.end(), [](const Cmd& c) { return c.kind != Cmd::kMode; }), p.end());
    for (Cmd& c : p) c.anchor = anchor_;
    packets_.push_back(std::move(p));
}

void Hud::Mode(uint16_t tpage) {
    Cmd c;
    c.kind = Cmd::kMode;
    c.tpage = tpage;
    Packet({c});
}

void Hud::SpriteAt(int x, int y, const gt2::HudSpriteDesc& d, uint32_t colorWord) {
    Cmd c;
    c.kind = Cmd::kSprite;
    c.x[0] = x;
    c.y[0] = y;
    c.u = d.u;
    c.v = d.v;
    c.w = d.w;
    c.h = d.h;
    c.clut = d.clut;
    c.color[0] = colorWord & 0xFFFFFF;
    c.semi = (colorWord & kSemi) != 0;
    Packet({c});
}

// The HUD routines centre their sprites as x - (w << 16 >> 17), y - (h >> 1).
void Hud::SpriteCentred(int cx, int cy, const gt2::HudSpriteDesc& d, uint32_t colorWord) { SpriteAt(cx - (d.w >> 1), cy - (d.h >> 1), d, colorWord); }

void Hud::Tile(int x, int y, int w, int h, uint32_t colorWord) {
    Cmd c;
    c.kind = Cmd::kTile;
    c.x[0] = x;
    c.y[0] = y;
    c.w = w;
    c.h = h;
    c.color[0] = colorWord & 0xFFFFFF;
    c.semi = (colorWord & kSemi) != 0;
    Packet({c});
}

void Hud::PolyF4(const int xs[4], const int ys[4], uint32_t colorWord) {
    Cmd c;
    c.kind = Cmd::kPoly;
    std::copy(xs, xs + 4, c.x);
    std::copy(ys, ys + 4, c.y);
    std::fill(c.color, c.color + 4, colorWord & 0xFFFFFF);
    c.semi = (colorWord & kSemi) != 0;
    Packet({c});
}

void Hud::PolyG4(const int xs[4], const int ys[4], const uint32_t colors[4]) {
    Cmd c;
    c.kind = Cmd::kPoly;
    c.gouraud = true;
    std::copy(xs, xs + 4, c.x);
    std::copy(ys, ys + 4, c.y);
    for (int i = 0; i < 4; i++) c.color[i] = colors[i] & 0xFFFFFF;
    c.semi = (colors[0] & kSemi) != 0;
    Packet({c});
}

// 0x8006B77C with the rectangle 0x8002F8E0 {x, y, w, h = 5, colour left, colour right}: a gouraud quad.
void Hud::Bar(int x, int y, int w) {
    const int h = tables_.barHeight;
    const int xs[4] = {x, x + w, x, x + w}, ys[4] = {y, y, y + h, y + h};
    const uint32_t colors[4] = {tables_.barColor0, tables_.barColor1, tables_.barColor0, tables_.barColor1};
    PolyG4(xs, ys, colors);
}

// The sprites of the text engine: every glyph is one packet {draw mode (its texture page), sprite}.
void Hud::Glyphs(const std::vector<gt2::HudFontSprite>& sprites, uint32_t colorWord) {
    for (const gt2::HudFontSprite& s : sprites) {
        Cmd mode;
        mode.kind = Cmd::kMode;
        mode.tpage = s.tpage;
        Cmd c;
        c.kind = Cmd::kSprite;
        c.x[0] = s.x;
        c.y[0] = s.y;
        c.u = s.u;
        c.v = s.v;
        c.w = s.w;
        c.h = s.h;
        c.clut = s.clut;
        c.color[0] = colorWord & 0xFFFFFF;
        c.semi = (colorWord & kSemi) != 0;
        Packet({mode, c});
    }
}

// 0x8002BCF0: a point at (radius, offset) turned by `angle` (4096 per turn), 1/16 pixels -> pixels.
static void RotatePoint(const std::array<int16_t, 5120>& table, int angle, int r, int w, int& x, int& y) {
    const int c = table[size_t((angle & 0xFFF) + 0x400)], s = table[size_t(angle & 0xFFF)];
    x = (((r * c) >> 12) - ((w * s) >> 12) + 4) >> 4;
    y = (((w * c) >> 12) + ((r * s) >> 12) - 8) >> 4;
}

// 0x8002BF14: a gouraud quad from radius0 +- half0 to radius1 +- half1, colours (c0, c1, c0, c1).
void Hud::Needle(int angle, int pivotX, int pivotY, const gt2::HudTables::Needle& n) {
    int xs[4], ys[4];
    RotatePoint(sinTable_, angle, n.radius0, -n.half0, xs[0], ys[0]);
    RotatePoint(sinTable_, angle, n.radius0, n.half0, xs[1], ys[1]);
    RotatePoint(sinTable_, angle, n.radius1, -n.half1, xs[2], ys[2]);
    RotatePoint(sinTable_, angle, n.radius1, n.half1, xs[3], ys[3]);
    for (int i = 0; i < 4; i++) PackedAdd(xs[i], ys[i], pivotX, pivotY);
    const uint32_t colors[4] = {n.color0, n.color1, n.color0, n.color1};
    PolyG4(xs, ys, colors);
}

// ---------------------------------------------------------------- the routines

// 0x8002C76C (x 12, y 16): position badge, captions with their bars, race time, lap times, lap counter.
void Hud::LapBlock(const HudFrame& f, int x, int y) {
    const int mode = f.gameMode;
    if (mode == 0 || mode == 2 || mode == 4 || mode == 0xB) {
        const int p = std::clamp(f.position - 1, 0, 5);
        const gt2::HudSpriteDesc& badge = tables_.badges[size_t(p)];
        SpriteAt(x - ((badge.w >> 1) - 0x44), y - ((badge.h >> 1) - 0xE), badge, kCaption);
        // the draw mode is the u16 at 0x8002F6F8 + 12 p (the tpage words of the km/h, mph and strip descriptors)
        Mode(p == 0 ? tables_.unitKmh.tpage : p == 1 ? tables_.unitMph.tpage : tables_.strip[size_t(p - 2)].tpage);
    }
    std::vector<gt2::HudFontSprite> s;
    auto flush = [&](uint32_t color) { Glyphs(s, color); s.clear(); };
    const std::string totalTime = strings_.At(strings_.Find("Total Time")), lapTime = strings_.At(strings_.Find("Lap Time"));
    const int32_t sub = (f.subFrame & 0xF) * 1000 / 900;
    int line = y + 0x4C, history = 3;
    int top = y;
    switch (mode) {
    case 0: case 2: case 4: case 0xB: {
        const int w1 = text_.Text(totalTime, x + 4, y + 0x26, 0, s);
        const int w2 = text_.Text(lapTime, x + 4, y + 0x40, 0, s);
        flush(kCaption);
        const int w = std::max(w1, w2);
        Bar(x + 2, y + 0x21, w + 5);
        Bar(x + 2, y + 0x3B, w + 5);
        const int32_t total = f.finished ? f.totalMs : f.totalMs + sub;
        text_.TimeRight(gt2::FormatRaceTime(uint32_t(total)), x + 0x46, y + 0x31, 7, 5, 0, 2, s);
        flush(kCaption);
        break;
    }
    case 3: {
        line = f.licenseType > 1 && f.licenseType < 4 ? y + 0x14 : y + 0x26;
        const int w = text_.Text(lapTime, x + 4, line, 0, s);
        flush(kCaption);
        Bar(x + 2, line - 5, w + 5);
        line += 0xC;
        break;
    }
    case 7: case 8: case 9:
        top = y - 0x12;
        [[fallthrough]];
    case 1: case 6: case 10: {
        line = top + 0x32;
        history = 6;
        const int w = text_.Text(lapTime, x + 4, top + 0x26, 0, s);
        flush(kCaption);
        Bar(x + 2, top + 0x21, w + 5);
        break;
    }
    default:
        break;
    }
    const int count = int(f.laps.size());
    if (!f.replay) {
        int i = f.finished ? (count - 1) - (history - 1) : count - (history - 1);
        for (; i < count; i++) {
            if (i < 0) continue;
            text_.TimeRight(gt2::FormatRaceTime(uint32_t(f.laps[size_t(i)])), x + 0x46, line, 7, 5, 0, 2, s);
            line += 9;
        }
        flush(kCaption);
    } else {
        // 0x8003D1E4 (the lap shown, at least 1; 1 in "N laps" licence tests) - 1, looked up by 0x8005E378 in the
        // recorded laps: index = lap - (lapNumber - count), -1 outside
        const int shown = (f.lap < 1 || (mode == 3 && f.licenseType == 5)) ? 1 : f.lap;
        const int index = (shown - 1) - (f.resultsLapNumber - count);
        const int32_t bracket = index >= 0 && index < count ? f.laps[size_t(index)] : -1;
        const int w = text_.TimeRight(gt2::FormatRaceTime(uint32_t(bracket)), x + 0x46, line, 7, 5, 0, 2, s);
        text_.TimeRight("[", x - (w - 0x46), line, 7, 5, 0, 2, s);
        text_.Text("]", x + 0x46, line, 0, s);
        flush(kCaption);
        line += 9;
    }
    if (!f.finished) {
        text_.TimeRight(gt2::FormatRaceTime(uint32_t(f.lapMs + sub)), x + 0x46, line, 7, 5, 0, 2, s);
        flush(kCaption);
    }
    if (!(mode == 0 || mode == 1 || mode == 2 || mode == 4 || mode == 6 || mode == 10 || mode == 0xB)) return;
    text_.Text(strings_.At(strings_.base), x + 4, y + 8, 0, s); // "Lap" (0x801C6C50)
    flush(kCaption);
    int lap = std::max(f.lap, 1);
    lap = std::min(lap, std::max(1, f.lapCount));
    std::vector<int> glyphs;
    int pen = x + 0xC;
    if (mode == 1 || mode == 6 || mode == 10) {
        const std::string digits = std::to_string(std::min(lap, 999));
        for (char c : digits) glyphs.push_back(c - '0');
        pen += (4 - int(digits.size())) * 8;
    } else {
        const std::string digits = std::to_string(lap) + "/" + std::to_string(std::max(1, f.lapCount));
        if (lap < 10 && digits.size() < 5) pen = x + 0x14;
        for (char c : digits) glyphs.push_back(c == '/' ? 11 : c - '0');
    }
    const int advance = glyphs.size() > 5 ? 32 / (int(glyphs.size()) - 1) : 8;
    for (int g : glyphs) {
        const gt2::HudSpriteDesc& d = tables_.strip[size_t(g)];
        SpriteAt(pen - (d.w >> 1), y - ((d.h >> 1) - 0x13), d, kCaption);
        pen += advance;
    }
    Mode(tables_.strip[0].tpage);
}

// 0x80029064 (x 16, y 144): the course map sprite and a dot per car; nothing when the Course Map option is Off (the
// routine's first test, career + 0xB1).
void Hud::CourseMapPanel(const HudFrame& f, int x, int y) {
    if (!f.courseMap) return;
    std::vector<std::pair<int, int>> dots;
    for (const HudFrame::MapCar& c : f.mapCars) {
        if (!c.present) { dots.push_back({-1, -1}); continue; }
        dots.push_back({x + ((int(c.x) * 163) >> 12) + 48, y + ((int(c.z) * 163) >> 12) + 48});
    }
    for (size_t i = 0; i < dots.size(); i++)
        if (dots[i].first >= 0) Tile(dots[i].first - 1, dots[i].second - 1, 2, 2, int(i) == f.mapHighlight ? 0xFFu : 0xFF00u);
    for (size_t i = 0; i < dots.size(); i++)
        if (dots[i].first >= 0) Tile(dots[i].first - 2, dots[i].second - 2, 4, 4, 0);
    // 0x80080450: one packet {draw mode 9 (the map's page (576, 0), 4-bit), sprite 96 x 96 uv (0, 144) CLUT 0x7F17}
    Cmd mode;
    mode.kind = Cmd::kMode;
    mode.tpage = 9;
    Cmd c;
    c.kind = Cmd::kSprite;
    c.x[0] = x;
    c.y[0] = y;
    c.u = 0;
    c.v = 0x90;
    c.w = c.h = 0x60;
    c.clut = 0x7F17;
    c.color[0] = 0x808080;
    Packet({mode, c});
}

// 0x8002C00C (x 276, y 180): the face (additive) and the ring - one flat quad per 250 rpm of the scale, red from
// the car's red line; the geometry is 0x8002BD84's (radii 641 / 516 sixteenths, 2412 / 4096 of a turn from 1274).
void Hud::Tachometer(const HudFrame& f, int x, int y) {
    const int thousands = (int(int16_t(f.revLimitRpm)) + 999) / 1000;
    const int segments = thousands * 4;
    const int grey = int(int16_t(f.redlineRpm)) / 250;
    const gt2::HudSpriteDesc& face = tables_.faces[size_t(std::clamp(f.faceSlot, 0, int(tables_.faces.size()) - 1))]; // car + 0x880 (0x8002BD84: the entry)
    SpriteCentred(x, y, face, kFace);
    Mode(uint16_t(face.tpage | 0x20));
    auto spoke = [&](int i, int& ox, int& oy, int& ix, int& iy) {
        const int angle = int(int16_t(uint16_t((i * 2412) / std::max(segments, 1) + 1274)));
        RotatePoint(sinTable_, angle, 641, 0, ox, oy);
        RotatePoint(sinTable_, angle, 516, 0, ix, iy);
        PackedAdd(ox, oy, x, y);
        PackedAdd(ix, iy, x, y);
    };
    uint32_t color = kRingGrey;
    for (int i = 0; i < segments; i++) {
        if (!(i < grey)) color = kRingRed;
        int xs[4], ys[4];
        spoke(i, xs[0], ys[0], xs[1], ys[1]);
        spoke(i + 1, xs[2], ys[2], xs[3], ys[3]);
        PolyF4(xs, ys, color);
    }
    Mode(0);
}

// 0x8002C1CC (x 276, y 180): the needle, the gear glyph over its subtractive box, the unit, the speed digits.
void Hud::Needles(const HudFrame& f, int x, int y) {
    const int thousands = (int(int16_t(f.revLimitRpm)) + 999) / 1000;
    const int maxRpm = int(int16_t(thousands * 1000));
    const int angle = int(int16_t(uint16_t((int(int16_t(f.rpm)) * 0x96C) / std::max(maxRpm, 1) + 0x4FA)));
    Needle(angle, x, y, tables_.needles[0]);
    Needle(angle, x, y, tables_.needles[1]);
    const gt2::HudSpriteDesc& gear = tables_.strip[size_t(tables_.gearGlyph[size_t(std::clamp(f.gear, 0, 9))])];
    SpriteAt(x - ((gear.w >> 1) - 0x17), y - ((gear.h >> 1) - 2), gear, f.clutchEngaged ? kCaption : kGearDim);
    Mode(uint16_t(tables_.strip[0].tpage | 0x20));
    Tile(x + 0x10, y - 6, 0xE, 0x10, kGearBox);
    Mode(0x40);
    const gt2::HudSpriteDesc& unit = f.metric ? tables_.unitKmh : tables_.unitMph;
    SpriteAt(x - ((unit.w >> 1) - 0x12), y - ((unit.h >> 1) - 0x28), unit, kCaption);
    Mode(uint16_t(unit.tpage | 0x20));
    int value = int(uint16_t(f.speedReadout)) / 100;
    if (f.metric) value = int(std::lround(double(uint16_t(f.speedReadout)) * 1.609344 / 100.0)); // ours: the readout is mph
    bool shown = false;
    int divisor = 100, pen = x + 2;
    for (int i = 0; i < 3; i++) {
        const int digit = value / divisor;
        value -= digit * divisor;
        if (digit != 0 || shown) {
            const gt2::HudSpriteDesc& d = tables_.speedDigits[size_t(std::clamp(digit, 0, 9))];
            SpriteAt(pen - (d.w >> 1), (y + 0x18) - (d.h >> 1), d, kCaption);
            shown = true;
        }
        divisor /= 10;
        pen += 0xC;
        if (divisor == 1) shown = true;
    }
    Mode(uint16_t(tables_.speedDigits[0].tpage | 0x20));
}

// 0x8002C584 (x 224, y 204): only for turbo cars (car + 0x154 != 0).
void Hud::TurboGauge(const HudFrame& f, int x, int y) {
    if (f.turbo == 0) return;
    int scale = f.turbo > 0x3000 ? 0x4000 : 0x2000;
    const int boost = int(int16_t(f.boost)) - 0x1000;
    int a;
    if (boost < 0) {
        a = boost * 0x555;
        if (a < 0) a += 0xFFF;
        a >>= 12;
    } else {
        a = (boost * 0x555) / scale;
    }
    const int angle = int(int16_t(uint16_t(a + 0x6AA)));
    Needle(angle, x, y, tables_.needles[2]);
    Needle(angle, x, y, tables_.needles[3]);
    SpriteCentred(x, y, tables_.turbo[0], kFace);
    Mode(uint16_t(tables_.turbo[0].tpage | 0x20));
    SpriteCentred(x, y, tables_.turbo[1], kTurboPlate);
    Mode(tables_.turbo[1].tpage);
}

// 0x8002D058: a time right-aligned at x + 3 and, when set, its top speed + unit right-aligned at x one line lower.
void Hud::TimeBlock(int32_t ms, int speed, int x, int y, const HudFrame& f) {
    std::vector<gt2::HudFontSprite> s;
    text_.TimeRight(gt2::FormatRaceTime(uint32_t(ms)), x + 3, y, 7, 5, 0, 2, s);
    if (ms != -1) {
        int readout = speed;
        if (f.metric) readout = int(std::lround(speed * 1.609344)); // ours: the readout is mph
        const std::string unit = strings_.At(strings_.Find(f.metric ? "km/h" : "mph"));
        text_.NumberRight(gt2::FormatRaceSpeed(uint32_t(std::max(readout, 0))) + unit, x, y + 9, 0, 3, -1, s);
    }
    Glyphs(s, kCaption);
}

// 0x8002D308 (x 308, y 16): "Record" (the course record) and "Best Lap" (this race's) with times and speeds.
void Hud::RecordPanel(const HudFrame& f, int x, int y) {
    bool record = true, best = true;
    switch (f.gameMode) {
    case 0: return;
    case 3: { // the licence's record block and the three medal times (0x8002D12C, 0x8003D7B8 on the test's settings)
        record = best = false;
        TimeBlock(f.licenseRecordMs, f.licenseRecordSpeed, x, y + 0x20, f);
        static constexpr uint32_t kMedal[3] = {0x64C0E0, 0xE0C070, 0x1050A0}; // gold, silver, bronze (0x8002D4AC..)
        for (int m = 0; m < 3; m++)
            if (f.medalMs[size_t(m)] >= 0) MedalLine(f.medalMs[size_t(m)], x, y + 58 + 9 * m, kMedal[m]);
        break;
    }
    case 7: case 8: case 9: record = best = false; break; // the machine test's record entry (0x8002D20C) below
    default: break;
    }
    std::vector<gt2::HudFontSprite> s;
    int w = text_.TextRight(strings_.At(strings_.Find("Record")), x, y + 0x14, 0, s);
    Glyphs(s, kCaption);
    s.clear();
    Bar(x - w - 2, y + 0xF, w + 5);
    if (best) {
        w = text_.TextRight(strings_.At(strings_.Find("Best Lap")), x, y + 0x3A, 0, s);
        Glyphs(s, kCaption);
        s.clear();
        Bar(x - w - 2, y + 0x35, w + 5);
    }
    if (record) TimeBlock(f.recordMs, f.recordSpeed, x, y + 0x20, f);
    if (best) TimeBlock(f.bestLapMs, f.bestLapSpeed, x, y + 0x46, f);
    if (f.gameMode >= 7 && f.gameMode <= 9) MachineRecord(f.machineRecord, f.gameMode == 9, x, y + 0x20, f);
}

// 0x8002D20C(ctx, the record's entry 0, x, y, speed): a time right-aligned at x + 3 (sub-modes 7 / 8), or the speed readout +
// the unit right-aligned at x (sub-mode 9; "----" of 0x8002F2E8 for an empty record).
void Hud::MachineRecord(uint32_t value, bool speed, int x, int y, const HudFrame& f) {
    std::vector<gt2::HudFontSprite> s;
    if (!speed) {
        text_.TimeRight(gt2::FormatRaceTime(value), x + 3, y, 7, 5, 0, 2, s);
    } else if (value == 0xFFFFFFFFu) {
        text_.NumberRight(noRecordSpeed_, x, y, 0, 3, -1, s);
    } else {
        int readout = int(value);
        if (f.metric) readout = int(std::lround(readout * 1.609344)); // ours: the readout is mph (as TimeBlock)
        const std::string unit = strings_.At(strings_.Find(f.metric ? "km/h" : "mph"));
        text_.NumberRight(gt2::FormatRaceSpeed(uint32_t(std::max(readout, 0))) + unit, x, y, 0, 3, -1, s);
    }
    Glyphs(s, kCaption);
}

// 0x8006B548: a colour word faded from `from` towards `to` by t / scale (t clamped to 0..scale, signed division per
// channel); the command byte of `from` stays.
static uint32_t FadeColor(uint32_t from, uint32_t to, int t, int scale = 0x80) {
    if (scale < t) t = scale;
    if (t < 0) t = 0;
    uint32_t out = 0;
    for (int k = 0; k < 3; k++) {
        const int a = int((from >> (8 * k)) & 0xFF), b = int((to >> (8 * k)) & 0xFF);
        out |= uint32_t(a + ((b - a) * t) / scale) << (8 * k);
    }
    return (out & 0xFFFFFFu) | (from & 0xFF000000u);
}

// 0x80068B04: the split difference "+S.mmm" / "-S.mmm" / character 0xB1 + "0.000" (equal), "--:--:---" when one is unset.
static std::string SplitDifference(uint32_t a, uint32_t b) {
    if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu) return "--:--:---";
    std::string s(1, '-');
    uint32_t d = a - b;
    if (a == b) { d = 0; s[0] = char(-0x4F); } // code 0xB1 (the font's plus-minus glyph)
    else if (a < b) { s[0] = '+'; d = b - a; }
    if (d < 60000) {
        char text[16];
        std::snprintf(text, sizeof(text), "%u.%03u", d / 1000, d % 1000);
        return s + text;
    }
    return s + gt2::FormatRaceTime(d);
}

// 0x8002A19C (the start timer 0x800AF224, text centred at (160, 110), the third font, draw mode blend 1):
// timer 241..419: the countdown digit '1' + (timer - 240) / 60 in 0x6F6F6F; 121..240: START (REPLAY in a replay) in
// 0x0A376E; 105..120 (t = 120 - timer 0..15): semi-transparent, colour from (110, 55, 10) + t * (34, 89, 134) / 16;
// 89..104 (t 16..31): semi-transparent grey 144 - 9 (t - 16); otherwise nothing.
void Hud::StartDisplay(const HudFrame& f) {
    const int c = f.startTimer;
    if (c < 0) return;
    std::string text;
    uint32_t color = 0;
    if (c >= 241) {
        const int digit = (c - 240) / 60;
        if (digit >= 3) return;
        text = std::string(1, char('1' + digit));
        color = 0x6F6F6F;
    } else {
        if (startToken_ == 0) return;
        text = strings_.At(f.replay ? replayToken_ : startToken_);
        if (c >= 121) {
            color = 0x0A376E;
        } else {
            const int t = 120 - c;
            if (t >= 32) return;
            if (t < 16) {
                const int r = (t * 34) / 16 + 110, g = (t * 89) / 16 + 55, b = (t * 134) / 16 + 10;
                color = kSemi | uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16;
            } else {
                const uint32_t grey = uint32_t(144 - 9 * (t - 16));
                color = kSemi | grey | grey << 8 | grey << 16;
            }
        }
    }
    std::vector<gt2::HudFontSprite> s;
    start_.Text(text, 0xA0 - (start_.TextWidth(text, 0) >> 1), 0x6E, 0, s); // 0x8006ADB4
    for (gt2::HudFontSprite& g : s) g.tpage = uint16_t((g.tpage & ~0x60) | 0x20);
    Glyphs(s, color);
}

// 0x8002E204 (x 160, y 182): the warning line of car + 0x790 (codes 1..12 through the jump table 0x8002F320; other
// codes an empty string), large font centred, colour 0x020C1879 with the context's blend bits cleared (mode 0).
void Hud::Warning(const HudFrame& f, int x, int y) {
    if (f.messageCode < 1 || f.messageCode > 12 || warningTokens_[0] == 0) return;
    const std::string text = strings_.At(warningTokens_[size_t(f.messageCode - 1)]);
    std::vector<gt2::HudFontSprite> s;
    large_.Text(text, x - (large_.TextWidth(text, 0) >> 1), y, 0, s);
    for (gt2::HudFontSprite& g : s) g.tpage = uint16_t(g.tpage & ~0x60);
    Glyphs(s, 0x20C1879);
}

// 0x80043108: the colour of a wheel's wear stage (wheel + 0x3F, signed): >= 64 (126, 127 - v, 0), 32..63
// (126, 2 (95 - v), 0), 0..31 (4 v, 126, 0), -63..-1 (0, 126 - 2|v|, 2|v|), below -63 black.
static uint32_t WearStageColor(int v) {
    int r = 0, g = 0, b = 0;
    if (v >= 64) { r = 126; g = 127 - v; }
    else if (v >= 32) { r = 126; g = (95 - v) * 2; }
    else if (v >= 0) { r = v * 4; g = 126; }
    else if (v >= -63) { g = 126 - 2 * -v; b = 2 * -v; }
    return uint32_t(r) | uint32_t(g) << 8 | uint32_t(b) << 16;
}

// 0x8002DE8C (x 296, y 124): two rows (front at y - 8, rear at y + 8), per wheel a damage tint sprite (fade
// 0x8002F904 -> 0x8002F908 by damage / 256) and a wear sprite in the wear stage's colour; all sprites of a row are
// centred on (x, row y) - the sheet's cells place the left / right tyre.
void Hud::TyrePanel(const HudFrame& f, int x, int y) {
    if (!f.tyrePanel || !haveTyreSprites_) return;
    for (int row = 0; row < 2; row++) {
        const int cy = y - 8 + row * 16;
        const int left = row * 2, right = row * 2 + 1;
        auto sprite = [&](const gt2::HudSpriteDesc& d, uint32_t color) {
            SpriteAt(x - (int(int16_t(d.w)) >> 1), cy - (int(int16_t(d.h)) >> 1), d, color);
            Mode(d.tpage);
        };
        sprite(tyreSprites_[1], FadeColor(tyreTintFrom_, tyreTintTo_, f.wheelDamage[size_t(left)], 256));
        sprite(tyreSprites_[0], WearStageColor(int(int8_t(f.wheelWearStage[size_t(left)]))));
        sprite(tyreSprites_[2], FadeColor(tyreTintFrom_, tyreTintTo_, f.wheelDamage[size_t(right)], 256));
        sprite(tyreSprites_[3], WearStageColor(int(int8_t(f.wheelWearStage[size_t(right)]))));
    }
}

// 0x8002D058's sibling 0x8002D12C: a licence medal time right-aligned at x + 3 and its marker, a 4 x 4 tile of the
// medal's colour at (x - 54, y - 6) over a black 6 x 6 one.
void Hud::MedalLine(int32_t ms, int x, int y, uint32_t colorWord) {
    std::vector<gt2::HudFontSprite> s;
    text_.TimeRight(gt2::FormatRaceTime(uint32_t(ms)), x + 3, y, 7, 5, 0, 2, s);
    Glyphs(s, kCaption);
    Tile(x - 54, y - 6, 4, 4, colorWord);
    Tile(x - 55, y - 7, 6, 6, 0);
}

// 0x8002D664 (x 160, y 96): the shell's message block. The glyphs' draw mode carries blend 1 (the context's page word
// gets bit 21 set), so the fading texts (colour word bit 25 below 31 frames) are additive.
void Hud::Messages(const HudFrame& f, int x, int y) {
    std::vector<gt2::HudFontSprite> s;
    auto emit = [&](uint32_t color) {
        for (gt2::HudFontSprite& g : s) g.tpage = uint16_t(g.tpage | 0x20);
        Glyphs(s, color);
        s.clear();
    };
    if (f.splitTimer > 0) {
        uint32_t color = 0x606060;
        if (f.splitB < f.splitA) color = 0x2E6010;
        if (f.splitA < f.splitB) color = 0x061060;
        if (f.timeInvalid) color = 0x0A1E50;
        int fade = 0;
        if (f.splitTimer < 0x1F) { color |= kSemi; fade = ((0x1E - f.splitTimer) * 0x80) / 0x1E; }
        if (f.splitA != 0xFFFFFFFFu) {
            const std::string text = SplitDifference(f.splitA, f.splitB);
            text_.Time(text, x - (text_.TimeWidth(text, 7, 5, 0) >> 1), y - 0x10, 7, 5, 0, 2, s); // 0x8006B49C
            emit(FadeColor(color, 0, fade));
        }
    }
    if (f.captionTimer > 0) {
        uint32_t timeColor = 0x1E5A78, captionColor = 0x285A78;
        if (f.timeInvalid) timeColor = 0x0A1E50;
        int fade = 0;
        if (f.captionTimer < 0x1F) { captionColor = 0x2285A78; timeColor |= kSemi; fade = ((0x1E - f.captionTimer) * 0x80) / 0x1E; }
        if (!f.caption.empty()) {
            text_.Text(f.caption, x - (text_.TextWidth(f.caption, 1) >> 1), y - 0x1A, 1, s); // 0x8006ADB4
            emit(FadeColor(captionColor, 0, fade));
        }
        const std::string time = gt2::FormatRaceTime(uint32_t(f.captionMs));
        large_.Time(time, x - (large_.TimeWidth(time, 8, 6, 0) >> 1), y, 8, 6, 0, 0, s);
        emit(FadeColor(timeColor, 0, fade));
        if (f.timeInvalid) {
            const std::string invalid = strings_.At(strings_.Find("Time Invalid !"));
            large_.Text(invalid, x - (large_.TextWidth(invalid, 0) >> 1), y + 0x10, 0, s);
            emit(FadeColor(timeColor, 0, fade));
        }
    }
    if (f.crashTimer > 0) {
        const std::string text = strings_.At(strings_.Find(f.crashKind == 2 ? "Out of Course" : "Crash !"));
        uint32_t color = 0x0A1E50;
        int fade = 0;
        if (f.crashTimer < 0x1F) { color = 0x20A1E50; fade = ((0x1E - f.crashTimer) * 0x80) / 0x1E; }
        large_.Text(text, x - (large_.TextWidth(text, 0) >> 1), y + 0x20, 0, s);
        emit(FadeColor(color, 0, fade));
    }
    if (f.raceFinished) { // our placement: the original's finish display is not ported
        const std::string finish = strings_.At(strings_.Find("Finish"));
        text_.Text(finish, 160 - text_.TextWidth(finish, 0) / 2, 60, 0, s);
        Glyphs(s, kCaption);
    }
}

std::vector<std::string> Hud::ListPrimitives() const {
    std::vector<std::string> out;
    char line[256];
    auto xy = [](int x, int y) { return "(" + std::to_string(x) + "," + std::to_string(y) + ")"; };
    for (auto p = packets_.rbegin(); p != packets_.rend(); ++p) {
        for (const Cmd& c : *p) {
            switch (c.kind) {
            case Cmd::kMode:
                std::snprintf(line, sizeof(line), "E1 tpage=%03X", unsigned(c.tpage));
                break;
            case Cmd::kSprite:
                std::snprintf(line, sizeof(line), "RECT %02X rgb=%06X xy=%s uv=(%d,%d) clut=%04X size=%s%s", c.semi ? 0x66u : 0x64u, c.color[0], xy(c.x[0], c.y[0]).c_str(),
                              c.u, c.v, unsigned(c.clut), xy(c.w, c.h).c_str(), c.semi ? " semi" : "");
                break;
            case Cmd::kTile:
                std::snprintf(line, sizeof(line), "RECT %02X rgb=%06X xy=%s size=%s%s", c.semi ? 0x62u : 0x60u, c.color[0], xy(c.x[0], c.y[0]).c_str(), xy(c.w, c.h).c_str(),
                              c.semi ? " semi" : "");
                break;
            case Cmd::kPoly:
                if (c.gouraud)
                    std::snprintf(line, sizeof(line), "POLY %02X quad gouraud%s rgb=%06X v0=%s rgb1=%06X v1=%s rgb2=%06X v2=%s rgb3=%06X v3=%s", c.semi ? 0x3Au : 0x38u,
                                  c.semi ? " semi" : "", c.color[0], xy(c.x[0], c.y[0]).c_str(), c.color[1], xy(c.x[1], c.y[1]).c_str(), c.color[2],
                                  xy(c.x[2], c.y[2]).c_str(), c.color[3], xy(c.x[3], c.y[3]).c_str());
                else
                    std::snprintf(line, sizeof(line), "POLY %02X quad%s rgb=%06X v0=%s v1=%s v2=%s v3=%s", c.semi ? 0x2Au : 0x28u, c.semi ? " semi" : "", c.color[0],
                                  xy(c.x[0], c.y[0]).c_str(), xy(c.x[1], c.y[1]).c_str(), xy(c.x[2], c.y[2]).c_str(), xy(c.x[3], c.y[3]).c_str());
                break;
            }
            out.push_back(line);
        }
    }
    return out;
}

// ---------------------------------------------------------------- frame

void Hud::Build(const HudFrame& f, float windowAspect, std::vector<DrawItem>& items) {
    const int dial = DialIndexFor(f.revLimitRpm);
    if (dial != uploadedDial_) {
        uploadedDial_ = dial;
        UploadBlock();
    }
    packets_.clear();
    quads_.clear();
    ViewHud(f);
    // 0x8002E818 (the dispatcher 0x800293D4 calls it for every game mode but 0): in a replay the caption "Replay" and the name of
    // the followed car's entry (captured attract race: RECT 64, colour 606060).
    if (f.replay && f.gameMode != 0) {
        visible_ = f.visibility.replay;
        std::vector<gt2::HudFontSprite> s;
        text_.Text(strings_.At(strings_.Find("Replay")), 16, 210, 0, s);
        text_.Text(f.carName, 16, 222, 0, s);
        Glyphs(s, kReplayText);
        visible_ = true;
    }
    Emit(windowAspect, items);
}

void Hud::Build2PFull(const HudFrame& f, int revLimitCar0, int revLimitCar1, float windowAspect, std::vector<DrawItem>& items) {
    const int dial = DialIndexFor(revLimitCar0), dial2 = DialIndexFor(revLimitCar1);
    if (dial != uploadedDial_ || dial2 != uploadedDial2_) { // the faces of both cars as the race start uploaded them (0x8002E390)
        uploadedDial_ = dial;
        uploadedDial2_ = dial2;
        UploadBlock();
    }
    packets_.clear();
    quads_.clear();
    ViewHud(f);
    Emit(windowAspect, items);
}

void Hud::ViewHud(const HudFrame& f) {
    // 0x8002E63C, in the original's order of insertion into the ordering table.
    anchor_ = kAnchorCentre;
    visible_ = f.visibility.countdown; StartDisplay(f);
    visible_ = f.visibility.warnings; Warning(f, 0xA0, 0xB6);
    visible_ = f.visibility.messages; Messages(f, 0xA0, 0x60);
    const bool full = !f.replay || f.replayView != 0;
    if (full) {
        anchor_ = kAnchorRight;
        visible_ = f.visibility.records; RecordPanel(f, 0x134, 0x10);
        visible_ = f.visibility.gauges; Needles(f, 0x114, 0xB4);
        visible_ = f.visibility.gauges; Tachometer(f, 0x114, 0xB4);
        visible_ = f.visibility.turbo; TurboGauge(f, 0xE0, 0xCC);
        visible_ = f.visibility.tyres; TyrePanel(f, 0x128, 0x7C);
        anchor_ = kAnchorLeft;
        visible_ = f.visibility.map;
        if (haveMap_ && (f.gameMode != 3 || f.licenseByte == 0)) CourseMapPanel(f, 0x10, 0x90);
    }
    anchor_ = kAnchorLeft;
    visible_ = f.visibility.lap; LapBlock(f, 0xC, 0x10);
    visible_ = true;
}

// 0x8002E908(view, car, camera) for car 0 then car 1: the 2 player Battle's HUD (game mode 0, the split screen of 0x800297F4),
// both halves in the one ordering table of the frame. Car 0 (the top half) also draws the start display 0x8002A19C (and the
// race-end display 0x8002B170, gt2view/race_overlay_screens.h); every call puts the grey line TILE (0, 119) 320 x 2 (0xC6C6C6)
// between the halves; then the routines of 0x8002E63C at the half's positions (car 1 = + 0x70 / + 0x6C down): warning (160,
// 80 / 192), messages (160, 48 / 156), [replay view] record (308, 16 / 124), needles and tachometer (276, 68 / 180), turbo (224,
// 92 / 204), tyre panel (296, 28 / 140), lap block (12, 16 / 124); no course map.
void Hud::Build2P(const HudFrame& top, const HudFrame& bottom, float windowAspect, std::vector<DrawItem>& items) {
    const int dial = DialIndexFor(top.revLimitRpm), dial2 = DialIndexFor(bottom.revLimitRpm);
    if (dial != uploadedDial_ || dial2 != uploadedDial2_) { // each car's face in its own slot (0x8002E390)
        uploadedDial_ = dial;
        uploadedDial2_ = dial2;
        UploadBlock();
    }
    packets_.clear();
    quads_.clear();
    for (int car = 0; car < 2; car++) {
        const HudFrame& f = car == 0 ? top : bottom;
        anchor_ = kAnchorCentre;
        if (car == 0) StartDisplay(f);
        anchor_ = kAnchorStretch;
        Tile(0, 0x77, 0x140, 2, 0xC6C6C6);
        anchor_ = kAnchorCentre;
        Warning(f, 0xA0, car ? 0xC0 : 0x50);
        Messages(f, 0xA0, car ? 0x9C : 0x30);
        const bool full = !f.replay || f.replayView != 0;
        if (full) {
            anchor_ = kAnchorRight;
            RecordPanel(f, 0x134, car ? 0x7C : 0x10);
            Needles(f, 0x114, car ? 0xB4 : 0x44);
            Tachometer(f, 0x114, car ? 0xB4 : 0x44);
            TurboGauge(f, 0xE0, car ? 0xCC : 0x5C);
            TyrePanel(f, 0x128, car ? 0x8C : 0x1C);
        }
        anchor_ = kAnchorLeft;
        LapBlock(f, 0xC, car ? 0x7C : 0x10);
    }
    Emit(windowAspect, items);
}

void Hud::Emit(float windowAspect, std::vector<DrawItem>& items) {
    // The GPU walks the slot from the last inserted packet: draw in reverse, with the draw mode state.
    uint16_t tpage = 0;
    for (auto p = packets_.rbegin(); p != packets_.rend(); ++p) {
        for (const Cmd& c : *p) {
            if (c.kind == Cmd::kMode) { tpage = c.tpage; continue; }
            Quad q;
            q.anchor = c.anchor;
            q.blend = c.semi ? uint32_t((tpage >> 5) & 3) : kBlendOpaque;
            if (c.kind == Cmd::kPoly) {
                // GPU quad v0 v1 v2 v3 (triangles 0-1-2, 1-2-3) -> our corners TL TR BR BL = v0 v1 v3 v2
                static constexpr int kMap[4] = {0, 1, 3, 2};
                for (int k = 0; k < 4; k++) {
                    q.x[k] = float(c.x[kMap[k]]);
                    q.y[k] = float(c.y[kMap[k]]);
                    q.u[k] = q.v[k] = 0;
                    q.color[k] = ColorOf(c.color[kMap[k]]);
                }
            } else {
                const float x0 = float(c.x[0]), y0 = float(c.y[0]), x1 = float(c.x[0] + c.w), y1 = float(c.y[0] + c.h);
                const float xs[4] = {x0, x1, x1, x0}, ys[4] = {y0, y0, y1, y1};
                std::copy(xs, xs + 4, q.x);
                std::copy(ys, ys + 4, q.y);
                for (int k = 0; k < 4; k++) q.color[k] = ColorOf(c.color[0]);
                if (c.kind == Cmd::kSprite) {
                    const float us[4] = {float(c.u), float(c.u + c.w), float(c.u + c.w), float(c.u)};
                    const float vs[4] = {float(c.v), float(c.v), float(c.v + c.h), float(c.v + c.h)};
                    std::copy(us, us + 4, q.u);
                    std::copy(vs, vs + 4, q.v);
                    q.textured = true;
                    q.page = uint32_t((tpage & 0xF) * 64) | ((kRowBase + uint32_t(((tpage >> 4) & 1) * 256)) << 16);
                    q.clut = uint32_t((c.clut & 0x3F) * 16) | ((kRowBase + uint32_t(c.clut >> 6)) << 16);
                } else {
                    std::fill(q.u, q.u + 4, 0.0f);
                    std::fill(q.v, q.v + 4, 0.0f);
                }
            }
            quads_.push_back(q);
        }
    }

    // ---- quads -> vertices (anchored, uniformly scaled), in list order (painter's order like the original)
    if (quads_.size() * 6 > kVertexLimit) quads_.resize(kVertexLimit / 6);
    // Uniform scale (window height = 1): 1/240 per frame pixel, or aspect/320 when the window is narrower than 4:3;
    // x from the quad's anchor edge, y centred - nothing is cut or stretched at any aspect ratio.
    const float scale = std::min(1.0f / float(kFrameHeight), windowAspect / float(kFrameWidth));
    auto screenX = [&](float x, int anchor) {
        const float px = anchor == kAnchorLeft ? x * scale
                         : anchor == kAnchorRight ? windowAspect - (float(kFrameWidth) - x) * scale
                         : anchor == kAnchorStretch ? x * windowAspect / float(kFrameWidth) // across the window (the 2P split line)
                                                  : windowAspect * 0.5f + (x - float(kFrameWidth) * 0.5f) * scale;
        return px / windowAspect * 2.0f - 1.0f;
    };
    std::vector<SceneVertex> vertices;
    vertices.reserve(quads_.size() * 6);
    static constexpr int kCorners[6] = {0, 1, 2, 0, 2, 3};
    for (const Quad& q : quads_) {
        for (int c : kCorners) {
            SceneVertex v{};
            v.pos[0] = screenX(q.x[c], q.anchor);
            v.pos[1] = (0.5f + (q.y[c] - float(kFrameHeight) * 0.5f) * scale) * 2.0f - 1.0f;
            v.pos[2] = 1.0f; // reversed Z: in front of the scene; equal depth lets later quads cover earlier ones
            v.texel[0] = q.u[c];
            v.texel[1] = q.v[c];
            for (int j = 0; j < 3; j++) v.color[j] = q.color[c][size_t(j)] / 255.0f;
            v.page = q.page;
            v.clut = q.clut;
            v.flags = q.textured ? kTextured | kClampTextureRect : 0u; // 4-bit pages: depth bits 8-9 = 0
            if(q.textured && q.page == (uint32_t(gt2::CourseMap::kVramX) | (kRowBase << 16)) &&
               q.clut == (uint32_t(gt2::CourseMap::kClutX) | ((kRowBase+gt2::CourseMap::kClutY)<<16))) v.flags |= kUiMap;
            vertices.push_back(v);
        }
    }
    renderer_.ApplyHdUi(vertices);
    renderer_.SetVertices(kVertexBase, vertices);
    size_t first = 0;
    while (first < quads_.size()) {
        size_t last = first;
        while (last + 1 < quads_.size() && quads_[last + 1].blend == quads_[first].blend) last++;
        DrawItem item;
        item.firstVertex = kVertexBase + uint32_t(first * 6);
        item.vertexCount = uint32_t((last - first + 1) * 6);
        item.blend = quads_[first].blend;
        const float identity[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        std::copy(identity, identity + 16, item.mvp);
        items.push_back(item);
        first = last + 1;
    }
}

TwoPlayerHudRam TwoPlayerHudFromRam(const uint8_t* ram, const gt2::ExeProfile& profile, const gt2::GuestImage& raceOverlay, const gt2::HudStrings& strings,
                                    bool tyrePanelSeen) {
    auto at = [&](uint32_t sim) { return profile.Race(sim) & 0x1FFFFF; };
    auto u8 = [&](uint32_t a) { return int(ram[at(a)]); };
    auto u16 = [&](uint32_t a) { const uint32_t o = at(a); return int(uint16_t(ram[o] | ram[o + 1] << 8)); };
    auto s16 = [&](uint32_t a) { return int(int16_t(uint16_t(u16(a)))); };
    auto s32 = [&](uint32_t a) { return int32_t(uint32_t(u16(a)) | uint32_t(u16(a + 2)) << 16); };
    TwoPlayerHudRam r;
    const uint32_t overlayWord = uint32_t(ram[0x100F4] | ram[0x100F5] << 8 | ram[0x100F6] << 16 | uint32_t(ram[0x100F7]) << 24);
    if (!(u8(0x801D5866u) == 0 && u8(0x800AF231u) >= 2 && overlayWord == raceOverlay.Get<uint32_t>(0x800100F4u))) return r;
    r.valid = true;
    const uint32_t view = 0x801FF8B8u; // the race task's view object: split flag + 0x2EA, camera objects + 0xC4 + i * 0x110
    r.split = u8(view + 0x2EA) != 0;
    r.followed = uint32_t(std::min(1, u8(view + 0xC4 + 0x10C)));
    r.courseIndex = u8(0x800AF230u);
    for (uint32_t k = 0; k < 2; k++) {
        const uint32_t car = 0x800A9688u + k * 0xB40u, results = k == 0 ? 0x801D5E88u : 0x801DA3A0u;
        HudFrame& hf = r.frames[k];
        hf.gameMode = 0;
        hf.faceSlot = s16(car + 0x880);
        hf.replay = u8(0x800A951Cu) != 0;
        hf.replayView = u8(view + 0xC4 + (r.split ? k : 0) * 0x110 + 0x107);
        hf.courseMap = u8(0x801C9991u) != 0; // career + 0xB1 (0x80029064)
        for (uint32_t i = 0; i < 2; i++) {
            const uint32_t c = 0x800A9688u + i * 0xB40u;
            hf.mapCars.push_back({int16_t(s16(c + 0x832)), int16_t(s16(c + 0x83A)), u8(c + 0x0E) != 0});
        }
        hf.lapCount = u8(0x801D586Bu);
        hf.lap = s16(car + 0x634);
        hf.position = int(int8_t(u8(car + 0x77C)));
        hf.finished = u8(car + 0x728) != 0;
        const int32_t clock = s32(0x80046F64u) / 3;
        hf.totalMs = hf.finished ? s32(results + 0xF8) : clock;
        hf.lapMs = clock - s32(car + 0x7AC);
        hf.subFrame = u8(0x8002F864u) & 0xF;
        for (int i = 0; i < s16(results + 4) && i < 10; i++) hf.laps.push_back(s32(results + 8 + uint32_t(i) * 20));
        hf.resultsLapNumber = s16(results + 2);
        hf.bestLapMs = s32(results + 0xD0);
        hf.bestLapSpeed = s16(results + 0xD0 + 0x10);
        hf.rpm = s16(car + 0x6D8);
        hf.revLimitRpm = u16(car + 0x134);
        hf.redlineRpm = u16(car + 0x3C2);
        hf.speedReadout = u16(car + 0x6DA);
        hf.gear = u8(car + 0x644);
        hf.clutchEngaged = u8(car + 0x645) == 1;
        hf.turbo = s16(car + 0x154);
        hf.boost = s16(car + 0x76E);
        hf.startTimer = s16(0x800AF224u);
        hf.messageCode = u8(car + 0x790);
        hf.timeInvalid = u8(car + 0xA8C) != 0;
        hf.crashKind = u8(car + 0xA8D);
        hf.captionTimer = s16(car + 0xA8E);
        hf.splitTimer = s16(car + 0xA90);
        hf.crashTimer = s16(car + 0xA92);
        hf.captionMs = s32(car + 0xA94);
        hf.caption = strings.At(uint32_t(s32(car + 0xA98)));
        hf.splitA = uint32_t(s32(car + 0xA9C));
        hf.splitB = uint32_t(s32(car + 0xAA0));
        hf.tyrePanel = tyrePanelSeen || s32(0x80046F48u) != 0; // 0x8002DE8C: the tyre wear word (0x800418E8 class 2 needs race block + 4)
        for (uint32_t w = 0; w < 4; w++) {
            hf.wheelDamage[w] = u8(car + 0x4AE + w * 0x68);
            hf.wheelWearStage[w] = int(int8_t(u8(car + 0x4CB + w * 0x68)));
        }
    }
    return r;
}

} // namespace gt2view
