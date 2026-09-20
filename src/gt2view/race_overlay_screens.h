#pragma once
// The race overlay's screens drawn OVER the race (GT2.OVL member 0 "ovl0", US Simulation v1.2, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; docs/formats/race_screens.md): the pause menu (0x80029D6C input,
// 0x80029E80 draw) and the race-end display (0x8002B170 with 0x8002AB60 "Finish", 0x8002A630 / 0x8002A3E0 position
// badge, 0x8002ACE0 result rows, 0x8002AF8C points rows; licence tests: prize text, time and the medal picture). Both
// are 320 x 240 frames in the race's drawing area, generated as the original generates them (every packet prepended
// to one ordering-table slot, so the GPU draws them reversed) and returned in GPU order. Pure CPU: the Vulkan side is
// gt2view/panel_view.h, the comparison with GP0 captures is in this file (ListGp0 = the text format of gt2play --prims,
// OverlayCanvas = the rasteriser rules of our interpreter's GPU, which made the captures).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/hud_assets.h"
#include "gt2formats/overlay_data.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2::raceui {

// One GPU command of these screens.
struct Gp0Prim {
    enum Kind : uint8_t { kMode, kSprite, kTile, kPolyF4, kPolyFT4 };
    Kind kind = kSprite;
    uint16_t e1 = 0;        // kMode: the draw-mode word (GP0 E1); kPolyFT4: its texture-page word (GP0 attribute)
    int16_t x[4] = {}, y[4] = {};
    int16_t w = 0, h = 0;   // kSprite / kTile
    uint8_t u[4] = {}, v[4] = {};
    uint16_t clut = 0;
    uint32_t colour = 0;    // 0xBBGGRR
    bool semi = false;
};

// Everything the screens read from the disc: the executable's fonts and tables, the overlay's tables and the VRAM the
// race holds while they are shown (only the areas these screens sample).
struct RaceOverlayAssets {
    GuestImage exe, ovl0;
    HudFont largeFont;  // 0x80093130: large digits (pause buttons, times, result rows)
    HudFont bigFont;    // 0x8009313C: the START / Finish / prize font
    // .text/data-race.txd: 0x80028CC0 reads file id 8 and copies the block of the career's language (career + 0,
    // 1 = USA) at file offset language * 0x1915 to RAM 0x801C6C50; the code refers to the strings by those addresses
    // (hud_assets.h LoadHudStrings(vol, language): the copied block, not block 0). Arcade v1.1: blocks of 0x167F at
    // 0x801C6940 (strings.base = the build's address of 0x801C6C50).
    HudStrings strings;
    // VRAM words (1024 x 512) of the console while these screens are up (checked word for word against the captures):
    //   (384, 0)   font/racefont.dat (tpages 6 / 7, the text CLUTs in its row 0)
    //   (512, 0)   arcade/game_status_files member 0, the HUD sheet (tpage 8: the position badges 0x8002F7F8 / 0x8002F804)
    //   (384, 256) arcade/license_tim.tim, image block (tpages 0x16 / 0x17: the licence prize pictures, CLUTs inside)
    std::vector<uint16_t> vram = std::vector<uint16_t>(size_t(1024) * 512, 0);
    std::array<HudSpriteDesc, 4> medals{};   // ovl0 0x8005B18C gold, 0x8005B198 silver, 0x8005B1A4 bronze, 0x8005B1B0 fourth ("kids") prize
    HudSpriteDesc badgeA{}, badgeB{};        // ovl0 0x8002F7F8 / 0x8002F804 (0x8002A630)
    std::array<int16_t, 26> capTable{};      // EXE 0x80091A78: 13 {x, y} points of the rounded button ends (0x800683FC)
    std::array<uint8_t, 17> ease{};          // ovl0 0x8002F5E8: 128 .. 0 (scale-in curves)
    // The screens name every table and string by its Simulation v1.2 address; the disc's build maps it (GuestImage::Sim,
    // gt2formats/exe_profile.h; identity on the Simulation disc). US Arcade v1.1: the same code modulo relocations (db
    // arcade_us11_symbols.yaml pause_menu_draw / race_end_*; docs/research/arcade_disc.md 17.7).
    uint32_t Colour(uint32_t address) const { return ovl0.Get<uint32_t>(ovl0.Sim(address)); } // colour words 0x8002F5FC..0x8002F62C
    std::string Text(uint32_t address) const; // the string at a (Simulation) RAM address of the copied block; "" when the build has none
    // The string `offset` bytes after a referenced one (code that adds a computed offset to a lui-built address).
    std::string Text(uint32_t address, uint32_t offset) const;
    static RaceOverlayAssets Load(const DiscImage& disc, const GtfsVolume& vol, uint8_t language = 1);
};

