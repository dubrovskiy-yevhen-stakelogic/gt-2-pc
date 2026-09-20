#pragma once
// The native GT-mode menu runtime: the current GM page, its items, the cursor, page history / back, the popup lists,
// the action dispatch of an item and the dynamic texts - ported from the GT-mode overlay (GT2.OVL member 4 at
// 0x80010000) of US Simulation v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a):
//   view update 0x80013EEC / page load half of the view draw 0x8001B3AC, page load + history 0x8001D2CC, cursor
//   0x8001E328 (+ 0x8001E22C / 0x8001E26C / 0x8001E924 / 0x8001E9D0), back 0x800142CC, actions 0x80014380, the
//   transaction object 0x801C3080 (0x8001DAA8 / 0x8001DB24 / 0x8001DA38 / 0x8001DA98 / 0x8001DB0C, check 0x8001DFEC,
//   run 0x8001DDAC, cancel 0x8001DD3C / 0x8001DCD0), the item pass 0x8001B9AC for the career-dependent types.
// No rendering API here: the view (gt2view/menu_view.h) draws Frame() with the page's pictures. Career rules are
// the career model's (src/game/career); what it does not provide yet is reported as "not available" (see
// CareerMenuActions), never faked. docs/formats/gt_menu.md.
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/career/events.h"
#include "game/career/garage.h"
#include "game/menu/menu_car.h"
#include "game/menu/menu_nav.h"
#include "gt2formats/gt_menu.h"
#include "gt2formats/gt_menu_images.h"
#include "gt2formats/gt_menu_list.h"

