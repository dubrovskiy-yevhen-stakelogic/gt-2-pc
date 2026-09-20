#pragma once
// Static tables of the arcade menus (GT2.OVL member 2, "ovl2", loaded at 0x80010000) of US Arcade v1.1 (SCUS_944.55,
// EXE SHA-1 231f9dba7191b9ef915621662afdc40a7c66df95; member 2 SHA-1 304ee2b35f0b3dc430394cbfbe14f670a3353cf3). The
// addresses below are ARCADE v1.1 member-2 addresses (the Simulation disc's member 2 is never loaded on that disc and is not
// the reference here). Evidence: our disassembly / Ghidra pseudo-C of the RAM dump taken at the race build of the arcade
// menus (work/re/arcade_menu, project gt2_arcade_menu) and the session runs of docs/research/arcade_disc.md section 16.
//
//   0x8002729C  char* [4 levels][4 classes]: the event names "A<level><class>" of usa_arcade_data.dat table 30, class
//               order S, A, B, C (selection +0 level, +1 class; the race build 0x80010C84 looks the row up by name)
//   0x800272DC  s8 [4]: race block + 0x57F by level (0x80010C84)
//   0x8004F8EC  s8 [4]: the game mode (selection + 2, race block + 0x0A) of the GAME SELECTION rows: Road Race 4, Rally 6,
//               Time Trial 6
//   0x8004FBD0  s8 [2]: the transmission byte (selection + 0x18, entry + 0x8F) of the TRANSMISSION bar: AT 0, MT 1
//   0x80050730 .. 0x800516B0: the course lists (0x20-byte rows, name pointer 0 ends a list; 0x8001D120 counts the
//               available rows at menu start, 0x800228BC picks the list by the list kind 0x800F364C)
//   0x800519A0  s16 [7]: cars per class list; 0x80051F14 / 0x80051F4C / 0x80051F68 / 0x80051F84 / 0x80051FA0 / 0x80051F30
//               [class]: car name pointers, model numbers, second numbers, 3-byte bars, 10-byte figures, availability
//               flags (0x8001D358 / 0x8001D2E0 / 0x8001D308 / 0x8001D330)
//   0x800519E4 / 0x80051D64: s8 per car of class lists 0 / 6: the course record whose flags unlock the car (-1 = always;
//               0x8001D418)
//   data-arcade.txd: gzip member at 0x800267D8; 0x80011954 copies block language * 0x2E0 to RAM 0x800F81E0.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gt2formats/overlay_data.h"

namespace gt2 {

class DiscImage;

// One row of a course list (0x20 bytes).
struct ArcadeCourse {
    std::string file;        // +00 the course file name ("tahiti_t")
    std::string display;     // +04 the name the menu shows ("Tahiti Road"; copied to selection + 0xB8)
    uint32_t mapInfo = 0;    // +08 pointer to the course's map picture record (member 2)
    uint32_t flags = 0;      // +0C 0x10 reverse, 0x08 2-player
    uint32_t id = 0;         // +10 the course id (0x80082F14 of the file name; 0x8001D120 recomputes it) -> selection + 0x1B8
    int32_t tier = 0;        // +14 the unlock tier (0x8002357C; -1 = always)
    int32_t record = 0;      // +18 the course record number (career + 0xB8 flag byte; -1 = none) -> selection + 0x1BC
    uint32_t available = 0;  // +1C as stored in the image (0x8001D120 rewrites it at menu start)
};

// One class car list.
struct ArcadeClassCars {
    std::vector<std::string> cars;     // car ids ("ccrcn")
    std::vector<int16_t> model;        // 0x8001D2E0: the car's number in the menu's model / logo list
    std::vector<int16_t> number2;      // 0x8001D330
    std::vector<std::array<uint8_t, 3>> bars;    // Max Speed / Handling / Acceleration bars (0x80051F84)
    std::vector<std::array<uint8_t, 10>> figures; // 0x80051FA0
    uint32_t flagsAddress = 0;         // 0x80051F30[class]: per car 1 = selectable (0x8001D418 rewrites lists 0 and 6)
};

struct ArcadeMenuData {
    static constexpr uint32_t kTextBase = 0x800F81E0u, kTextStride = 0x2E0, kTxdGzip = 0x800267D8u;
    static constexpr size_t kCourseListCount = 7; // 0 road, 1 road reverse, 2 time trial, 3 time trial reverse, 4 rally, 5 2P, 6 2P dirt
    static constexpr size_t kClassListCount = 7;  // 0 S, 1 A, 2 B, 3 C, 4 / 5 rally, 6 bonus

    GuestImage ovl2;
    std::array<std::array<std::string, 4>, 4> eventNames;  // [level][class]
    std::array<int8_t, 4> levelBlockByte{};                // race block + 0x57F
    std::array<int8_t, 4> gameModes{};                     // GAME SELECTION row -> game mode
    std::array<int8_t, 2> transmissions{};
    std::array<std::vector<ArcadeCourse>, kCourseListCount> courses;
    std::array<ArcadeClassCars, kClassListCount> classes;
    std::array<int8_t, 10> classSUnlock{};                 // 0x800519E4
    std::array<int8_t, 24> bonusUnlock{};                  // 0x80051D64
    std::vector<uint8_t> text;                             // the language block of data-arcade.txd (RAM 0x800F81E0)

    // `language` = career + 0 (1 = USA).
    static ArcadeMenuData Load(const DiscImage& disc, uint8_t language = 1);
    // The NUL-terminated string at a RAM address of the text block ("" outside it).
    std::string Text(uint32_t address) const;
    // A NUL-terminated string of the member-2 image.
    std::string ImageString(uint32_t address) const;
};

} // namespace gt2
