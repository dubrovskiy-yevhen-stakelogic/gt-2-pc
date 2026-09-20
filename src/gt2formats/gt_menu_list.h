#pragma once
// The popup list widget of the GT-mode menus: the used-car list of a dealer page (item type 0x50) and the garage
// list (type 9). The widget itself is resident EXE code (update 0x8006CFC4, draw 0x8006D50C, reset 0x8006CDCC, open
// 0x8006CE70, close 0x8006CED8, clamp 0x8006D4B0, primitive helpers 0x8006B548 / 0x8006B6E4 / 0x8006B814 /
// 0x8006B988 / 0x8006BB08 / 0x8007E780); the two widget objects, their row callbacks and the list objects the page
// drives are in GT2.OVL member 4 (GT-mode menus, loaded at 0x80010000):
//   garage:    widget 0x80052958, row callback 0x80020198, list 0x800A8D60, flash 0x800B9508; reset 0x80020490,
//              load 0x800204EC, update 0x8002055C, anchor 0x800204D8, draw 0x8002068C
//   used cars: widget 0x8005299C, row callback 0x800206F8, list 0x800A8D68, flash 0x800B950C; reset 0x800209B0,
//              load 0x80020A0C (rows 0x80022634), update 0x80020A94, anchor 0x800209F8, draw 0x80020B70, chosen row
//              0x80020BDC (car) / 0x80020C00 (paint) / 0x80020C24 (price)
//
// Facts of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a), from our disassembly of
// the RAM dumps with member 4 loaded and the GP0 primitives of the original (gt2play --prims); the full description
// with evidence is docs/formats/gt_menu.md section "Popup list widget".
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "gt2formats/car_params.h"
#include "gt2formats/gt_menu.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/gtmode_tables.h"