namespace gt2::menu {

// Pad words of the menu view (0x80013EEC: view +0x178 held, +0x17C pressed this frame, +0x184 auto-repeat pulses),
// in the logical bits of the EXE's mapping table 0x800A6F3C (gt2formats/gt_menu_list.h menu_list_pad): the cursor
// runs the item's action on 0xA00 (cross 0x200 or circle 0x800; 0x80014380's third argument = the cross bit) and
// goes back on 0x500 (triangle 0x100 or square 0x400).
namespace pad {
constexpr uint32_t kUp = 0x1, kDown = 0x2, kLeft = 0x4, kRight = 0x8;
constexpr uint32_t kTriangle = 0x100, kCross = 0x200, kSquare = 0x400, kCircle = 0x800;
constexpr uint32_t kChoose = kCross | kCircle, kBack = kTriangle | kSquare;
constexpr uint32_t kStart = 0x10000; // 0x8002055C: moves the selected garage car to the top of the list
} // namespace pad
struct PadState {
    uint32_t held = 0, pressed = 0, repeat = 0;
};

// Menu sound ids of 0x80060840: 0 buzzer, 1 accept, 2 back, 5 cursor move, 6 list move.
enum Sound : int { kSoundBuzzer = 0, kSoundAccept = 1, kSoundBack = 2, kSoundMove = 5, kSoundListMove = 6 };

// The transaction object (0x801C3080) the shop pages share.
struct Transaction {
    enum State : uint16_t { kNone = 0, kBuyCar = 1, kCarBought = 2, kBuyPart = 3, kFitPart = 4, kWheels = 5, kSell = 6, kSelect = 7 };
    uint16_t state = kNone;       // +00
    int32_t sellIndex = 0;        // +04 garage index (0x8001DA38)
    int32_t selectIndex = 0;      // +08 garage index (0x8001DA98)
    uint32_t carId = 0;           // +0C car to buy (0x8001DAA8)
    uint32_t paint = 0;           // +10 paint id of the displayed colour (0x80014380 case 2: 0x8001A454)
    int32_t partSlot = 0;         // +14 garage index of the part purchase (0x8001DB24: the current car)
    int32_t partKind = 0;         // +18 part kind
    int32_t powerBefore = 0;      // +1C (0x801C309C, 0x8001DB90)
    int32_t powerAfter = -1;      // +20 (0x801C30A0)
    int32_t owned = 0;            // +24 (0x801C30A4) "Purchased"
    int32_t wheelSlot = -1;       // +28 garage index of the wheel purchase (0x8001DAF0: the current car)
    uint32_t wheel = 0;           // +2C wheel id (0x80013A28)
    int32_t price = 0;            // +30 (0x801C30B0)
};

// The car the page shows (the car view object at page object + 0x28, 0x8001AC20; the 3D model is the view's).
struct CarView {
    bool shown = false;            // +0x1C
    uint32_t carId = 0;            // +0x0C
    uint32_t modelId = 0;
    int paintIndex = 0;            // +0x11 index into the car's paint list (0x8001A530 cycles it)
    int paintCount = 0;            // 0x8001A504
    int garageIndex = -1;          // -1 = a dealer car (0x8001AFA8), else a garage slot (0x8001B10C)
    bool current = false;          // +0x1D the garage slot is the current car
    // The data block at +0x394 (0x8001AFA8 / 0x8001B10C): year, dimensions (mm), weight (kg), displacement word,
    // drive (index of 0x80051260), power (PS) / rpm, torque (kgm x 10).
    int year = 0, length = 0, width = 0, height = 0, weight = 0, displacement = 0, drive = 0;
    int power = 0, powerRpm = 0, torque = 0, torqueRpm = 0, price = 0;
    int engineText[3] = {};        // +0x1C / +0x20 / +0x24: unistrdb ids of the engine texts (type 0xB8)
    std::string torqueRpmText;     // +0x3C.. (garage cars): "%drpm" of the figures' torque rpm
    // Wheels: the garage slot's wheel word (config +0, 0x8001AC20 with a garage car) or the wheel shop's preview
    // (0x80014380 bit 28 -> view +0x1C8 / +0x1CD, drawn by 0x8001AEF8); 0 = the car's own wheels.
    uint32_t wheelId = 0;
    int wheelColour = 0;
    bool wheelPreview = false;     // the wheel shop's preview (0x8001AEF8: dish = s16 0x80091A70[wheelColour]); else 0x8001AC20 (default dish)
};

// The parts page's figures of 0x8001DB90 (career::PartPreview).
struct PartFigures {
    int32_t price = -1, owned = 0, powerBefore = 0, powerAfter = -1;
};

// What 0x80014348 asks the 3D view to show (kind 1 = a car, 3 = a racing-modification body).
struct ShowCarRequest {
    int kind = 1;
    uint32_t modelId = 0, carId = 0;
    int paintIndex = 0;
    int garageIndex = -1;
};

// The career / host side of the item actions. The runtime (port of 0x80014380) decides WHAT happens; these calls
// do it. CareerMenuActions implements the career part with src/game/career; the host part (sound, race start,
// exit, 3D car) is given as callbacks by the program.
class MenuActions {
public:
    static constexpr int32_t kNotAvailable = -100; // EntryCheck: the check / race kind is not ported (no page change)
    virtual ~MenuActions() = default;
    virtual career::CareerState& Career() = 0;
    // 0x8001DFEC: check a transaction -> message page id (0x800000xx) or 0 (nothing); may change the state.
    virtual uint32_t CheckTransaction(Transaction& t) = 0;
    // 0x8001DDAC: run it (buy / sell / select / part / wheels) -> message page id or 0.
    virtual uint32_t RunTransaction(Transaction& t) = 0;
    // Bit 27 items: 0x8001973C (events) / 0x80019B88 (licence tests, names starting with 'L'): 1 admitted, else a
    // refusal with `message`.
    virtual int32_t EntryCheck(const std::string& name, uint32_t& message) = 0;
    // 0x801EF5F5 = 2 (checked race) / 3 (bit 30: no check): leave the menus for the race of `name`.
    virtual void StartRace(const std::string& name, int racePath) = 0;
    // Type 0x0B: 0x801EF5F5 = 0 (back to the title).
    virtual void Exit() = 0;
    // 0x8001A454 / 0x8001A530: the displayed paint and its change.
    virtual void Sound(int /*id*/) {}
    virtual void Music(int /*track*/) {}
    virtual void ShowCar(const ShowCarRequest& /*request*/) {}
    // A career action the career model does not provide yet (listed in the report): the host shows / logs it.
    virtual void NotAvailable(const std::string& /*what*/) {}
    // 0x8001DB90: power before / after part `kind` of the current car, its price and "owned".
    virtual PartFigures PreviewPart(int32_t /*kind*/) { return {}; }
    // 0x800174F4 (first = true) / 0x80017530: the racing-modification body shown / fitted next; its model id (0 = none).
    virtual uint32_t RacingBody(bool /*first*/) { return 0; }
    // 0x80013A28 / 0x80021BEC: the wheel id of a wheel shop code and its colour byte for the current car.
    virtual uint32_t WheelId(const std::string& /*code*/) { return 0; }
    virtual int WheelColour(uint32_t /*wheelId*/) { return 0; }
    // 0x80017288(slot): the tune sheet of a garage car (the parts page 0x80017318 reads its stages, +0x17B0).
    virtual bool CarStages(int /*garageIndex*/, std::array<int16_t, 27>& /*stages*/) { return false; }
};

// Menu-side data read from the disc once: the tables the career rules use, the menus' event list and records,
// the car names / colours, the overlay's lists.
struct MenuData {
    career::CareerData career;
    career::EventMenuData events;
    career::EventInfoTable eventInfos;
    std::vector<std::u16string> strings;         // carparam/usa_unistrdb.dat (0x80076C14)
    std::unique_ptr<CarColorNames> colours;      // .carcolor + .cclatain (0x800223C4)
    std::vector<uint32_t> unqualifiedCars;       // ovl4 0x80050B78 (0x80018210: type 0xAE refuses these)
    std::u16string noName;                       // ovl4 0x80050B9C "No Name" (0x800182A8)
    std::u16string racingPrefix;                 // ovl4 0x80050BB0 (0x8001828C(1)): drawn before modified cars' names
    std::map<uint32_t, std::string> globalFormats; // data-global.txd strings the menus use, by RAM address (0x801EF6C1 / 0x801EF6C6)
    NavVectors vectors;
    std::unique_ptr<UsedCarLists> usedCars;      // .usedcar_usa (the used-car lots, 0x800224E0)
    std::unique_ptr<MenuListNames> listNames;    // names / chip colours of the popup lists' rows
    static MenuData Load(const DiscImage& disc, const GtfsVolume& vol);
    std::u16string String(uint16_t index) const; // 0x80076C14
};

// The career model behind the actions (src/game/career). Unported career routines -> NotAvailable + a refusal.
class CareerMenuActions : public MenuActions {
public:
    CareerMenuActions(career::CareerState& state, const MenuData& data) : state_(state), data_(data) {}
    career::CareerState& Career() override { return state_; }
    uint32_t CheckTransaction(Transaction& t) override;
    uint32_t RunTransaction(Transaction& t) override;
    int32_t EntryCheck(const std::string& name, uint32_t& message) override;
    void StartRace(const std::string& name, int racePath) override { if (onRace) onRace(name, racePath); }
    void Exit() override { if (onExit) onExit(); }
    void Sound(int id) override { if (onSound) onSound(id); }
    void Music(int track) override { if (onMusic) onMusic(track); }
    void ShowCar(const ShowCarRequest& r) override { if (onShowCar) onShowCar(r); }
    void NotAvailable(const std::string& what) override { if (onNotAvailable) onNotAvailable(what); }
    PartFigures PreviewPart(int32_t kind) override;
    uint32_t RacingBody(bool first) override;
    uint32_t WheelId(const std::string& code) override;
    int WheelColour(uint32_t wheelId) override;
    bool CarStages(int garageIndex, std::array<int16_t, 27>& stages) override;

