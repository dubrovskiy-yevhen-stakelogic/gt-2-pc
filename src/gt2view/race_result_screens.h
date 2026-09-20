#pragma once
// The race overlay's post-race full-screen views (GT2.OVL member 0 at 0x80010000) as GPU primitives in draw order
// (gt2formats MenuPrim, like race_menus.h), with the views' per-field state machines (timelines and pad rules of the
// original):
//   A. RESULTS  (view 0x8005B7C4: setup 0x80050FD0, update 0x8005162C, draw 0x80051BF4)
//   B. BONUS    (view 0x8005D57C single race: setup 0x80059A7C; 0x8005D4EC championship race: setup 0x80059704;
//                0x8005D534 championship end: setup 0x80059800; shared: 0x800595E0, update 0x80059BAC, draw 0x8005A218)
//   C. the post-race event menu (view 0x8005AE54: setup 0x80049D90, update 0x8004A0BC, draw 0x8004A55C)
// The 3D car / trophy these views draw through 0x80048754: the camera objects and the floor disc are ported (Model(),
// BuildPostRaceModelFloor); the model itself is drawn by the renderer (gt2view/menu_view.h MenuCarView).
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); code addresses in
// ovl0 unless marked EXE; evidence: our disassembly of the dumps work/re/spec_prize (views_spec_listing.txt), GP0
// captures work/play/racescreens/cap/post_*.txt (gt2play --prims, route CBM0001).
//
// Notation of the comments: W = the work block *(u32*)0x801C90A0, M = the view manager *(u32*)0x801C90A4, V = the
// view object (its +0x14 / +0x16 / +0x18 hold per-view state). Slot n = entry n of the view's ordering table (MenuOt);
// the GPU draws slot 4 (the header, 0x80047024) first, then 3, 2, 1, 0.
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/menu/menu_car.h"
#include "game/shell/title_screens.h"
#include "gt2view/race_menus.h"

namespace gt2::screens {

// ---------------------------------------------------------------- building blocks

// {s16 steps, s16 anim}: tick 0x80049274 (== EXE 0x8006BDD0: anim++ clamped to steps; < -1 counts up to -1), alpha
// EXE 0x8006BE20 (anim >= 0: anim * 128 / steps; < -1: ~anim * 128 / steps; -1: 0).
struct FadePair {
    int16_t steps = 1, anim = -1;
    static FadePair Read(const GuestImage& image, uint32_t address);
    void Tick();
    int Alpha() const;
    void Close() { anim = int16_t(~steps); }
};

// The label object 0x80048D14 (0x50 bytes: +00 band, +1C text object, +44 value, +48 value colour, +4C s16 value
// fade, +4E flags) of a 0x20-byte descriptor (+0 band c0, +4 band c1, +8 label string, +C text c0, +10 value (string
// pointer or u32 time), +14 value colour, +18 u16 flags (bits 0..1 value kind 0 text / 1 time / 2 number, bit 2 a
// subtractive shade band), +1C s16 band width, +1E s16 steps). Tick 0x80048E14, draw 0x80048E84.
struct ResultLabel {
    shell::Band band;
    TextObject text;
    std::string value;         // kind 0 / 2
    uint32_t time = 0;         // kind 1 (FormatRaceTime)
    uint32_t valueColour = 0;
    int16_t fade = -1;         // -1 hidden; 0..16 opening / open; -17..-2 closing
    uint16_t flags = 0;

