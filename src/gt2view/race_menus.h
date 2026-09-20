#pragma once
// The race overlay's full-screen menus as GPU primitives in draw order (gt2formats MenuPrim, the list the software
// canvas rasterises for comparisons and the native views draw): the licence test menu (view 0x8005B470, draw
// 0x8004F474), the event pre-race menu ("SINGLE RACE", view 0x8005D36C, draw 0x800585C0) and the settings screens.
// Pure CPU frame builders: the caller keeps the state (career / licence data) and passes plain structs.
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); code addresses in
// GT2.OVL member 0 (ovl0, at 0x80010000) unless marked EXE; evidence: our disassembly / Ghidra pseudo-C of the dumps
// work/re/rs_licmenu, work/re/rs_menus and the GP0 captures work/play/racescreens/cap/*.txt (gt2play --prims).
//
// Frame structure (0x800479AC, the view manager): a black TILE 352 x 480 (E1 0x200) in the manager's OT, then the
// view's ordering table of which the GPU draws the highest slot first: slot 4 = the view header (0x80047024), the
// view's draw function fills slots 0..2 (widgets put their bands and text-object glyphs one slot above the slot they
// are given: 0x8006C5DC draws glyphs into ot + 4).
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "game/career/tuning.h"
#include "game/shell/title_screens.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/race_menu_assets.h"

namespace gt2::screens {

struct ResultBar; // race_result_screens.h: the EXE's two-button bar (0x8006E1CC..0x8006E5B8)

// ---------------------------------------------------------------- building blocks

// An ordering table of kSlots entries as the race menus fill it (MenuOtSlot per entry: packets prepended); the
// GPU walks it from the highest slot to slot 0.
class MenuOt {
public:
    static constexpr int kSlots = 8;
    MenuOt();
    MenuOtSlot& operator[](int slot) { return slots_[size_t(slot)]; }
    // Appends every slot in GPU order; `mode` = the draw mode in effect before the table (E1).
    void Emit(std::vector<MenuPrim>& gpuOrder, uint16_t mode = 0x200) const;
private:
    std::array<MenuOtSlot, kSlots> slots_;
};

// The EXE's "text object" (0x28 bytes; init 0x8006C460, open 0x8006C4B0, restart 0x8006C514, close 0x8006C548,
// tick 0x8006C580, width 0x8006CD04, draw 0x8006C5DC):
//   +00 u8 revealDivisor, +01 u8 waveDivisor, +02 s16 steps, +04 s16 glowSpread, +06 s16 period, +08 s16 fadeSteps,
//   +0A s8 extra spacing, +0B u8 height, +0C u16 flags (bits 0..1 glyph semi-transparency mode, 2 underline TILE per
//   character, 3 selected (settled: a brightness wave c1 -> c0), 4 fixed cells, 5 semi-transparent glyphs, 6 glow
//   copies while revealing, 7..8 alignment 0 left / 0x80 centre / 0x100 right), +10 font descriptor, +14 colour c0,
//   +18 colour c1, +1C string, +20 u8 alpha, +22 s16 anim (-1 hidden, 0.. revealing, >= settle settled, < -1
//   closing), +24 s16 settle, +26 s16 length.
struct TextObject {
    uint8_t revealDivisor = 0, waveDivisor = 1;
    int16_t steps = 1, glowSpread = 0, period = 1, fadeSteps = 1;
    int8_t extra = 0;
    uint8_t height = 0;
    uint16_t flags = 0;
    uint32_t font = 0;
    uint32_t c0 = 0, c1 = 0;
    std::string text;
    uint8_t alpha = 128;
    int16_t anim = -1, settle = 0, length = 0;

