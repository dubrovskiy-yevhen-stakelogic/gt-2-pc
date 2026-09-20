#pragma once
// The 2 player Battle's menus of US Arcade v1.1 (GT2.OVL member 2; SCUS_944.55 SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95,
// ARCADE v1.1 addresses), docs/research/arcade_disc.md section 19:
//   - 2P GAME SELECTION (view 0x800520E8: init 0x8001DB10, update 0x8001DB74, draw 0x8001DCFC) is a panel list (0x8004F980: Road
//     Race / Rally) handled by ArcadeMenus;
//   - 2PLAYER BATTLE (view 0x800522E0: init 0x80020700, update 0x8002229C, draw 0x80022658) is this page: both players choose
//     at once, each with a state machine 0x80020D88 on its own controller (the view's pads + 0x1A4 + player * 0x10) over its own
//     copies of the single player widgets - the class list (0x8004FDDC / 0x8004FDFC), the garage lists (EXE list widgets
//     0x8004FEC0 + garage * 0x34 + player * 0x68), the car carousel (0x800F16D0 / 0x800F16FC, page callback 0x80020208 with the
//     layouts 0x8004FE58 / 0x8004FE6C), the spec widgets (0x800F18E0 / 0x800F2560), the TRANSMISSION / SETTINGS bars (0x800F31E0 /
//     0x800F3278, 0x800F3310 / 0x800F33A8), the name logo (view + 0x23C / + 0x268, texture pages 0x16 / 0x17), the menu model
//     (view + 0x228 / + 0x22C) with its camera (0x800F1728 / 0x800F1804, 0x80020170) - and the "PLAYER 1 / 2" labels, their bands
//     and the class labels. When both players are done (states 4 / 8) COURSE SELECTION follows with the 2P course lists.
// Player state (6 bytes at 0x800F3660 / 0x800F3666): +0 state (0 class list, 1 garage list, 2 garage car, 3 its transmission,
// 4 done, 5 class car, 6 its transmission, 7 its settings, 8 done, 9 left), +1 class row, +2 car index, +3 spec refresh, +4 timer.
// The choices go to the selection block's 2P fields (arcade_setup.h Sel2P) that 0x80010C84 reads in mode 0.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "game/arcade/arcade_car_page.h"
#include "game/arcade/arcade_course_page.h" // ArcBand (0x80011EA0 ..)
#include "game/arcade/arcade_menus.h"
#include "game/arcade/arcade_widgets.h"
#include "game/menu/menu_car.h"

namespace gt2::arcade {

class ArcadeBattlePage {
public:
    ArcadeBattlePage(const ArcadeMenuAssets& a, const ArcadeData& data, const CarInfoDirectory& cars);

    // What 0x80020700 reads besides its own objects.
    struct Context {
        int listKind = 3;                              // 0x800F364C: 3 road, 4 Rally (2P GAME SELECTION)
        bool classSOpen = false;                       // 0x8001D53C: the class lists with S
        std::array<std::vector<uint8_t>, 7> classFlags; // 0x80051F30[class] (lists 0 and 6 as 0x8001D418 left them)
        std::array<std::span<const uint8_t>, 2> garages; // home (career + 0x3C74) / guest (0x801D0FDC) blocks
        bool rally = false;                            // 0x800F0298
        uint8_t language = 1;
    };
    // 0x80020700 (`reenter`: back from COURSE SELECTION; `sel` = the selection block 0x801C3010).
    void Enter(const Context& ctx, bool reenter, const uint8_t* sel, MenuVram& vram);
    enum Result { kStay = 0, kChosen = 1, kBack = 2 };
    // 0x8002229C: `pads` = the players' controllers (null during the view transitions); fills the 2P selection fields.
    Result Update(const std::array<const MenuListPad*, 2>& pads, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c);
    // 0x80022658 without the 3D cars (slot 2 the widgets, slot 0 the labels; the header is the menus').
    void Draw(ViewOt& ot, TextCtx& c) const;
    // The menu models' tick (0x80016624 after the views' update): data and model ready at once (ours, as ArcadeCarPage).
    void ModelTick();

    // The two 3D cars (0x80015F24 while view + 0x18 == 0 and the model is loaded) in the environments view + 0xBC / + 0xCC with
    // the drawing areas (0x8A, 0x5E, 0xD2, 0x90) / (4, 0x12E, 0xD2, 0x90).
    bool CarShown(int player) const;
    uint32_t CarId(int player) const { return models_[size_t(player)].car; }
    int Paint(int player) const { return models_[size_t(player)].paint; }
    menu::MenuCarProjection Projection(int player, bool withYaw) const;
    std::vector<MenuPrim> Floor(int player) const;
    uint32_t Uploads() const { return logos_[0].uploads + logos_[1].uploads; }

private:
    struct PlayerState { // 0x800F3660 + player * 6
        uint8_t state = 0;
        int8_t row = 0, index = 0;
        uint8_t refresh = 0;
        int16_t timer = 0x18;
    };
    struct Model { // view + 0x228 + player * 4 (0x80016530 / 0x8001634C / 0x800165CC)
        uint32_t car = 0;
        int paint = 0, paints = 1;
        bool data = false, loaded = false;
        bool garage = false;
        int drive = 0;                    // + 0x29E
        std::array<int16_t, 5> figures{}; // 0x80016A7C: power, 0, torque, 0, weight
    };
    int Player(int p, const MenuListPad* pad, bool input, uint8_t* sel, std::vector<int>& sounds, MenuVram& vram, TextCtx& c); // 0x80020D88
    void CloseOther(int p, const uint8_t* sel);                                                                                 // 0x80022058
    void PageDraw(int p, const ArcCarousel::Item& it, MenuOtSlot& ot, TextCtx& c) const;                                       // 0x80020208
    void SetClassModel(int p, int cls, int index, int paintIndex); // 0x80016530
    void SetGarageModel(int p, int garage, int index);           // 0x8001634C
    void ClearModel(int p);                                      // 0x800165CC
    void SetLogo(int p, int cls, int index, MenuVram& vram);     // 0x80016ECC of a class car
    const ArcadeClassCars& ClassOf(int cls) const;

    const ArcadeMenuAssets& a_;
    const ArcadeData& data_;
    const CarInfoDirectory& cars_;
    Context ctx_;
    int timerOpen_ = 0x18, timerModels_ = 0x18, timerReopen_ = -1; // view + 0x14 / + 0x18 / + 0x1C
    std::array<PlayerState, 2> state_{};
    std::array<PanelList, 2> classList_;
    std::array<std::array<MenuListWidget, 2>, 2> garageList_;     // [player][garage]
    std::array<std::array<std::vector<ArcGarageEntry>, 2>, 2> garageInfo_; // the summaries (per player for the row callbacks)
    std::array<std::array<int16_t, 100>, 2> order_{};             // 0x800F3440 + player * 200
    std::array<ArcCarousel, 2> carousel_;
    std::array<std::vector<uint8_t>, 2> available_;              // the carousel's availability (0x80051F30[class])
    std::array<ArcCarSpecs, 2> specs_;
    std::array<ArcButtonBar, 2> transmission_, settings_;
    std::array<ArcNameLogo, 2> logos_;
    std::array<Model, 2> models_;
    std::array<menu::OverlayModelCamera, 2> cameras_;
    std::array<ArcText, 2> labels_, classLabels_;
    std::array<ArcBand, 2> bands_;
    ArcGrowLine line_;
    std::array<std::vector<uint8_t>, 2> garageBlocks_;
    mutable TextCtx* rowCtx_ = nullptr; // the view's text context for the list rows (0x8001F50C)
};

} // namespace gt2::arcade
