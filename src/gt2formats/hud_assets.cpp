#include "gt2formats/hud_assets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace gt2 {
namespace {

uint16_t U16(std::span<const uint8_t> d, size_t o) {
    if (o + 2 > d.size()) throw std::runtime_error("hud assets: read out of bounds");
    return uint16_t(d[o] | (d[o + 1] << 8));
}
uint32_t U32(std::span<const uint8_t> d, size_t o) { return uint32_t(U16(d, o)) | (uint32_t(U16(d, o + 2)) << 16); }

// One TIM block header {u32 length, u16 x, y, w, h} + the words that follow it. `lastBlock`: the length is not checked against
// the size - the EXE's TIM upload (0x8007BCA0 -> 0x8007BBD4) loads w x h words from + 12 and uses the length only to step to the
// next block, so the last block's length is never read (the 2 player courses' crsmap/*_2p.tim image blocks claim 0x240C bytes
// for a 0x120C-byte block).
struct TimBlock { uint32_t length; int x, y, w, h; size_t data; };
TimBlock ReadBlock(std::span<const uint8_t> d, size_t o, bool lastBlock = false) {
    TimBlock b{U32(d, o), U16(d, o + 4), U16(d, o + 6), U16(d, o + 8), U16(d, o + 10), o + 12};
    if (b.data + size_t(b.w) * size_t(b.h) * 2 > d.size() || (!lastBlock && (b.length < 12 || size_t(b.w) * size_t(b.h) * 2 != b.length - 12)))
        throw std::runtime_error("hud assets: TIM block does not fit");
    return b;
}
std::vector<uint16_t> BlockWords(std::span<const uint8_t> d, const TimBlock& b) {
    std::vector<uint16_t> words(size_t(b.w) * size_t(b.h));
    for (size_t i = 0; i < words.size(); i++) words[i] = U16(d, b.data + i * 2);
    return words;
}

} // namespace

// ---------------------------------------------------------------- race font

int RaceFont::GlyphTop(char c) const {
    const RaceFontGlyph& g = glyphs[uint8_t(c)];
    // Bottom-aligned on the line (captured: 'L' h9 at +0, 'a' h7 at +2, ':' h8 at +1); the descenders p / y (h8)
    // were captured one row lower (+2). g / j / q are assumed to be descenders too.
    const bool descender = c != 0 && std::strchr("gjpqy", c) != nullptr;
    return std::max(0, kLineHeight - g.height) + (descender ? 1 : 0); // '[' / ']' (h10) were captured at the line top
}

int RaceFont::Advance(char a, char b) const {
    const RaceFontGlyph& ga = glyphs[uint8_t(a)];
    const RaceFontGlyph& gb = glyphs[uint8_t(b)];
    if (!ga.present) return 0;
    const int topA = GlyphTop(a), topB = gb.present ? GlyphTop(b) : 0;
    int advance = -1;
    for (int row = 0; row < kLineHeight + 2; row++) {
        const int ra = row - topA, rb = row - topB;
        if (ra < 0 || ra >= ga.height || rb < 0 || rb >= gb.height || !gb.present) continue;
        int right = -1, left = -1;
        for (int i = 0; i < ga.width; i++) if (Ink(ga, i, ra)) right = i;
        for (int i = gb.width - 1; i >= 0; i--) if (Ink(gb, i, rb)) left = i;
        if (right < 0 || left < 0) continue;
        advance = std::max(advance, right + 1 - left + 1);
    }
    return advance >= 0 ? advance : ga.width - 1; // no body overlap (spaces, end of text): the glyph cell minus the outline
}

int RaceFont::TextWidth(const std::string& text) const {
    int x = 0;
    for (size_t i = 0; i < text.size(); i++) {
        if (i + 1 < text.size()) x += Advance(text[i], text[i + 1]);
        else x += glyphs[uint8_t(text[i])].width;
    }
    return x;
}

