#pragma once
// The support log (discussion #77): one file in the saves folder with the Vulkan probe, the chosen renderer path and
// the graphics settings, so a user reporting a graphics problem can send it without running anything technical.
#include <filesystem>
#include <string>

namespace gt2game {

// Opens logs/gt2.log for append (rotating a >2 MiB one to gt2.old.log) and writes the session header with the
// Vulkan diagnose report. When the game was not started from a terminal, stdout and stderr are appended to the same
// file too, so a desktop/Finder launch keeps the renderer's messages. Safe to call again: only the first call opens.
void StartSupportLog(const std::filesystem::path& savesDir);
// Appends one line; a no-op before StartSupportLog or when the folder is not writable.
void LogLine(const std::string& line);
// The open log file, empty when no log could be opened.
const std::string& SupportLogPath();

} // namespace gt2game
