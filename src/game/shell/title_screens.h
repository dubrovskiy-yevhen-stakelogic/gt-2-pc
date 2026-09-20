#pragma once
// The sub-screens of the title overlay (GT2.OVL member 1) ported to native code: OPTIONS (view 0x8004C07C: page
// switcher 0x8001D4B0.., page callback 0x800186E4, row callback 0x80018574 / 0x8001805C, the EXE list widget) and the
// memory-card manager of the executable (0x80072F9C save / 0x80073010 load, per-field 0x800728F0, state handlers of the
// table 0x800921D4, button bars 0x8006E1CC.., progress bar 0x8006C04C..) on .mcd card images. Facts of US Simulation
// v1.2 (SCUS_944.88, EXE SHA-1 3030aa271c0a4022fc69ce09d76a6bc75e69a32a); evidence: docs/research/menus_gtmode.md
// section 10 (our disassembly / Ghidra pseudo-C of work/re/title, interpreter captures of every screen).
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "game/career/career_state.h"
#include "game/shell/analog_config.h"
#include "game/shell/key_config.h"
#include "game/shell/title_replay.h"
#include "game/shell/title_options.h"
#include "gt2formats/gt_menu_list.h"
#include "gt2formats/title_assets.h"

namespace gt2::shell {

// ---------------------------------------------------------------- shared pieces

// 0x8006BEF4 / 0x8006BE64: a gradient band (0x1C bytes: s16 w, h, steps, flags; u32 c0, c1, target, targetOut; s16 anim).
struct Band {
    int16_t w = 0, h = 0, steps = 0, flags = 0;
    uint32_t c0 = 0, c1 = 0, target = 0, targetOut = 0;
    int16_t anim = -1;
    static Band Read(const GuestImage& image, uint32_t address);
    void Tick();                                   // 0x8006BE64
    void Draw(MenuOtSlot& ot, int x, int y) const; // 0x8006BEF4
};

// 0x8006B6E4: POLY_G4 0x3A (x, y, w, h), c0 on the left (v0 / v2), c1 on the right.
void AddGradientQuad(MenuOtSlot& ot, int x, int y, int w, int h, uint32_t c0, uint32_t c1);

// ---------------------------------------------------------------- options

class OptionsScreen {
public:
    static constexpr uint32_t kSwitcher = 0x8004BFB8u, kPageTitles = 0x8004BFA4u;
    static constexpr uint32_t kGlobalWidget = 0x8004BDDCu, kRaceWidget = 0x8004BF14u, kGlobalArg = 0x8004BE10u, kRaceArg = 0x8004BF48u;
    static constexpr uint32_t kBands[5] = {0x8004BE34u, 0x8004BE50u, 0x8004BE6Cu, 0x8004BF6Cu, 0x8004BF88u};
    static constexpr uint32_t kColours = 0x8004BCCCu; // base, label, label selected, value, value current, page, bar off, bar on, section
    static constexpr int kPages = 5;
    static constexpr uint32_t kViewColour = 0x6ED23Cu; // view 0x8004C07C +0x0C (title +0x10 = "OPTIONS" 0x801B9AB6)
    static constexpr uint32_t kViewTitle = 0x801B9AB6u;

    // `pc` (optional): our settings the US game does not have (speed units, the graphics settings), shown as a sixth
    // page "PC SETTINGS" after the original's five (the original pages stay as they are). Up / down choose a row while
    // editing the page, left / right step its value.
    OptionsScreen(const TitleAssets& assets, career::CareerState& state, PcSettings* pc = nullptr);
    OptionsScreen(const OptionsScreen&) = delete;
    OptionsScreen& operator=(const OptionsScreen&) = delete;

    // 0x80018EFC: page switcher reset + start, both lists reset / opened (no selection), bands.
    void Start();
    // 0x80019038: one field (pad 1, pad 2). Returns false once the screen has been left (triangle / square while not
    // editing a page: the switcher fades out over 16 fields first).
    bool Update(const MenuListPad* pad, const MenuListPad* pad2);
    // 0x8001191C (header) + 0x8001D7C0 (the switcher and its pages).
    std::vector<MenuPrim> Frame() const;