RaceFont LoadRaceFont(const GtfsVolume& vol, const GuestImage& exe) {
    RaceFont font;
    const std::vector<uint8_t> raw = vol.Read("font/racefont.dat");
    if (raw.size() != size_t(RaceFont::kWords) * RaceFont::kRows * 2) throw std::runtime_error("font/racefont.dat: unexpected size");
    font.words.resize(size_t(RaceFont::kWords) * RaceFont::kRows);
    for (size_t i = 0; i < font.words.size(); i++) font.words[i] = U16(raw, i * 2);
    for (uint32_t c = RaceFont::kGlyphFirst; c < RaceFont::kGlyphCount; c++) {
        const uint32_t at = exe.Sim(RaceFont::kGlyphTableAddress) + c * 4; // the table of the executable's build
        const uint8_t u = exe.Get<uint8_t>(at), v = exe.Get<uint8_t>(at + 1);
        const uint16_t packed = exe.Get<uint16_t>(at + 2);
        RaceFontGlyph& g = font.glyphs[c];
        g.present = (packed & 0x1000) != 0;
        if (!g.present) continue;
        g.x = uint16_t(u + ((packed & 0x2000) ? 256 : 0));
        g.y = v;
        g.width = uint8_t((packed & 0x1F) * 2);
        g.height = uint8_t((packed >> 5) & 0xF);
        if (g.x + g.width > 512 || g.y + g.height > 256 || g.height == 0 || g.height > RaceFont::kLineHeight + 1)
            throw std::runtime_error("race font: glyph table entry outside the font image");
    }
    if (!font.glyphs['0'].present || !font.glyphs['A'].present) throw std::runtime_error("race font: glyph table has no digits / letters");
    return font;
}

// ---------------------------------------------------------------- gauge sheet and dial faces

const HudSheet::Dial& HudSheet::DialFor(int revLimitRpm) const {
    const Dial* best = nullptr;
    for (const Dial& d : dials)
        if (d.limitRpm >= revLimitRpm && (!best || d.limitRpm < best->limitRpm)) best = &d;
    return best ? *best : dials.back();
}

HudSheet LoadHudSheet(const GtfsVolume& vol) {
    const std::vector<uint8_t> raw = vol.Read("arcade/game_status_files");
    const uint32_t count = U32(raw, 0);
    if (count != 1 + HudSheet::kDialCount) throw std::runtime_error("game_status_files: unexpected member count");
    std::vector<size_t> offsets(count + 1, raw.size());
    for (uint32_t i = 0; i < count; i++) offsets[i] = U32(raw, 4 + i * 4);
    HudSheet sheet;
    for (uint32_t i = 0; i < count; i++) {
        size_t o = offsets[i];
        if (U32(raw, o) != 0x10) throw std::runtime_error("game_status_files: member is not a TIM");
        const uint32_t flags = U32(raw, o + 4);
        if ((flags & 7) != 0) throw std::runtime_error("game_status_files: member is not 4-bit");
        o += 8;
        if (i == 0) {
            if (flags & 8) throw std::runtime_error("game_status_files: the sheet has a CLUT block");
            const TimBlock image = ReadBlock(raw, o);
            if (image.w != HudSheet::kSheetWords || image.h != HudSheet::kSheetRows) throw std::runtime_error("game_status_files: sheet size");
            sheet.sheet = BlockWords(raw, image);
            if (o + image.length != offsets[1]) throw std::runtime_error("game_status_files: sheet member length");
            continue;
        }
        if (!(flags & 8)) throw std::runtime_error("game_status_files: dial face without a CLUT");
        const TimBlock clut = ReadBlock(raw, o);
        if (clut.w != 16 || clut.h != 1) throw std::runtime_error("game_status_files: dial CLUT size");
        const TimBlock image = ReadBlock(raw, o + clut.length);
        if (image.w != HudSheet::kDialWords || image.h != HudSheet::kDialRows) throw std::runtime_error("game_status_files: dial face size");
        if (o + clut.length + image.length != offsets[i + 1]) throw std::runtime_error("game_status_files: dial member length");
        static constexpr int kLimits[HudSheet::kDialCount] = {6000, 7000, 8000, 9000, 10000, 11000, 12000, 14000, 16000, 18000};
        HudSheet::Dial dial;
        dial.limitRpm = kLimits[i - 1];
        const std::vector<uint16_t> clutWords = BlockWords(raw, clut);
        std::copy(clutWords.begin(), clutWords.end(), dial.clut.begin());
        dial.words = BlockWords(raw, image);
        sheet.dials.push_back(std::move(dial));
    }
    return sheet;
}