    std::function<void(const std::string&, int)> onRace;
    std::function<void()> onExit;
    std::function<void(int)> onSound, onMusic;
    std::function<void(const ShowCarRequest&)> onShowCar;
    std::function<void(const std::string&)> onNotAvailable;

    // 0x800173E8: the menus' tune sheet of the current car (0x800B4490), used by the entry check and 0x800174D0.
    const career::TuneSheet& CurrentSheet();
private:
    // The sheet holding garage car `index` (0x800173E8 = career::LoadCarSheet: clear with 0xFF, rows, configuration).
    career::TuneSheet& SheetOf(int index);
    career::CareerState& state_;
    const MenuData& data_;
    std::unique_ptr<career::TuneSheet> sheet_, purchaseSheet_;
    int16_t sheetCar_ = -2;
    uint32_t sheetCarId_ = 0;
    CarConfig sheetConfig_{};
    int16_t racingBody_ = 0; // 0x800B5C78: the racing-modification body chosen (0x800174F4 / 0x80017530)
    std::unique_ptr<career::TuneSheet> listSheet_; // 0x801D5FA0: the sheet of the garage car chosen in the list (0x80017288)
    int listSheetCar_ = -2;
    CarConfig listSheetConfig_{};
    std::vector<uint8_t> scratch_ = std::vector<uint8_t>(0x400, 0); // the scratchpad the record builder uses
};

// One popup list (types 9 / 0x50) as the runtime drives it; the widget itself (EXE 0x8006CFC4 / 0x8006D50C) is
// ported in gt2formats/gt_menu_list.h. `Update` returns the codes of 0x8002055C / 0x80020A94: row >= 0 chosen,
// -1 nothing, -2 closed.
class PopupList {
public:
    virtual ~PopupList() = default;
    virtual void Reset() = 0;                                                              // 0x80020490 / 0x800209B0
    virtual int Count() const = 0;
    virtual void Load(const MenuPage& page, const MenuItem& item, bool keepSelection) = 0; // 0x800204EC / 0x80020A0C
    virtual int Update(const PadState* pad, bool active) = 0;                              // 0x8002055C / 0x80020A94
    virtual int Selection() const = 0;                                                     // garage slot / lot row
    virtual uint32_t CarId() const { return 0; }                                           // 0x80020BDC
    virtual int PaintId() const { return 0; }                                              // 0x80020C00
    virtual int32_t Price() const { return 0; }                                            // 0x80020C24
    virtual void Draw(const MenuItem& item, std::vector<MenuPrim>& drawOrder) = 0;         // 0x8002068C / 0x80020B70
};

// The two popup lists of the career over the widget port (gt2formats/gt_menu_list.h MenuPopupList): garage rows from
// the career's garage, used-car rows of the page's maker for the current day.
std::unique_ptr<PopupList> MakeCareerPopupList(MenuListKind kind, const MenuAssets& assets, const MenuData& data, MenuActions& actions);

// The cursor object (view + 0x1D0; 0x8001E328).
struct Cursor {
    int16_t x = 0x100, y = 0xFC;          // +00 drawn position (the arrow's tip)
    uint8_t countdown = 0;                // +04 0 = snap to the target, 8 after a move (steps 7 / 6 accept diagonals)
    int8_t lastResult = 0;                // +05 1 = bounced
    int8_t direction = 0;                 // +06
    int16_t fromX = 0x100, fromY = 0xFC;  // +08 / +0A
    int16_t toX = 0x100, toY = 0xFC;      // +0C / +0E target (the item centre)
    int32_t vx = 0, vy = 0;               // +10 / +14 velocity (x 256 x 60)
    int32_t fx = 0, fy = 0;               // +18 / +1C position x 256
    uint8_t blink = 0;                    // +20 0..64
    int item = -1;                        // +24 the item it rests on (-1 none)
    void Reset();                         // 0x8001E22C
    void Bounce(int direction);           // 0x8001E26C
};

class MenuRuntime {
public:
    static constexpr uint32_t kWorldMap = 0;
    MenuRuntime(const MenuPages& pages, const MenuData& data, MenuActions& actions);

