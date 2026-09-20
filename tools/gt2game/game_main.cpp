// gt2game - the native game. No PS1 code, no emulation: only our engine, the ported simulation and the asset
// files read from the user's disc image (later: from a moddable data directory).
//
// FILES (2026-09-19, one window for title / menus / race): this file is the program's body - the command line and the
// dispatch (GameMain, game_main.h); main.cpp is the Windows entry point that calls it.
// The race building blocks moved to race_common.* (race data, options, shell log, headless drivers,
// self-test), the interactive race to race_view.* (scene, HUD, sound, music; the pre-race / pause / result panels of
// races started from the menus), the career races to career_race.* (--career headless and the menus' race hand-off,
// licence tests), the window / input / script to game_window.*, the native panels to panel.* and
// settings_screen.* (machine settings before a race). The title (title_mode.*) runs the GT-mode menus (menu_mode.*
// RunMenuSession) in its own window on its in-memory career; no child process.
//
//   gt2game <disc.bin> [--track NAME] [--car ID] [--paint N] [--shot <frames> <out.png>] [--drive] [--dump <ram.bin>]
//           [--cars N] [--manual] [--selftest] [--scan-courses] [--attract] [--generated-atan]
//           [--laps N] [--no-countdown] [--ai-player] [--headless <seconds>] [--no-sound]
// --no-sound runs without the sound (game/audio: engine / exhaust banks of the cars, tyre and impact effects, start beeps);
// --no-music without the XA race music (intro, race track from the green light, finish jingle), --music N forces the
// race track id (0..20, docs/formats/sound.md section 6), --no-reverb switches the SPU reverb of the voices off;
// --no-particles hides the tyre smoke (the original's sprite pool, gt2view/particles.h, docs/formats/particles.md);
// --laps N = the race's lap count (default 2); --no-countdown skips the 3 s start hold; --ai-player lets the AI drive
// slot 0; --headless <seconds> runs the race without a window and prints the shell's events and the standings;
// --license B-1 (or LJB00) runs a licence test from carparam/usa_license_data.dat (course, car, settings block; game
// mode 3; docs/formats/license.md), --license-decel A = the braking of the headless test driver (m/s^2);
// --scan-courses runs every course of the VOL through the parsers and the race-data builder (headless summary);
// --attract uses the attract race's shell state (see disc_data.h ShellState); --generated-atan uses the generated arc-tangent
// table of trig.h instead of the executable's (0x800A4AC8, the default; the two differ by one unit in a few entries).
// Automated runs of the windowed modes: --script "field:key[:hold],..." (keys of game_window.h, fields = frames from the
// program's first frame, across title, menus, race and panels), --shot-at <field> <png> (repeatable), --fast (no frame
// pacing). Races from the menus: --headless <s> runs them without the window (the former behaviour), --ai-player lets
// the AI drive the player's car, --seed N fixes the VSync counter of the event picks.
// Keys (race): Up = throttle, Down = brake, Left/Right = steer, Space = handbrake, R = reverse, Q/A = gear up/down (with
// --manual), Backspace = back to the grid, C = camera, Esc = quit (a race from the menus: pause menu), Enter = X.
// The race camera is the original's (game/camera): C = the camera button (Driver -> Chase 1 -> Chase 2), V held = look
// back; start position / view angle from the options (--camera N, --view-angle N, --look-back override; --old-camera =
// the former camera, debugging only).
//
// STATUS: the driving model is the bit-exact port of the original's car physics (src/game/sim, sim::RaceSim),
// stepped at the original's 30 Hz independently of the render rate. Everything it consumes comes from the disc:
// the tuning constants from the race overlay / executable (src/game/sim/disc_data.*), the course's race data
// from its .tro and .crsinfo (src/gt2formats/course_data.*), the car records from the parameter tables
// (src/gt2formats/car_params.*). --dump <ram.bin> is a development cross-check only: the disc-derived data is
// compared with the RAM dump of the original's race and the differences are printed.
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "career_race.h"
#include "game_main.h"
#include "pc_overlay.h"
#include "game/career/career_state.h"
#include "game/career/championship.h"
#include "game/shell/title_options.h"
#include "game/sim/dev_dump_constants.h"
#include "game_window.h"
#include "graphics_options.h"
#include "panel.h"
#include "change_parts_check.h"
#include "race_menu_check.h"
#include "gt2formats/car_info.h"
#include "gt2formats/exe_profile.h"
#include "gt2formats/license_data.h"
#include "gt2formats/replay_card.h"
#include "gt2formats/xa_audio.h"
#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "platform/os/paths.h"
#include "menu_mode.h"
#include "arcade_mode.h"
#include "arcade_race.h"
#include "ghost_replay.h"
#include "mods.h"
#include "movie_player.h"
#include "race_common.h"
#include "race_view.h"
#include "split_race.h"
#include "title_mode.h"

using namespace gt2;