// ---------------------------------------------------------------- strings

std::string HudStrings::At(uint32_t token) const {
    if (token < base || token - base >= bytes.size()) return {};
    const size_t o = token - base;
    const auto end = std::find(bytes.begin() + std::ptrdiff_t(o), bytes.end(), uint8_t(0));
    return std::string(bytes.begin() + std::ptrdiff_t(o), end);
}

uint32_t HudStrings::Find(const std::string& text) const {
    for (size_t o = 0; o < bytes.size();) {
        const auto end = std::find(bytes.begin() + std::ptrdiff_t(o), bytes.end(), uint8_t(0));
        const size_t len = size_t(end - (bytes.begin() + std::ptrdiff_t(o)));
        if (len == text.size() && std::memcmp(&bytes[o], text.data(), len) == 0) return base + uint32_t(o);
        o += len + 1;
    }
    return 0;
}

HudStrings LoadHudStrings(const GtfsVolume& vol) {
    HudStrings s;
    s.bytes = vol.Read(".text/data-race.txd");
    if (s.bytes.size() < 16 || std::memcmp(s.bytes.data(), "Lap\0", 4) != 0) throw std::runtime_error(".text/data-race.txd: unexpected content");
    return s;
}

HudStrings LoadHudStrings(const GtfsVolume& vol, uint8_t language) { return LoadHudStrings(vol, language, kRaceTextBlockSize); }

HudStrings LoadHudStrings(const GtfsVolume& vol, uint8_t language, uint32_t blockSize) {
    const std::vector<uint8_t> file = vol.Read(".text/data-race.txd");
    const size_t at = size_t(language) * blockSize;
    if (at + blockSize > file.size()) throw std::runtime_error(".text/data-race.txd: no block for language " + std::to_string(language));
    HudStrings s;
    s.bytes.assign(file.begin() + std::ptrdiff_t(at), file.begin() + std::ptrdiff_t(at + blockSize));
    return s;
}

// ---------------------------------------------------------------- text engine

uint32_t HudFont::Word(uint32_t address) const {
    if (address < base_ || address - base_ + 4 > bytes_.size()) throw std::runtime_error("hud font: table read outside the copied range");
    const size_t o = address - base_;
    return uint32_t(bytes_[o]) | uint32_t(bytes_[o + 1]) << 8 | uint32_t(bytes_[o + 2]) << 16 | uint32_t(bytes_[o + 3]) << 24;
}

uint32_t HudFont::GlyphWord(uint8_t c, int word) const { return Word(glyphTable_ + uint32_t(c) * 8 + uint32_t(word) * 4); }

void HudFont::Glyph(uint32_t code, int x, int y, std::vector<HudFontSprite>& out) const {
    const uint32_t w0 = GlyphWord(uint8_t(code & 0xFF), 0), w1 = GlyphWord(uint8_t(code & 0xFF), 1);
    if ((w0 & 0x3FC0) == 0) return; // no sprite (space)
    if (code & kCentre) x += int((w0 >> 17) & 0x3F);
    const uint32_t s = Word(glyphTable_ + 0x818 + ((w0 & 0x3FC0) >> 6) * 4);
    const int halfWidth = int((s >> 16) & 0x1F);
    if (code & kRightAlign) x -= halfWidth;
    const int top = y - int((w1 >> 6) & 0x3F);
    HudFontSprite g;
    g.x = int(int16_t(uint16_t(x + int(w1 & 0x3F))));
    g.y = top;
    g.w = halfWidth * 2;
    g.h = int((s >> 21) & 0x3F);
    g.u = int(s & 0xFF);
    g.v = int((s >> 8) & 0xFF);
    g.tpage = uint16_t(tpage + (s >> 29));
    g.clut = uint16_t(clutBase + ((s >> 27) & 3));
    out.push_back(g);
    const uint32_t accent = (w0 >> 14) & 7;
    if (accent != 0) {
        const uint32_t a = Word(glyphTable_ + 0x7FC + accent * 4);
        HudFontSprite d;
        d.x = x + int((w1 >> 12) & 0x3F);
        d.y = top - int((w1 >> 18) & 0x1F);
        d.w = int((a >> 15) & 0x3E);
        d.h = int((a >> 21) & 0x3F);
        d.u = int(a & 0xFF);
        d.v = int((a >> 8) & 0xFF);
        d.tpage = uint16_t(tpage + (a >> 29));
        d.clut = uint16_t(clutBase + ((a >> 27) & 3));
        out.push_back(d);
    }
}

