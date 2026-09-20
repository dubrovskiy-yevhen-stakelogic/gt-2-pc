#pragma once
// The arcade menus of US Arcade v1.1 (GT2.OVL member 2; ARCADE v1.1 addresses, EXE SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95)
// natively: the view stack with its 16-field slide transitions (0x8001419C / 0x80014544 / 0x80014470), the view headers
// (0x80013828), the panel lists of ARCADE MODE / GAME SELECTION / LEVEL SELECTION / CLASS SELECTION (0x8001B6C0 .. 0x8001BB6C
// with the panel items 0x8001C0FC .. 0x8001C218 and the EXE highlight 0x8006B724), and the car and course selections. Frames are
// GPU primitive lists (gt2formats MenuPrim) of the 352 x 480 screen, drawn by gt2view/title_view natively and by MenuCanvas for
// comparisons with gt2play captures. docs/research/arcade_disc.md section 16.
//
// What is ported as the original does it: the views' order and cursor memory, the laps of Single Player (career + 3), the game
// mode table, the class unlock (0x8001D418) and course availability (0x8001D120), the car lists per class, the colour /
// transmission / tyre choices, the selection block handed to the race build (arcade_setup.h). The car and course selection
// screens are drawn in OUR layout from the original's data (their widgets are not decoded; section 16.6). Rally (GAME SELECTION row
// 1: the class view 0x800521E4 "Rally Car" -> class list 6) and Time Trial (row 2: CLASS SELECTION) lead to the mode-6 race build
// (section 17). ARCADE MODE row 1 (2 player Battle) leads to 2P GAME SELECTION (view 0x800520E8) and 2PLAYER BATTLE (0x800522E0,
// game/arcade/arcade_battle.h: both players' selections, the second one on the second controller) and COURSE SELECTION with the 2P
// course lists (kinds 3 / 4); the race build is mode 0 (arcade_setup.h, docs/research/arcade_disc.md section 19).
#include <array>
#include <map>
#include <memory>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/arcade/arcade_setup.h"
#include "game/arcade/arcade_widgets.h"
#include "game/shell/title_screens.h"
#include "gt2formats/course_map.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/hud_assets.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2::arcade {

// The VRAM and fonts of the arcade menus: 0x80013BC8 uploads (file id -> page, arcade file table 0x801E2950) arc_panels_us.tim
// (id 24 = 0x8004F408[language]) -> page 6, arc_maker.tim (16) -> 0x0B, arc_font.tim (11) -> 0x1E, arc_other.tim (17) -> 0x0F,
// arc_goodies_us.tim (14 = 0x8004F418[language]) -> 0x1C (image blocks at the page origins, 0x80013658); the four fonts of
// arc_fontinfo (the title's: cells 12 / 7 / 5 / 3).
// The menus' font descriptors: 0x801294F0 (cell 12, fonts[0]), 0x801294E0 (7, [1]), 0x801234D0 (5, [2]), 0x80129500 (3, [3]); the EXE's
// small font 0x80092E1C (a text object without template). arcade/arc_carlogo (the name logos of the 63 player cars, a container
// {u32 count, u32 offsets[]} of 4-bit TIMs) is loaded at 0x80129520 (0x80016E54 of the menus' entry, logo object view + 0x23C).
struct ArcadeMenuAssets {
    MenuVram vram;
    std::array<HudFont, 4> fonts;
    HudFont exeFont;
    GuestImage exe; // the executable (the EXE widgets' templates)
    // data-global.txd's language block as the title overlay leaves it at 0x801EF0E0 (member 1 0x8001D674: block language * 0x76F
    // of the gzip at member 1 0x8002247C) - the card screens' strings.
    static constexpr uint32_t kGlobalTextAt = 0x801EF0E0u, kGlobalTextStride = 0x76F, kGlobalTxdGzip = 0x8002247Cu;
    std::vector<uint8_t> globalText;
    ArcadeMenuData data;
    std::vector<uint8_t> carLogos;
    CourseInfoTable courses; // .crsinfo (the course records' index, 0x80060DC4)
    const GtfsVolume* vol = nullptr; // the car name logos of the garages' cars (carlogo/, 0x80016FB4) are read when a car is chosen
    std::map<std::string, CoursePicture> coursePictures; // arcade/course_map of every course of the lists (gt2formats/course_map.h)
    static ArcadeMenuAssets Load(const DiscImage& disc, const GtfsVolume& vol, uint8_t language = 1);
    const HudFont& FontAt(uint32_t descriptor) const;
    // A string the menus address: the menu text block (0x800F81E0), the global block (0x801EF0E0) or member 2's image ("" else).
    std::string AnyText(uint32_t address) const;
};

