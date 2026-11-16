#include "support_log.h"
#include "gt2_version.h"
#include "gt2view/vk_capabilities.h"

#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

#if defined(__EMSCRIPTEN__) || defined(__ANDROID__)
namespace gt2game {
void StartSupportLog(const std::filesystem::path&) {}
void LogLine(const std::string&) {}
const std::string& SupportLogPath() { static const std::string none; return none; }
} // namespace gt2game
#else
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace gt2game {
namespace {
std::string g_path;
bool g_console = false;

bool ConsoleAttached() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}
} // namespace

void StartSupportLog(const std::filesystem::path& savesDir) {
    if (!g_path.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(savesDir, error);
    const auto path = savesDir / "gt2.log";
    if (std::filesystem::file_size(path, error) > 2 * 1024 * 1024 && !error) {
        std::filesystem::remove(savesDir / "gt2.old.log", error);
        std::filesystem::rename(path, savesDir / "gt2.old.log", error);
    }
    std::ofstream out(path, std::ios::app);
    if (!out) return; // a read-only folder must not take the game down
    g_path = path.string();
    std::time_t now = std::time(nullptr);
    char when[32] = {};
    if (const std::tm* local = std::localtime(&now)) std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", local);
    out << "=== GT2 " << gt2::kVersion << " started " << when << " ===\n";
#if defined(__APPLE__)
    out << "platform: macOS\n";
#elif defined(_WIN32)
    out << "platform: Windows\n";
#else
    out << "platform: Linux\n";
#endif
    out << gt2view::VulkanDiagnosticReport();
    out.flush();
    g_console = ConsoleAttached();
    if (!g_console) {
        // The renderer's own messages go to stdout: keep them where a user can send them.
        std::freopen(g_path.c_str(), "a", stdout);
        std::freopen(g_path.c_str(), "a", stderr);
        std::setvbuf(stdout, nullptr, _IONBF, 0);
    }
}

void LogLine(const std::string& line) {
    if (g_path.empty()) return;
    std::ofstream out(g_path, std::ios::app);
    out << line << "\n";
    if (g_console) std::fprintf(stdout, "%s\n", line.c_str()); // the log replaces nothing on a terminal
}

const std::string& SupportLogPath() { return g_path; }

} // namespace gt2game
#endif
