#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>

namespace gt2linux {
inline constexpr int kImportDiscExitCode = 42;
inline constexpr const char* kLauncherEnvironment = "GT2_LINUX_LAUNCHER";

inline bool HasDisc(const std::filesystem::path& root, const std::string& mode) {
    return std::filesystem::is_regular_file(root / mode / "disc.raw2352") &&
           std::filesystem::is_directory(root / mode / "assets");
}
inline bool HasAnyDisc(const std::filesystem::path& root) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error))
        if (std::filesystem::is_regular_file(entry.path() / "disc.raw2352") &&
            std::filesystem::is_directory(entry.path() / "assets")) return true;
    return false;
}

inline void PublishDisc(const std::filesystem::path& root, const std::string& folder,
                        const std::filesystem::path& prepared) {
    // Install folders are "<mode>" or "<mode>-<dataset suffix>" (gt2formats/dataset.h).
    const auto dash = folder.find('-');
    const std::string mode = folder.substr(0, dash);
    const bool valid = (mode == "arcade" || mode == "simulation" || mode == "combined") &&
        (dash == std::string::npos || dash + 1 < folder.size()) &&
        folder.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
    if (!valid) throw std::runtime_error("Unsupported disc type");
    const auto target = root / folder;
    if (std::filesystem::exists(std::filesystem::symlink_status(target)))
        throw std::runtime_error("This disc is already installed. Existing data was kept.");
    if (!std::filesystem::is_regular_file(prepared / "disc.raw2352") ||
        !std::filesystem::is_directory(prepared / "assets"))
        throw std::runtime_error("Disc extraction is incomplete. Existing data was kept.");
    // The launcher holds the application lock; staging lives on this filesystem.
    std::filesystem::rename(prepared, target);
}
}
