#pragma once
// The race overlay's CHANGE PARTS page, interactive (GT2.OVL member 0 of US Simulation v1.2, SCUS_944.88, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a; the US Arcade v1.1 member 0 holds the same code at -0xE0 / data -0x90):
//   view 0x8005D1C0 {init 0x800572C4, update 0x8005731C, draw 0x80057450}, the page object *0x801C90F0: init 0x80053558,
//   update 0x800536A4, draw 0x80053CA8, the part rows 0x800529C0, the stage list (EXE list widget 0x8005C3DC with the row
//   callback 0x80052D84, its band 0x8005C410), the preview queue (0x80057628 / 0x80057654 / 0x800576D8 / 0x800576FC /
//   0x80057854 / 0x80057910 / 0x80057954 / 0x80057998 / 0x80057A28: one stage's record, figures and graph curves per field)
//   and the executable's power / torque graph (power_graph.h, object 0x8005C42C).
// States: 0 the groups (up / down; cross / circle / right enter; L1 PARTS SETTING; triangle / square leave), 1 the parts of
// the group (up / down; cross / circle enter; triangle / square / left back), 2 the stage list (the list widget; a stage is
// enabled when the car's sheet has its row, the garage car owns it (stage > 0), a power restriction does not forbid its
// preview's power, and the tyre rule of the course); choosing a stage selects its row into the sheet (0x8005EAC0). Leaving
// commits the sheet (0x80056FF0, career::CommitSettings, done by the caller). Parts with the flags bit 7 (performance chip,
// NA tune-up, turbo kit, intercooler, muffler) show the current curves and, in the stage list, the selected stage's preview
// ("%dhp / %drpm", "%d.%dkgm / %drpm"). Evidence: our disassembly / Ghidra pseudo-C (work/re/change_parts/sim_decomp,
// sim_decomp2); captures of the original arcade (work/play/change_parts/cap2). docs/formats/race_screens.md section 8.
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/career/garage.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/race_menu_assets.h"
#include "gt2view/power_graph.h"
#include "gt2view/race_menus.h"
#include "gt2view/race_result_screens.h"

namespace gt2::screens {

// 0x8005EE4C(sheet, kind, stage): the row index word of the sheet's slot of `kind` (0..22) at `stage`; negative = no row
// (0x80015404 fills the sheet with 0xFF).
int32_t SheetSlotRow(const career::TuneSheet& sheet, int32_t kind, int32_t stage);

// What the page reads besides its tables: the sheet 0x8016E894 (the race car's, edited in place), the career data (the
// record builder's tables), the garage car of the race block (+ 0x582 garage, + 0x584 index: 0x8005E874 decides whether
// a stage > 0 is owned; null = no garage car: every stage > 0 disabled) and the race block's restrictions (+ 0x586 s16
// power limit, 0x801D5DE2; + 0x588 flags 0x801D5DE4: bit 0 a tarmac course (tyres: the dirt stage 7 disabled, else only it),
// bits 1..2: 1 no turbo kit / intercooler, 2 no NA tune-up).
struct ChangePartsContext {
    career::TuneSheet* sheet = nullptr;
    const career::CareerData* data = nullptr;
    const career::GarageCar* garageCar = nullptr;
    int16_t powerLimit = 0;
    uint16_t restrictions = 0;
};

// The preview queue (page + 0x38): slot 0 = the sheet as it is (0x8005F410), slot i = the part's stage i - 1 (0x8005F044
// when the sheet has its row); each: the record's figures (0x80075930: + 0 power, + 2 its rpm, + 4 torque, + 6 its rpm,
// + 0xA points, + 0xC rpm / + 0x2C power / + 0x4C torque lists) and the graph samples (0x8007489C).
struct PartsPreview {
    struct Slot {
        int16_t kind = 0, stage = 0;
        std::array<uint8_t, 0x6C> figures{};
        std::array<int16_t, kPowerGraphSamples> power{}, torque{};
        uint16_t Figure(size_t offset) const { return uint16_t(figures[offset] | figures[offset + 1] << 8); }
    };
    int8_t index = 0, count = 0;       // + 8 / + 9
    std::array<bool, 10> done{};       // + 0xA ..
    std::array<Slot, 10> slots;        // + 4 -> page + 0x4C, 0x434 bytes each
    void Reset();                                       // 0x800576D8
    void Queue(int stages, int16_t kind);               // 0x80057654 (the pairs page + 0x2A54: {kind, stage})
    bool Step(const ChangePartsContext& c);             // 0x800576FC: one slot per call; true when the last one is done
    void Maxima(int& rpm, int& power, int& torque) const; // 0x80057854
};

class ChangePartsView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005D1C0u;
    ChangePartsView(const RaceMenuAssets& a, const ChangePartsContext& c);
    ChangePartsView(const ChangePartsView&) = delete;
    ChangePartsView& operator=(const ChangePartsView&) = delete;