namespace gt2 {

class GtfsVolume;

// ---------------------------------------------------------------- pad

// The pad block the menu view hands to the lists (view +0x178, a copy made by 0x80083998 of the state that
// 0x800838B4 accumulates between two reads; null while the view ignores input). The widget reads `pressed` and
// `repeat`, 0x8002055C reads `pressed` bit 0x10000.
struct MenuListPad {
    uint32_t held = 0;      // +0x0 held at some time since the last read
    uint32_t pressed = 0;   // +0x4 pressed since the last read (edges)
    uint32_t released = 0;  // +0x8
    uint32_t repeat = 0;    // +0xC auto-repeat pulses
};
// Logical bits of those words: the raw pad bits mapped by the 16 pairs {raw bit, logical bit} at EXE 0x800A6F3C
// (0x80083A4C), raw bits in the standard pad order (0 select, 3 start, 4 up, 5 right, 6 down, 7 left, 8 L2, 9 R2,
// 10 L1, 11 R1, 12 triangle, 13 circle, 14 cross, 15 square).
namespace menu_list_pad {
constexpr uint32_t kUp = 0x1, kDown = 0x2, kLeft = 0x4, kRight = 0x8;
constexpr uint32_t kL1 = 0x10, kL2 = 0x20, kL3 = 0x40;
constexpr uint32_t kTriangle = 0x100, kCross = 0x200, kSquare = 0x400, kCircle = 0x800;
constexpr uint32_t kR1 = 0x1000, kR2 = 0x2000, kR3 = 0x4000, kStart = 0x10000, kSelect = 0x20000;
constexpr uint32_t kBack = kTriangle | kSquare;   // 0x500: the widget returns -1
constexpr uint32_t kChoose = kCross | kCircle;    // 0xA00: the widget returns the row (or -4)
} // namespace menu_list_pad

// ---------------------------------------------------------------- the widget (EXE)

// Commands of the per-list callback (widget +0x28, called as callback(command, &block, row)).
enum MenuListCommand : int {
    kMenuListReset = 0,   // 0x8006CDCC, every row (unless flags16 bit 0)
    kMenuListReveal = 1,  // 0x8006CFC4, one row every revealPeriod frames after an open (unless flags16 bit 1)
    kMenuListClose = 2,   // 0x8006CED8 every row but the selection; 0x8006CFC4 the selection when state reaches -58
    kMenuListTick = 3,    // 0x8006CFC4 every row, every frame (unless flags16 bit 3)
    kMenuListDraw = 4,    // 0x8006D50C every visible row (unless flags16 bit 4)
    kMenuListLeave = 5,   // 0x8006CFC4 the row the selection leaves
    kMenuListEnter = 6,   // 0x8006CFC4 the row the selection enters (both lists: flash counter = 0)
    kMenuListOpen = 7,    // 0x8006CE70 the selection (unless flags16 bit 7)
    kMenuListEnabled = 8, // 0x8006CFC4 on choose / 0x8006D50C per row: 0 = disabled (both lists: always 1)
};

class MenuOtSlot;
struct MenuListWidget;

// The block 0x8006D50C passes to command 4 (its stack at sp+0x20): +0 widget, +4 OT, +0xC x, +0xE row centre y,
// +0x10 alpha, +0x12 rows drawn so far, +0x14 enabled. Other commands get only +0 (the widget) and +4 (the pad).
struct MenuListRowDraw {
    MenuOtSlot* ot = nullptr;
    int16_t x = 0, y = 0;
    int16_t alpha = 128;   // 128; while scrolling: 128 - |scroll| * 16 for the row entering the view, |scroll| * 16
                           // for the row leaving it; halved when the row is disabled (command 8 returned 0)
    int16_t drawn = 0;
    bool enabled = true;
};
using MenuListCallback = std::function<int32_t(int command, const MenuListWidget& widget, int row, const MenuListRowDraw* draw)>;

// The widget object (0x30 bytes of guest state; the two lists' objects are initialised by the overlay image at
// 0x80052958 / 0x8005299C, and each list keeps its colours right behind it, see MenuListColours).
struct MenuListWidget {
    int16_t count = 0;          // +00 rows
    uint16_t flags = 0;         // +02 bit 2 wrap up/down, bit 3 draw the widget's own highlight bar, bit 4 left/right
                                //     page, bit 5 arrows without blinking (both lists: 0x14)
    int16_t visible = 0;        // +04 rows shown (garage 11, used cars 9)
    int16_t selection = 0;      // +06 selected row
    int16_t width = 0;          // +08 width of the own highlight bar (garage 0x180, used cars 0x1A0)
    int16_t rowHeight = 0;      // +0A (garage 18, used cars 22)
    int16_t rowGap = 0;         // +0C (2); row pitch = rowHeight + rowGap
    int8_t arrowHalfWidth = 0;  // +0E (8)
    int8_t arrowHeight = 0;     // +0F (10)
    int16_t x = 0, y = 0;       // +10 centre x, +12 top y (set before each draw by 0x800204D8 / 0x800209F8)
    int16_t fadeMax = 0;        // +14 (6)
    uint16_t callbackFlags = 0; // +16 bits 0 / 1 / 2 / 3 / 4 / 7 suppress commands 0 / 1 / 2 / 3 / 4 / 7 (both lists: 0)
    int16_t revealed = 0;       // +18 rows revealed (command 1)
    int16_t revealDelay = 0;    // +1A frames to the next reveal
    int16_t revealPeriod = 0;   // +1C (6)
    uint8_t active = 0;         // +1E 1 when the last update had a pad (the arrows are drawn only then)
    uint8_t byte1F = 0;         // +1F
    int16_t scroll = 0;         // +20 scroll animation: -8 / +8 after a move up / down, one step to 0 per update
    int16_t blink = 0;          // +22 0..60 (the own highlight bar's pulse), 0 after a move
    int16_t fade = 0;           // +24 0..fadeMax while open, counts down while closing (not drawn by these lists)
    int16_t state = -1;         // +26 -1 closed; 0..45 open (cycles; the arrows' blink); -65..-2 closing
    MenuListCallback callback;  // +28 (and +2C, the callback's argument, 0 for both lists)

