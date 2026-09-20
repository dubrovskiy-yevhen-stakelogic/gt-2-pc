#pragma once
// The title's DATA TRANSFER (GT2.OVL member 1 of US Simulation v1.2, SCUS_944.88, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): title jump table slot 5 -> view 0x8004C4A0 (20 fields, CD track 7) -> 0x8004C548
// "DATA TRANSFER" (colour 0xD6142C; list 0x8004C3B4 of three panels, rows 0x8001DC38 = the title row draw with the sprites
// 0x8004B134..: TRADE, MIX RECORDS, CONVERT) -> the views of 0x8004C3A4:
//   TRADE        0x8004C59C (0xD68C9A): member 1's card manager (object 0x800B1588, states 0x8004C878) in mode 0 loads another
//                GT2 save ("BASCUS-94455GAME" of the chosen slot; CRC 0x8006A314), then 0x8004C5F0 lists its garage cars:
//                "Purchase Cars from Saved Game Files" - a car costs its value (+0x90), 0x8001EAD8 / 0x8001EB60
//   MIX RECORDS  0x8004C644 (0x90B4D6): mode 1, the same load; 0x8001E014 / 0x8001E284 / 0x8001E38C / 0x8001E408 / 0x8001E484
//                merge the loaded save's course, licence and machine-test records into the career ("Combining Records
//                Complete")
//   CONVERT      0x8004C698 (0x7878D6): mode 2 reads a Gran Turismo (GT1) save "BASCUS-94194GT" (0xA000 bytes, the check
//                0x8001FC7C), 0x8001E61C marks the GT2 B / A licence tests as passed when every GT1 B / A test was passed
//                ("Converting Data Complete")
// Evidence: our disassembly / Ghidra pseudo-C of work/re/title and work/re/theater (member 1 loaded); docs/formats/title.md
// section 10. This file: the rules (what a transfer changes in the career), checked against the original by the gt2verify
// rows of tools/gt2verify/verify_title_transfer.cpp.
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/title_screens.h"
#include "gt2formats/gtmode_tables.h"

namespace gt2::shell {

// 0x8001E014: the first 128 course records: the loaded save's record replaces ours when ours is empty (time[0] == -1) or
// slower (unsigned), the whole 0x24 bytes.
void MixCourseRecords(career::CareerState& ours, const career::CareerState& other);
// 0x8001E11C(time, name, record): the slot the loaded time would take (-1 = none): the first empty or slower slot (unsigned);
// an equal slot whose 20 bytes and name are the same makes it a duplicate (-1).
int32_t MixLicenceRank(const career::TimeRecord& time, const char* name, const career::LicenceTestRecord& record);
// 0x8001E284: every licence test's five best times of the loaded save go into ours with 0x8005DEFC (StoreLicenceRecord)
// when 0x8001E11C ranks them (the prize bytes are not touched).
void MixLicenceRecords(career::CareerState& ours, const career::CareerState& other);
// EXE 0x8005E0D0(record, entry, higher): a machine-test entry (0x14 bytes: u32 car, u32 value, ...) into the record's list
// (u8 count <= 8, entries at +4): an entry of the same car is replaced only by a better value (higher = larger is better),
// the list stays sorted, at most 8 kept. Returns the index taken, -1 when not better / not among the 8.
int32_t InsertMachineTestEntry(career::MachineTestRecord& record, const uint8_t* entry, bool higher);
// 0x8001E38C / 0x8001E408 / 0x8001E484: the loaded save's entries of the three machine tests (0-400 m, 0-1000 m: smaller is
// better; max speed: larger).
void MixMachineTests(career::CareerState& ours, const career::CareerState& other);
// 0x8001E550 on success: all of the above in the original's order.
void MixRecords(career::CareerState& ours, const career::CareerState& other);

// GT1 save ("BASCUS-94194GT", EXE ovl1 0x8004C6EC; 0xA000 bytes read): the data = file + 0x200.
constexpr const char* kGt1SaveFileName = "BASCUS-94194GT";
constexpr size_t kGt1ReadSize = 0xA000, kGt1DataOffset = 0x200, kGt1CheckedSize = 0x6BA4;
// 0x8001FBFC(data, size): a CRC-16 (0x1021, register 0x3770, the bits of each byte shifted in msb first) in the low half and a
// running sum (0xAAAA, (sum + b) ^ (b << 8)) in the high half.
uint32_t Gt1Checksum(std::span<const uint8_t> data);
// 0x8001FC7C: the checksum of the data's first 0x6BA4 bytes equals the u32 behind them.
bool Gt1DataOk(std::span<const uint8_t> data);
// 0x8001E61C(data): GT1's B licence (8 bytes at +0x2B5C all non-zero) -> every test of GT2's B licence (licences[5]) with
// +1 == 0 gets +1 = 1; only then GT1's A licence (+0x2B64) -> GT2's A (licences[4]).
void ConvertGt1Licences(career::CareerState& ours, std::span<const uint8_t> data);

// 0x8001EAD8(row): 0 = the loaded save's car `row` can be bought, 1 = our garage is full (100 cars), 2 = our money is below
// its value (+0x90).
int32_t TradeAvailability(const career::CareerState& ours, const career::GarageBlock& other, int row);
// 0x8001EB60(row): the car appended to our garage (0x8005E7F0, a full garage leaves it out) and its value paid.
void TradeBuy(career::CareerState& ours, const career::GarageBlock& other, int row);

// ---------------------------------------------------------------- screens

// Member 1's card manager (object 0x800B1588: 0x80020788 + 0x80020828(mode), per field 0x80020868, draw 0x80020A3C, states
// 0x8004C878) - the EXE card manager's bars and texts with member 1's templates (+0x50 0x8004C718 error, +0xE8 0x8004C748
// slot (Slot2 preselected), +0x180 0x8004C778 no card, +0x218 0x8004C7A8 complete, +0x348 0x8004C7D8 no cars, +0x2B0
// 0x8004C808 "Start Loading?"). States: 0 delay 25 -> 2 "Select a Slot" -> 3 "Checking Slot..." (mode 0 / 1: the GT2 save
// present 6, else 5; mode 2: the GT1 save 10 / 5; no card 4; read error 1) -> 6 / 10 "Start Loading?" -> 7 (GT2 save,
// 0x7F00 bytes, CRC) / 11 (GT1 save, 0xA000 bytes, 0x8001FC7C) -> mode 0: done (the TRADE list follows; no car: 9 "Cannot
// Find Cars in Game File"), modes 1 / 2: 8 "Combining Records Complete" / "Converting Data Complete"; 5 "No Game File
// Found" / "Cannot Find Gran Turismo Game Data", 1 = an error text; 0xD = exit.
class TransferManager {
public:
    enum Mode : int16_t { kTrade = 0, kMix = 1, kConvert = 2 };
    static constexpr int kExit = 0xD;

