#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <utility>

namespace gt2game {
struct DiscSelection {
    std::string folder, saveMode, flag;
};
inline DiscSelection DescribeDiscSelection(const std::string& key) {
    if (key == "combined-arcade") return {"combined", "arcade", "--combined-arcade"};
    if (key == "combined-simulation") return {"combined", "simulation", ""};
    return {key, key, ""};
}
inline bool ValidDiscSelection(const std::string& key) {
    return key == "arcade" || key == "simulation" || key == "combined-arcade" || key == "combined-simulation";
}
inline std::vector<std::string> InstalledDiscArguments(const std::string& executable,
    const std::filesystem::path& root, const std::filesystem::path& saves, const std::string& key) {
    const auto selection = DescribeDiscSelection(key);
    std::vector<std::string> args{executable, (root / selection.folder).string(),
        "--settings", (saves / selection.saveMode / "settings.txt").string(),
        "--card", (saves / selection.saveMode / "card1.mcd").string()};
    if (!selection.flag.empty()) args.push_back(selection.flag);
    return args;
}
// Positional-disc launches (including Web) keep the same image across combined-mode transitions.
// Only standard per-mode save paths change; explicitly chosen custom files stay untouched.
inline bool SwitchCombinedDiscArguments(std::vector<std::string>& args, const std::string& key,
                                       const std::filesystem::path& defaultSaves) {
    if (args.size() < 2 || (key != "combined-arcade" && key != "combined-simulation")) return false;
    const auto selection = DescribeDiscSelection(key);
    std::vector<std::string> next{args[0], args[1]};
    bool card = false, settings = false;
    for (size_t i = 2; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--combined-arcade") continue;
        next.push_back(arg);
        if ((arg == "--card" || arg == "--card2" || arg == "--settings") && i + 1 < args.size()) {
            card |= arg == "--card"; settings |= arg == "--settings";
            auto path = std::filesystem::path(args[++i]);
            const auto parent = path.parent_path();
            if (parent.filename() == "arcade" || parent.filename() == "simulation")
                path = parent.parent_path() / selection.saveMode / path.filename();
            next.push_back(path.string());
        }
    }
    if (!card) { next.push_back("--card"); next.push_back((defaultSaves / selection.saveMode / "card1.mcd").string()); }
    if (!settings) { next.push_back("--settings"); next.push_back((defaultSaves / selection.saveMode / "settings.txt").string()); }
    if (!selection.flag.empty()) next.push_back(selection.flag);
    args = std::move(next);
    return true;
}
} // namespace gt2game