// ---------------------------------------------------------------- the panel list (0x8001B6C0 ..)

struct PanelSprite { // 12 bytes: u, v, clut, w, h, tpage
    uint8_t u = 0, v = 0;
    uint16_t clut = 0, w = 0, h = 0, tpage = 0;
};

// One row (0x14 bytes) with its panel object (0x18 bytes, 0x8001C0FC from the row's 12-byte template).
struct PanelItem {
    uint8_t kind = 1;       // +0 bit 0 = panel sprite, bit 7 = disabled (drawn at 0x20, chosen -> -4)
    int16_t height = -1;    // +2 (< 1 = the list's row height)
    int32_t result = 0;     // +0x10
    PanelSprite sprite;     // +0xC
    // panel object
    int16_t pulse = 0;      // +0 (template +2)
    int16_t steps = 0;      // +2 (template +4): reveal steps; the selected pulse runs steps .. steps + pulse
    uint8_t flags = 0;      // +4 (template +0): 1 selected, 2 semi colour, 4 reveal ghosts, 0x18 semi mode, 0x60 x anchor
    int16_t spread = 0;     // +6 (template +8): the reveal ghosts' distance
    int16_t x = 0, y = 0;   // +8 / +0xA centre
    uint8_t brightness = 0x80, target = 0x80; // +0xC (set by the list every draw) / +0xD
    int16_t scale = 0x80;   // +0xE
    int16_t anim = -1;      // +0x14: -1 hidden, 0.. revealing / shown, < -1 closing
};

void PanelItemTick(PanelItem& p);                  // 0x8001C1BC
void PanelItemDraw(const PanelItem& p, MenuOtSlot& ot); // 0x8001C218 (at p.x / p.y with p.brightness)

class PanelList {
public:
    static PanelList Read(const GuestImage& ovl2, uint32_t address, uint32_t itemsAddress = 0, int countOverride = -1);
    void Init();            // 0x8001B6C0
    void Open();            // 0x8001B744
    void Close();           // 0x8001B7D4
    // 0x8001B84C: the row's result, -1 back, -2 nothing, -3 moved, -4 a disabled row chosen; `pad` null = no input.
    int32_t Update(const MenuListPad* pad);
    void Draw(MenuOtSlot& ot) const; // 0x8001BB6C

    int8_t count = 0;
    uint8_t flags = 0;       // bit 2 = wrap
    int8_t visible = 0;
    int16_t width = 0, rowHeight = 0, gap = 0;
    int16_t x = 0, y = 0;
    int8_t selected = 0, revealed = 0;
    int16_t scroll = 0, highlight = 0, revealCountdown = 6, state = -1;
    std::vector<PanelItem> items;
};

// 0x80013828: a view's header (the view's title in the cell-12 font, letter spacing 0x22 - alpha / 4, centred on 352 at y 0x4C
// with a black copy at +3, +3; the underline TILE (x + 1, 0x4E, width, 2); the gradient POLY_G4 from the view colour).
void AddArcadeHeader(MenuOtSlot& textSlot, MenuOtSlot& gradientSlot, const HudFont& font, const std::string& title, uint32_t colour, int alpha);

// ---------------------------------------------------------------- the menus

enum class ArcadeView : uint8_t { kRoot, kMode, kGame, kLevel, kClass, kCar, kCourse, kFinal, kNotice, kRallyClass, kGarage, kBonus, kCredits, kGuestLoad,
                                  kGame2P, kBattle };
constexpr int kArcadeViewCount = 16;

