#include "game_main.h"
#include "game_window.h"
#include "pc_overlay.h"
#include "platform/os/paths.h"
#include "game/shell/shared_vr_settings.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
    using namespace gt2game;
    if (argc > 1 && !std::string(argv[1]).starts_with("--")) return GameMain(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::puts("gt2game [--vr] [--data-root installed-folder] [--save-root saves-folder]\n"
                  "Plays the optional PlayStation intro, then shows installed discs.\n"
                  "Advanced tools: gt2game <disc-folder-or-image> [options]");
        return 0;
    }
    int result = 0;
    try {
        auto root = gt2::os::DataRoot();
        std::filesystem::path saves;
        bool vr = false, deterministic = false, sound = true;
        std::string pickerScript, pickerShot;
        std::vector<std::string> forwarded;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            auto value = [&]() -> std::string {
                if (++i >= argc) throw std::runtime_error(arg + " needs a value");
                return argv[i];
            };
            if (arg == "--data-root") root = value();
            else if (arg == "--save-root") saves = value();
            else if (arg == "--picker-script") pickerScript = value();
            else if (arg == "--picker-shot") pickerShot = value();
            else if (arg != "--player" && arg != "--flat") {
                forwarded.push_back(arg);
                if (arg == "--vr") vr = true;
                if (arg == "--xr-deterministic") deterministic = true;
                if (arg == "--no-sound") sound = false;
            }
        }
        root = std::filesystem::absolute(root);
        if (saves.empty()) saves = root / "saves";
        std::string preferred = "arcade";
        std::ifstream(saves / "last-disc.txt") >> preferred;
        if (preferred != "arcade" && preferred != "simulation") preferred = "arcade";
        gt2::shell::InitializeSharedVrSettings(saves,
            "units=kmh\nframe_rate=display\ndraw_distance=all\ntexture_filter=smooth\n", "");
        LoadOverlaySettings((saves / preferred / "settings.txt").string());
        SetWindowNoFocus(deterministic || !pickerScript.empty() || !pickerShot.empty());
        bool firstLaunch = true;
        for (;;) {
            const auto mode = SelectGameDisc(root.string(), preferred, vr, deterministic, pickerScript, pickerShot, sound, firstLaunch);
            if (mode.empty()) break;
            firstLaunch = false;
            preferred = mode;
            gt2::shell::WritePreferences(saves / "last-disc.txt", mode + "\n");
            std::vector<std::string> args{argv[0], (root / mode).string(),
                "--settings", (saves / mode / "settings.txt").string(),
                "--card", (saves / mode / "card1.mcd").string()};
            args.insert(args.end(), forwarded.begin(), forwarded.end());
            std::vector<char*> pointers;
            for (auto& arg : args) pointers.push_back(arg.data());
            result = GameMain(int(pointers.size()), pointers.data());
            if (result != 0 || !TakeGameChangeRequest()) break;
            SetFakePadScript({}); SetFakePad2Script({});
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Startup failed: %s\n", e.what());
        result = 1;
    }
    ReleaseRetainedWindow();
    return result;
}
