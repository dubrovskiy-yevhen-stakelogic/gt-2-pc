#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/exe_profile.h"
#include "gt2formats/hud_assets.h"
#include "gt2formats/overlay_data.h"
#include "gt2view/vk_scene_renderer.h"
#include "gt2view/hud_visibility.h"

namespace gt2view {

// What the HUD shows in one frame (filled by the game from RaceSim). The fields are the original's inputs of the
// HUD routines (0x8002E63C and callees, docs/formats/hud.md); times are 1/1000 s, -1 = none.
struct HudFrame {
    HudVisibility visibility;
    // ---- race shell
    int gameMode = 2;           // 0x801D5866: 0 GT-mode race, 2 / 4 / 0xB arcade, 3 licence, 1 / 6 / 10 and 7 / 8 / 9 other layouts
    int licenseByte = 0;        // 0x801D5867 (mode 3: non-zero = no course map)
    int licenseType = 0;        // 0x801C98A2 (mode 3: types 2 / 3 put "Lap Time" one line higher)
    int32_t licenseRecordMs = -1; // mode 3: the licence's record time block (0x801CACFC + 0x668 * 0x801D5867 + 0xA4 * 0x801D5868)
    int licenseRecordSpeed = 0;
    bool replay = false;        // 0x800A951C: the attract race / a replay
    int replayView = 0;         // camera + 0x107 in a replay: 0 = only the lap block, 1 = the full HUD (2: + two panels, not ported)
    int lapCount = 2;           // 0x801D586B
    int lap = 1;                // car + 0x634
    int position = 1;           // car + 0x77C (1..6)
    bool finished = false;      // car + 0x728: the lap block shows the final time and no running lap
    int32_t totalMs = -1;       // the race clock (0x80046F64 / 3) or, finished, the result's final time
    int32_t lapMs = -1;         // the running lap: race clock / 3 - car + 0x7AC
    int subFrame = 0;           // 0x8002F864 & 0xF: the original adds (n * 1000) / 900 ms to the running times
    std::vector<int32_t> laps;  // the player's recorded laps (0x801D5E88: count at +4, times at +8 + 20 i)
    int32_t bestLapMs = -1;     // 0x801D5F58: the best lap of the race and its top speed readout
    int bestLapSpeed = 0;
    int32_t recordMs = -1;      // *(0x800A9524): the course record and its top speed readout
    int recordSpeed = 0;
    uint32_t machineRecord = 0xFFFFFFFFu; // modes 7 / 8 / 9: entry 0's value of the career's machine-test record (0x801CD36C / 0x801CD410 / 0x801CD4B4 + 4; a speed readout in 9)
    int resultsLapNumber = 0;   // 0x801D5E88 + 2: the next lap number to record (the replay's bracketed lap, 0x8005E378)
    // ---- gauges (car record fields)
    int rpm = 0;                // car + 0x6D8
    int revLimitRpm = 8000;     // car + 0x134: sets the face and the scale (0x8002BD84)
    int faceSlot = 0;           // car + 0x880: the face slot of the sheet (0x8002BD84(car, entry): 0 player 1, 1 player 2 of a 2P race)
    int redlineRpm = 7000;      // car + 0x3C2: the ring turns red from here
    int speedReadout = 0;       // car + 0x6DA: 1/100 mph (the US readout)
    int gear = 1;               // car + 0x644, 0 = reverse
    bool clutchEngaged = true;  // car + 0x645 == 1: the gear glyph is bright
    int turbo = 0;              // car + 0x154: 0 = no turbo gauge
    int boost = 0;              // car + 0x76E
    bool metric = false;        // km/h (our option: the US game has only mph; speeds converted from the readout)
    // ---- course map
    struct MapCar { int16_t x = 0, z = 0; bool present = true; };
    std::vector<MapCar> mapCars; // car + 0x832 / 0x83A: the integer metres of the world position (x, z)
    int mapHighlight = 0;       // the car drawn red (0x80029064's 5th argument)
    bool courseMap = true;      // Course Map option (career + 0xB1 = 0x801C9991): 0x80029064 returns at once when 0
    // ---- start / messages
    int startTimer = -1;        // 0x800AF224 (s16): hold + 240 at the start, -fields per frame down to 0 (0x8002A0D4); < 0 = nothing (0x8002A19C)
    int messageCode = 0;        // car + 0x790 (body.messageCode): the warning line of 0x8002E204 (1..12, 0 = none)
    bool raceFinished = false;  // "Finish" (our placement; the race-end display 0x8002B170 is gt2view/race_overlay_screens)
    // ---- licence medals (mode 3, 0x8002D308 -> 0x8002D12C): 0x8003D7B8(settings block, 1..3) in 1/1000 s
    std::array<int32_t, 3> medalMs{-1, -1, -1};
    // ---- tyre panel 0x8002DE8C: drawn when the tyre-wear constant 0x80046F48 != 0 or the shell's control class
    // (0x800418E8) is 2; per wheel (FL, FR, RL, RR) wheel + 0x22 (damage, red tint) and wheel + 0x3F (wear stage colour)
    bool tyrePanel = false;
    std::array<int, 4> wheelDamage{};
    std::array<int, 4> wheelWearStage{};
    // The shell's message block of the car record (0x8002D664 at (160, 96)): split difference, caption + time,
    // "Crash !" / "Out of Course"; each with a display timer (> 0 = shown, fading out over its last 30 frames).
    bool timeInvalid = false;   // car + 0xA8C
    int crashKind = 0;          // car + 0xA8D: 2 = "Out of Course", else "Crash !"
    int captionTimer = 0;       // car + 0xA8E
    int splitTimer = 0;         // car + 0xA90
    int crashTimer = 0;         // car + 0xA92
    int32_t captionMs = -1;     // car + 0xA94
    std::string caption;        // car + 0xA98 (token -> string; empty = none)
    uint32_t splitA = 0xFFFFFFFFu, splitB = 0xFFFFFFFFu; // car + 0xA9C / 0xAA0: this split and the compared one
    std::string carName;
};

// Dev aid (oracle checks: gt2play --prims / --hud2p-check, gt2game's split HUD check): the 2 player Battle's HUD inputs as the
// original's RAM holds them in a frame of game mode 0 (Simulation v1.2 addresses translated by `profile`): the view's split flag
// (view 0x801FF8B8 + 0x2EA), the car camera 1 follows (+ 0xC4 + 0x10C), each car's HudFrame (car records 0x800A9688 + k * 0xB40,
// results 0x801D5E88 / 0x801DA3A0, camera k + 0x107; the full view: camera 1's + 0x107 for both), the course's .crsinfo entry.
struct TwoPlayerHudRam {
    bool valid = false;         // game mode 0, two cars, the race overlay loaded
    bool split = true;
    uint32_t followed = 0;
    int courseIndex = -1;       // 0x800AF230
    std::array<HudFrame, 2> frames;
};
TwoPlayerHudRam TwoPlayerHudFromRam(const uint8_t* ram, const gt2::ExeProfile& profile, const gt2::GuestImage& raceOverlay, const gt2::HudStrings& strings,
                                    bool tyrePanelSeen);

// The in-race HUD drawn with the original's graphics at the original's 320 x 240 layout, scaled to the window.
// The routines of the race overlay are ported (0x8002E63C: lap block 0x8002C76C, course map 0x80029064, gauges
// 0x8002C00C / 0x8002C1CC / 0x8002BD84 / 0x8002BF14, turbo 0x8002C584, record panel 0x8002D308 / 0x8002D058 /
// 0x8002D12C (medal lines), START / countdown 0x8002A19C, warnings 0x8002E204, tyre panel 0x8002DE8C, the text engine
// of hud_assets.h): the primitives are generated in the original's order into the
// ordering-table slot and drawn in reverse like the GPU walks it; VRAM is a copy of the console's HUD area (font
// (384, 0), gauge sheet (512, 0) with the car's dial face in slot 0, course map (576, 144) / CLUT (368, 508)).
class Hud {
public:
    static constexpr uint32_t kVertexBase = 1'000'064, kVertexLimit = 40'000;
    static constexpr uint32_t kRowBase = 1536; // our VRAM rows 1536..2047 = console rows 0..511 of the HUD's data
    static constexpr int kFrameWidth = 320, kFrameHeight = 240;

