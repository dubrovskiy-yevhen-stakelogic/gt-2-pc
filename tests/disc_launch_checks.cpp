#include "../tools/gt2game/disc_launch.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string Value(const std::vector<std::string>& args, const std::string& key) {
    const auto it = std::find(args.begin(), args.end(), key);
    return it != args.end() && it + 1 != args.end() ? *(it + 1) : "";
}
}
int main() { try {
    using namespace gt2game;
    namespace fs = std::filesystem;
    for (const std::string mode : {"arcade", "simulation"}) {
        const auto standalone = InstalledDiscArguments("gt2game", "data", "saves", mode);
        const auto combined = InstalledDiscArguments("gt2game", "data", "saves", "combined:" + mode);
        Check(fs::path(combined[1]) == fs::path("data") / "combined", "Quest combined rows must open the installed combined image");
        Check(fs::path(standalone[1]) == fs::path("data") / mode, "standalone disc paths are unchanged");
        Check(Value(combined, "--card") == Value(standalone, "--card"), "combined mode shares the standalone card");
        Check(Value(combined, "--settings") == Value(standalone, "--settings"), "combined mode shares standalone settings");
        Check((std::count(combined.begin(), combined.end(), "--combined-arcade") == 1) == (mode == "arcade"), "only combined Arcade requests the Arcade flow");
    }
    // A modified dataset has its own install folder and its own card, but the mode's settings stay shared.
    const auto aspec = InstalledDiscArguments("gt2game", "data", "saves", "simulation-aspec");
    Check(fs::path(aspec[1]) == fs::path("data") / "simulation-aspec", "modified dataset opens its own folder");
    Check(fs::path(Value(aspec, "--card")) == fs::path("saves/simulation-aspec/card1.mcd"), "modified dataset uses its own card folder");
    Check(fs::path(Value(aspec, "--settings")) == fs::path("saves/simulation/settings.txt"), "modified dataset shares the mode settings");
    const auto aspecCombined = InstalledDiscArguments("gt2game", "data", "saves", "combined-aspec:simulation");
    Check(fs::path(aspecCombined[1]) == fs::path("data") / "combined-aspec", "modified combined disc uses its own folder");
    Check(fs::path(Value(aspecCombined, "--card")) == fs::path("saves/simulation-aspec/card1.mcd"), "combined and standalone of one dataset share a card");
    const auto aspecArcade = InstalledDiscArguments("gt2game", "data", "saves", "combined-aspec:arcade");
    Check(fs::path(Value(aspecArcade, "--card")) == fs::path("saves/arcade-aspec/card1.mcd"), "combined Arcade uses the arcade dataset card");
    Check(ValidDiscSelection("combined-arcade") && ValidDiscSelection("combined:simulation"), "legacy combined keys still load");
    Check(!ValidDiscSelection("combined") && !ValidDiscSelection("simulation:arcade") && !ValidDiscSelection("nonsense"),
          "incomplete or unknown keys are rejected");

    for (const std::string folder : {"combined", "combined-plus", "combined-aspec"}) {
        std::string current = folder + ":simulation";
        for (int round = 0; round < 3; ++round) {
            for (const std::string flow : {"arcade", "simulation"}) {
                current = ResolveInstalledDiscChange(current, "combined-" + flow, true);
                const auto installed = InstalledDiscArguments("gt2game", "data", "saves", current);
                Check(fs::path(installed[1]) == fs::path("data") / folder, "installed transition must retain the dataset");
                Check(fs::path(Value(installed, "--card")) == fs::path("saves") / Suffixed(flow, InstallSuffixOf(folder)) / "card1.mcd",
                      "installed transition must retain the dataset card");
            }
        }
        Check(ResolveInstalledDiscChange(current, "", false) == current, "empty picker preference retains current dataset");
        Check(ResolveInstalledDiscChange(current, "combined:arcade", false) == "combined:arcade", "explicit retail selection is not rewritten");
        Check(ResolveInstalledDiscChange(current, "combined-other:arcade", true) == "combined-other:arcade", "explicit dataset selection is not rewritten");
    }
    std::vector<std::string> args{"gt2game", "/data/disc.bin", "--windowed", "--card", "/saves/simulation/card1.mcd",
        "--settings", "/saves/simulation/settings.txt", "--card2", "/saves/simulation/card2.mcd"};
    for (int round = 0; round < 3; ++round) {
        Check(SwitchCombinedDiscArguments(args, "combined-arcade", "/saves"), "Simulation title requests Arcade");
        Check(args[1] == "/data/disc.bin", "Web switches flow on the same mounted image");
        Check(fs::path(Value(args, "--card")) == fs::path("/saves/arcade/card1.mcd"), "Web switches memory card slot");
        Check(fs::path(Value(args, "--card2")) == fs::path("/saves/arcade/card2.mcd"), "second card follows its mode");
        Check(std::count(args.begin(), args.end(), "--combined-arcade") == 1, "Arcade flag occurs once");
        Check(SwitchCombinedDiscArguments(args, "combined-simulation", "/saves"), "Arcade returns to Simulation title");
        Check(std::count(args.begin(), args.end(), "--combined-arcade") == 0, "return clears Arcade flow");
        Check(fs::path(Value(args, "--settings")) == fs::path("/saves/simulation/settings.txt"), "return restores Simulation settings");
    }
    const auto before = args;
    Check(!SwitchCombinedDiscArguments(args, "arcade", "/saves") && args == before, "ordinary game-change requests do not loop the positional launcher");
    args = {"gt2game", "disc.bin", "--card", "custom.mcd", "--settings", "custom.ini"};
    SwitchCombinedDiscArguments(args, "combined-arcade", "saves");
    Check(Value(args, "--card") == "custom.mcd" && Value(args, "--settings") == "custom.ini", "explicit custom files are preserved");
    args = {"gt2game", "disc.bin"};
    SwitchCombinedDiscArguments(args, "combined-simulation", "saves");
    Check(fs::path(Value(args, "--card")) == fs::path("saves/simulation/card1.mcd"), "positional launch supplies a mode-specific default card");

    // A modified combined install keeps its suffix across the title <-> Arcade flow switch.
    args = {"gt2game", "/data/combined-aspec", "--card", "/saves/simulation-aspec/card1.mcd",
        "--settings", "/saves/simulation/settings.txt"};
    Check(SwitchCombinedDiscArguments(args, "combined-arcade", "/saves"), "modified combined disc switches to Arcade");
    Check(fs::path(Value(args, "--card")) == fs::path("/saves/arcade-aspec/card1.mcd"), "the dataset suffix survives the switch");
    Check(fs::path(Value(args, "--settings")) == fs::path("/saves/arcade/settings.txt"), "settings follow the flow only");
    std::cout << "Combined disc paths, dataset suffixes and repeated mode transitions passed\n";
} catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; } }