    // 0x80048D14 (the value from the descriptor's +0x10: a string of the race text / ovl0, or the u32 time).
    static ResultLabel Init(const RaceMenuAssets& a, uint32_t descriptor);
    void Open();   // band anim 0, text Open(-1), value fade 0 (the updates' "open label")
    void Close();  // band anim ~steps, text Close, value fade -17
    void Tick();
    // Value glyphs into ot[slot], the text (glyphs slot + 1), the band into ot[slot + 1] at (x, y - h / 2), E1 0x220,
    // flag 4: the shade band + E1 0x240.
    void Draw(const RaceMenuAssets& a, MenuOt& ot, int slot, int x, int y) const;
};

// The course name object 0x80048BD8 (band template 0x8005ABAC with w 0x100, text template 0x8005ABC8), draw
// 0x80048C60(obj, ot, x, y): text into slot 0 at (x - width / 2, y + 8), band into slot 1 at (x - 0x80, y - h / 2),
// E1 0x220 (slot 1).
struct CourseTitle {
    shell::Band band;
    TextObject text;
    static CourseTitle Init(const RaceMenuAssets& a, const std::string& name);
    void Open();
    void Close();
    void Tick();
    void Draw(const RaceMenuAssets& a, MenuOt& ot, int x, int y) const;
};

// The one-button dialog of the executable (init 0x8006DCB8, open 0x8006DDD0, close 0x8006DE1C, update 0x8006DE44,
// draw 0x8006DEEC; text template EXE 0x80091EA8). Draws into slots 0..2.
struct ResultDialog {
    int16_t x = 0, y = 0;
    TextObject text;
    uint16_t flags = 0;        // +2C
    int16_t w = 80, h = 12;    // +2E / +30
    int16_t anim = -1;         // +32: -1 hidden, 0..71 open (72 -> 12), < -1 closing (-13 ..)
    uint32_t fill = 0, gradient = 0;
    static ResultDialog Init(const RaceMenuAssets& a, uint32_t templateAddress);
    void Open();
    void Close();
    // `pad` null = no input: -2 nothing, 0 chosen (cross / circle), -1 back (triangle / square).
    int Update(const MenuListPad* pad);
    void Draw(const RaceMenuAssets& a, MenuOt& ot) const;
};

// The two-button bar of the executable (init 0x8006E1CC, open 0x8006E388, close 0x8006E3FC, update 0x8006E43C, draw
// 0x8006E5B8; text template EXE 0x80091EC8). Draws into slots 0..2.
struct ResultBar {
    int16_t x = 0, y = 0;
    TextObject title, label0, label1;
    int8_t sound = -1;         // +7C
    int8_t cursor = 0;         // +7D: 0 left, 1 right
    int16_t w = 80, h = 12;    // +7E / +80
    int16_t anim = -1;         // +82
    int16_t slide = 0;         // +84
    uint16_t flags = 0;        // +86: bit 0 slide from the left, bits 1..2 size, bit 3 close on choose / back, 0x80 flash
    uint32_t fill = 0, gradient = 0;
    static ResultBar Init(const RaceMenuAssets& a, uint32_t templateAddress, int cursor = 0);
    void Open();
    void Close();
    // -2 nothing, -1 back, 0 / 1 chosen (the cursor), -3 moved (plays `sound`).
    int Update(const MenuListPad* pad, std::vector<int>* sounds = nullptr);
    void Draw(const RaceMenuAssets& a, MenuOt& ot) const;
};

// The style block of the time displays (+0 colour, +4 colour2, +8 advance, +A narrow, +C lap-label offset, +E digit
// shift, +10 font), e.g. 0x8005B664 / 0x8005B678.
struct TimeStyle {
    uint32_t colour = 0, colour2 = 0;
    int16_t advance = 7, narrow = 6, lapOffset = 0x82, digitShift = -2;
    uint32_t font = 0x801C9120u;
    static TimeStyle Read(const GuestImage& image, uint32_t address);
};

// 0x800492C4 with isNumber 0: a race time right-aligned at x (centre: x += width / 2) fading in with `fade` (two ghost
// copies at x -/+ k / 4 while opening, k = 128 - alpha), all glyphs into `ot`.
void AddTimeDisplay(const RaceMenuAssets& a, MenuOtSlot& ot, const FadePair& fade, uint32_t value, int x, int y, int mode, const TimeStyle& style,
                    bool centre = false);
// 0x800490F8: one lap row (time right-aligned at `right`, then "Lap" + the lap number at right - lapOffset when
// lap >= 0) into `ot`.
void AddLapRow(const RaceMenuAssets& a, MenuOtSlot& ot, uint32_t time, int lap, int right, int y, const TimeStyle& style, int mode);
// 0x8005A11C: the money figures of BONUS ("1,500"; big font 0x801C9130, spacing 2, additive) right-aligned, fading in
// over 12 fields with t (t < 0: nothing).
void AddMoneyNumber(const RaceMenuAssets& a, MenuOtSlot& ot, int right, int y, uint32_t value, int t, uint32_t colour);
// EXE 0x80068D0C: unsigned decimal with ',' every three digits.
std::string MoneyText(uint32_t value);

// ---------------------------------------------------------------- views

// The 3D model a view draws this frame through 0x80048754 into the model environment M+0xC0 (drawn by the GPU after the
// frame's clear and before the views: its floor disc (2D, OverlayModelFloor) and the model itself (3D, the renderer's)):
// car 0's model (*(u32*)0x800A9F00, model instance {CLUT 0x7FD7, page 9, +8 = 1, +9 reflection 0x40}; 0x80067444) in
// RESULTS / the post-race menu, the 'gtprz' trophy ({CLUT 0x6028, page 0x1A, +9 = 0x20}; 0x80048528) at the
// championship end.
struct PostRaceModel {
    bool trophy = false;
    menu::OverlayModelCamera camera;
    int16_t envX = 0, envY = 0;     // the environment's drawing offset (0x8008034C of the view's draw)
    uint16_t envClut = 0x7FD7, envPage = 9;
    uint8_t reflection = 0x40;
    // The floor camera's projection and the model camera's (menu::OverlayModelProject).
    menu::MenuCarProjection Projection(bool withYaw) const { return menu::OverlayModelProject(camera, envX, envY, withYaw); }
};

// A view of the race overlay's view manager (0x800474F4 / 0x800479AC): its header colour / title (view +0x0C /
// +0x10), a per-field update and a draw into the view's ordering table.
class PostRaceView {
public:
    // The model drawn this frame (none while the view's model flag is off).
    virtual std::optional<PostRaceModel> Model() const { return std::nullopt; }
    virtual ~PostRaceView() = default;
    // One field (the manager's update(arg): `input` = arg 1, i.e. the current view; the pad block M+0x1A8 is `pad`).
    // Returns 1 when the view is left (Next() then names the next step), else 0.
    virtual int Update(const MenuListPad* pad, bool input = true) = 0;
    virtual void Draw(MenuOt& ot) const = 0;
    virtual std::string Title() const = 0;
    virtual uint32_t Colour() const = 0;
    std::vector<int> sounds; // 0x80060840 requests of the last update
};

// A view that only waits (0x80050E94 pre-results 24 fields, 0x8005947C / 0x80049C68 16 fields, the leave view
// 0x80049D28 20 fields): nothing drawn; left after `fields` updates.
class WaitView : public PostRaceView {
public:
    WaitView(const RaceMenuAssets& a, uint32_t view, int fields);
    int Update(const MenuListPad* pad, bool input = true) override;
    void Draw(MenuOt&) const override {}
    std::string Title() const override { return title_; }
    uint32_t Colour() const override { return colour_; }
    int16_t counter = 0; // V+14
private:
    std::string title_;
    uint32_t colour_ = 0;
};

// ---- A. RESULTS

struct ResultsInput {
    int place = 1;                   // s16 0x801D5E88 (1..6; others show place 1's text)
    uint32_t totalTime = 0;          // u32 0x801D5F80 (ms)
    uint32_t fastestLap = 0;         // u32 0x801D5F58
    std::vector<uint32_t> laps;      // u32 0x801D5E90 + i * 0x14 (s16 0x801D5E8C of them, at most 10)
    int firstLap = 1;                // lap number of laps[0]: s16 0x801D5E8A - count + 1
    std::string course;              // 0x801D587C
    bool saveBar = true;             // 0x80050D00: the "Save Game?" bar (else the dialog "Next")
    uint32_t vsync = 0;              // the VSync counter 0x8007D23C(0) at the setup: seed of the car camera's pose
    // Game mode 0, the 2 player Battle (race block + 0x0A == 0; the setup's W+0 = 1): the place text is the winner's
    // ("PLAYER 1 WINS !!" 0x801C7814 / "PLAYER 2 WINS !!" 0x801C782B, colour 0x8005AB84[0]), a second column of player 2's
    // results record (0x801DA3A0) right-aligned 90 pixels right of player 1's (x 210 / 300 instead of 250), the lap rows
    // of the player with more kept laps (0x8005E378 per player: 0xFFFFFFFF when that player has no such lap), player 2's
    // rows without the lap label. `laps` / `firstLap` are then player 1's column / the rows' first lap number.
    bool battle = false;
    int winner = 0;                  // 0 / 1 (the setup's winner rule, arcade_results.h ApplyBattleResult)
    uint32_t totalTime2 = 0;         // u32 0x801DA498 (W+0xC)
    uint32_t fastestLap2 = 0;        // u32 0x801DA470 (W+0x14)
    std::vector<uint32_t> laps2;     // W+0x40 + i * 4
};

class ResultsView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005B7C4u;
    explicit ResultsView(const RaceMenuAssets& a);
    ResultsView(const ResultsView&) = delete;
    ResultsView& operator=(const ResultsView&) = delete;