    Hud(VkSceneRenderer& renderer, const gt2::GtfsVolume& vol, const gt2::GuestImage& exe, const gt2::GuestImage& raceOverlay);

    const gt2::HudStrings& Strings() const { return strings_; }

    // Loads crsmap/<course>.tim (no map when the course has none).
    void UseCourse(const std::string& courseName);

    // Appends the HUD's draw items (drawn after the scene; they carry identity transforms and NDC positions).
    void Build(const HudFrame& frame, float windowAspect, std::vector<DrawItem>& items);
    // The 2 player Battle's HUD (game mode 0, 0x8002E908 for car 0 and car 1): both halves of the split screen in one frame
    // (`top` = player 1 / car 0, `bottom` = player 2 / car 1; their gameMode 0 layout of the lap block), docs/research/arcade_disc.md
    // section 19. The dial face is the top car's (one face slot, as the original's single upload).
    void Build2P(const HudFrame& top, const HudFrame& bottom, float windowAspect, std::vector<DrawItem>& items);
    // The 2 player Battle's full view (a replay with the split off, view + 0x2EA = 0): the dispatcher 0x800293D4 calls 0x8002E63C
    // with the car camera 1 follows (car + 0xB40 * camera + 0x10C) and camera 1 - no 0x8002E818 caption in game mode 0; the dial
    // faces stay those of the race start (car 0's in slot 0 with its CLUT, car 1's image in slot 1: `f.faceSlot` = the car's).
    void Build2PFull(const HudFrame& f, int revLimitCar0, int revLimitCar1, float windowAspect, std::vector<DrawItem>& items);

