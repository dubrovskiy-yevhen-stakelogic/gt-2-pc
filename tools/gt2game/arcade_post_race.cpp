// After an arcade race: RESULTS and the post-race menu on the arcade disc (arcade_post_race.h).
#include "arcade_post_race.h"

#include "platform/os/keys.h"
#include "platform/os/paths.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "game_window.h"
#include "panel.h"
#include "gt2export/png_writer.h"
#include "gt2formats/car_info.h"
#include "gt2formats/car_texture.h"
#include "gt2formats/course_map.h"
#include "game/arcade/arcade_results.h"
#include "gt2view/race_card_screens.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/hud_assets.h"
#include "gt2formats/overlay_data.h"
#include "gt2formats/title_assets.h"
#include "gt2view/menu_view.h"
#include "gt2view/race_result_screens.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"

using namespace gt2;
namespace pad = gt2::menu_list_pad;

namespace gt2game {
namespace {

// Arcade v1.1: the race overlay's copy of .text/data-race.txd (member 0 0x80028C4C: block language * 0x167F -> 0x801C6940).
constexpr uint32_t kArcadeRaceText = 0x801C6940u, kArcadeRaceTextStride = 0x167F;
// Where the arcade text block sits in the Simulation-layout copy (behind the Simulation block's 0x1915 bytes).
constexpr uint32_t kExtensionOffset = 0x2000;

// A build image copied to the Simulation addresses of [simBase, simEnd) in `scope`: ranges applied from the lowest priority
// (aligned runs) to the highest (facts), as ExeProfile::TryData orders them. `inverse[a - build.base]` receives the Simulation
// address of build byte a (0 = none).
GuestImage SimLayout(const GuestImage& build, const ExeProfile& p, int scope, uint32_t simBase, uint32_t simEnd, std::vector<uint32_t>& inverse) {
    GuestImage out;
    out.base = simBase;
    out.bytes.assign(simEnd - simBase, 0);
    out.module = build.module;
    inverse.assign(build.bytes.size(), 0);
    for (const ProfileRange::Kind kind : {ProfileRange::kAligned, ProfileRange::kRefRun, ProfileRange::kFact})
        for (const ProfileRange& r : p.ranges) {
            if (r.kind != kind || r.scope != scope) continue;
            const uint32_t from = std::max(r.simStart, simBase), to = std::min(r.simEnd, simEnd);
            for (uint32_t s = from; s < to; s++) {
                const uint32_t a = uint32_t(int64_t(s) + r.delta);
                if (a < build.base || a - build.base >= build.bytes.size()) continue;
                out.bytes[s - simBase] = build.bytes[a - build.base];
                inverse[a - build.base] = s;
            }
        }
    return out;
}

// The Simulation address of a build RAM address (scope -1 facts, then reference runs, as TryData orders them); 0 = none.
uint32_t SimRamAddress(const ExeProfile& p, uint32_t build) {
    for (const ProfileRange::Kind kind : {ProfileRange::kFact, ProfileRange::kRefRun})
        for (const ProfileRange& r : p.ranges) {
            if (r.kind != kind || r.scope != -1) continue;
            const uint32_t s = uint32_t(int64_t(build) - r.delta);
            if (s >= r.simStart && s < r.simEnd) return s;
        }
    return 0;
}

// The pointers of a Simulation-layout copy back to Simulation addresses: every aligned word that is the address of a mapped
// byte of one of the build images, of the arcade race text, or of RAM a profile range maps.
void TranslatePointers(GuestImage& image, const GuestImage& ovl0, const std::vector<uint32_t>& ovl0Inverse, const GuestImage& exe,
                       const std::vector<uint32_t>& exeInverse, uint32_t textSize, const ExeProfile* profile) {
    const uint32_t ovl0End = ovl0.End();
    for (size_t o = 0; o + 4 <= image.bytes.size(); o += 4) {
        uint32_t w;
        std::memcpy(&w, image.bytes.data() + o, 4);
        uint32_t t = 0;
        if (w >= ovl0.base && w < ovl0End) t = ovl0Inverse[w - ovl0.base];      // member 0 (below its end: the race map)
        else if (w >= ovl0End && w >= exe.base && w < exe.End()) t = exeInverse[w - exe.base]; // the resident executable
        else if (w >= kArcadeRaceText && w < kArcadeRaceText + textSize) t = RaceMenuAssets::kRaceTextBase + kExtensionOffset + (w - kArcadeRaceText);
        else if (w >= exe.End() && w < 0x80200000u) t = SimRamAddress(*profile, w); // RAM / BSS (e.g. the font descriptors 0x801C91xx)
        if (t) std::memcpy(image.bytes.data() + o, &t, 4);
    }
}

// Data words of member 0 that look like addresses (TranslatePointers maps every such word): CHANGE PARTS' part entries (the group
// tables 0x8005C364 / 0x8005C384 / 0x8005C3A4 -> 0x90-byte records, part p at + 0x10 + p * 0x10 with + 0xC s16 kind, s8 picture, u8
// flags; flags bit 7, a power part, makes the word 0x80xxxxxx: e.g. the muffler's 0x80060011 read as an executable address). They
// get the arcade image's words back (the address through the profile).
void RestoreDataWords(GuestImage& simLayout, const GuestImage& ovl0, const ExeProfile& p) {
    for (const uint32_t table : {0x8005C364u, 0x8005C384u, 0x8005C3A4u})
        for (uint32_t g = 0; g < 8; g++) {
            const uint32_t record = simLayout.Get<uint32_t>(table + g * 4);
            if (!simLayout.Contains(record, 0x90)) continue;
            for (uint32_t part = 0; part < 8; part++) {
                const uint32_t e = record + 0x10 + part * 0x10;
                if (simLayout.Get<uint32_t>(e) == 0) break;
                const std::optional<uint32_t> at = p.TryData(e + 0xC, 0);
                if (!at) throw std::runtime_error("arcade race menus: no arcade address of the CHANGE PARTS entry word 0x" + std::to_string(e + 0xC));
                const uint32_t word = ovl0.Get<uint32_t>(*at);
                std::memcpy(simLayout.bytes.data() + (e + 0xC - simLayout.base), &word, 4);
            }
        }
}

MenuListPad ReadPad(GameWindow& window, uint32_t& previousHeld, int& repeatTimer) {
    auto bit = [](int key) -> uint32_t {
        switch (key) {
        case gt2::keys::kUp: return pad::kUp;
        case gt2::keys::kDown: return pad::kDown;
        case gt2::keys::kLeft: return pad::kLeft;
        case gt2::keys::kRight: return pad::kRight;
        case gt2::keys::kReturn: return pad::kCross;
        case gt2::keys::kSpace: return pad::kCircle;
        case gt2::keys::kBack:
        case gt2::keys::kEscape: return pad::kTriangle;
        case 'S': return pad::kStart; // the title's keys: S = Start, Q / W = L1 / R1 (the card list's delete mode)
        case 'Q': return pad::kL1;
        case 'W': return pad::kR1;
        default: return 0;
        }
    };
    uint32_t held = 0;
    for (int k : {gt2::keys::kUp, gt2::keys::kDown, gt2::keys::kLeft, gt2::keys::kRight, gt2::keys::kReturn, gt2::keys::kSpace, gt2::keys::kBack, gt2::keys::kEscape, int('S'), int('Q'), int('W')})
        if (window.Held(k)) held |= bit(k);
    uint32_t presses = 0;
    for (int k : window.PressedKeys()) presses |= bit(k);
    MenuListPad p;
    p.held = held;
    p.pressed = (held & ~previousHeld) | presses;
    p.released = previousHeld & ~held;
    const uint32_t dirs = held & (pad::kUp | pad::kDown | pad::kLeft | pad::kRight);
    if (dirs && dirs == (previousHeld & dirs)) {
        if (++repeatTimer >= 20 && (repeatTimer - 20) % 5 == 0) p.repeat = dirs;
    } else {
        repeatTimer = 0;
    }
    previousHeld = held;
    return p;
}

// Dev aid (comparisons with gt2play --prims captures): the frame's primitives on the software canvas with the interpreter
// GPU's rules (the captures' rasteriser; sprites as arcade_mode.cpp draws them) against the capture's VRAM (the 352 x 480
// drawing area at (0, 0), 5-bit channels). Pixels inside `clip` (the 3D model's environment: the car is not in the primitives)
// are counted apart. Writes capture | ours | differences.
void CompareFrame(const RaceMenuAssets& a, const std::vector<MenuPrim>& prims, const std::string& capture, const std::string& sidePath, int clipX, int clipY, int clipW,
                  int clipH) {
    MenuCanvas canvas;
    canvas.rules = MenuCanvas::Rules::kInterpreter;
    for (const MenuPrim& p : prims) {
        if (p.kind != MenuPrim::kSprite) {
            canvas.Draw(a.vram, p);
            continue;
        }
        const int mr = int(p.colour[0] & 0xFF), mg = int((p.colour[0] >> 8) & 0xFF), mb = int((p.colour[0] >> 16) & 0xFF);
        const int mode = (p.tpage >> 5) & 3;
        for (int j = 0; j < p.h; j++)
            for (int i = 0; i < p.w; i++) {
                const int x = p.x[0] + i, y = p.y[0] + j;
                if (x < 0 || y < 0 || x >= MenuCanvas::kWidth || y >= MenuCanvas::kHeight) continue;
                const uint16_t t = a.vram.Sample(p.tpage, p.clut, uint8_t(p.u + i), uint8_t(p.v + j));
                if (t == 0) continue;
                int c[3] = {((t & 31) << 3) * mr / 128, (((t >> 5) & 31) << 3) * mg / 128, (((t >> 10) & 31) << 3) * mb / 128};
                if (p.semi && (t & 0x8000)) {
                    const uint16_t d = canvas.At(x, y);
                    const int back[3] = {(d & 31) << 3, ((d >> 5) & 31) << 3, ((d >> 10) & 31) << 3};
                    for (int k = 0; k < 3; k++) {
                        switch (mode) {
                        case 0: c[k] = (back[k] + c[k]) / 2; break;
                        case 1: c[k] = back[k] + c[k]; break;
                        case 2: c[k] = back[k] - c[k]; break;
                        default: c[k] = back[k] + c[k] / 4; break;
                        }
                    }
                }
                for (int& v : c) v = (std::clamp(v, 0, 255) >> 3) << 3;
                canvas.Fill(x, y, 1, 1, uint8_t(c[0]), uint8_t(c[1]), uint8_t(c[2]));
            }
    }
    std::FILE* f = std::fopen(capture.c_str(), "rb");
    if (!f) throw std::runtime_error("cannot read " + capture);
    std::vector<uint16_t> vram(1024 * 512);
    const size_t n = std::fread(vram.data(), 2, vram.size(), f);
    std::fclose(f);
    if (n != vram.size()) throw std::runtime_error(capture + ": not a 1024 x 512 VRAM dump");
    const int W = RaceMenuAssets::kScreenWidth, H = RaceMenuAssets::kScreenHeight;
    std::vector<uint8_t> side(size_t(W) * 3 * H * 4, 255);
    size_t outside = 0, inside = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint16_t o = canvas.At(x, y), c = vram[size_t(y) * 1024 + size_t(x)];
            const bool d = (o & 0x7FFF) != (c & 0x7FFF);
            const bool clipped = x >= clipX && x < clipX + clipW && y >= clipY && y < clipY + clipH;
            if (d) (clipped ? inside : outside)++;
            auto put = [&](int col, uint16_t v) {
                uint8_t* q = &side[(size_t(y) * W * 3 + size_t(col * W + x)) * 4];
                q[0] = uint8_t((v & 31) << 3), q[1] = uint8_t(((v >> 5) & 31) << 3), q[2] = uint8_t(((v >> 10) & 31) << 3);
            };
            put(0, c);
            put(1, o);
            uint8_t* e = &side[(size_t(y) * W * 3 + size_t(2 * W + x)) * 4];
            e[0] = d ? 255 : (clipped ? 40 : 0), e[1] = d && clipped ? 160 : 0, e[2] = 0;
        }
    WritePngRgba(sidePath, W * 3, H, side);
    std::printf("post-race compare vs %s: %zu differing pixels outside the model's rectangle, %zu inside -> %s\n", capture.c_str(), outside, inside, sidePath.c_str());
}

// Dev aid: <variable>="field:out.png,..." - the window's frame of that field (the renderer's, with the 3D car) as a PNG.
std::string ShotPathOf(const char* variable, int field) {
    const char* spec = std::getenv(variable);
    if (!spec || !*spec) return {};
    const std::string all = spec;
    for (size_t pos = 0; pos < all.size();) {
        size_t end = all.find(',', pos);
        if (end == std::string::npos) end = all.size();
        const std::string item = all.substr(pos, end - pos);
        pos = end + 1;
        const size_t colon = item.find(':');
        if (colon != std::string::npos && std::atoi(item.c_str()) == field) return item.substr(colon + 1);
    }
    return {};
}

} // namespace

