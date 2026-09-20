// gt2game --race-menu-check results / bonus / postmenu: see race_result_check.h.
#include "race_result_check.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "game/shell/title_draw.h"
#include "gt2export/car_mesh.h"
#include "gt2export/png_writer.h"
#include "game_window.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_model.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/png_reader.h"
#include "panel.h"
#include "gt2formats/race_menu_assets.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2view/race_result_screens.h"
#include "machine_test_check.h"
#include "race_menu_check.h"

using namespace gt2;
using gt2game::GameWindow;
using gt2game::Panels;

namespace {

std::vector<uint8_t> ReadBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// A 2 MB RAM dump as a guest image of KSEG0.
struct Ram {
    GuestImage image;
    explicit Ram(std::vector<uint8_t> bytes) {
        image.base = 0x80000000u;
        image.bytes = std::move(bytes);
        image.module = GuestImage::kRaceRam;
    }
    template <typename T> T Get(uint32_t a) const { return image.Get<T>(a); }
    std::string Text(uint32_t a) const {
        std::string s;
        if (a < 0x80000000u || a >= image.End()) return s;
        for (uint32_t o = a - image.base; o < image.bytes.size() && image.bytes[o] != 0; o++) s.push_back(char(image.bytes[o]));
        return s;
    }
};

screens::TextObject TextAt(const Ram& r, uint32_t a) { // 0x28 bytes
    screens::TextObject t;
    t.revealDivisor = r.Get<uint8_t>(a), t.waveDivisor = r.Get<uint8_t>(a + 1);
    t.steps = r.Get<int16_t>(a + 2), t.glowSpread = r.Get<int16_t>(a + 4), t.period = r.Get<int16_t>(a + 6), t.fadeSteps = r.Get<int16_t>(a + 8);
    t.extra = r.Get<int8_t>(a + 0xA), t.height = r.Get<uint8_t>(a + 0xB), t.flags = r.Get<uint16_t>(a + 0xC);
    t.font = r.Get<uint32_t>(a + 0x10), t.c0 = r.Get<uint32_t>(a + 0x14), t.c1 = r.Get<uint32_t>(a + 0x18);
    t.text = r.Text(r.Get<uint32_t>(a + 0x1C));
    t.alpha = r.Get<uint8_t>(a + 0x20);
    t.anim = r.Get<int16_t>(a + 0x22), t.settle = r.Get<int16_t>(a + 0x24), t.length = r.Get<int16_t>(a + 0x26);
    return t;
}

shell::Band BandAt(const Ram& r, uint32_t a) {
    shell::Band b = shell::Band::Read(r.image, a);
    b.anim = r.Get<int16_t>(a + 0x18);
    return b;
}

screens::FadePair FadeAt(const Ram& r, uint32_t a) { return {r.Get<int16_t>(a), r.Get<int16_t>(a + 2)}; }

screens::ResultLabel LabelAt(const Ram& r, uint32_t a) { // 0x50 bytes
    screens::ResultLabel l;
    l.band = BandAt(r, a);
    l.text = TextAt(r, a + 0x1C);
    l.flags = r.Get<uint16_t>(a + 0x4E);
    const uint32_t v = r.Get<uint32_t>(a + 0x44);
    if ((l.flags & 3) == 1) l.time = v;
    else l.value = r.Text(v);
    l.valueColour = r.Get<uint32_t>(a + 0x48);
    l.fade = r.Get<int16_t>(a + 0x4C);
    return l;
}

screens::CourseTitle CourseAt(const Ram& r, uint32_t a) { return {BandAt(r, a), TextAt(r, a + 0x1C)}; }

screens::ResultDialog DialogAt(const Ram& r, uint32_t a) {
    screens::ResultDialog d;
    d.x = r.Get<int16_t>(a), d.y = r.Get<int16_t>(a + 2);
    d.text = TextAt(r, a + 4);
    d.flags = r.Get<uint16_t>(a + 0x2C), d.w = r.Get<int16_t>(a + 0x2E), d.h = r.Get<int16_t>(a + 0x30), d.anim = r.Get<int16_t>(a + 0x32);
    d.fill = r.Get<uint32_t>(a + 0x34), d.gradient = r.Get<uint32_t>(a + 0x38);
    return d;
}

screens::ResultBar BarAt(const Ram& r, uint32_t a) {
    screens::ResultBar b;
    b.x = r.Get<int16_t>(a), b.y = r.Get<int16_t>(a + 2);
    b.title = TextAt(r, a + 4), b.label0 = TextAt(r, a + 0x2C), b.label1 = TextAt(r, a + 0x54);
    b.sound = r.Get<int8_t>(a + 0x7C), b.cursor = r.Get<int8_t>(a + 0x7D);
    b.w = r.Get<int16_t>(a + 0x7E), b.h = r.Get<int16_t>(a + 0x80), b.anim = r.Get<int16_t>(a + 0x82), b.slide = r.Get<int16_t>(a + 0x84);
    b.flags = r.Get<uint16_t>(a + 0x86);
    b.fill = r.Get<uint32_t>(a + 0x8C), b.gradient = r.Get<uint32_t>(a + 0x90);
    return b;
}

MenuListWidget ListAt(const Ram& r, uint32_t a) { return MenuListWidget::Read(r.image, a); }

menu::OverlayModelCamera CameraAt(const Ram& r, uint32_t a) { // the camera object of 0x80048754 (0xDC bytes)
    menu::OverlayModelCamera c;
    for (int i = 0; i < 3; i++) c.position[size_t(i)] = r.Get<int32_t>(a + 0xA0 + uint32_t(i) * 4);
    c.pitch = r.Get<int16_t>(a + 0xAC), c.yaw = r.Get<int16_t>(a + 0xAE), c.roll = r.Get<int16_t>(a + 0xB0);
    c.x = r.Get<int16_t>(a + 0xC0), c.y = r.Get<int16_t>(a + 0xC2), c.w = r.Get<int16_t>(a + 0xC4), c.h = r.Get<int16_t>(a + 0xC6);
    c.left = r.Get<int16_t>(a + 0xC8), c.right = r.Get<int16_t>(a + 0xCA), c.top = r.Get<int16_t>(a + 0xCC), c.bottom = r.Get<int16_t>(a + 0xCE);
    c.distance = r.Get<int16_t>(a + 0xD0), c.farZ = r.Get<int16_t>(a + 0xD2);
    c.floor = r.Get<uint8_t>(a + 0xD4) != 0, c.floorSemi = r.Get<uint8_t>(a + 0xD5) != 0;
    c.floorColour = r.Get<uint32_t>(a + 0xD8);
    return c;
}

// W, M, the current view (M+0x1C8), the previous one (M+0x1CC) and the manager's transition counter (M+0x211).
struct Blocks {
    uint32_t w = 0, m = 0, v = 0, previous = 0;
    int transition = 0;
    explicit Blocks(const Ram& r)
        : w(r.Get<uint32_t>(0x801C90A0u)), m(r.Get<uint32_t>(0x801C90A4u)), v(r.Get<uint32_t>(m + 0x1C8)), previous(r.Get<uint32_t>(m + 0x1CC)),
          transition(r.Get<int8_t>(m + 0x211)) {}
    // The view object of `wanted` (the current one, or the previous one during a transition); 0 when neither.
    uint32_t Find(std::initializer_list<uint32_t> wanted) const {
        for (uint32_t a : wanted)
            if (v == a) return v;
        for (uint32_t a : wanted)
            if (transition > 0 && previous == a) return previous;
        return 0;
    }
};

bool SaveBarOf(const Ram& r) { // 0x80050D00
    const uint8_t mode = r.Get<uint8_t>(0x801D5866u);
    return !(mode == 0 || mode == 2 || mode == 11 || r.Get<uint8_t>(0x801D5865u) == 0);
}

// ---- the views from a dump

void ResultsFromRam(const Ram& r, screens::ResultsView& s, uint32_t view) {
    const Blocks b(r);
    const uint32_t W = b.w;
    s.t = r.Get<int16_t>(view + 0x14), s.car = r.Get<int16_t>(view + 0x16), s.done = r.Get<int16_t>(view + 0x18);
    s.total = r.Get<uint32_t>(W + 8), s.fastest = r.Get<uint32_t>(W + 0x10);
    s.lapCount = r.Get<int16_t>(W + 2);
    s.lapTimes.clear(), s.lapNumbers.clear();
    for (int i = 0; i < s.lapCount && i < 10; i++) {
        s.lapTimes.push_back(r.Get<uint32_t>(W + 0x18 + uint32_t(i) * 4));
        s.lapNumbers.push_back(r.Get<int16_t>(W + 0x68 + uint32_t(i) * 2));
    }
    s.revealed = r.Get<int16_t>(W + 0x90), s.revealPeriod = r.Get<int16_t>(W + 0x92);
    s.course = CourseAt(r, W + 0x2B4);
    s.resultsLabel = LabelAt(r, W + 0x2F8), s.totalLabel = LabelAt(r, W + 0x38C), s.fastestLabel = LabelAt(r, W + 0x3F8), s.lapLabel = LabelAt(r, W + 0x464);
    s.place = TextAt(r, W + 0x348);
    s.placeBand = BandAt(r, W + 0x370), s.totalBand = BandAt(r, W + 0x3DC), s.fastestBand = BandAt(r, W + 0x448), s.lapBand = BandAt(r, W + 0x4B4);
    s.totalFade = FadeAt(r, 0x8005B6C0u), s.fastestFade = FadeAt(r, 0x8005B700u), s.lapFade = FadeAt(r, 0x8005B740u);
    s.list = ListAt(r, 0x8005B76Cu);
    s.AttachList();
    s.bar = BarAt(r, W + 0x4F8);
    s.dialog = DialogAt(r, W + 0x58C);
    s.saveBar = SaveBarOf(r);
    s.carCamera = CameraAt(r, W + 0x1D8);
    s.frameLength = r.Get<int32_t>(b.m + 0x234);
}

screens::ResultsInput ResultsInputOf(const Ram& r) {
    screens::ResultsInput in;
    in.place = r.Get<int16_t>(0x801D5E88u);
    in.totalTime = r.Get<uint32_t>(0x801D5F80u);
    in.fastestLap = r.Get<uint32_t>(0x801D5F58u);
    const int n = r.Get<int16_t>(0x801D5E8Cu);
    for (int i = 0; i < n && i < 10; i++) in.laps.push_back(r.Get<uint32_t>(0x801D5E90u + uint32_t(i) * 0x14));
    in.firstLap = r.Get<int16_t>(0x801D5E8Au) - n + 1;
    in.course = r.Text(0x801D587Cu);
    in.saveBar = SaveBarOf(r);
    in.vsync = r.Get<uint32_t>(0x801F0680u); // not the setup's value: the car camera's pose differs in sim= runs
    return in;
}

void BonusFromRam(const Ram& r, screens::BonusView& s, uint32_t view) {
    const Blocks b(r);
    const uint32_t W = b.w;
    s.view = view;
    s.t = r.Get<int16_t>(W), s.done = r.Get<uint8_t>(W + 2), s.closing = r.Get<uint8_t>(W + 3), s.placeIndex = r.Get<int16_t>(W + 4);
    s.shown = r.Get<uint32_t>(W + 8), s.remaining = r.Get<uint32_t>(W + 0xC), s.car = r.Get<uint32_t>(W + 0x10), s.useBar = r.Get<uint32_t>(W + 0x14);
    s.speed = r.Get<uint32_t>(W + 0x18), s.prizeFade = r.Get<int16_t>(W + 0x1C), s.moneyFade = r.Get<int16_t>(W + 0x1E);
    s.model = r.Get<uint32_t>(W + 0x208), s.modelAnim = r.Get<int16_t>(W + 0x20C);
    s.resultsLabel = LabelAt(r, W + 0x20), s.bonusLabel = LabelAt(r, W + 0x98), s.moneyLabel = LabelAt(r, W + 0xE8);
    s.place = TextAt(r, W + 0x70);
    s.newCarBand = BandAt(r, 0x8005D454u);
    s.bar = BarAt(r, W + 0x138);
    s.dialog = DialogAt(r, W + 0x1CC);
    s.trophyCamera = CameraAt(r, W + 0x354);
}

screens::BonusInput BonusInputOf(const Ram& r, uint32_t view) {
    const Blocks b(r);
    screens::BonusInput in;
    in.kind = view == screens::BonusView::kViewChampionship    ? screens::BonusKind::kChampionshipRace
              : view == screens::BonusView::kViewChampionshipEnd ? screens::BonusKind::kChampionshipEnd
                                                                 : screens::BonusKind::kSingleRace;
    in.place = r.Get<int8_t>(0x801D5DE8u);
    in.prize = in.kind == screens::BonusKind::kChampionshipEnd ? r.Get<uint32_t>(0x801D55ACu)
                                                               : r.Get<uint32_t>(0x801D55B0u + uint32_t(std::clamp(in.place - 1, 0, 5)) * 4);
    in.money = r.Get<uint32_t>(0x801D1568u);
    in.prizeCar = r.Get<uint32_t>(b.w + 0x10) != 0;
    return in;
}

void PostMenuFromRam(const Ram& r, screens::PostRaceMenuView& s, uint32_t view) {
    const Blocks b(r);
    const uint32_t W = b.w;
    s.counter = r.Get<int16_t>(view + 0x14), s.car = r.Get<int16_t>(view + 0x16), s.confirming = r.Get<int16_t>(view + 0x18);
    s.mode = r.Get<uint8_t>(0x801D5866u);
    s.title = r.Text(r.Get<uint32_t>(view + 0x10));
    s.list = ListAt(r, 0x8005ADC0u);
    s.AttachList();
    const uint32_t table = r.Get<uint32_t>(0x801C90B0u);
    s.rows.clear(), s.rowText.clear(), s.rowBand.clear();
    for (int i = 0; i < s.list.count && i < 8; i++) {
        const uint32_t e = table + uint32_t(i) * 8;
        s.rows.push_back({r.Text(r.Get<uint32_t>(e)), r.Get<int8_t>(e + 4) != 0, r.Get<int8_t>(e + 5)});
        s.rowText.push_back(TextAt(r, W + uint32_t(i) * 0x28));
        s.rowBand.push_back(BandAt(r, W + 0x140 + uint32_t(i) * 0x1C));
    }
    s.course = CourseAt(r, W + 0x440);
    s.resultsLabel = LabelAt(r, W + 0x484), s.totalLabel = LabelAt(r, W + 0x4D4), s.fastestLabel = LabelAt(r, W + 0x524);
    s.bar = BarAt(r, W + 0x654);
    s.carCamera = CameraAt(r, W + 0x364);
    s.frameLength = r.Get<int32_t>(b.m + 0x234);
}

screens::PostMenuInput PostMenuInputOf(const Ram& r) {
    screens::PostMenuInput in;
    in.mode = r.Get<uint8_t>(0x801D5866u);
    in.race = r.Get<int16_t>(0x801D5DF4u), in.races = r.Get<int16_t>(0x801D5DF6u);
    in.place = r.Get<int16_t>(0x801D5E88u);
    in.totalTime = r.Get<uint32_t>(0x801D5F80u), in.fastestLap = r.Get<uint32_t>(0x801D5F58u);
    in.course = r.Text(0x801D587Cu);
    return in;
}

// ---- primitive listings

std::string Hex3(uint16_t v) {
    char b[8];
    std::snprintf(b, sizeof b, "[%03X] ", v & 0x7FFu);
    return b;
}

// Our frame in the capture's text format, each line prefixed with the draw mode in effect ([E1 & 0x7FF]).
std::vector<std::string> OurLines(const std::vector<MenuPrim>& prims) {
    std::vector<std::string> out;
    char l[400];
    for (size_t i = 0; i < prims.size(); i++) {
        const MenuPrim& p = prims[i];
        switch (p.kind) {
        case MenuPrim::kSprite:
            std::snprintf(l, sizeof l, "RECT %02X rgb=%06X xy=(%d,%d) uv=(%u,%u) clut=%04X size=(%d,%d)%s", p.semi ? 0x66 : 0x64, p.colour[0], p.x[0], p.y[0], p.u, p.v, p.clut,
                          p.w, p.h, p.semi ? " semi" : "");
            break;
        case MenuPrim::kPolyFT4:
            std::snprintf(l, sizeof l, "POLY %02X quad tex%s rgb=%06X v0=(%d,%d) uv0=(%u,%u) clut=%04X v1=(%d,%d) uv1=(%u,%u) tpage=%04X v2=(%d,%d) uv2=(%u,%u) v3=(%d,%d) uv3=(%u,%u)",
                          p.semi ? 0x2E : 0x2C, p.semi ? " semi" : "", p.colour[0], p.x[0], p.y[0], p.tu[0], p.tv[0], p.clut, p.x[1], p.y[1], p.tu[1], p.tv[1], p.tpage, p.x[2],
                          p.y[2], p.tu[2], p.tv[2], p.x[3], p.y[3], p.tu[3], p.tv[3]);
            break;
        case MenuPrim::kTile:
            std::snprintf(l, sizeof l, "RECT %02X rgb=%06X xy=(%d,%d) size=(%d,%d)%s", p.semi ? 0x62 : 0x60, p.colour[0], p.x[0], p.y[0], p.w, p.h, p.semi ? " semi" : "");
            break;
        case MenuPrim::kPolyG4:
            std::snprintf(l, sizeof l, "POLY %02X quad gouraud%s rgb=%06X v0=(%d,%d) rgb1=%06X v1=(%d,%d) rgb2=%06X v2=(%d,%d) rgb3=%06X v3=(%d,%d)", p.semi ? 0x3A : 0x38,
                          p.semi ? " semi" : "", p.colour[0], p.x[0], p.y[0], p.colour[1], p.x[1], p.y[1], p.colour[2], p.x[2], p.y[2], p.colour[3], p.x[3], p.y[3]);
            break;
        case MenuPrim::kPolyF4:
            if (p.x[3] == p.x[2] && p.y[3] == p.y[2])
                std::snprintf(l, sizeof l, "POLY %02X tri%s rgb=%06X v0=(%d,%d) v1=(%d,%d) v2=(%d,%d)", p.semi ? 0x22 : 0x20, p.semi ? " semi" : "", p.colour[0], p.x[0], p.y[0],
                              p.x[1], p.y[1], p.x[2], p.y[2]);
            else
                std::snprintf(l, sizeof l, "POLY %02X quad%s rgb=%06X v0=(%d,%d) v1=(%d,%d) v2=(%d,%d) v3=(%d,%d)", p.semi ? 0x2A : 0x28, p.semi ? " semi" : "", p.colour[0], p.x[0],
                              p.y[0], p.x[1], p.y[1], p.x[2], p.y[2], p.x[3], p.y[3]);
            break;
        case MenuPrim::kLine: {
            // 0x8007E738's polyline (four segments of MenuListFrame) is one packet in the capture's listing.
            bool loop = i + 3 < prims.size();
            for (size_t k = 0; loop && k < 3; k++)
                loop = prims[i + k + 1].kind == MenuPrim::kLine && prims[i + k].x[1] == prims[i + k + 1].x[0] && prims[i + k].y[1] == prims[i + k + 1].y[0];
            loop = loop && prims[i + 3].x[1] == p.x[0] && prims[i + 3].y[1] == p.y[0];
            // gt2play lists the vertices (a polyline: each segment's start and the last end).
            std::string v = " v=(" + std::to_string(p.x[0]) + "," + std::to_string(p.y[0]) + ")";
            for (size_t k = 0; k < (loop ? 4u : 1u); k++) v += " v=(" + std::to_string(prims[i + k].x[1]) + "," + std::to_string(prims[i + k].y[1]) + ")";
            std::snprintf(l, sizeof l, "LINE %02X rgb=%06X%s", (loop ? 0x48 : 0x40) | (p.semi ? 2 : 0), p.colour[0], v.c_str());
            if (loop) i += 3;
            break;
        }
        }
        out.push_back(Hex3(p.tpage) + l);
    }
    return out;
}

struct CaptureFrame {
    std::vector<std::string> lines;  // 2D primitives (drawing area = the full frame), [E1] + text
    size_t carPrims = 0;             // primitives drawn in another drawing area (the 3D car)
    int carX0 = 0, carY0 = 0, carX1 = -1, carY1 = -1;
    int carOffsetX = 0, carOffsetY = 0;   // that environment's drawing offset (E5)
    std::vector<std::string> carLines;    // its primitives, [E1] + text (coordinates before the offset)
};

// The frames of a gt2play listing (blocks between "GP1 05" lines). The view manager's frame (0x800479AC) sets a draw
// environment (E3 / E4 / E5) per ordering table: [0] the clear (M+0xB8), [1] / [2] the 3D car's (M+0xC0 / M+0xD0,
// 0x8008034C), [3] the previous view (M+0xA0), [4] the current view (M+0x90); the car's primitives (environments 1 and
// 2) are excluded, the others kept with their coordinates before the environment's offset.
std::vector<CaptureFrame> CaptureFrames(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::vector<CaptureFrame> frames;
    uint32_t e3 = 0, e4 = 0, e5 = 0;
    uint16_t e1 = 0;
    int env = -1;
    for (std::string line; std::getline(in, line);) {
        if (line.rfind("GP1 05", 0) == 0) {
            frames.emplace_back();
            env = -1;
            continue;
        }
        if (frames.empty() || line.rfind("GP1", 0) == 0 || line.rfind("#", 0) == 0) continue;
        const size_t sp = line.find(' ');
        if (sp == std::string::npos) continue;
        const std::string t = line.substr(sp + 1);
        CaptureFrame& f = frames.back();
        if (t.rfind("E1 ", 0) == 0) {
            const size_t at = t.find("tpage=");
            if (at != std::string::npos) e1 = uint16_t(std::strtoul(t.c_str() + at + 6, nullptr, 16));
        } else if (t.rfind("E3 ", 0) == 0) {
            e3 = uint32_t(std::strtoul(t.c_str() + 3, nullptr, 16));
            env++;
        } else if (t.rfind("E4 ", 0) == 0) {
            e4 = uint32_t(std::strtoul(t.c_str() + 3, nullptr, 16));
        } else if (t.rfind("E5 ", 0) == 0) {
            e5 = uint32_t(std::strtoul(t.c_str() + 3, nullptr, 16));
        } else if (t.rfind("RECT ", 0) == 0 || t.rfind("POLY ", 0) == 0 || t.rfind("LINE ", 0) == 0) {
            if (env == 1 || env == 2) {
                f.carPrims++;
                f.carX0 = int(e3 & 0x3FF), f.carY0 = int((e3 >> 10) & 0x1FF), f.carX1 = int(e4 & 0x3FF), f.carY1 = int((e4 >> 10) & 0x1FF);
                f.carOffsetX = int(int32_t(e5 << 21) >> 21), f.carOffsetY = int(int32_t((e5 >> 11) << 21) >> 21);
                f.carLines.push_back(Hex3(e1) + t);
            } else {
                f.lines.push_back(Hex3(e1) + t);
            }
        }
    }
    while (!frames.empty() && frames.back().lines.empty()) frames.pop_back();
    return frames;
}

// ---- the 3D model (0x80048754): floor disc and silhouette

// The corners "vK=(x,y)" of a listing line, in order.
std::vector<std::array<int, 2>> LineVertices(const std::string& t) {
    std::vector<std::array<int, 2>> out;
    for (int k = 0; k < 4; k++) {
        const std::string key = "v" + std::to_string(k) + "=(";
        const size_t at = t.find(key);
        if (at == std::string::npos) break;
        const char* p = t.c_str() + at + key.size();
        char* end = nullptr;
        const int x = int(std::strtol(p, &end, 10));
        const int y = int(std::strtol(end + 1, nullptr, 10));
        out.push_back({x, y});
    }
    return out;
}

// A coverage mask of the frame (pixel centres inside the triangle), clipped to a rectangle.
struct Mask {
    int w = 0, h = 0;
    std::vector<uint8_t> bits;
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1; // clip (inclusive)
    Mask(int width, int height) : w(width), h(height), bits(size_t(width) * size_t(height), 0) {}
    void Triangle(const float* ax, const float* ay) {
        const float minX = std::min({ax[0], ax[1], ax[2]}), maxX = std::max({ax[0], ax[1], ax[2]});
        const float minY = std::min({ay[0], ay[1], ay[2]}), maxY = std::max({ay[0], ay[1], ay[2]});
        const int bx0 = std::max(x0, int(std::floor(minX))), bx1 = std::min(x1, int(std::ceil(maxX)));
        const int by0 = std::max(y0, int(std::floor(minY))), by1 = std::min(y1, int(std::ceil(maxY)));
        const float area = (ax[1] - ax[0]) * (ay[2] - ay[0]) - (ax[2] - ax[0]) * (ay[1] - ay[0]);
        if (std::fabs(area) < 1e-6f) return;
        for (int y = by0; y <= by1; y++)
            for (int x = bx0; x <= bx1; x++) {
                const float px = float(x) + 0.5f, py = float(y) + 0.5f;
                float e[3];
                for (int k = 0; k < 3; k++) {
                    const int a = k, b = (k + 1) % 3;
                    e[k] = ((ax[b] - ax[a]) * (py - ay[a]) - (ay[b] - ay[a]) * (px - ax[a])) * (area > 0 ? 1.0f : -1.0f);
                }
                if (e[0] >= 0 && e[1] >= 0 && e[2] >= 0) bits[size_t(y) * size_t(w) + size_t(x)] = 1;
            }
    }
    size_t Count() const { return size_t(std::count(bits.begin(), bits.end(), uint8_t(1))); }
    void Describe(const char* name) const {
        int bx0 = w, by0 = h, bx1 = -1, by1 = -1;
        double sx = 0, sy = 0;
        size_t n = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if (bits[size_t(y) * size_t(w) + size_t(x)]) {
                    bx0 = std::min(bx0, x), by0 = std::min(by0, y), bx1 = std::max(bx1, x), by1 = std::max(by1, y);
                    sx += x, sy += y, n++;
                }
        std::printf("race-menu-check:   %s silhouette %zu px, box (%d,%d)-(%d,%d), centroid (%.1f, %.1f)\n", name, n, bx0, by0, bx1, by1, n ? sx / double(n) : 0.0,
                    n ? sy / double(n) : 0.0);
    }
};

// The model of the view against the capture's model environment: the floor disc primitive by primitive (ours with the
// environment's offset taken off, the listing's coordinates are before it) and the model's silhouette - the captured
// non-floor primitives filled vs our model's LOD 0 triangles (wheels, ground shadow) through our projection. Returns
// true when the floor is equal and the silhouettes overlap with IoU >= 0.85.
bool CheckModel(const GtfsVolume& vol, const Ram& ram, const screens::PostRaceModel& model, const CaptureFrame& capture, std::vector<uint8_t>* overlay, int W,
                int H, Mask* capturedOut) {
    const menu::MenuCarProjection carP = model.Projection(true);
    std::printf("race-menu-check: model %s: camera pitch %d yaw %d roll %d, position (%d, %d, %d), rectangle %d x %d, window (%d, %d) x (%d, %d) H %d, floor %d/%d "
                "colour %06X, environment offset (%d, %d); the capture's (%d, %d), area (%d,%d)-(%d,%d)\n",
                model.trophy ? "trophy" : "car", model.camera.pitch, model.camera.yaw, model.camera.roll, model.camera.position[0], model.camera.position[1],
                model.camera.position[2], model.camera.w, model.camera.h, model.camera.left, model.camera.right, model.camera.top, model.camera.bottom,
                model.camera.distance, int(model.camera.floor), int(model.camera.floorSemi), model.camera.floorColour, model.envX, model.envY, capture.carOffsetX,
                capture.carOffsetY, capture.carX0, capture.carY0, capture.carX1, capture.carY1);
    // Floor.
    std::vector<MenuPrim> floor = screens::BuildPostRaceModelFloor(model);
    for (MenuPrim& p : floor)
        for (int k = 0; k < 4; k++) p.x[k] = int16_t(p.x[k] - model.envX), p.y[k] = int16_t(p.y[k] - model.envY);
    std::vector<std::string> ours = OurLines(floor);
    for (std::string& l : ours) { // our triangles as the listing writes a POLY_G3 ("tri gouraud", three corners)
        const size_t q = l.find(" quad gouraud");
        if (q == std::string::npos) continue;
        l.replace(q, 13, " tri gouraud");
        const size_t c3 = l.find(" rgb3=");
        if (c3 != std::string::npos) l.erase(c3);
        for (const auto& [from, to] : {std::pair<const char*, const char*>{"POLY 38", "POLY 30"}, {"POLY 3A", "POLY 32"}})
            if (const size_t at = l.find(from); at != std::string::npos) l.replace(at, 7, to);
    }
    char floorKey[32];
    std::snprintf(floorKey, sizeof floorKey, "gouraud%s rgb=%06X", model.camera.floorSemi ? " semi" : "", model.camera.floorColour & 0xFFFFFF);
    std::vector<std::string> capturedFloor, capturedModel;
    for (const std::string& l : capture.carLines)
        (l.find(floorKey) != std::string::npos && l.find(" tri ") != std::string::npos ? capturedFloor : capturedModel).push_back(l);
    const bool floorEqual = ours == capturedFloor;
    std::printf("race-menu-check:   floor disc: ours %zu, captured %zu triangles, %s\n", ours.size(), capturedFloor.size(), floorEqual ? "equal" : "DIFFERENT");
    if (!floorEqual)
        for (size_t i = 0; i < std::max(ours.size(), capturedFloor.size()); i++)
            if (i >= ours.size() || i >= capturedFloor.size() || ours[i] != capturedFloor[i]) {
                std::printf("    first difference %zu\n    ours     %s\n    captured %s\n", i, i < ours.size() ? ours[i].c_str() : "(end)",
                            i < capturedFloor.size() ? capturedFloor[i].c_str() : "(end)");
                break;
            }
    // Silhouettes.
    Mask captured(W, H), native(W, H);
    captured.x0 = capture.carX0, captured.y0 = capture.carY0, captured.x1 = capture.carX1, captured.y1 = capture.carY1;
    native.x0 = carP.x0, native.y0 = carP.y0, native.x1 = carP.x0 + carP.w - 1, native.y1 = carP.y0 + carP.h - 1;
    for (const std::string& l : capturedModel) {
        const auto v = LineVertices(l);
        if (v.size() < 3) continue;
        float xs[3], ys[3];
        for (int k = 0; k < 3; k++) xs[k] = float(v[size_t(k)][0] + capture.carOffsetX), ys[k] = float(v[size_t(k)][1] + capture.carOffsetY);
        captured.Triangle(xs, ys);
        if (v.size() == 4) { // GPU quad: v1 v2 v3
            for (int k = 0; k < 3; k++) xs[k] = float(v[size_t(k + 1)][0] + capture.carOffsetX), ys[k] = float(v[size_t(k + 1)][1] + capture.carOffsetY);
            captured.Triangle(xs, ys);
        }
    }
    // Race slot 0 (0x801D58B8, 0xD0 bytes: +0 car id, +4 paint character, +0x8E entry kind, +0x90 name).
    const std::string id = model.trophy ? "gtprz" : UnpackCarId(ram.Get<uint32_t>(0x801D58B8u));
    const CarModel car = ParseCarModel(vol.Read("carobj/" + id + ".cdo"));
    const float lift = float(menu::MenuCarLift(car.wheelRadiusFront, car.wheels[0].y)) / 65536.0f; // 0x80061544
    CarMeshOptions options;
    options.wheels = !model.trophy;
    std::vector<CarMeshVertex> mesh = BuildCarMesh(car, options);
    if (!model.trophy) { // 0x80067444 draws the car's ground shadow; the trophy's 0x80048528 draws none
        const std::vector<CarMeshVertex> shadow = BuildCarShadowMesh(car, CarShadowHeight(car));
        mesh.insert(mesh.end(), shadow.begin(), shadow.end());
    }
    const menu::MenuCarLinearView L = menu::MenuCarLinear(carP);
    for (size_t i = 0; i + 2 < mesh.size(); i += 3) {
        float xs[3], ys[3];
        bool behind = false;
        for (int k = 0; k < 3; k++) {
            const float* p = mesh[i + size_t(k)].pos;
            float a[3];
            for (int r = 0; r < 3; r++) a[r] = L.rows[r][0] * p[0] + L.rows[r][1] * (p[1] + lift) + L.rows[r][2] * p[2] + L.rows[r][3];
            if (a[2] < 0.05f) behind = true;
            xs[k] = L.originX + L.H * a[0] / a[2];
            ys[k] = L.originY + L.H * a[1] / a[2];
        }
        if (!behind) native.Triangle(xs, ys);
    }
    size_t both = 0, either = 0;
    for (size_t i = 0; i < captured.bits.size(); i++) {
        both += captured.bits[i] & native.bits[i];
        either += captured.bits[i] | native.bits[i];
        if (overlay && (captured.bits[i] | native.bits[i])) { // red = only ours, green = only the original's, yellow = both
            uint8_t* px = &(*overlay)[i * 4];
            px[0] = native.bits[i] ? 255 : 0, px[1] = captured.bits[i] ? 255 : 0, px[2] = 0;
        }
    }
    const double iou = either ? double(both) / double(either) : 1.0;
    std::printf("race-menu-check:   model %s (%zu captured primitives, %zu of our triangles):\n", id.c_str(), capturedModel.size(), mesh.size() / 3);
    captured.Describe("original");
    native.Describe("ours    ");
    std::printf("race-menu-check:   silhouette IoU %.3f\n", iou);
    if (capturedOut) *capturedOut = captured;
    return floorEqual && iou >= 0.85;
}

// The renderer's path (vulkan=<png>): our frame with the model through Panels::FullScreenModel (gt2game's post-race
// views) in a 640 x 480 window (the 352 x 480 frame at 4:3), shot twice - with the model and with the model's slot
// empty; the pixels that differ are the model's. Their silhouette (sampled back to frame pixels) against the captured
// one; writes <base>_vulkan.png (the capture's frame | ours from the renderer).
bool VulkanModelCheck(const DiscImage& disc, const GtfsVolume& vol, const Ram& ram, const std::vector<MenuPrim>& prims, const screens::PostRaceModel& model,
                      const Mask& captured, const std::vector<uint16_t>& vram, const MenuCanvas& views, const std::string& base) {
    constexpr int kWinW = 640, kWinH = 480;
    GameWindow window("gt2game --race-menu-check (vulkan)", kWinW, kWinH);
    window.SetPacing(false);
    Panels panels(window.Renderer(), disc, vol);
    std::vector<MenuPrim> frame = prims;
    size_t modelAt = shell::TitleFrameStart().size();
    const std::vector<MenuPrim> floor = screens::BuildPostRaceModelFloor(model);
    frame.insert(frame.begin() + std::ptrdiff_t(modelAt), floor.begin(), floor.end());
    modelAt += floor.size();
    const uint32_t carId = ram.Get<uint32_t>(0x801D58B8u);
    uint32_t paintChar = ram.Get<uint32_t>(0x801D58BCu);
    int paint = 0;
    try {
        paint = std::max(0, ParseCarTexture(vol.Read("carobj/" + UnpackCarId(carId) + ".cdp")).PaintIndex(uint8_t(paintChar)));
    } catch (const std::exception&) {
    }
    std::fill(std::begin(window.Renderer().clearColor), std::end(window.Renderer().clearColor), 0.0f);
    const std::string withPath = base + "_vk_with.png", withoutPath = base + "_vk_without.png";
    for (int pass = 0; pass < 4 && window.BeginFrame(); pass++) { // two frames per shot (the uploads settle)
        std::optional<screens::PostRaceModel> m = model;
        if (pass >= 2) m->trophy = false; // car id 0: nothing loads, the model's slot stays empty
        std::vector<gt2view::DrawItem> items;
        panels.Clear();
        panels.FullScreenModel(frame, Panels::Screen::kSettings, modelAt, m, pass >= 2 ? 0u : carId, paint);
        panels.Append(window.Renderer().AspectRatio(), items);
        window.EndFrame(items, pass == 1 ? withPath : pass == 3 ? withoutPath : std::string());
    }
    const PngImage with = ReadPngFile(withPath), without = ReadPngFile(withoutPath);
    if (with.width != kWinW || with.height != kWinH || without.width != kWinW || without.height != kWinH) {
        std::printf("race-menu-check: vulkan: unexpected shot size %d x %d\n", with.width, with.height);
        return false;
    }
    const int W = RaceMenuAssets::kScreenWidth, H = RaceMenuAssets::kScreenHeight;
    Mask ours(W, H);
    std::vector<uint8_t> side(size_t(W) * 2 * H * 4, 255);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const int wx = std::min(kWinW - 1, int((float(x) + 0.5f) * float(kWinW) / float(W))), wy = y * kWinH / H;
            const uint8_t* a = &with.rgba[(size_t(wy) * kWinW + size_t(wx)) * 4];
            const uint8_t* b = &without.rgba[(size_t(wy) * kWinW + size_t(wx)) * 4];
            if (std::abs(int(a[0]) - b[0]) + std::abs(int(a[1]) - b[1]) + std::abs(int(a[2]) - b[2]) > 12) ours.bits[size_t(y) * W + size_t(x)] = 1;
            const uint16_t c = vram[size_t(y) * 1024 + size_t(x)];
            uint8_t* l = &side[(size_t(y) * W * 2 + size_t(x)) * 4];
            l[0] = uint8_t((c & 31) << 3), l[1] = uint8_t(((c >> 5) & 31) << 3), l[2] = uint8_t(((c >> 10) & 31) << 3);
            uint8_t* r = &side[(size_t(y) * W * 2 + size_t(W + x)) * 4];
            r[0] = a[0], r[1] = a[1], r[2] = a[2];
        }
    WritePngRgba(base + "_vulkan.png", W * 2, H, side);
    // The views' 2D pixels (texts, bands) cover the model in both shots: those pixels are left out of both silhouettes.
    Mask original = captured;
    size_t covered = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if ((views.At(x, y) & 0x7FFF) != 0) {
                const size_t i = size_t(y) * W + size_t(x);
                covered += original.bits[i];
                original.bits[i] = ours.bits[i] = 0;
            }
    size_t both = 0, either = 0;
    for (size_t i = 0; i < ours.bits.size(); i++) both += original.bits[i] & ours.bits[i], either += original.bits[i] | ours.bits[i];
    const double iou = either ? double(both) / double(either) : 1.0;
    std::printf("race-menu-check: vulkan (the renderer's model %s, paint %d; %zu captured model pixels under the views' 2D left out):\n",
                UnpackCarId(model.trophy ? PackCarId("gtprz") : carId).c_str(), paint, covered);
    original.Describe("original");
    ours.Describe("renderer");
    std::printf("race-menu-check:   silhouette IoU %.3f -> %s_vulkan.png\n", iou, base.c_str());
    return iou >= 0.85;
}

