#pragma once
// The race overlay's licence record screens as GPU primitives in draw order (gt2formats MenuPrim, like race_menus.h):
//   - "NEW RECORD" (view 0x8005B494: init 0x8004E5E8, update 0x8004E658, draw 0x8004E7D8), entered after a passed licence
//     test whose time ranks (0x8004E104 -> 0x8005DE8C >= 0): the name entry of the executable's keyboard widget
//     (EXE 0x80073548 init, 0x8007364C open, 0x800736E0 close, 0x80073720 update, 0x80073AFC draw; row callback
//     0x8007306C, delete 0x80073524) over the EXE list widget (gt_menu_list.h);
//   - "RECORD" (view 0x8005B4DC: init 0x8004FDC4, update 0x8004FE40, draw 0x80050084), the licence menu's "Records ..."
//     row: the five best times of a test (rows 0x8004FB30 / 0x8004F8B4, loaded by 0x8004FC8C, times 0x800495E4).
// The rules on the career's licence record (rank 0x8005DE8C, store 0x8005DEFC, sectors 0x8005DD94, the entered name) are
// in game/career/results.h.
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); ovl0 = GT2.OVL member 0
// at 0x80010000, "EXE" = the resident executable. Evidence: our disassembly of the race-overlay RAM dumps of
// work/re/spec_records (prims/kb_*.ram.bin, rec_*.ram.bin, lic_pass4) and the GP0 captures of the original
// (gt2play --prims: work/play/racescreens/cap/rec_*).
//
// Keyboard object (0x180 bytes into the race work block *0x801C90A0, i.e. 0x801B723C in the captures):
//   +00 descriptor (0x14 bytes, ovl0 0x8005B2B0): u32 font 0x801C9150, s8 cell width 16, s8 cell height 38, s8 cell gap
//       1, s8 row gap 2, s16 text page 6, s16 +0A glyph baseline in a cell 30, s16 +0C text spacing 2, s16 +0E centre
//       x 176, s16 +10 name baseline y 176, s16 +12 0;
//   +14 s16 anim (-1 closed, 0..12 opening, < -1 closing), +16 s16 highlight pulse (0..60), +18 s16 caret phase (0..40),
//   +1A s8 row (0..12), +1B s8 column (0..15; row 12: < 8 CANCEL, >= 8 OK), +1C s16 caret, +1E s16 maximum length
//   (the view: 11), +20 s16 width limit (the view: 256), +24 name buffer (the view: 0x801D156F);
//   +28 band (EXE template 0x80092338: 0 x 6, 12 steps, 0x02000084 -> 0x02000022, target 0xD4D4D4), its width = +20;
//   +44 list widget (EXE template 0x80092354: 13 rows, wrap, 5 visible, fade 12; callback 0x8007306C with the object).
// Character layout: EXE 0x8009226C, 12 rows of 16 codes; row 12 = CANCEL (data-global 0x801EFBE7) / OK (0x801EFBE4).
// Colours EXE 0x8009232C (cell 0x02322314), 0x80092330 (the cell at fade 0: 0x02B6B6B6), 0x80092334 (0x02000000).
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/title_screens.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/race_menu_assets.h"
#include "gt2view/race_menus.h"

namespace gt2::screens {

// ---------------------------------------------------------------- the keyboard widget (EXE)

class NameEntry {
public:
    static constexpr uint32_t kDescriptor = 0x8005B2B0u;      // ovl0: the NEW RECORD view's descriptor
    static constexpr uint32_t kBandTemplate = 0x80092338u;    // EXE
    static constexpr uint32_t kListTemplate = 0x80092354u;    // EXE
    static constexpr uint32_t kLayout = 0x8009226Cu;          // EXE: 12 x 16 character codes
    static constexpr uint32_t kColourCell = 0x8009232Cu, kColourCellOpening = 0x80092330u, kColourBase = 0x80092334u;
    static constexpr uint32_t kStrOk = 0x801EFBE4u, kStrCancel = 0x801EFBE7u; // data-global
    static constexpr int kColumns = 16, kButtonRow = 12, kFadeMax = 12;
    // Results of Update (0x80073720).
    static constexpr int kNothing = -2, kMoved = -3, kCancel = -1, kOk = 0;