int HudFont::Advance(uint8_t a, uint8_t b) const {
    const uint32_t header = Word(kerningTable_);
    const uint32_t wa = GlyphWord(a, 0), wb = GlyphWord(b, 1);
    const int base = int(gap) + int(wa & 0x3F);
    int kern;
    if (int32_t(wa) < 0) {
        kern = int((uint32_t(int32_t(wa) >> 23)) & 0x3F);
    } else if (int32_t(wb) < 0) {
        kern = int((uint32_t(int32_t(wb) >> 23)) & 0x3F);
    } else {
        const uint32_t rowClass = (wa >> 23) & 0xFF, columnClass = (wb >> 23) & 0xFF;
        const uint32_t bits = (header >> 16) & 0xFF, stride = header & 0xFF;
        uint32_t bitOffset = columnClass * 4;
        if (bits & 1) bitOffset = columnClass * 5;
        if (bits & 2) bitOffset += columnClass * 2;
        const uint32_t word = Word(kerningTable_ + 4 + rowClass * stride + (bitOffset >> 3)); // lwl / lwr: unaligned
        kern = int(uint32_t(int32_t(word) >> (bitOffset & 7)) & ((1u << bits) - 1u));
    }
    return base - kern;
}

int HudFont::Text(const std::string& s, int x, int y, int spacing, std::vector<HudFontSprite>& out) const {
    int pen = x;
    for (size_t i = 0; i < s.size(); i++) {
        Glyph(uint8_t(s[i]), pen, y, out);
        pen += Advance(uint8_t(s[i]), i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0)) + spacing;
    }
    return pen - x - spacing;
}

int HudFont::TextWidth(const std::string& s, int spacing) const {
    int w = 0;
    for (size_t i = 0; i < s.size(); i++) w += Advance(uint8_t(s[i]), i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0)) + spacing;
    return w - spacing;
}

int HudFont::TextRight(const std::string& s, int right, int y, int spacing, std::vector<HudFontSprite>& out) const {
    const int w = TextWidth(s, spacing);
    Text(s, right - w, y, spacing, out);
    return w;
}

namespace {
bool Narrow(char c) { return c == ':' || c == '.'; }
bool Sign(char c) { return c == '+' || c == '-'; }
} // namespace

int HudFont::TimeWidth(const std::string& s, int advance, int narrow, int signFlag) const {
    int w = 0;
    bool afterNarrow = false;
    for (size_t i = 0; i < s.size(); i++) {
        const char next = i + 1 < s.size() ? s[i + 1] : '\0';
        w += advance;
        if (afterNarrow || Narrow(next)) w = w - advance + narrow;
        if (signFlag != 0 && Sign(s[i])) w += 1;
        afterNarrow = Narrow(next);
    }
    return w;
}

int HudFont::Time(const std::string& s, int x, int y, int advance, int narrow, int signFlag, int dotShift, std::vector<HudFontSprite>& out) const {
    int pen = x - (cell >> 1);
    bool afterNarrow = false;
    for (size_t i = 0; i < s.size(); i++) {
        const char c = s[i];
        if (c == ' ') { pen += advance; continue; }
        Glyph(uint8_t(c) | kCentre, c == '.' ? pen + dotShift : pen, y, out);
        const char next = i + 1 < s.size() ? s[i + 1] : '\0';
        pen += advance;
        if (afterNarrow || Narrow(next)) pen = pen - advance + narrow;
        afterNarrow = Narrow(next);
        if (signFlag != 0 && (Sign(c) || c == 'O')) pen += 1;
    }
    return pen - x + (advance - narrow);
}

