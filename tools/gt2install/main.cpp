#include "gt2vfs/disc_image.h"
#include "gt2vfs/gtfs.h"
#include "gt2formats/exe_profile.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
static fs::path SafePath(const fs::path& root, const std::string& name) {
    const fs::path relative(name);
    if (relative.empty() || relative.is_absolute() || relative.has_root_name() || name.find(':') != std::string::npos || name.find('\\') != std::string::npos)
        throw std::runtime_error("unsafe asset path");
    for (const auto& part : relative) if (part == ".." || part == ".") throw std::runtime_error("unsafe asset traversal");
    return root / relative;
}
int main(int argc, char** argv) {
    try {
        if (argc < 3 || (std::string(argv[1]) != "inspect" && std::string(argv[1]) != "extract")) {
            std::cerr << "usage: gt2install inspect <disc> | extract <disc> <new-directory>\n"; return 2;
        }
        gt2::DiscImage disc(argv[2]);
        const auto& profile = gt2::ProfileOf(disc); // rejects unsupported executables by SHA-1
        const char* mode = profile.arcade ? "arcade" : "simulation";
        gt2::GtfsVolume volume(disc);
        if (std::string(argv[1]) == "inspect") {
            std::cout << "{\"mode\":\"" << mode << "\",\"exeSha1\":\"" << profile.exeSha1 << "\",\"assets\":" << volume.Files().size() << "}\n";
            return 0;
        }
        if (argc != 4) throw std::runtime_error("extract requires a new destination directory");
        const fs::path target = fs::absolute(argv[3]);
        if (fs::exists(target)) throw std::runtime_error("destination already exists; refusing to overwrite");
        // Preflight every archive name before creating any files.
        for (const auto& e : volume.Files()) (void)SafePath(target / "assets", e.path);
        fs::create_directories(target / "assets");
        fs::copy_file(argv[2], target / "disc.raw2352"); // preserves XA sectors and movie data exactly
        size_t count = 0;
        for (const auto& e : volume.Files()) {
            const auto path = SafePath(target / "assets", e.path);
            fs::create_directories(path.parent_path());
            std::vector<uint8_t> bytes;
            try { bytes = volume.Read(e); }
            catch (const std::exception& ex) { throw std::runtime_error(e.path + ": " + ex.what()); }
            std::ofstream out(path, std::ios::binary);
            out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size())); out.flush();
            if (!out) throw std::runtime_error("cannot extract " + e.path);
            ++count;
        }
        std::cout << "Extracted " << count << " assets for " << mode << "; original media retained for XA/audio/video.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "gt2install: " << e.what() << '\n'; return 1; }
}
