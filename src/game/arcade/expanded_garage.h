#pragma once
#include "game/career/garage.h"
#include <span>
#include <string>
#include <vector>
namespace gt2::arcade {
constexpr int kArcadeGarageCapacity = 4096;
int ExpandedGarageCount(std::span<const uint8_t> block);
career::GarageCar& ExpandedGarageCar(std::span<uint8_t> block, int index);
// The arcade extension keeps the retail career block and its 100-car layout intact.
std::vector<uint8_t> AddAllArcadeCars(std::span<const uint8_t> block, const career::CareerData& data);
std::vector<uint8_t> ReadArcadeGarage(const std::string& path);
void SaveArcadeGarage(const std::string& path, std::span<const uint8_t> block);
}