RaceMenuAssets LoadArcadeRaceMenuAssets(const DiscImage& disc, const GtfsVolume& vol) {
    const ExeProfile& p = ProfileOf(disc);
    if (!p.arcade) throw std::runtime_error("arcade race menus: not the arcade disc");
    if (p.build == ExeBuild::kArcadeEu) return RaceMenuAssets::Load(disc, vol, RaceMenuAssets::Pictures::kSettings);
    const GuestImage ovl0 = UiLayout(LoadOverlayImage(disc, kRaceOverlayIndex),true);
    const GuestImage exe = UiLayout(LoadExeImage(disc),true);
    RaceMenuAssets a;
    a.language = 1;
    a.pictures = RaceMenuAssets::Pictures::kSettings;
    std::vector<uint32_t> ovl0Inverse, exeInverse;
    a.ovl0 = SimLayout(ovl0, p, 0, kOverlayLoadAddress, ExeProfile::kSimRaceOverlayEnd, ovl0Inverse);
    a.exe = SimLayout(exe, p, -1, exe.base, exe.base + uint32_t(exe.bytes.size() + 0x400), exeInverse);
    a.ovl0.profile = a.exe.profile = &SimProfile();
    a.exe.fileName = exe.fileName;

    // The race text: the arcade block of the language, placed at the Simulation string addresses of the reference runs
    // (each run's bytes plus the string at its last reference), and as a whole behind them (the pointers of the images).
    const std::vector<uint8_t> txd = vol.Read(".text/data-race.txd");
    if (txd.size() < size_t(a.language + 1) * kArcadeRaceTextStride) throw std::runtime_error("arcade race menus: data-race.txd has no language block");
    const std::span<const uint8_t> text(txd.data() + size_t(a.language) * kArcadeRaceTextStride, kArcadeRaceTextStride);
    a.raceText.assign(kExtensionOffset + kArcadeRaceTextStride + 1, 0);
    std::copy(text.begin(), text.end(), a.raceText.begin() + kExtensionOffset);
    const uint32_t simText = RaceMenuAssets::kRaceTextBase, simTextEnd = simText + RaceMenuAssets::kRaceTextStride;
    size_t placed = 0;
    for (const ProfileRange& r : p.ranges) {
        if (r.scope != -1 || r.kind == ProfileRange::kAligned || r.simEnd <= simText || r.simStart >= simTextEnd) continue;
        uint32_t s = std::max(r.simStart, simText);
        auto arcadeByte = [&](uint32_t sim) -> int {
            const uint32_t at = uint32_t(int64_t(sim) + r.delta);
            return at >= kArcadeRaceText && at - kArcadeRaceText < text.size() ? text[at - kArcadeRaceText] : -1;
        };
        for (; s < std::min(r.simEnd, simTextEnd); s++) {
            const int b = arcadeByte(s);
            if (b >= 0) a.raceText[s - simText] = uint8_t(b);
        }
        for (; s < simTextEnd; s++) { // the rest of the string at the run's last reference
            const int b = arcadeByte(s);
            if (b < 0) break;
            a.raceText[s - simText] = uint8_t(b);
            if (b == 0) break;
        }
        placed++;
    }
    TranslatePointers(a.ovl0, ovl0, ovl0Inverse, exe, exeInverse, kArcadeRaceTextStride, &p);
    TranslatePointers(a.exe, ovl0, ovl0Inverse, exe, exeInverse, kArcadeRaceTextStride, &p);
    RestoreDataWords(a.ovl0, ovl0, p);
    // data-global.txd of the arcade title is another build (docs/formats/title.md section 8; member 1 copies its block to
    // 0x801EF0E0): its strings at the Simulation addresses the profile maps (the reference runs of the EXE's code, e.g. the
    // keyboard's OK / CANCEL 0x801EFBE4 / 0x801EFBE7 at -0x5D8), each run's last string completed up to its NUL.
    {
        const TitleAssets title = TitleAssets::LoadArcade(disc, vol);
        const std::vector<uint8_t>& block = title.globalText;
        const uint32_t base = title.globalTextAt;
        a.globalText.assign(RaceMenuAssets::kGlobalTextStride + 1, 0);
        int64_t lastDelta = 0;
        bool inRun = false;
        for (uint32_t s = RaceMenuAssets::kGlobalTextBase; s < RaceMenuAssets::kGlobalTextBase + RaceMenuAssets::kGlobalTextStride; s++) {
            const std::optional<uint32_t> at = p.TryData(s, -1);
            int64_t delta = 0;
            if (at) delta = int64_t(*at) - int64_t(s), inRun = true, lastDelta = delta;
            else if (inRun) delta = lastDelta; // past the run: the string at its last reference goes on to its NUL
            else continue;
            const int64_t arcade = int64_t(s) + delta - int64_t(base);
            if (arcade < 0 || arcade >= int64_t(block.size())) {
                inRun = false;
                continue;
            }
            const uint8_t b = block[size_t(arcade)];
            a.globalText[s - RaceMenuAssets::kGlobalTextBase] = b;
            if (!at && b == 0) inRun = false;
        }
    }
    a.licenceInfo = vol.Read("arcade/license_info_us");

    UploadTimImage(a.vram, vol.Read("arcade/arc_font.tim"), 0x180, 0);
    UploadTimImage(a.vram, vol.Read("arcade/setting.tim"), 0x180, 0x100);
    // The fonts as RaceMenuAssets::Load sets them (arc_fontinfo + the four descriptors of 0x80047EA0 state 1).
    GuestImage info;
    info.base = RaceMenuAssets::kFontInfoAddress;
    info.bytes = vol.Read("arcade/arc_fontinfo");
    auto u32 = [&](size_t o) {
        if (o + 4 > info.bytes.size()) throw std::runtime_error("arc_fontinfo: short");
        uint32_t v;
        std::memcpy(&v, info.bytes.data() + o, 4);
        return v;
    };
    if (info.bytes.size() < 0x24 || u32(0) != 8) throw std::runtime_error("arc_fontinfo: unexpected header");
    info.bytes.resize((info.bytes.size() + 3) & ~size_t(3));
    const uint32_t descriptors = info.End();
    std::vector<uint8_t> extra;
    for (int f = 0; f < 4; f++) {
        const uint32_t glyphs = RaceMenuAssets::kFontInfoAddress + u32(4 + size_t(f) * 8), kerning = RaceMenuAssets::kFontInfoAddress + u32(8 + size_t(f) * 8);
        uint8_t d[16] = {};
        std::memcpy(d, &glyphs, 4);
        std::memcpy(d + 4, &kerning, 4);
        d[8] = RaceMenuAssets::kFontCells[f];
        extra.insert(extra.end(), d, d + 16);
    }
    info.bytes.insert(info.bytes.end(), extra.begin(), extra.end());
    for (int f = 0; f < 4; f++) {
        HudFont& font = a.fonts[size_t(f)];
        font = LoadHudFont(info, descriptors + uint32_t(f) * 16);
        font.tpage = RaceMenuAssets::kFontPage;
        font.clutBase = uint16_t((RaceMenuAssets::kFontPage & 0xF) * 4 + (RaceMenuAssets::kFontPage & 0x10) * 0x400);
    }
    std::printf("arcade race menus: member 0 / EXE in the Simulation layout, race text at %zu reference runs (%s)\n", placed, a.Text(0x801C6E29u).c_str());
    return a;
}

