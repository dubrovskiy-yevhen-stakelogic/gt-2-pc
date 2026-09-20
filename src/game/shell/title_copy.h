#pragma once
// The replay theater's COPY REPLAY (GT2.OVL member 1 of US Simulation v1.2, SCUS_944.88, EXE SHA-1
// 3030aa271c0a4022fc69ce09d76a6bc75e69a32a): theater row 2 -> view 0x8004B470 "COPY REPLAY" (colour 0x2878F2, title
// 0x801B99DA; init 0x80012AA4, update 0x80012B00 (the screen's exit: sound 4, back to the theater), draw 0x80012B60) over
// member 1's own copy screen: setup 0x80015CF8 (object 0x8004B1BC: the fonts 0x801B95D0 / 0x801B95C0 / 0x801B9620, page
// 0x1E, the card status words 0x800A8D64), init 0x80015D98 (0x80015B6C + state 0), per field 0x80015DC0, draw 0x80015F4C,
// 21 states in the table 0x8004B7D8 (0x80014024 .. 0x80015A30). It copies chosen replays of the replay file on memory card 1
// into the replay file on memory card 2 (created there with "Select No. of Blocks on Memory Card 2" when missing).
//
// Object (0x800A8DD8 -> its block): +0x20 the error text, +0x24 / +0x28 the two lines, +0x30 the "Copy To Memory Card 2"
// band (0x8004B614), +0x4C the rule (0x8004B630: the two lines at y 0x6E / 0xC6), bars +0x60 error (0x8004B644), +0xF8 no
// data (0x8004B674), +0x190 "Copy Replay" Start / Exit (0x8004B6A4), +0x228 format (0x8004B6D4), +0x2C0 "Copy Complete" OK
// (0x8004B758); the block selector 0x8004B704; the list 0x8004B724 (rows 0x80013AA4); the progress object +0x35C
// (0x8004B788: (0x60, 0x14A)); +0x394 "card 1 still usable"; +0x398 the rows {kind, enabled, why}; +0x48C the replay file
// of card 2 (header + directory), +0x1A1C card 1's; +0x2FA8 the chosen flags; +0x2FD0 the copied payloads; +0x22FD0.. the
// copy list (index, count, bytes, entries, offsets, sizes, remaining, descriptions).
// Evidence: our disassembly / Ghidra pseudo-C of work/re/theater (member 1 loaded), gt2run sessions and gt2play captures of
// the original with two cards (work/play/copy); docs/formats/replay.md section 9.7.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "game/shell/title_replay.h"
#include "game/shell/title_screens.h"

namespace gt2::shell {

class CopyReplayScreen {
public:
    static constexpr uint32_t kViewColour = 0x2878F2u, kViewTitle = 0x801B99DAu; // view 0x8004B470
    static constexpr int kExit = 0x16;

    CopyReplayScreen(const TitleAssets& assets, const ReplayRowText& text, std::array<CardSlot, 2> slots);
    CopyReplayScreen(const CopyReplayScreen&) = delete;
    CopyReplayScreen& operator=(const CopyReplayScreen&) = delete;

    // 0x80015DC0: one field; false once the screen has been left (state 0x16; the view then plays sound 4 and goes back to
    // the theater).
    bool Update(const MenuListPad* pad);
    std::vector<MenuPrim> Frame() const; // the view header + 0x80015F4C

    int StateId() const { return state_; }
    int Copied() const { return copied_; } // replays written to card 2 by this screen
    std::vector<int> sounds;
    std::vector<std::string> log;
    int sectorsPerField = 8; // our card rate (CardManager)
    // The US Arcade v1.1 member 1 (SCUS_944.55, member 1 SHA-1 20bb63ff...) prints the list state's message line (0x8001503C..,
    // Simulation 0x800150C4..) without 0x8006AC68: the text context on its stack (sp + 24 = 0x801FFAD0 in gt2run session
    // work/play/arc_theater/sess2) is never initialised, and it passes the medium font (0x800A8AE8 = Simulation 0x800A8DF0)
    // where the Simulation code passes the small one (0x800A8DE0). The context's page word is what the list widget's
    // highlight record (EXE 0x8006D6C8.., Simulation 0x8006D7B8..: {x, y, w, h, 0x0F0F0F, 0x363636}) left at that address
    // in the same draw (watched write pc 0x8006D700, ra 0x80016168): CLUT base 0x3636, page 0x36 - the glyphs sample the
    // wrong VRAM. true = draw it so (docs/formats/title.md section 12).
    bool arcadeMessageContext = false;

private:
    using Bar = CardManager::Bar;
    enum BarIndex { kError = 0, kNoData = 1, kStart = 2, kFormat = 3, kComplete = 4 };
    struct Rule { uint32_t c0 = 0, c1 = 0; int16_t x = 0, y = 0, w = 0, h = 0, steps = 1, anim = -1; };
    struct Row { int16_t kind = 0, enabled = 0, why = 0; }; // + 0x398 + i * 6
    struct Pending { int entry = 0; uint32_t offset = 0, size = 0, rounded = 0, after = 0; ReplayCardEntry desc; };