// ---- pause (0x80029D6C / 0x80029E80)
// State bytes 0x800A94C0 (selection 0 Continue / 1 Exit) and 0x800A94C1 (fields since the menu opened or the selection
// moved, wraps from 31 to 0; -1 = closed).
struct PauseMenu {
    int8_t selection = 0;
    int8_t counter = -1;
    void Open() { selection = 0, counter = 0; }
    bool IsOpen() const { return counter >= 0; }
    // 0x80029D6C: one field of input (bit 0 up, bit 1 down, 0x10A00 choose: cross / circle / start). Returns -1 while
    // open, 0 = Continue, 1 = Exit (the menu is closed then).
    int Update(bool up, bool down, bool choose);
};
std::vector<Gp0Prim> BuildPauseFrame(const RaceOverlayAssets& a, const PauseMenu& menu);

// ---- the race-end display (0x8002B170, one call per frame from the HUD 0x8002E63C)
struct RaceEndRow {                 // one car of the race order (results position + 1 = row)
    std::string name;               // race slot name (0x801D5948 + car * 0xD0)
    bool player = false;            // car 0: highlighted colour 0x70543A
    int32_t finishTime = -1;        // total time (car record + 0x...; 0x800A9E34 + car * 0xB40), 1/1000 s
    bool finished = false;          // 0x800A9DB0 + car * 0xB40
    int32_t laps = 0;               // laps completed (0x800A9CBC + car * 0xB40)
    int32_t racePoints = 0;         // 0x801D5E82[car] (championship race)
    int32_t totalPoints = 0;        // 0x801D5E7C[car], rows sorted by 0x8005E6B0 for "Total Points"
    int car = 0;                    // the car index (2 player Battle: the winner line names the leader's player, car 0 / 1)
};
struct RaceEndState {
    int subMode = 1;                // 0x801D5866: 0 2 player Battle, 1 GT-mode event, 2 championship race, 3 licence test (others: arcade)
    int16_t timer = -1;             // 0x800AF226: frames since the finish (-1 = racing)
    int16_t auxTimer = -1;          // 0x800AF228: started at timer 0x9E (the position badge)
    int seriesRaces = 1;            // 0x801D5DF6: races of the championship (case 2 needs > 1)
    int lapCount = 1;               // 0x801D586B
    std::vector<RaceEndRow> rows;   // in finishing order (row 0 = 1st)
    std::vector<RaceEndRow> standings; // "Total Points" order (0x8005E6B0)
    // Licence tests: result code 0x801D5DEC (1 = pass), time 0x801D5DF0, medal times 0x8003D7B8(record + 0x44, 1..4)
    // and whether a fourth prize counts (test record +1 == 0 and +2 + 1 == record + 0x74).
    int32_t licenceResult = -1;
    uint32_t licenceTime = 0;
    std::array<uint32_t, 4> medalTimes{};
    bool fourthPrizeCounts = false;
};
std::vector<Gp0Prim> BuildRaceEndFrame(const RaceOverlayAssets& a, const RaceEndState& s);
// The prize of a licence result as 0x8002B170 picks it: 4 gold .. 1 fourth, 0 none ("FAIL").
int LicencePrizeOf(const RaceEndState& s);

// ---- comparisons with gt2play --prims captures
// Our primitives in the capture's text format ("E1 tpage=026 blend=1 depth=0 dither=0", "RECT 64 rgb=1E3264 xy=(181,164)
// uv=(162,234) clut=001A size=(10,14)", ...), without the capture's running index.
std::vector<std::string> ListGp0(const std::vector<Gp0Prim>& prims);
// The rasteriser of our interpreter's GPU (src/machine/gpu.cpp: sprites, tiles, triangles with a top-left rule and
// floating-point barycentrics, 8-bit blending) on a 1024 x 512 VRAM; the frame is drawn at (0, originY).
class OverlayCanvas {
public:
    explicit OverlayCanvas(const std::vector<uint16_t>& vram) : vram_(vram) {}
    void Draw(const std::vector<Gp0Prim>& prims, int originY = 0);
    const std::vector<uint16_t>& Vram() const { return vram_; }
    uint16_t At(int x, int y) const { return vram_[size_t(y & 511) * 1024 + size_t(x & 1023)]; }
    void Clear(int x, int y, int w, int h);

private:
    uint16_t Texel(int u, int v) const;
    void Plot(int x, int y, int r, int g, int b, bool semi, bool mask);
    void Triangle(const int* xs, const int* ys, const int* us, const int* vs, int i0, int i1, int i2, int r, int g, int b, bool textured, bool semi);
    std::vector<uint16_t> vram_;
    uint16_t mode_ = 0, clut_ = 0;
    int left_ = 0, top_ = 0, right_ = 319, bottom_ = 239;
};
// Parses the primitives of a capture's text lines (E1, RECT 60..67, POLY 28..2F; the lines of one frame).
std::vector<Gp0Prim> ParseGp0Lines(const std::vector<std::string>& lines);

} // namespace gt2::raceui