    // Enters the menus at `page` (the GT-mode entry: view init, cursor 0x8001E22C, page request with argument 0).
    void Start(uint32_t page = kWorldMap);
    // One frame of the view (the original runs one per 1/60 s field): 0x80013EEC, then the page load of 0x8001B3AC.
    void Update(const PadState& pad);

    const MenuPage& Page() const { return page_; }
    uint32_t PageId() const { return pageId_; }
    uint32_t BackTarget() const { return back_; }
    const Cursor& CursorState() const { return cursor_; }
    int PopupMode() const { return popup_; }
    const Transaction& CurrentTransaction() const { return transaction_; }
    const CarView& Car() const { return car_; }
    // The car view's camera (page object + 0x478): reset by every shown car, turned by every view update.
    const MenuCarCamera& Camera() const { return camera_; }
    // The name logo of the shown car (type 0x11): the host loads it (LoadMenuCarLogo) when Car().modelId changes
    // and uploads it into the page VRAM; nullopt = none.
    void SetCarLogo(std::optional<MenuCarLogo> logo) { logo_ = std::move(logo); }
    const std::optional<MenuCarLogo>& CarLogo() const { return logo_; }
    bool Loading() const { return pageCountdown_ != 0; }
    // Popup lists (null until attached: the widget port of gt2formats/gt_menu_list.h).
    void AttachLists(std::unique_ptr<PopupList> garage, std::unique_ptr<PopupList> usedCars);

