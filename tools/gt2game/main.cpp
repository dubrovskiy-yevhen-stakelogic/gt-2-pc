#include "game_main.h"
#include "disc_launch.h"
#include "game_window.h"
#include "pc_overlay.h"
#include "../gt2mac/launch_protocol.h"
#include "platform/os/paths.h"
#include "game/shell/shared_vr_settings.h"
#include "graphics_options.h"
#ifndef __EMSCRIPTEN__
#include "gt2view/vk_capabilities.h"
#endif
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef __EMSCRIPTEN__
extern "C" void gt2_web_game_exit(int code);
#endif

int main(int argc, char** argv) {
    using namespace gt2game;
#ifdef __APPLE__
    const char* launcher = std::getenv(gt2mac::kLauncherEnvironment);
    SetDiscImportAvailable(launcher && std::string(launcher) == "1");
#elif defined(__linux__) && !defined(__ANDROID__)
    const char* launcher = std::getenv("GT2_LINUX_LAUNCHER");
    SetDiscImportAvailable(launcher && std::string(launcher) == "1");
#endif
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc > 1 && std::string(argv[1]) == "--help") {
        std::puts("gt2game [--vr] [--data-root installed-folder] [--save-root saves-folder]\n"
                  "Plays the optional PlayStation intro, then shows installed discs.\n"
                  "Flat mode starts fullscreen. Alt+Enter toggles windowed mode; --windowed starts in a window.\n"
                  "--diagnose reports the Vulkan loader, GPUs and the renderer path this machine gets.\n"
                  "Advanced tools: gt2game <disc-folder-or-image> [options]");
        return 0;
    }
#ifdef _WIN32
    // Vulkan is a load-time DLL: report a missing loader instead of failing inside the loader.
    if (!gt2view::VulkanLoaderInstalled()) {
        std::fputs("Startup failed: the Vulkan loader (vulkan-1.dll) was not found. Install or update the graphics driver, then run "
                   "gt2game --diagnose.\n", stderr);
        return 1;
    }
#endif
    if (argc > 1 && !std::string(argv[1]).starts_with("--")) {
        std::vector<std::string> args(argv, argv + argc);
        int result;
        for (;;) {
            std::vector<char*> pointers;
            for (auto& arg : args) pointers.push_back(arg.data());
            result = GameMain(int(pointers.size()), pointers.data());
            if (result != 0 || !TakeGameChangeRequest()) break;
            const auto next = TakeGameChangePreference();
            const bool direct = TakeGameChangeDirect();
            if (!direct || !SwitchCombinedDiscArguments(args, next, gt2::os::SavesDir())) break;
            SetFakePadScript({}); SetFakePad2Script({});
        }
        ReleaseRetainedWindow();
#ifdef __EMSCRIPTEN__
        gt2_web_game_exit(result);
#endif
        return result == 0 && TakeDiscImportRequest() ? gt2mac::kImportDiscExitCode : result;
    }
    int result = 0;
    try {
        auto root = gt2::os::DataRoot();
        std::filesystem::path saves;
        bool vr = false, deterministic = false, sound = true, diagnose = false;
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
            else if (arg == "--diagnose") diagnose = true;
            else if (arg == "--picker-script") pickerScript = value();
            else if (arg == "--picker-shot") pickerShot = value();
            else if (arg != "--player" && arg != "--flat") {
                forwarded.push_back(arg);
                if (arg == "--vr") vr = true;
                if (arg == "--xr-deterministic") deterministic = true;
                if (arg == "--no-sound") sound = false;
                if (arg == "--windowed" || arg == "--window") SetWindowedMode(true);
            }
        }
        root = std::filesystem::absolute(root);
        if (saves.empty()) saves = root / "saves";
#ifndef __EMSCRIPTEN__
        if (diagnose) {
            // Discussion #77: runs before any Vulkan call, so it works where the loader or GPU is unusable.
            std::fputs(gt2view::VulkanDiagnosticReport().c_str(), stdout);
            return 0;
        }
#else
        (void)diagnose;
#endif
        std::string preferred = "arcade";
        std::ifstream(saves / "last-disc.txt") >> preferred;
        if (!ValidDiscSelection(preferred))
            preferred = "arcade";
        gt2::shell::InitializeSharedVrSettings(saves,
            "units=kmh\nframe_rate=display\ndraw_distance=all\ntexture_filter=smooth\n", "");
        const RecommendedMemory memory = ApplyRecommendedMemory((saves / "settings.txt").string()); // before the overlay
        if (memory.memoryTooLow) {
            std::fprintf(stderr, "GT2 cannot start: not enough video memory (%s).\n", memory.reason.c_str());
            return 1;
        }
        if (memory.applied) std::printf("graphics: Memory Use set to Low (%s)\n", memory.reason.c_str());
        LoadOverlaySettings((saves / DescribeDiscSelection(preferred).saveMode / "settings.txt").string());
        SetWindowNoFocus(deterministic || !pickerScript.empty() || !pickerShot.empty());
        bool firstLaunch = true;
        bool directLaunch = false;
        auto installed = [&](const std::string& key) {
            const auto [folder, saveMode, modeFlag] = DescribeDiscSelection(key);
            (void)saveMode; (void)modeFlag;
            return std::filesystem::exists(root / folder / "disc.raw2352");
        };
        for (;;) {
            std::string mode;
            if (directLaunch && installed(preferred)) mode = preferred; // combined title <-> Arcade menus
            else mode = SelectGameDisc(root.string(), preferred, vr, deterministic, pickerScript, pickerShot, sound, firstLaunch);
            directLaunch = false;
            if (mode.empty()) break;
            firstLaunch = false;
            preferred = mode;
            gt2::shell::WritePreferences(saves / "last-disc.txt", mode + "\n");
            auto args = InstalledDiscArguments(argv[0], root, saves, mode);
            args.insert(args.end(), forwarded.begin(), forwarded.end());
            std::vector<char*> pointers;
            for (auto& arg : args) pointers.push_back(arg.data());
            result = GameMain(int(pointers.size()), pointers.data());
            if (result != 0 || !TakeGameChangeRequest()) break;
            if (const std::string next = TakeGameChangePreference(); !next.empty()) preferred = next;
            directLaunch = TakeGameChangeDirect();
            SetFakePadScript({}); SetFakePad2Script({});
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Startup failed: %s\n", e.what());
        result = 1;
    }
    ReleaseRetainedWindow();
    if (result == 0 && TakeDiscImportRequest()) return gt2mac::kImportDiscExitCode;
    return result;
}