std::unique_ptr<Panels> LoadArcadeRacePanels(gt2view::VkSceneRenderer& renderer, const DiscImage& disc, const GtfsVolume& vol) {
    try {
        return std::make_unique<Panels>(renderer, disc, vol);
    } catch (const std::exception& e) {
        std::printf("arcade: no pause / race-end display (%s)\n", e.what());
        return nullptr;
    }
}

std::vector<std::string> ArcadeRaceSlotNames(const std::array<uint8_t, 0x58C>& raceBlock, size_t count) {
    std::vector<std::string> names;
    for (size_t car = 0; car < count && car < 6; car++) {
        std::string n;
        for (size_t o = 0xEC + car * 0xD0; o < raceBlock.size() && raceBlock[o] && n.size() < 0x40; o++) n.push_back(char(raceBlock[o]));
        names.push_back(n);
    }
    return names;
}

ArcadeBattleLaps BattleLapRows(const sim::PlayerResults& p1, const sim::PlayerResults& p2) { // 0x80051294 .. 0x8005132C
    // 0x8005E378(record, lap): k = lap - (record + 2 - record + 4); the kept lap k's time, 0xFFFFFFFF outside [0, record + 4)
    auto lapTime = [](const sim::PlayerResults& r, int lap) -> uint32_t {
        const int k = lap - (int(r.lapNumber) - int(r.count));
        if (k < 0 || k >= r.count || k >= 10) return 0xFFFFFFFFu;
        return uint32_t(r.laps[k].time);
    };
    int count = p1.count, first = int(p1.lapNumber) - int(p1.count);
    if (p1.count < p2.count) count = p2.count, first = int(p2.lapNumber) - int(p2.count);
    ArcadeBattleLaps out;
    out.firstLap = first + 1;
    for (int i = 0; i < count && i < 10; i++) {
        out.laps1.push_back(lapTime(p1, i + first));
        out.laps2.push_back(lapTime(p2, i + first));
    }
    return out;
}

