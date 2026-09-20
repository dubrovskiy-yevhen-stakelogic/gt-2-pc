#pragma once
// CAR SELECTION of the arcade menus (view 0x80052238 of GT2.OVL member 2, US Arcade v1.1 SCUS_944.55 SHA-1
// 231f9dba7191b9ef915621662afdc40a7c66df95; ARCADE v1.1 addresses): init 0x8001E890, update 0x8001EAA8, draw 0x8001F124.
// docs/research/arcade_disc.md section 18.
//
// The page is a carousel of the class's cars (0x800F04B0, page callback 0x8001E5F8) over the car's widgets (the object 0x800F05C0,
// two copies so that the old car's widgets close while the new one's open): the drive badge and power text (0x8001C218 +
// 0x8001A204), the power / torque graph (0x800121C4 .. 0x80013034), the three bars Max Speed / Handling / Acceleration (0x8001852C
// .. 0x8001863C), the colour chips (0x80018230 .. 0x800182E0) and the spec box Weight / Max Power / Max Torque (0x80018930 ..
// 0x80018EB8); then the maker logo (arc_maker, sprites 0x8004F5F0) and the name logo (arc_carlogo entry of the car's model number,
// 0x80016ECC / 0x80016CA4 / 0x8001713C), the grey rule 0x8004FB8C, and the TRANSMISSION / SETTINGS bars (0x8001AA98 ..).
// The spec data: 0x800194FC(object, badge 0x80016A10, curve, bars 0x80051F84, figures 0x80051FA0, paints, chip colours) with the
// curve = 0x80075840 (Simulation 0x80075930, career::CarPowerFigures) of the record 0x800770BC builds from the car's row of
// player table 32 (0x80076864 in data mode 1 -> 0x80076ED0, arcade ConfigFromCarSpec).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "game/arcade/arcade_menus.h"
#include "game/arcade/arcade_widgets.h"
#include "game/menu/menu_car.h"

namespace gt2::arcade {

// ---------------------------------------------------------------- the car's widgets

struct ArcGraph { // 0x44 bytes (template 0x8004F488)
    uint8_t mode = 1, reveal = 0x14;       // +0 / +1
    int16_t x = 0, y = 0, w = 0, h = 0;    // +2 .. +8
    int8_t labelGap = 0, digitShift = 0, labelSize = 0; // +0xA / +0xB / +0xC
    const HudFont* font = nullptr;         // +0x10
    uint32_t power = 0, torque = 0, grid = 0; // +0x14 / +0x18 / +0x1C
    int16_t maxRpm = 0, maxHp = 0, maxTorque = 0; // +0x20 / +0x22 / +0x24
    std::array<int16_t, 80> powerSamples{}, torqueSamples{}; // object + 0x9FC / 0xB3C (+ copy * 0xA0)
    int16_t anim = -1;                     // +0x30
    int16_t hpMax = 0, hpTicks = 0, hpStep = 0, tqMax = 0, tqTicks = 0, tqStep = 0, rpmTicks = 0, samples = 0; // +0x32 .. +0x40
    int16_t animMax = 0x5F;                // +0x42
    std::string unitPower, unitTorque, unitRpm; // "hp" 0x800F829A, "lb-ft" 0x800F829D, "1000rpm" 0x800F82A3 (data-arcade.txd)