    void Setup(const ResultsInput& in);                     // 0x80050FD0
    int Update(const MenuListPad* pad, bool input = true) override; // 0x8005162C
    void Draw(MenuOt& ot) const override;                   // 0x80051BF4
    std::optional<PostRaceModel> Model() const override;    // car >= 0: car 0 (game mode 0: the winner's car, 0x80050B4C) in (0, 0x96, 0x160, 300)
    std::string Title() const override;
    uint32_t Colour() const override;
    bool SaveChosen() const { return save_; }               // left with "Save" (next view 0x8005B588)
    void AttachList();                                      // the list's row callback (after filling the state by hand)

    // State (the original's fields; public so that the checks can load it from a RAM dump).
    int16_t t = 0, car = -1, done = 0;                      // V+14 counter, V+16 car shown, V+18 buttons active
    uint32_t total = 0, fastest = 0;                        // W+8, W+10
    int16_t lapCount = 0;                                   // W+2
    int16_t battle = 0;                                     // W+0 (game mode 0: two columns)
    uint32_t total2 = 0, fastest2 = 0;                      // W+0xC, W+0x14 (game mode 0)
    std::vector<uint32_t> lapTimes, lapTimes2;              // W+0x18.., W+0x40.. (game mode 0)
    std::vector<int16_t> lapNumbers;                        // W+0x68..
    int16_t revealed = 0, revealPeriod = 0;                 // W+0x90 / W+0x92
    CourseTitle course;                                     // W+0x2B4
    ResultLabel resultsLabel, totalLabel, fastestLabel, lapLabel; // W+0x2F8 / 0x38C / 0x3F8 / 0x464
    TextObject place;                                       // W+0x348
    shell::Band placeBand, totalBand, fastestBand, lapBand; // W+0x370 / 0x3DC / 0x448 / 0x4B4
    FadePair totalFade, fastestFade, lapFade;               // 0x8005B6C0 / 0x8005B700 / 0x8005B740
    MenuListWidget list;                                    // 0x8005B76C
    ResultBar bar;                                          // W+0x4F8
    ResultDialog dialog;                                    // W+0x58C
    bool saveBar = true;                                    // 0x80050D00
    menu::OverlayModelCamera carCamera;                     // W+0x1D8 (0x80050BC4, turned by 0x80050CC4)
    int32_t frameLength = 256;                              // M+0x234 (u32 0x801F0698)

private:
    int32_t ListCallback(int command, int row, const MenuListRowDraw* d);
    const RaceMenuAssets& a_;
    mutable MenuOt* drawOt_ = nullptr;
    bool save_ = false;
};

