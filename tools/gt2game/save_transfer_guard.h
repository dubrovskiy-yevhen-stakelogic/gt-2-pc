#pragma once
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace gt2game {
// A separate lock survives atomic replacement of the actual memory card.
class SaveTransferGuard {
public:
    SaveTransferGuard(const std::string& first, const std::string& second) {
#ifdef _WIN32
        std::vector<std::filesystem::path> dirs;
        try {
            for (const auto& card : {first, second}) {
                if (card.empty()) continue;
                auto dir = std::filesystem::absolute(card).parent_path().lexically_normal();
                bool duplicate = false;
                std::filesystem::create_directories(dir);
                for (const auto& old : dirs) if (std::filesystem::equivalent(old, dir)) duplicate = true;
                if (duplicate) continue;
                const auto lock = dir / ".transfer.lock";
                HANDLE file = CreateFileW(lock.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (file == INVALID_HANDLE_VALUE) {
                    throw std::runtime_error("Save folder is busy. Close the other game or finish the save transfer first: " + dir.string());
                }
                files_.push_back(file);
                dirs.push_back(dir);
            }
        } catch (...) {
            for (auto file : files_) CloseHandle(file);
            throw;
        }
#else
        (void)first; (void)second; // Android holds the app-wide lock in GT2Activity.
#endif
    }
    ~SaveTransferGuard() {
#ifdef _WIN32
        for (auto file : files_) CloseHandle(file);
#endif
    }
    SaveTransferGuard(const SaveTransferGuard&) = delete;
    SaveTransferGuard& operator=(const SaveTransferGuard&) = delete;
private:
#ifdef _WIN32
    std::vector<HANDLE> files_;
#endif
};
}
