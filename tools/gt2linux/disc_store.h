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
    for (const char* mode : {"arcade", "simulation", "combined"}) if (HasDisc(root, mode)) return true;
    return false;
}

inline void PublishDisc(const std::filesystem::path& root, const std::string& mode,
                        const std::filesystem::path& prepared) {
    if (mode != "arcade" && mode != "simulation" && mode != "combined") throw std::runtime_error("Unsupported disc type");
    const auto target = root / mode;
    if (std::filesystem::exists(std::filesystem::symlink_status(target)))
        throw std::runtime_error("This disc is already installed. Existing data was kept.");
    if (!std::filesystem::is_regular_file(prepared / "disc.raw2352") ||
        !std::filesystem::is_directory(prepared / "assets"))
        throw std::runtime_error("Disc extraction is incomplete. Existing data was kept.");
    // The launcher holds the application lock; staging lives on this filesystem.
    std::filesystem::rename(prepared, target);
}
}