size_t CompareVramRegion(const MenuVram& ours, const std::vector<uint16_t>& vram, int x0, int y0, int w, int h) {
    size_t diff = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (ours.Word(x, y) != vram[size_t(y) * 1024 + size_t(x)]) diff++;
    return diff;
}

} // namespace

bool IsRaceResultScreen(const char* s) {
    return std::strcmp(s, "results") == 0 || std::strcmp(s, "bonus") == 0 || std::strcmp(s, "postmenu") == 0 || std::strcmp(s, "eventmenu") == 0 ||
           IsMachineTestScreen(s); // machine_test_check.h
}

int RunRaceResultCheck(const DiscImage& disc, const GtfsVolume& vol, const std::vector<std::string>& args) {
    if (args.size() < 3) {
        std::puts("usage: gt2game <disc> --race-menu-check <results|bonus|postmenu> <cap.txt.vram.bin> <side.png> [ram=<ram.bin>] [sim=N] [advance=N] [ps1] [vulkan]");
        return 2;
    }
    const std::string screen = args[0], vramPath = args[1], sidePath = args[2];
    std::string listing = vramPath;
    if (listing.size() > 9 && listing.compare(listing.size() - 9, 9, ".vram.bin") == 0) listing.resize(listing.size() - 9);
    std::string ramPath = listing + ".ram.bin";
    int sim = -1, advance = 0;
    bool ps1 = false, vulkan = false;
    std::vector<std::pair<int, uint32_t>> presses; // press=K:button,... (update K of the run, 1-based)
    for (size_t i = 3; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a.rfind("ram=", 0) == 0) ramPath = a.substr(4);
        else if (a.rfind("sim=", 0) == 0) sim = std::atoi(a.c_str() + 4);
        else if (a.rfind("advance=", 0) == 0) advance = std::atoi(a.c_str() + 8);
        else if (a == "ps1") ps1 = true;
        else if (a == "vulkan") vulkan = true;
        else if (a.rfind("press=", 0) == 0) {
            static const std::pair<const char*, uint32_t> kButtons[] = {{"cross", menu_list_pad::kCross}, {"circle", menu_list_pad::kCircle},
                                                                        {"triangle", menu_list_pad::kTriangle}, {"square", menu_list_pad::kSquare},
                                                                        {"up", menu_list_pad::kUp}, {"down", menu_list_pad::kDown},
                                                                        {"left", menu_list_pad::kLeft}, {"right", menu_list_pad::kRight},
                                                                        {"start", menu_list_pad::kStart}};
            size_t at = 6;
            while (at < a.size()) {
                const size_t end = std::min(a.find(',', at), a.size());
                const std::string item = a.substr(at, end - at);
                const size_t colon = item.find(':');
                uint32_t bit = 0;
                for (const auto& [name, b] : kButtons)
                    if (colon != std::string::npos && item.substr(colon + 1) == name) bit = b;
                if (!bit) throw std::runtime_error("press=: bad item " + item);
                presses.push_back({std::atoi(item.c_str()), bit});
                at = end + 1;
            }
        }
    }
    const Ram ram(ReadBytes(ramPath));
    std::vector<uint16_t> vram(1024 * 512);
    {
        const std::vector<uint8_t> bytes = ReadBytes(vramPath);
        if (bytes.size() != vram.size() * 2) throw std::runtime_error(vramPath + ": not a 1024 x 512 VRAM dump");
        std::memcpy(vram.data(), bytes.data(), bytes.size());
    }
    const RaceMenuAssets assets = RaceMenuAssets::Load(disc, vol, RaceMenuAssets::Pictures::kSettings);

    const Blocks blocks(ram);
    std::vector<MenuPrim> prims, listed;
    std::optional<screens::PostRaceModel> model;
    if (screen == "eventmenu") {
        // The event menu 0x800585C0 (race_menus.h, state from the dump) with its car (M+0x241 and view + 0x16 >= 0:
        // 0x80048754(W+0x220, M+0xC0, M+0xD0, W+0x364, 0) in (0x7C, 0xA0, 200, 200) - the post-race menu's objects).
        prims = listed = screens::BuildEventMenuFrame(assets, EventMenuStateOfRam(ram.image.bytes)); // listed: the 2D primitives (the floor is the 3D area's)
        if (ram.Get<uint8_t>(blocks.m + 0x241) != 0 && ram.Get<int16_t>(blocks.v + 0x16) >= 0) {
            screens::PostRaceModel m;
            m.camera = CameraAt(ram, blocks.w + 0x364);
            m.envX = 0x7C, m.envY = 0xA0;
            const size_t clear = shell::TitleFrameStart().size();
            const std::vector<MenuPrim> floor = screens::BuildPostRaceModelFloor(m);
            prims.insert(prims.begin() + std::ptrdiff_t(clear), floor.begin(), floor.end());
            model = m;
        }
        std::printf("race-menu-check: event menu, car %s\n", model ? "drawn (M+0x241)" : "not drawn");
    }
    // Our view from the dump: the current view, or the previous one while the manager switches (M+0x211 > 0; the
    // other one is then a wait / leave view that draws its header only).
    const screens::PostRaceView* previous = nullptr;
    const screens::PostRaceView* current = nullptr;
    int switching = 0;
    std::unique_ptr<screens::PostRaceView> view;
    std::unique_ptr<screens::WaitView> other;
    std::unique_ptr<screens::PostRaceFlow> flow;
    if (screen != "eventmenu") {
    uint32_t viewAddress = 0;
    if (screen == "results") {
        viewAddress = blocks.Find({screens::ResultsView::kView});
        auto v = std::make_unique<screens::ResultsView>(assets);
        if (sim >= 0) {
            // The car camera's pose is seeded with the VSync counter at the setup (0x80050BC4): find the counter near
            // (the dump's - sim) that gives the dump's pitch and, after `sim` turns of 12, its yaw.
            screens::ResultsInput in = ResultsInputOf(ram);
            const menu::OverlayModelCamera want = CameraAt(ram, Blocks(ram).w + 0x1D8);
            for (int k = -16; k <= 16; k++) {
                const uint32_t c = in.vsync - uint32_t(sim) + uint32_t(k);
                const menu::OverlayModelCamera cam = menu::ResultsModelCamera(c);
                if (cam.pitch == want.pitch && ((cam.yaw + 12 * sim) & 0x3FFF) == want.yaw) {
                    std::printf("race-menu-check: car camera (0x80050BC4): the VSync counter %u (dump - sim %+d) gives the dump's pitch %d / yaw %d\n", c, k, want.pitch,
                                want.yaw);
                    in.vsync = c;
                    break;
                }
                if (k == 16) std::printf("race-menu-check: car camera (0x80050BC4): no VSync counter near the dump's gives its pitch / yaw\n");
            }
            v->Setup(in);
        } else if (viewAddress) {
            ResultsFromRam(ram, *v, viewAddress);
        }
        view = std::move(v);
    } else if (screen == "bonus") {
        viewAddress = blocks.Find({screens::BonusView::kViewSingle, screens::BonusView::kViewChampionship, screens::BonusView::kViewChampionshipEnd});
        auto v = std::make_unique<screens::BonusView>(assets);
        if (sim >= 0) v->Setup(BonusInputOf(ram, viewAddress));
        else if (viewAddress) BonusFromRam(ram, *v, viewAddress);
        view = std::move(v);
    } else if (IsMachineTestScreen(screen)) { // machine_test_check.h: the machine-test views' objects from the dump
        view = MachineTestViewFromRam(assets, ram.image, screen, viewAddress);
    } else {
        viewAddress = blocks.Find({screens::PostRaceMenuView::kView});
        auto v = std::make_unique<screens::PostRaceMenuView>(assets);
        if (sim >= 0) v->Setup(PostMenuInputOf(ram));
        else if (viewAddress) PostMenuFromRam(ram, *v, viewAddress);
        view = std::move(v);
    }
    if (!viewAddress) {
        std::printf("race-menu-check: the dump's views (current 0x%08X, previous 0x%08X, transition %d) are not %s\n", blocks.v, blocks.previous, blocks.transition,
                    screen.c_str());
        return 2;
    }
    const bool isPrevious = viewAddress != blocks.v;
    const int transition = sim >= 0 ? 0 : blocks.transition;
    if (transition > 0) other = std::make_unique<screens::WaitView>(assets, isPrevious ? blocks.v : blocks.previous, 0);
    // sim= / advance=: our updates through the view manager (PostRaceFlow): a view that is left switches to the leave
    // view 0x8005AE30 (20 fields), as after "Next" / a menu row; `press=` gives the pad of single updates (pressed).
    const int updates = sim >= 0 ? sim : advance;
    if (updates > 0) {
        if (transition > 0) throw std::runtime_error("advance= needs a dump without a view switch");
        flow = std::make_unique<screens::PostRaceFlow>();
        flow->Start(std::move(view), false);
        for (int i = 1; i <= updates; i++) {
            MenuListPad pad;
            for (const auto& [k, bit] : presses)
                if (k == i) pad.pressed |= bit, pad.held |= bit;
            if (flow->Update(&pad) == 1) {
                std::printf("race-menu-check: update %d: the view is left -> leave view 0x8005AE30\n", i);
                flow->Switch(std::make_unique<screens::WaitView>(assets, 0x8005AE30u, 20));
            }
        }
    }
    // The view of our state (ours after the run: the previous view while the run's last switch is under way).
    const screens::PostRaceView* shown = view.get();
    if (flow) shown = flow->Previous() && !dynamic_cast<const screens::WaitView*>(flow->Previous()) ? flow->Previous() : flow->Current();
    if (auto* v = dynamic_cast<const screens::ResultsView*>(shown))
        std::printf("race-menu-check: RESULTS t %d car %d done %d, laps %d revealed %d, list state %d sel %d, dialog %d bar %d\n", v->t, v->car, v->done, v->lapCount,
                    v->revealed, v->list.state, v->list.selection, v->dialog.anim, v->bar.anim);
    if (auto* v = dynamic_cast<const screens::BonusView*>(shown))
        std::printf("race-menu-check: BONUS (view 0x%08X) t %d done %d, remaining %u shown %u speed %u, fades %d / %d, car %u, bar %d dialog %d\n", v->view, v->t, v->done,
                    v->remaining, v->shown, v->speed, v->prizeFade, v->moneyFade, v->car, v->bar.anim, v->dialog.anim);
    if (auto* v = dynamic_cast<const screens::PostRaceMenuView*>(shown))
        std::printf("race-menu-check: post-race menu \"%s\" counter %d, %zu rows, list state %d sel %d fade %d, labels %d, bar %d\n", v->title.c_str(), v->counter,
                    v->rows.size(), v->list.state, v->list.selection, v->list.fade, v->totalLabel.fade, v->bar.anim);
    if (sim >= 0) {
        // The simulated state against the dump's counters.
        const Blocks& b = blocks;
        if (auto* v = dynamic_cast<const screens::ResultsView*>(shown); v && ram.Get<int16_t>(viewAddress + 0x14) != v->t)
            std::printf("race-menu-check: sim: t %d, the dump's %d\n", v->t, ram.Get<int16_t>(viewAddress + 0x14));
        if (auto* v = dynamic_cast<const screens::BonusView*>(shown); v && (ram.Get<int16_t>(b.w) != v->t || ram.Get<uint32_t>(b.w + 0xC) != v->remaining))
            std::printf("race-menu-check: sim: t %d remaining %u, the dump's %d / %u\n", v->t, v->remaining, ram.Get<int16_t>(b.w), ram.Get<uint32_t>(b.w + 0xC));
    }

    // Our frame (pixels) and its primitives before the environments' offsets (the listing's coordinates).
    current = view.get();
    switching = transition;
    if (flow) {
        previous = flow->Previous(), current = flow->Current(), switching = flow->Transition();
    } else if (transition > 0) {
        previous = isPrevious ? view.get() : other.get();
        current = isPrevious ? other.get() : view.get();
        std::printf("race-menu-check: view switch M+0x211 = %d: previous 0x%08X, current 0x%08X (ours is the %s)\n", transition, blocks.previous, blocks.v,
                    isPrevious ? "previous" : "current");
    }
    if (flow && flow->Transition() > 0) std::printf("race-menu-check: our view switch after the run: M+0x211 = %d\n", flow->Transition());
    if (switching > 0 && previous) {
        prims = screens::BuildPostRaceTransitionFrame(assets, previous, *current, switching);
        listed = shell::TitleFrameStart();
        for (const MenuPrim& p : screens::BuildPostRaceViewPart(assets, *previous, switching * 8, false)) listed.push_back(p);
        for (const MenuPrim& p : screens::BuildPostRaceViewPart(assets, *current, 128 - switching * 8, true)) listed.push_back(p);
    } else {
        prims = listed = screens::BuildPostRaceFrame(assets, *current, 128);
    }
    model = current->Model();
    if (!model && previous && switching > 0) model = previous->Model();
    } // not the event menu

    // Primitive sequence against the capture's last frame (2D only).
    const std::vector<CaptureFrame> frames = CaptureFrames(listing);
    if (frames.empty()) throw std::runtime_error(listing + ": no frames");
    const CaptureFrame& last = frames.back();
    std::vector<std::string> ours = OurLines(listed);
    for (size_t k = 0; k < ours.size() && k < last.lines.size(); k++) // listings of older gt2play builds carry no line vertices
        if (ours[k].find(" LINE ") != std::string::npos && last.lines[k].find(" v=(") == std::string::npos)
            if (const size_t at = ours[k].find(" v=("); at != std::string::npos) ours[k].resize(at);
    size_t same = 0;
    while (same < ours.size() && same < last.lines.size() && ours[same] == last.lines[same]) same++;
    const bool seqEqual = same == ours.size() && same == last.lines.size();
    std::printf("race-menu-check: primitives: ours %zu, capture %zu 2D + %zu excluded (3D car, drawing area %d,%d - %d,%d); %s", ours.size(), last.lines.size(),
                last.carPrims, last.carX0, last.carY0, last.carX1, last.carY1, seqEqual ? "equal sequences\n" : "");
    if (!seqEqual) {
        std::printf("first difference at %zu\n  ours     %s\n  captured %s\n", same, same < ours.size() ? ours[same].c_str() : "(end)",
                    same < last.lines.size() ? last.lines[same].c_str() : "(end)");
        for (size_t k = 0; k + 1 < frames.size(); k++)
            if (frames[k].lines == ours) std::printf("race-menu-check: (ours equals the capture's frame %zu of %zu)\n", k + 1, frames.size());
    }

    // VRAM of the assets.
    const size_t fontDiff = CompareVramRegion(assets.vram, vram, 384, 0, 128, 256);
    const size_t picDiff = CompareVramRegion(assets.vram, vram, 384, 256, 64, 235);
    std::printf("race-menu-check: VRAM font pages 6/7: %zu differing words, arcade/setting.tim page 0x16: %zu\n", fontDiff, picDiff);

    // Pixels.
    const MenuCanvas canvas = screens::RenderRaceMenuFrame(assets, prims, !ps1);
    const int W = RaceMenuAssets::kScreenWidth, H = RaceMenuAssets::kScreenHeight;
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    size_t diff = 0, excluded = 0;
    int minX = W, minY = H, maxX = -1, maxY = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint16_t o = canvas.At(x, y), c = vram[size_t(y) * 1024 + size_t(x)];
            const bool out = last.carPrims > 0 && x >= last.carX0 && x <= last.carX1 && y >= last.carY0 && y <= last.carY1;
            const bool d = !out && (o & 0x7FFF) != (c & 0x7FFF);
            if (out) excluded++;
            if (d) {
                diff++;
                minX = std::min(minX, x), minY = std::min(minY, y), maxX = std::max(maxX, x), maxY = std::max(maxY, y);
            }
            auto put = [&](int column, uint16_t v) {
                uint8_t* p = &side[(size_t(y) * W * 3 + size_t(column * W + x)) * 4];
                p[0] = uint8_t((v & 31) << 3), p[1] = uint8_t(((v >> 5) & 31) << 3), p[2] = uint8_t(((v >> 10) & 31) << 3);
            };
            put(0, o);
            put(1, c);
            uint8_t* e = &side[(size_t(y) * W * 3 + size_t(2 * W + x)) * 4];
            const uint8_t grey = uint8_t((((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) << 3) / 12);
            e[0] = d ? 255 : out ? 0 : grey, e[1] = d ? 0 : out ? 0 : grey, e[2] = d ? 0 : out ? 160 : grey;
        }
    std::printf("race-menu-check: %zu differing pixels", diff);
    if (diff) std::printf(" (box %d,%d - %d,%d)", minX, minY, maxX, maxY);
    std::printf(", %zu pixels excluded (3D car area) -> %s\n", excluded, sidePath.c_str());
    if (!std::filesystem::path(sidePath).parent_path().empty()) std::filesystem::create_directories(std::filesystem::path(sidePath).parent_path());
    WritePngRgba(sidePath, W * 3, H, side);

    // The 3D model (0x80048754): the floor disc exactly, the model's silhouette with a tolerance (the renderer draws it).
    bool modelOk = true;
    if (model.has_value() != (last.carPrims > 0)) {
        std::printf("race-menu-check: model: ours %s, the capture %s\n", model ? "drawn" : "not drawn", last.carPrims ? "has model primitives" : "has none");
        modelOk = false;
    } else if (model) {
        std::vector<uint8_t> overlay(size_t(W) * H * 4, 255);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint16_t c = vram[size_t(y) * 1024 + size_t(x)];
                const uint8_t grey = uint8_t((((c & 31) + ((c >> 5) & 31) + ((c >> 10) & 31)) << 3) / 6);
                uint8_t* p = &overlay[(size_t(y) * W + size_t(x)) * 4];
                p[0] = p[1] = p[2] = grey;
            }
        Mask captured(W, H);
        modelOk = CheckModel(vol, ram, *model, last, &overlay, W, H, &captured);
        std::string base = sidePath;
        if (base.size() > 4 && base.compare(base.size() - 4, 4, ".png") == 0) base.resize(base.size() - 4);
        WritePngRgba(base + "_model.png", W, H, overlay);
        std::printf("race-menu-check: model overlay (red ours, green the original's, yellow both) -> %s_model.png\n", base.c_str());
        if (vulkan) modelOk = VulkanModelCheck(disc, vol, ram, prims, *model, captured, vram, canvas, base) && modelOk;
    }
    return (diff == 0 && seqEqual && modelOk) ? 0 : 1;
}
