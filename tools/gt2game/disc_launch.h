#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace gt2game {

// An install folder is the mode, plus "-<dataset suffix>" for a modified dataset: "simulation", "simulation-aspec",
// "combined-plus". The suffix comes from the car-table fingerprint (gt2formats/dataset.h), so a mod's saves and its
// installed data stay together while the retail disc keeps the original folders (docs/MOD-SUPPORT.md).
inline std::string InstallModeOf(std::string_view folder) {
    return std::string(folder.substr(0, folder.find('-')));
}
inline std::string InstallSuffixOf(std::string_view folder) {
    const auto dash = folder.find('-');
    return dash == std::string_view::npos ? std::string() : std::string(folder.substr(dash + 1));
}
inline bool IsDiscMode(std::string_view mode) {
    return mode == "arcade" || mode == "simulation" || mode == "combined";
}
inline std::string Suffixed(std::string_view mode, std::string_view suffix) {
    return suffix.empty() ? std::string(mode) : std::string(mode) + "-" + std::string(suffix);
}

// A picker row: `folder` is the installed disc, `saveMode` the settings folder (arcade/simulation), `saveFolder` the
// memory-card folder (the install folder for standalone discs, "simulation-aspec" for a modified combined disc).
struct DiscSelection {
    std::string folder, saveMode, saveFolder, flag;
};

// last-disc.txt kept the pre-dataset keys; a combined flow is now "<install>:arcade" / "<install>:simulation".
inline std::string NormalizeDiscSelection(std::string key) {
    if (key == "combined-arcade") return "combined:arcade";
    if (key == "combined-simulation") return "combined:simulation";
    return key;
}

inline DiscSelection DescribeDiscSelection(const std::string& rawKey) {
    const std::string key = NormalizeDiscSelection(rawKey);
    const auto colon = key.rfind(':');
    if (colon != std::string::npos) {
        const std::string folder = key.substr(0, colon), flow = key.substr(colon + 1);
        if (InstallModeOf(folder) != "combined" || (flow != "arcade" && flow != "simulation")) return {};
        return {folder, flow, Suffixed(flow, InstallSuffixOf(folder)), flow == "arcade" ? "--combined-arcade" : ""};
    }
    if (!IsDiscMode(InstallModeOf(key)) || InstallModeOf(key) == "combined") return {};
    return {key, InstallModeOf(key), key, ""};
}

inline bool ValidDiscSelection(const std::string& key) { return !DescribeDiscSelection(key).folder.empty(); }

inline std::vector<std::string> InstalledDiscArguments(const std::string& executable,
    const std::filesystem::path& root, const std::filesystem::path& saves, const std::string& key) {
    const auto selection = DescribeDiscSelection(key);
    std::vector<std::string> args{executable, (root / selection.folder).string(),
        "--settings", (saves / selection.saveMode / "settings.txt").string(),
        "--card", (saves / selection.saveFolder / "card1.mcd").string()};
    if (!selection.flag.empty()) args.push_back(selection.flag);
    return args;
}

// Positional-disc launches (including Web) keep the same image across combined-mode transitions.
// Only standard save paths change; explicitly chosen custom files stay untouched.
inline bool SwitchCombinedDiscArguments(std::vector<std::string>& args, const std::string& rawKey,
                                       const std::filesystem::path& defaultSaves) {
    if (args.size() < 2) return false;
    const std::string key = NormalizeDiscSelection(rawKey);
    const auto colon = key.rfind(':');
    if (colon == std::string::npos || InstallModeOf(key.substr(0, colon)) != "combined") return false;
    const std::string flow = key.substr(colon + 1);
    if (flow != "arcade" && flow != "simulation") return false;

    // The dataset suffix follows the running install: the second argument for installed discs, the current card
    // folder for the web's positional image. Without either, the canonical folder is used.
    std::string suffix;
    auto suffixOf = [&](const std::filesystem::path& path, bool parent) {
        const std::string name = (parent ? path.parent_path() : path).filename().string();
        if (!IsDiscMode(InstallModeOf(name))) return false;
        suffix = InstallSuffixOf(name);
        return true;
    };
    if (!suffixOf(args[1], false))
        for (size_t i = 2; i + 1 < args.size(); ++i)
            if (args[i] == "--card" || args[i] == "--card2") suffixOf(args[i + 1], true);
    const std::string saveFolder = Suffixed(flow, suffix);

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
            if (IsDiscMode(InstallModeOf(parent.filename().string())))
                path = parent.parent_path() / (arg == "--settings" ? flow : saveFolder) / path.filename();
            next.push_back(path.string());
        }
    }
    if (!card) { next.push_back("--card"); next.push_back((defaultSaves / saveFolder / "card1.mcd").string()); }
    if (!settings) { next.push_back("--settings"); next.push_back((defaultSaves / flow / "settings.txt").string()); }
    if (flow == "arcade") next.push_back("--combined-arcade");
    args = std::move(next);
    return true;
}

} // namespace gt2game