    // 0x8006C460 (template = the first 0x1C bytes of a template in ovl0).
    static TextObject FromTemplate(const GuestImage& image, uint32_t address, std::string text);
    void Open(int settleAt = -1);  // 0x8006C4B0
    void Restart();                // 0x8006C514: open, then settled at once
    void Close();                  // 0x8006C548
    void Tick();                   // 0x8006C580
    void SetSettled(int phase = 0) { anim = int16_t(settle + phase); }
    int Width(const HudFont& font) const; // 0x8006CD04
    // 0x8006C5DC at (x, y) (y = baseline): glyphs into ot[slot + 1], the underline / glow / closing quads into ot[slot].
    void Draw(MenuOt& ot, int slot, int x, int y, const HudFont& font, bool thickUnderline = false) const;
};

// ---------------------------------------------------------------- licence test menu

// Rows of the licence menu's list (widget 0x8005B39C, row table 0x8005B364 + row * 8 {string, s8 enabled, s8 action}).
enum LicenceRow : int { kLicenceTestSelect = 0, kLicenceReplay, kLicenceStart, kLicenceRecords, kLicenceSaveReplay, kLicenceDemo, kLicenceExit, kLicenceRows };

struct LicenceMenuState {
    int licence = 5;              // 0x801D5867: 0 S, 1 IA, 2 IB, 3 IC, 4 A, 5 B
    int test = 0;                 // block +0x4B8 (0..9)
    std::string title;            // license_info_us title (block +0x4D8; RaceMenuAssets::Licence)
    std::vector<std::string> description; // its lines (block +0x4DC.., count +0x4FC)
    std::string carName;          // 0x801DA4B8 + test * 0x44 + licence * 0x2A8
    int16_t carPower = 0;         // +0x40: printed as power * 1000 / 0x3F6 with "%dhp" (0x801C70E9)
    int16_t carDrive = 1;         // +0x42: 0 FR, 2 4WD, 3 MR, 4 RR, others FF (0x801C70D2..)
    uint8_t launchSpeed = 0;      // test settings +0: "Launch Speed at %d mph" (0x801C70EE) of speed * 10000 / 0x3EDD
    std::array<uint32_t, 3> medalTimes{}; // gold / silver / bronze in 1/1000 s (0x8003D7B8 1..3; FormatRaceTime)
    std::array<uint8_t, 10> medals{};     // licence record +1 of every test (0x801CACF8 + licence * 0x668 + i * 0xA4): 0 none, 1..3 the sprite of 0x8005B150
    std::array<bool, kLicenceRows> rowEnabled{true, true, true, true, true, true, true}; // 0x8005B368 + row * 8
    int selectedRow = kLicenceStart;      // widget +6
    int flashPhase = 0;           // the selected row's text object: anim - settle (0..44; 0 = the brightest)
    int arrowPhase = 0;           // view +0x16: the arrows around the licence label while row 0 is selected (0x8006BA48)
    int carInfoFade = 0;          // block +0x51C (8 at the setup 0x8004ED00 and at a test change, -1 per update 0x8004EEB0): the car and
                                  // licence info (0x8004D7D0) at (0x80 - fade * 0x80 / 8) of the band's alpha
    bool dimDescription = false;  // view +0x1A == 1 (0x8004F474: the description band and text at a third)
    int alpha = 128;              // the menu's fade (band 0x8005B238: anim * 128 / 16)
    int listAlpha = 128;          // widget fade * 128 / fadeMax (0x8005B3C0 / 0x8005B3B0): the licence label
    int headerAlpha = 128;        // 0x80047024's alpha (view manager)
    // The TRANSMISSION bar W+0xEC (template 0x8005AC20 "TRANSMISSION" / "AT" / "MT" at (0xB0, 0x19A), 0x8004ED00), drawn
    // first by 0x8004F474 (0x8006E5B8); null = hidden (its anim -1 draws nothing).
    std::shared_ptr<const ResultBar> transmissionBar;
};

// Fills the licence-file parts of a state (title and description of license_info_us, labels) for `licence` / `test`.
LicenceMenuState LicenceMenuDefaults(const RaceMenuAssets& assets, int licence, int test);
std::vector<MenuPrim> BuildLicenceMenuFrame(const RaceMenuAssets& assets, const LicenceMenuState& state);

// ---------------------------------------------------------------- event pre-race menu

// Rows of the event menu (widget 0x8005D284, row table 0x8005D244 + row * 8 {string, s8 enabled, s8 action}, y
// offsets 0x8005D27C; the machine-test modes 7..9 use 0x8005D2B8 / 0x8005D2E8 instead). 0x80057EAC: outside race
// mode 10 the widget has 6 rows and row 5 is the Exit of row 6; in mode 10 row 5 is "Ghost Options ..." and row 6
// Exit. Rows 0 (Replay) and 3 (Save Replay) are enabled when a replay exists (0x801C90F4 == 0 && view +0x240 != 0).
enum EventRow : int { kEventReplay = 0, kEventTestRun, kEventSettings, kEventSaveReplay, kEventStartRace, kEventExit, kEventGhostExit, kEventRows };

struct EventMenuState {
    std::string title;                 // empty = the view's title (view 0x8005D36C +0x10 = 0x801C6E29)
    uint32_t titleColour = 0xFFFFFFFFu; // GP0 BGR; 0xFFFFFFFF = the view's colour (+0x0C)
    std::string course;                // the race block's course display name (0x801D587C)
    std::array<bool, kEventRows> rowEnabled{false, true, true, false, true, true, true}; // 0x8005D248 + row * 8
    int selectedRow = kEventReplay;
    int flashPhase = 0;                // the selected row's text object: anim - settle (0..44)
    int headerAlpha = 128;
    bool machineTest = false;          // race mode 7..9: the other row table
    bool ghostOptions = false;         // race mode 10: 7 rows (0x80057EAC)
    // The bars 0x80057EAC sets up and 0x800585C0 draws first (0x8006E5B8): W+0x484 TRANSMISSION (template 0x8005AC20) and
    // W+0x518 "Exit?" Yes / No (template 0x8005ACF0), both at (0xB0, 0x19A); null = hidden.
    std::shared_ptr<const ResultBar> transmissionBar;
    std::shared_ptr<const ResultBar> exitBar;
};
std::vector<MenuPrim> BuildEventMenuFrame(const RaceMenuAssets& assets, const EventMenuState& state);

// ---------------------------------------------------------------- settings: CHANGE PARTS (group selection)

// "Settings ..." of the event menu opens the view 0x8005D1C0 ("CHANGE PARTS" 0x801C7152, colour 0; update 0x8005731C,
// draw 0x80057450 -> 0x80053CA8(page 0x801C90F0, ot + 0x10): the page draws into ot[4] and ot[5], after the header).
// Page object (0x80053558(page, x 0, y 100)): +0 x, +2 y, +4 state (0 groups, 1 parts of a group, 2 the stages list),
// +5 group, +6 part, +8 group count (0x80052D40: 8 when 0x8005F7C8(sheet) != 0, else 7), +0xA scroll (-6..6),
// +0xC arrow phase (0..45), +0x10 flash (0..60), +0x14 band (template 0x8005C3C0: 0xDE x 0x40, colour =
// lerp(+0x30 previous group colour, +0x34 group colour, 128 - |scroll| * 128 / 6)).
// Groups: 0x80052CB4 picks the table 0x8005C364 (0x8005F7C8 && 0x8005F790), 0x8005C384 (0x8005F7C8 only) or 0x8005C3A4
// of pointers to 0x90-byte records {+0 name, +4 description (JP), +8 colour, +0xC s8 icon (0x8005B830 + icon * 12),
// +0x10 parts: 8 x {name, JP name, stage names (12-byte entries, string at +0), s16 kind (sheet stage index at sheet
// 0x8016E894 + 0x17B0 + kind * 2), u8 +0xF flags (bit 6: a sub-part, colour 0x020C3060 and 8 pixels lower)}}.
// Only state 0 (the group selection, as the page opens) is built here.
struct PartsPageState {
    int groupTable = 0;              // 0 = 0x8005C364, 1 = 0x8005C384, 2 = 0x8005C3A4 (see above)
    int groupCount = 8;              // page +8
    int group = 0;                   // page +5
    std::array<int16_t, 32> stages{}; // the sheet's stage of every part kind (0x8016E894 + 0x17B0)
    uint32_t previousColour = 0;     // page +0x30 (0 when the page has just opened: the band is then opaque)
    int arrowPhase = 0;              // page +0xC
    bool partsSetting = true;        // 0x801C90D0: "L1 - PARTS SETTING" (0x801C83E6) with its arrow
    int alpha = 128;                 // the page band's alpha (anim * 128 / 12)
    int headerAlpha = 128;
};
std::vector<MenuPrim> BuildPartsPageFrame(const RaceMenuAssets& assets, const PartsPageState& state);

// ---------------------------------------------------------------- settings: PARTS SETTING (machine settings)

// L1 on CHANGE PARTS (0x800536A4 returns -8) switches to the view 0x8005D1E4 ("PARTS SETTING" 0x801C7167, colour 0;
// update 0x800574C0 -> 0x80056194, draw 0x800575F8 -> 0x80056810(page 0x801C90F0 + 0x2A7C, ot + 0x10)); R1 goes back.
// Page: +0 x 0, +2 y 100, +4 state (0 groups, 1 rows, 2 slider), +5 group, +6 row, +8 group count, +0xC scroll,
// +0xE arrow phase, +0x10 flash, +0x92 s8[16] per row: GetSetting's count of the row's setting (0x80056000; > 0 = the
// row can be adjusted: a marker TILE and brighter texts), +0x224 band (template as CHANGE PARTS, colour lerp(+0x240
// previous, +0x244 group colour)).
// Groups: 0x80055B14 fills 0x8005D140 from the sheet's parts (suspension 0x8005C6E0 or 0x8005C650, brakes 0x8005C770,
// gears 0x8005D100[k] / 0x8005D120[k], aerodynamics 0x8005CFE0, others 0x8005D070 whose rows it writes at 0x8005D080).
// Group record: +0 name, +4 JP, +8 colour, +0xC s8 icon (0x8005B830 table), +0x10 rows: 16 x {u32 row record,
// s16 setting (tuning.h SettingKind)}. Row record (0x80055328): +0 name, +4 JP, +8 unit string, +0xC u8 value kind
// (0x800551BC: the configuration byte(s) shown: 0 springs +0x60 / +0x61, 1 ride height +0x5C / +0x5D, 2 damper bump
// +0x66 / +0x6A, 3 dampers +0x64 / +0x68, 4 camber +0x5A / +0x5B, 5 toe +0x5E / +0x5F - 0x80, 6 stabilisers
// +0x6C / +0x6D, 7 brake balance +0x50 / +0x51, 9..0x10 u16 at +0x3C + (kind - 8) * 2 (gears, final), 0x11 downforce
// +0x52 / +0x53, 0x16 +0x74, 0x17 +0x75; 8 and 0x12..0x17 are sliders (0x8005480C, not built here)), +0xD u8 flags:
// bits 0..3 the number format of 0x80054B9C (0 "%d", 1 "%d.%d" of v / 10, 2 "%d.%02d" of v / 100, 3 "%d.%03d" of
// v / 1000, 4 as 2 of v * 5; negative values get a '-'), bit 7 a second (rear) value. Texts: name Number(x, y, 1, -3,
// 0) in 0x024A250E (0x025E4D36 when adjustable), value NumberRight(x + 0x78 / x + 0xB8, 1, -3, 0) in 0x025E5E5E, unit
// Number(x + 0x7A / x + 0xBA, 0, -2, 0) in 0x02003060 (0x8005C46C..). Rows 30 pixels apart, 4 pixels to the right each.
// Only state 0 (as the page opens) is built here.
struct MachineSettingsState {
    std::vector<uint32_t> groups;    // 0x8005D140[0 .. count) as 0x80055B14 builds it (group record addresses)
    std::vector<std::pair<uint32_t, int16_t>> rows; // the current group's rows when not the image's (the "Others" group: 0x8005D080..)
    int group = 0;                   // page +5
    std::array<int8_t, 16> adjustable{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1}; // page +0x92
    std::array<uint8_t, 0x80> sheet{}; // the settings sheet 0x8016E894 +0x00..0x7F (its CarConfig)
    bool springValues = true;        // 0x8005F92C(sheet) == 1: the spring rate row shows its values
    uint32_t previousColour = 0;     // page +0x240
    int arrowPhase = 0;              // page +0xE
    bool changeParts = true;         // 0x801C90E4: "R1 - CHANGE PARTS" (0x801C83FE) with its arrow
    int alpha = 128;
    int headerAlpha = 128;
};
std::vector<MenuPrim> BuildMachineSettingsFrame(const RaceMenuAssets& assets, const MachineSettingsState& state);
// 0x800551BC + 0x80054B9C: the value text of a row record (front = the first value) from the sheet bytes.
std::string SettingValueText(const RaceMenuAssets& assets, uint32_t rowRecord, const std::array<uint8_t, 0x80>& sheet, bool front);

// ---------------------------------------------------------------- settings: PARTS SETTING, the interactive page

// 0x80055B14(): the groups of the page from the settings sheet (0x8016E894; predicates on its stages +0x17B0):
// suspension 0x8005C650 when stage[0] == 3 (0x8005F800), else 0x8005C6E0; brake controller 0x8005C770 when stage[2] > 0
// (0x8005F814); gears 0x8005D120[k] when stage[6] == 3 (0x8005F820), else 0x8005D100[k], k = the gear count byte at
// sheet + 0x1221 + stage[6] * 0x24 (0x8005F834); aerodynamics 0x8005CFE0 always; "Others" 0x8005D070 when it has rows
// (written at 0x8005D080, ended by {0, -1}): {0x8005C620 Yaw Control, 13} when stage[22] == 5 (0x8005F8F0), else the
// three LSD rows {0x8005C5F0, 10} {0x8005C600, 11} {0x8005C610, 12} when GetSetting(10 / 11 / 12) > 0 (0x8005F88C);
// then {0x8005C630, 14} when stage[20] == 1 (0x8005F904), {0x8005C640, 15} when stage[21] == 1 (0x8005F918).
struct MachineSettingsGroups {
    std::vector<uint32_t> groups;                         // 0x8005D140[0 .. count)
    std::vector<std::pair<uint32_t, int16_t>> otherRows;  // 0x8005D080.. {row record, setting}
};
MachineSettingsGroups BuildMachineSettingsGroups(const GuestImage& ovl0, const career::TuneSheet& sheet, const career::CareerData& data);
// 0x8005F92C(sheet): the byte at sheet + 0x9D3 + stage[0] * 0x4C is 0 (the spring rate row shows its values).
bool MachineSettingsSpringValues(const career::TuneSheet& sheet);

// The page of the view 0x8005D1E4 (enter 0x8005747C: view +0x14 = 12, 0x80055E90(P, 0, 100); update 0x800574C0 ->
// 0x80056194; draw 0x800575F8 -> 0x80056810) with its objects: page P = [0x801C90F0] + 0x2A7C, the popup list widget
// 0x8005D154 (row callback 0x80055D50, argument P), the popup band 0x8005D188 and the globals 0x801C90DC..0x801C90EC.
// State 0 selects a group (up / down), 1 a row of the group (cross / circle / right from 0), 2 edits the row's
// entries in the popup list of sliders (cross / circle on an adjustable row; cross / circle writes them with
// 0x8005F9DC, triangle / square drops them). Start in state 1 sets the row's setting to its default (0x80060410).
class MachineSettingsPage {
public:
    static constexpr uint32_t kOthersGroup = 0x8005D070u, kWidget = 0x8005D154u, kPopupBand = 0x8005D188u, kPageBand = 0x8005D1A4u;
    static constexpr uint32_t kEmptyString = 0x8005A9E0u;
    // 0x80056194's results (0x800574C0 plays the sound of Sound() and acts on -8 / -4).
    enum Code : int {
        kNothing = -1, kMoved = -2, kLeave = -4, kEntered = -5, kBack = -6, kNotAdjustable = -7, kChangeParts = -8,
    };
    // One slider object (0x18 bytes at P + 0xA4 + i * 0x18; init 0x80054CC8, set 0x80054CD4, tick 0x80054D10, draw
    // 0x80054E20): +0 label, +4 unit (guest string addresses), +8 u8 format (row flags & 0xF: 5 "Sports"/"Wide",
    // 6 LSD "Soft"/label/"Hard", 7 "Soft"/"Hard", else a value text of 0x80054B9C), +A value, +C min, +E max, +10 field,
    // +12 active (-1 unused), +14 phase 0..60.
    struct Slider {
        uint32_t label = 0, unit = 0;
        uint8_t format = 0;
        int16_t value = 0, min = 0, max = 0, field = 0;
        int16_t active = -1, phase = 0;
    };