    void Init(const ArcadeMenuAssets& a, uint32_t templ); // 0x800121C4
    void Open();                           // 0x80012288 (+ 0x8001234C)
    void Close() { if (anim > 0) anim = -16; } // 0x800122D0
    void Tick();                           // 0x800122EC
    void Draw(MenuOtSlot* ot, TextCtx& c) const; // 0x80013034
private:
    void Axis(MenuOtSlot* ot, TextCtx& c, bool torqueAxis) const; // 0x80012484
    void RpmAxis(MenuOtSlot* ot, TextCtx& c) const;               // 0x8001292C
    void Curve(MenuOtSlot* ot, bool torqueCurve) const;           // 0x80012D1C
};

struct ArcBar { // 0x34 bytes (0x8001852C)
    int16_t anim = -1;
    ArcText label;
    const ArcadeMenuAssets* unknownFont = nullptr; // the font 0x80129500 of the "unknown" (0x800F8361) a bar without value shows
    std::string unknownText;
    void Open() { anim = 0; label.Open(-1); }    // 0x80018584
    void Close() { anim = -1; label.Close(); }   // 0x800185AC
    void Tick();                                 // 0x800185D4
    void Draw(MenuOtSlot* ot, int x, int y, TextCtx& c, int value) const; // 0x8001863C
};

struct ArcChips { // 0x86 bytes (0x80018230)
    int16_t selected = 0, count = 0;
    std::array<uint16_t, 64> colours{};
    int16_t anim = -1;
    void Set(int n, const std::vector<uint16_t>& chips); // 0x80018240
    void Close() { anim = -17; }                          // 0x80018284
    void Tick();                                          // 0x80018290
    void Draw(MenuOtSlot& ot, int x, int y) const;        // 0x800182E0
};

struct ArcSpecBox { // 0x250 bytes (0x80018930)
    std::array<ArcText, 6> text; // Weight, its value, Max Power, value, Max Torque, value
    int16_t labelWidth = 0, valueWidth = 0, anim = -1;
    void Init(const ArcadeMenuAssets& a);
    void Fill(const ArcadeMenuAssets& a, const std::array<int16_t, 5>& figures, TextCtx& c); // 0x80018A3C
    void Reset();                                  // 0x80018DE0
    void Close();                                  // 0x80018E04
    void Tick();                                   // 0x80018E6C
    void Draw(MenuOtSlot* ot, int x, int y, TextCtx& c) const; // 0x80018EB8
};

// What 0x800194FC shows of a car.
struct ArcCarFigures {
    int badge = 0;                        // 0x80016A10: sprite of *(0x8004F444 + language * 4)
    std::vector<uint16_t> chips;          // the paints' chip colours (.carinfoa)
    std::array<uint8_t, 3> bars{};
    std::array<int16_t, 5> figures{};     // power PS, its rpm, torque kgm x 10, its rpm (< 0: "~"), weight kg
    bool curve = false;
    std::array<uint8_t, 0x6C> powerCurve{}; // 0x80075840's figures (+0 power, +4 torque, +0xA count, +0xC rpm, +0x2C power, +0x4C torque)
};

class ArcCarSpecs { // 0x800F05C0
public:
    void Init(const ArcadeMenuAssets& a);                                        // 0x80019048
    void Fill(const ArcadeMenuAssets& a, const ArcCarFigures& f, uint8_t language, TextCtx& c); // 0x800194FC
    void Swap();                                                                 // 0x80019330
    void CloseCurrent();                                                         // 0x800191F4
    void SelectChip(int paint) { copy_[size_t(current_)].chips.selected = int16_t(paint); } // 0x800192F0
    void Tick();                                                                 // 0x80019A80
    // 0x80019B64(object, ot, x, y, ctx, current, which): widget `which` of the current copy (or of the other one).
    void Draw(int which, bool current, MenuOtSlot* ot, int x, int y, TextCtx& c) const;
private:
    struct Copy {
        ArcText power;
        PanelItem badge;
        ArcGraph graph;
        std::array<ArcBar, 3> bars;
        std::array<uint8_t, 3> values{};
        ArcChips chips;
        ArcSpecBox spec;
        bool filled = false;
    };
    std::array<Copy, 2> copy_;
    int current_ = 0;
};

// The name logo object (view + 0x23C, 0x2C bytes; 0x80016E54 with tpage 0x16): two slots (v 0 / 128 of the page) so that the old
// car's logo stays while the new one's is loaded; 0x80016ECC (set), 0x80016E84 (clear), 0x80016CA4 (upload), 0x8001713C (draw).
struct ArcNameLogo {
    uint16_t tpage = 0x16;
    int slot = 0;                       // +8
    std::array<uint32_t, 2> ids{};      // +0x14
    std::array<bool, 2> loaded{};       // +0x1C
    std::array<int16_t, 2> w{}, h{};    // +0xA / +0xE
    uint32_t uploads = 0;
    void Clear() { ids = {}; loaded = {}; slot = 0; }
    // 0x80016ECC + the upload 0x80016CA4 at once (the original uploads in its draw callback 0x80015654 one or two fields later).
    void Set(const ArcadeMenuAssets& a, MenuVram& vram, int model, uint32_t carId);
    // `scale` > 0 (the 2PLAYER BATTLE page: 0x40): a POLY_FT4 of (w * scale >> 8) half width instead of the sprite.
    void Draw(MenuOtSlot& ot, uint32_t carId, int x, int y, int brightness, bool centred, int scale = 0) const;
};

// 0x800194FC's inputs for car `index` of class list `cls` (0x80051F84 bars, 0x80051FA0 figures, the .carinfoa chips, the power
// curve 0x80075840 of the record 0x800770BC; the drive badge 0x80016A10).
ArcCarFigures ClassCarFigures(const ArcadeMenuAssets& a, const ArcadeData& data, const CarInfoDirectory& cars, int cls, int index);

// ---------------------------------------------------------------- the page

// ---------------------------------------------------------------- the garages

// The career's garage blocks (the GT-mode garage of the shared save: career + 0x3C74 + g * 0x4028, g 0 home / 1 guest; +0 s16
// count, +4 100 cars of 0xA4 bytes, career::GarageCar) and the menus' per-car summary 0x80019F44 / 0x80019E88 (0x800EFAB8 home,
// 0x800EFEA8 guest: 10 bytes per car).
constexpr size_t kGarageBlock = 0x3C74, kGarageStride = 0x4028, kGarageCarSize = 0xA4;
struct ArcGarageEntry {
    uint8_t rally = 0;   // +1: 0x8005E784(car, 0x2D): the car owns dirt tyres (the Rally lists enable only these)
    uint8_t cls = 3;     // +2: 0 S .. 3 C by weight * 300000 / power^2 (0x80019E44) < 0xE10 / 0x170C / 0x2AE5
    uint8_t drive = 0;   // +3: car + 0x94 >> 13
    int16_t power = 0;   // +4: car + 0x98 & 0x3FFF (PS)
    int16_t torque = 0;  // +6: car + 0x96 (kgm x 10)
    int16_t weight = 0;  // +8: car + 0x94 & 0x1FFF (kg)
};
// A garage block (0x4028 bytes): the home garage = career + 0x3C74 (in the save), the guest garage = the block behind the career
// (RAM 0x801D0FDC; filled by LOAD GUEST GARAGE, not saved).
// 0x80019E88: one car's summary (`car` = its 0xA4 bytes).
ArcGarageEntry GarageEntryOf(std::span<const uint8_t> car);
// The summary of the block's `count` cars (at most 100).
std::vector<ArcGarageEntry> GarageSummary(std::span<const uint8_t> garage);
// The block's car count (+0 s16; 0x8001DF88 disables the class row while it is 0).
int GarageCount(std::span<const uint8_t> garage);
// 0x8001F50C: the row callback of a garage list widget (HOME / GUEST GARAGE 0x8001F694 / 0x8001F6F4, the 2PLAYER BATTLE page's
// four lists 0x80020580 .. 0x800206A0): command 8 = the Rally filter (0x800F0298: only the cars owning dirt tyres), command 4 =
// 0x8001F21C (the car's name, paint chip, "%dhp" and class letter over the row banner) with the view's text context.
struct GarageRowSource {
    const ArcadeMenuAssets* a = nullptr;
    const CarInfoDirectory* cars = nullptr;
    std::span<const uint8_t> block;               // the garage block (0x4028 bytes)
    const std::vector<ArcGarageEntry>* info = nullptr; // its summary (0x800EFAB8 / 0x800EFEA8)
    const int16_t* order = nullptr;               // row -> car (0x800F1370; the battle page's 0x800F3440 + player * 200)
    bool rally = false;
    TextCtx* ctx = nullptr;
};
int32_t GarageListRow(const GarageRowSource& g, int command, const MenuListWidget& w, int row, const MenuListRowDraw* d);

class ArcadeCarPage {
public:
    ArcadeCarPage(const ArcadeMenuAssets& a, const ArcadeData& data, const CarInfoDirectory& cars);