ArcadePostRaceInput BattlePostRaceInput(const sim::PlayerResults& p1, const sim::PlayerResults& p2, const std::array<uint16_t, 2>& wins,
                                        const std::string& course, const std::array<uint32_t, 2>& carIds, const std::array<int, 2>& paints) {
    ArcadePostRaceInput in;
    in.battle = true;
    in.finished = p1.position > 0 || p2.position > 0; // 0x80016D6C
    std::array<uint8_t, arcade::kBattleWins + 4> counts{};
    in.winner = arcade::ApplyBattleResult(counts, p1.position, p2.position); // the rule only (a scratch career)
    in.place = in.winner + 1;
    in.totalTime = uint32_t(p1.finishTime);   // W+8 = + 0xF8
    in.totalTime2 = uint32_t(p2.finishTime);  // W+0xC
    in.fastestLap = uint32_t(p1.best.time);   // W+0x10 = + 0xD0
    in.fastestLap2 = uint32_t(p2.best.time);  // W+0x14
    const ArcadeBattleLaps rows = BattleLapRows(p1, p2);
    in.laps = rows.laps1;
    in.laps2 = rows.laps2;
    in.firstLap = rows.firstLap;
    in.course = course;
    in.wins = wins;
    in.carId = carIds[0];
    in.paint = paints[0];
    in.winnerCarId = carIds[size_t(in.winner)];
    in.winnerPaint = paints[size_t(in.winner)];
    return in;
}

ArcadePostRaceInput BattlePostRaceInputFromRam(const DiscImage& disc, const GtfsVolume& vol, const std::vector<uint8_t>& ram) {
    const ExeProfile& p = ProfileOf(disc);
    auto at = [&](uint32_t sim, size_t size) -> const uint8_t* {
        const uint32_t a = p.Race(sim) & 0x1FFFFF;
        if (a + size > ram.size()) throw std::runtime_error("battle post-race test: address outside the RAM image");
        return ram.data() + a;
    };
    sim::PlayerResults r[2];
    std::memcpy(&r[0], at(0x801D5E88u, sizeof r[0]), sizeof r[0]);
    std::memcpy(&r[1], at(0x801DA3A0u, sizeof r[1]), sizeof r[1]);
    std::array<uint16_t, 2> wins{};
    std::memcpy(wins.data(), at(0x801C99DCu, 4), 4); // career + 0xFC / + 0xFE
    const uint8_t* block = at(0x801D585Cu, 0x5C + 2 * 0xD0);
    std::string course;
    for (size_t k = 0; k < 0x20 && block[0x20 + k]; k++) course.push_back(char(block[0x20 + k]));
    std::array<uint32_t, 2> cars{};
    std::array<int, 2> paints{};
    for (size_t i = 0; i < 2; i++) {
        std::memcpy(&cars[i], block + 0x5C + i * 0xD0, 4);
        try { // the slot's paint code (+4) -> the index of that paint in the car's .cdp list (race_common.cpp)
            paints[i] = std::max(0, ParseCarTexture(vol.Read("carobj/" + UnpackCarId(cars[i]) + ".cdp")).PaintIndex(block[0x5C + i * 0xD0 + 4]));
        } catch (const std::exception&) {
            paints[i] = 0;
        }
    }
    std::printf("battle post-race test: %s, places %d / %d, totals %s / %s, laps kept %d / %d, wins %u / %u\n", course.c_str(), int(r[0].position),
                int(r[1].position), FormatRaceTime(uint32_t(r[0].finishTime)).c_str(), FormatRaceTime(uint32_t(r[1].finishTime)).c_str(), int(r[0].count),
                int(r[1].count), unsigned(wins[0]), unsigned(wins[1]));
    return BattlePostRaceInput(r[0], r[1], wins, course, cars, paints);
}

// The views of the arcade post-race runs (Simulation addresses): the menu's wait view 0x8005AE0C (init 0x80049C68: V+14 = 20,
// update 0x80049CB0 pushes the menu when it reaches 0), the leave view 0x8005AE30 (init 0x80049D28: V+14 = 20; update 0x80049D58
// returns 4 once the counter is below 0, i.e. at the 21st update: the manager's task ends).
constexpr uint32_t kMenuWaitView = 0x8005AE0Cu, kLeaveView = 0x8005AE30u;
constexpr int kMenuWaitFields = 20, kLeaveFields = 21;
// Fields between the end of one view-manager run and the first update of the next (the arcade loop's 0x800832F8 wait for the
// task's end, 0x800471F4, the task's start 0x800472E0): 0 against the captures of work/play/arcade_2p/post1 and the mode 4
// captures of work/play/arcade_results/cap; the capture run work/play/arcade_2p/post_p1 starts the second run one field earlier
// (a phase of the original's task switch, not modelled: its menu frames equal ours one field earlier).
constexpr int kManagerRestartFields = 0;