int HudFont::TimeRight(const std::string& s, int right, int y, int advance, int narrow, int signFlag, int dotShift, std::vector<HudFontSprite>& out) const {
    const int w = TimeWidth(s, advance, narrow, signFlag);
    return Time(s, right - w, y, advance, narrow, signFlag, dotShift, out);
}

int HudFont::NumberWidth(const std::string& s, int spacing, int digitExtra) const {
    int w = 0;
    for (size_t i = 0; i < s.size(); i++) {
        const uint8_t c = uint8_t(s[i]);
        if (uint32_t(c) - 0x30u < 10u) w += spacing + cell + digitExtra;
        else w += Advance(c, i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0)) + spacing;
    }
    return w - spacing;
}

int HudFont::Number(const std::string& s, int x, int y, int spacing, int digitShift, int digitExtra, std::vector<HudFontSprite>& out) const {
    int pen = x;
    for (size_t i = 0; i < s.size(); i++) {
        const uint8_t c = uint8_t(s[i]);
        if (uint32_t(c) - 0x30u < 10u) {
            Glyph(c | kCentre, pen + digitShift - (cell >> 1), y, out);
            pen += spacing + cell + digitExtra;
        } else {
            Glyph(c, pen, y, out);
            pen += Advance(c, i + 1 < s.size() ? uint8_t(s[i + 1]) : uint8_t(0)) + spacing;
        }
    }
    return pen - x - spacing;
}

int HudFont::NumberRight(const std::string& s, int right, int y, int spacing, int digitShift, int digitExtra, std::vector<HudFontSprite>& out) const {
    const int w = NumberWidth(s, spacing, digitExtra);
    return Number(s, right - w, y, spacing, digitShift, digitExtra, out);
}

HudFont LoadHudFont(const GuestImage& exe, uint32_t descriptor) {
    HudFont f;
    f.glyphTable_ = exe.Get<uint32_t>(descriptor);
    f.kerningTable_ = exe.Get<uint32_t>(descriptor + 4);
    f.cell = exe.Get<uint8_t>(descriptor + 8);
    f.gap = exe.Get<uint8_t>(descriptor + 9);
    // 0x8007DC40(-1) takes the page word 0x80093148; in the race it is 6 (captured RAM and draw lists: the glyphs use
    // tpage 6 / 7 and CLUT 0x001A), i.e. the page of the race font's upload at (384, 0) - the file's initial word is
    // not that value, the race sets it when it loads the font.
    const uint16_t page = uint16_t(RaceFont::kVramX / 64 + (RaceFont::kVramY / 256) * 16);
    f.tpage = page;
    f.clutBase = uint16_t((page & 0xF) * 4 + (page & 0x10) * 0x400);
    // Copy the glyph table, its sprite tables and the kerning table (header + rows) into one range.
    const uint32_t kernHeader = exe.Get<uint32_t>(f.kerningTable_);
    const uint32_t kernEnd = f.kerningTable_ + 4 + 256 * (kernHeader & 0xFF) + 8;
    const uint32_t first = std::min(f.glyphTable_, f.kerningTable_);
    const uint32_t last = std::min(std::max(f.glyphTable_ + 0x818 + 256 * 4, kernEnd), exe.End());
    f.base_ = first;
    const uint8_t* p = exe.At(first, last - first);
    f.bytes_.assign(p, p + (last - first));
    if (((kernHeader >> 16) & 0xFF) < 1 || ((kernHeader >> 16) & 0xFF) > 16) throw std::runtime_error("hud font: unexpected kerning table header");
    return f;
}

std::string FormatRaceTime(uint32_t ms) {
    char text[16];
    if (ms < 3600000u) {
        const uint32_t minutes = ms / 60000, seconds = (ms % 60000) / 1000, millis = (ms % 60000) % 1000;
        std::snprintf(text, sizeof(text), "%u:%02u.%03u", minutes, seconds, millis);
    } else if (ms < 360000000u) {
        const uint32_t hours = ms / 3600000, rest = ms % 3600000;
        std::snprintf(text, sizeof(text), "%u:%02u:%02u.%u", hours, rest / 60000, (rest % 60000) / 1000, (rest % 1000) / 100);
    } else {
        return "--:--:---";
    }
    return text;
}