    // 0x80073548(object, descriptor) and the view's 0x8004E5E8 (+0x1E = 11, +0x20 = 256).
    explicit NameEntry(const RaceMenuAssets& assets, uint32_t descriptor = kDescriptor);
    // The same widget over other assets (the executable's card manager of the title: descriptor EXE 0x800921A0 with the
    // title's fonts, shell::CardManager): the executable image (layout, colours, band / list templates), the image holding
    // the descriptor, the font of a descriptor address and the strings (OK / CANCEL) by address.
    using FontLookup = std::function<const HudFont&(uint32_t descriptor)>;
    using TextLookup = std::function<std::string(uint32_t address)>;
    NameEntry(const GuestImage& exe, const GuestImage& descriptorImage, uint32_t descriptor, FontLookup fontAt, TextLookup text);
    NameEntry(const NameEntry&) = delete; // the list's row callback refers to the object
    NameEntry& operator=(const NameEntry&) = delete;

    // 0x8007364C (row / column / caret / counters 0, the band opening, the list opened on row 0). The view then puts
    // the caret at the end of the name (0x8004E658: +1C = strlen).
    void Open();
    // 0x800736E0: the band closing, the list closing, anim -1.
    void Close();
    // 0x80073720: one frame with the pad (null = none). Plays `sound` (0x80060840): 2 delete / name full, 0 nothing to
    // delete / too wide, 1 a character typed, 5 a move. Returns kNothing, kMoved, kCancel (cross / circle on CANCEL) or
    // kOk (on OK).
    int Update(const MenuListPad* pad);
    // 0x80073AFC: the list (0x8006D50C with the row callback 0x8007306C), the name twice (context modes 1 and 2), the band,
    // E1 0x220, the caret arrow (0x8006B988), E1 0x20 - all into `ot`.
    void Draw(MenuOtSlot& ot) const;

    std::function<void(int sound)> sound;

    // Descriptor (+00..+13).
    uint32_t font = 0x801C9150u;
    int8_t cellWidth = 16, cellHeight = 38, cellGap = 1, rowGap = 2;
    int16_t page = 6, glyphBaseline = 30, spacing = 2, centreX = 176, nameY = 176;
    // State (+14..+24).
    int16_t anim = -1, pulse = 0, caretPhase = 0;
    int8_t row = 0, column = 0;
    int16_t caret = 0;
    int16_t maxLength = 11, widthLimit = 256;
    std::string name;
    shell::Band band;   // +28
    MenuListWidget list; // +44

private:
    int32_t RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) const;
    int Width(const std::string& s) const; // 0x8006AD3C with the object's font and spacing
    void Sound(int id) const {
        if (sound) sound(id);
    }
    const GuestImage& exe_;
    FontLookup fontAt_;
    TextLookup text_;
};

// The keyboard of the executable's memory-card manager (mgr + 0x6D8; descriptor EXE 0x800921A0: the medium font 0x801C94C4 of
// the manager's fonts, which the title's views point at 0x801B9620 / 95C0 / 95D0; + 0x1E = 31 characters, + 0x20 = 256 pixels)
// for shell::CardManager, over the title's assets (the arcade title: TitleAssets::SimLayoutScreens).
std::unique_ptr<shell::CardKeyboard> MakeTitleCardKeyboard(const TitleAssets& assets);

// 0x8006D400(widget, row): the selection set to `row` (clamped to the rows), scroll -8 / +8 by the direction, pulse 0,
// the callback's commands 5 (leave) and 6 (enter) when it changes.
void MenuListSelect(MenuListWidget& w, int row);

// ---------------------------------------------------------------- NEW RECORD (view 0x8005B494)