ArcadePostRaceChoice RunArcadePostRace(GameWindow& window, const RaceMenuAssets& a, const GtfsVolume& vol, const ArcadePostRaceInput& in, bool withResults,
                                       bool squarePixels, const std::function<void(const std::vector<int>&)>& sounds, const ViewMusic& music, ArcadeCardContext* cards) {
    enum class Stage { kWaitResults, kResults, kLeaveResults, kWaitMenu, kMenu, kCard, kLeave };
    screens::SessionViewStack stack; // the view manager 0x800474F4 / 0x800479AC with its stack (push 0x800483A4, back 0x800483D8)
    Stage stage = Stage::kWaitMenu;
    ArcadePostRaceChoice choice = ArcadePostRaceChoice::kExit;
    // 0x80016D8C: mode 4 - the player finished; mode 0 - either player did (0x80016D6C)
    const bool results = withResults && in.finished && (in.place > 0 || in.battle);
    auto menuInput = [&](bool first) {
        screens::PostMenuInput m;
        m.mode = in.battle ? 0 : 4; // race block + 0x0A: "SINGLE RACE" / "2PLAYER BATTLE", the rows 0x8005AD58 (Replay / Try Again / Save Replay ... / Exit)
        m.wins = in.wins;
        m.place = in.finished ? in.place : 0;
        m.totalTime = in.totalTime;
        m.fastestLap = in.fastestLap;
        m.course = in.course;
        m.firstEntry = first;
        return m;
    };
    const bool comparison = std::getenv("GT2_ARCADE_POST_TEST") != nullptr || std::getenv("GT2_ARCADE_BATTLE_POST_TEST") != nullptr; // the original's frames (row enabled): checks
    auto rowRules = [&](screens::PostRaceMenuView& v) {
        for (screens::PostRaceMenuView::Row& row : v.rows)
            if (row.action == -2 && !comparison && !cards) row.enabled = false; // Save Replay ...: needs the replay card manager's context
    };
    auto makeMenu = [&](bool first) {
        auto v = std::make_unique<screens::PostRaceMenuView>(a);
        v->Setup(menuInput(first));
        rowRules(*v);
        return v;
    };
    if (results) { // 0x80016D8C: 0x800471F4(M, 0x8005B710) - the wait view, then RESULTS
        stage = Stage::kWaitResults;
        stack.Start(std::make_unique<screens::WaitView>(a, 0x8005B7A0u, 24), false);
    } else {
        stack.Start(std::make_unique<screens::WaitView>(a, kMenuWaitView, kMenuWaitFields), false);
        if (music) music(8); // 0x80049C68: 0x800481C8(8)
    }
    // The update's result `r` (1 = the view pushed its next view / left, 2 = back) -> the next step; false = the views end.
    auto next = [&](int r) -> bool {
        switch (stage) {
        case Stage::kWaitResults: {
            if (r != 1) return true;
            stage = Stage::kResults;
            auto v = std::make_unique<screens::ResultsView>(a);
            screens::ResultsInput ri;
            ri.place = in.place;
            ri.totalTime = in.totalTime;
            ri.fastestLap = in.fastestLap;
            ri.laps = in.laps;
            ri.firstLap = in.firstLap;
            ri.course = in.course;
            ri.saveBar = false; // the arcade RESULTS ends with the dialog "Next" (captured: work/re/arcade_results/r3)
            ri.vsync = in.vsync;
            ri.battle = in.battle;
            ri.winner = in.winner;
            ri.totalTime2 = in.totalTime2;
            ri.fastestLap2 = in.fastestLap2;
            ri.laps2 = in.laps2;
            v->Setup(ri);
            stack.Push(std::move(v));
            if (music) music(in.battle || in.place == 1 ? 8 : 19); // the setup's 0x800481C8 (0x800515F4): 8, or 19 below 1st in 1P
            return true;
        }
        case Stage::kResults: // "Next": 0x800483A4(M, 0x8005AE30) - the forward transition into the leave view
            if (r != 1) return true;
            stage = Stage::kLeaveResults;
            stack.Push(std::make_unique<screens::WaitView>(a, kLeaveView, kLeaveFields));
            if (music) music(-1); // 0x80049D28: 0x800481E8
            return true;
        case Stage::kLeaveResults: // the leave view returned 4: the manager ends; 0x80016DC0: a new run 0x800471F4(M, 0x8005AD7C)
            if (r != 1) return true;
            stage = Stage::kWaitMenu;
            stack.Start(std::make_unique<screens::WaitView>(a, kMenuWaitView, kMenuWaitFields + kManagerRestartFields), false);
            if (music) music(8); // 0x80049C68: 0x800481C8(8)
            return true;
        case Stage::kWaitMenu:
            if (r != 1) return true;
            stage = Stage::kMenu;
            stack.Push(makeMenu(true));
            return true;
        case Stage::kMenu: { // a row: M + 0x7C = its action, 0x800483A4(M, 0x8005AE30) - the transition into the leave view
            if (r != 1) return true;
            auto* v = dynamic_cast<screens::PostRaceMenuView*>(stack.Top());
            const int action = v ? v->Action() : 2;
            if (action == -2 && cards) { // "Save Replay ...": 0x800483A4(M, 0x8005B51C), the executable's card manager (mode 0, replay)
                stage = Stage::kCard;
                auto card = std::make_unique<screens::CardView>(a, *cards->cardAssets, *cards->text, screens::CardView::kSaveReplayView, cards->slots);
                auto [payload, desc] = cards->replayPayload();
                card->Manager().SetSaveData(std::move(payload), desc);
                stack.Push(std::move(card));
                return true;
            }
            choice = action == 0 ? ArcadePostRaceChoice::kReplay : action == 1 ? ArcadePostRaceChoice::kTryAgain : ArcadePostRaceChoice::kExit;
            stage = Stage::kLeave;
            stack.Push(std::make_unique<screens::WaitView>(a, kLeaveView, kLeaveFields));
            if (music) music(-1); // 0x80049D28: 0x800481E8
            return true;
        }
        case Stage::kCard: { // the manager exited (code 2): back to the menu, its setup with 1 (0x80049D90(1): the list kept)
            if (r != 2) return true;
            if (auto* card = dynamic_cast<screens::CardView*>(stack.Top()))
                for (const std::string& line : card->Manager().log) std::printf("arcade card: %s\n", line.c_str());
            stack.Pop();
            if (auto* m = dynamic_cast<screens::PostRaceMenuView*>(stack.Top())) {
                m->Setup(menuInput(false));
                rowRules(*m);
            }
            stage = Stage::kMenu;
            return true;
        }
        case Stage::kLeave:
            return r != 1;
        }
        return false;
    };

    gt2view::VkSceneRenderer& renderer = window.Renderer();
    gt2view::MenuView view(renderer, 0);
    view.SetFrameSize(RaceMenuAssets::kScreenWidth, RaceMenuAssets::kScreenHeight);
    view.SetInterpolatedPolygons(true);
    view.UploadVram(a.vram);
    gt2view::MenuCarView car(renderer, vol);
    car.SetFrameSize(RaceMenuAssets::kScreenWidth, RaceMenuAssets::kScreenHeight);
    car.SetReflection(9, uint16_t((576 / 16) | (152 << 6)), 0); // no reflection pass: the race's map is not in this VRAM
    // mode 0: RESULTS draws the winner's car (0x80050B4C(winner): *(0x800A9F00 + winner * 0xB40)), the menu car 0 - both cars in
    // the one view's car slots (two MenuCarViews would share the renderer's slots)
    const bool carLoaded = in.battle ? car.UsePair(in.carId, in.winnerCarId) : car.Use(in.carId);
    float clear[3];
    std::copy(std::begin(renderer.clearColor), std::end(renderer.clearColor), clear);
    renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0.0f;
    uint32_t previousHeld = 0;
    int repeatTimer = 0;
    bool closed = true;
    const auto fieldTime = std::chrono::nanoseconds(16'683'333);
    const screens::PostRaceView* lastView = nullptr;
    while (window.BeginFrame()) {
        const MenuListPad p = ReadPad(window, previousHeld, repeatTimer);
        const int r = stack.Update(&p);
        if (stack.Top() && sounds) sounds(stack.Top()->sounds);
        if (r != 0 && !next(r)) {
            closed = false;
            break;
        }
        size_t modelAt = 0;
        std::optional<screens::PostRaceModel> model;
        MenuFrame frame;
        frame.prims = stack.Frame(a, modelAt, model);
        frame.layer3dAt = modelAt;
        // the view whose model is drawn (SessionViewStack::Frame: the top's, else the leaving view's)
        const screens::PostRaceView* modelView = stack.Top() && stack.Top()->Model() ? stack.Top() : stack.Previous();
        const bool resultsModel = dynamic_cast<const screens::ResultsView*>(modelView) != nullptr;
        std::vector<gt2view::DrawItem> items;
        const float aspect = renderer.AspectRatio();
        view.Build(frame, aspect, items, squarePixels, [&](std::vector<gt2view::DrawItem>& layer) {
            if (!model || !carLoaded) return;
            if (in.battle) car.AppendPair(layer, resultsModel ? 1 : 0, model->Projection(true), resultsModel ? in.winnerPaint : in.paint, aspect, squarePixels);
            else car.Append(layer, model->Projection(true), in.paint, aspect, squarePixels);
        });
        window.EndFrame(items, ShotPathOf("GT2_ARCADE_POST_SHOT", window.Field()), fieldTime);
        // Dev aid: GT2_ARCADE_POST_COMPARE="field:cap.vram.bin:side.png,..." - this field's frame against a capture.
        if (const char* spec = std::getenv("GT2_ARCADE_POST_COMPARE"); spec && *spec) {
            std::string all = spec;
            for (size_t pos = 0; pos < all.size();) {
                size_t end = all.find(',', pos);
                if (end == std::string::npos) end = all.size();
                const std::string item = all.substr(pos, end - pos);
                pos = end + 1;
                const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
                if (c1 == std::string::npos || c2 == std::string::npos || std::atoi(item.c_str()) != window.Field()) continue;
                const bool withModel = model.has_value();
                CompareFrame(a, frame.prims, item.substr(c1 + 1, c2 - c1 - 1), item.substr(c2 + 1), withModel ? model->envX : 0, withModel ? model->envY : 0,
                             withModel ? (resultsModel ? 0x160 : 200) : 0, withModel ? (resultsModel ? 300 : 200) : 0);
            }
        }
        if (stack.Top() != lastView) { // the view switches with the window's field (aligning comparisons with captures)
            lastView = stack.Top();
            std::printf("arcade post-race f%d: %s\n", window.Field(), lastView ? lastView->Title().c_str() : "");
        }
    }
    std::copy(clear, clear + 3, renderer.clearColor);
    if (music) music(-1); // leaving the views (0x8005AE30 init 0x80049D28: 0x800481E8)
    static const char* const kChoice[] = {"Replay", "Try Again", "Exit", "window closed"};
    std::printf("arcade post-race views: %s (f%d)\n", kChoice[int(closed ? ArcadePostRaceChoice::kClosed : choice)], window.Field());
    return closed ? ArcadePostRaceChoice::kClosed : choice;
}

