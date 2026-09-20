#include "game/career/cheats.h"
#include "game/career/results.h"
#include "gt2vfs/gtfs.h"
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
using namespace gt2;
using namespace gt2::career;
static void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
int main(int argc, char** argv) {
    try {
        CareerSave before; before.state = NewCareer({}); before.header.resize(0x200);
        before.header[0] = 'S'; before.header[1] = 'C';
        before.state.licences[2][3].times[0].time[0] = 123456;
        auto candidate = before;
        ApplyCheat(candidate, nullptr, Cheat::GoldLicences);
        for (int i = 0; i < 6; ++i) Check(LicenceHeld(candidate.state, i), "all six licences available");
        Check(candidate.state.licences[2][3].times[0].time[0] == 123456, "licence times preserved");
        ApplyCheat(candidate, nullptr, Cheat::MaxCredits);
        Check(candidate.state.garage.money == kMoneyLimit, "credits respect original limit");
        Check(candidate.state.garage.count == before.state.garage.count, "existing garage retained");
        const auto dir = std::filesystem::temp_directory_path() / ("gt2-cheat-check-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        Check(std::filesystem::create_directory(dir), "new test directory");
        const auto path = (dir / "card.mcd").string();
        SaveCareer(path, before);
        const auto original = ReadFileBytes(path);
        SaveCheat(path, before, candidate);
        auto check = LoadCareer(path);
        Check(check.CrcOk() && std::memcmp(&check.state, &candidate.state, sizeof(CareerState)) == 0, "cheat save reload and CRC");
        Check(ReadFileBytes(path + ".before-cheats.bak") == original, "backup contains exact original card");
        SaveCheat(path, candidate, candidate);
        Check(ReadFileBytes(path + ".before-cheats.bak") == original, "second action preserves first backup");
        std::filesystem::remove(path); std::filesystem::remove(path + ".before-cheats.bak"); std::filesystem::remove(dir);
        std::cout << "Career cheat persistence and backup checks passed\n";
        if (argc > 1) {
            DiscImage disc(argv[1]); GtfsVolume vol(disc);
            auto data = CareerData::Load(disc, vol); data.strict = false;
            const auto catalogue = CheatCatalogue(data);
            Check(catalogue.size() > 500, "full car catalogue");
            for (size_t index : catalogue) {
                auto carSave = before;
                ApplyCheat(carSave, &data, Cheat::AddCar, index);
                Check(carSave.state.garage.count == 1 && carSave.state.garage.currentCar == 0, "added car selected in empty garage");
                Check(carSave.state.garage.cars[0].carId == data.cars.At(index).carId, "selected catalogue car added");
                Check(carSave.state.garage.money == before.state.garage.money, "free car does not spend credits");
            }
            auto full = before; full.state.garage.count = kGarageCapacity;
            const auto fullBefore = full;
            bool refused = false;
            try { ApplyCheat(full, &data, Cheat::AddCar, catalogue.front()); } catch (const std::exception&) { refused = true; }
            Check(refused && std::memcmp(&full.state, &fullBefore.state, sizeof(CareerState)) == 0, "full garage refused without mutation");
            std::cout << catalogue.size() << " catalogue cars and garage capacity checked\n";
        }
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
