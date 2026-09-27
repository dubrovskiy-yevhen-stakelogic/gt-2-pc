#include "../tools/gt2linux/disc_store.h"
#include <chrono>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
static void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class F> static void Reject(F action) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    Check(failed, "unsafe import accepted");
}
int main() {
    const auto root = fs::temp_directory_path() / ("gt2-install-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directories(root / "saves");
        std::ofstream(root / "saves/card1.mcd") << "saved career";
        const auto stage = root / ".import-test/disc";
        fs::create_directories(stage / "assets");
        Reject([&] { gt2linux::PublishDisc(root, "arcade", stage); });
        Check(!fs::exists(root / "arcade"), "partial extraction published");
        std::ofstream(stage / "disc.raw2352") << "synthetic image";
        Reject([&] { gt2linux::PublishDisc(root, "../outside", stage); });
        gt2linux::PublishDisc(root, "arcade", stage);
        Check(gt2linux::HasDisc(root, "arcade"), "first disc missing");
        fs::create_directories(stage / "assets");
        std::ofstream(stage / "disc.raw2352") << "second synthetic image";
        Reject([&] { gt2linux::PublishDisc(root, "arcade", stage); });
        Check(fs::exists(stage), "duplicate import consumed source");
        gt2linux::PublishDisc(root, "simulation", stage);
        Check(gt2linux::HasDisc(root, "arcade") && gt2linux::HasDisc(root, "simulation"), "adding second disc removed first");
        std::string save;
        std::getline(std::ifstream(root / "saves/card1.mcd"), save);
        Check(save == "saved career", "import changed saved game");
        fs::remove_all(root);
        std::cout << "Linux disc import checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::error_code ignored; fs::remove_all(root, ignored);
        return 1;
    }
}