// ---- B. BONUS

enum class BonusKind : uint8_t {
    kSingleRace,      // 0x80059A7C (view 0x8005D57C): the "Save Game?" bar
    kChampionshipRace,// 0x80059704 (view 0x8005D4EC): the dialog "Next"
    kChampionshipEnd  // 0x80059800 (view 0x8005D534): the bar, count 45 fields later, header colour of that view
};

struct BonusInput {
    BonusKind kind = BonusKind::kSingleRace;
    int place = 1;          // s8 0x801D5DE8 (1..6); the championship end shows place 1
    uint32_t prize = 0;     // credits added (0x801D55B0[place - 1]; the championship end: u32 0x801D55AC)
    uint32_t money = 0;     // the money after the prize (0x801D1568)
    bool prizeCar = false;  // a prize car was added (0x8005E7F0 returned nonzero): "New Car Acquired!!"
};

class BonusView : public PostRaceView {
public:
    static constexpr uint32_t kViewSingle = 0x8005D57Cu, kViewChampionship = 0x8005D4ECu, kViewChampionshipEnd = 0x8005D534u;
    explicit BonusView(const RaceMenuAssets& a);
    void Setup(const BonusInput& in);                        // 0x80059A7C / 0x80059704 / 0x80059800 -> 0x800595E0
    int Update(const MenuListPad* pad, bool input = true) override; // 0x80059BAC
    void Draw(MenuOt& ot) const override;                    // 0x8005A218
    std::optional<PostRaceModel> Model() const override;     // the championship end's trophy (W+0x208, W+0x20C > 0x17)
    std::string Title() const override;
    uint32_t Colour() const override;
    bool SaveChosen() const { return save_; }

