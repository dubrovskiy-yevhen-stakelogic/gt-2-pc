#include "game/shell/key_config.h"

#include <algorithm>
#include <cstring>

#include "game/shell/title_draw.h"

namespace gt2::shell {

namespace {

constexpr uint32_t kHeldStart = 0x10000; // generic Start (held: the D-pad assigns)
constexpr uint32_t kChoose = 0xA00;      // Cross / Circle
constexpr uint32_t kKeyPages = 0x2D;     // pad block + 0x2D + table * 4: the list's bytes + 4 .. + 7
constexpr uint32_t kHoldStartText = 0x801BA196u, kDefaultText = 0x801BA0F6u, kExitText = 0x801BA0EEu;

bool IsAxis(uint8_t entry) { return int8_t(entry) < 0; }

// 0x8006BA48: the blinking arrow (POLY_F3, semi-transparent), as the options' page switcher draws it.
void AddArrow(MenuOtSlot& ot, int x, int y, int dir, int size, int phase) {
    int k = 40 - phase;
    if (k > 10) k = 10;
    if (k < 0) k = 0;
    const uint32_t r = uint32_t(k * 0xFF) / 10;
    MenuPrim p;
    p.kind = MenuPrim::kPolyF4;
    p.semi = true;
    p.x[0] = int16_t(x + dir), p.y[0] = int16_t(y);
    p.x[1] = int16_t(x), p.y[1] = int16_t(y + size);
    p.x[2] = int16_t(x), p.y[2] = int16_t(y - size);
    p.x[3] = p.x[2], p.y[3] = p.y[2];
    p.colour[0] = r | (r >> 1) << 8;
    ot.Add(p);
}

// 0x80081478 + the E1 of the record: an opaque SPRT of an icon record centred on (x, y), modulated by grey `level`.
void AddIcon(MenuOtSlot& ot, const KeyConfigData& data, int index, int x, int y, int level) {
    if (index < 0 || index >= KeyConfigData::kIconCount) return; // (the original reads the record before the table)
    const KeyConfigData::Icon& icon = data.icons[size_t(index)];
    MenuPrim p;
    p.kind = MenuPrim::kSprite;
    p.x[0] = int16_t(x - (int16_t(icon.w) >> 1)), p.y[0] = int16_t(y - (int16_t(icon.h) >> 1));
    p.w = int16_t(icon.w), p.h = int16_t(icon.h);
    p.u = icon.u, p.v = icon.v;
    p.clut = icon.clut;
    p.tpage = uint16_t(icon.tpage | 0x20);
    const uint32_t g = uint32_t(level) & 0xFF;
    p.colour[0] = g | g << 8 | g << 16;
    ot.Add(p);
    ot.DrawMode(uint16_t(icon.tpage | 0x20));
}

} // namespace

KeyConfigData KeyConfigData::Read(const GuestImage& ovl1, const GuestImage& exe) {
    KeyConfigData d;
    for (uint32_t r = 0; r < kRows; r++) {
        d.rowFunction[r] = ovl1.Get<int16_t>(kRowFunctions + r * 2);
        d.rowLabel[r] = ovl1.Get<uint32_t>(kRowLabels + r * 4);
    }
    for (uint32_t p = 0; p < 9; p++) {
        d.presets[p][0] = ovl1.Get<uint8_t>(kPresets + p * 4);
        d.presets[p][1] = ovl1.Get<uint8_t>(kPresets + p * 4 + 2);
    }
    for (uint32_t t = 0; t < 3; t++)
        for (uint32_t k = 0; k < 11; k++) d.defaults[t][k] = exe.Get<uint8_t>(exe.Sim(kDefaults) + t * 12 + k);
    for (uint32_t i = 0; i < uint32_t(kIconCount); i++) {
        const uint32_t a = kIcons + i * 12;
        Icon& c = d.icons[i];
        c.u = ovl1.Get<uint8_t>(a), c.v = ovl1.Get<uint8_t>(a + 1), c.clut = ovl1.Get<uint16_t>(a + 2);
        c.w = ovl1.Get<uint16_t>(a + 4), c.h = ovl1.Get<uint16_t>(a + 6), c.tpage = ovl1.Get<uint16_t>(a + 8);
    }
    for (uint32_t b = 0; b < 16; b++) {
        d.buttonIconOf[b] = ovl1.Get<int16_t>(kButtonIconOf + b * 2);
        d.negconIconOf[b] = ovl1.Get<int16_t>(kNegconIconOf + b * 2);
    }
    for (uint32_t c = 0; c < 3; c++) d.colours[c] = ovl1.Get<uint32_t>(kColours + c * 4);
    return d;
}

uint8_t NegconEntryOf(uint8_t b) { return b == 9 ? 0x81 : b == 10 ? 0x82 : b == 4 ? 0x83 : b; }         // 0x80019218
uint8_t NegconButtonOf(uint8_t e) { return e == 0x81 ? 9 : e == 0x82 ? 10 : e == 0x83 ? 4 : e; }        // 0x80019250

int AxisIconOf(uint8_t e) { // 0x80019288
    switch (e) {
    case 0xC3: return 3;
    case 0xA3: return 2;
    case 0xC2: return 1;
    case 0xA2: return 0;
    case 0xC1: return 7;
    case 0xA1: return 6;
    case 0xC0: return 5;
    case 0xA0: return 4;
    default: return 0;
    }
}

void LoadKeyList(KeyConfigList& list, uint8_t padType, const uint8_t* block) { // 0x80019388
    int table;
    if (padType == 4) table = 0;
    else if (padType == 2) table = 2;
    else if (padType == 5 || padType == 7) table = 1;
    else {
        list.type = 3;
        return;
    }
    list.type = table;
    std::memcpy(list.table.data(), block + table * 11, 11);
    std::memcpy(&list.steering, block + kKeyPages + table * 4, 4);
}

void StoreKeyList(const KeyConfigList& list, uint8_t* block) { // 0x80019498
    if (list.type < 0 || list.type > 2) return;
    std::memcpy(block + list.type * 11, list.table.data(), 11);
    std::memcpy(block + kKeyPages + list.type * 4, &list.steering, 4);
}

int FindKeyUse(const KeyConfigList& l, uint8_t b) { // 0x80019598
    for (int i = 4; i < 10; i++)
        if (l.table[size_t(i)] == b) return i;
    switch (l.type) {
    case 0:
        if (l.table[2] == b) return 2;
        return l.table[3] == b ? 3 : -1;
    case 1:
        if (l.table[2] == b) return 2;
        if (l.table[3] == b) return 3;
        if (IsAxis(l.table[2]) && l.savedAccel == b) return 2;
        if (!IsAxis(l.table[3])) return -1;
        return l.savedBrake == b ? 3 : -1;
    case 2: {
        if (l.table[2] == b) return 2;
        if (l.table[3] == b) return 3;
        const uint8_t e = NegconEntryOf(b);
        if (l.table[2] == e) return 2;
        return l.table[3] == e ? 3 : -1;
    }
    default:
        return -1;
    }
}

void AssignKey(KeyConfigList& l, int f, uint8_t b) { // 0x800196E8
    const int used = FindKeyUse(l, b);
    uint8_t* t = l.table.data();
    if (l.type == 1) {
        if (used >= 0) {
            uint8_t old = t[f];
            if (f == 2) {
                if (IsAxis(t[2])) old = l.savedAccel;
            } else if (f == 3 && IsAxis(t[3])) {
                old = l.savedBrake;
            }
            if (used == 2) {
                if (IsAxis(t[2])) l.savedAccel = old;
                else t[2] = old;
            } else if (used == 3) {
                if (IsAxis(t[3])) l.savedBrake = old;
                else t[3] = old;
            } else {
                t[used] = old;
            }
        }
        if (f == 2) {
            if (IsAxis(t[3])) {
                t[3] = l.savedBrake;
                l.preset = 0;
            }
            t[2] = b;
        } else if (f == 3) {
            if (IsAxis(t[2])) {
                t[2] = l.savedAccel;
                l.preset = 0;
            }
            t[3] = b;
        } else {
            t[f] = b;
        }
        return;
    }
    if (l.type == 0) {
        if (used >= 0) t[used] = t[f];
        t[f] = b;
        return;
    }
    if (l.type == 2) {
        if (used >= 0) t[used] = (used < 4 && used > 1) ? NegconEntryOf(t[f]) : NegconButtonOf(t[f]);
        t[f] = (f < 4 && f > 1) ? NegconEntryOf(b) : b;
    }
}

int SteeringInput(KeyConfigList& l, const MenuListPad& pad, std::vector<int>& sounds) { // 0x80019934
    const uint32_t pressed = pad.pressed;
    if (pad.held & kHeldStart) {
        if ((pressed & ~kHeldStart) == 0) return 0;
        sounds.push_back(0);
        return 0;
    }
    if (l.type == 0) {
        if (pressed & ~3u) {
            sounds.push_back(0);
            return 0;
        }
        return 1;
    }
    if (l.type != 1 && l.type != 2) return 1;
    if (pressed & ~0xFu) {
        sounds.push_back(0);
        return 0;
    }
    const int modes = l.type == 1 ? 3 : 2;
    int m = l.steering;
    if ((pressed & 4) && --m < 0) m = modes - 1;
    if ((pressed & 8) && ++m > modes - 1) m = 0;
    if (m == l.steering) return 1;
    sounds.push_back(5);
    l.steering = uint8_t(m);
    if (l.type == 1 && m == 0) l.table[0] = l.table[1] = 0x82;
    else if (m == (l.type == 1 ? 1 : 0)) l.table[0] = l.table[1] = 0x80;
    else l.table[0] = 2, l.table[1] = 3;
    return 0;
}

int PedalPresetInput(KeyConfigList& l, const MenuListPad& pad, const KeyConfigData& data, std::vector<int>& sounds) { // 0x80019B24
    const uint32_t pressed = pad.pressed;
    if ((pad.held & kHeldStart) || l.type != 1 || (pressed & 0xC) == 0) return 1;
    int p = l.preset;
    if ((pressed & 4) && --p < 0) p = 8;
    if ((pressed & 8) && ++p > 8) p = 0;
    if (p == l.preset) return 1;
    sounds.push_back(5);
    if (l.preset == 0) {
        l.savedAccel = l.table[2];
        l.savedBrake = l.table[3];
    }
    l.preset = uint8_t(p);
    if (p == 0) {
        l.table[2] = l.savedAccel;
        l.table[3] = l.savedBrake;
    } else {
        const size_t i = size_t(p) < data.presets.size() ? size_t(p) : 0; // (a stored preset above 9 reads past the table in the original)
        l.table[2] = data.presets[i][0];
        l.table[3] = data.presets[i][1];
    }
    return 0;
}

void DefaultKeys(KeyConfigList& l, const KeyConfigData& data) { // 0x80019C4C
    if (l.type < 0 || l.type > 2) return;
    l.table = data.defaults[size_t(l.type)];
    if (l.type == 1) l.steering = 0, l.preset = 0;
    if (l.type == 2) l.steering = 0;
}

int EditKeyRow(KeyConfigList& l, const MenuListPad& pad, int row, const KeyConfigData& data, std::vector<int>& sounds) { // 0x80019D5C
    const int f = data.rowFunction[size_t(row)];
    const uint32_t pressed = pad.pressed;
    if (f == -1) {
        if ((pressed & kChoose) == 0) return 0;
        DefaultKeys(l, data);
        sounds.push_back(1);
        return 1;
    }
    if (f == -2) return (pressed & kChoose) ? 2 : 0;
    if (f == 0) return SteeringInput(l, pad, sounds) == 0 ? 1 : 0;
    if (l.type == 1 && f < 4 && f > 1 && PedalPresetInput(l, pad, data, sounds) == 0) return 1;
    int b = -1;
    if ((pad.held & kHeldStart) == 0) {
        if (pressed & 0x10) b = 4;
        if (pressed & 0x20) b = 5;
        if (pressed & 0x1000) b = 12;
        if (pressed & 0x2000) b = 13;
        if (pressed & 0x200) b = 9;
        if (pressed & 0x100) b = 8;
        if (pressed & 0x400) b = 10;
        if (pressed & 0x800) b = 11;
    } else {
        if (pressed & 1) b = 0;
        if (pressed & 2) b = 1;
        if (pressed & 4) b = 2;
        if (pressed & 8) b = 3;
    }
    if (b < 0) return 0;
    sounds.push_back(1);
    AssignKey(l, f, uint8_t(b));
    return 1;
}

// ---------------------------------------------------------------- the page

void KeyConfigPage::Init() { // 0x8001A7E0
    rows_ = {-1, -1};
    blink_ = 0;
    lists_[0].type = lists_[1].type = 3;
    delay_ = -1;
}

void KeyConfigPage::Enter() { // 0x8001A834
    rows_ = {10, 10};
    delay_ = 4;
}

void KeyConfigPage::Leave() { // 0x8001A84C
    rows_ = {-1, -1};
    delay_ = -1;
}

int KeyConfigPage::Update(uint8_t* blocks[2], const std::array<uint8_t, 2>& padTypes, const MenuListPad* pads[2], const KeyConfigData& data,
                          std::vector<int>& sounds) { // 0x8001A860
    bool input = pads[0] != nullptr;
    if (delay_ > 0) {
        input = false;
        delay_--;
    }
    if (++blink_ > 0x2C) blink_ = 0;
    LoadKeyList(lists_[0], padTypes[0], blocks[0]);
    LoadKeyList(lists_[1], padTypes[1], blocks[1]);
    const MenuListPad none;
    int r1 = 1, r2 = 1;
    if (input) {
        r1 = EditKeyRow(lists_[0], pads[0] ? *pads[0] : none, rows_[0], data, sounds);
        r2 = EditKeyRow(lists_[1], pads[1] ? *pads[1] : none, rows_[1], data, sounds);
    }
    StoreKeyList(lists_[0], blocks[0]);
    StoreKeyList(lists_[1], blocks[1]);
    if (r1 == 2 || r2 == 2) return 0;
    for (int port = 0; port < 2; port++) {
        const int r = port == 0 ? r1 : r2;
        if (!input || r != 0) continue;
        const MenuListPad& pad = pads[port] ? *pads[port] : none;
        const uint32_t dirs = pad.pressed | pad.repeat;
        int row = rows_[size_t(port)];
        if ((dirs & 1) && --row < 0) row = 10;
        if ((dirs & 2) && ++row > 10) row = 0;
        if (row != rows_[size_t(port)]) {
            rows_[size_t(port)] = int16_t(row);
            sounds.push_back(6);
        }
    }
    return -1;
}

void KeyConfigPage::Draw(MenuOtSlot& ot, const TitleAssets& assets, const KeyConfigData& data, int x, int y, int alpha) const {
    const HudFont& small = assets.fonts[TitleAssets::kSmallFont];
    const HudFont& tiny = assets.fonts[TitleAssets::kTinyFont];
    // 0x80019F44: one port's cell of a row.
    auto cell = [&](const KeyConfigList& l, int cx, int cy, int row, int selected) {
        const int level = (alpha * (selected < 0 ? 0x80 : selected == row ? 0x80 : 0x30)) >> 7;
        if (l.type == 3) return;
        if (row == 9 || row == 10) { // Default / EXIT: only while the port edits
            if (selected < 0) return;
            const uint32_t c = MenuListLerp(data.colours[0], data.colours[2], level, 0x80);
            AddText(ot, tiny, assets.Text(row == 9 ? kDefaultText : kExitText), cx, row == 9 ? cy + 6 : cy, 1, c, 1, TextAlign::kCentre);
            return;
        }
        if (row == 0) {
            int icon = KeyConfigData::kSteerDpad;
            if (l.type == 1) icon = l.steering == 0 ? KeyConfigData::kSteerLeft : l.steering == 1 ? KeyConfigData::kSteerRight : KeyConfigData::kSteerDpad;
            else if (l.type == 2) icon = l.steering == 0 ? KeyConfigData::kSteerTwist : KeyConfigData::kSteerDpad;
            AddIcon(ot, data, icon, cx, cy, level);
            if (l.type == 0 || selected != 0) return;
            AddArrow(ot, cx + 0x10, cy, 6, 10, blink_);
            AddArrow(ot, cx - 0x10, cy, -6, 10, blink_);
            ot.DrawMode(0x20);
            return;
        }
        static constexpr int kEntryOfRow[9] = {0, 2, 3, 5, 4, 6, 7, 9, 8};
        const uint8_t value = l.table[size_t(kEntryOfRow[row])];
        int icon = -1;
        if (l.type == 1) {
            if (value < 0x80) icon = value < 16 ? data.buttonIconOf[value] : -1;
            else icon = KeyConfigData::kAxisIcons + AxisIconOf(value);
            if (unsigned(selected - 1) < 2u && row == 1) { // on Acceleration / Brake: the preset arrows between the two rows
                AddArrow(ot, cx + 0x10, cy + 0xD, 6, 10, blink_);
                AddArrow(ot, cx - 0x10, cy + 0xD, -6, 10, blink_);
                ot.DrawMode(0x20);
            }
        } else if (l.type == 0) {
            icon = value < 16 ? data.buttonIconOf[value] : -1;
        } else {
            const uint8_t b = NegconButtonOf(value);
            icon = b < 16 && data.negconIconOf[b] >= 0 ? KeyConfigData::kNegconIcons + data.negconIconOf[b] : -1;
        }
        AddIcon(ot, data, icon, cx, cy, level);
    };
    // 0x8001A624
    int ry = y;
    for (int row = 0; row < KeyConfigData::kRows; row++) {
        const uint32_t c = MenuListLerp(data.colours[0], data.colours[1], alpha, 0x80);
        AddText(ot, small, assets.Text(data.rowLabel[size_t(row)]), x, ry, 1, c, 1, TextAlign::kCentre);
        cell(lists_[0], x - 0x50, ry - 10, row, rows_[0]);
        cell(lists_[1], x + 0x50, ry - 10, row, rows_[1]);
        ry += 0x1A;
    }
    if (rows_[0] >= 0) AddText(ot, tiny, assets.Text(kHoldStartText), x, y - 0x18, 1, 0xB3060u, 1, TextAlign::kCentre);
}

} // namespace gt2::shell