    // 0x8005731C: 1 when the page is left (exit says how; the caller commits with 0x80056FF0 first).
    int Update(const MenuListPad* pad, bool input = true) override;
    void Draw(MenuOt& ot) const override; // 0x80057450 -> 0x80053CA8(page, ot + 4 entries)
    std::string Title() const override;
    uint32_t Colour() const override;

    enum Exit { kStay = 0, kLeave, kPartsSetting };
    Exit exit = kStay;                 // -4 (triangle / square in states 0 / 1) / -8 (L1)
    int changes = 0;                   // stages selected into the sheet (0x80052928)

    // The page object (0x80053558(page, 0, 100)).
    struct Page {
        int16_t x = 0, y = 100;
        int8_t state = 0, group = 0, part = 0;
        int16_t groupCount = 7, scroll = 0, arrowPhase = 0, open = -1, flash = 0; // +8 / +A / +C / +E / +10
        shell::Band band;              // +14 (template 0x8005C3C0)
        uint32_t previousColour = 0, colour = 0; // +30 / +34
    };
    const Page& PageState() const { return page_; }
    int16_t viewDelay = 12;            // the view object + 0x14 (0x800572C4): 0x80053674 when it reaches 0

private:
    uint32_t GroupRecord(int group) const;    // 0x80052CB4
    int PartCount(uint32_t record) const;     // 0x80052958
    int StageCount(uint32_t record, int part) const; // 0x8005298C
    void SelectGroup(int group);              // 0x800534B4
    void SelectPart(int part);                // 0x8005350C
    int32_t PageUpdate(const MenuListPad* pad, bool input); // 0x800536A4
    int32_t ListCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* d); // 0x80052D84
    void DrawParts(MenuOt& ot, uint32_t record, int selected, int x, int y, int alpha, int flash) const; // 0x800529C0
    int ListFade() const;                     // (list + 0x24 << 7) / list + 0x14

    const RaceMenuAssets& a_;
    ChangePartsContext c_;
    Page page_;
    MenuListWidget list_;                     // 0x8005C3DC
    shell::Band listBand_;                    // 0x8005C410
    PowerGraph graph_;                        // 0x8005C42C
    PowerGraphText graphText_;
    PartsPreview preview_;
    int16_t group_ = 0, part_ = 0;            // 0x801C90C8 / 0x801C90CA (the entered group / part)
    bool graphPart_ = false;                  // 0x801C90CC: the entered part's flags bit 7
    bool input_ = true;                       // 0x801C90D0
    uint32_t groupText_ = 0, partText_ = 0;   // 0x801C90D4 / 0x801C90D8 (drawn by the JP branch; the US draws partText_ in state 1)
    mutable MenuOt* drawOt_ = nullptr;        // the list callback's OT while Draw runs
};

// PARTS SETTING as a view of the race overlay's manager (view 0x8005D1E4: init 0x8005747C, update 0x800574C0, draw
// 0x800575F8 -> 0x80056810), over the same sheet as CHANGE PARTS (the page: MachineSettingsPage). CHANGE PARTS' L1 replaces
// the manager's top with it (0x80048374, then the manager's slide 3); its R1 (-8) replaces it with CHANGE PARTS again
// (0x80048374(M, 0x8005D1C0), slide 2), triangle / square in the group selection (-4) leaves (back to the view below). Both
// commit the sheet first (0x80056FF0, done by the caller) and close the page band (0x80055FE0); sounds 3 / 4.
class PartsSettingView : public PostRaceView {
public:
    static constexpr uint32_t kView = 0x8005D1E4u;
    PartsSettingView(const RaceMenuAssets& a, const ChangePartsContext& c); // 0x8005747C: view + 0x14 = 12, 0x80055E90(P, 0, 100)
    PartsSettingView(const PartsSettingView&) = delete;
    PartsSettingView& operator=(const PartsSettingView&) = delete;

    // 0x800574C0: 1 when the page is left (exit says how; the caller commits with 0x80056FF0).
    int Update(const MenuListPad* pad, bool input = true) override;
    void Draw(MenuOt& ot) const override; // 0x800575F8 -> 0x80056810(P, ot + 4 entries)
    std::string Title() const override;
    uint32_t Colour() const override;

    enum Exit { kStay = 0, kLeave, kChangeParts };
    Exit exit = kStay;
    bool changed = false;              // a setting was written into the sheet (0x8005F9DC / 0x80060410)
    const MachineSettingsPage& Page() const { return page_; }
    int16_t viewDelay = 12;            // the view object + 0x14: 0x80055FD0 when it reaches 0

private:
    const RaceMenuAssets& a_;
    ChangePartsContext c_;
    MachineSettingsPage page_;
};

} // namespace gt2::screens