    static constexpr uint32_t kGarage = 0x80052958u, kUsedCars = 0x8005299Cu;
    // The object as the overlay image initialises it (fields +00..+2F; the callback is not read).
    static MenuListWidget Read(const GuestImage& ovl4, uint32_t address);
};

void MenuListReset(MenuListWidget& w, MenuListCallback callback); // 0x8006CDCC
void MenuListOpen(MenuListWidget& w);                             // 0x8006CE70
void MenuListClose(MenuListWidget& w);                            // 0x8006CED8
void MenuListClamp(MenuListWidget& w, int lo, int hi);            // 0x8006D4B0
// 0x8006CFC4: one frame. `pad` null = no input this frame. Returns -2 nothing, -1 back (triangle / square), -3 the
// selection moved, -4 choose on a disabled row, >= 0 the row chosen (cross / circle).
int32_t MenuListUpdate(MenuListWidget& w, const MenuListPad* pad);
// 0x8006D50C: the visible rows (command 4 through the callback), the widget's highlight bar, the scroll arrows.
void MenuListDraw(const MenuListWidget& w, MenuOtSlot& ot);

// ---------------------------------------------------------------- ordering-table slot

// One OT entry as the menus fill it: every packet is prepended, so the GPU draws the packets in the reverse order
// of adding. Draw-mode packets (GP0 E1, 0x8007DA44) set the texture page, semi-transparency mode and dither bit
// of the primitives the GPU draws after them.
class MenuOtSlot {
public:
    void Add(const MenuPrim& p) { packets_.push_back({false, 0, p}); }
    void DrawMode(uint16_t e1) { packets_.push_back({true, e1, {}}); }
    // Glyph sprites of MenuTextWriter (generation order): each is a SPRT (0x80081478) followed by its E1
    // (tpage | 0x40), as 0x8001F38C / 0x8001F520 add them.
    void AddGlyphs(const std::vector<MenuPrim>& glyphs);
    // The packets of `later` (from its `skip`-th on) as if they had been added to this slot after the ones it holds.
    void Append(const MenuOtSlot& later, size_t skip = 0) {
        if (skip < later.packets_.size()) packets_.insert(packets_.end(), later.packets_.begin() + std::ptrdiff_t(skip), later.packets_.end());
    }
    // Appends the packets in GPU order; untextured primitives take the texture page / semi-transparency mode and
    // the dither bit of the draw mode in effect (`mode` = the one before the slot).
    void Emit(std::vector<MenuPrim>& gpuOrder, uint16_t mode = 0) const;
    // The draw mode in effect after the GPU walked the slot (`mode` = the one before it).
    uint16_t FinalMode(uint16_t mode) const;
    size_t Size() const { return packets_.size(); }
private:
    struct Packet { bool mode; uint16_t e1; MenuPrim prim; };
    std::vector<Packet> packets_;
};

// The EXE's primitive helpers (added to `ot` in the original's order):
//   0x8006B548 colour interpolation: a + (b - a) * clamp(t, 0, max) / max per channel, byte 3 of a kept.
uint32_t MenuListLerp(uint32_t a, uint32_t b, int t, int max);
//   0x8006B814 highlight bar centred at (x, y) w x h: two or four POLY_G4 0x3A from c0 to c1 growing with t (0..128),
//   then E1 0x20 | (t <= 64 ? 0x200 : 0).
void MenuListHighlight(MenuOtSlot& ot, int x, int y, int w, int h, uint32_t c0, uint32_t c1, int t);
//   0x8006BB08 paint chip in (x, y, w, h): POLY_G4 0x38 (grey alpha * 0xF4 >> 7, the chip colour, black, black) inset
//   by one pixel, a black TILE of the whole rectangle, E1 0x200.
void MenuListPaintChip(MenuOtSlot& ot, int x, int y, int w, int h, uint16_t colour15, int alpha);
//   0x8007E780 rectangle outline: a LINE when h or w is 1, else the 5-point polyline 0x48 around (x, y, w, h).
void MenuListFrame(MenuOtSlot& ot, uint32_t colour, int x, int y, int w, int h);
//   0x8006B988 scroll arrow: POLY_F3 0x22 (x, y + h), (x + w, y), (x - w, y), colour r = 255 * t' / 10, g = r / 2,
//   t' = clamp(40 - t, 0, 10); stored as a POLY_F4 with v3 = v2 (the second triangle is empty).
void MenuListArrow(MenuOtSlot& ot, int x, int y, int w, int h, int t);

// ---------------------------------------------------------------- rows

// One used-car row: the 8-byte lot entry (0x800B9544 period copy; list pointer 0x800B9510).
struct MenuUsedCarRow {
    uint32_t carId = 0;   // +0
    uint32_t price = 0;   // +4 & 0xFFFFFF
    int8_t paint = 0;     // +7 paint character
};
// 0x800224E0 (period (day / 10) % 60, the entries of cars hidden in the language dropped) + 0x80022634 (maker).
std::vector<MenuUsedCarRow> MenuUsedCarRows(const UsedCarLists& lots, const CarInfoDirectory& cars, uint32_t day, uint8_t maker,
                                            uint8_t language = kLanguageUsa);

// One garage row: the fields of garage slot i (RAM 0x801CD558 + i * 0xA4) that 0x80020198 reads.
struct MenuGarageRow {
    uint32_t carId = 0;     // +00 names (0x800182A8 / 0x800182FC)
    uint32_t paint = 0;     // +04 paint character (0x80060D28's second argument)
    uint32_t modelId = 0;   // +8C the car whose paint list gives the chip colour
    uint16_t word98 = 0;    // +98 bit 15 set = racing modification: the glyph string ovl4 0x80050BB0 before the name
};

// What the row callbacks read besides the rows: car names (0x800182A8 / 0x800182FC: catalogue row +3C / +3E ->
// carparam/usa_unistrdb.dat, "No Name" ovl4 0x80050B9C when the car is not in the catalogue) and chip colours
// (0x80060D28: .carinfoa paint list, the paint character matched case-insensitively (0x80060C90), index 0 when absent).
struct MenuListNames {
    CarInfoDirectory cars;
    CarParamTables params;
    std::vector<std::u16string> strings;
    std::u16string noName, racingPrefix;
    static MenuListNames Load(const GtfsVolume& vol, const GuestImage& ovl4);
    std::u16string Model(uint32_t carId) const;
    std::u16string Grade(uint32_t carId) const;
    uint16_t ChipColour(uint32_t carId, int32_t paint) const;
};

// The colours each list keeps behind its widget (ovl4; 0x8006B548 interpolates from `base` by the row alpha).
struct MenuListColours {
    uint32_t base = 0, text = 0, price = 0, frame = 0, marker = 0;
    // garage: base 0x8005298C, text 0x80052990, frame 0x80052994, current-car marker 0x80052998;
    // used cars: base 0x800529D0, text 0x800529D4, price 0x800529D8, frame 0x800529DC.
    static MenuListColours Read(const GuestImage& ovl4, bool garage);
};

// ---------------------------------------------------------------- the two lists

enum class MenuListKind : uint8_t { kGarage, kUsedCars };

// One popup list as the page drives it: the list object (0x800A8D60 / 0x800A8D68: s16 count, s16 chosen row,
// item pointer), its widget, rows and flash counter, and the row callback (0x80020198 / 0x800206F8). Not copyable
// (the widget's callback refers to the object).
class MenuPopupList {
public:
    MenuPopupList(MenuListKind kind, const MenuAssets& assets, const MenuListNames& names);
    MenuPopupList(const MenuPopupList&) = delete;
    MenuPopupList& operator=(const MenuPopupList&) = delete;