    // Dev aid: the GP0 primitives of the last Build in draw order, in the text format of gt2play --prims
    // ("E1 tpage=...", "RECT 64 rgb=... xy=(x,y) uv=(u,v) clut=... size=(w,h)", "POLY 38 ..."), for comparing with
    // the original's captured draw list.
    std::vector<std::string> ListPrimitives() const;
    // Dev aid: our copy of the console's HUD VRAM (1024 x 512 words at the console's coordinates) as the last Build uploaded it.
    const std::vector<uint16_t>& Vram() const { return block_; }

private:
    using Rgb = std::array<uint8_t, 3>;
    // One GPU command of the HUD: a draw mode (E1 texture page / blend), a sprite (RECT, texture page from the
    // current draw mode), a tile, or a flat / gouraud quad.
    struct Cmd {
        enum Kind { kMode, kSprite, kTile, kPoly } kind = kSprite;
        uint16_t tpage = 0;
        int x[4] = {}, y[4] = {};
        uint32_t color[4] = {};  // GP0 colour words (0xBBGGRR)
        int u = 0, v = 0, w = 0, h = 0;
        uint16_t clut = 0;
        bool semi = false, gouraud = false;
        int anchor = 0;
    };
    struct Quad {
        float x[4], y[4];       // frame pixels, corner order 0 1 2 3 = TL TR BR BL
        float u[4], v[4];
        uint32_t page = 0, clut = 0;
        bool textured = false;
        Rgb color[4] = {};
        uint32_t blend = kBlendOpaque;
        int anchor = 0;
    };
    enum Anchor { kAnchorLeft = 0, kAnchorCentre = 1, kAnchorRight = 2, kAnchorStretch = 3 };

    // Packet emitters in the original's insertion order (each packet is inserted at the head of the same ordering
    // table slot, so the GPU draws the packets in reverse and the commands of one packet in order).
    void Packet(std::initializer_list<Cmd> cmds);
    void Mode(uint16_t tpage);
    void SpriteAt(int x, int y, const gt2::HudSpriteDesc& d, uint32_t colorWord); // RECT with the colour word's flags
    void SpriteCentred(int cx, int cy, const gt2::HudSpriteDesc& d, uint32_t colorWord);
    void Tile(int x, int y, int w, int h, uint32_t colorWord);
    void PolyF4(const int xs[4], const int ys[4], uint32_t colorWord);
    void PolyG4(const int xs[4], const int ys[4], const uint32_t colors[4]);
    void Bar(int x, int y, int w);                         // 0x8006B77C with the rectangle 0x8002F8E0
    void Glyphs(const std::vector<gt2::HudFontSprite>& sprites, uint32_t colorWord);
    void Needle(int angle, int pivotX, int pivotY, const gt2::HudTables::Needle& n); // 0x8002BF14