    std::vector<int> sounds;       // 0x80060840 requests of the last update
    std::vector<std::string> notes; // pages that could not be entered (the analog settings without a neGcon-type controller)
    int Page() const { return page_; }
    bool Editing() const { return editing_ != 0; }
    // The controller types of ports 1 / 2 (platform/input/ps1_pad.h PadType; the title's pad objects + 2): the KEY
    // CONFIGURATION page edits the key table of that type (key_config.h).
    void SetPadTypes(uint8_t port1, uint8_t port2) { padTypes_ = {port1, port2}; }
    // The analogue bytes of a port's controller as received (buffer + 4..7): the ANALOG pages' live values.
    void SetAnalogRaw(int port, const std::array<uint8_t, 4>& bytes) { analogRaw_[size_t(port & 1)] = bytes; }

private:
    int32_t PageCallback(int command, int page, const MenuListPad* pad, MenuOtSlot* ot, int x, int y, int alpha);
    int32_t RowCallback(int list, int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw);
    void DrawRow(MenuOtSlot& ot, const OptionRow& row, int x, int y, int alpha, bool selected) const; // 0x8001805C
    void DrawPage(MenuOtSlot& ot, int page, int x, int y, int alpha) const;
    int SwitcherAlpha() const;     // 0x8001D754

    const TitleAssets& assets_;
    career::CareerState& state_;
    std::array<std::vector<OptionRow>, 2> rows_;
    std::array<MenuListWidget, 2> lists_;
    std::array<int32_t, 2> listAlpha_{};
    std::array<std::array<int16_t, 8>, 2> rowOffsets_{};
    mutable std::array<Band, 5> bands_;
    std::array<uint32_t, 5> pageTitles_{};
    std::array<uint32_t, 9> colours_{};
    // The page switcher object (0x8004BFB8).
    int16_t x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    int8_t count_ = 0, page_ = 0, previous_ = -1;
    int16_t anim_ = -1, slide_ = 0;
    uint8_t arrows_ = 0, editing_ = 0, skip_ = 0;
    int leaving_ = 0;
    PcSettings* pc_ = nullptr;
    int8_t pcRow_ = -1;            // page 5: the selected row while editing
    KeyConfigData keyData_;        // page 2 (key_config.h)
    KeyConfigPage keyPage_;
    std::array<uint8_t, 2> padTypes_{4, 0};
    AnalogConfigData analogData_;  // pages 3 / 4 (analog_config.h)
    std::array<AnalogPageObject, 2> analogPages_{};
    AnalogGlobals analogGlobals_;
    std::array<std::array<uint8_t, 4>, 2> analogRaw_{};
    uint8_t* PadBlock(int port) { return reinterpret_cast<uint8_t*>(&state_) + (port == 0 ? 0x0A : 0x5C); }
};

// ---------------------------------------------------------------- memory cards

// One slot of our memory-card manager: a .mcd image on disk (128 KB, the PS1 card layout; the career's save file
// "BASCUS-94455GAME" as the original writes it). No path = no card inserted.
struct CardSlot {
    std::string path;
    bool Present() const { return !path.empty(); }
};

// The card status word the manager reads (0x800A8D64, from the slot state by 0x8006EB64): 0 ready, 1 busy, 2 no card,
// 3 not formatted, 4 error.
int CardStatus(const CardSlot& slot, std::vector<uint8_t>* image = nullptr);
// 0x800826C8: free blocks (15 minus the blocks of the files, chains starting at a 0x51 entry).
int CardFreeBlocks(const std::vector<uint8_t>& image);
// 0x80082814: the directory entry of `name` (state 0x51), -1 when absent.
int CardFindFile(const std::vector<uint8_t>& image, const std::string& name);

// The executable's keyboard widget (0x80073548 init, 0x8007364C open, 0x800736E0 close, 0x80073720 update, 0x80073AFC
// draw) as the card manager holds it at mgr + 0x6D8 (descriptor EXE 0x800921A0: the medium font, 16 x 38 cells, centre
// (0xB0, 0xB0); +0x1E = 31 characters, +0x20 = 256 pixels, the name buffer mgr + 0x752). The widget itself is
// gt2view/race_record_screens.h NameEntry (gt2screens); MakeCardKeyboard there builds this adapter.
class CardKeyboard {
public:
    virtual ~CardKeyboard() = default;
    // 0x8007364C on the name `name`, then the caret at its end (the state's caller: +0x6F4 = strlen).
    virtual void Open(const std::string& name) = 0;
    virtual void Close() = 0;                                                         // 0x800736E0
    // 0x80073720: -2 nothing, -3 a move, -1 CANCEL chosen, 0 OK chosen; the sounds it plays (0x80060840) into `sounds`.
    virtual int Update(const MenuListPad* pad, std::vector<int>& sounds) = 0;
    virtual void Draw(MenuOtSlot& ot) const = 0;                                      // 0x80073AFC
    virtual std::string Name() const = 0;
};

class CardManager {
public:
    // Modes of 0x801C94AC +0: 0 = SAVE REPLAY / SAVE GHOST (0x80072E7C / 0x80072EB4: the race overlay's menus; the payload and
    // its description are the caller's, SetSaveData), 1 = LOAD REPLAY (0x80072EEC, the replay theater's "Load Replay"),
    // 2 = RENAME & DELETE (0x80072F20, the theater's "Rename & Delete"), 3 = LOAD GHOST (0x80072F54: the ghost entries of the
    // race's course, SetGhostCourse; 0x80072F8C = Loaded()), 4 = SAVE GAME, 5 = LOAD GAME.
    enum Mode : uint16_t { kSaveReplay = 0, kLoadReplay = 1, kRenameReplay = 2, kLoadGhost = 3, kSaveGame = 4, kLoadGame = 5 };
    static constexpr int kExit = 0x27;
    static constexpr int kSaveBlocks = 4; // 0x8006A000: (0x7EA0 + 0x1FFF) >> 13