    TransferManager(const TitleAssets& assets, Mode mode, std::array<CardSlot, 2> slots);
    // 0x80020868: 0 running, 1 left (0xD), 2 = the data is loaded: mode 0 the manager is done (LoadedSave() holds the save),
    // modes 1 / 2 the caller applies it now (MixRecords / ConvertGt1Licences) and the manager shows "... Complete".
    int Update(const MenuListPad* pad);
    std::vector<MenuPrim> Frame() const; // the view header + 0x80020A3C

    Mode TransferMode() const { return mode_; }
    int StateId() const { return state_; }
    const std::optional<career::CareerState>& LoadedSave() const { return loaded_; }
    const std::vector<uint8_t>& Gt1Data() const { return gt1_; } // the GT1 file's data (file + 0x200)
    std::vector<int> sounds;
    std::vector<std::string> log;
    int sectorsPerField = 8;       // our card rate (CardManager)

private:
    using Bar = CardManager::Bar;
    enum BarIndex { kError = 0, kSlot = 1, kNoCard = 2, kComplete = 3, kNoCars = 4, kStart = 5 };
    int Enter(int id);
    int Step();
    void Switch(int id);
    int Status() const;
    void ProgressReset(uint32_t total);
    void ProgressUpdate(uint32_t remaining);

    const TitleAssets& assets_;
    Mode mode_;
    std::array<CardSlot, 2> slots_;
    std::array<Bar, 6> bars_;
    std::array<int, 6> barResult_{};
    Band headerBand_;
    int state_ = 0, slot_ = 0, delay_ = 0, transferLeft_ = 0;
    uint32_t line1_ = 0, line2_ = 0, error_ = 0, progressTotal_ = 0, progressColour_ = 0;
    int16_t progressVisible_ = -1;
    std::array<int8_t, 32> progress_{};
    bool idle_ = false;
    std::optional<career::CareerState> loaded_;
    std::vector<uint8_t> gt1_;
};

// The TRADE list (view 0x8004C5F0 "TRADE" 0xD68C9A: init 0x8001F084, update 0x8001F11C, draw 0x8001F358): the loaded save's
// garage in the list 0x8004C43C (9 visible rows of 0x18 + 3, rows 0x8001EE24 / 0x8001EBD4: paint chip, the .carinfoa name
// (0x80060AE8, with its 0x7F padding), the value "1,234" (0x8001E808), a 256 x 24 panel of title_item.tim), the chosen row's
// availability ("Available" 0x606060 / "Garage is Full" / "Not Enough Money" 0x142864), our money "Cr." and "%d/% 3d cars"
// over the bands 0x8004C404 / 0x8004C420; choosing an available car opens the "Buy ?" bar (0x800B14F0, template 0x8004C470,
// "No" preselected); Yes = TradeBuy.
class TradeScreen {
public:
    static constexpr uint32_t kViewColour = 0xD68C9Au, kViewTitle = 0x801B9CE0u;
    TradeScreen(const TitleAssets& assets, const CarInfoDirectory& cars, career::CareerState& career, const career::CareerState& other);
    TradeScreen(const TradeScreen&) = delete;
    TradeScreen& operator=(const TradeScreen&) = delete;
    int Update(const MenuListPad* pad); // 0 running, 2 left
    std::vector<MenuPrim> Frame() const;
    std::vector<int> sounds;
    int bought = 0;                     // cars bought on this screen

private:
    int32_t RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw);
    void DrawRow(MenuOtSlot& ot, int row, int x, int y, int alpha, int level) const; // 0x8001EBD4
    const TitleAssets& assets_;
    const CarInfoDirectory& cars_;
    career::CareerState& career_;
    const career::CareerState& other_;
    MenuListWidget list_;
    Band money_, count_;
    CardManager::Bar bar_;
    int state_ = 0, delay_ = 12;        // view + 0x14 / + 0x18
    mutable MenuOtSlot* rowSlot_ = nullptr;
};

} // namespace gt2::shell