    uint32_t view = kViewSingle;
    int16_t t = 0;                   // W+0
    uint8_t done = 0, closing = 0;   // W+2 / W+3
    int16_t placeIndex = 0;          // W+4
    uint32_t shown = 0, remaining = 0; // W+8 money shown, W+C prize still to count
    uint32_t car = 0;                // W+10 (nonzero: a prize car)
    uint32_t useBar = 1;             // W+14
    uint32_t speed = 3;              // W+18
    int16_t prizeFade = 0, moneyFade = 0; // W+1C / W+1E
    uint32_t model = 0;              // W+208 (the championship end's prize model)
    int16_t modelAnim = 0;           // W+20C
    menu::OverlayModelCamera trophyCamera; // W+0x354 (0x80049780(0x160, 0x1E0), moved by 0x800593F4)
    int16_t modelEnvX = 0, modelEnvY = 0;  // the model environment M+0xC0 (not set by this view's draw)
    ResultLabel resultsLabel, bonusLabel, moneyLabel; // W+0x20 / 0x98 / 0xE8
    TextObject place;                // W+0x70
    shell::Band newCarBand;          // 0x8005D454
    ResultBar bar;                   // W+0x138
    ResultDialog dialog;             // W+0x1CC

private:
    const RaceMenuAssets& a_;
    bool save_ = false;
};

// ---- C. the post-race event menu

struct PostMenuInput {
    int mode = 2;              // u8 0x801D5866: 2 "CHAMPIONSHIP", 0 "2PLAYER BATTLE", 11 "RALLY", else "SINGLE RACE"
    int race = 0;              // s16 0x801D5DF4 (the series' race index)
    int races = 0;             // s16 0x801D5DF6 (races of the series)
    int place = 1;             // s16 0x801D5E88 (<= 0: "Retire")
    uint32_t totalTime = 0;    // 0x801D5F80
    uint32_t fastestLap = 0;   // 0x801D5F58
    std::string course;        // 0x801D587C
    bool firstEntry = true;    // setup argument 0 (the list is reset)
    std::array<uint16_t, 2> wins{}; // mode 0: u16 career + 0xFC / + 0xFE (0x801C99DC / 0x801C99DE) as "win %d" (0x801C7842)
};

class PostRaceMenuView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005AE54u;
    explicit PostRaceMenuView(const RaceMenuAssets& a);
    PostRaceMenuView(const PostRaceMenuView&) = delete;
    PostRaceMenuView& operator=(const PostRaceMenuView&) = delete;

    void Setup(const PostMenuInput& in);                     // 0x80049D90
    int Update(const MenuListPad* pad, bool input = true) override; // 0x8004A0BC
    void Draw(MenuOt& ot) const override;                    // 0x8004A55C
    std::optional<PostRaceModel> Model() const override;     // car >= 0: car 0 in (0x7C, 0xA0, 200, 200)
    std::string Title() const override { return title; }
    uint32_t Colour() const override;
    void AttachList();
    std::string title;         // 0x8005AE64 (patched by the setup)

    // The chosen row's action (row +5: 0 Replay, 1 Try Again, 4 Continue / Next Session, 2 Exit (confirmed by the
    // "Exit?" bar in mode 2), -2 Save Replay; M+0x7C) once Update returned 1.
    int Action() const { return action_; }
    struct Row {
        std::string text;
        bool enabled = true;   // row +4 (the table: all 1; the game disables what it does not support)
        int8_t action = 0;     // row +5
    };
    std::vector<Row> rows;     // 0x801C90B0 -> 0x8005AD58 / 0x8005AD78 / 0x8005AD98 / 0x8005ADB0

    int16_t counter = 16, car = -1, confirming = 0; // V+14 / V+16 / V+18
    int mode = 2;
    std::vector<TextObject> rowText; // W + r * 0x28
    std::vector<shell::Band> rowBand; // W + 0x140 + r * 0x1C
    CourseTitle course;              // W+0x440
    ResultLabel resultsLabel, totalLabel, fastestLabel; // W+0x484 / 0x4D4 / 0x524 (1P)
    ResultLabel winsLabel1, winsLabel2;                 // W+0x574 / 0x5C4 (mode 0: "PLAYER 1" / "PLAYER 2" with the win counts)
    ResultBar bar;                   // W+0x654 ("Exit?")
    MenuListWidget list;             // 0x8005ADC0
    menu::OverlayModelCamera carCamera; // W+0x364 (0x80049780(200, 200), turned by 0x80049874)
    int32_t frameLength = 256;       // M+0x234