namespace gt2game {

int GameMain(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // the log of automated runs stays complete whatever happens to the process
    if (argc < 2) {
        std::puts("usage: gt2game <disc.bin> [--track NAME] [--car ID] [--paint N] [--shot <frames> <out.png>] [--drive] [--dump <ram.bin>] [--cars N] [--manual] [--selftest]\n"
                  "               [--scan-courses] [--attract] [--generated-atan] [--laps N] [--no-countdown] [--ai-player] [--headless <seconds>] [--license B-1] [--license-decel A] [--no-sound] [--no-music] [--music N] [--no-reverb] [--no-hud] [--no-particles] [--replay-hud] [--replay-view N] [--hud-mode N] [--kmh] [--race-detail] [--max-detail] [--sponsors CATEGORY] [--sponsor-seed N] [--window WxH]\n"
                  "               [--career <card.mcd | save> (--event <name> | --championship <name>) [--save-out <file>] [--seed N] [--force-entry] [--headless <seconds>]]\n"
                  "               [--menu [--career <card.mcd | save>] [--save-out <file>] [--menu-page N] [--menu-square] [--menu-script S] [--menu-shot <field> <out.png>] [--menu-compare <cap.vram.bin>] [--menu-frames N]]\n"
                  "               no mode flag / --title: the title screen [--card <slot1.mcd>] [--card2 <slot2.mcd>] [--career <save>] [--settings <file>] [--title-square] [--title-script S] [--title-shot <field> <out.png>] [--title-compare <cap.vram.bin>] [--title-frames N]; --race: the default race\n"
                  "               windowed modes: [--script S] [--shot-at <field> <out.png>] [--fast]\n"
                  "               VR (docs/research/vr_port_plan.md): [--vr] (every screen on the OpenXR cinema quad, the race in stereo) [--xr-deterministic] (one field per compositor frame)\n"
                  "               VR options (settings.txt vr_*): [--vr-mono] [--vr-multiview 0|1] [--vr-horizon 0..100] [--vr-world-scale P] [--vr-render-scale 50..200] [--vr-near-mm N] [--vr-seat X Y Z]\n"
                  "               VR checks (tests\\xr): [--vr-ipd M] [--vr-original-fov] [--vr-eye WxH] [--vr-pose-log <csv>]\n"
                  "               movies (Arcade disc, docs/formats/str_video.md): [--no-movies] [--movies] (boot intro also in scripted runs) [--movie N] (play one, 0..26)\n"
                  "               mods (docs/formats/car_json.md, track_json.md): [--mods <dir>] (<dir>\\cars\\<id>.json, <dir>\\tracks\\<name>.json) [--ai-cars id,id,...] [--mod-physics-check <steps>]\n"
                  "               graphics (settings.txt / PC SETTINGS; docs/formats/modern_graphics.md): [--vanilla] [--modern] [--frame-rate original|display] [--frame-cap N]\n"
                  "               [--vsync 0|1] [--render-scale 50..200] [--msaa 1|2|4|8] [--texture-filter nearest|smooth] [--texture-mapping perspective|affine]\n"
                  "               [--draw-distance original|all|<m>] [--max-detail] [--frame-log <csv>] [--interp-alpha A]");
        return 2;
    }
    try {
        std::string trackName = "seattle", carId = "us36n", shotPath, dumpPath, modsDir;
        std::vector<std::string> aiCars; // --ai-cars
        int modPhysicsSteps = 0;         // --mod-physics-check
        bool arcadeMode = false, trackGiven = false, carGiven = false, carsGiven = false; // --arcade (arcade_race.h)
        ArcadeRaceOptions arcadeOptions;
        uint32_t paint = 0;
        int shotFrames = 0;
        size_t carCount = 1;
        bool drive = false, selfTest = false, scanCourses = false, noSound = false, noHud = false, replayHud = false, raceDetail = false, metricHud = false;
        int hudMode = 2, replayView = 0;
        bool hudModeGiven = false;
        bool noParticles = false; // --no-particles: no tyre smoke (gt2view/particles.h)
        bool noMusic = false, noReverb = false;
        int musicTrack = -1;
        int cameraPosition = -1, viewAngle = -1; // --camera N / --view-angle N (race_view.h; default: the title's options)
        bool lookBack = false, oldCamera = false;
        std::string recordPath;
        double recordSeconds = 0;
        std::string licenseName;
        std::string replayPath, framesComparePath, replayOut, raceSetupPath; // --replay <file.gmr | demo>, --frames-compare <capture>, --replay-out <file.gmr>
        int framesMax = 0;
        double replayCheckSeconds = 0;
        bool replayCheckAnalog = false;
        double headlessSeconds = 0;
        int windowWidth = 1280, windowHeight = 720;
        std::string sponsorCategory = "General01"; // 0x800275E8: "General01" unless the menus chose another category
        uint32_t sponsorSeed = 0;
        bool sponsorSeedGiven = false;
        DataOptions options;
        RaceOptions raceOptions;
        CareerOptions careerOptions;
        bool seedGiven = false;
        bool menuMode = false;         // --menu: the GT-mode menus (menu_mode.h)
        MenuModeOptions menuOptions;
        std::string globalScript;      // --script: keys for every windowed screen (game_window.h)
        std::vector<std::pair<int, std::string>> anyShots; // --shot-at <field> <png>
        bool fast = false;             // --fast: no frame pacing
        bool vr = false, vrDeterministic = false; // --vr / --xr-deterministic (game_window.h, vr_port_plan.md M1)
        // The VR options (M2): settings.txt's vr_* keys, each overridden by its flag.
        shell::VrSettings vrFile;
        bool vrMonoFlag = false, vrOriginalFov = false;
        int vrMultiview = -1, vrHorizon = -1, vrWorldScale = -1, vrRenderScale = -1, vrNearMm = -1;
        int vrSeatMm[3] = {INT_MIN, INT_MIN, INT_MIN};
        double vrIpd = -1;
        int vrEyeWidth = 0, vrEyeHeight = 0;
        std::string vrPoseLog;
        bool autoRace = false;         // --auto-race (race_view.h RaceViewConfig::autoAdvance)
        // The title (title_mode.h) is the start screen when no race / mode flag is given (or with --title); --race
        // runs the former default race. Cards and settings live in "saves" next to the executable unless given.
        bool titleMode = true, forceTitle = false;
        std::vector<std::string> raceScreenCheck; // --race-screen-check <screen> <capture.txt> <out.png> [state] (panel.h)
        bool raceMenuCheck = false;               // --race-menu-check <screen> <cap.vram.bin> <side.png> [..] (race_menu_check.h)
        bool changePartsCheck = false;            // --change-parts-check <ram.bin> <open field> <script> <out dir> <cap>@<field> .. (change_parts_check.h)
        TitleModeOptions titleOptions;
        ArcadeModeOptions arcadeModeOptions; // the arcade disc without a race / mode flag (arcade_mode.h)
        int movieOnly = -1;                  // --movie N: one movie of STREAM.DAT (movie_player.h), then exit
        const std::filesystem::path savesDir = gt2::os::SavesDir();
        titleOptions.card1Path = (savesDir / "card1.mcd").string();
        titleOptions.settingsPath = (savesDir / "settings.txt").string();
        for (int i = 2; i < argc; i++) {
            static const char* const kTitleFlags[] = {"--title", "--window", "--no-sound", "--no-music", "--card", "--card2", "--career", "--settings",
                                                      "--title-square", "--title-script", "--title-shot", "--title-compare", "--title-frames",
                                                      "--script", "--shot-at", "--fast", "--ai-player", "--seed", "--headless", "--manual", "--auto-race",
                                                      "--vr", "--xr-deterministic", // every mode's window becomes the OpenXR cinema quad
                                                      "--vr-mono", "--vr-multiview", "--vr-horizon", "--vr-world-scale", "--vr-render-scale",
                                                      "--vr-near-mm", "--vr-seat", "--vr-ipd", "--vr-original-fov", "--vr-eye", "--vr-pose-log",
                                                      "--arcade-square", "--arcade-shot", "--arcade-compare", "--arcade-frames", "--arcade-vsync",
                                                      "--arcade-setup-out", "--arcade-no-race", "--arcade-record-audio", "--fake-pad", "--fake-pad2",
                                                      "--no-movies", "--movies", // movie_player.h: the intro / endings / course previews
                                                      // presentation options that apply to the races the title and the menus start
                                                      "--kmh", "--hud-mode", "--no-hud", "--replay-hud", "--no-particles", "--no-reverb", "--race-detail",
                                                      "--sponsors", "--sponsor-seed", "--mods", "--ai-cars", "--generated-atan", "--laps", "--music",
                                                      // the graphics settings (graphics_options.h) apply to every mode
                                                      "--vanilla", "--modern", "--frame-rate", "--frame-cap", "--vsync", "--render-scale", "--msaa",
                                                      "--texture-filter", "--texture-mapping", "--draw-distance", "--frame-log"};
            const std::string a = argv[i];
            if (a.rfind("--", 0) != 0) continue; // a flag's value
            if (std::find(std::begin(kTitleFlags), std::end(kTitleFlags), a) == std::end(kTitleFlags)) titleMode = false;
        }
        // The graphics settings (graphics_options.h): settings.txt (--settings), then the command line's graphics flags.
        for (int i = 2; i + 1 < argc; i++)
            if (std::string(argv[i]) == "--settings") titleOptions.settingsPath = argv[i + 1];
        const shell::GraphicsSettings fileGraphics = shell::PcSettings::Load(titleOptions.settingsPath).graphics;
        SetGraphicsFromSettings(fileGraphics);
        LoadOverlaySettings(titleOptions.settingsPath);
        std::string frameLogPath;  // --frame-log <csv> (race_view.h)
        double interpAlpha = -1;   // --interp-alpha A (race_view.h)
        for (int i = 2; i < argc; i++) {
            std::string a = argv[i];
            if (a == "--race-screen-check") { raceScreenCheck.assign(argv + i + 1, argv + argc); break; } // panel.h RunRaceScreenCheck
            if (ParseGraphicsFlag(argc, argv, i, fileGraphics)) continue; // --vanilla, --modern, --frame-rate ... --max-detail
            if (a == "--frame-log" && i + 1 < argc) { frameLogPath = argv[++i]; continue; }
            if (a == "--interp-alpha" && i + 1 < argc) { interpAlpha = std::clamp(std::atof(argv[++i]), 0.0, 1.0); continue; }
            if (a == "--race-menu-check") { raceMenuCheck = true; break; }                                  // race_menu_check.h
            if (a == "--change-parts-check") { changePartsCheck = true; break; }                            // change_parts_check.h
            if (a == "--title") { forceTitle = true; continue; }
            if (a == "--race") { titleMode = false; continue; }
            if (a == "--card" && i + 1 < argc) { titleOptions.card1Path = argv[++i]; continue; }   // memory card slot 1 (.mcd)
            if (a == "--card2" && i + 1 < argc) { titleOptions.card2Path = argv[++i]; continue; } // slot 2 (none by default)
            if (a == "--settings" && i + 1 < argc) { titleOptions.settingsPath = argv[++i]; continue; }
            if (a == "--title-square") { titleOptions.squarePixels = true; continue; }
            if (a == "--title-script" && i + 1 < argc) { titleOptions.script = argv[++i]; continue; }
            if (a == "--title-shot" && i + 2 < argc) { titleOptions.shots.emplace_back(std::atoi(argv[i + 1]), argv[i + 2]); i += 2; continue; }
            if (a == "--title-compare" && i + 1 < argc) { titleOptions.compare.push_back(argv[++i]); continue; }
            if (a == "--title-frames" && i + 1 < argc) { titleOptions.quitAfter = std::atoi(argv[++i]); continue; }
            // The arcade disc's title and menus (arcade_mode.h): comparisons and automated runs.
            if (a == "--arcade-square") { arcadeModeOptions.squarePixels = true; continue; }
            if (a == "--arcade-shot" && i + 2 < argc) { arcadeModeOptions.shots.emplace_back(std::atoi(argv[i + 1]), argv[i + 2]); i += 2; continue; }
            if (a == "--arcade-compare" && i + 1 < argc) { arcadeModeOptions.compare.push_back(argv[++i]); continue; }
            if (a == "--arcade-frames" && i + 1 < argc) { arcadeModeOptions.quitAfter = std::atoi(argv[++i]); continue; }
            if (a == "--arcade-vsync" && i + 1 < argc) { arcadeModeOptions.vsync = int32_t(std::strtoul(argv[++i], nullptr, 0)); continue; }
            if (a == "--arcade-setup-out" && i + 1 < argc) { arcadeModeOptions.setupOut = argv[++i]; continue; }
            if (a == "--arcade-no-race") { arcadeModeOptions.noRace = true; continue; }
            if (a == "--no-movies") { arcadeModeOptions.noMovies = true; continue; } // movie_player.h: no intro / endings / previews
            if (a == "--movies") { arcadeModeOptions.forceMovies = true; continue; } // the boot intro also in scripted runs
            if (a == "--movie" && i + 1 < argc) { movieOnly = std::atoi(argv[++i]); continue; } // play one STREAM.DAT movie and exit
            if (a == "--arcade-record-audio" && i + 1 < argc) { arcadeModeOptions.recordAudio = argv[++i]; continue; } // menus' WAV + .events.txt
            if (a == "--script" && i + 1 < argc) { globalScript = argv[++i]; continue; }
            if (a == "--shot-at" && i + 2 < argc) { anyShots.emplace_back(std::atoi(argv[i + 1]), argv[i + 2]); i += 2; continue; }
            if (a == "--fast") { fast = true; continue; }
            // --vr: every screen of every mode on the cinema quad of an OpenXR session (game_window_xr.cpp).
            if (a == "--vr") { vr = true; continue; }
            if (a == "--xr-deterministic") { vr = true; vrDeterministic = true; continue; }
            // VR options (docs/research/vr_port_plan.md, M2); the last five exist for the checks in tests\xr.
            if (a == "--vr-mono") { vrMonoFlag = true; continue; }
            if (a == "--vr-multiview" && i + 1 < argc) { vrMultiview = std::atoi(argv[++i]) != 0 ? 1 : 0; continue; }
            if (a == "--vr-horizon" && i + 1 < argc) { vrHorizon = std::clamp(std::atoi(argv[++i]), 0, 100); continue; }
            if (a == "--vr-world-scale" && i + 1 < argc) { vrWorldScale = std::clamp(std::atoi(argv[++i]), 10, 1000); continue; }
            if (a == "--vr-render-scale" && i + 1 < argc) { vrRenderScale = std::clamp(std::atoi(argv[++i]), 50, 200); continue; }
            if (a == "--vr-near-mm" && i + 1 < argc) { vrNearMm = std::clamp(std::atoi(argv[++i]), 10, 1000); continue; }
            if (a == "--vr-seat" && i + 3 < argc) {
                for (int k = 0; k < 3; k++) vrSeatMm[k] = std::clamp(std::atoi(argv[i + 1 + k]), -2000, 2000);
                i += 3;
                continue;
            }
            if (a == "--vr-ipd" && i + 1 < argc) { vrIpd = std::clamp(std::atof(argv[++i]), 0.0, 0.2); continue; }
            if (a == "--vr-original-fov") { vrOriginalFov = true; continue; }
            if (a == "--vr-eye" && i + 1 < argc) { // WxH: a fixed per-eye image, so a capture can be compared with a desktop frame
                const std::string size = argv[++i];
                const size_t x = size.find('x');
                if (x == std::string::npos) throw std::runtime_error("--vr-eye needs WxH");
                vrEyeWidth = std::atoi(size.substr(0, x).c_str());
                vrEyeHeight = std::atoi(size.substr(x + 1).c_str());
                continue;
            }
            if (a == "--vr-pose-log" && i + 1 < argc) { vrPoseLog = argv[++i]; continue; }
            if (a == "--fake-pad" && i + 1 < argc) { SetFakePadScript(argv[++i]); continue; } // a scripted controller (platform/input/input_system.h)
            if (a == "--fake-pad2" && i + 1 < argc) { SetFakePad2Script(argv[++i]); continue; } // a scripted controller in port 2 (the 2 player Battle)
            if (a == "--auto-race") { autoRace = true; continue; } // dev aid: the race panels of the menus advance by themselves
            if (a == "--menu") menuMode = true;
            else if (a == "--menu-page" && i + 1 < argc) menuOptions.startPage = uint32_t(std::strtoul(argv[++i], nullptr, 0));
            else if (a == "--menu-square") menuOptions.squarePixels = true;
            else if (a == "--menu-script" && i + 1 < argc) menuOptions.script = argv[++i];
            else if (a == "--menu-shot" && i + 2 < argc) { menuOptions.shots.emplace_back(std::atoi(argv[i + 1]), argv[i + 2]); i += 2; }
            else if (a == "--menu-compare" && i + 1 < argc) menuOptions.compare.push_back(argv[++i]);
            else if (a == "--menu-frames" && i + 1 < argc) menuOptions.quitAfter = std::atoi(argv[++i]);
            else if (a == "--track" && i + 1 < argc) { trackName = argv[++i]; trackGiven = true; }
            else if (a == "--car" && i + 1 < argc) { carId = argv[++i]; carGiven = true; }
            else if (a == "--arcade") arcadeMode = true; // the arcade menus' race: event by level and class (arcade_race.h)
            else if (a == "--arcade-level" && i + 1 < argc) { arcadeOptions.level = std::clamp(std::atoi(argv[++i]), 0, 2); arcadeMode = true; }
            else if (a == "--arcade-class" && i + 1 < argc) { arcadeOptions.carClass = char(std::toupper(uint8_t(argv[++i][0]))); arcadeMode = true; }
            else if (a == "--arcade-tyres" && i + 1 < argc) { arcadeOptions.tyreTable = std::atoi(argv[++i]); arcadeMode = true; }
            else if (a == "--arcade-seed" && i + 1 < argc) { arcadeOptions.seed = uint32_t(std::strtoul(argv[++i], nullptr, 0)); arcadeMode = true; }
            else if (a == "--opponents" && i + 1 < argc) { // opponent rows of usa_arcade_data.dat table 31 (number = row + 1), comma separated
                for (const char* p = argv[++i]; *p;) {
                    char* end = nullptr;
                    arcadeOptions.opponents.push_back(uint32_t(std::strtoul(p, &end, 10)));
                    p = (*end == ',') ? end + 1 : end;
                    if (end == p && *p) throw std::runtime_error("bad --opponents");
                }
                arcadeMode = true;
            }
            else if (a == "--paint" && i + 1 < argc) paint = uint32_t(std::atoi(argv[++i]));
            else if (a == "--shot" && i + 2 < argc) { shotFrames = std::atoi(argv[i + 1]); shotPath = argv[i + 2]; i += 2; }
            else if (a == "--dump" && i + 1 < argc) dumpPath = argv[++i];
            else if (a == "--mods" && i + 1 < argc) modsDir = argv[++i]; // override layer: <dir>\cars\<id>.json (+ glTF), docs/formats/car_json.md
            else if (a == "--cars" && i + 1 < argc) { carCount = size_t(std::clamp(std::atoi(argv[++i]), 1, int(sim::kMaxCars))); carsGiven = true; }
            else if (a == "--laps" && i + 1 < argc) raceOptions.laps = uint8_t(std::clamp(std::atoi(argv[++i]), 1, 99));
            else if (a == "--headless" && i + 1 < argc) headlessSeconds = std::atof(argv[++i]);
            else if (a == "--no-countdown") raceOptions.countdown = false;
            else if (a == "--no-sound") noSound = true;
            else if (a == "--no-music") noMusic = true;   // no XA race music (MUSIC.DAT); the shell's music requests still run
            else if (a == "--no-reverb") noReverb = true; // the SPU reverb of the engine voices off (for A/B listening)
            else if (a == "--record-audio" && i + 2 < argc) { recordPath = argv[i + 1]; recordSeconds = std::atof(argv[i + 2]); i += 2; } // dev aid: WAV of the output, quit after N s
            else if (a == "--music" && i + 1 < argc) musicTrack = std::clamp(std::atoi(argv[++i]), 0, int(kMusicTrackCount) - 1); // force the race track id (0..20)
            else if (a == "--no-hud") noHud = true;
            else if (a == "--no-particles") noParticles = true; // no tyre smoke sprites
            else if (a == "--window" && i + 1 < argc) { // client size, e.g. 1280x720 (default) or 960x720
                if (std::sscanf(argv[++i], "%dx%d", &windowWidth, &windowHeight) != 2 || windowWidth < 64 || windowHeight < 64) throw std::runtime_error("bad --window");
            }
            else if (a == "--sponsors" && i + 1 < argc) sponsorCategory = argv[++i]; // .crstims.tsd category (General01 default; General02, One-Make, JP, US, UK, DE, FR, IT, TUNE)
            else if (a == "--sponsor-seed" && i + 1 < argc) { sponsorSeed = uint32_t(std::strtoul(argv[++i], nullptr, 0)); sponsorSeedGiven = true; }
            else if (a == "--race-detail") raceDetail = true; // the original's in-race chunk rule (far render-list entries: glow records only)
            else if (a == "--hud-mode" && i + 1 < argc) { hudMode = std::atoi(argv[++i]); hudModeGiven = true; } // the HUD layout of shell mode N (0 GT race, 2 arcade = default, 3 licence ...)
            else if (a == "--replay-view" && i + 1 < argc) replayView = std::atoi(argv[++i]); // with --replay-hud: 0 lap block only (attract), 1 full HUD
            else if (a == "--kmh") metricHud = true; // km/h instead of the US game's mph (our option)
            else if (a == "--replay-hud") replayHud = true; // the attract race's HUD layout (for comparing with the original's capture)
            else if (a == "--ai-player") raceOptions.aiPlayer = true;
            else if (a == "--license" && i + 1 < argc) { // a licence test: "B-1" .. "S-10" or a test name "LJB00" (license_data.h)
                licenseName = LicenseTestName(argv[++i]);
                if (licenseName.empty()) throw std::runtime_error(std::string("bad --license ") + argv[i]);
            }
            else if (a == "--license-decel" && i + 1 < argc) raceOptions.licenseDecel = std::atof(argv[++i]);
            else if (a == "--replay" && i + 1 < argc) replayPath = argv[++i];                  // play a replay file (.gmr; "demo" = arcade/demofile_us.gmr)
            else if (a == "--replay-out" && i + 1 < argc) replayOut = argv[++i];               // save the race's replay (.gmr layout, gt2formats/replay.h)
            else if (a == "--frames-compare" && i + 1 < argc) framesComparePath = argv[++i];   // dev: the race against gt2verify --race-capture frames
            else if (a == "--race-setup" && i + 1 < argc) raceSetupPath = argv[++i];           // dev: the race of a race block setup file (<capture>.setup); mode 0 = the split screen
            else if (a == "--frames" && i + 1 < argc) framesMax = std::atoi(argv[++i]);
            else if (a == "--replay-check" && i + 1 < argc) replayCheckSeconds = std::atof(argv[++i]); // dev: record / replay / reload determinism
            else if (a == "--replay-check-analog" && i + 1 < argc) { replayCheckSeconds = std::atof(argv[++i]); replayCheckAnalog = true; } // ... with a scripted analog pad
            else if (a == "--career" && i + 1 < argc) careerOptions.savePath = argv[++i];   // card image (.mcd) or save file
            else if (a == "--event" && i + 1 < argc) careerOptions.eventName = argv[++i];   // event name of usa_gtmode_race.dat
            else if (a == "--championship" && i + 1 < argc) { // a series: race 1's event name, or the series base (e.g. "GTW05")
                careerOptions.eventName = argv[++i];
                careerOptions.championship = true;
            }
            else if (a == "--save-out" && i + 1 < argc) careerOptions.saveOut = argv[++i];  // where the career goes after the race
            else if (a == "--seed" && i + 1 < argc) { careerOptions.seed = uint32_t(std::strtoul(argv[++i], nullptr, 0)); seedGiven = true; } // the VSync counter value
            else if (a == "--force-entry") careerOptions.force = true;
            else if (a == "--drive") drive = true;
            else if (a == "--manual") raceOptions.manual = true;
            else if (a == "--selftest") selfTest = true;
            else if (a == "--scan-courses") scanCourses = true;
            else if (a == "--ai-cars" && i + 1 < argc) { // the AI slots' cars in order, disc or --mods ids (mods.h), comma separated
                for (std::string list = argv[++i]; !list.empty();) {
                    const size_t comma = list.find(',');
                    aiCars.push_back(list.substr(0, comma));
                    list = comma == std::string::npos ? std::string() : list.substr(comma + 1);
                }
            }
            else if (a == "--mod-physics-check" && i + 1 < argc) modPhysicsSteps = std::atoi(argv[++i]); // mods.h ModCoursePhysicsCheck
            else if (a == "--attract") options.attractShell = true;
            else if (a == "--generated-atan") options.generatedAtan = true;
            else if (a == "--camera" && i + 1 < argc) cameraPosition = std::clamp(std::atoi(argv[++i]), 0, 2); // 0 Driver, 1 Chase 1, 2 Chase 2
            else if (a == "--view-angle" && i + 1 < argc) viewAngle = std::clamp(std::atoi(argv[++i]), 0, 2); // 0 Narrow, 1 Standard, 2 Wide
            else if (a == "--look-back") lookBack = true;   // the look-back button held (screenshots)
            else if (a == "--old-camera") oldCamera = true; // the former chase / bonnet / high camera (debugging only)
            else throw std::runtime_error("unknown argument " + a);
        }

        // An automated run (a key script, a screenshot, a comparison or check, --fast, --headless, a frame limit)
        // shows its window without activating it, so that it does not take the keyboard from whatever the user is
        // doing; an interactive launch activates its window as before (game_window.h SetWindowNoFocus).
        for (int i = 2; i < argc; i++) {
            const std::string a = argv[i];
            if (a.rfind("--", 0) != 0) continue;
            auto endsWith = [&](const char* suffix) {
                const size_t n = std::strlen(suffix);
                return a.size() > n && a.compare(a.size() - n, n, suffix) == 0;
            };
            if (a == "--fast" || a == "--headless" || a == "--xr-deterministic" || a == "--selftest" || a == "--fake-pad" || a == "--fake-pad2" ||
                a.find("script") != std::string::npos || a.find("shot") != std::string::npos || endsWith("-compare") || endsWith("-check") ||
                endsWith("-frames") || a == "--frames")
                SetWindowNoFocus(true);
        }

        if (vr) { // the OpenXR cinema quad instead of the desktop window (docs/research/vr_port_plan.md, M1)
            SetVrMode(true, vrDeterministic);
            windowWidth = kCinemaWidth; // every screen is built for the quad's 4:3 image
            windowHeight = kCinemaHeight;
            // The VR options: settings.txt (the same file the graphics settings come from) under the flags (M2).
            vrFile = shell::PcSettings::Load(titleOptions.settingsPath).vr;
            VrOptions o;
            o.stereo = vrFile.stereo && !vrMonoFlag;
            o.multiview = vrMultiview >= 0 ? vrMultiview != 0 : vrFile.multiview;
            o.horizonLock = float(vrHorizon >= 0 ? vrHorizon : vrFile.horizonLock) / 100.0f;
            o.worldScale = float(vrWorldScale >= 0 ? vrWorldScale : vrFile.worldScale) / 100.0f;
            o.renderScale = float(vrRenderScale >= 0 ? vrRenderScale : OverlayVrScale() >= 0 ? OverlayVrScale() : vrFile.renderScale) / 100.0f;
            o.nearZ = float(vrNearMm >= 0 ? vrNearMm : vrFile.nearMm) / 1000.0f;
            for (int k = 0; k < 3; k++) o.seat[k] = float(vrSeatMm[k] != INT_MIN ? vrSeatMm[k] : vrFile.seatMm[k]) / 1000.0f;
            o.ipd = float(vrIpd);
            o.originalFov = vrOriginalFov;
            o.eyeWidth = vrEyeWidth;
            o.eyeHeight = vrEyeHeight;
            o.poseLog = vrPoseLog;
            SetVrOptions(o);
        }

        DiscImage disc(argv[1]);
        GtfsVolume vol(disc);
        // The build of the disc (gt2formats/exe_profile.h): every table / global the game reads by its US Simulation v1.2
        // address is translated to this build's address; an unknown executable stops here.
        const ExeProfile& profile = ProfileOf(disc);
        SetActiveProfile(profile);
        if (!profile.reference) std::printf("disc: %s (%s), address profile of that build\n", profile.name, profile.exeName);
        if (!raceScreenCheck.empty()) return RunRaceScreenCheck(disc, vol, raceScreenCheck);
        if (raceMenuCheck) return RunRaceMenuCheck(disc, vol, argc, argv);
        if (changePartsCheck) return RunChangePartsCheck(disc, vol, argc, argv);
        if (scanCourses) return ScanCourses(disc, vol) ? 1 : 0;
        if (modPhysicsSteps > 0) { // every mod course of --mods (or the --track one) against its disc course, step by step
            if (modsDir.empty()) throw std::runtime_error("--mod-physics-check needs --mods <dir>");
            return ModCoursePhysicsCheck(disc, vol, modsDir, trackGiven ? trackName : std::string(), carCount, modPhysicsSteps) ? 1 : 0;
        }
        // The options of the title (PC settings file, shell::PcSettings) for the races: units, music / effects volumes.
        const shell::PcSettings pcSettings = shell::PcSettings::Load(titleOptions.settingsPath);
        if (pcSettings.metric) metricHud = true;
        // The race view's presentation options (race_view.h) for the standalone race and the races of the menus.
        RaceViewConfig view;
        view.discPath = argv[1];
        view.sound = !noSound;
        view.music = !noMusic;
        view.reverb = !noReverb;
        view.hud = !noHud;
        view.particles = !noParticles;
        view.raceDetail = raceDetail;
        view.frameLog = frameLogPath;
        view.interpAlpha = interpAlpha;
        std::printf("graphics: %s\n", DescribeGraphics(CurrentGraphics()).c_str());
        view.metric = metricHud;
        view.replayHud = replayHud;
        view.hudMode = hudMode;
        view.replayView = replayView;
        view.musicTrack = musicTrack;
        view.sponsorCategory = sponsorCategory;
        view.sponsorSeed = sponsorSeed;
        view.sponsorSeedGiven = sponsorSeedGiven;
        view.recordAudio = recordPath;
        view.recordSeconds = recordSeconds;
        view.drive = drive;
        view.attractShell = options.attractShell;
        view.pcSettings = &pcSettings;
        // The controller: player 1's pad block of the settings (the title's KEY CONFIGURATION / ANALOG / vibration) and ours.
        if (pcSettings.havePadBlocks) view.padBlock.assign(pcSettings.padBlocks[0].begin(), pcSettings.padBlocks[0].end());
        view.triggerPedals = pcSettings.triggerPedals;
        SetRumbleScale(pcSettings.rumbleScale);
        view.cameraPosition = cameraPosition;
        view.viewAngle = viewAngle;
        view.lookBack = lookBack;
        view.oldCamera = oldCamera;
        view.deterministic = !globalScript.empty() || !menuOptions.script.empty() || !anyShots.empty() || FakePadGiven() || vrDeterministic;
        view.autoAdvance = autoRace;

        // The menus' race hand-off (career_race.h): in the menus' window, or headless with --headless <s>.
        const MenuRaceHook raceHook = [&](career::CareerSave& save, const std::string& eventName, int racePath, GameWindow* window, Panels* panels) {
            MenuRaceContext ctx;
            ctx.window = headlessSeconds > 0 ? nullptr : window;
            ctx.panels = panels;
            ctx.view = view;
            { // the career's own pad block (career + 0x0A): the title's key configuration of this career
                const uint8_t* b = reinterpret_cast<const uint8_t*>(&save.state) + 0x0A;
                ctx.view.padBlock.assign(b, b + 0x52);
            }
            ctx.race = raceOptions;
            ctx.headlessSeconds = headlessSeconds > 0 ? headlessSeconds : 1800.0;
            ctx.seed = (seedGiven || view.deterministic) ? careerOptions.seed : gt2::os::TickCountMs();
            ctx.card1Path = titleOptions.card1Path; // SAVE GAME after a race (career_race.h)
            ctx.card2Path = titleOptions.card2Path;
            return RunMenuRace(disc, vol, save, eventName, racePath, ctx);
        };
        auto menuModeOptions = [&] {
            MenuModeOptions mo = menuOptions;
            mo.careerPath = careerOptions.savePath;
            mo.saveOut = careerOptions.saveOut;
            mo.windowWidth = windowWidth;
            mo.windowHeight = windowHeight;
            mo.sound = !noSound; // the menus' effects and music (menu_mode.cpp)
            mo.reverb = !noReverb;
            mo.recordAudio = recordPath;
            mo.pacing = !fast;
            if (!globalScript.empty()) mo.script = mo.script.empty() ? globalScript : mo.script + "," + globalScript;
            mo.anyShots = anyShots;
            return mo;
        };

        if (movieOnly >= 0) { // --movie N: the movie as the original plays it (intro / endings: member 5's numbers; previews full size)
            const std::unique_ptr<MovieLibrary> movies = MovieLibrary::Load(disc, argv[1]);
            if (!movies) throw std::runtime_error("this disc has no STREAM.DAT (movies are on the Arcade disc only)");
            GameWindow window("gt2game (movie)", windowWidth, windowHeight);
            window.SetPacing(!fast);
            if (!globalScript.empty()) window.AddScript(globalScript);
            for (const auto& s : anyShots) window.AddShot(s.first, s.second);
            const MovieResult r = PlayMovie(window, disc, *movies, movieOnly, !noSound);
            return r == MovieResult::kClosed ? 1 : 0;
        }
        if (profile.arcade && (forceTitle || titleMode) && !menuMode && !arcadeMode) { // the arcade disc: its title and menus (arcade_mode.h)
            arcadeModeOptions.discPath = argv[1];
            arcadeModeOptions.card1Path = titleOptions.card1Path;
            arcadeModeOptions.card2Path = titleOptions.card2Path;
            arcadeModeOptions.windowWidth = windowWidth;
            arcadeModeOptions.windowHeight = windowHeight;
            arcadeModeOptions.noSound = noSound;
            arcadeModeOptions.pacing = !fast;
            arcadeModeOptions.globalScript = globalScript;
            arcadeModeOptions.anyShots = anyShots;
            arcadeModeOptions.race = raceOptions;
            arcadeModeOptions.view = view;
            if (!hudModeGiven) arcadeModeOptions.view.hudMode = -1;
            if (!hudModeGiven) arcadeModeOptions.view.hudMode = 2; // the arcade HUD layout (shell mode 2 / 4)
            return RunArcadeMode(disc, vol, arcadeModeOptions);
        }
        if (profile.arcade && (menuMode || !careerOptions.savePath.empty() || !licenseName.empty()))
            throw std::runtime_error("the arcade disc has no GT mode / licences (US Arcade v1.1): use --arcade or --race");
        if (arcadeMode) { // the defaults of the arcade race dump (docs/research/arcade_disc.md): Tahiti Road, the Corvette, six cars
            if (!trackGiven) trackName = "tahiti_t";
            if (!carGiven) carId = "ccrcn";
            if (!carsGiven) carCount = 6;
            if (!hudModeGiven) view.hudMode = 2; // the arcade HUD layout (shell mode 2 / 4)
        }
        if (forceTitle || (titleMode && !arcadeMode && !menuMode && careerOptions.eventName.empty())) { // the start screen (title_mode.h)
            titleOptions.discPath = argv[1];
            titleOptions.careerPath = careerOptions.savePath;
            titleOptions.windowWidth = windowWidth;
            titleOptions.windowHeight = windowHeight;
            titleOptions.noSound = noSound || noMusic;
            titleOptions.noMovies = arcadeModeOptions.noMovies;
            titleOptions.forceMovies = arcadeModeOptions.forceMovies;
            titleOptions.globalScript = globalScript;
            titleOptions.anyShots = anyShots;
            titleOptions.pacing = !fast;
            titleOptions.raceView = &view; // the replay theater's replays (title_mode.h)
            titleOptions.raceOptions = &raceOptions;
            titleOptions.hudModeGiven = hudModeGiven;
            // Start Game: the GT-mode menus in the title's window on the title's career (in memory).
            const TitleGtModeHook gtMode = [&](GameWindow& window, career::CareerSave& save) {
                MenuModeOptions mo = menuModeOptions();
                mo.script.clear(); // the window already has the global script
                mo.anyShots.clear();
                mo.saveOut = titleOptions.card1Path;
                mo.endAfterShots = false; // the menus end by their Exit (to the title) or the window
                return RunMenuSession(window, disc, vol, save, mo, raceHook) == 0;
            };
            return RunTitleMode(disc, vol, titleOptions, gtMode);
        }
        if (menuMode) return RunMenuMode(disc, vol, menuModeOptions(), raceHook); // the GT-mode menus in their own window
        if (!careerOptions.savePath.empty()) {
            if (careerOptions.eventName.empty()) throw std::runtime_error("--career needs --event <name> or --championship <name>");
            if (careerOptions.championship) { // a series base name ("GTW05") names its first race; the name must be a series
                if (career::SeriesRaceCount(careerOptions.eventName) >= 2) careerOptions.eventName = career::SeriesEventName(careerOptions.eventName, 1);
                if (career::SeriesRaceCount(career::SeriesBase(careerOptions.eventName)) < 2)
                    throw std::runtime_error(careerOptions.eventName + " is not a championship (its series digits give fewer than 2 races)");
            }
            return RunCareerEvent(disc, vol, careerOptions, raceOptions, headlessSeconds > 0 ? headlessSeconds : 1800.0);
        }
        // --license: the test's record in carparam/usa_license_data.dat names the course and the car; the race is game
        // mode 3 with the test's settings block, one car, the lap count of the licence dump (0x801D586B = 255).
        std::optional<LicenseData> licenseData;
        LicenseTest licenseTest;
        if (!licenseName.empty()) {
            licenseData.emplace(LicenseData::Load(vol));
            licenseTest = licenseData->Test(licenseName);
            trackName = licenseTest.course;
            carCount = 1;
            raceOptions.laps = 255;
            raceOptions.license = &licenseTest;
            options.license = &licenseTest;
            if (!hudModeGiven) view.hudMode = 3;
            std::printf("licence %s: course %s, car %s, type %u, target lap %u, box %u m + %u m, medals %s / %s / %s\n", licenseTest.name.c_str(), licenseTest.course.c_str(),
                        UnpackCarId(licenseData->RaceCar(licenseTest).carId).c_str(), licenseTest.Type(), licenseTest.TargetLap(), licenseTest.BoxStart() * 10u,
                        licenseTest.BoxLength(), FormatMs(int32_t(licenseTest.MedalTime(1))).c_str(), FormatMs(int32_t(licenseTest.MedalTime(2))).c_str(),
                        FormatMs(int32_t(licenseTest.MedalTime(3))).c_str());
        }
        std::optional<ReplayFile> replayFile;
        std::optional<ReplayPayload> ghostRecord; // a game mode 6 record (ghost_replay.h)
        sim::GhostSession ghostSession;
        if (!replayPath.empty()) { // a replay: its race block, cars and player stream (gt2formats/replay.h)
            std::vector<uint8_t> bytes;
            int replayIndex = 0; // "<file>#N": replay N of a replay file / memory card (gt2formats/replay_card.h)
            if (const size_t hash = replayPath.rfind('#'); hash != std::string::npos && hash + 1 < replayPath.size()) {
                replayIndex = std::atoi(replayPath.c_str() + hash + 1);
                replayPath.resize(hash);
            }
            int demoLicence = 0, demoTest = 0; // "licence-demo:B-1": the licence menu's Demonstration (career_race.h LicenceDemoReplay)
            if (replayPath.rfind("licence-demo:", 0) == 0 && ParseLicenceLabel(replayPath.substr(13), demoLicence, demoTest)) replayFile = LicenceDemoReplay(disc, vol, demoLicence, demoTest);
            else if (replayPath == "demo") bytes = vol.Read("arcade/demofile_us.gmr");
            else {
                std::ifstream in(replayPath, std::ios::binary);
                if (!in) throw std::runtime_error("cannot open " + replayPath);
                bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            }
            if (!replayFile) {
                ghostRecord = GhostRecordOf(bytes, replayIndex);
                if (!ghostRecord) replayFile = LoadReplay(bytes, replayIndex);
            }
        }
        RaceData data;
        // --frames-compare of a capture with a setup file (gt2verify --race-capture): the capture's own race (entries, settings).
        const std::string captureSetup = !raceSetupPath.empty() ? raceSetupPath : framesComparePath.empty() ? std::string() : framesComparePath + ".setup";
        const bool captureRace = !replayFile && !ghostRecord && !licenseData && !captureSetup.empty() && std::filesystem::exists(captureSetup);
        if (captureRace) {
            carCount = LoadCaptureRace(disc, vol, captureSetup, trackName, raceOptions, data);
            carId = data.carIds[0];
        } else if (arcadeMode && !replayFile) {
            BuildArcadeRace(disc, vol, trackName, carId, carCount, arcadeOptions, raceOptions, data);
        } else if (ghostRecord) { // the replay of a Time Trial / Rally: player 1 on the record's lap ring
            carCount = LoadGhostRecordRace(disc, vol, *ghostRecord, data, raceOptions, ghostSession);
            trackName = data.trackName;
            carId = data.carIds[0];
            view.ghostReplay = true;
        } else if (replayFile) {
            LoadReplayRace(disc, vol, *replayFile, data, raceOptions);
            trackName = data.trackName;
            carCount = data.params.size();
            carId = data.carIds[0];
            if (!sponsorSeedGiven) { view.sponsorSeed = replayFile->SponsorSeed(); view.sponsorSeedGiven = true; } // race block + 0x54
            if (!hudModeGiven && replayFile->GameMode() == 3) view.hudMode = 3; // a licence replay: the licence layout
        } else if (const std::string modCourse = ModCoursePath(modsDir, trackName); !modCourse.empty()) {
            LoadModRaceTrack(disc, vol, modCourse, trackName, options, data); // --mods <dir> with <dir>/tracks/<name>.json (mods.h)
        } else {
            LoadRaceTrack(disc, vol, trackName, options, data);
        }
        if (replayFile || ghostRecord || captureRace || arcadeMode) {
        } else if (licenseData) {
            BuildLicenseCar(vol, *licenseData, licenseTest, data);
            carId = data.carIds[0];
        } else {
            BuildCarRecords(vol, carId, carCount, modsDir, data, aiCars);
        }
        if (!data.paints.empty() && !replayFile && !ghostRecord && !captureRace && (!licenseData || paint != 0)) data.paints[0] = paint; // a replay / capture / licence keeps its slots' paints (--paint overrides a licence car)
        size_t sections = 0;
        for (const auto& list : data.course.sections) sections += list.size();
        std::printf("%s: %d m, %zu chunks, .crsinfo entry %d%s, %zu start lines, %zu race records, grid list %d; mode %u, shell class %d\n", trackName.c_str(),
                    data.track.lengthMetres, data.track.chunks.size(), data.courseIndex, data.course.dirtCourse ? " (dirt)" : "", data.course.startLineDistances.size(),
                    sections, data.course.grid.count, data.constants.gameMode, data.constants.shellControlClass);
        if (!dumpPath.empty()) {
            std::string error;
            if (!sim::dev::LoadRaceFromDump(dumpPath, data.dump, error)) throw std::runtime_error(error);
            data.haveDump = true;
            CrossCheckDump(disc, trackName, data);
        }
        ApplyIntroHold(disc, data, raceOptions); // as the race load does (0x800299D8): the intro raises the start hold
        if (selfTest) return SelfTest(data, carCount) ? 1 : 0;
        if (replayCheckSeconds > 0) return ReplayCheck(disc, vol, data, carCount, raceOptions, replayCheckSeconds, replayOut, replayCheckAnalog) ? 1 : 0;
        if (!framesComparePath.empty() && ghostRecord) return FramesCompareGhostRecord(data, raceOptions, framesComparePath, framesMax) ? 1 : 0;
        if (!framesComparePath.empty() && captureRace && data.constants.gameMode == 6) // Time Trial / Rally: + the ghost's state
            return FramesCompareGhost(data, carCount, raceOptions, framesComparePath, PedalTable(disc), framesMax) ? 1 : 0;
        if (!framesComparePath.empty() && captureRace && data.constants.gameMode == 0 && std::getenv("GT2_SOUND_COMPARE")) // dev: the split screen's sound oracle
            return SoundCompareSplit(disc, vol, data, raceOptions, framesComparePath, framesMax) ? 1 : 0;
        if (!framesComparePath.empty() && replayFile && replayFile->GameMode() == 0) // a 2 player Battle's replay: both streams (split_race.h)
            return FramesCompareSplitReplay(data, raceOptions, framesComparePath, *replayFile, PedalTable(disc), framesMax) ? 1 : 0;
        if (!framesComparePath.empty())
            return FramesCompare(data, carCount, raceOptions, framesComparePath, replayFile ? &*replayFile : nullptr, PedalTable(disc), framesMax) ? 1 : 0;
        if (headlessSeconds > 0) return Headless(data, carCount, raceOptions, headlessSeconds);

        // The standalone race in its window (race_view.h; Esc quits).
        view.shotPath = shotPath;
        view.shotFrames = shotFrames;
        view.replay = replayFile ? &*replayFile : nullptr;
        view.replayOut = replayOut;
        GameWindow window("gt2game", windowWidth, windowHeight);
        window.SetPacing(!fast);
        if (!globalScript.empty()) window.AddScript(globalScript);
        for (const auto& s : anyShots) window.AddShot(s.first, s.second);
        if (captureRace && data.constants.gameMode == 0 && carCount >= 2) { // a 2 player Battle's race block: the split screen (split_race.h)
            SplitRaceConfig split;
            split.view = view;
            split.view.deterministic = split.view.deterministic || !globalScript.empty() || !shotPath.empty();
            Panels panels(window.Renderer(), disc, vol);
            RunSplitRace(window, &panels, disc, vol, data, raceOptions, split);
            return 0;
        }
        if (replayFile && replayFile->GameMode() == 0 && carCount >= 2) { // a 2 player Battle's replay: split screen, both streams
            SplitRaceConfig split;
            split.view = view;
            split.view.deterministic = split.view.deterministic || !globalScript.empty() || !shotPath.empty();
            split.replay = &*replayFile;
            Panels panels(window.Renderer(), disc, vol);
            RunSplitRace(window, &panels, disc, vol, data, raceOptions, split);
            return 0;
        }
        RunRaceView(window, nullptr, disc, vol, data, carCount, raceOptions, view, nullptr);
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

} // namespace gt2game
