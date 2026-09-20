#pragma once
// The title's REPLAY THEATER (GT2.OVL member 1 of US Simulation v1.2, SCUS_944.88, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): the entry of the title's jump table 0x80020CB8 slot 1 (0x801EF5F3 = 1) runs
// the views
//   0x8004B2CC {0x800124A4 init: 20 fields, CD track 0 (0x80012414(0)), the demo file loaded (0x80020DCC) on the first
//               entry; 0x800124F0 update: pushes 0x8004B374 after 20 fields unless a replay was chosen (+0x18 == 1)}
//   0x8004B374 "REPLAY THEATER" (colour 0xD67890) {0x800126BC, 0x80012730, 0x80012870}: the list widget 0x8004B16C
//               (4 rows, rows = the 256 x 63 panels of arcade/topmenu_panels_us.tim, sprites 0x8004B104, row template
//               0x8004B168, row callback 0x800125D0 -> the title's row draw 0x80016410); a choice pushes the view of
//               table 0x8004B158:
//     0 Load Replay ...      0x8004B3C8 "LOAD REPLAY" (0xD63B54): the EXE card manager in mode 1 (0x80072EEC)
//     1 Rename & Delete ...  0x8004B41C "RENAME & DELETE" (0x288DC0): the card manager in mode 2 (0x80072F20)
//     2 Copy Replay ...      0x8004B470 "COPY REPLAY" (0x2878F2): member 1's copy screen 0x80015CF8 / 0x80015DC0
//     3 Demonstration ...    0x8004B224 "DEMONSTRATION" (0x1428DE): the demo file's replays (list 0x8004B1D8, rows
//                            0x80012B84) -> 0x8004B278 (20 fields, 0x80020E14 unpacks the chosen one)
// A loaded / chosen replay sets manager + 0x21C; the entry 0x80011384 then sets up the race (0x80010EDC) and runs the race
// overlay with argument 1 (replay); coming back, 0x801EF5F2 == 2 makes the title start in the replay theater again.
// Evidence: our disassembly / Ghidra pseudo-C of work/re/theater (RAM at the theater's screens, a card with a replay the
// original saved), GP0 captures work/play/theater/cap; docs/formats/replay.md section 9, docs/formats/title.md section 9.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/shell/title_menu.h"
#include "gt2formats/course_data.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/replay_card.h"
#include "gt2formats/title_assets.h"

namespace gt2 {
class GtfsVolume;
}

namespace gt2::shell {

// The row printer 0x8006A4E4's style object (0x800921BC for the card manager's list, 0x8004B20C for the demonstration):
// +0 the title line's font descriptor (0x801C94C4 / 0x801B9620: the medium font), +6 s16 its letter spacing, +8 the detail
// lines' font (0x801C94B4 / 0x801B95C0: small), +0xE s16 their spacing, +0x10 the row colour (0x5A4A3E; the card list
// writes 0x324052 there in delete mode).
struct ReplayRowStyle {
    int titleFont = TitleAssets::kMediumFont, detailFont = TitleAssets::kSmallFont;
    int titleSpacing = 1, detailSpacing = 1;
    uint32_t rowColour = 0x5A4A3E;
};
enum ReplayRowKind : int { kReplayRowEntry = 0, kReplayRowNewFile = 1, kReplayRowOk = 2, kReplayRowExit = 3 };

// What the row printer reads besides the entry: the course names (.crsinfo as loaded at 0x801E18E0: 0x80060EB4 finds the
// course file id, 0x80060E94 gives the record, whose +0 names the course; not found = record 0) and the EXE's "..."
// (0x8008FA74) for cut texts.
struct ReplayRowText {
    CourseInfoTable courses;
    std::string ellipsis = "...";
    static ReplayRowText Load(const GtfsVolume& vol, const TitleAssets& assets);
};

// 0x8006A4E4: one replay row centred at (x, y) with alpha 0..128 (the list's fade * row alpha), into `ot` (the list's
// OT slot + 1). Kind 0 = an entry: its title (medium font), three rules, the "%d sector" count, the kind ("Race", "License",
// "Time Trial", ... or "Ghost"), the course (a licence: the licence / test name of +0x44), the car name, each text behind a
// 4 x 6 marker TILE (0x8006A3FC, cut with "..." to 0x48 / 0xAC / 0x94 pixels by 0x8006AE98); kinds 1..3 = "- New File -",
// "- OK -", "- Exit -". Every kind ends with the row's background gradient (0x8006B77C, 0x120 x 0x54) and E1 0x220.
void DrawReplayRow(MenuOtSlot& ot, const TitleAssets& assets, const ReplayRowText& text, const ReplayRowStyle& style, const ReplayCardEntry* entry, int kind, int x, int y,
                   int alpha);

// The alpha and x of a row the replay lists draw (0x8006F060 / 0x80012B84 command 4): alpha = fade ratio * row alpha, the row
// slides in from the right by (0x80 - fade ratio) * 11 / 8 while the list opens. False = not drawn (closed / alpha 0).
bool ReplayRowPlacement(const MenuListWidget& w, const MenuListRowDraw& draw, int& x, int& alpha);

// ---------------------------------------------------------------- REPLAY THEATER (view 0x8004B374)

// The same panel list serves DATA TRANSFER (view 0x8004C548: init 0x8001DD24, update 0x8001DD98 - the code of 0x80012730 -,
// draw 0x8001DED8; list 0x8004C3B4 of 3 rows, template 0x8004C3B0, rows 0x8001DC38 with the sprites 0x8004B134 = entries 4..6).
struct PanelMenuLayout {
    uint32_t widget = 0, rowTemplate = 0, sprites = 0, viewColour = 0, viewTitle = 0;
};

class ReplayTheaterMenu {
public:
    static constexpr uint32_t kWidget = 0x8004B16Cu, kRowTemplate = 0x8004B168u, kSprites = 0x8004B104u;
    static constexpr uint32_t kViewColour = 0xD67890u, kViewTitle = 0x801B99B4u; // view 0x8004B374 +0x0C / +0x10
    static constexpr int kRows = 4;
    enum Choice : int { kLoadReplay = 0, kRenameDelete = 1, kCopyReplay = 2, kDemonstration = 3 };
    enum TransferChoice : int { kTrade = 0, kMixRecords = 1, kConvert = 2 };
    static PanelMenuLayout TheaterLayout() { return {kWidget, kRowTemplate, kSprites, kViewColour, kViewTitle}; }
    static PanelMenuLayout TransferLayout() { return {0x8004C3B4u, 0x8004C3B0u, 0x8004B134u, 0xD6142Cu, 0x801B9CCFu}; }