private:
    int32_t ListCallback(int command, int row, const MenuListRowDraw* d);
    const RaceMenuAssets& a_;
    mutable MenuOt* drawOt_ = nullptr;
    int action_ = 0;
};

// ---------------------------------------------------------------- frames

// The frame of a settled view (0x800479AC with M+0x211 = 0): the black clear, the header (0x80047024, `headerAlpha`),
// the view's draw. GPU order.
std::vector<MenuPrim> BuildPostRaceFrame(const RaceMenuAssets& a, const PostRaceView& view, int headerAlpha = 128);
// The model environment's 2D part of `model` (0x80048754 with +0xD4: the floor disc, GPU order), drawn after the clear.
std::vector<MenuPrim> BuildPostRaceModelFloor(const PostRaceModel& model);
// One view's primitives in its draw environment (the header 0x80047024 + the view's draw), before the environment's
// offset; no clear. GPU order.
std::vector<MenuPrim> BuildPostRaceViewPart(const RaceMenuAssets& a, const PostRaceView& view, int headerAlpha, bool entering);
// 0x800479AC with M+0x211 = c (1..16): the clear (M+0xB8), the previous view (env M+0xA0: header alpha c * 8, not
// entering, offset (0, (c - 16) * 5)), then the current one (env M+0x90: alpha 128 - c * 8, entering, offset
// (0, c * 200 / 16), drawing area from that y down) - the order of the captured transition frames
// (post_results_16104, post_bonus_16805, post_postmenu_17405). Sprites and tiles are cut to the frame here.
std::vector<MenuPrim> BuildPostRaceTransitionFrame(const RaceMenuAssets& a, const PostRaceView* previous, const PostRaceView& current, int c);

// The view manager's switch (0x800474F4 / 0x800479AC): update returns 1 -> the next view's setup, 16 transition
// fields (M+0x211) in which the previous view is still updated (arg 0) and drawn sliding out (header alpha c * 8,
// offset (0, (c - 16) * 5)) and the new one slides in (header alpha 128 - c * 8, offset (0, c * 200 / 16)).
class PostRaceFlow {
public:
    // Starts with `first` (already set up), entering as after a switch (transition = 16) or settled (0).
    void Start(std::unique_ptr<PostRaceView> first, bool transition = true);
    // Replaces the current view by `next` (already set up) with the 16-field transition.
    void Switch(std::unique_ptr<PostRaceView> next);
    // One field: the transition counter, the previous view's update(0), the current view's update(1). Returns what
    // the current view's update returned.
    int Update(const MenuListPad* pad);
    std::vector<MenuPrim> Frame(const RaceMenuAssets& a) const;
    // The frame with the model's floor disc after the clear (the model environment, GPU order) and the index of the
    // first primitive after it (where a renderer draws the 3D model), and the model; no model: `model` empty.
    std::vector<MenuPrim> Frame(const RaceMenuAssets& a, size_t& modelAt, std::optional<PostRaceModel>& model) const;
    PostRaceView* Current() const { return current_.get(); }
    PostRaceView* Previous() const { return transition_ > 0 ? previous_.get() : nullptr; }
    int Transition() const { return transition_; }

private:
    std::unique_ptr<PostRaceView> current_, previous_;
    int transition_ = 0;
};

} // namespace gt2::screens