    // The ported routines (argument = the original's screen position).
    void LapBlock(const HudFrame& f, int x, int y);        // 0x8002C76C
    void CourseMapPanel(const HudFrame& f, int x, int y);  // 0x80029064
    void Tachometer(const HudFrame& f, int x, int y);      // 0x8002C00C (face, ring)
    void Needles(const HudFrame& f, int x, int y);         // 0x8002C1CC (needle, gear, unit, speed)
    void TurboGauge(const HudFrame& f, int x, int y);      // 0x8002C584
    void RecordPanel(const HudFrame& f, int x, int y);     // 0x8002D308
    void TimeBlock(int32_t ms, int speed, int x, int y, const HudFrame& f); // 0x8002D058
    void MachineRecord(uint32_t value, bool speed, int x, int y, const HudFrame& f); // 0x8002D20C
    void MedalLine(int32_t ms, int x, int y, uint32_t colorWord);           // 0x8002D12C
    void StartDisplay(const HudFrame& f);                                   // 0x8002A19C (countdown digits, START / REPLAY)
    void Warning(const HudFrame& f, int x, int y);                          // 0x8002E204
    void TyrePanel(const HudFrame& f, int x, int y);                        // 0x8002DE8C
    void Messages(const HudFrame& f, int x, int y);         // 0x8002D664
    void ViewHud(const HudFrame& f);                        // 0x8002E63C (view, car, camera)
    void Emit(float windowAspect, std::vector<DrawItem>& items); // the packets (reverse order) -> quads -> draw items

    void UploadBlock();
    int DialIndexFor(int revLimitRpm) const;               // 0x8002BD84's face choice

    VkSceneRenderer& renderer_;
    const gt2::GtfsVolume& vol_;
    std::array<int16_t, 5120> sinTable_{}; // the executable's 0x80093150 (sine; cosine at + 0x400)
    gt2::RaceFont font_;
    gt2::HudFont text_;
    gt2::HudFont large_;   // 0x80093130: the message block's large digits
    gt2::HudFont start_;   // 0x8009313C: the START / countdown font
    std::array<uint32_t, 12> warningTokens_{}; // 0x8002E204's strings for codes 1..12 (from the overlay's jump table 0x8002F320); 0 = unknown build
    std::array<gt2::HudSpriteDesc, 4> tyreSprites_{}; // 0x8002F81C..0x8002F840: {left wear, left tint, right tint, right wear}
    uint32_t tyreTintFrom_ = 0, tyreTintTo_ = 0x80;   // 0x8002F904 / 0x8002F908: damage 0 -> 256 fades between these
    bool haveTyreSprites_ = false;
    std::string noRecordSpeed_;                       // 0x8002F2E8: 0x8002D20C's speed text of an empty record (read from the overlay)
    uint32_t startToken_ = 0, replayToken_ = 0;       // 0x8002A19C's "START" / "REPLAY" tokens (decoded from its code); 0 = unknown build
    gt2::HudSheet sheet_;
    gt2::HudStrings strings_;
    gt2::HudTables tables_;
    gt2::CourseMap map_;
    bool haveMap_ = false;
    int uploadedDial_ = -2;
    int uploadedDial2_ = -2; // the 2 player Battle's second face (slot 1); -2 = none
    std::vector<uint16_t> block_;       // 1024 x 512 words: our copy of the console's HUD VRAM
    std::vector<std::vector<Cmd>> packets_;
    int anchor_ = kAnchorLeft;
    bool visible_ = true;
    std::vector<Quad> quads_;
};

} // namespace gt2view