    // 0x80055E90(P, 0, 100) after 0x8005747C: the groups (0x80055B14), group 0 (0x80056000), sliders unused, the
    // widget reset (0x8006CDCC), the bands closed. `ovl0` must outlive the page.
    void Init(const GuestImage& ovl0, const career::TuneSheet& sheet, const career::CareerData& data);
    // A page whose state was filled from elsewhere (a RAM dump) runs Update on this image.
    void Bind(const GuestImage& ovl0) { ovl0_ = &ovl0; }
    void StartOpen();  // 0x80055FD0: page band anim 0 (the view's countdown +0x14 reached 0)
    void StartClose(); // 0x80055FE0: page band closing (~steps), P + 0xA = -1
    // 0x80056194(P, pad, input): one field; `pad` null = no input. Returns a Code. `sounds` gets the slider's
    // 0x80060840(8) requests of this field.
    int Update(const MenuListPad* pad, bool input, career::TuneSheet& sheet, const career::CareerData& data);
    // 0x800574C0: the sound (0x80060840) of a result, -1 = none.
    static int Sound(int code);
    // 0x80047024 (view header "PARTS SETTING") + 0x80056810.
    std::vector<MenuPrim> Frame(const RaceMenuAssets& assets, const career::TuneSheet& sheet, const career::CareerData& data, int headerAlpha = 128) const;
    // 0x80056810 alone into the view's ordering table (slots 4 / 5): the page as a view of the race overlay's manager.
    void Draw(MenuOt& ot, const RaceMenuAssets& assets, const career::TuneSheet& sheet, const career::CareerData& data) const;