// The view: 0x8004E5E8 (keyboard init, view +0x14 = 24), 0x8004E658 (after 24 frames the keyboard opens with the caret
// at the end of the name; CANCEL plays sound 0 and stays; OK: 0x8005DEFC(record, 0x801D5E90, name), close, sound 3, the
// licence menu 0x8005B470, or 0x8005B4B8 when a licence / the prize car was just won).
class NewRecordView {
public:
    static constexpr uint32_t kView = 0x8005B494u; // ovl0: +0C colour 0x4066DD, +10 title
    explicit NewRecordView(const RaceMenuAssets& assets) : keyboard(assets) {}
    // 0x8004E5E8 with the name buffer's text (the name entered last).
    void Enter(const std::string& lastName);
    // 0x8004E658. Returns true when OK was chosen (the caller stores keyboard.name with 0x8005DEFC and leaves).
    bool Update(const MenuListPad* pad);
    int delay = 0; // view +0x14
    NameEntry keyboard;
};

// 0x800479AC with the view 0x8005B494: the header (0x80047024) and 0x8004E7D8 (the keyboard into ot[0]).
std::vector<MenuPrim> BuildNewRecordFrame(const RaceMenuAssets& assets, const NameEntry& keyboard, int headerAlpha = 128);

// ---------------------------------------------------------------- RECORD (view 0x8005B4DC)

// What 0x80050084 draws. The rows show the career's licence record live (0x8004FC8C keeps pointers to the record's
// time entries; the names are read from the record at every draw).
struct RecordsState {
    int licence = 5;                       // 0x801D5867
    int test = 0;                          // block +0x5DE
    std::string title;                     // block +0x4D8 (0x8004CCF8(licence, test): license_info_us)
    std::array<career::TimeRecord, 5> times{};
    std::array<std::string, 5> names;      // record +0x68 + i * 12
    int count = 0;                         // block +0x5DC: entries with a time (time[0] != -1) when loaded
    int slide = 128;                       // block +0x684: -1 = not loaded yet; 0..128 (rows slide in over 8 steps each, 4 apart)
    bool active = true;                    // view +0x18: the test title is drawn
    int arrowPhase = 0;                    // view +0x16 (0..45): the arrows around the licence label
    shell::Band band;                      // block +0x520 (ovl0 template 0x8005B238)
    int headerAlpha = 128;
};

// The view's logic: 0x8004FDC4 (+0x14 = 12, slide -1, the band opening), 0x8004FE40 (after 12 frames the rows load;
// left / right: sound 7, the previous / next test (wrapping over 0..9), rows reloaded; a face button (0xF00): sound 3,
// the band closing, leave).
class RecordsView {
public:
    static constexpr uint32_t kView = 0x8005B4DCu, kBandTemplate = 0x8005B238u;
    explicit RecordsView(const RaceMenuAssets& assets) : assets_(assets) {}
    void Enter(int licence, int test);
    // Returns true when the view is left (0x8004FE40 returned 2). `active` = view +0x18 (the view manager's input flag).
    bool Update(const MenuListPad* pad, bool active = true);
    // The frame's state with the career's record of the current test.
    RecordsState State(const career::CareerState& career) const;
    std::function<void(int sound)> sound;
    int licence = 5, test = 0, delay = 0, slide = -1, arrowPhase = 0;
    bool active = true;
    shell::Band band;

private:
    const RaceMenuAssets& assets_;
};

// Fills the rows of a state from a licence test record (0x8004FC8C: count of the entries with a time, the names).
void LoadRecordRows(RecordsState& state, const career::LicenceTestRecord& record);
std::vector<MenuPrim> BuildRecordsFrame(const RaceMenuAssets& assets, const RecordsState& state);

// EXE 0x800683FC(ot, {colour, x, y, w, h}, factor): the rounded bar - a TILE (x - w/2, y - h/2, w, h) and six POLY_F4 at
// each end through the half-circle points of EXE 0x80091A78 (x scaled by h * factor >> 17, y by h >> 12). The race
// end display passes 32, the RECORD rows 24 (0x8004F8B4).
void AddRoundedBar(MenuOtSlot& ot, const GuestImage& exe, uint32_t colour, int x, int y, int w, int h, int factor);

} // namespace gt2::screens