    // 0x8007284C + 0x80072F9C / 0x80073010. The save image is built when the screen opens (0x80072F9C: 0x8006A038 header,
    // 0x8006A214 block + CRC-32).
    CardManager(const TitleAssets& assets, Mode mode, career::CareerState& state, std::array<CardSlot, 2> slots);

    // 0x800728F0: one field; false once the manager has exited (0x27). After a successful load `state` holds the loaded
    // block (0x8006A278; 0x8006A348 only copies the pad configurations out).
    bool Update(const MenuListPad* pad);
    std::vector<MenuPrim> Frame() const; // header 0x8001191C + 0x80072B78
    // 0x80072B78 alone into the three OT slots it uses (base = the view's slot, then + 1, + 2): the race overlay's card views
    // (the race view manager draws their header).
    void Draw(MenuOtSlot& base, MenuOtSlot& fills, MenuOtSlot& gradients) const;
    // Mode 0: the packed payload (0x80069948 / 0x80069CC0 into mgr + 0x26A4, its size mgr + 0xA6A4) and the entry description
    // (0x800724F8 / 0x80072598: mgr + 0x1EC4, "No Name" until the name entry).
    void SetSaveData(std::vector<uint8_t> payload, const std::array<uint8_t, 0x50>& desc) {
        savePayload_ = std::move(payload);
        saveDesc_ = desc;
    }
    // Mode 3: the course file id the ghost entries must match (0x80070C14: entry + 0x42 == 1 and + 0x44 == 0x801D589C).
    void SetGhostCourse(uint32_t courseId) { ghostCourse_ = courseId; }
    // Mode 3 after "Loading Complete": the chosen entry's payload (the bytes 0x80069D58 unpacks).
    const std::vector<uint8_t>& LoadedData() const { return loadedData_; }
    int SavedEntry() const { return savedEntry_; }

    int StateId() const { return state_; }
    Mode CardMode() const { return mode_; }
    bool Loaded() const { return loaded_; }
    bool Saved() const { return saved_; }
    // Mode 1: the replay file's rows use the row printer 0x8006A4E4 (course names from `text`); after the manager exited
    // with 0x28 ("loaded") LoadedReplay() holds the unpacked payload (0x80069AC4) of the chosen entry.
    void SetReplayText(const ReplayRowText* text) { replayText_ = text; }
    // Mode 2 (and the name entry of the save modes): the keyboard at mgr + 0x6D8.
    void SetKeyboard(std::unique_ptr<CardKeyboard> keyboard) { keyboard_ = std::move(keyboard); }
    // Mode 2: the replay file as the manager holds it (the card's file after a completed "Save Changes").
    const std::optional<ReplayCardFile>& ReplayFile() const { return replayFile_; }
    const std::optional<ReplayPayload>& LoadedReplay() const { return loadedReplay_; }
    std::string LoadedReplayTitle() const { return loadedTitle_; }
    std::vector<int> sounds;
    std::vector<std::string> log;
    // Sectors per field of our card transfer (the original's rate is the card hardware's; ours is instantaneous I/O
    // shown over a short progress run): 254 sectors of 128 bytes.
    int sectorsPerField = 8;