    // Group records: +0 name, +4 description, +8 colour, +0xC s8 icon, +0x10 16 x {row record, s16 setting};
    // "Others" takes its rows from `groups.otherRows`.
    uint32_t GroupAt(int g) const;
    int RowCount(const GuestImage& ovl0, uint32_t group) const; // 0x80055808
    uint32_t RowRecord(const GuestImage& ovl0, uint32_t group, int row) const;
    int16_t RowSetting(const GuestImage& ovl0, uint32_t group, int row) const;

    // Page P (+0x00 ..).
    int16_t x = 0, y = 100;                     // +0 / +2
    int8_t state = 0, group = 0, row = 0;       // +4 / +5 / +6
    int16_t groupCount = 0;                     // +8
    int16_t opening = -1;                       // +A: -1 until 0x80055FD0 (and after 0x80055FE0)
    int16_t scroll = 0;                         // +C group scroll -6..6
    int16_t arrowPhase = 0;                     // +E 0..45
    int16_t flash = 0;                          // +10 0..60
    std::array<career::SettingValue, 9> entries{}; // +12: GetSetting / SetSetting buffer
    std::array<int8_t, 16> counts{};            // +92: GetSetting's count of every row of the group
    std::array<Slider, 16> sliders{};           // +A4
    shell::Band band;                           // +224 (template 0x8005D1A4)
    uint32_t previousColour = 0, colour = 0;    // +240 / +244
    MenuListWidget widget;                      // 0x8005D154
    shell::Band popupBand;                      // 0x8005D188
    int16_t entryGroup = 0;                     // 0x801C90DC: the group of the entered row
    int16_t entryCount = 0;                     // 0x801C90E0: entries of the popup
    bool input = true;                          // 0x801C90E4
    uint32_t groupDescription = kEmptyString;   // 0x801C90E8 (drawn by the JP branch only)
    uint32_t description = kEmptyString;        // 0x801C90EC: the row record's +4
    MachineSettingsGroups groups;
    std::vector<int> sounds;

private:
    void SelectGroup(const GuestImage& ovl0, int g, const career::TuneSheet& sheet, const career::CareerData& data); // 0x80056000
    void SelectRow(const GuestImage& ovl0, int r);                                                                  // 0x8005613C
    int32_t WidgetUpdate(const MenuListPad* pad);                                                                   // 0x8006CFC4 with 0x80055D50
    void SliderTick(Slider& s, const MenuListPad* pad);                                                             // 0x80054D10
    const GuestImage* ovl0_ = nullptr;
};

// ---------------------------------------------------------------- shared

// 0x80047024: the view header in ot[4]: title (header font, spacing 2 at full alpha) grey 0x66 * a / 128 over a
// black copy at (+2, +3), a red TILE underline (x + 1, 78, width, 2) of 0xFF * a / 128, the gradient POLY_G4
// (0, 45 - 45a/128) .. (352, 45 + 45a/128) from the view colour * a / 128 to black, E1 0x220.
void AddRaceViewHeader(MenuOt& ot, const RaceMenuAssets& assets, const std::string& title, uint32_t colour, int alpha, bool entering = true);

// Software rendering of a frame on the assets' VRAM. `captureRules` = the rules of our interpreter's GPU (the
// captures' rasteriser: MenuCanvas::Rules::kInterpreter for polygons / lines / tiles and its sprite modulation and
// blending in 8 bits); else the PS1 rules.
MenuCanvas RenderRaceMenuFrame(const RaceMenuAssets& assets, const std::vector<MenuPrim>& prims, bool captureRules);

} // namespace gt2::screens