class ArcadeCarPage;
class ArcadeBattlePage;
class ArcadeCoursePage;
class ArcadeBonusPage;
class ArcadeCreditsPage;
class CourseMovieSource;
class ArcadeGuestLoad;

class ArcadeMenus {
public:
    // kEnding: ENDING CREDITS chose a row: the top level plays its ending movie (member 5), then the title.
    enum Outcome : uint8_t { kStay, kRace, kTitle, kEnding };

    // `career` = the career block (0x7C9C bytes: +0 language, +3 / +6 laps, the unlock state).
    ArcadeMenus(const ArcadeMenuAssets& assets, const ArcadeData& data, const CarInfoDirectory& cars, std::span<const uint8_t> career);

    void Reset(bool afterRace = false);  // the root view (0x8001D54C); afterRace = re-entered (the cursors kept)
    // `pad2` = the second controller (the 2PLAYER BATTLE page's player 2; view + 0x1B4); null = none (no input for player 2).
    Outcome Update(const MenuListPad& pad, const MenuListPad* pad2 = nullptr);
    std::vector<MenuPrim> Frame() const;

    // The race context + selection (RAM 0x801C2EB0..) as the menus filled it; valid after Update returned kRace.
    const std::array<uint8_t, kMenuRegionSize>& Region() const { return region_; }
    std::span<const uint8_t> Career() const { return career_; }
    // The career write of the race's RESULTS setup (game/arcade/arcade_results.h ApplyArcadeRaceResult) on the menus' career;
    // the unlocks are recomputed at the next Reset (the root view's first init). Returns true when a flag byte changed.
    bool ApplyRaceResult(std::span<const uint8_t> raceBlock, int32_t place);
    // The career block (0x7C9C bytes) - replaced by a loaded save (the title's Load); Reset recomputes the unlocks.
    void SetCareer(std::span<const uint8_t> career);
    // The career block for the race's writes (Time Trial / Rally course records, arcade_results.h SetCourseRecord).
    std::span<uint8_t> MutableCareer() { return career_; }
    // After kRace: the race cannot run natively (Rally / Time Trial: the race shell's mode 6 is not ported). The final view
    // becomes a notice page; leaving it goes back to COURSE SELECTION.
    void RaceNotAvailable(const std::string& title);

    // The race of the menus' selection: 0x80010C84 (arcade_setup.h BuildArcadeRace) and, for a garage car, member 3's rebuild of its
    // entry (RebuildGarageEntry: the GT-mode tables of the disc, loaded on first use), whose tune-sheet store changes the garage
    // car - written back here (the home garage into the career, as the original's RAM).
    ArcadeRaceSetup BuildRace(const ArcadeSetupData& d, uint32_t p1, uint32_t p2, uint32_t vsync);
    // The garage blocks (0x4028 bytes): 0 = career + 0x3C74 (home), 1 = the guest garage (RAM 0x801D0FDC, behind the career).
    std::span<const uint8_t> GarageBlock(int g) const;
    void SetGuestGarage(std::span<const uint8_t> block);
    // The course movie decoder of COURSE SELECTION (arcade_course_page.h CourseMovieSource; null = no previews).
    void SetCourseMovieSource(CourseMovieSource* source);
    // After kEnding: the ENDING CREDITS row chosen (0 Arcade Mode -> movie 25, 1 Simulation Mode -> movie 26).
    int EndingRow() const { return endingRow_; }
    // The memory card slots LOAD GUEST GARAGE reads (.mcd images; no path = no card).
    void SetCards(const std::array<shell::CardSlot, 2>& slots);
    std::span<const uint8_t> GuestGarage() const { return guestGarage_; }
    // The car view of the car selection (the caller draws the 3D car): shown, car id, paint index.
    bool CarShown() const;
    uint32_t CarId() const;
    int CarPaint() const;
    const ArcadeCarPage& CarPage() const { return *carPage_; } // the car camera / projection / floor (arcade_car_page.h)
    // The 2PLAYER BATTLE page while it is on screen (its two 3D cars: arcade_battle.h CarShown / Projection / Floor), else null.
    const ArcadeBattlePage* BattleShown() const;
    ArcadeView Current() const { return stack_.empty() ? ArcadeView::kRoot : stack_.back(); }
    // The menus' VRAM (the assets' + the name logos the car page uploaded); the version changes with every upload.
    const MenuVram& Vram() const { return vram_; }
    uint32_t VramVersion() const;
    ~ArcadeMenus();
    std::vector<int> sounds; // effect numbers of 0x80060750 played this field (the caller plays them)

private:
    uint8_t* Sel() { return region_.data() + (kSelectionAddress - kMenuRegionAddress); }
    const uint8_t* Sel() const { return region_.data() + (kSelectionAddress - kMenuRegionAddress); }
    void Push(ArcadeView v);
    void Pop();
    void InitView(ArcadeView v, bool reenter);
    int UpdateView(ArcadeView v, const MenuListPad* pad); // 0 stay, 1 pushed, 2 back, 3 race, 4 title
    void DrawView(ArcadeView v, int alpha, int dy, std::vector<MenuPrim>& out) const;
    std::vector<uint8_t> ClassFlags(int cls) const;
    PanelList* ListOf(ArcadeView v);
    const PanelList* ListOf(ArcadeView v) const;
    std::string TitleOf(ArcadeView v) const;
    uint32_t ColourOf(ArcadeView v) const;
    const std::vector<std::string>& ClassCars() const;
    int CourseListKind() const { return listKind_; }