    // 0x80020490 / 0x800209B0: widget reset with the row callback, count 0, flash -1. The GT-mode entry
    // (0x80013CF8) resets both; the used-car list is reset again on every page load with argument 0 (0x8001D2CC).
    void Reset();
    // 0x800204EC: the garage list of the page (item type 9); `currentCar` = 0x801D156C (the row with the marker).
    void LoadGarage(const MenuItem* item, std::vector<MenuGarageRow> rows, int16_t currentCar);
    // 0x80020A0C: the used-car list of the page (item type 0x50), rows of MenuUsedCarRows(page maker).
    void LoadUsedCars(const MenuItem* item, std::vector<MenuUsedCarRow> rows);
    // 0x8002055C / 0x80020A94. `active` = the view is in this list's popup mode (0x80013EEC passes 1; 0 in page
    // mode, with pad null). Returns -1 nothing, -2 back (the view leaves the popup mode, sound 2), >= 0 the row
    // chosen (the view runs the item's action 0x80014380 with circle = 0). Moving plays sound 6; on the garage list
    // start (pressed bit 0x10000) when the widget returned -2 moves the selected car to the top (0x8001EF10(garage,
    // selection, 0), `moveToTop`) and plays sound 1.
    int32_t Update(const MenuListPad* pad, bool active);
    // 0x800204D8 + 0x8002068C (anchor 0x100, 0xB0) / 0x800209F8 + 0x80020B70 (anchor 0x100, 0x9C): the widget, or
    // the empty-list text (8-bit string 0x801C30C0 at (x - 152 / x - 188, y + 64), colour 0xF0780A) when count <= 0.
    // Appends in GPU order.
    void Draw(std::vector<MenuPrim>& gpuOrder);
    void Draw(MenuOtSlot& ot);

    // The chosen used car (0x80020BDC / 0x80020C00 / 0x80020C24 read the row of the widget's selection).
    MenuUsedCarRow ChosenUsedCar() const;

    const MenuListKind kind;
    int16_t count = 0;                 // list +0
    int16_t chosen = 0;                // list +2
    const MenuItem* item = nullptr;    // list +4
    int16_t flash = -1;                // 0x800B9508 / 0x800B950C: the row highlight's pulse (0..60, -1 = off)
    int16_t currentCar = -1;           // garage: 0x801D156C
    MenuListWidget widget;
    MenuListColours colours;
    std::vector<MenuGarageRow> garageRows;
    std::vector<MenuUsedCarRow> usedCarRows;
    std::function<void(int sound)> sound;          // 0x80060840
    std::function<void(int garageIndex)> moveToTop; // 0x8001EF10(garage, index, 0)

private:
    int32_t RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw);
    const MenuAssets& assets_;
    const MenuListNames& names_;
};

} // namespace gt2
