#include "game/arcade/expanded_garage.h"
#include "game/career/cheats.h"
#include "gt2formats/save_data.h"
#include <cstring>
#include <filesystem>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#endif
namespace gt2::arcade {
namespace {
constexpr size_t kCarBytes = sizeof(career::GarageCar);
constexpr uint8_t kMagic[8] = {'G','T','2','A','G','0','0','1'};
}
int ExpandedGarageCount(std::span<const uint8_t> block) {
    if (block.size() < 4) throw std::runtime_error("Arcade garage header is missing.");
    int16_t count; std::memcpy(&count, block.data(), 2);
    if (count < 0 || count > kArcadeGarageCapacity || block.size() < 4 + size_t(count)*kCarBytes)
        throw std::runtime_error("Arcade garage has an invalid car count.");
    return count;
}
career::GarageCar& ExpandedGarageCar(std::span<uint8_t> block, int index) {
    if (index < 0 || index >= ExpandedGarageCount(block)) throw std::out_of_range("Arcade garage car index.");
    return *reinterpret_cast<career::GarageCar*>(block.data()+4+size_t(index)*kCarBytes);
}
std::vector<uint8_t> AddAllArcadeCars(std::span<const uint8_t> block, const career::CareerData& data) {
    int count = ExpandedGarageCount(block);
    std::vector<uint8_t> result(block.begin(), block.begin()+4+size_t(count)*kCarBytes);
    std::set<uint32_t> owned;
    for (int i=0; i<count; ++i) owned.insert(ExpandedGarageCar(result,i).carId);
    for (size_t index: career::CheatCatalogue(data)) {
        const auto& info = data.cars.At(index);
        if (owned.contains(info.carId)) continue;
        if (count == kArcadeGarageCapacity) throw std::runtime_error("Arcade garage full (4096). No changes saved.");
        career::GarageBlock temporary{}; temporary.currentCar=-1;
        career::TuneSheet sheet{}; std::array<uint8_t,0x230> scratch{};
        if (career::BuyCar(temporary,info.carId,info.paintIds.front(),0,data,sheet,{scratch.data()}) != 1)
            throw std::runtime_error("Cannot build garage car: "+info.name);
        const auto* bytes=reinterpret_cast<const uint8_t*>(&temporary.cars[0]);
        result.insert(result.end(),bytes,bytes+kCarBytes);
        owned.insert(info.carId); ++count;
    }
    const int16_t n=int16_t(count); std::memcpy(result.data(),&n,2);
    return result;
}
std::vector<uint8_t> ReadArcadeGarage(const std::string& path) {
    const auto bytes=career::ReadFileBytes(path);
    if (bytes.size()<16 || std::memcmp(bytes.data(),kMagic,8)) throw std::runtime_error("Invalid arcade garage file.");
    uint32_t size,crc; std::memcpy(&size,bytes.data()+8,4); std::memcpy(&crc,bytes.data()+12,4);
    if (size!=bytes.size()-16 || Crc32(std::span(bytes).subspan(16))!=crc) throw std::runtime_error("Arcade garage checksum failed.");
    std::vector<uint8_t> block(bytes.begin()+16,bytes.end());
    if (block.size()!=4+size_t(ExpandedGarageCount(block))*kCarBytes) throw std::runtime_error("Arcade garage file length mismatch.");
    return block;
}
void SaveArcadeGarage(const std::string& path, std::span<const uint8_t> block) {
    if (path.empty()) throw std::runtime_error("No arcade save path. No changes saved.");
    const uint32_t size=uint32_t(4+size_t(ExpandedGarageCount(block))*kCarBytes), crc=Crc32(block.first(size));
    std::vector<uint8_t> bytes(16+size); std::memcpy(bytes.data(),kMagic,8);
    std::memcpy(bytes.data()+8,&size,4);std::memcpy(bytes.data()+12,&crc,4);std::memcpy(bytes.data()+16,block.data(),size);
    namespace fs=std::filesystem; const fs::path dest(path),tmp(path+".tmp"),backup(path+".before-cheats.bak");
    if (!dest.parent_path().empty()) fs::create_directories(dest.parent_path());
    if (fs::exists(dest) && !fs::exists(backup)) fs::copy_file(dest,backup);
    career::WriteFileBytes(tmp.string(),bytes);
    if (ReadArcadeGarage(tmp.string())!=std::vector<uint8_t>(block.begin(),block.begin()+size)) throw std::runtime_error("Arcade save verification failed.");
#ifdef _WIN32
    if (!MoveFileExW(tmp.c_str(),dest.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace arcade garage file.");
#else
    fs::rename(tmp,dest);
#endif
}
}