    // 0x8001E890: `available` = the class list's flags (0x80051F30[class], rewritten for S / bonus by 0x8001D418).
    void Enter(int cls, const std::vector<uint8_t>& available, bool reenter, uint8_t language, MenuVram& vram, TextCtx& c);
    enum Result { kStay = 0, kChosen = 1, kBack = 2 };
    // 0x8001EAA8 (`pad` null during the view transitions): fills the selection (car ids, colour, transmission, tyres).
    Result Update(const MenuListPad* pad, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c);
    // 0x8001F124 (without the 3D car): the carousel page and the rule into slot 2, the bars into slots 0 / 1.
    void Draw(ViewOt& ot, TextCtx& c) const;
    // The menu model's tick (0x80016624, after the views' update every field): its data (+0xF) is ready at the first tick after
    // a car change; the model files (+0xE, loaded from the CD in the original a few fields later) are ready at once here.
    void ModelTick();

    // The 3D car (0x80015F24: drawn while view + 0x24 == 0 and the model is loaded, model + 0xE) with the camera object
    // 0x800F04E0 (0x8001E520: position (0, 0, 0x94CCC), pitch 0x5E, yaw 0x1500, window (-128, 128) x (90, -50) at H 400 for a
    // 256 x 240 view, the floor disc on, semi-transparent, colour 0xA2A2A2; 0x8001E5BC: yaw += 16 every update while the model
    // is loaded) in the car environment view + 0xBC, drawing area / offset (48, 180) 256 x 200 (0x8008025C in 0x8001F124).
    bool CarShown() const { return carHidden_ == 0 && modelLoaded_; }
    const menu::OverlayModelCamera& Camera() const { return camera_; }
    // The camera's projection in frame coordinates (the environment's offset and drawing area): the car's (`withYaw`) or
    // the floor's; and the floor disc (0x80016B60 = Simulation 0x8006C31C with the disc of 0x80016AB8, 4 m).
    menu::MenuCarProjection Projection(bool withYaw) const;
    std::vector<MenuPrim> Floor() const;
    uint32_t CarId() const { return modelCar_; }
    int Paint() const { return modelPaint_; }
    int Index() const { return carousel_.index; }
    uint32_t Uploads() const { return logo_.uploads; } // VRAM uploads so far (the caller re-uploads its copy when this changes)

