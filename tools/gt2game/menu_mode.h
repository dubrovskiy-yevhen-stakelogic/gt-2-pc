#pragma once
// gt2game --menu: the GT-mode menus natively (src/game/menu runtime + gt2view/menu_view), starting at the world map
// with a new-game career or a loaded save. Keyboard: arrows move, Enter = cross (action), Space = circle (the
// original's second "choose" button), Backspace / Esc = back (the original's triangle / square), S = start, F5 = write
// the career to --save-out now; the window's close button quits. A race started from the menus runs in the same
// window (the race hook, career_race.h) and comes back to the page it was started from.
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "game/career/career_state.h"

namespace gt2 {
class DiscImage;
class GtfsVolume;
} // namespace gt2

namespace gt2game {
class GameWindow;
class Panels;
} // namespace gt2game

struct MenuModeOptions {
    std::string careerPath;        // --career: card image or save file (empty = new game)
    std::string saveOut;           // --save-out: where the career is written when the menus are left (and on F5)
    uint32_t startPage = 0;        // --menu-page N (default 0 = world map)
    int windowWidth = 1280, windowHeight = 720;
    bool squarePixels = false;     // --menu-square: 512 x 480 with square pixels (a 512 x 480 window = 1:1)
    // --menu-script / --script "field:key[:hold],...": scripted input like gt2play (up down left right cross circle
    // square triangle start, and the keys of game_window.h); fields counted from the first frame of the window.
    std::string script;
    std::vector<std::pair<int, std::string>> shots; // --menu-shot <field> <out.png> (repeatable; menu frames only)
    std::vector<std::pair<int, std::string>> anyShots; // --shot-at <field> <out.png>: whatever screen shows then (menus, race, panels)
    std::vector<std::string> compare;               // --menu-compare <capture.vram.bin> (one per shot, optional)
    int quitAfter = 0;             // --menu-frames N: leave after N frames (0 = run until closed)
    bool sound = true;             // false with --no-sound: no menu effects / music (game/audio/menu_audio.h)
    bool reverb = true;            // --no-reverb
    bool pacing = true;            // false with --fast: frames as fast as they render (automated runs)
    bool endAfterShots = true;     // leave after the last shot once the script is done (false when the title hosts the menus)
    std::string recordAudio;       // --record-audio <wav> <s>: the device output also written to a WAV (dev aid)
};

// Hands an event / licence test over to the race path (career_race.h RunMenuRace) with the menus' window and panels
// (window null = run it headless); returns false when it did not run. The career is changed in place (day, prizes,
// results, licence records). `racePath` = the menu result 0x801EF5F5: 2 after the menus' entry check, 3 for items with
// flag bit 30 (the menus do not check the entry).
using MenuRaceHook = std::function<bool(gt2::career::CareerSave& save, const std::string& eventName, int racePath, gt2game::GameWindow* window,
                                        gt2game::Panels* panels)>;

// --menu: the menus in their own window on the career of options.careerPath (or a new game); the career is written to
// options.saveOut when they are left.
int RunMenuMode(const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, const MenuModeOptions& options, const MenuRaceHook& race);
// The menus in an existing window on an in-memory career (the title's Start Game): returns when the menus are left
// (Exit to the title, the window closed, or the frame / shot limits of `options`). options.careerPath / saveOut are
// used only by F5.
int RunMenuSession(gt2game::GameWindow& window, const gt2::DiscImage& disc, const gt2::GtfsVolume& vol, gt2::career::CareerSave& save, const MenuModeOptions& options,
                   const MenuRaceHook& race);