std::string FormatRaceSpeed(uint32_t readout) {
    readout = std::min(readout, 99999u);
    char text[16];
    std::snprintf(text, sizeof(text), "%u.%u", readout / 100, readout % 100);
    return text;
}

HudTables LoadHudTables(const GuestImage& o) {
    auto desc = [&](uint32_t a) {
        HudSpriteDesc d;
        d.u = o.Get<uint8_t>(a);
        d.v = o.Get<uint8_t>(a + 1);
        d.clut = o.Get<uint16_t>(a + 2);
        d.w = o.Get<uint16_t>(a + 4);
        d.h = o.Get<uint16_t>(a + 6);
        d.tpage = o.Get<uint16_t>(a + 8);
        return d;
    };
    HudTables t;
    auto at = [&](uint32_t simAddress) { return o.Sim(simAddress); }; // the overlay's build (gt2formats/exe_profile.h)
    for (uint32_t i = 0; i < t.faces.size(); i++) t.faces[i] = desc(at(0x8002F630u) + i * 12);
    for (uint32_t i = 0; i < t.speedDigits.size(); i++) t.speedDigits[i] = desc(at(0x8002F678u) + i * 12);
    t.unitKmh = desc(at(0x8002F6F0u));
    t.unitMph = desc(at(0x8002F6FCu));
    for (uint32_t i = 0; i < t.strip.size(); i++) t.strip[i] = desc(at(0x8002F708u) + i * 12);
    for (uint32_t i = 0; i < t.badges.size(); i++) t.badges[i] = desc(at(0x8002F798u) + i * 12);
    for (uint32_t i = 0; i < t.turbo.size(); i++) t.turbo[i] = desc(at(0x8002F7E0u) + i * 12);
    for (uint32_t i = 0; i < 10; i++) {
        t.dialLimits[i] = o.Get<int8_t>(at(0x8002F868u) + i);
        t.gearGlyph[i] = o.Get<int8_t>(at(0x8002F874u) + i);
    }
    for (uint32_t i = 0; i < t.needles.size(); i++) {
        const uint32_t a = at(0x8002F880u) + i * 24;
        t.needles[i] = {o.Get<uint32_t>(a), o.Get<uint32_t>(a + 4), o.Get<int32_t>(a + 8), o.Get<int32_t>(a + 12), o.Get<int32_t>(a + 16), o.Get<int32_t>(a + 20)};
    }
    t.barHeight = o.Get<int16_t>(at(0x8002F8E6u));
    t.barColor0 = o.Get<uint32_t>(at(0x8002F8E8u));
    t.barColor1 = o.Get<uint32_t>(at(0x8002F8ECu));
    if (t.faces[0].w != 80 || t.dialLimits[0] != 6 || t.gearGlyph[0] != 10 || t.barHeight != 5)
        throw std::runtime_error("hud tables: the race overlay's HUD tables are not where US v1.2 has them");
    return t;
}

CourseMap LoadCourseMap(const GtfsVolume& vol, const std::string& courseName) {
    const std::vector<uint8_t> raw = vol.Read("crsmap/" + courseName + ".tim");
    if (U32(raw, 0) != 0x10 || U32(raw, 4) != 8) throw std::runtime_error("crsmap: not a 4-bit TIM with CLUT");
    const TimBlock clut = ReadBlock(raw, 8);
    if (clut.w != 16 || clut.h != 1) throw std::runtime_error("crsmap: CLUT size");
    const TimBlock image = ReadBlock(raw, 8 + clut.length, true);
    if (image.w != CourseMap::kWords || image.h != CourseMap::kRows) throw std::runtime_error("crsmap: image size");
    CourseMap m;
    const std::vector<uint16_t> c = BlockWords(raw, clut);
    std::copy(c.begin(), c.end(), m.clut.begin());
    m.words = BlockWords(raw, image);
    return m;
}

} // namespace gt2