    const ArcadeMenuAssets& a_;
    const ArcadeData& data_;
    const CarInfoDirectory& cars_;
    std::vector<uint8_t> career_;
    std::vector<uint8_t> guestGarage_;
    std::unique_ptr<career::CareerData> gtData_; // the GT-mode tables for the garage cars' records (member 3 loads them)
    std::array<uint8_t, kMenuRegionSize> region_{};
    ClassUnlocks unlocks_;
    uint64_t unlockRevision_ = 0;
    std::array<std::vector<uint8_t>, ArcadeMenuData::kCourseListCount> courseFlags_;
    PanelList mode_, game_, level_, class_, rally_;
    std::vector<ArcadeView> stack_;
    ArcadeView old_ = ArcadeView::kRoot; // the view leaving during a transition
    int transition_ = 0;                 // 0x212: 16 .. 0
    bool popping_ = false;               // 0x214
    int viewTimer_[kArcadeViewCount] = {};  // view + 0x14 per view
    int viewChoice_[kArcadeViewCount] = {}; // view + 0x18
    int8_t cursorMode_ = 0, cursorGame_ = 0, cursorLevel_ = 0, cursorClass_ = 0, cursorRally_ = 0; // 0x801D5004..08
    uint32_t carColour_ = 0;             // the CAR SELECTION view's colour (+0x0C of 0x80052238): 0x8004FAC0[class], 0x2084B6 for Rally
    int listKind_ = 0;                   // 0x800F364C
    std::unique_ptr<ArcadeCarPage> carPage_;
    std::unique_ptr<ArcadeBattlePage> battlePage_; // 2PLAYER BATTLE (arcade_battle.h)
    PanelList game2p_;                   // 2P GAME SELECTION 0x8004F980
    int8_t cursor2P_ = 0;                // 0x801D5009
    const MenuListPad* pad2_ = nullptr;  // this field's second controller (Update)
    MenuVram vram_;                      // the menus' VRAM (the car page uploads its name logos)
    mutable TextCtx ctx_;                // the EXE text context (view + 0x1C4): its semi mode persists across draws
    std::unique_ptr<ArcadeCoursePage> coursePage_;
    std::unique_ptr<ArcadeBonusPage> bonusPage_;     // BONUS ITEMS (arcade_bonus.h)
    std::unique_ptr<ArcadeCreditsPage> creditsPage_; // ENDING CREDITS
    std::unique_ptr<ArcadeGuestLoad> guestLoad_;     // LOAD GUEST GARAGE (arcade_card.h)
    std::string notice_;
    [[maybe_unused]] int noticeTimer_ = 0;
    int endingRow_ = -1, endingTimer_ = 0; // view 0x80052670: 24 fields, then result 5 / 6 (0x800F36AE = row != 0)
};

} // namespace gt2::arcade