    // The EXE's button bars (also used by member 1's own card manager of DATA TRANSFER, title_transfer.h).
    struct Bar {
        // template (0x80091F04..): x, y, title / label strings, colours, flags; state (0x98 bytes at mgr + 0x50 + k * 0x98)
        int16_t x = 176, y = 380, w = 96, h = 24;
        uint32_t title = 0, label0 = 0, label1 = 0;
        uint32_t titleColour = 0, labelColour = 0, fill = 0, gradient = 0;
        uint16_t flags = 0;
        int8_t labelExtra = 1;   // template +0x22: the labels' extra letter spacing
        int8_t cursor = 0;
        int16_t anim = -1, slide = 0;
        bool open = false;
    };
    static Bar ReadBar(const GuestImage& image, uint32_t address);
    static void OpenBar(Bar& b);                                                        // 0x8006E388
    static void CloseBar(Bar& b);                                                       // 0x8006E3FC
    static int UpdateBar(Bar& b, const MenuListPad* pad, std::vector<int>& sounds);    // 0x8006E43C
    static void DrawBar(const TitleAssets& assets, MenuOtSlot& gradients, MenuOtSlot& fills, MenuOtSlot& base, const Bar& b); // 0x8006E5B8
    // The "Memory Card N" band (mgr + 0x34, 0x8006BEF4) and its text; the progress bar (0x8006C174: 32 segments at (96 + 5 i,
    // 280) 4 x 24); the error text of state 1 (US: centred at (0xB0, 0xFA), medium font, colour 0x6F6F6F).
    static void DrawSlotBand(const TitleAssets& assets, MenuOtSlot& base, const Band& band, int slot);
    // `y`: the progress object's +2 (EXE manager 0x118; member 1's Copy Replay object 0x8004B788: 0x14A).
    static void DrawProgress(MenuOtSlot& base, const std::array<int8_t, 32>& segments, uint32_t colour, int y = 0x118);
    static void DrawErrorText(const TitleAssets& assets, MenuOtSlot& base, uint32_t text);
    // The sector bar 0x8006AA68(sectors {total, used}, ot, x, y, fade, less, more): the used sectors minus `less` in the
    // "used" gradient, then up to used - less + more in the "selected" gradient, the black TILE and the frame; the colours fade
    // toward 0x80091E7C by 0x80 - fade.
    static void DrawSectorBar(const TitleAssets& assets, MenuOtSlot& base, int total, int used, int x, int y, int fade, int less, int more);

    // The block count selector (0x8006D9C8 reset, 0x8006D9DC update, 0x8006DAF8 draw; EXE template 0x80091FC4, member 1's Copy
    // Replay 0x8004B704): +0 x, +2 y, +4 value, +6 min, +8 max, +0xC colour, +0x10 font, +0x14 s16 height, +0x16 spacing, +0x18
    // digit shift, +0x1A sound, +0x1C s16 anim (-1 hidden, 0..0x2D the arrows' blink).
    struct BlockSelector {
        int16_t x = 0xB0, y = 0x118, value = 3, min = 3, max = 15;
        uint32_t colour = 0;
        int fontIndex = TitleAssets::kMediumFont; // +0x10 as one of the title fonts
        int16_t height = 0x18, spacing = 2, digitShift = -4, sound = 6, anim = -1;
    };
    static BlockSelector ReadBlockSelector(const GuestImage& image, uint32_t address, int fontIndex);
    static int UpdateBlockSelector(BlockSelector& b, const MenuListPad* pad, std::vector<int>& sounds); // 0x8006D9DC
    static void DrawBlockSelector(const TitleAssets& assets, MenuOtSlot& base, const BlockSelector& b); // 0x8006DAF8

private:

    int Enter(int id);   // h(0, 0)
    int Step();          // h(1, 0) of the current state
    void Switch(int id); // 0x80072494 with the chaining of 0x80072A88
    int Status() const;  // the selected slot's status
    void ProgressReset(uint32_t total);
    void ProgressUpdate(uint32_t remaining); // 0x8006C074

    const TitleAssets& assets_;
    Mode mode_;
    career::CareerState& career_;
    std::array<CardSlot, 2> slots_;
    career::CareerSave image_;     // the file image built at open (mode 4: 0x8006A038 header + 0x8006A214 block)
    std::array<int, 11> barResult_{};
    int state_ = 0, slot_ = 0, delay_ = 0;
    uint32_t line1_ = 0, line2_ = 0, error_ = 0;
    int16_t headerAnim_ = -1;      // mgr + 0x34 band anim (the "Memory Card N" header)
    Band headerBand_;
    std::array<Bar, 11> bars_;     // mgr + 0x50 + k * 0x98 (BarIndex in title_screens.cpp)
    int transferLeft_ = 0;         // sectors of the running write / read
    uint32_t progressTotal_ = 0, progressColour_ = 0;
    int16_t progressVisible_ = -1;
    std::array<int8_t, 32> progress_{};
    bool loaded_ = false, saved_ = false, exited_ = false;
    int leaving_ = 0;
    // Mode 1 (replays): the file read by state 7, the list 0x80091FE4 (row callback 0x8006F060) and its rows
    // (mgr + 0x890 + i * 4: {entry, kind, enabled, playable}), the chosen row (mgr + 0x930), the loaded payload.
    struct ReplayRow { int8_t entry = 0, kind = 0; bool enabled = true, playable = true; };
    int32_t ReplayRowCallback(int command, const MenuListWidget& w, int row, const MenuListRowDraw* draw);
    const ReplayRowText* replayText_ = nullptr;
    std::optional<ReplayCardFile> replayFile_;
    MenuListWidget replayList_;
    std::vector<ReplayRow> replayRows_;
    ReplayRowStyle replayStyle_;
    int32_t listResult_ = -2;
    int replayChosen_ = 0;
    // Mode 2: mgr + 0x12 (fields before the list opens / the keyboard opens), + 0x930 (the entry being renamed),
    // + 0x932 (the list's selection kept), + 0x20 (L1 + R1 held: delete mode), + 0x934 / + 0x936 (the file's sectors,
    // total / used, for the sector bar 0x8006AA68); the keyboard's result of the field (+0x750).
    int16_t listDelay_ = 0, renameIndex_ = -1, keptSelection_ = 0;
    bool deleteMode_ = false;
    int16_t sectorsTotal_ = 0, sectorsUsed_ = 0;
    int keyboardResult_ = -2;
    // Mode 0: the payload / description to store, the fit of each row (+0x893), the file created in memory (state 8), whether
    // the card holds the file (state 0xC creates it); the entry the save wrote.
    std::vector<uint8_t> savePayload_;
    std::array<uint8_t, 0x50> saveDesc_{};
    std::vector<int8_t> rowFit_;
    bool fileOnCard_ = true;
    int savedEntry_ = -1;
    // Mode 3
    uint32_t ghostCourse_ = 0;
    std::vector<uint8_t> loadedData_;
    // The block count selector 0x80091FC4 of state 8 ("Select Number of Blocks").
    BlockSelector blocks_;
    int blockResult_ = -2;
    int UpdateBlocks(const MenuListPad* pad); // 0x8006D9DC
    void DrawBlocks(MenuOtSlot& base) const;  // 0x8006DAF8
    std::unique_ptr<CardKeyboard> keyboard_;
    void BuildReplayRows();        // 0x8006F6E8 h(0): the rows of modes 0 / 2
    void DrawReplayHints(MenuOtSlot& base) const; // 0x8006F6E8 h(2) / 0x8006FF8C h(2)
    std::optional<ReplayPayload> loadedReplay_;
    std::string loadedTitle_;
    mutable MenuOtSlot* rowSlot_ = nullptr;
};

// A page with a view header and a line (gt2game's notices, e.g. the arcade disc's "Arcade Mode Disc" page).
std::vector<MenuPrim> NoticeFrame(const TitleAssets& assets, const std::string& title, uint32_t colour, const std::string& line);

} // namespace gt2::shell