    // The render state of the current frame for gt2formats' BuildMenuFrame: money, day, prices, event records,
    // the career-dependent item texts (dynamicItem), the popup lists (customItem), the cursor.
    MenuRenderState RenderState(const MenuAssets& assets) const;

    // Dev aid: requests a page like an action does (argument 0).
    void RequestPage(uint32_t page, uint32_t argument = 0);
    std::vector<std::string> log;  // one line per page change / action / refusal (the program prints and clears it)

private:
    SelectContext Context() const;
    void LoadPage(uint32_t id, uint32_t argument);    // 0x8001D2CC + the cursor placement of 0x8001B3AC
    void CursorUpdate(const PadState* pad);           // 0x8001E328 (pad null = input disabled)
    void CursorToDefault();                           // 0x8001E924
    void CursorToItem(int index);                     // 0x8001E9D0
    bool Action(int index, bool cross);               // 0x80014380 (third argument: the cross bit)
    void Back();                                      // 0x800142CC
    // 0x80014348: a car to show (view +0x1B0 = kind: 1 at this frame's draw, 3 two draws later).
    void ShowCarOf(int garageIndex, uint32_t carId, uint32_t modelId, int paintIndex, int kind = 1);
    struct PendingShow { int garageIndex = -1; uint32_t carId = 0, modelId = 0; int paintIndex = 0, kind = 1; };
    // 0x8001D5C8 (camera reset + 0x8001AC20) or, without the reset, 0x8001AC20 alone (0x8001AB6C).
    void DoShowCar(const PendingShow& show, bool resetCamera);
    bool DrawLate(const MenuAssets& assets, const MenuItem& item, std::vector<MenuPrim>& out) const; // the second OT (view +0x88)
    void NewPageRequest(uint32_t page, uint32_t argument);
    int PaintIndexOf(uint32_t carId, uint32_t paintId) const; // 0x80018350
    std::optional<CarCatalogueRow> Catalogue(uint32_t carId) const;
    bool DrawDynamic(const MenuAssets& assets, const MenuItem& item, std::vector<MenuPrim>& out) const;

    const MenuPages& pages_;
    const MenuData& data_;
    MenuActions& actions_;
    MenuPage page_;
    uint32_t pageId_ = 0, back_ = 0xFFFFFFFFu, backBefore_ = 0xFFFFFFFFu;
    bool loaded_ = false;
    int music_ = -1;
    Cursor cursor_;
    Transaction transaction_;
    CarView car_;
    int popup_ = 0;                  // +0x1CE: 0 page, 1 garage list, 2 used-car list, 3 wash wait
    int holdOff_ = 0;                // +0x19C
    int pageCountdown_ = 0;          // +0x1C4
    uint32_t nextPage_ = 0, nextArgument_ = 0; // +0x1BC / +0x1C0
    int backAllowed_ = 1;            // +0x1F8 (bit 0): 0x800142CC goes back only when set
    int washTimer_ = 0;              // page object +0x552 / +0x554
    int racingBodies_ = 0;           // 0x800174D0 of the current car's sheet
    int garageSelection_ = 0;        // 0x800A8D62: the garage slot chosen in the list
    std::unique_ptr<PopupList> garageList_, usedList_;
    PendingShow pendingShow_;
    int showPending_ = 0;            // view +0x1B0
    MenuCarCamera camera_;
    std::optional<MenuCarLogo> logo_;
};

} // namespace gt2::menu
