#pragma once
// Keyboard codes as the game's screens use them (gt2game: GameWindow::Held / Pressed and the "--script" key script,
// tools/gt2game/game_window.h). The numbers are the Win32 virtual-key codes, so the Windows window backend passes a
// key message's wParam straight through and nothing outside that backend needs <windows.h>; another platform's
// backend maps its own key codes onto these numbers.
//
// Letters and digits are their upper-case ASCII codes ('A', '5'), as the callers already write them.
namespace gt2::keys {

constexpr int kBack = 0x08;     // VK_BACK      Backspace (the menus' Triangle)
constexpr int kTab = 0x09;      // VK_TAB
constexpr int kReturn = 0x0D;   // VK_RETURN    Enter (Cross)
constexpr int kShift = 0x10;    // VK_SHIFT
constexpr int kControl = 0x11;  // VK_CONTROL
constexpr int kAlt = 0x12;      // VK_MENU
constexpr int kEscape = 0x1B;   // VK_ESCAPE
constexpr int kSpace = 0x20;    // VK_SPACE     (Circle)
constexpr int kPageUp = 0x21;   // VK_PRIOR
constexpr int kPageDown = 0x22; // VK_NEXT
constexpr int kEnd = 0x23;      // VK_END
constexpr int kHome = 0x24;     // VK_HOME
constexpr int kLeft = 0x25;     // VK_LEFT
constexpr int kUp = 0x26;       // VK_UP
constexpr int kRight = 0x27;    // VK_RIGHT
constexpr int kDown = 0x28;     // VK_DOWN
constexpr int kInsert = 0x2D;   // VK_INSERT
constexpr int kDelete = 0x2E;   // VK_DELETE    (Square)
constexpr int kF1 = 0x70;       // VK_F1
constexpr int kF5 = 0x74;       // VK_F5
constexpr int kF8 = 0x77;       // VK_F8
constexpr int kF10 = 0x79;      // VK_F10
constexpr int kF12 = 0x7B;      // VK_F12

} // namespace gt2::keys