    // HOME / GUEST GARAGE (view 0x8005228C: init 0x8001F8CC, update 0x8001FA8C, draw 0x8002007C): the garage's cars in the EXE
    // list widget (0x8004FBD4, rows 0x8001F694 / 0x8001F6F4 -> 0x8001F50C -> 0x8001F21C: the car's name, its paint chip, power
    // "%dhp" and class letter), the chosen car on the carousel page 0x8001F754 (the widgets with the garage figures, no power
    // curve, no bars; the name logo of the VOL), then the TRANSMISSION bar (no tyre choice). `rally` = GAME SELECTION's Rally
    // (0x800F0298): only the cars owning dirt tyres can be chosen.
    void EnterGarage(int garage, bool reenter, std::span<const uint8_t> garageBlock, bool rally, uint8_t language, MenuVram& vram);
    Result UpdateGarage(const MenuListPad* pad, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c);
    void DrawGarage(ViewOt& ot, TextCtx& c) const;

private:
    const ArcadeClassCars& Class() const;
    ArcCarFigures FiguresOf(int index) const;
    void SetModel(int index);                 // 0x80016530
    void PageDraw(const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const; // 0x8001E5F8
    void SetGarageModel(int index);                                                  // 0x8001634C
    void GaragePageDraw(const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const; // 0x8001F754
    int32_t GarageRow(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw) const; // 0x8001F694 -> 0x8001F50C

    const ArcadeMenuAssets& a_;
    const ArcadeData& data_;
    const CarInfoDirectory& cars_;
    int cls_ = 1;
    uint8_t language_ = 1;
    std::vector<uint8_t> available_;
    ArcCarousel carousel_;
    ArcGrowLine rule_;
    ArcButtonBar transmission_, settings_;
    ArcCarSpecs specs_;
    ArcNameLogo logo_;
    // the view's state words (view + 0x14 ..)
    int openTimer_ = 0, stage_ = 0, fillPending_ = 0, carHidden_ = 0, reopenTimer_ = 0;
    // the menu model (view + 0x228): the car, its paint (+0xD), data ready (+0xF), loaded (+0xE)
    menu::OverlayModelCamera camera_;
    uint32_t modelCar_ = 0;
    int modelPaint_ = 0, modelPaints_ = 1;
    bool modelData_ = false, modelLoaded_ = false;
    // the garage view
    int garage_ = -1;
    bool rally_ = false;
    std::vector<uint8_t> garageBlock_;           // career + 0x3C74 + g * 0x4028 (0x4028 bytes)
    std::vector<ArcGarageEntry> garageInfo_;
    MenuListWidget list_;
    int listTimer_ = 0;                           // view + 0x24 in the garage view
    std::array<int16_t, 100> order_{};            // 0x800F1370: row -> car (identity)
    int garageCar_ = -1;                          // selection + 8
    std::array<int16_t, 5> garageFigures_{};      // 0x80016A7C: power, 0, torque, 0, weight
    int garageDrive_ = 0;                         // model + 0x29E
    mutable TextCtx* rowCtx_ = nullptr;           // the text context of the list's row callback (the view's, 0x8001F50C)
};

} // namespace gt2::arcade
