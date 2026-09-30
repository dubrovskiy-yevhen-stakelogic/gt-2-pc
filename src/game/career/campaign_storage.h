#pragma once
#include "game/career/career_state.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace gt2::career::campaign {
namespace fs = std::filesystem;

inline void WriteVerified(const fs::path& path, const std::vector<uint8_t>& bytes) {
#ifdef _WIN32
    auto* file = _wfopen(path.c_str(), L"wb");
#else
    auto* file = std::fopen(path.c_str(), "wb");
#endif
    if (!file) throw std::runtime_error("Cannot create campaign backup. No changes made.");
    bool ok = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    ok = std::fflush(file) == 0 && ok;
#ifdef _WIN32
    ok = _commit(_fileno(file)) == 0 && ok;
#else
    ok = fsync(fileno(file)) == 0 && ok;
#endif
    ok = std::fclose(file) == 0 && ok;
    if (!ok || ReadFileBytes(path.string()) != bytes)
        throw std::runtime_error("Campaign backup verification failed. No changes made.");
}

inline bool SyncDirectory(const fs::path& path) {
#ifdef _WIN32
    (void)path; return true; // MoveFileEx uses WRITE_THROUGH below.
#else
    const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool ok = fsync(fd) == 0;
    close(fd); return ok;
#endif
}

inline fs::path BackupRoot(const std::string& path) { return fs::path(path + ".campaign-backups"); }
inline int BackupNumber(const fs::path& path) {
    const auto name = path.filename().string();
    if (name.size() != 8 || name.find_first_not_of("0123456789") != std::string::npos) return 0;
    return std::stoi(name);
}
inline fs::path Previous(const std::string& path) {
    const auto root = BackupRoot(path);
    fs::path latest;
    int number = 0;
    if (fs::exists(root)) for (const auto& entry : fs::directory_iterator(root)) {
        const int n = BackupNumber(entry.path());
        if (n > number && entry.is_directory() && fs::is_regular_file(entry.path() / "ready")) {
            latest = entry.path(); number = n;
        }
    }
    if (latest.empty()) throw std::runtime_error("No previous campaign backup is available.");
    return latest;
}
inline CareerSave ReadPrevious(const std::string& path) {
    auto saved = LoadCareer((Previous(path) / "campaign.sav").string());
    if (!saved.CrcOk()) throw std::runtime_error("Backup is damaged. No changes made.");
    return saved;
}

// Preserve the full on-disk card AND unsaved in-memory progress before replacing
// only the career entry. Every operation gets an immutable numbered backup.
// The caller updates live state only after this function returns successfully.
inline void Replace(const std::string& path, const CareerSave& before, const CareerSave& candidate) {
    if (path.empty()) throw std::runtime_error("No campaign save path.");
    const fs::path destination = fs::absolute(path);
    const bool exists = fs::exists(destination);
    const auto old = exists ? ReadFileBytes(destination.string()) : std::vector<uint8_t>{};
    auto extension = destination.extension().string();
    for (auto& c : extension) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    std::vector<uint8_t> replacement;
    if (extension == ".mcd") {
        replacement = exists ? old : FormatMemoryCard();
        StoreCareerOnCard(replacement, candidate); // Invalid/full cards fail before any backup or replacement.
    } else {
        if (exists && !LoadCareerSaveFile(old).CrcOk()) throw std::runtime_error("Save is damaged. No changes made.");
        replacement = BuildCareerSaveFile(candidate);
    }
    const auto snapshot = BuildCareerSaveFile(before);
    const auto root = BackupRoot(destination.string());
    fs::create_directories(root);
    int number = 0;
    for (const auto& entry : fs::directory_iterator(root)) number = (std::max)(number, BackupNumber(entry.path()));
    fs::path backup;
    do {
        if (++number > 99999999) throw std::runtime_error("Campaign backup limit reached.");
        char name[16]; std::snprintf(name, sizeof(name), "%08d", number);
        backup = root / name;
    } while (!fs::create_directory(backup));
    if (exists) WriteVerified(backup / "original.bin", old);
    WriteVerified(backup / "campaign.sav", snapshot);
    const auto reread = LoadCareer((backup / "campaign.sav").string());
    if (!reread.CrcOk() || std::memcmp(&reread.state, &before.state, sizeof(CareerState)))
        throw std::runtime_error("Campaign backup is invalid. No changes made.");
    const auto temporary = backup / "replacement.tmp";
    WriteVerified(temporary, replacement);
    const auto verified = LoadCareer(temporary.string());
    if (!verified.CrcOk() || std::memcmp(&verified.state, &candidate.state, sizeof(CareerState)))
        throw std::runtime_error("New campaign verification failed. No changes made.");
    WriteVerified(backup / "ready", {'O', 'K'});
    if (!SyncDirectory(backup) || !SyncDirectory(root) || !SyncDirectory(destination.parent_path()))
        throw std::runtime_error("Cannot secure campaign backup. No changes made.");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace campaign. Original retained.");
#else
    fs::rename(temporary, destination);
    // Commit point: never throw after replacement, so live state stays in sync.
    SyncDirectory(destination.parent_path());
#endif
}
}