// ---------------------------------------------------------------- game mode 6

RaceMenuAssets WithCoursePicture(const RaceMenuAssets& a, const GtfsVolume& vol, const std::string& courseFile) {
    RaceMenuAssets out = a;
    try {
        const CoursePicture pic = LoadCoursePicture(vol, courseFile);
        std::vector<uint8_t> bytes;
        for (uint16_t w : pic.clut) bytes.push_back(uint8_t(w)), bytes.push_back(uint8_t(w >> 8));
        out.vram.Upload(448, 256, 16, 1, bytes);
        bytes.clear();
        for (uint16_t w : pic.image) bytes.push_back(uint8_t(w)), bytes.push_back(uint8_t(w >> 8));
        out.vram.Upload(448, 257, pic.words, pic.rows, bytes);
    } catch (const std::exception& e) {
        std::printf("arcade: no course picture for the TIME TRIAL menu (%s)\n", e.what());
    }
    return out;
}

screens::SessionInput SessionInputFromRam(const DiscImage& disc, const std::vector<uint8_t>& ram, uint32_t& courseId, uint32_t& carId) {
    const ExeProfile& p = ProfileOf(disc);
    auto at = [&](uint32_t sim) -> const uint8_t* {
        const uint32_t a = p.Race(sim) & 0x1FFFFF;
        if (a + 0x100 > ram.size()) throw std::runtime_error("session test: address outside the RAM image");
        return ram.data() + a;
    };
    auto str = [&](const uint8_t* s, size_t max) {
        std::string t;
        for (size_t k = 0; k < max && s[k]; k++) t.push_back(char(s[k]));
        return t;
    };
    screens::SessionInput in;
    std::memcpy(&in.results, at(0x801D5E88u), sizeof in.results);
    in.newRecord = *at(0x801D5DE9u);
    const uint8_t* block = at(0x801D585Cu); // the race block
    uint32_t recordPointer;
    std::memcpy(&recordPointer, at(0x800A9524u), 4);
    const uint8_t* record = ram.data() + (recordPointer & 0x1FFFFF);
    std::memcpy(&in.courseRecord, record, sizeof in.courseRecord);
    in.recordName = str(record + 0x18, 12);
    in.recordCar = str(block + 0x53C, 0x40);
    in.slotCar = str(block + 0xEC, 0x40);
    in.course = str(block + 0x20, 0x20);
    in.ghostOption = *at(0x801C9995u);
    in.enteredName = str(at(0x801D156Fu), 12);
    in.savedBest = *at(0x8002F4B1u) != 0;
    in.ghostLoaded = *at(0x801D55AAu) != 0;
    std::memcpy(&courseId, block + 0x40, 4);
    std::memcpy(&carId, block + 0x5C, 4);
    return in;
}