    explicit ReplayTheaterMenu(const TitleAssets& assets, const PanelMenuLayout& layout = TheaterLayout());
    ReplayTheaterMenu(const ReplayTheaterMenu&) = delete;
    ReplayTheaterMenu& operator=(const ReplayTheaterMenu&) = delete;

    // 0x800126BC(back): 20 fields before the list opens; `back` = returning from a sub-view (the selection kept).
    void Start(bool back);
    // 0x80012730: 0 running, 1 a row was chosen (`choice`), 2 left (triangle / square).
    int Update(const MenuListPad* pad);
    // 0x80012870 + the view manager's header (0x8001191C).
    std::vector<MenuPrim> Frame() const;

    int choice = 0;           // view + 0x18
    std::vector<int> sounds;  // 0x80060840 requests of the last update

private:
    int32_t RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw);
    const TitleAssets& assets_;
    PanelMenuLayout layout_;
    MenuListWidget widget_;
    std::array<TitleRowState, kRows> rows_{};
    std::array<TitleSprite, kRows> sprites_{};
    uint8_t templateFlags_ = 0, templateBrightness_ = 0;
    int16_t templateReveal_ = 0;
    int delay_ = 0;           // view + 0x14
};

// ---------------------------------------------------------------- DEMONSTRATION (view 0x8004B224)

// The demo file of the language: file id *(0x8004C8A8 + language * 4) (0x23 / 0x24 / 0x25 through the boot's file table
// 0x801E2EF0 = VOL records 0x2B / 0x2C / 0x2D: arcade/demofile_eu / _jp / _us.gmr; US = language 1 -> demofile_us), loaded by
// 0x80020DCC.
std::string DemoFilePath(const GuestImage& ovl1, uint8_t language);

class DemonstrationScreen {
public:
    static constexpr uint32_t kWidget = 0x8004B1D8u;
    static constexpr uint32_t kViewColour = 0x1428DEu, kViewTitle = 0x801BA469u; // view 0x8004B224

    DemonstrationScreen(const TitleAssets& assets, const ReplayRowText& text, const ReplayCardFile& demo);
    DemonstrationScreen(const DemonstrationScreen&) = delete;
    DemonstrationScreen& operator=(const DemonstrationScreen&) = delete;

    void Start();                          // 0x80012CE4(0)
    int Update(const MenuListPad* pad);    // 0x80012D34: 0 running, 1 chosen (`choice`), 2 left
    std::vector<MenuPrim> Frame() const;   // 0x80012E74 + the header

    int choice = 0;                        // 0x800A8DCC
    std::vector<int> sounds;

private:
    const TitleAssets& assets_;
    const ReplayRowText& text_;
    const ReplayCardFile& demo_;
    MenuListWidget widget_;
    ReplayRowStyle style_;
    int delay_ = 0;                        // 0x800A8DD0
    mutable MenuOtSlot* rowSlot_ = nullptr; // the rows' OT slot while Frame() draws (0x80012B84 draws into the list's slot + 1)
};

} // namespace gt2::shell
