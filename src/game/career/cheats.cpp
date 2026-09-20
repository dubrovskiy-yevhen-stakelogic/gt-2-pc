#include "game/career/cheats.h"
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif

namespace gt2::career {
std::vector<size_t> CheatCatalogue(const CareerData& data) {
    std::vector<size_t> cars;
    for (size_t i = 0; i < data.cars.Count(); ++i)
        if (FirstRowOfCar(data.tables, 30, data.cars.At(i).carId) >= 0 && !data.cars.At(i).paintIds.empty()) cars.push_back(i);
    std::sort(cars.begin(), cars.end(), [&](size_t a, size_t b) { return data.cars.At(a).name < data.cars.At(b).name; });
    return cars;
}
void ApplyCheat(CareerSave& candidate, const CareerData* data, Cheat action, size_t carIndex) {
    if (action == Cheat::GoldLicences) {
        for (auto& licence : candidate.state.licences) for (auto& test : licence) test.passed = 4;
    } else if (action == Cheat::MaxCredits) candidate.state.garage.money = kMoneyLimit;
    else {
        if (!data) throw std::runtime_error("Enter GT Mode first.");
        auto& garage = candidate.state.garage;
        if (garage.count < 0 || garage.count >= kGarageCapacity) throw std::runtime_error("Garage full (100). Sell a car first.");
        const auto& car = data->cars.At(carIndex);
        if (car.paintIds.empty() || FirstRowOfCar(data->tables, 30, car.carId) < 0) throw std::runtime_error("No drivable car configuration.");
        TuneSheet sheet{}; std::array<uint8_t, 0x230> scratch{};
        if (BuyCar(garage, car.carId, car.paintIds.front(), 0, *data, sheet, {scratch.data()}) != 1) throw std::runtime_error("Car could not be added.");
        if (garage.currentCar < 0) garage.currentCar = int16_t(garage.count - 1);
    }
}
void SaveCheat(const std::string& path, const CareerSave& before, const CareerSave& candidate) {
    if (path.empty()) throw std::runtime_error("No career save path.");
    namespace fs = std::filesystem;
    const fs::path destination(path), backup(path + ".before-cheats.bak");
    if (!destination.parent_path().empty()) fs::create_directories(destination.parent_path());
    const bool exists = fs::exists(destination);
    const auto oldBytes = exists ? ReadFileBytes(path) : std::vector<uint8_t>{};
    if (!fs::exists(backup)) {
        if (exists) WriteFileBytes(backup.string(), oldBytes);
        else WriteFileBytes(backup.string(), BuildCareerSaveFile(before));
    }
    auto bytes = oldBytes.size() == 128 * 1024 ? oldBytes : FormatMemoryCard();
    std::string extension = destination.extension().string();
    for (char& c : extension) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    if (extension == ".mcd") StoreCareerOnCard(bytes, candidate);
    else bytes = BuildCareerSaveFile(candidate);
    const fs::path temporary(path + ".cheat-tmp");
    WriteFileBytes(temporary.string(), bytes);
    const auto reread = LoadCareer(temporary.string());
    if (!reread.CrcOk() || std::memcmp(&reread.state, &candidate.state, sizeof(CareerState)) != 0)
        throw std::runtime_error("Save verification failed; original retained.");
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace save; original retained.");
#else
    fs::rename(temporary, destination);
#endif
}
}