ArcadeSessionOutcome RunArcadeSessionViews(GameWindow& window, const RaceMenuAssets& a, const GtfsVolume& vol, const screens::SessionInput& input, bool raceRun,
                                           uint32_t carId, int paint, bool squarePixels, bool allRows, const std::function<void(const std::vector<int>&)>& sounds,
                                           const ViewMusic& music, ArcadeCardContext* cards, ArcadePartsContext* parts) {
    using screens::SessionNameView;
    using screens::SessionResultsView;
    using screens::SessionWaitView;
    using screens::TimeTrialMenuView;
    screens::SessionInput in = input;
    ArcadeSessionOutcome outcome;
    outcome.ghostOption = in.ghostOption;
    // Save Ghost / Load Ghost / Save Replay (0x8005B01C rows 4..6): not available in gt2game (Replay, row 0, plays the ring).
    // Settings ... (row 2, 0xFB: CHANGE PARTS 0x8005D1C0 of a garage car) needs `parts`; the card rows need `cards`.
    const uint16_t unsupported = allRows ? 0 : uint16_t((parts ? 0u : 1u << 2) | (cards ? 0u : (1u << 4 | 1u << 5 | 1u << 6)));
    screens::ChangePartsContext partsContext;
    auto menu = [&]() {
        auto v = std::make_unique<TimeTrialMenuView>(a, in);
        v->unsupported = unsupported;
        v->Setup(false);
        return v;
    };
    screens::SessionViewStack stack;
    stack.Start(std::make_unique<SessionWaitView>(a, raceRun, in.newRecord == 1)); // 0x800471F4(M, 0x8005B00C)

    gt2view::VkSceneRenderer& renderer = window.Renderer();
    gt2view::MenuView view(renderer, 0);
    view.SetFrameSize(RaceMenuAssets::kScreenWidth, RaceMenuAssets::kScreenHeight);
    view.SetInterpolatedPolygons(true);
    view.UploadVram(a.vram);
    gt2view::MenuCarView car(renderer, vol);
    car.SetFrameSize(RaceMenuAssets::kScreenWidth, RaceMenuAssets::kScreenHeight);
    car.SetReflection(9, uint16_t((576 / 16) | (152 << 6)), 0); // no reflection pass: the race's map is not in this VRAM
    const bool carLoaded = car.Use(carId);
    float clear[3];
    std::copy(std::begin(renderer.clearColor), std::end(renderer.clearColor), clear);
    renderer.clearColor[0] = renderer.clearColor[1] = renderer.clearColor[2] = 0.0f;
    uint32_t previousHeld = 0;
    int repeatTimer = 0;
    bool closed = true;
    const auto fieldTime = std::chrono::nanoseconds(16'683'333);
    const screens::PostRaceView* lastView = nullptr;
    while (window.BeginFrame()) {
        const MenuListPad p = ReadPad(window, previousHeld, repeatTimer);
        const int r = stack.Update(&p);
        screens::PostRaceView* top = stack.Top();
        if (top && sounds) sounds(top->sounds);
        if (auto* w = dynamic_cast<SessionWaitView*>(top); w && music)
            for (int track : w->cdTracks) music(track); // 0x800481C8(8)
        bool done = false;
        auto* waitView = dynamic_cast<SessionWaitView*>(top);
        auto* nameView = dynamic_cast<SessionNameView*>(top);
        auto* resultsView = dynamic_cast<SessionResultsView*>(top);
        auto* menuView = dynamic_cast<TimeTrialMenuView*>(top);
        auto* cardView = dynamic_cast<screens::CardView*>(top);
        auto* partsView = dynamic_cast<screens::ChangePartsView*>(top);
        auto* settingsView = dynamic_cast<screens::PartsSettingView*>(top);
        if (menuView) outcome.ghostOption = menuView->ghostOption;
        if (waitView && r == 1) {
            if (waitView->next == SessionWaitView::kEnterName) stack.Push(std::make_unique<SessionNameView>(a, in.enteredName));
            else if (waitView->next == SessionWaitView::kResults) stack.Push(std::make_unique<SessionResultsView>(a, in, true));
            else stack.Push(menu());
        } else if (nameView && r == 1) {
            // 0x8004A990: record + 0x18 = the name, + 0x14 = car 0's id, race block + 0x53C = the slot's car name.
            outcome.named = true;
            outcome.name = nameView->keyboard.name;
            in.enteredName = in.recordName = nameView->keyboard.name;
            in.recordCar = in.slotCar;
            stack.Push(std::make_unique<SessionResultsView>(a, in, true));
        } else if (resultsView && r == 1) {
            stack.Push(menu()); // 0x800483A4(M, 0x8005B12C)
        } else if (resultsView && r == 2) {
            stack.Pop();
            if (auto* m = dynamic_cast<TimeTrialMenuView*>(stack.Top())) m->Setup(true); // 0x8004C034(1)
        } else if (menuView && r == 1) {
            in.ghostOption = menuView->ghostOption;
            const int8_t act = menuView->Chosen();
            if (act == TimeTrialMenuView::kRecords) {
                stack.Push(std::make_unique<SessionResultsView>(a, in, false));
            } else if (parts && act == TimeTrialMenuView::kSettings) { // 0xFB: 0x800483A4(M, 0x8005D1C0) CHANGE PARTS
                menuView->in.replayAvailable = in.replayAvailable = false; // 0x8004C4B0: 0x801C90B4 = 1 (Replay / Save Replay disabled)
                partsContext = parts->open();
                stack.Push(std::make_unique<screens::ChangePartsView>(a, partsContext));
            } else if (cards && (act == TimeTrialMenuView::kSaveGhost || act == TimeTrialMenuView::kLoadGhost || act == TimeTrialMenuView::kSaveReplay)) {
                // 0xFA / 0xFD / 0xFE: 0x800483A4(M, 0x8005B540 / 0x8005B564 / 0x8005B51C), the executable's card manager
                const uint32_t cardViewId = act == TimeTrialMenuView::kSaveGhost   ? screens::CardView::kSaveGhostView
                                      : act == TimeTrialMenuView::kLoadGhost ? screens::CardView::kLoadGhostView
                                                                             : screens::CardView::kSaveReplayView;
                auto v = std::make_unique<screens::CardView>(a, *cards->cardAssets, *cards->text, cardViewId, cards->slots);
                if (act == TimeTrialMenuView::kLoadGhost) {
                    v->Manager().SetGhostCourse(cards->courseId);
                } else {
                    auto [payload, desc] = act == TimeTrialMenuView::kSaveGhost ? cards->ghostPayload() : cards->replayPayload();
                    v->Manager().SetSaveData(std::move(payload), desc);
                }
                stack.Push(std::move(v));
            } else if (act == TimeTrialMenuView::kReplay || act == TimeTrialMenuView::kTryAgain || act == TimeTrialMenuView::kExit) {
                outcome.choice = act == TimeTrialMenuView::kReplay     ? ArcadeSessionOutcome::kReplay
                                 : act == TimeTrialMenuView::kTryAgain ? ArcadeSessionOutcome::kTryAgain
                                                                       : ArcadeSessionOutcome::kExit;
                stack.Push(std::make_unique<screens::SessionLeaveView>()); // M+0x7C = the action, 0x800483A4(M, 0x8005B0C0)
                if (music) music(-1); // 0x8004A8B8: 0x800481E8
            } else {
                throw std::logic_error("TIME TRIAL: a row without a ported view was chosen");
            }
        } else if (cardView && r == 2) { // the manager exited: 0x80050304 (Load Ghost) -> 0x801D55AA = 1, 0x8002F4B1 = 1; back
            for (const std::string& line : cardView->Manager().log) std::printf("arcade card: %s\n", line.c_str());
            if (cardView->View() == screens::CardView::kLoadGhostView && cardView->Manager().Loaded()) {
                cards->loadedGhost = cardView->Manager().LoadedData();
                in.ghostLoaded = true;
                in.savedBest = true;
                outcome.ghostLoaded = true;
            }
            stack.Pop();
            if (auto* m = dynamic_cast<TimeTrialMenuView*>(stack.Top())) {
                m->in = in;
                m->Setup(true); // 0x8004C034(1)
            }
        } else if (partsView && r != 0) { // -4 (0x80056FF0, back: the manager's 2) / -8 (0x80048374(M, 0x8005D1E4), 0x80056FF0: 5)
            const bool toSettings = partsView->exit == screens::ChangePartsView::kPartsSetting;
            std::printf("arcade CHANGE PARTS: %d stage(s) selected, committed%s\n", partsView->changes, toSettings ? "; L1: PARTS SETTING" : "");
            parts->commit();
            if (toSettings) {
                stack.Replace(std::make_unique<screens::PartsSettingView>(a, partsContext), screens::SessionViewStack::kFromLeft);
            } else {
                stack.Pop();
                if (auto* m = dynamic_cast<TimeTrialMenuView*>(stack.Top())) m->Setup(true); // 0x8004C034(1)
            }
        } else if (settingsView && r != 0) { // -4 (0x80056FF0, back: 2) / -8 R1 (0x80048374(M, 0x8005D1C0), 0x80056FF0: 6)
            const bool toParts = settingsView->exit == screens::PartsSettingView::kChangeParts;
            std::printf("arcade PARTS SETTING: %s, committed%s\n", settingsView->changed ? "settings changed" : "no change", toParts ? "; R1: CHANGE PARTS" : "");
            parts->commit();
            if (toParts) {
                stack.Replace(std::make_unique<screens::ChangePartsView>(a, partsContext), screens::SessionViewStack::kFromRight);
            } else {
                stack.Pop();
                if (auto* m = dynamic_cast<TimeTrialMenuView*>(stack.Top())) m->Setup(true); // 0x8004C034(1)
            }
        } else if (dynamic_cast<screens::SessionLeaveView*>(top) && r == 4) {
            done = true; // the manager ends (0x800474F4 returns 0 for 3 / 4)
        }
        if (done) {
            closed = false;
            break;
        }
        size_t modelAt = 0;
        std::optional<screens::PostRaceModel> model;
        MenuFrame frame;
        frame.prims = stack.Frame(a, modelAt, model);
        frame.layer3dAt = modelAt;
        std::vector<gt2view::DrawItem> items;
        const float aspect = renderer.AspectRatio();
        view.Build(frame, aspect, items, squarePixels, [&](std::vector<gt2view::DrawItem>& layer) {
            if (model && carLoaded) car.Append(layer, model->Projection(true), paint, aspect, squarePixels);
        });
        window.EndFrame(items, {}, fieldTime);
        // Dev aid: GT2_ARCADE_SESSION_COMPARE="field:cap.vram.bin:side.png,..." - this field's frame against a capture.
        if (const char* spec = std::getenv("GT2_ARCADE_SESSION_COMPARE"); spec && *spec) {
            std::string all = spec;
            for (size_t pos = 0; pos < all.size();) {
                size_t end = all.find(',', pos);
                if (end == std::string::npos) end = all.size();
                const std::string item = all.substr(pos, end - pos);
                pos = end + 1;
                const size_t c1 = item.find(':'), c2 = item.find(':', c1 + 1);
                if (c1 == std::string::npos || c2 == std::string::npos || std::atoi(item.c_str()) != window.Field()) continue;
                const bool withModel = model.has_value();
                CompareFrame(a, frame.prims, item.substr(c1 + 1, c2 - c1 - 1), item.substr(c2 + 1), withModel ? model->envX : 0, withModel ? model->envY : 0,
                             withModel ? 200 : 0, withModel ? 200 : 0);
            }
        }
        if (stack.Top() != lastView) {
            lastView = stack.Top();
            const std::string title = lastView ? lastView->Title() : std::string();
            std::printf("arcade session views f%d: %s\n", window.Field(), title.empty() ? "(no title)" : title.c_str());
        }
        if (const char* frames = std::getenv("GT2_ARCADE_SESSION_FRAMES"); frames && *frames && window.Field() >= std::atoi(frames)) break; // dev aid
    }
    std::copy(clear, clear + 3, renderer.clearColor);
    if (closed) outcome.choice = ArcadeSessionOutcome::kClosed;
    static const char* const kChoice[] = {"Replay", "Try Again", "Exit", "window closed"};
    std::printf("arcade session views: %s, ghost option %u, name %s (f%d)\n", kChoice[int(outcome.choice)], unsigned(outcome.ghostOption),
                outcome.named ? outcome.name.c_str() : "(not entered)", window.Field());
    return outcome;
}

} // namespace gt2game