    int Enter(int id);                 // h(0, 0)
    int Step();                        // h(1, 0)
    int Switch(int id);                // 0x80015B08 + the chaining of 0x80015DC0
    void Poll();                       // the card slots' status words (0x800A8D64) and the replay files' presence
    bool CardsReady(bool strict) const; // 0x80013E14
    bool Lines(uint32_t& line1, uint32_t& line2) const; // 0x80013EA0(.., 1): true = no problem
    void ShowHeader(bool show);        // + 0x48 / + 0x5E anim = 0 (show) or ~steps (hide when shown)
    int Fail(uint32_t text);           // + 0x20 = text, state 1
    void ProgressReset(uint32_t total, uint32_t colour);
    void ProgressUpdate(uint32_t remaining);
    bool Transfer();                   // one field of our transfer; true when it is done
    void BuildRows();                  // 0x80014D60 h(0)
    int ChosenCount() const;           // 0x80013518
    int ChosenSectors() const;         // 0x800134C4
    int FreeAfterCopy() const;         // 0x8001358C
    int RowFit(int i, int& why) const; // 0x800135E0
    int32_t RowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw); // 0x80013AA4
    void DrawHints(MenuOtSlot& base, bool writing) const;                                          // 0x80013728
    struct Piece { size_t offset = 0; const uint8_t* source = nullptr; size_t length = 0; }; // bytes at a file offset
    bool WriteCard(const std::vector<Piece>& pieces, bool create); // our card I/O on card 2's replay file (create: 0x800696EC first)

    const TitleAssets& assets_;
    const ReplayRowText& text_;
    std::array<CardSlot, 2> slots_;
    std::array<int, 2> status_{2, 2};  // 0x800A8D64 +0 / +2
    std::array<bool, 2> hasFile_{};    // 0x8007D3E8(slot, "BASCUS-94455REPLAY")
    std::array<int, 2> freeBlocks_{};  // 0x801F097E + slot * 0x264
    int state_ = 0, delay_ = 0, listDelay_ = 0;
    uint32_t line1_ = 0, line2_ = 0, error_ = 0;
    Band band_;
    Rule rule_;
    std::array<Bar, 5> bars_;
    std::array<int, 5> barResult_{};
    CardManager::BlockSelector blocks_;
    int blockResult_ = -2;
    MenuListWidget list_;
    int32_t listResult_ = -2;
    std::vector<Row> rows_;
    int16_t keptSelection_ = 0;        // + 0x48A
    bool usable_ = false;              // + 0x394
    std::optional<ReplayCardFile> file1_, file2_;
    int16_t barTotal_ = 0, barUsed_ = 0; // + 0x1A18 / + 0x1A1A (0x800136EC)
    std::array<bool, 32> chosen_{};    // + 0x2FA8
    std::vector<Pending> copy_;        // + 0x22FD0.. (0x80015158)
    std::vector<uint8_t> buffer_;      // + 0x2FD0: the entries' sectors as read from card 1
    int copyIndex_ = 0;                // + 0x22FD0 (reading) / + 0x246D2 (writing)
    uint32_t copyBytes_ = 0;           // + 0x22FD4
    int transferLeft_ = 0;
    uint32_t progressTotal_ = 0, progressColour_ = 0, progressBase_ = 0;
    int16_t progressVisible_ = -1;     // + 0x390
    std::array<int8_t, 32> progress_{};
    int copied_ = 0;
    mutable MenuOtSlot* rowSlot_ = nullptr;
};

} // namespace gt2::shell
